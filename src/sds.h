/* SDSLib 2.0 -- A C dynamic strings library
 *
 * Copyright (c) 2006-2015, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2015, Oran Agra
 * Copyright (c) 2015, Redis Labs, Inc
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

#ifndef __SDS_H
#define __SDS_H

#define SDS_MAX_PREALLOC (1024*1024)
extern const char *SDS_NOINIT;

#include <sys/types.h>
#include <stdarg.h>
#include <stdint.h>

typedef char *sds;

/* Note: sdshdr5 is never used, we just access the flags byte directly.
 * However is here to document the layout of type 5 SDS strings. */
/* 注意: sdshdr5这种类型从未被使用, 我们仅仅直接访问它的flags。
 * 这里记录的是type为5的sds的布局。
 * __attribute__ ((__packed__))表示结构体字节对齐，这是GNU C特有的语法 */
struct __attribute__ ((__packed__)) sdshdr5 {
    unsigned char flags; /* 3 lsb of type, and 5 msb of string length */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr8 {
    uint8_t len; /* 已使用的字符串长度 */
    uint8_t alloc; /* 分配的内存空间大小，不包括头部和空终止符 */
    unsigned char flags; /* 3个最低有效位表示类型，5个最高有效位未使用 */
    char buf[];//字符数组
};
struct __attribute__ ((__packed__)) sdshdr16 {
    uint16_t len; /* used */
    uint16_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr32 {
    uint32_t len; /* used */
    uint32_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};
struct __attribute__ ((__packed__)) sdshdr64 {
    uint64_t len; /* used */
    uint64_t alloc; /* excluding the header and null terminator */
    unsigned char flags; /* 3 lsb of type, 5 unused bits */
    char buf[];
};

/* SDS类型值，一共5种类型 */
#define SDS_TYPE_5  0
#define SDS_TYPE_8  1
#define SDS_TYPE_16 2
#define SDS_TYPE_32 3
#define SDS_TYPE_64 4
#define SDS_TYPE_MASK 7  //sds类型掩码 0b00000111，因为flags中只有3个最低有效位表示类型
#define SDS_TYPE_BITS 3  //表示sds类型的比特位数，前面有提到：3个最低有效位表示类型
/* 从sds获取其header起始位置的指针，并声明一个sh变量赋值给它，获得方式是sds的地址减去头部大小 */
#define SDS_HDR_VAR(T,s) struct sdshdr##T *sh = (void*)((s)-(sizeof(struct sdshdr##T)));
/* 从sds获取其header起始位置的指针，作用和上面一个定义差不多，只不过不赋值给sh变量 */
#define SDS_HDR(T,s) ((struct sdshdr##T *)((s)-(sizeof(struct sdshdr##T))))
// 获取type为5的sds的长度，由于其flags的5个最高有效位表示字符串长度，所以直接把flags右移3位即是其字符串长度
#define SDS_TYPE_5_LEN(f) ((f)>>SDS_TYPE_BITS)

/* 获取一个sds的长度 */
static inline size_t sdslen(const sds s) {
    /* 通过sds字符指针获得header类型的方法是，先向低地址方向偏移1个字节的位置，得到flags字段，
      然后取flags的最低3个bit得到header的类型。 */
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {//操作：0b00000??? & 0b00000111，根据sds类型获取其字符串长度
        case SDS_TYPE_5:
            return SDS_TYPE_5_LEN(flags);
        case SDS_TYPE_8:
            return SDS_HDR(8,s)->len;
        case SDS_TYPE_16:
            return SDS_HDR(16,s)->len;
        case SDS_TYPE_32:
            return SDS_HDR(32,s)->len;
        case SDS_TYPE_64:
            return SDS_HDR(64,s)->len;
    }
    return 0;
}

/* 获取一个sds的空闲空间，计算方式是：已分配的空间 - 字符串长度大小。 */
static inline size_t sdsavail(const sds s) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {//同上，获取sds类型
        case SDS_TYPE_5: {//SDS_TYPE_5未被使用，直接返回0
            return 0;
        }
        case SDS_TYPE_8: {
            SDS_HDR_VAR(8,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_16: {
            SDS_HDR_VAR(16,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_32: {
            SDS_HDR_VAR(32,s);
            return sh->alloc - sh->len;
        }
        case SDS_TYPE_64: {
            SDS_HDR_VAR(64,s);
            return sh->alloc - sh->len;
        }
    }
    return 0;
}

/* 设置sds的字符串长度 */
static inline void sdssetlen(sds s, size_t newlen) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {//同上，获取sds类型
        case SDS_TYPE_5:
            {
                unsigned char *fp = ((unsigned char*)s)-1;//fp是sdshdr5的flags的指针
                *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);//把newlen右移SDS_TYPE_BITS位再和SDS_TYPE_5合成即可
            }
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->len = newlen;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->len = newlen;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->len = newlen;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->len = newlen;
            break;
    }
}

/* 增加sds的长度。 */
static inline void sdsinclen(sds s, size_t inc) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            {
                unsigned char *fp = ((unsigned char*)s)-1;  //fp是sdshdr5的flags的指针
                unsigned char newlen = SDS_TYPE_5_LEN(flags)+inc;  // 计算出newlen
                *fp = SDS_TYPE_5 | (newlen << SDS_TYPE_BITS);      // 同sdssetlen
            }
            break;
        case SDS_TYPE_8:
            SDS_HDR(8,s)->len += inc;//直接增加header中的len
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->len += inc;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->len += inc;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->len += inc;
            break;
    }
}

/* 获取sds容量，sdsalloc() = sdsavail() + sdslen()。 */
static inline size_t sdsalloc(const sds s) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            return SDS_TYPE_5_LEN(flags);
        case SDS_TYPE_8:
            return SDS_HDR(8,s)->alloc;
        case SDS_TYPE_16:
            return SDS_HDR(16,s)->alloc;
        case SDS_TYPE_32:
            return SDS_HDR(32,s)->alloc;
        case SDS_TYPE_64:
            return SDS_HDR(64,s)->alloc;
    }
    return 0;
}

/* 设置sds容量。 */
static inline void sdssetalloc(sds s, size_t newlen) {
    unsigned char flags = s[-1];
    switch(flags&SDS_TYPE_MASK) {
        case SDS_TYPE_5:
            /* Nothing to do, this type has no total allocation info. */
            break;
        case SDS_TYPE_8://其他type直接设置header的alloc属性
            SDS_HDR(8,s)->alloc = newlen;
            break;
        case SDS_TYPE_16:
            SDS_HDR(16,s)->alloc = newlen;
            break;
        case SDS_TYPE_32:
            SDS_HDR(32,s)->alloc = newlen;
            break;
        case SDS_TYPE_64:
            SDS_HDR(64,s)->alloc = newlen;
            break;
    }
}

sds sdsnewlen(const void *init, size_t initlen);//创建一个长度为initlen的sds
sds sdsnew(const char *init);//内部调用sdsnewlen，创建一个sds
sds sdsempty(void);//返回一个空的sds
sds sdsdup(const sds s);//拷贝一个sds并返回这个拷贝
void sdsfree(sds s);//释放一个sds
sds sdsgrowzero(sds s, size_t len);//使一个sds的长度增长到一个指定的值，末尾未使用的空间用0填充
sds sdscatlen(sds s, const void *t, size_t len);//连接一个sds和一个二进制安全的数据t，t的长度为len
sds sdscat(sds s, const char *t);//连接一个sds和一个二进制安全的数据t，内部调用sdscatlen
sds sdscatsds(sds s, const sds t);//连接两个sds
sds sdscpylen(sds s, const char *t, size_t len);// 把二进制安全的数据t复制到一个sds的内存中，覆盖原来的字符串，t的长度为len
sds sdscpy(sds s, const char *t);//把二进制安全的数据t复制到一个sds的内存中，覆盖原来的字符串，内部调用sdscpylen

/* 通过fmt指定个格式来格式化字符串 */
sds sdscatvprintf(sds s, const char *fmt, va_list ap);
#ifdef __GNUC__
sds sdscatprintf(sds s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
sds sdscatprintf(sds s, const char *fmt, ...);
#endif

sds sdscatfmt(sds s, char const *fmt, ...);//将格式化后的任意数量个字符串追加到s的末尾
sds sdstrim(sds s, const char *cset);//删除sds两端由cset指定的字符
void sdsrange(sds s, ssize_t start, ssize_t end);//通过区间[start, end]截取字符串
void sdsupdatelen(sds s);//根据字符串占用的空间来更新len
void sdsclear(sds s);//把字符串的第一个字符设置为'\0'，把字符串设置为空字符串，但是并不释放内存
int sdscmp(const sds s1, const sds s2);
sds *sdssplitlen(const char *s, ssize_t len, const char *sep, int seplen, int *count);//使用分隔符sep对s进行分割，返回一个sds数组
void sdsfreesplitres(sds *tokens, int count);//释放sds数组tokens中的count个sds
void sdstolower(sds s);//将sds所有字符转换为小写
void sdstoupper(sds s);//将sds所有字符转换为大写
sds sdsfromlonglong(long long value);//将长整型转换为字符串
sds sdscatrepr(sds s, const char *p, size_t len);//将长度为len的字符串p以带引号的格式追加到s的末尾
sds *sdssplitargs(const char *line, int *argc);//将一行文本分割成多个参数，参数的个数存在argc
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen);//将字符串s中，出现存在from中指定的字符，都转换成to中的字符，from与to有位置关系
sds sdsjoin(char **argv, int argc, char *sep);//使用分隔符sep将字符数组argv拼接成一个字符串
sds sdsjoinsds(sds *argv, int argc, const char *sep, size_t seplen);// 和sdsjoin类似，不过拼接的是一个sds数组

/* 暴露出来作为用户API的低级函数 */
sds sdsMakeRoomFor(sds s, size_t addlen);//为指定的sds扩充大小，扩充的大小为addlen
void sdsIncrLen(sds s, ssize_t incr);//根据incr增加或减少sds的字符串长度
sds sdsRemoveFreeSpace(sds s);  // 移除一个sds的空闲空间
size_t sdsAllocSize(sds s);  // 获取一个sds的总大小（包括header、字符串、末尾的空闲空间和隐式项目）
void *sdsAllocPtr(sds s);  // 获取一个sds确切的内存空间的指针（一般的sds引用都是一个指向其字符串的指针）

/* Export the allocator used by SDS to the program using SDS.
 * Sometimes the program SDS is linked to, may use a different set of
 * allocators, but may want to allocate or free things that SDS will
 * respectively free or allocate. */
/* 导出供外部程序调用的sds的分配/释放函数 */
void *sds_malloc(size_t size);  // sds分配器的包装函数，内部调用s_malloc
void *sds_realloc(void *ptr, size_t size);  // sds分配器的包装函数，内部调用s_realloc
void sds_free(void *ptr);  // sds释放器的包装函数，内部调用s_free

#ifdef REDIS_TEST
int sdsTest(int argc, char *argv[]);
#endif

#endif
