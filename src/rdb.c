/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"
#include "lzf.h"    /* LZF compression library */
#include "zipmap.h"
#include "endianconv.h"
#include "stream.h"

#include <math.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/param.h>

/* This macro is called when the internal RDB stracture is corrupt */
#define rdbExitReportCorruptRDB(...) rdbReportError(1, __LINE__,__VA_ARGS__)
/* This macro is called when RDB read failed (possibly a short read) */
#define rdbReportReadError(...) rdbReportError(0, __LINE__,__VA_ARGS__)

char* rdbFileBeingLoaded = NULL; /* used for rdb checking on read error */
extern int rdbCheckMode;    //在redis-check-rdb文件的一个标志，初始化为0
void rdbCheckError(const char *fmt, ...);
void rdbCheckSetError(const char *fmt, ...);

#ifdef __GNUC__
void rdbReportError(int corruption_error, int linenum, char *reason, ...) __attribute__ ((format (printf, 3, 4)));
#endif
void rdbReportError(int corruption_error, int linenum, char *reason, ...) {
    va_list ap;
    char msg[1024];
    int len;

    len = snprintf(msg,sizeof(msg),
        "Internal error in RDB reading offset %llu, function at rdb.c:%d -> ",
        (unsigned long long)server.loading_loaded_bytes, linenum);
    va_start(ap,reason);
    vsnprintf(msg+len,sizeof(msg)-len,reason,ap);
    va_end(ap);

    if (!rdbCheckMode) {
        if (rdbFileBeingLoaded || corruption_error) {
            serverLog(LL_WARNING, "%s", msg);
            char *argv[2] = {"",rdbFileBeingLoaded};
            redis_check_rdb_main(2,argv,NULL);
        } else {
            serverLog(LL_WARNING, "%s. Failure loading rdb format from socket, assuming connection error, resuming operation.", msg);
            return;
        }
    } else {
        rdbCheckError("%s",msg);
    }
    serverLog(LL_WARNING, "Terminating server after rdb file reading failure.");
    exit(1);
}

// 将长度为len的数组p写到rdb中，写入成功返回写入长度
static int rdbWriteRaw(rio *rdb, void *p, size_t len) {
    //rio写入
    if (rdb && rioWrite(rdb,p,len) == 0)
        return -1;
    return len;
}

// 将长度为1的type字符写到rdb中
int rdbSaveType(rio *rdb, unsigned char type) {
    return rdbWriteRaw(rdb,&type,1);
}


/* Load a "type" in RDB format, that is a one byte unsigned integer.
 *
 * 从 rdb 中载入 1 字节长的 type 数据。
 *
 * This function is not only used to load object types, but also special
 * "types" like the end-of-file type, the EXPIRE type, and so forth.
 *
 * 函数即可以用于载入键的类型（rdb.h/REDIS_RDB_TYPE_*），
 * 也可以用于载入特殊标识号（rdb.h/REDIS_RDB_OPCODE_*）
 */
int rdbLoadType(rio *rdb) {
    unsigned char type;
    if (rioRead(rdb,&type,1) == 0) return -1;
    return type;
}

/* This is only used to load old databases stored with the RDB_OPCODE_EXPIRETIME
 * opcode. New versions of Redis store using the RDB_OPCODE_EXPIRETIME_MS
 * opcode. On error -1 is returned, however this could be a valid time, so
 * to check for loading errors the caller should call rioGetReadError() after
 * calling this function. */
// 从rio读出一个时间，单位为秒，长度为4字节
time_t rdbLoadTime(rio *rdb) {
    int32_t t32;
    if (rioRead(rdb,&t32,4) == 0) return -1;
    return (time_t)t32;
}

// 写一个longlong类型的时间，单位为毫秒
int rdbSaveMillisecondTime(rio *rdb, long long t) {
    int64_t t64 = (int64_t) t;
    memrev64ifbe(&t64); /* Store in little endian. */
    return rdbWriteRaw(rdb,&t64,8);
}

/* This function loads a time from the RDB file. It gets the version of the
 * RDB because, unfortunately, before Redis 5 (RDB version 9), the function
 * failed to convert data to/from little endian, so RDB files with keys having
 * expires could not be shared between big endian and little endian systems
 * (because the expire time will be totally wrong). The fix for this is just
 * to call memrev64ifbe(), however if we fix this for all the RDB versions,
 * this call will introduce an incompatibility for big endian systems:
 * after upgrading to Redis version 5 they will no longer be able to load their
 * own old RDB files. Because of that, we instead fix the function only for new
 * RDB versions, and load older RDB versions as we used to do in the past,
 * allowing big endian systems to load their own old RDB files.
 *
 * On I/O error the function returns LLONG_MAX, however if this is also a
 * valid stored value, the caller should use rioGetReadError() to check for
 * errors after calling this function. */
// 从rio中读出一个毫秒时间返回
long long rdbLoadMillisecondTime(rio *rdb, int rdbver) {
    int64_t t64;
    if (rioRead(rdb,&t64,8) == 0) return LLONG_MAX;
    if (rdbver >= 9) /* Check the top comment of this function. */
        memrev64ifbe(&t64); /* Convert in big endian if the system is BE. */
    return (long long)t64;
}

/* Saves an encoded length. The first two bits in the first byte are used to
 * hold the encoding type. See the REDIS_RDB_* definitions for more information
 * on the types of encoding.
 *
 * 对 len 进行特殊编码之后写入到 rdb 。
 *
 * 写入成功返回保存编码后的 len 所需的字节数。
 */
int rdbSaveLen(rio *rdb, uint64_t len) {
    unsigned char buf[2];
    size_t nwritten;

    if (len < (1<<6)) {
        /* Save a 6 bit len */
        buf[0] = (len&0xFF)|(RDB_6BITLEN<<6);   //高两位为00
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        nwritten = 1;
    } else if (len < (1<<14)) {
        /* Save a 14 bit len */
        buf[0] = ((len>>8)&0xFF)|(RDB_14BITLEN<<6); // 高两位是01表示14位长，剩下的14位表示len的值
        buf[1] = len&0xFF;
        if (rdbWriteRaw(rdb,buf,2) == -1) return -1;
        nwritten = 2;
    } else if (len <= UINT32_MAX) {
        /* Save a 32 bit len */
        buf[0] = RDB_32BITLEN;  //高两位为10表示32位长，剩下的6位不使用
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        uint32_t len32 = htonl(len);
        if (rdbWriteRaw(rdb,&len32,4) == -1) return -1;
        nwritten = 1+4;
    } else {
        /* Save a 64 bit len */
        buf[0] = RDB_64BITLEN;  //高两位为11
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        len = htonu64(len);
        if (rdbWriteRaw(rdb,&len,8) == -1) return -1;
        nwritten = 1+8;
    }
    return nwritten;
}


/* Load an encoded length. If the loaded length is a normal length as stored
 * with rdbSaveLen(), the read length is set to '*lenptr'. If instead the
 * loaded length describes a special encoding that follows, then '*isencoded'
 * is set to 1 and the encoding format is stored at '*lenptr'.
 *
 * See the RDB_ENC_* definitions in rdb.h for more information on special
 * encodings.
 *
 * The function returns -1 on error, 0 on success. */
//获取保存数字的长度和保存的是一个单纯的数字还是一个长度
//isencoded 返回代表是一个数字还是长度
//lenptr 在isencoded为0会读取具的长度当为1的时候没有读取具体的值只是读取值保存的长度
int rdbLoadLenByRef(rio *rdb, int *isencoded, uint64_t *lenptr) {
    unsigned char buf[2];
    int type;

    if (isencoded) *isencoded = 0;
    if (rioRead(rdb,buf,1) == 0) return -1;
    type = (buf[0]&0xC0)>>6;    //获取高两位
    if (type == RDB_ENCVAL) {
        /* Read a 6 bit encoding type. */
        if (isencoded) *isencoded = 1;  //代表是返回的类型
        *lenptr = buf[0]&0x3F;  //获取低六位  低六位保存的是类型
    } else if (type == RDB_6BITLEN) {   //高两位为0  代表后六位为长度
        /* Read a 6 bit len. */
        *lenptr = buf[0]&0x3F;  //拉取长度
    } else if (type == RDB_14BITLEN) {  //高两位为01 代表后十四位位长度
        /* Read a 14 bit len. */
        if (rioRead(rdb,buf+1,1) == 0) return -1;
        *lenptr = ((buf[0]&0x3F)<<8)|buf[1];
    } else if (buf[0] == RDB_32BITLEN) {    // 判断是32位的类型
        /* Read a 32 bit len. */
        uint32_t len;
        if (rioRead(rdb,&len,4) == 0) return -1;    //读取四位
        *lenptr = ntohl(len);   //获取长度
    } else if (buf[0] == RDB_64BITLEN) {    //64位类型
        /* Read a 64 bit len. */
        uint64_t len;
        if (rioRead(rdb,&len,8) == 0) return -1;    //读取八位
        *lenptr = ntohu64(len);
    } else {    //错误的类型
        rdbExitReportCorruptRDB(
            "Unknown length encoding %d in rdbLoadLen()",type);
        return -1; /* Never reached. */
    }
    return 0;
}

/* This is like rdbLoadLenByRef() but directly returns the value read
 * from the RDB stream, signaling an error by returning RDB_LENERR
 * (since it is a too large count to be applicable in any Redis data
 * structure). */
uint64_t rdbLoadLen(rio *rdb, int *isencoded) {
    uint64_t len;

    if (rdbLoadLenByRef(rdb,isencoded,&len) == -1) return RDB_LENERR;
    return len;
}

/* Encodes the "value" argument as integer when it fits in the supported ranges
 * for encoded types. If the function successfully encodes the integer, the
 * representation is stored in the buffer pointer to by "enc" and the string
 * length is returned. Otherwise 0 is returned. */
// 将longlong类型的value编码成一个整数编码，如果可以编码，将编码后的值保存在enc中，返回编码后的字节数
int rdbEncodeInteger(long long value, unsigned char *enc) {
    // -2^8 <= value <= 2^8-1
    // 最高两位设置为 11，表示是一个编码过的值，低6位为 000000 ，表示是 RDB_ENC_INT8 编码格式
    // 剩下的1个字节保存value，返回2字节
    if (value >= -(1<<7) && value <= (1<<7)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT8;  //高位为11
        enc[1] = value&0xFF;
        return 2;


        // -2^16 <= value <= 2^16-1
        // 最高两位设置为 11，表示是一个编码过的值，低6位为 000001 ，表示是 RDB_ENC_INT16 编码格式
        // 剩下的2个字节保存value，返回3字节
    } else if (value >= -(1<<15) && value <= (1<<15)-1) {   //两位可以保存
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT16;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        return 3;

        // -2^32 <= value <= 2^32-1
        // 最高两位设置为 11，表示是一个编码过的值，低6位为 000010 ，表示是 RDB_ENC_INT32 编码格式
        // 剩下的4个字节保存value，返回5字节
    } else if (value >= -((long long)1<<31) && value <= ((long long)1<<31)-1) { //四位可以保存
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT32;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        enc[3] = (value>>16)&0xFF;
        enc[4] = (value>>24)&0xFF;
        return 5;
    } else {
        return 0;
    }
}

/* Loads an integer-encoded object with the specified encoding type "enctype".
 * The returned value changes according to the flags, see
 * rdbGenericLoadStringObject() for more info. */
// 将rio中的整数值根据不同的编码读出来，并根据flags构建成一个不同类型的值并返回
void *rdbLoadIntegerObject(rio *rdb, int enctype, int flags, size_t *lenptr) {
    int plain = flags & RDB_LOAD_PLAIN; //无格式
    int sds = flags & RDB_LOAD_SDS;
    int encode = flags & RDB_LOAD_ENC;  //字符串对象
    unsigned char enc[4];
    long long val;

    // 根据不同的整数编码类型，从rio中读出整数值到enc中
    if (enctype == RDB_ENC_INT8) {
        if (rioRead(rdb,enc,1) == 0) return NULL;
        val = (signed char)enc[0];
    } else if (enctype == RDB_ENC_INT16) {
        uint16_t v;
        if (rioRead(rdb,enc,2) == 0) return NULL;
        v = enc[0]|(enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == RDB_ENC_INT32) {
        uint32_t v;
        if (rioRead(rdb,enc,4) == 0) return NULL;
        v = enc[0]|(enc[1]<<8)|(enc[2]<<16)|(enc[3]<<24);
        val = (int32_t)v;
    } else {
        rdbExitReportCorruptRDB("Unknown RDB integer encoding type %d",enctype);
        return NULL; /* Never reached. */
    }

    // 如果是整数，转换为字符串类型返回
    if (plain || sds) {
        char buf[LONG_STR_SIZE], *p;
        int len = ll2string(buf,sizeof(buf),val);
        if (lenptr) *lenptr = len;
        p = plain ? zmalloc(len) : sdsnewlen(SDS_NOINIT,len);
        memcpy(p,buf,len);
        return p;
        // 如果是编码过的整数值，则转换为字符串对象，返回
    } else if (encode) {
        return createStringObjectFromLongLongForValue(val);
    } else {
        //返回字符串
        return createObject(OBJ_STRING,sdsfromlonglong(val));
    }
}

/* String objects in the form "2391" "-100" without any space and with a
 * range of values that can fit in an 8, 16 or 32 bit signed value can be
 * encoded as integers to save space */
// 将一些纯数字的字符串尝试转换为可以编码的整数，以节省内存
int rdbTryIntegerEncoding(char *s, size_t len, unsigned char *enc) {
    long long value;
    char *endptr, buf[32];

    /* Check if it's possible to encode this value as a number */
    // 尝试将字符串值转换为整数
    value = strtoll(s, &endptr, 10);
    // 字符串不是纯数字的返回0
    if (endptr[0] != '\0') return 0;
    // 将value转回字符串类型
    ll2string(buf,32,value);

    /* If the number converted back into a string is not identical
     * then it's not possible to encode the string as integer */
    // 比较转换前后的字符串，如果不相等，则返回0
    if (strlen(buf) != len || memcmp(buf,s,len)) return 0;

    // 可以编码则转换成整数，将编码类型保存enc中
    return rdbEncodeInteger(value,enc);
}

//保存压缩对象
//标记位为RDB_ENCVAL<<6)|RDB_ENC_LZF;
//然后保存压缩后长度 然后写入原来的长度
//最后写入压缩后的二进制 长度为压缩后的长度
ssize_t rdbSaveLzfBlob(rio *rdb, void *data, size_t compress_len,
                       size_t original_len) {
    unsigned char byte;
    ssize_t n, nwritten = 0;

    /* Data compressed! Let's save it on disk */
    byte = (RDB_ENCVAL<<6)|RDB_ENC_LZF; //保存格式 压缩标记高两位也是用的11 低两位使用的11  在loadlen 代码可以参考
    if ((n = rdbWriteRaw(rdb,&byte,1)) == -1) goto writeerr;    //首先写入压缩标记
    nwritten += n;

    if ((n = rdbSaveLen(rdb,compress_len)) == -1) goto writeerr;    //写入压缩后的长度
    nwritten += n;

    if ((n = rdbSaveLen(rdb,original_len)) == -1) goto writeerr;    //写入原来的长度
    nwritten += n;

    if ((n = rdbWriteRaw(rdb,data,compress_len)) == -1) goto writeerr;  //写入压缩后的二进制
    nwritten += n;

    return nwritten;

writeerr:
    return -1;
}

// string压缩保存
ssize_t rdbSaveLzfStringObject(rio *rdb, unsigned char *s, size_t len) {
    size_t comprlen, outlen;
    void *out;

    /* We require at least four bytes compression for this to be worth it */
    if (len <= 4) return 0;
    outlen = len-4;
    if ((out = zmalloc(outlen+1)) == NULL) return 0;    //分配压缩后的空间
    comprlen = lzf_compress(s, len, out, outlen);   //压缩
    if (comprlen == 0) {    //压缩失败
        zfree(out);
        return 0;
    }
    ssize_t nwritten = rdbSaveLzfBlob(rdb, out, comprlen, len); //写入压缩后的文件
    zfree(out);
    return nwritten;
}

/* Load an LZF compressed string in RDB format. The returned value
 * changes according to 'flags'. For more info check the
 * rdbGenericLoadStringObject() function. */
// 从rio中读出一个压缩过的字符串，将其解压并返回构建成的字符串对象
void *rdbLoadLzfStringObject(rio *rdb, int flags, size_t *lenptr) {
    int plain = flags & RDB_LOAD_PLAIN; //无格式的，没有编码过
    int sds = flags & RDB_LOAD_SDS;
    uint64_t len, clen;
    unsigned char *c = NULL;
    char *val = NULL;

    // 读出clen值，表示压缩的长度
    if ((clen = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
    // 读出len值，表示未压缩的长度
    if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
    // 分配一个clen长度的空间
    if ((c = zmalloc(clen)) == NULL) goto err;

    /* Allocate our target according to the uncompressed size. */
    // 如果是一个未编码过的值
    if (plain) {
        val = zmalloc(len); //分配一个未编码值的空间
    } else {
        // 否则分配一个字符串空间
        val = sdsnewlen(SDS_NOINIT,len);
    }
    if (lenptr) *lenptr = len;

    /* Load the compressed representation and uncompress it to target. */
    // 从rio中读出压缩过的值
    if (rioRead(rdb,c,clen) == 0) goto err;
    // 将压缩过的值解压缩，保存在val中，解压后的长度为len
    if (lzf_decompress(c,clen,val,len) == 0) {
        rdbExitReportCorruptRDB("Invalid LZF compressed string");
    }
    zfree(c);   //释放空间

    if (plain || sds) {
        return val; //返回源值
    } else {
        return createObject(OBJ_STRING,val);     //返回字一个字符串对象
    }
err:
    zfree(c);
    if (plain)
        zfree(val);
    else
        sdsfree(val);
    return NULL;
}

/* Save a string object as [len][data] on disk. If the object is a string
 * representation of an integer value we try to save it in a special form */
// 保存rawstring
// 在raw主要进行了两次判断
// 第一次判断这个字符串能否转换成一个数字进行保存
// 第二次判断是判断是否能够压缩然后进行压缩保存
// 都不行的话才进行普通的string保存
ssize_t rdbSaveRawString(rio *rdb, unsigned char *s, size_t len) {
    int enclen;
    ssize_t n, nwritten = 0;

    /* Try integer encoding */
    if (len <= 11) {    //首先这个还是蛮小的
        unsigned char buf[5];
        if ((enclen = rdbTryIntegerEncoding((char*)s,len,buf)) > 0) {   //尝试转换成int
            if (rdbWriteRaw(rdb,buf,enclen) == -1) return -1;   //如果是int  buf已经被编码了 写入就行了
            return enclen;
        }
    }

    /* Try LZF compression - under 20 bytes it's unable to compress even
     * aaaaaaaaaaaaaaaaaa so skip it */
    if (server.rdb_compression && len > 20) {   //如果配置了压缩选项
        n = rdbSaveLzfStringObject(rdb,s,len);
        if (n == -1) return -1;
        if (n > 0) return n;
        /* Return value of 0 means data can't be compressed, save the old way */
    }

    /* Store verbatim */
    // 字符串既不能被压缩，也不能编码成整数
    // 因此直接写入rio中
    // 写入长度
    if ((n = rdbSaveLen(rdb,len)) == -1) return -1;     //没有压缩标记或长度蛮短的 先写入长度
    nwritten += n;
    if (len > 0) {
        if (rdbWriteRaw(rdb,s,len) == -1) return -1;    //写入二进制
        nwritten += len;
    }
    return nwritten;
}

/* Save a long long value as either an encoded string or a string. */
// string是否是一个数字或者是否能够被转换成一个数字来进行保存
ssize_t rdbSaveLongLongAsStringObject(rio *rdb, long long value) {
    unsigned char buf[32];
    ssize_t n, nwritten = 0;
    int enclen = rdbEncodeInteger(value,buf);   //对数字进行编码
    if (enclen > 0) {   //编码成功
        return rdbWriteRaw(rdb,buf,enclen); //直接写入buf里面的内容
    } else {
        /* Encode as string */
        enclen = ll2string((char*)buf,32,value);    //转换才string
        serverAssert(enclen < 32);
        if ((n = rdbSaveLen(rdb,enclen)) == -1) return -1;  //先写入len  返回花费的长度
        nwritten += n;
        if ((n = rdbWriteRaw(rdb,buf,enclen)) == -1) return -1; //写入内容
        nwritten += n;
    }
    return nwritten;
}

/* Like rdbSaveRawString() gets a Redis object instead. */
// 将字符串对象obj写到rio中
ssize_t rdbSaveStringObject(rio *rdb, robj *obj) {
    /* Avoid to decode the object, then encode it again, if the
     * object is already integer encoded. */
    // 如果是int编码的是字符串对象
    if (obj->encoding == OBJ_ENCODING_INT) {
        // 讲对象值进行编码后发送给rio
        return rdbSaveLongLongAsStringObject(rdb,(long)obj->ptr);
        // RAW或EMBSTR编码类型的字符串对象
    } else {
        serverAssertWithInfo(NULL,obj,sdsEncodedObject(obj));
        // 将字符串类型的对象写到rio
        return rdbSaveRawString(rdb,obj->ptr,sdslen(obj->ptr));
    }
}

/* Load a string object from an RDB file according to flags:
 *
 * RDB_LOAD_NONE (no flags): load an RDB object, unencoded.
 * RDB_LOAD_ENC: If the returned type is a Redis object, try to
 *               encode it in a special way to be more memory
 *               efficient. When this flag is passed the function
 *               no longer guarantees that obj->ptr is an SDS string.
 * RDB_LOAD_PLAIN: Return a plain string allocated with zmalloc()
 *                 instead of a Redis object with an sds in it.
 * RDB_LOAD_SDS: Return an SDS string instead of a Redis object.
 *
 * On I/O error NULL is returned.
 */

// 根据flags，将从rio读出一个字符串对象进行编码
void *rdbGenericLoadStringObject(rio *rdb, int flags, size_t *lenptr) {
    int encode = flags & RDB_LOAD_ENC;
    int plain = flags & RDB_LOAD_PLAIN;
    int sds = flags & RDB_LOAD_SDS;
    int isencoded;
    unsigned long long len;

    // 从rio中读出一个字符串对象，编码类型保存在isencoded中，所需的字节为len
    len = rdbLoadLen(rdb,&isencoded);
    // 如果读出的对象被编码(isencoded被设置为1)，则根据不同的长度值len映射到不同的整数编码
    if (isencoded) {
        switch(len) {
        case RDB_ENC_INT8:
        case RDB_ENC_INT16:
        case RDB_ENC_INT32:
            // 以上三种类型的整数编码，根据flags返回不同类型值
            return rdbLoadIntegerObject(rdb,len,flags,lenptr);
        case RDB_ENC_LZF:
            // 如果是压缩后的字符串，进行构建压缩字符串编码对象
            return rdbLoadLzfStringObject(rdb,flags,lenptr);
        default:
            rdbExitReportCorruptRDB("Unknown RDB string encoding type %llu",len);
            return NULL;
        }
    }

    // 如果len值错误，则返回NULL
    if (len == RDB_LENERR) return NULL;

    // 如果是原生值
    if (plain || sds) {
        // 分配空间
        void *buf = plain ? zmalloc(len) : sdsnewlen(SDS_NOINIT,len);
        if (lenptr) *lenptr = len;
        // 从rio中读出来
        if (len && rioRead(rdb,buf,len) == 0) {
            if (plain)
                zfree(buf);
            else
                sdsfree(buf);
            return NULL;
        }
        //返回
        return buf;
    } else {
        // 根据encode编码类型创建不同的字符串对象
        robj *o = encode ? createStringObject(SDS_NOINIT,len) :
                           createRawStringObject(SDS_NOINIT,len);
        // 设置o对象的值，从rio中读出来，如果失败，释放对象返回NULL
        if (len && rioRead(rdb,o->ptr,len) == 0) {
            decrRefCount(o);
            return NULL;
        }
        return o;
    }
}

// 从rio中读出一个字符串编码的对象
robj *rdbLoadStringObject(rio *rdb) {
    return rdbGenericLoadStringObject(rdb,RDB_LOAD_NONE,NULL);
}

// 从rio中读出一个字符串编码的对象，对象使用不同类型的编码
robj *rdbLoadEncodedStringObject(rio *rdb) {
    return rdbGenericLoadStringObject(rdb,RDB_LOAD_ENC,NULL);
}

/* Save a double value. Doubles are saved as strings prefixed by an unsigned
 * 8 bit integer specifying the length of the representation.
 * This 8 bit integer has special values in order to specify the following
 * conditions:
 * 253: not a number
 * 254: + inf
 * 255: - inf
 */
// 写入一个double类型的字符串值，字符串前是一个8位长的无符号整数，它表示浮点数的长度
// 八位整数中的值表示一些特殊情况，
// 253：表示不是数字
// 254：表示正无穷
// 255：表示负无穷
int rdbSaveDoubleValue(rio *rdb, double val) {
    unsigned char buf[128];
    int len;

    // 如果val不是一个数字，则写入253
    if (isnan(val)) {
        buf[0] = 253;
        len = 1;
        // 如果val不是一个有限的值，根据正负性，写入255或254
    } else if (!isfinite(val)) {
        len = 1;
        buf[0] = (val < 0) ? 255 : 254;
        // 如果不是上面的两种情况，则表示val是一个double类型的数
    } else {
        // 64位系统中
        // DBL_MANT_DIG：表示尾数中的位数为53
        // LLONG_MAX：longlong表示的最大数为0x7fffffffffffffffLL
#if (DBL_MANT_DIG >= 52) && (LLONG_MAX == 0x7fffffffffffffffLL)
        /* Check if the float is in a safe range to be casted into a
         * long long. We are assuming that long long is 64 bit here.
         * Also we are assuming that there are no implementations around where
         * double has precision < 52 bit.
         *
         * Under this assumptions we test if a double is inside an interval
         * where casting to long long is safe. Then using two castings we
         * make sure the decimal part is zero. If all this is true we use
         * integer printing function that is much faster. */
        double min = -4503599627370495; /* (2^52)-1 */
        double max = 4503599627370496; /* -(2^52) */
        if (val > min && val < max && val == ((double)((long long)val)))
            ll2string((char*)buf+1,sizeof(buf)-1,(long long)val);
        else
#endif
        // 以宽度为17位的方式写到8位长度值的后面，17位的double双精度浮点数的长度最短且无损
        snprintf((char*)buf+1,sizeof(buf)-1,"%.17g",val);
        buf[0] = strlen((char*)buf+1);  //将刚才加入buf+1的字符串值的长度写到前8位的长度值中
        len = buf[0]+1; //总字节数
    }
    return rdbWriteRaw(rdb,buf,len);
}

/* For information about double serialization check rdbSaveDoubleValue() */
// 读出字符串表示的double值
int rdbLoadDoubleValue(rio *rdb, double *val) {
    char buf[256];
    unsigned char len;

    // 从rio中读出一个字节的长度，保存在len中
    if (rioRead(rdb,&len,1) == 0) return -1;
    // Redis中，0，负无穷，正无穷，非数字分别如下表示，在redis.c中
    // static double R_Zero, R_PosInf, R_NegInf, R_Nan;
    // R_Zero = 0.0;
    // R_PosInf = 1.0/R_Zero;
    // R_NegInf = -1.0/R_Zero;
    // R_Nan = R_Zero/R_Zero;
    // 如果长度值为这三个特殊值，返回0
    switch(len) {
    case 255: *val = R_NegInf; return 0;
    case 254: *val = R_PosInf; return 0;
    case 253: *val = R_Nan; return 0;
    // 否则从rio读出len长的字符串
    default:
        if (rioRead(rdb,buf,len) == 0) return -1;
        buf[len] = '\0';
        sscanf(buf, "%lg", val);     //将buf值写到val中
        return 0;
    }
}

/* Saves a double for RDB 8 or greater, where IE754 binary64 format is assumed.
 * We just make sure the integer is always stored in little endian, otherwise
 * the value is copied verbatim from memory to disk.
 *
 * Return -1 on error, the size of the serialized value on success. */
int rdbSaveBinaryDoubleValue(rio *rdb, double val) {
    memrev64ifbe(&val);
    return rdbWriteRaw(rdb,&val,sizeof(val));
}

/* Loads a double from RDB 8 or greater. See rdbSaveBinaryDoubleValue() for
 * more info. On error -1 is returned, otherwise 0. */
int rdbLoadBinaryDoubleValue(rio *rdb, double *val) {
    if (rioRead(rdb,val,sizeof(*val)) == 0) return -1;
    memrev64ifbe(val);
    return 0;
}

/* Like rdbSaveBinaryDoubleValue() but single precision. */
int rdbSaveBinaryFloatValue(rio *rdb, float val) {
    memrev32ifbe(&val);
    return rdbWriteRaw(rdb,&val,sizeof(val));
}

/* Like rdbLoadBinaryDoubleValue() but single precision. */
int rdbLoadBinaryFloatValue(rio *rdb, float *val) {
    if (rioRead(rdb,val,sizeof(*val)) == 0) return -1;
    memrev32ifbe(val);
    return 0;
}

/* Save the object type of object "o". */
// 将对象o的类型写到rio中，保存类型
int rdbSaveObjectType(rio *rdb, robj *o) {
    // 根据不同数据类型，写入不同编码类型
    switch (o->type) {
    case OBJ_STRING:    //字符串编码
        return rdbSaveType(rdb,RDB_TYPE_STRING);
    case OBJ_LIST:      //列表类型
        if (o->encoding == OBJ_ENCODING_QUICKLIST)
            return rdbSaveType(rdb,RDB_TYPE_LIST_QUICKLIST);
        else
            serverPanic("Unknown list encoding");
    case OBJ_SET:       //集合类型
        if (o->encoding == OBJ_ENCODING_INTSET)
            return rdbSaveType(rdb,RDB_TYPE_SET_INTSET);
        else if (o->encoding == OBJ_ENCODING_HT)
            return rdbSaveType(rdb,RDB_TYPE_SET);
        else
            serverPanic("Unknown set encoding");
    case OBJ_ZSET:      //有序集合
        if (o->encoding == OBJ_ENCODING_ZIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_ZSET_ZIPLIST);
        else if (o->encoding == OBJ_ENCODING_SKIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_ZSET_2);
        else
            serverPanic("Unknown sorted set encoding");
    case OBJ_HASH:      //哈希表
        if (o->encoding == OBJ_ENCODING_ZIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_HASH_ZIPLIST);
        else if (o->encoding == OBJ_ENCODING_HT)
            return rdbSaveType(rdb,RDB_TYPE_HASH);
        else
            serverPanic("Unknown hash encoding");
    case OBJ_STREAM:
        return rdbSaveType(rdb,RDB_TYPE_STREAM_LISTPACKS);
    case OBJ_MODULE:
        return rdbSaveType(rdb,RDB_TYPE_MODULE_2);
    default:
        serverPanic("Unknown object type");
    }
    return -1; /* avoid warning */
}

/* Use rdbLoadType() to load a TYPE in RDB format, but returns -1 if the
 * type is not specifically a valid Object Type. */
// 从rio中读出一个类型并返回
int rdbLoadObjectType(rio *rdb) {
    int type;
    // 从rio中读出类型保存在type中
    if ((type = rdbLoadType(rdb)) == -1) return -1;
    // 判断是否是一个rio规定的类型
    if (!rdbIsObjectType(type)) return -1;
    return type;
}

/* This helper function serializes a consumer group Pending Entries List (PEL)
 * into the RDB file. The 'nacks' argument tells the function if also persist
 * the informations about the not acknowledged message, or if to persist
 * just the IDs: this is useful because for the global consumer group PEL
 * we serialized the NACKs as well, but when serializing the local consumer
 * PELs we just add the ID, that will be resolved inside the global PEL to
 * put a reference to the same structure. */
ssize_t rdbSaveStreamPEL(rio *rdb, rax *pel, int nacks) {
    ssize_t n, nwritten = 0;

    /* Number of entries in the PEL. */
    if ((n = rdbSaveLen(rdb,raxSize(pel))) == -1) return -1;
    nwritten += n;

    /* Save each entry. */
    raxIterator ri;
    raxStart(&ri,pel);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        /* We store IDs in raw form as 128 big big endian numbers, like
         * they are inside the radix tree key. */
        if ((n = rdbWriteRaw(rdb,ri.key,sizeof(streamID))) == -1) {
            raxStop(&ri);
            return -1;
        }
        nwritten += n;

        if (nacks) {
            streamNACK *nack = ri.data;
            if ((n = rdbSaveMillisecondTime(rdb,nack->delivery_time)) == -1) {
                raxStop(&ri);
                return -1;
            }
            nwritten += n;
            if ((n = rdbSaveLen(rdb,nack->delivery_count)) == -1) {
                raxStop(&ri);
                return -1;
            }
            nwritten += n;
            /* We don't save the consumer name: we'll save the pending IDs
             * for each consumer in the consumer PEL, and resolve the consumer
             * at loading time. */
        }
    }
    raxStop(&ri);
    return nwritten;
}

/* Serialize the consumers of a stream consumer group into the RDB. Helper
 * function for the stream data type serialization. What we do here is to
 * persist the consumer metadata, and it's PEL, for each consumer. */
size_t rdbSaveStreamConsumers(rio *rdb, streamCG *cg) {
    ssize_t n, nwritten = 0;

    /* Number of consumers in this consumer group. */
    if ((n = rdbSaveLen(rdb,raxSize(cg->consumers))) == -1) return -1;
    nwritten += n;

    /* Save each consumer. */
    raxIterator ri;
    raxStart(&ri,cg->consumers);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        streamConsumer *consumer = ri.data;

        /* Consumer name. */
        if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) {
            raxStop(&ri);
            return -1;
        }
        nwritten += n;

        /* Last seen time. */
        if ((n = rdbSaveMillisecondTime(rdb,consumer->seen_time)) == -1) {
            raxStop(&ri);
            return -1;
        }
        nwritten += n;

        /* Consumer PEL, without the ACKs (see last parameter of the function
         * passed with value of 0), at loading time we'll lookup the ID
         * in the consumer group global PEL and will put a reference in the
         * consumer local PEL. */
        if ((n = rdbSaveStreamPEL(rdb,consumer->pel,0)) == -1) {
            raxStop(&ri);
            return -1;
        }
        nwritten += n;
    }
    raxStop(&ri);
    return nwritten;
}

/* Save a Redis object.
 * Returns -1 on error, number of bytes written on success. */
// 其他Object的保存
ssize_t rdbSaveObject(rio *rdb, robj *o, robj *key) {
    ssize_t n = 0, nwritten = 0;

    if (o->type == OBJ_STRING) {
        /* Save a string value */
        if ((n = rdbSaveStringObject(rdb,o)) == -1) return -1;
        nwritten += n;
    } else if (o->type == OBJ_LIST) {   //list
        /* Save a list value */
        //对于我们的quicklist来说 他的每一个节点是一个quicklistnode 他的数据区域是使用的ziplist
        //ziplist是一个连续的内存块 所以在为压缩的node节点将将ziplist当成一个string来保存
        //如果是一个压缩的节点，使用blob的方式的保存
        /* Save a list value */
        if (o->encoding == OBJ_ENCODING_QUICKLIST) {
            quicklist *ql = o->ptr;
            quicklistNode *node = ql->head;

            // 先保存长度
            if ((n = rdbSaveLen(rdb,ql->len)) == -1) return -1;
            nwritten += n;

            // 然后遍历quicklist中的结点
            while(node) {
                // 根据是否压缩分别存储
                if (quicklistNodeIsCompressed(node)) {
                    void *data;
                    size_t compress_len = quicklistGetLzf(node, &data);
                    // 压缩过，则解压后保存二进制数据到rdb
                    if ((n = rdbSaveLzfBlob(rdb,data,compress_len,node->sz)) == -1) return -1;
                    nwritten += n;
                } else {
                    // 未压缩过，则直接保存原始的字符串数据
                    if ((n = rdbSaveRawString(rdb,node->zl,node->sz)) == -1) return -1;
                    nwritten += n;
                }
                // 下一个结点
                node = node->next;
            }
        } else {
            serverPanic("Unknown list encoding");
        }
        //集合对象
    } else if (o->type == OBJ_SET) {
        /* Save a set value */
        // 哈希表编码
        if (o->encoding == OBJ_ENCODING_HT) {
            dict *set = o->ptr;
            // 获取集合对象迭代器
            dictIterator *di = dictGetIterator(set);
            dictEntry *de;

            // 保存字典长度到RDB
            if ((n = rdbSaveLen(rdb,dictSize(set))) == -1) {
                dictReleaseIterator(di);
                return -1;
            }
            nwritten += n;

            // 遍历字典
            while((de = dictNext(di)) != NULL) {
                // 获取sds字符串对象
                sds ele = dictGetKey(de);
                // 保存到RDB中
                if ((n = rdbSaveRawString(rdb,(unsigned char*)ele,sdslen(ele)))
                    == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
            }
            // 释放迭代器
            dictReleaseIterator(di);
            // 整数集合编码
        } else if (o->encoding == OBJ_ENCODING_INTSET) {
            // 获取整数集合长度
            size_t l = intsetBlobLen((intset*)o->ptr);

            // 保存原始字符串到RDB
            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else {
            serverPanic("Unknown set encoding");
        }
        // 有序集合对象是ziplist类型
    } else if (o->type == OBJ_ZSET) {
        /* Save a sorted set value */
        // 有序集合对象是ziplist类型
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            // 压缩列表长度
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);

            // 以一个原生字符串对象保存ziplist类型的有序集合
            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
            // 有序集合对象是skiplist类型
        } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = o->ptr;
            zskiplist *zsl = zs->zsl;

            // 存储跳跃表长度到RDB
            if ((n = rdbSaveLen(rdb,zsl->length)) == -1) return -1;
            nwritten += n;

            /* We save the skiplist elements from the greatest to the smallest
             * (that's trivial since the elements are already ordered in the
             * skiplist): this improves the load process, since the next loaded
             * element will always be the smaller, so adding to the skiplist
             * will always immediately stop at the head, making the insertion
             * O(1) instead of O(log(N)). */
            zskiplistNode *zn = zsl->tail;
            // 遍历跳跃表
            while (zn != NULL) {
                // 成员ele存储原始字符串到RDB
                if ((n = rdbSaveRawString(rdb,
                    (unsigned char*)zn->ele,sdslen(zn->ele))) == -1)
                {
                    return -1;
                }
                nwritten += n;
                // 分数score存储二进制double到rdb
                if ((n = rdbSaveBinaryDoubleValue(rdb,zn->score)) == -1)
                    return -1;
                nwritten += n;
                zn = zn->backward;
            }
        } else {
            serverPanic("Unknown sorted set encoding");
        }
        // 哈希对象
    } else if (o->type == OBJ_HASH) {
        /* Save a hash value */
        // 压缩列表编码
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            // 长度
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);

            // 存储到RDB
            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;

            // 哈希表编码
        } else if (o->encoding == OBJ_ENCODING_HT) {
            // 迭代器
            dictIterator *di = dictGetIterator(o->ptr);
            dictEntry *de;

            // 保存字典长度到RDB
            if ((n = rdbSaveLen(rdb,dictSize((dict*)o->ptr))) == -1) {
                dictReleaseIterator(di);
                return -1;
            }
            nwritten += n;

            // 遍历字典
            while((de = dictNext(di)) != NULL) {
                // 获取成员field
                sds field = dictGetKey(de);
                // 获取成员value
                sds value = dictGetVal(de);

                // 存储field
                if ((n = rdbSaveRawString(rdb,(unsigned char*)field,
                        sdslen(field))) == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
                // 存储value
                if ((n = rdbSaveRawString(rdb,(unsigned char*)value,
                        sdslen(value))) == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
            }
            dictReleaseIterator(di);
        } else {
            serverPanic("Unknown hash encoding");
        }

    } else if (o->type == OBJ_STREAM) {
        /* Store how many listpacks we have inside the radix tree. */
        stream *s = o->ptr;
        rax *rax = s->rax;
        if ((n = rdbSaveLen(rdb,raxSize(rax))) == -1) return -1;
        nwritten += n;

        /* Serialize all the listpacks inside the radix tree as they are,
         * when loading back, we'll use the first entry of each listpack
         * to insert it back into the radix tree. */
        raxIterator ri;
        raxStart(&ri,rax);
        raxSeek(&ri,"^",NULL,0);
        while (raxNext(&ri)) {
            unsigned char *lp = ri.data;
            size_t lp_bytes = lpBytes(lp);
            if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) {
                raxStop(&ri);
                return -1;
            }
            nwritten += n;
            if ((n = rdbSaveRawString(rdb,lp,lp_bytes)) == -1) {
                raxStop(&ri);
                return -1;
            }
            nwritten += n;
        }
        raxStop(&ri);

        /* Save the number of elements inside the stream. We cannot obtain
         * this easily later, since our macro nodes should be checked for
         * number of items: not a great CPU / space tradeoff. */
        if ((n = rdbSaveLen(rdb,s->length)) == -1) return -1;
        nwritten += n;
        /* Save the last entry ID. */
        if ((n = rdbSaveLen(rdb,s->last_id.ms)) == -1) return -1;
        nwritten += n;
        if ((n = rdbSaveLen(rdb,s->last_id.seq)) == -1) return -1;
        nwritten += n;

        /* The consumer groups and their clients are part of the stream
         * type, so serialize every consumer group. */

        /* Save the number of groups. */
        size_t num_cgroups = s->cgroups ? raxSize(s->cgroups) : 0;
        if ((n = rdbSaveLen(rdb,num_cgroups)) == -1) return -1;
        nwritten += n;

        if (num_cgroups) {
            /* Serialize each consumer group. */
            raxStart(&ri,s->cgroups);
            raxSeek(&ri,"^",NULL,0);
            while(raxNext(&ri)) {
                streamCG *cg = ri.data;

                /* Save the group name. */
                if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;

                /* Last ID. */
                if ((n = rdbSaveLen(rdb,cg->last_id.ms)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;
                if ((n = rdbSaveLen(rdb,cg->last_id.seq)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;

                /* Save the global PEL. */
                if ((n = rdbSaveStreamPEL(rdb,cg->pel,1)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;

                /* Save the consumers of this group. */
                if ((n = rdbSaveStreamConsumers(rdb,cg)) == -1) {
                    raxStop(&ri);
                    return -1;
                }
                nwritten += n;
            }
            raxStop(&ri);
        }
        // 模块对象
    } else if (o->type == OBJ_MODULE) {
        /* Save a module-specific value. */
        RedisModuleIO io;
        moduleValue *mv = o->ptr;
        moduleType *mt = mv->type;

        /* Write the "module" identifier as prefix, so that we'll be able
         * to call the right module during loading. */
        int retval = rdbSaveLen(rdb,mt->id);
        if (retval == -1) return -1;
        io.bytes += retval;

        /* Then write the module-specific representation + EOF marker. */
        moduleInitIOContext(io,mt,rdb,key);
        mt->rdb_save(&io,mv->value);
        retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_EOF);
        if (retval == -1)
            io.error = 1;
        else
            io.bytes += retval;

        if (io.ctx) {
            moduleFreeContext(io.ctx);
            zfree(io.ctx);
        }
        return io.error ? -1 : (ssize_t)io.bytes;
    } else {
        serverPanic("Unknown object type");
    }
    return nwritten;
}

/* Return the length the object will have on disk if saved with
 * the rdbSaveObject() function. Currently we use a trick to get
 * this length with very little changes to the code. In the future
 * we could switch to a faster solution. */
// 返回一个对象的长度，通过写入的方式
// 但是已经被放弃使用
size_t rdbSavedObjectLen(robj *o, robj *key) {
    ssize_t len = rdbSaveObject(NULL,o,key);
    serverAssertWithInfo(NULL,o,len != -1);
    return len;
}

/* Save a key-value pair, with expire time, type, key, value.
 * On error -1 is returned.
 * On success if the key was actually saved 1 is returned, otherwise 0
 * is returned (the key was already expired). */
// 将一个键对象，值对象，过期时间，和类型写入到rio中，出错返回-1，成功返回1，键过期返回0
int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val, long long expiretime) {
    int savelru = server.maxmemory_policy & MAXMEMORY_FLAG_LRU;
    int savelfu = server.maxmemory_policy & MAXMEMORY_FLAG_LFU;

    /* Save the expire time */
    // 保存过期时间
    if (expiretime != -1) {
        // 将一个毫秒的过期时间类型写入rio
        if (rdbSaveType(rdb,RDB_OPCODE_EXPIRETIME_MS) == -1) return -1;
        // 将过期时间写入rio
        if (rdbSaveMillisecondTime(rdb,expiretime) == -1) return -1;
    }

    /* Save the LRU info. */
    if (savelru) {
        uint64_t idletime = estimateObjectIdleTime(val);
        idletime /= 1000; /* Using seconds is enough and requires less space.*/
        if (rdbSaveType(rdb,RDB_OPCODE_IDLE) == -1) return -1;
        if (rdbSaveLen(rdb,idletime) == -1) return -1;
    }

    /* Save the LFU info. */
    if (savelfu) {
        uint8_t buf[1];
        buf[0] = LFUDecrAndReturn(val);
        /* We can encode this in exactly two bytes: the opcode and an 8
         * bit counter, since the frequency is logarithmic with a 0-255 range.
         * Note that we do not store the halving time because to reset it
         * a single time when loading does not affect the frequency much. */
        if (rdbSaveType(rdb,RDB_OPCODE_FREQ) == -1) return -1;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
    }

    /* Save type, key, value */
    // 将值对象的类型，键对象和值对象到rio中
    if (rdbSaveObjectType(rdb,val) == -1) return -1;
    if (rdbSaveStringObject(rdb,key) == -1) return -1;
    if (rdbSaveObject(rdb,val,key) == -1) return -1;

    /* Delay return if required (for testing) */
    if (server.rdb_key_save_delay)
        usleep(server.rdb_key_save_delay);

    return 1;
}

/* Save an AUX field. */
// 保存设备的一些信息 后面跟两个string load的时候之间读取
ssize_t rdbSaveAuxField(rio *rdb, void *key, size_t keylen, void *val, size_t vallen) {
    ssize_t ret, len = 0;
    // RDB_OPCODE_AUX 对应的操作码是250
    if ((ret = rdbSaveType(rdb,RDB_OPCODE_AUX)) == -1) return -1;
    len += ret;
    // 写入键对象和值对象
    if ((ret = rdbSaveRawString(rdb,key,keylen)) == -1) return -1;
    len += ret;
    if ((ret = rdbSaveRawString(rdb,val,vallen)) == -1) return -1;
    len += ret;
    return len;
}

/* Wrapper for rdbSaveAuxField() used when key/val length can be obtained
 * with strlen(). */
// rdbSaveAuxField()的封装，适用于key和val是c语言字符串类型
ssize_t rdbSaveAuxFieldStrStr(rio *rdb, char *key, char *val) {
    return rdbSaveAuxField(rdb,key,strlen(key),val,strlen(val));
}

/* Wrapper for strlen(key) + integer type (up to long long range). */
// rdbSaveAuxField()的封装，适用于key是c语言类型字符串，val是一个longlong类型的整数
ssize_t rdbSaveAuxFieldStrInt(rio *rdb, char *key, long long val) {
    char buf[LONG_STR_SIZE];
    int vlen = ll2string(buf,sizeof(buf),val);
    return rdbSaveAuxField(rdb,key,strlen(key),buf,vlen);
}

/* Save a few default AUX fields with information about the RDB generated. */
// 将一个rdb文件的默认信息写入到rio中
int rdbSaveInfoAuxFields(rio *rdb, int rdbflags, rdbSaveInfo *rsi) {
    // 判断主机的总线宽度，是64位还是32位
    int redis_bits = (sizeof(void*) == 8) ? 64 : 32;
    int aof_preamble = (rdbflags & RDBFLAGS_AOF_PREAMBLE) != 0;

    /* Add a few fields about the state when the RDB was created. */
    // 添加rdb文件的状态信息：Redis版本，redis位数，当前时间和Redis当前使用的内存数
    if (rdbSaveAuxFieldStrStr(rdb,"redis-ver",REDIS_VERSION) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"redis-bits",redis_bits) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"ctime",time(NULL)) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"used-mem",zmalloc_used_memory()) == -1) return -1;

    /* Handle saving options that generate aux fields. */
    if (rsi) {
        if (rdbSaveAuxFieldStrInt(rdb,"repl-stream-db",rsi->repl_stream_db)
            == -1) return -1;
        if (rdbSaveAuxFieldStrStr(rdb,"repl-id",server.replid)
            == -1) return -1;
        if (rdbSaveAuxFieldStrInt(rdb,"repl-offset",server.master_repl_offset)
            == -1) return -1;
    }
    if (rdbSaveAuxFieldStrInt(rdb,"aof-preamble",aof_preamble) == -1) return -1;
    return 1;
}

ssize_t rdbSaveSingleModuleAux(rio *rdb, int when, moduleType *mt) {
    /* Save a module-specific aux value. */
    RedisModuleIO io;
    int retval = rdbSaveType(rdb, RDB_OPCODE_MODULE_AUX);
    if (retval == -1) return -1;
    io.bytes += retval;

    /* Write the "module" identifier as prefix, so that we'll be able
     * to call the right module during loading. */
    retval = rdbSaveLen(rdb,mt->id);
    if (retval == -1) return -1;
    io.bytes += retval;

    /* write the 'when' so that we can provide it on loading. add a UINT opcode
     * for backwards compatibility, everything after the MT needs to be prefixed
     * by an opcode. */
    retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_UINT);
    if (retval == -1) return -1;
    io.bytes += retval;
    retval = rdbSaveLen(rdb,when);
    if (retval == -1) return -1;
    io.bytes += retval;

    /* Then write the module-specific representation + EOF marker. */
    moduleInitIOContext(io,mt,rdb,NULL);
    mt->aux_save(&io,when);
    retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_EOF);
    if (retval == -1)
        io.error = 1;
    else
        io.bytes += retval;

    if (io.ctx) {
        moduleFreeContext(io.ctx);
        zfree(io.ctx);
    }
    if (io.error)
        return -1;
    return io.bytes;
}

/* Produces a dump of the database in RDB format sending it to the specified
 * Redis I/O channel. On success C_OK is returned, otherwise C_ERR
 * is returned and part of the output, or all the output, can be
 * missing because of I/O errors.
 *
 * When the function returns C_ERR and if 'error' is not NULL, the
 * integer pointed by 'error' is set to the value of errno just after the I/O
 * error. */
// 将一个RDB格式文件内容写入到rio中，成功返回C_OK，否则C_ERR和一部分或所有的出错信息
// 当函数返回C_ERR，并且error不是NULL，那么error被设置为一个错误码errno
int rdbSaveRio(rio *rdb, int *error, int rdbflags, rdbSaveInfo *rsi) {
    dictIterator *di = NULL;
    dictEntry *de;
    char magic[10];
    int j;
    uint64_t cksum;
    size_t processed = 0;

    // 开启了校验和选项
    if (server.rdb_checksum)
        // 设置校验和的函数
        rdb->update_cksum = rioGenericUpdateChecksum;
    // 将Redis版本信息保存到magic中
    snprintf(magic,sizeof(magic),"REDIS%04d",RDB_VERSION);
    // 将magic写到rio中
    if (rdbWriteRaw(rdb,magic,9) == -1) goto werr;
    // 将rdb文件的默认信息写到rio中
    if (rdbSaveInfoAuxFields(rdb,rdbflags,rsi) == -1) goto werr;
    if (rdbSaveModulesAux(rdb, REDISMODULE_AUX_BEFORE_RDB) == -1) goto werr;

    // 遍历所有服务器内的数据库
    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;      //当前的数据库指针
        dict *d = db->dict;             //当数据库的键值对字典
        // 跳过为空的数据库
        if (dictSize(d) == 0) continue;
        // 创建一个字典类型的迭代器
        di = dictGetSafeIterator(d);

        /* Write the SELECT DB opcode */
        // 写入数据库的选择标识码 RDB_OPCODE_SELECTDB为254
        if (rdbSaveType(rdb,RDB_OPCODE_SELECTDB) == -1) goto werr;
        // 写入数据库的id，占了一个字节的长度
        if (rdbSaveLen(rdb,j) == -1) goto werr;

        /* Write the RESIZE DB opcode. */
        // 写入调整数据库的操作码，我们将大小限制在UINT32_MAX以内，这并不代表数据库的实际大小，只是提示去重新调整哈希表的大小
        uint64_t db_size, expires_size;
        db_size = dictSize(db->dict);
        expires_size = dictSize(db->expires);
        // 写入调整哈希表大小的操作码，RDB_OPCODE_RESIZEDB = 251
        if (rdbSaveType(rdb,RDB_OPCODE_RESIZEDB) == -1) goto werr;
        // 写入提示调整哈希表大小的两个值，如果
        if (rdbSaveLen(rdb,db_size) == -1) goto werr;
        if (rdbSaveLen(rdb,expires_size) == -1) goto werr;

        /* Iterate this DB writing every entry */
        // 遍历数据库所有的键值对
        while((de = dictNext(di)) != NULL) {
            sds keystr = dictGetKey(de);        //当前键
            robj key, *o = dictGetVal(de);      //当前值
            long long expire;

            // 在栈中创建一个键对象并初始化
            initStaticStringObject(key,keystr);
            // 当前键的过期时间
            expire = getExpire(db,&key);
            // 将键的键对象，值对象，过期时间写到rio中
            if (rdbSaveKeyValuePair(rdb,&key,o,expire) == -1) goto werr;

            /* When this RDB is produced as part of an AOF rewrite, move
             * accumulated diff from parent to child while rewriting in
             * order to have a smaller final write. */
            if (rdbflags & RDBFLAGS_AOF_PREAMBLE &&
                rdb->processed_bytes > processed+AOF_READ_DIFF_INTERVAL_BYTES)
            {
                processed = rdb->processed_bytes;
                aofReadDiffFromParent();
            }
        }
        dictReleaseIterator(di);        //释放迭代器
        di = NULL; /* So that we don't release it again on error. */
    }

    /* If we are storing the replication information on disk, persist
     * the script cache as well: on successful PSYNC after a restart, we need
     * to be able to process any EVALSHA inside the replication backlog the
     * master will send us. */
    if (rsi && dictSize(server.lua_scripts)) {
        di = dictGetIterator(server.lua_scripts);
        while((de = dictNext(di)) != NULL) {
            robj *body = dictGetVal(de);
            if (rdbSaveAuxField(rdb,"lua",3,body->ptr,sdslen(body->ptr)) == -1)
                goto werr;
        }
        dictReleaseIterator(di);
        di = NULL; /* So that we don't release it again on error. */
    }

    if (rdbSaveModulesAux(rdb, REDISMODULE_AUX_AFTER_RDB) == -1) goto werr;

    /* EOF opcode */
    // 写入一个EOF码，RDB_OPCODE_EOF = 255
    if (rdbSaveType(rdb,RDB_OPCODE_EOF) == -1) goto werr;

    /* CRC64 checksum. It will be zero if checksum computation is disabled, the
     * loading code skips the check in this case. */
    // CRC64检验和，当校验和计算为0，没有开启是，在载入rdb文件时会跳过
    cksum = rdb->cksum;
    memrev64ifbe(&cksum);
    if (rioWrite(rdb,&cksum,8) == 0) goto werr;
    return C_OK;

werr:
    if (error) *error = errno;   //保存错误码
    if (di) dictReleaseIterator(di);    //如果没有释放迭代器，则释放
    return C_ERR;
}

/* This is just a wrapper to rdbSaveRio() that additionally adds a prefix
 * and a suffix to the generated RDB dump. The prefix is:
 *
 * $EOF:<40 bytes unguessable hex string>\r\n
 *
 * While the suffix is the 40 bytes hex string we announced in the prefix.
 * This way processes receiving the payload can understand when it ends
 * without doing any processing of the content. */
// 在rdbSaveRio()基础上，继续添加一个前缀和一个后缀，格式如下：
// $EOF:<40 bytes unguessable hex string>\r\n
int rdbSaveRioWithEOFMark(rio *rdb, int *error, rdbSaveInfo *rsi) {
    char eofmark[RDB_EOF_MARK_SIZE];

    startSaving(RDBFLAGS_REPLICATION);
    //随机生成一个40位长的十六进制的字符串作为当前Redis的运行ID
    getRandomHexChars(eofmark,RDB_EOF_MARK_SIZE);
    if (error) *error = 0;   //初始化错误码
    // 写"$EOF:"
    if (rioWrite(rdb,"$EOF:",5) == 0) goto werr;
    // 将Redis运行ID写入
    if (rioWrite(rdb,eofmark,RDB_EOF_MARK_SIZE) == 0) goto werr;
    // 写入后缀"\r\n"
    if (rioWrite(rdb,"\r\n",2) == 0) goto werr;
    // 写入rdb文件内容
    if (rdbSaveRio(rdb,error,RDBFLAGS_NONE,rsi) == C_ERR) goto werr;
    // 将Redis运行ID写入
    if (rioWrite(rdb,eofmark,RDB_EOF_MARK_SIZE) == 0) goto werr;
    stopSaving(1);
    return C_OK;

werr: /* Write error. */
    /* Set 'error' only if not already set by rdbSaveRio() call. */
    if (error && *error == 0) *error = errno;
    stopSaving(0);
    return C_ERR;
}

/* Save the DB on disk. Return C_ERR on error, C_OK on success. */
// 将数据库保存在磁盘上，返回C_OK成功，否则返回C_ERR
int rdbSave(char *filename, rdbSaveInfo *rsi) {
    char tmpfile[256];
    char cwd[MAXPATHLEN]; /* Current working dir path for error messages. */
    FILE *fp = NULL;
    rio rdb;
    int error = 0;

    // 创建临时文件
    snprintf(tmpfile,256,"temp-%d.rdb", (int) getpid());
    // 以写方式打开该文件
    fp = fopen(tmpfile,"w");
    // 打开失败，获取文件目录，写入日志
    if (!fp) {
        char *cwdp = getcwd(cwd,MAXPATHLEN);
        // 写日志信息到logfile
        serverLog(LL_WARNING,
            "Failed opening the RDB file %s (in server root dir %s) "
            "for saving: %s",
            filename,
            cwdp ? cwdp : "unknown",
            strerror(errno));
        return C_ERR;
    }

    // 初始化一个rio对象，该对象是一个文件对象IO
    rioInitWithFile(&rdb,fp);
    startSaving(RDBFLAGS_NONE);

    if (server.rdb_save_incremental_fsync)
        rioSetAutoSync(&rdb,REDIS_AUTOSYNC_BYTES);

    if (rdbSaveRio(&rdb,&error,RDBFLAGS_NONE,rsi) == C_ERR) {
        errno = error;
        goto werr;
    }

    /* Make sure data will not remain on the OS's output buffers */
    // 冲洗缓冲区，确保所有的数据都写入磁盘
    if (fflush(fp)) goto werr;
    // 将fp指向的文件同步到磁盘中
    if (fsync(fileno(fp))) goto werr;
    // 关闭文件
    if (fclose(fp)) { fp = NULL; goto werr; }
    fp = NULL;
    
    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    // 原子性改变rdb文件的名字
    if (rename(tmpfile,filename) == -1) {
        // 改变名字失败，则获得当前目录路径，发送日志信息，删除临时文件
        char *cwdp = getcwd(cwd,MAXPATHLEN);
        serverLog(LL_WARNING,
            "Error moving temp DB file %s on the final "
            "destination %s (in server root dir %s): %s",
            tmpfile,
            filename,
            cwdp ? cwdp : "unknown",
            strerror(errno));
        unlink(tmpfile);
        stopSaving(0);
        return C_ERR;
    }

    // 写日志文件
    serverLog(LL_NOTICE,"DB saved on disk");
    // 重置服务器的脏键
    server.dirty = 0;
    // 更新上一次SAVE操作的时间
    server.lastsave = time(NULL);
    // 更新SAVE操作的状态
    server.lastbgsave_status = C_OK;
    stopSaving(1);
    return C_OK;

    //dbSaveRio()函数的写错误处理，写日志，关闭文件，删除临时文件，发送C_ERR
werr:
    serverLog(LL_WARNING,"Write error saving DB on disk: %s", strerror(errno));
    if (fp) fclose(fp);
    unlink(tmpfile);
    stopSaving(0);
    return C_ERR;
}

// 后台进行RDB持久化BGSAVE操作
int rdbSaveBackground(char *filename, rdbSaveInfo *rsi) {
    pid_t childpid;

    // 当前没有正在进行AOF和RDB操作，否则返回C_ERR
    if (hasActiveChildProcess()) return C_ERR;

    // 备份当前数据库的脏键值
    server.dirty_before_bgsave = server.dirty;
    // 最近一个执行BGSAVE的时间
    server.lastbgsave_try = time(NULL);
    // fork函数开始时间，记录fork函数的耗时
    openChildInfoPipe();

    // 创建子进程
    if ((childpid = redisFork(CHILD_TYPE_RDB)) == 0) {
        int retval;

        // 子进程执行的代码
        /* Child */

        // 设置进程标题，方便识别
        redisSetProcTitle("redis-rdb-bgsave");
        redisSetCpuAffinity(server.bgsave_cpulist);
        // 执行保存操作，将数据库的写到filename文件中
        retval = rdbSave(filename,rsi);
        if (retval == C_OK) {
            sendChildCOWInfo(CHILD_TYPE_RDB, "RDB");
        }
        // 子进程退出，发送信号给父进程，发送0表示BGSAVE成功，1表示失败
        exitFromChild((retval == C_OK) ? 0 : 1);
    } else {
        // 父进程执行的代码
        /* Parent */

        // 如果fork出错
        if (childpid == -1) {
            closeChildInfoPipe();
            server.lastbgsave_status = C_ERR;
            serverLog(LL_WARNING,"Can't save in background: fork: %s",
                strerror(errno));
            return C_ERR;
        }
        serverLog(LL_NOTICE,"Background saving started by pid %d",childpid);
        server.rdb_save_time_start = time(NULL);    //设置BGSAVE开始的时间
        server.rdb_child_pid = childpid;            //设置负责执行BGSAVE操作的子进程id
        server.rdb_child_type = RDB_CHILD_TYPE_DISK;//设置BGSAVE的类型，往磁盘中写入
        //关闭哈希表的resize，因为resize过程中会有复制拷贝动作
        return C_OK;
    }
    return C_OK; /* unreached */
}

/* Note that we may call this function in signal handle 'sigShutdownHandler',
 * so we need guarantee all functions we call are async-signal-safe.
 * If  we call this function from signal handle, we won't call bg_unlik that
 * is not async-signal-safe. */
// 删除临时文件，当BGSAVE执行被中断时使用
void rdbRemoveTempFile(pid_t childpid, int from_signal) {
    char tmpfile[256];
    char pid[32];

    /* Generate temp rdb file name using aync-signal safe functions. */
    int pid_len = ll2string(pid, sizeof(pid), childpid);
    strcpy(tmpfile, "temp-");
    strncpy(tmpfile+5, pid, pid_len);
    strcpy(tmpfile+5+pid_len, ".rdb");

    if (from_signal) {
        /* bg_unlink is not async-signal-safe, but in this case we don't really
         * need to close the fd, it'll be released when the process exists. */
        int fd = open(tmpfile, O_RDONLY|O_NONBLOCK);
        UNUSED(fd);
        unlink(tmpfile);
    } else {
        bg_unlink(tmpfile);
    }
}

/* This function is called by rdbLoadObject() when the code is in RDB-check
 * mode and we find a module value of type 2 that can be parsed without
 * the need of the actual module. The value is parsed for errors, finally
 * a dummy redis object is returned just to conform to the API. */
robj *rdbLoadCheckModuleValue(rio *rdb, char *modulename) {
    uint64_t opcode;
    while((opcode = rdbLoadLen(rdb,NULL)) != RDB_MODULE_OPCODE_EOF) {
        if (opcode == RDB_MODULE_OPCODE_SINT ||
            opcode == RDB_MODULE_OPCODE_UINT)
        {
            uint64_t len;
            if (rdbLoadLenByRef(rdb,NULL,&len) == -1) {
                rdbExitReportCorruptRDB(
                    "Error reading integer from module %s value", modulename);
            }
        } else if (opcode == RDB_MODULE_OPCODE_STRING) {
            robj *o = rdbGenericLoadStringObject(rdb,RDB_LOAD_NONE,NULL);
            if (o == NULL) {
                rdbExitReportCorruptRDB(
                    "Error reading string from module %s value", modulename);
            }
            decrRefCount(o);
        } else if (opcode == RDB_MODULE_OPCODE_FLOAT) {
            float val;
            if (rdbLoadBinaryFloatValue(rdb,&val) == -1) {
                rdbExitReportCorruptRDB(
                    "Error reading float from module %s value", modulename);
            }
        } else if (opcode == RDB_MODULE_OPCODE_DOUBLE) {
            double val;
            if (rdbLoadBinaryDoubleValue(rdb,&val) == -1) {
                rdbExitReportCorruptRDB(
                    "Error reading double from module %s value", modulename);
            }
        }
    }
    return createStringObject("module-dummy-value",18);
}

/* Load a Redis object of the specified type from the specified file.
 * On success a newly allocated object is returned, otherwise NULL. */
// 从rio中读出一个rdbtype类型的对象，成功返回新对象地址，否则返回NULL
robj *rdbLoadObject(int rdbtype, rio *rdb, sds key) {
    robj *o = NULL, *ele, *dec;
    uint64_t len;
    unsigned int i;

    // 读出一个字符串对象
    if (rdbtype == RDB_TYPE_STRING) {
        /* Read string value */
        // 从rio中读出一个已经编码过的字符串对象
        if ((o = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
        // 优化编码
        o = tryObjectEncoding(o);
        // 读出一个列表对象
    } else if (rdbtype == RDB_TYPE_LIST) {
        /* Read list value */
        // 从rio中读出一个长度，表示列表的元素个数
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;

        // 创建一个quicklist的列表对象
        o = createQuicklistObject();
        // 设置列表的压缩程度和ziplist最大的长度
        quicklistSetOptions(o->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);

        /* Load every single element of the list */
        // 读出len个元素
        while(len--) {
            // 从rio中读出已经编码过的字符串对象
            if ((ele = rdbLoadEncodedStringObject(rdb)) == NULL) {
                decrRefCount(o);
                return NULL;
            }
            // 解码成字符串对象
            dec = getDecodedObject(ele);
            // 获得字符串的长度
            size_t len = sdslen(dec->ptr);
            // 将读出来的字符串对象push进quicklist
            quicklistPushTail(o->ptr, dec->ptr, len);
            decrRefCount(dec);
            decrRefCount(ele);
        }
        // 读出一个集合对象
    } else if (rdbtype == RDB_TYPE_SET) {
        /* Read Set value */
        // 读出集合的成员个数
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;

        /* Use a regular set when there are too many entries. */
        // 根据集合成员的数量，如果大于配置的最多intset节点数量，则创建一个字典编码的集合对象
        if (len > server.set_max_intset_entries) {
            o = createSetObject();
            /* It's faster to expand the dict to the right size asap in order
             * to avoid rehashing */
            if (len > DICT_HT_INITIAL_SIZE) //按需扩展集合大小
                dictExpand(o->ptr,len);
            // 小于则创建一个intset编码的集合对象
        } else {
            o = createIntsetObject();
        }

        /* Load every single element of the set */
        // 读入len个元素
        for (i = 0; i < len; i++) {
            long long llval;
            sds sdsele;

            // 从rio中读出一个元素
            if ((sdsele = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                decrRefCount(o);
                return NULL;
            }
            // 将元素添加到intset中
            if (o->encoding == OBJ_ENCODING_INTSET) {
                /* Fetch integer value from element. */
                // 尝试将元素对象转换为longlong类型，保存在llval中，如果可以则加入到intset中
                if (isSdsRepresentableAsLongLong(sdsele,&llval) == C_OK) {
                    o->ptr = intsetAdd(o->ptr,llval,NULL);

                    // 如果不行，则进行编码转换，保存到字典类型的集合中
                } else {
                    setTypeConvert(o,OBJ_ENCODING_HT);
                    dictExpand(o->ptr,len);
                }
            }

            /* This will also be called when the set was just converted
             * to a regular hash table encoded set. */
            // 如果是字典类型的集合对象，将元素对象加入到字典中
            if (o->encoding == OBJ_ENCODING_HT) {
                dictAdd((dict*)o->ptr,sdsele,NULL);
            } else {
                sdsfree(sdsele);
            }
        }
        // 读出一个有序集合对象
    } else if (rdbtype == RDB_TYPE_ZSET_2 || rdbtype == RDB_TYPE_ZSET) {
        /* Read list/set value. */
        uint64_t zsetlen;
        size_t maxelelen = 0;
        zset *zs;

        // 读出一个有序集合的元素个数
        if ((zsetlen = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        // 创建一个有序集合对象
        o = createZsetObject();
        zs = o->ptr;

        //集合扩展
        if (zsetlen > DICT_HT_INITIAL_SIZE)
            dictExpand(zs->dict,zsetlen);

        /* Load every single element of the sorted set. */
        // 读出每一个有序集合对象
        while(zsetlen--) {
            sds sdsele;
            double score;
            zskiplistNode *znode;

            // 从rio中读出一个有序集合的元素对象
            if ((sdsele = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                decrRefCount(o);
                return NULL;
            }

            if (rdbtype == RDB_TYPE_ZSET_2) {
                if (rdbLoadBinaryDoubleValue(rdb,&score) == -1) {
                    decrRefCount(o);
                    sdsfree(sdsele);
                    return NULL;
                }
            } else {
                if (rdbLoadDoubleValue(rdb,&score) == -1) {
                    decrRefCount(o);
                    sdsfree(sdsele);
                    return NULL;
                }
            }

            /* Don't care about integer-encoded strings. */
            // 保存成员的最大长度
            if (sdslen(sdsele) > maxelelen) maxelelen = sdslen(sdsele);

            // 将当前元素对象插入到有序集合的跳跃表和字典中
            znode = zslInsert(zs->zsl,score,sdsele);
            dictAdd(zs->dict,sdsele,&znode->score);
        }

        /* Convert *after* loading, since sorted sets are not stored ordered. */
        // 如果可以进行编码转换，那么转换成ziplist编码，节约空间
        if (zsetLength(o) <= server.zset_max_ziplist_entries &&
            maxelelen <= server.zset_max_ziplist_value)
                zsetConvert(o,OBJ_ENCODING_ZIPLIST);
        // 读出一个哈希对象
    } else if (rdbtype == RDB_TYPE_HASH) {
        uint64_t len;
        int ret;
        sds field, value;

        // 读出哈希节点数量
        len = rdbLoadLen(rdb, NULL);
        if (len == RDB_LENERR) return NULL;

        // 创建一个哈希对象，默认压缩列表编码类型
        o = createHashObject();

        /* Too many entries? Use a hash table. */
        // 如果节点数超过限制，则转换成字典编码类型
        if (len > server.hash_max_ziplist_entries)
            hashTypeConvert(o, OBJ_ENCODING_HT);

        /* Load every field and value into the ziplist */
        // 将field和value读到ziplist编码的哈希对象中将field和value读到ziplist编码的哈希对象中
        while (o->encoding == OBJ_ENCODING_ZIPLIST && len > 0) {
            len--;
            /* Load raw strings */
            // 读出field和value对象
            if ((field = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                decrRefCount(o);
                return NULL;
            }
            if ((value = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                sdsfree(field);
                decrRefCount(o);
                return NULL;
            }

            /* Add pair to ziplist */
            // 将field和value对象push到ziplist中
            o->ptr = ziplistPush(o->ptr, (unsigned char*)field,
                    sdslen(field), ZIPLIST_TAIL);
            o->ptr = ziplistPush(o->ptr, (unsigned char*)value,
                    sdslen(value), ZIPLIST_TAIL);

            /* Convert to hash table if size threshold is exceeded */
            // 如果超过了ziplist的限制，则要将编码转换字典类型
            if (sdslen(field) > server.hash_max_ziplist_value ||
                sdslen(value) > server.hash_max_ziplist_value)
            {
                sdsfree(field);
                sdsfree(value);
                hashTypeConvert(o, OBJ_ENCODING_HT);
                break;
            }
            sdsfree(field);
            sdsfree(value);
        }

        //如果长度超长，则进行扩容操作
        if (o->encoding == OBJ_ENCODING_HT && len > DICT_HT_INITIAL_SIZE)
            dictExpand(o->ptr,len);

        /* Load remaining fields and values into the hash table */
        // 将field和value读到ht编码的哈希对象中
        while (o->encoding == OBJ_ENCODING_HT && len > 0) {
            len--;
            /* Load encoded strings */
            // 从rio中读出一个编码过的字符串field对象
            if ((field = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                decrRefCount(o);
                return NULL;
            }
            // 从rio中读出一个编码过的字符串value对象
            if ((value = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL) {
                sdsfree(field);
                decrRefCount(o);
                return NULL;
            }

            /* Add pair to hash table */
            // 将field和value对象将入哈希对象中
            ret = dictAdd((dict*)o->ptr, field, value);
            if (ret == DICT_ERR) {
                rdbExitReportCorruptRDB("Duplicate keys detected");
            }
        }

        /* All pairs should be read by now */
        serverAssert(len == 0);
        // 读出一个quicklist类型的列表
    } else if (rdbtype == RDB_TYPE_LIST_QUICKLIST) {
        // 读出quicklist的节点
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        // 创建一个quicklist编码的列表对象
        o = createQuicklistObject();
        // 设置列表的压缩程度和ziplist最大的长度
        quicklistSetOptions(o->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);

        // 读出len个节点
        while (len--) {
            // 从rio中读出一个quicklistNode节点的ziplist地址
            unsigned char *zl =
                rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,NULL);
            if (zl == NULL) {
                decrRefCount(o);
                return NULL;
            }
            // 将当前ziplist加入quicklistNode节点中，然后将该节点加到quicklist中
            quicklistAppendZiplist(o->ptr, zl);
        }
    } else if (rdbtype == RDB_TYPE_HASH_ZIPMAP  ||
               rdbtype == RDB_TYPE_LIST_ZIPLIST ||
               rdbtype == RDB_TYPE_SET_INTSET   ||
               rdbtype == RDB_TYPE_ZSET_ZIPLIST ||
               rdbtype == RDB_TYPE_HASH_ZIPLIST)
    {
        // 读出一个字符串值
        unsigned char *encoded =
            rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,NULL);
        if (encoded == NULL) return NULL;
        // 创建字符串
        o = createObject(OBJ_STRING,encoded); /* Obj type fixed below. */

        /* Fix the object encoding, and make sure to convert the encoded
         * data type into the base type if accordingly to the current
         * configuration there are too many elements in the encoded data
         * type. Note that we only check the length and not max element
         * size as this is an O(N) scan. Eventually everything will get
         * converted. */
        // 根据读取的编码类型，将值回复成原来的编码对象
        switch(rdbtype) {
            case RDB_TYPE_HASH_ZIPMAP:  //将zipmap转换为ziplist，zipmap已经不在使用
                /* Convert to ziplist encoded hash. This must be deprecated
                 * when loading dumps created by Redis 2.4 gets deprecated. */
                {
                    unsigned char *zl = ziplistNew();
                    unsigned char *zi = zipmapRewind(o->ptr);
                    unsigned char *fstr, *vstr;
                    unsigned int flen, vlen;
                    unsigned int maxlen = 0;

                    while ((zi = zipmapNext(zi, &fstr, &flen, &vstr, &vlen)) != NULL) {
                        if (flen > maxlen) maxlen = flen;
                        if (vlen > maxlen) maxlen = vlen;
                        zl = ziplistPush(zl, fstr, flen, ZIPLIST_TAIL);
                        zl = ziplistPush(zl, vstr, vlen, ZIPLIST_TAIL);
                    }

                    zfree(o->ptr);
                    o->ptr = zl;
                    o->type = OBJ_HASH;
                    o->encoding = OBJ_ENCODING_ZIPLIST;

                    if (hashTypeLength(o) > server.hash_max_ziplist_entries ||
                        maxlen > server.hash_max_ziplist_value)
                    {
                        hashTypeConvert(o, OBJ_ENCODING_HT);
                    }
                }
                break;
            case RDB_TYPE_LIST_ZIPLIST: //ziplist编码的列表，3.2版本不使用ziplist作为列表的编码
                o->type = OBJ_LIST;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                listTypeConvert(o,OBJ_ENCODING_QUICKLIST);  //转换为quicklist
                break;
            case RDB_TYPE_SET_INTSET:   //整数集合编码的集合
                o->type = OBJ_SET;
                o->encoding = OBJ_ENCODING_INTSET;
                if (intsetLen(o->ptr) > server.set_max_intset_entries)  //按需转换为字典类型编码
                    setTypeConvert(o,OBJ_ENCODING_HT);
                break;
            case RDB_TYPE_ZSET_ZIPLIST: //压缩列表编码的有序集合对象
                o->type = OBJ_ZSET;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                if (zsetLength(o) > server.zset_max_ziplist_entries)    //按需转换为跳跃表类型编码
                    zsetConvert(o,OBJ_ENCODING_SKIPLIST);
                break;
            case RDB_TYPE_HASH_ZIPLIST: // 压缩列表编码的哈希对象
                o->type = OBJ_HASH;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                if (hashTypeLength(o) > server.hash_max_ziplist_entries)    //按需转换为字典集合对象
                    hashTypeConvert(o, OBJ_ENCODING_HT);
                break;
            default:
                /* totally unreachable */
                rdbExitReportCorruptRDB("Unknown RDB encoding type %d",rdbtype);
                break;
        }
    } else if (rdbtype == RDB_TYPE_STREAM_LISTPACKS) {
        o = createStreamObject();
        stream *s = o->ptr;
        uint64_t listpacks = rdbLoadLen(rdb,NULL);
        if (listpacks == RDB_LENERR) {
            rdbReportReadError("Stream listpacks len loading failed.");
            decrRefCount(o);
            return NULL;
        }

        while(listpacks--) {
            /* Get the master ID, the one we'll use as key of the radix tree
             * node: the entries inside the listpack itself are delta-encoded
             * relatively to this ID. */
            sds nodekey = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
            if (nodekey == NULL) {
                rdbReportReadError("Stream master ID loading failed: invalid encoding or I/O error.");
                decrRefCount(o);
                return NULL;
            }
            if (sdslen(nodekey) != sizeof(streamID)) {
                rdbExitReportCorruptRDB("Stream node key entry is not the "
                                        "size of a stream ID");
            }

            /* Load the listpack. */
            unsigned char *lp =
                rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,NULL);
            if (lp == NULL) {
                rdbReportReadError("Stream listpacks loading failed.");
                sdsfree(nodekey);
                decrRefCount(o);
                return NULL;
            }
            unsigned char *first = lpFirst(lp);
            if (first == NULL) {
                /* Serialized listpacks should never be empty, since on
                 * deletion we should remove the radix tree key if the
                 * resulting listpack is empty. */
                rdbExitReportCorruptRDB("Empty listpack inside stream");
            }

            /* Insert the key in the radix tree. */
            int retval = raxInsert(s->rax,
                (unsigned char*)nodekey,sizeof(streamID),lp,NULL);
            sdsfree(nodekey);
            if (!retval)
                rdbExitReportCorruptRDB("Listpack re-added with existing key");
        }
        /* Load total number of items inside the stream. */
        s->length = rdbLoadLen(rdb,NULL);

        /* Load the last entry ID. */
        s->last_id.ms = rdbLoadLen(rdb,NULL);
        s->last_id.seq = rdbLoadLen(rdb,NULL);

        if (rioGetReadError(rdb)) {
            rdbReportReadError("Stream object metadata loading failed.");
            decrRefCount(o);
            return NULL;
        }

        /* Consumer groups loading */
        uint64_t cgroups_count = rdbLoadLen(rdb,NULL);
        if (cgroups_count == RDB_LENERR) {
            rdbReportReadError("Stream cgroup count loading failed.");
            decrRefCount(o);
            return NULL;
        }
        while(cgroups_count--) {
            /* Get the consumer group name and ID. We can then create the
             * consumer group ASAP and populate its structure as
             * we read more data. */
            streamID cg_id;
            sds cgname = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
            if (cgname == NULL) {
                rdbReportReadError(
                    "Error reading the consumer group name from Stream");
                decrRefCount(o);
                return NULL;
            }

            cg_id.ms = rdbLoadLen(rdb,NULL);
            cg_id.seq = rdbLoadLen(rdb,NULL);
            if (rioGetReadError(rdb)) {
                rdbReportReadError("Stream cgroup ID loading failed.");
                sdsfree(cgname);
                decrRefCount(o);
                return NULL;
            }

            streamCG *cgroup = streamCreateCG(s,cgname,sdslen(cgname),&cg_id);
            if (cgroup == NULL)
                rdbExitReportCorruptRDB("Duplicated consumer group name %s",
                                         cgname);
            sdsfree(cgname);

            /* Load the global PEL for this consumer group, however we'll
             * not yet populate the NACK structures with the message
             * owner, since consumers for this group and their messages will
             * be read as a next step. So for now leave them not resolved
             * and later populate it. */
            uint64_t pel_size = rdbLoadLen(rdb,NULL);
            if (pel_size == RDB_LENERR) {
                rdbReportReadError("Stream PEL size loading failed.");
                decrRefCount(o);
                return NULL;
            }
            while(pel_size--) {
                unsigned char rawid[sizeof(streamID)];
                if (rioRead(rdb,rawid,sizeof(rawid)) == 0) {
                    rdbReportReadError("Stream PEL ID loading failed.");
                    decrRefCount(o);
                    return NULL;
                }
                streamNACK *nack = streamCreateNACK(NULL);
                nack->delivery_time = rdbLoadMillisecondTime(rdb,RDB_VERSION);
                nack->delivery_count = rdbLoadLen(rdb,NULL);
                if (rioGetReadError(rdb)) {
                    rdbReportReadError("Stream PEL NACK loading failed.");
                    decrRefCount(o);
                    streamFreeNACK(nack);
                    return NULL;
                }
                if (!raxInsert(cgroup->pel,rawid,sizeof(rawid),nack,NULL))
                    rdbExitReportCorruptRDB("Duplicated gobal PEL entry "
                                            "loading stream consumer group");
            }

            /* Now that we loaded our global PEL, we need to load the
             * consumers and their local PELs. */
            uint64_t consumers_num = rdbLoadLen(rdb,NULL);
            if (consumers_num == RDB_LENERR) {
                rdbReportReadError("Stream consumers num loading failed.");
                decrRefCount(o);
                return NULL;
            }
            while(consumers_num--) {
                sds cname = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
                if (cname == NULL) {
                    rdbReportReadError(
                        "Error reading the consumer name from Stream group.");
                    decrRefCount(o);
                    return NULL;
                }
                streamConsumer *consumer =
                    streamLookupConsumer(cgroup,cname,SLC_NONE);
                sdsfree(cname);
                consumer->seen_time = rdbLoadMillisecondTime(rdb,RDB_VERSION);
                if (rioGetReadError(rdb)) {
                    rdbReportReadError("Stream short read reading seen time.");
                    decrRefCount(o);
                    return NULL;
                }

                /* Load the PEL about entries owned by this specific
                 * consumer. */
                pel_size = rdbLoadLen(rdb,NULL);
                if (pel_size == RDB_LENERR) {
                    rdbReportReadError(
                        "Stream consumer PEL num loading failed.");
                    decrRefCount(o);
                    return NULL;
                }
                while(pel_size--) {
                    unsigned char rawid[sizeof(streamID)];
                    if (rioRead(rdb,rawid,sizeof(rawid)) == 0) {
                        rdbReportReadError(
                            "Stream short read reading PEL streamID.");
                        decrRefCount(o);
                        return NULL;
                    }
                    streamNACK *nack = raxFind(cgroup->pel,rawid,sizeof(rawid));
                    if (nack == raxNotFound)
                        rdbExitReportCorruptRDB("Consumer entry not found in "
                                                "group global PEL");

                    /* Set the NACK consumer, that was left to NULL when
                     * loading the global PEL. Then set the same shared
                     * NACK structure also in the consumer-specific PEL. */
                    nack->consumer = consumer;
                    if (!raxInsert(consumer->pel,rawid,sizeof(rawid),nack,NULL))
                        rdbExitReportCorruptRDB("Duplicated consumer PEL entry "
                                                " loading a stream consumer "
                                                "group");
                }
            }
        }
    } else if (rdbtype == RDB_TYPE_MODULE || rdbtype == RDB_TYPE_MODULE_2) {
        uint64_t moduleid = rdbLoadLen(rdb,NULL);
        if (rioGetReadError(rdb)) {
            rdbReportReadError("Short read module id");
            return NULL;
        }
        moduleType *mt = moduleTypeLookupModuleByID(moduleid);
        char name[10];

        if (rdbCheckMode && rdbtype == RDB_TYPE_MODULE_2) {
            moduleTypeNameByID(name,moduleid);
            return rdbLoadCheckModuleValue(rdb,name);
        }

        if (mt == NULL) {
            moduleTypeNameByID(name,moduleid);
            serverLog(LL_WARNING,"The RDB file contains module data I can't load: no matching module '%s'", name);
            exit(1);
        }
        RedisModuleIO io;
        robj keyobj;
        initStaticStringObject(keyobj,key);
        moduleInitIOContext(io,mt,rdb,&keyobj);
        io.ver = (rdbtype == RDB_TYPE_MODULE) ? 1 : 2;
        /* Call the rdb_load method of the module providing the 10 bit
         * encoding version in the lower 10 bits of the module ID. */
        void *ptr = mt->rdb_load(&io,moduleid&1023);
        if (io.ctx) {
            moduleFreeContext(io.ctx);
            zfree(io.ctx);
        }

        /* Module v2 serialization has an EOF mark at the end. */
        if (io.ver == 2) {
            uint64_t eof = rdbLoadLen(rdb,NULL);
            if (eof == RDB_LENERR) {
                o = createModuleObject(mt,ptr); /* creating just in order to easily destroy */
                decrRefCount(o);
                return NULL;
            }
            if (eof != RDB_MODULE_OPCODE_EOF) {
                serverLog(LL_WARNING,"The RDB file contains module data for the module '%s' that is not terminated by the proper module value EOF marker", name);
                exit(1);
            }
        }

        if (ptr == NULL) {
            moduleTypeNameByID(name,moduleid);
            serverLog(LL_WARNING,"The RDB file contains module data for the module type '%s', that the responsible module is not able to load. Check for modules log above for additional clues.", name);
            exit(1);
        }
        o = createModuleObject(mt,ptr);
    } else {
        rdbReportReadError("Unknown RDB encoding type %d",rdbtype);
        return NULL;
    }
    return o;
}

/* Mark that we are loading in the global state and setup the fields
 * needed to provide loading stats. */
// 设置载入的状态信息
void startLoading(size_t size, int rdbflags) {
    /* Load the DB */
    server.loading = 1; //正在载入状态
    server.loading_start_time = time(NULL); //载入开始时间
    server.loading_loaded_bytes = 0;    //已载入的字节数
    server.loading_total_bytes = size;

    /* Fire the loading modules start event. */
    int subevent;
    if (rdbflags & RDBFLAGS_AOF_PREAMBLE)
        subevent = REDISMODULE_SUBEVENT_LOADING_AOF_START;
    else if(rdbflags & RDBFLAGS_REPLICATION)
        subevent = REDISMODULE_SUBEVENT_LOADING_REPL_START;
    else
        subevent = REDISMODULE_SUBEVENT_LOADING_RDB_START;
    moduleFireServerEvent(REDISMODULE_EVENT_LOADING,subevent,NULL);
}

/* Mark that we are loading in the global state and setup the fields
 * needed to provide loading stats.
 * 'filename' is optional and used for rdb-check on error */
// 标记我们正在加载全局状态，并设置提供加载状态所需的字段。 'filename'是可选的，用于对错误进行rdb检查
void startLoadingFile(FILE *fp, char* filename, int rdbflags) {
    struct stat sb;
    if (fstat(fileno(fp), &sb) == -1)
        sb.st_size = 0;
    rdbFileBeingLoaded = filename;
    startLoading(sb.st_size, rdbflags);
}

/* Refresh the loading progress info */
// 设置载入时server的状态信息
void loadingProgress(off_t pos) {
    server.loading_loaded_bytes = pos;//更新已载入的字节

    // 更新服务器内存使用的峰值
    if (server.stat_peak_memory < zmalloc_used_memory())
        server.stat_peak_memory = zmalloc_used_memory();
}

/* Loading finished */
// 载入完成
void stopLoading(int success) {
    server.loading = 0;
    rdbFileBeingLoaded = NULL;

    /* Fire the loading modules end event. */
    moduleFireServerEvent(REDISMODULE_EVENT_LOADING,
                          success?
                            REDISMODULE_SUBEVENT_LOADING_ENDED:
                            REDISMODULE_SUBEVENT_LOADING_FAILED,
                          NULL);
}

void startSaving(int rdbflags) {
    /* Fire the persistence modules end event. */
    int subevent;
    if (rdbflags & RDBFLAGS_AOF_PREAMBLE)
        subevent = REDISMODULE_SUBEVENT_PERSISTENCE_AOF_START;
    else if (getpid()!=server.pid)
        subevent = REDISMODULE_SUBEVENT_PERSISTENCE_RDB_START;
    else
        subevent = REDISMODULE_SUBEVENT_PERSISTENCE_SYNC_RDB_START;
    moduleFireServerEvent(REDISMODULE_EVENT_PERSISTENCE,subevent,NULL);
}

void stopSaving(int success) {
    /* Fire the persistence modules end event. */
    moduleFireServerEvent(REDISMODULE_EVENT_PERSISTENCE,
                          success?
                            REDISMODULE_SUBEVENT_PERSISTENCE_ENDED:
                            REDISMODULE_SUBEVENT_PERSISTENCE_FAILED,
                          NULL);
}

/* Track loading progress in order to serve client's from time to time
   and if needed calculate rdb checksum  */
// 跟踪载入的信息，以便client进行查询，在rdb查询和时也需要
void rdbLoadProgressCallback(rio *r, const void *buf, size_t len) {
    // 如果设置了校验和，则进行校验和计算
    if (server.rdb_checksum)
        rioGenericUpdateChecksum(r, buf, len);
    // loading_process_events_interval_bytes 在server初始化是设置为2M
    // 在load时，用来设置读或写的最大字节数max_processing_chunk
    if (server.loading_process_events_interval_bytes &&
        (r->processed_bytes + len)/server.loading_process_events_interval_bytes > r->processed_bytes/server.loading_process_events_interval_bytes)
    {
        /* The DB can take some non trivial amount of time to load. Update
         * our cached time since it is used to create and update the last
         * interaction time with clients and for other important things. */
        // 数据库采用非琐碎的时间来加载，更新缓存时间因为它被用来创建和更新最后和client的互动时间
        updateCachedTime(0);
        // 如果环境为从节点，当前处于rdb文件的复制传输状态
        if (server.masterhost && server.repl_state == REPL_STATE_TRANSFER)
            // 发送空行给主节点，防止超时连接
            replicationSendNewlineToMaster();
        // 刷新载入进度信息
        loadingProgress(r->processed_bytes);
        //让服务器在被阻塞的情况下，仍然处理某些网络事件
        processEventsWhileBlocked();
        processModuleLoadingProgressEvent(0);
    }
}

/* Load an RDB file from the rio stream 'rdb'. On success C_OK is returned,
 * otherwise C_ERR is returned and 'errno' is set accordingly. */
int rdbLoadRio(rio *rdb, int rdbflags, rdbSaveInfo *rsi) {
    uint64_t dbid;
    int type, rdbver;
    redisDb *db = server.db+0;
    char buf[1024];

    rdb->update_cksum = rdbLoadProgressCallback;
    rdb->max_processing_chunk = server.loading_process_events_interval_bytes;
    if (rioRead(rdb,buf,9) == 0) goto eoferr;
    buf[9] = '\0';
    if (memcmp(buf,"REDIS",5) != 0) {
        serverLog(LL_WARNING,"Wrong signature trying to load DB from file");
        errno = EINVAL;
        return C_ERR;
    }
    rdbver = atoi(buf+5);
    if (rdbver < 1 || rdbver > RDB_VERSION) {
        serverLog(LL_WARNING,"Can't handle RDB format version %d",rdbver);
        errno = EINVAL;
        return C_ERR;
    }

    /* Key-specific attributes, set by opcodes before the key type. */
    long long lru_idle = -1, lfu_freq = -1, expiretime = -1, now = mstime();
    long long lru_clock = LRU_CLOCK();

    while(1) {
        sds key;
        robj *val;

        /* Read type. */
        if ((type = rdbLoadType(rdb)) == -1) goto eoferr;

        /* Handle special types. */
        if (type == RDB_OPCODE_EXPIRETIME) {
            /* EXPIRETIME: load an expire associated with the next key
             * to load. Note that after loading an expire we need to
             * load the actual type, and continue. */
            expiretime = rdbLoadTime(rdb);
            expiretime *= 1000;
            if (rioGetReadError(rdb)) goto eoferr;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EXPIRETIME_MS) {
            /* EXPIRETIME_MS: milliseconds precision expire times introduced
             * with RDB v3. Like EXPIRETIME but no with more precision. */
            expiretime = rdbLoadMillisecondTime(rdb,rdbver);
            if (rioGetReadError(rdb)) goto eoferr;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_FREQ) {
            /* FREQ: LFU frequency. */
            uint8_t byte;
            if (rioRead(rdb,&byte,1) == 0) goto eoferr;
            lfu_freq = byte;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_IDLE) {
            /* IDLE: LRU idle time. */
            uint64_t qword;
            if ((qword = rdbLoadLen(rdb,NULL)) == RDB_LENERR) goto eoferr;
            lru_idle = qword;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EOF) {
            /* EOF: End of file, exit the main loop. */
            break;
        } else if (type == RDB_OPCODE_SELECTDB) {
            /* SELECTDB: Select the specified database. */
            if ((dbid = rdbLoadLen(rdb,NULL)) == RDB_LENERR) goto eoferr;
            if (dbid >= (unsigned)server.dbnum) {
                serverLog(LL_WARNING,
                    "FATAL: Data file was created with a Redis "
                    "server configured to handle more than %d "
                    "databases. Exiting\n", server.dbnum);
                exit(1);
            }
            db = server.db+dbid;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_RESIZEDB) {
            /* RESIZEDB: Hint about the size of the keys in the currently
             * selected data base, in order to avoid useless rehashing. */
            uint64_t db_size, expires_size;
            if ((db_size = rdbLoadLen(rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            if ((expires_size = rdbLoadLen(rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            dictExpand(db->dict,db_size);
            dictExpand(db->expires,expires_size);
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_AUX) {
            /* AUX: generic string-string fields. Use to add state to RDB
             * which is backward compatible. Implementations of RDB loading
             * are required to skip AUX fields they don't understand.
             *
             * An AUX field is composed of two strings: key and value. */
            robj *auxkey, *auxval;
            if ((auxkey = rdbLoadStringObject(rdb)) == NULL) goto eoferr;
            if ((auxval = rdbLoadStringObject(rdb)) == NULL) goto eoferr;

            if (((char*)auxkey->ptr)[0] == '%') {
                /* All the fields with a name staring with '%' are considered
                 * information fields and are logged at startup with a log
                 * level of NOTICE. */
                serverLog(LL_NOTICE,"RDB '%s': %s",
                    (char*)auxkey->ptr,
                    (char*)auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"repl-stream-db")) {
                if (rsi) rsi->repl_stream_db = atoi(auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"repl-id")) {
                if (rsi && sdslen(auxval->ptr) == CONFIG_RUN_ID_SIZE) {
                    memcpy(rsi->repl_id,auxval->ptr,CONFIG_RUN_ID_SIZE+1);
                    rsi->repl_id_is_set = 1;
                }
            } else if (!strcasecmp(auxkey->ptr,"repl-offset")) {
                if (rsi) rsi->repl_offset = strtoll(auxval->ptr,NULL,10);
            } else if (!strcasecmp(auxkey->ptr,"lua")) {
                /* Load the script back in memory. */
                if (luaCreateFunction(NULL,server.lua,auxval) == NULL) {
                    rdbExitReportCorruptRDB(
                        "Can't load Lua script from RDB file! "
                        "BODY: %s", (char*)auxval->ptr);
                }
            } else if (!strcasecmp(auxkey->ptr,"redis-ver")) {
                serverLog(LL_NOTICE,"Loading RDB produced by version %s",
                    (char*)auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"ctime")) {
                time_t age = time(NULL)-strtol(auxval->ptr,NULL,10);
                if (age < 0) age = 0;
                serverLog(LL_NOTICE,"RDB age %ld seconds",
                    (unsigned long) age);
            } else if (!strcasecmp(auxkey->ptr,"used-mem")) {
                long long usedmem = strtoll(auxval->ptr,NULL,10);
                serverLog(LL_NOTICE,"RDB memory usage when created %.2f Mb",
                    (double) usedmem / (1024*1024));
            } else if (!strcasecmp(auxkey->ptr,"aof-preamble")) {
                long long haspreamble = strtoll(auxval->ptr,NULL,10);
                if (haspreamble) serverLog(LL_NOTICE,"RDB has an AOF tail");
            } else if (!strcasecmp(auxkey->ptr,"redis-bits")) {
                /* Just ignored. */
            } else {
                /* We ignore fields we don't understand, as by AUX field
                 * contract. */
                serverLog(LL_DEBUG,"Unrecognized RDB AUX field: '%s'",
                    (char*)auxkey->ptr);
            }

            decrRefCount(auxkey);
            decrRefCount(auxval);
            continue; /* Read type again. */
        } else if (type == RDB_OPCODE_MODULE_AUX) {
            /* Load module data that is not related to the Redis key space.
             * Such data can be potentially be stored both before and after the
             * RDB keys-values section. */
            uint64_t moduleid = rdbLoadLen(rdb,NULL);
            int when_opcode = rdbLoadLen(rdb,NULL);
            int when = rdbLoadLen(rdb,NULL);
            if (rioGetReadError(rdb)) goto eoferr;
            if (when_opcode != RDB_MODULE_OPCODE_UINT)
                rdbReportReadError("bad when_opcode");
            moduleType *mt = moduleTypeLookupModuleByID(moduleid);
            char name[10];
            moduleTypeNameByID(name,moduleid);

            if (!rdbCheckMode && mt == NULL) {
                /* Unknown module. */
                serverLog(LL_WARNING,"The RDB file contains AUX module data I can't load: no matching module '%s'", name);
                exit(1);
            } else if (!rdbCheckMode && mt != NULL) {
                if (!mt->aux_load) {
                    /* Module doesn't support AUX. */
                    serverLog(LL_WARNING,"The RDB file contains module AUX data, but the module '%s' doesn't seem to support it.", name);
                    exit(1);
                }

                RedisModuleIO io;
                moduleInitIOContext(io,mt,rdb,NULL);
                io.ver = 2;
                /* Call the rdb_load method of the module providing the 10 bit
                 * encoding version in the lower 10 bits of the module ID. */
                if (mt->aux_load(&io,moduleid&1023, when) != REDISMODULE_OK || io.error) {
                    moduleTypeNameByID(name,moduleid);
                    serverLog(LL_WARNING,"The RDB file contains module AUX data for the module type '%s', that the responsible module is not able to load. Check for modules log above for additional clues.", name);
                    exit(1);
                }
                if (io.ctx) {
                    moduleFreeContext(io.ctx);
                    zfree(io.ctx);
                }
                uint64_t eof = rdbLoadLen(rdb,NULL);
                if (eof != RDB_MODULE_OPCODE_EOF) {
                    serverLog(LL_WARNING,"The RDB file contains module AUX data for the module '%s' that is not terminated by the proper module value EOF marker", name);
                    exit(1);
                }
                continue;
            } else {
                /* RDB check mode. */
                robj *aux = rdbLoadCheckModuleValue(rdb,name);
                decrRefCount(aux);
                continue; /* Read next opcode. */
            }
        }

        /* Read key */
        if ((key = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL)) == NULL)
            goto eoferr;
        /* Read value */
        if ((val = rdbLoadObject(type,rdb,key)) == NULL) {
            sdsfree(key);
            goto eoferr;
        }

        /* Check if the key already expired. This function is used when loading
         * an RDB file from disk, either at startup, or when an RDB was
         * received from the master. In the latter case, the master is
         * responsible for key expiry. If we would expire keys here, the
         * snapshot taken by the master may not be reflected on the slave.
         * Similarly if the RDB is the preamble of an AOF file, we want to
         * load all the keys as they are, since the log of operations later
         * assume to work in an exact keyspace state. */
        if (iAmMaster() &&
            !(rdbflags&RDBFLAGS_AOF_PREAMBLE) &&
            expiretime != -1 && expiretime < now)
        {
            sdsfree(key);
            decrRefCount(val);
        } else {
            robj keyobj;
            initStaticStringObject(keyobj,key);

            /* Add the new object in the hash table */
            int added = dbAddRDBLoad(db,key,val);
            if (!added) {
                if (rdbflags & RDBFLAGS_ALLOW_DUP) {
                    /* This flag is useful for DEBUG RELOAD special modes.
                     * When it's set we allow new keys to replace the current
                     * keys with the same name. */
                    dbSyncDelete(db,&keyobj);
                    dbAddRDBLoad(db,key,val);
                } else {
                    serverLog(LL_WARNING,
                        "RDB has duplicated key '%s' in DB %d",key,db->id);
                    serverPanic("Duplicated key found in RDB file");
                }
            }

            /* Set the expire time if needed */
            if (expiretime != -1) {
                setExpire(NULL,db,&keyobj,expiretime);
            }

            /* Set usage information (for eviction). */
            objectSetLRUOrLFU(val,lfu_freq,lru_idle,lru_clock,1000);

            /* call key space notification on key loaded for modules only */
            moduleNotifyKeyspaceEvent(NOTIFY_LOADED, "loaded", &keyobj, db->id);
        }

        /* Loading the database more slowly is useful in order to test
         * certain edge cases. */
        if (server.key_load_delay) usleep(server.key_load_delay);

        /* Reset the state that is key-specified and is populated by
         * opcodes before the key, so that we start from scratch again. */
        expiretime = -1;
        lfu_freq = -1;
        lru_idle = -1;
    }
    /* Verify the checksum if RDB version is >= 5 */
    if (rdbver >= 5) {
        uint64_t cksum, expected = rdb->cksum;

        if (rioRead(rdb,&cksum,8) == 0) goto eoferr;
        if (server.rdb_checksum) {
            memrev64ifbe(&cksum);
            if (cksum == 0) {
                serverLog(LL_WARNING,"RDB file was saved with checksum disabled: no check performed.");
            } else if (cksum != expected) {
                serverLog(LL_WARNING,"Wrong RDB checksum expected: (%llx) but "
                    "got (%llx). Aborting now.",
                        (unsigned long long)expected,
                        (unsigned long long)cksum);
                rdbExitReportCorruptRDB("RDB CRC error");
            }
        }
    }
    return C_OK;

    /* Unexpected end of file is handled here calling rdbReportReadError():
     * this will in turn either abort Redis in most cases, or if we are loading
     * the RDB file from a socket during initial SYNC (diskless replica mode),
     * we'll report the error to the caller, so that we can retry. */
eoferr:
    serverLog(LL_WARNING,
        "Short read or OOM loading DB. Unrecoverable error, aborting now.");
    rdbReportReadError("Unexpected EOF reading RDB file");
    return C_ERR;
}

/* Like rdbLoadRio() but takes a filename instead of a rio stream. The
 * filename is open for reading and a rio stream object created in order
 * to do the actual loading. Moreover the ETA displayed in the INFO
 * output is initialized and finalized.
 *
 * If you pass an 'rsi' structure initialied with RDB_SAVE_OPTION_INIT, the
 * loading code will fiil the information fields in the structure. */
// 将指定的RDB文件读到数据库中
int rdbLoad(char *filename, rdbSaveInfo *rsi, int rdbflags) {
    FILE *fp;
    rio rdb;
    int retval;

    if ((fp = fopen(filename,"r")) == NULL) return C_ERR;   // 只读打开文件
    startLoadingFile(fp, filename,rdbflags);
    rioInitWithFile(&rdb,fp);
    retval = rdbLoadRio(&rdb,rdbflags,rsi);
    fclose(fp);
    stopLoading(retval==C_OK);
    return retval;
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of actual BGSAVEs. */
static void backgroundSaveDoneHandlerDisk(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        serverLog(LL_NOTICE,
            "Background saving terminated with success");
        server.dirty = server.dirty - server.dirty_before_bgsave;
        server.lastsave = time(NULL);
        server.lastbgsave_status = C_OK;
    } else if (!bysignal && exitcode != 0) {
        serverLog(LL_WARNING, "Background saving error");
        server.lastbgsave_status = C_ERR;
    } else {
        mstime_t latency;

        serverLog(LL_WARNING,
            "Background saving terminated by signal %d", bysignal);
        latencyStartMonitor(latency);
        rdbRemoveTempFile(server.rdb_child_pid, 0);
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("rdb-unlink-temp-file",latency);
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * triggering an error condition. */
        if (bysignal != SIGUSR1)
            server.lastbgsave_status = C_ERR;
    }
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of RDB -> Slaves socket transfers for
 * diskless replication. */
static void backgroundSaveDoneHandlerSocket(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        serverLog(LL_NOTICE,
            "Background RDB transfer terminated with success");
    } else if (!bysignal && exitcode != 0) {
        serverLog(LL_WARNING, "Background transfer error");
    } else {
        serverLog(LL_WARNING,
            "Background transfer terminated by signal %d", bysignal);
    }
}

/* When a background RDB saving/transfer terminates, call the right handler. */
void backgroundSaveDoneHandler(int exitcode, int bysignal) {
    int type = server.rdb_child_type;
    switch(server.rdb_child_type) {
    case RDB_CHILD_TYPE_DISK:
        backgroundSaveDoneHandlerDisk(exitcode,bysignal);
        break;
    case RDB_CHILD_TYPE_SOCKET:
        backgroundSaveDoneHandlerSocket(exitcode,bysignal);
        break;
    default:
        serverPanic("Unknown RDB child type.");
        break;
    }

    server.rdb_child_pid = -1;
    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    server.rdb_save_time_last = time(NULL)-server.rdb_save_time_start;
    server.rdb_save_time_start = -1;
    /* Possibly there are slaves waiting for a BGSAVE in order to be served
     * (the first stage of SYNC is a bulk transfer of dump.rdb) */
    updateSlavesWaitingBgsave((!bysignal && exitcode == 0) ? C_OK : C_ERR, type);
}

/* Kill the RDB saving child using SIGUSR1 (so that the parent will know
 * the child did not exit for an error, but because we wanted), and performs
 * the cleanup needed. */
void killRDBChild(void) {
    kill(server.rdb_child_pid,SIGUSR1);
    rdbRemoveTempFile(server.rdb_child_pid, 0);
    closeChildInfoPipe();
    updateDictResizePolicy();
}

/* Spawn an RDB child that writes the RDB to the sockets of the slaves
 * that are currently in SLAVE_STATE_WAIT_BGSAVE_START state. */
// 直接写到socket中
// fork一个子进程将rdb写到 状态为等待BGSAVE开始的从节点的socket中
int rdbSaveToSlavesSockets(rdbSaveInfo *rsi) {
    listNode *ln;
    listIter li;
    pid_t childpid;
    int pipefds[2];

    if (hasActiveChildProcess()) return C_ERR;

    /* Even if the previous fork child exited, don't start a new one until we
     * drained the pipe. */
    if (server.rdb_pipe_conns) return C_ERR;

    /* Before to fork, create a pipe that is used to transfer the rdb bytes to
     * the parent, we can't let it write directly to the sockets, since in case
     * of TLS we must let the parent handle a continuous TLS state when the
     * child terminates and parent takes over. */
    if (pipe(pipefds) == -1) return C_ERR;
    server.rdb_pipe_read = pipefds[0];
    server.rdb_pipe_write = pipefds[1];
    anetNonBlock(NULL, server.rdb_pipe_read);

    /* Collect the connections of the replicas we want to transfer
     * the RDB to, which are i WAIT_BGSAVE_START state. */
    server.rdb_pipe_conns = zmalloc(sizeof(connection *)*listLength(server.slaves));
    server.rdb_pipe_numconns = 0;
    server.rdb_pipe_numconns_writing = 0;
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;
        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
            server.rdb_pipe_conns[server.rdb_pipe_numconns++] = slave->conn;
            replicationSetupSlaveForFullResync(slave,getPsyncInitialOffset());
        }
    }

    /* Create the child process. */
    openChildInfoPipe();
    if ((childpid = redisFork(CHILD_TYPE_RDB)) == 0) {
        /* Child */
        int retval;
        rio rdb;

        rioInitWithFd(&rdb,server.rdb_pipe_write);

        redisSetProcTitle("redis-rdb-to-slaves");
        redisSetCpuAffinity(server.bgsave_cpulist);

        retval = rdbSaveRioWithEOFMark(&rdb,NULL,rsi);
        if (retval == C_OK && rioFlush(&rdb) == 0)
            retval = C_ERR;

        if (retval == C_OK) {
            sendChildCOWInfo(CHILD_TYPE_RDB, "RDB");
        }

        rioFreeFd(&rdb);
        close(server.rdb_pipe_write); /* wake up the reader, tell it we're done. */
        exitFromChild((retval == C_OK) ? 0 : 1);
    } else {
        /* Parent */
        if (childpid == -1) {
            serverLog(LL_WARNING,"Can't save in background: fork: %s",
                strerror(errno));

            /* Undo the state change. The caller will perform cleanup on
             * all the slaves in BGSAVE_START state, but an early call to
             * replicationSetupSlaveForFullResync() turned it into BGSAVE_END */
            listRewind(server.slaves,&li);
            while((ln = listNext(&li))) {
                client *slave = ln->value;
                if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) {
                    slave->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
                }
            }
            close(server.rdb_pipe_write);
            close(server.rdb_pipe_read);
            zfree(server.rdb_pipe_conns);
            server.rdb_pipe_conns = NULL;
            server.rdb_pipe_numconns = 0;
            server.rdb_pipe_numconns_writing = 0;
            closeChildInfoPipe();
        } else {
            serverLog(LL_NOTICE,"Background RDB transfer started by pid %d",
                childpid);
            server.rdb_save_time_start = time(NULL);
            server.rdb_child_pid = childpid;
            server.rdb_child_type = RDB_CHILD_TYPE_SOCKET;
            close(server.rdb_pipe_write); /* close write in parent so that it can detect the close on the child. */
            if (aeCreateFileEvent(server.el, server.rdb_pipe_read, AE_READABLE, rdbPipeReadHandler,NULL) == AE_ERR) {
                serverPanic("Unrecoverable error creating server.rdb_pipe_read file event.");
            }
        }
        return (childpid == -1) ? C_ERR : C_OK;
    }
    return C_OK; /* Unreached. */
}

void saveCommand(client *c) {
    if (server.rdb_child_pid != -1) {
        addReplyError(c,"Background save already in progress");
        return;
    }
    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);
    if (rdbSave(server.rdb_filename,rsiptr) == C_OK) {
        addReply(c,shared.ok);
    } else {
        addReply(c,shared.err);
    }
}

/* BGSAVE [SCHEDULE] */
void bgsaveCommand(client *c) {
    int schedule = 0;

    /* The SCHEDULE option changes the behavior of BGSAVE when an AOF rewrite
     * is in progress. Instead of returning an error a BGSAVE gets scheduled. */
    if (c->argc > 1) {
        if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"schedule")) {
            schedule = 1;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);

    if (server.rdb_child_pid != -1) {
        addReplyError(c,"Background save already in progress");
    } else if (hasActiveChildProcess()) {
        if (schedule) {
            server.rdb_bgsave_scheduled = 1;
            addReplyStatus(c,"Background saving scheduled");
        } else {
            addReplyError(c,
            "Another child process is active (AOF?): can't BGSAVE right now. "
            "Use BGSAVE SCHEDULE in order to schedule a BGSAVE whenever "
            "possible.");
        }
    } else if (rdbSaveBackground(server.rdb_filename,rsiptr) == C_OK) {
        addReplyStatus(c,"Background saving started");
    } else {
        addReply(c,shared.err);
    }
}

/* Populate the rdbSaveInfo structure used to persist the replication
 * information inside the RDB file. Currently the structure explicitly
 * contains just the currently selected DB from the master stream, however
 * if the rdbSave*() family functions receive a NULL rsi structure also
 * the Replication ID/offset is not saved. The function popultes 'rsi'
 * that is normally stack-allocated in the caller, returns the populated
 * pointer if the instance has a valid master client, otherwise NULL
 * is returned, and the RDB saving will not persist any replication related
 * information. */
rdbSaveInfo *rdbPopulateSaveInfo(rdbSaveInfo *rsi) {
    rdbSaveInfo rsi_init = RDB_SAVE_INFO_INIT;
    *rsi = rsi_init;

    /* If the instance is a master, we can populate the replication info
     * only when repl_backlog is not NULL. If the repl_backlog is NULL,
     * it means that the instance isn't in any replication chains. In this
     * scenario the replication info is useless, because when a slave
     * connects to us, the NULL repl_backlog will trigger a full
     * synchronization, at the same time we will use a new replid and clear
     * replid2. */
    if (!server.masterhost && server.repl_backlog) {
        /* Note that when server.slaveseldb is -1, it means that this master
         * didn't apply any write commands after a full synchronization.
         * So we can let repl_stream_db be 0, this allows a restarted slave
         * to reload replication ID/offset, it's safe because the next write
         * command must generate a SELECT statement. */
        rsi->repl_stream_db = server.slaveseldb == -1 ? 0 : server.slaveseldb;
        return rsi;
    }

    /* If the instance is a slave we need a connected master
     * in order to fetch the currently selected DB. */
    if (server.master) {
        rsi->repl_stream_db = server.master->db->id;
        return rsi;
    }

    /* If we have a cached master we can use it in order to populate the
     * replication selected DB info inside the RDB file: the slave can
     * increment the master_repl_offset only from data arriving from the
     * master, so if we are disconnected the offset in the cached master
     * is valid. */
    if (server.cached_master) {
        rsi->repl_stream_db = server.cached_master->db->id;
        return rsi;
    }
    return NULL;
}
