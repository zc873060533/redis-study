/* Redis Object implementation.
 *
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
#include <math.h>
#include <ctype.h>

#ifdef __CYGWIN__
#define strtold(a,b) ((long double)strtod((a),(b)))  //将a和b转换为long double类型
#endif

/* ===================== Creation and parsing of objects ==================== */

/** 创建一个默认的对象 */
robj *createObject(int type, void *ptr) {
    robj *o = zmalloc(sizeof(*o));      //分配空间
    o->type = type;                     //设置对象类型
    o->encoding = OBJ_ENCODING_RAW;     //设置默认的编码方式
    o->ptr = ptr;                       //设置
    o->refcount = 1;                    //引用计数为1

    /* Set the LRU to the current lruclock (minutes resolution), or
     * alternatively the LFU counter. */
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        o->lru = (LFUGetTimeInMinutes()<<8) | LFU_INIT_VAL;
    } else {
        o->lru = LRU_CLOCK();           //计算设置当前LRU时间
    }
    return o;
}

/* Set a special refcount in the object to make it "shared":
 * incrRefCount and decrRefCount() will test for this special refcount
 * and will not touch the object. This way it is free to access shared
 * objects such as small integers from different threads without any
 * mutex.
 *
 * A common patter to create shared objects:
 *
 * robj *myobject = makeObjectShared(createObject(...));
 *
 */
/**
 * 在对象中设置一个特殊的引用计数以使其“共享”：
 * incrRefCount和decrRefCount（）将测试该特殊的引用计数，并且不会接触该对象。
 * 这样，就可以自由访问共享对象，例如来自不同线程的小整数而无需任何互斥体。
 * */
robj *makeObjectShared(robj *o) {
    serverAssert(o->refcount == 1);
    o->refcount = OBJ_SHARED_REFCOUNT;
    return o;
}

/* Create a string object with encoding OBJ_ENCODING_RAW, that is a plain
 * string object where o->ptr points to a proper sds string. */
/** 创建一个字符串对象，编码默认为OBJ_ENCODING_RAW，指向的数据为一个sds */
robj *createRawStringObject(const char *ptr, size_t len) {
    return createObject(OBJ_STRING, sdsnewlen(ptr,len));
}

/* Create a string object with encoding OBJ_ENCODING_EMBSTR, that is
 * an object where the sds string is actually an unmodifiable string
 * allocated in the same chunk as the object itself. */
/**
 * 创建一个embstr编码的字符串对象
 * embstr类型的字符串限定在64byte，- object结构体需要占用16byte，-sdshdr8结构体需要3byte，-末尾的"\0"1byte.=44
 * */
robj *createEmbeddedStringObject(const char *ptr, size_t len) {
    robj *o = zmalloc(sizeof(robj)+sizeof(struct sdshdr8)+len+1);   //分配内存空间 把对象头和SDS 连续存在一起，另外长度44就是这里算出来的
    struct sdshdr8 *sh = (void*)(o+1);          //内存连续，所以这里的1要理解为sizeof(robj)，就是指针从头指向sdshdr

    o->type = OBJ_STRING;                       //类型为字符串对象
    o->encoding = OBJ_ENCODING_EMBSTR;          //设置编码类型OBJ_ENCODING_EMBSTR
    o->ptr = sh+1;                              //sh+1 实际上是 sh + sizeof(struct sdshdr8),指向buf
    o->refcount = 1;                            //设置引用计数
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        o->lru = (LFUGetTimeInMinutes()<<8) | LFU_INIT_VAL;
    } else {
        o->lru = LRU_CLOCK();                   //计算设置当前LRU时间
    }

    sh->len = len;                              //设置字符串长度
    sh->alloc = len;                            //设置最大容量
    sh->flags = SDS_TYPE_8;                     //设置sds的类型
    if (ptr == SDS_NOINIT)
        sh->buf[len] = '\0';
    else if (ptr) {                             //如果传了字符串参数
        memcpy(sh->buf,ptr,len);                //将传进来的ptr保存到对象中
        sh->buf[len] = '\0';                    //结束符标志
    } else {
        memset(sh->buf,0,len+1);                //否则将对象的空间初始化为0
    }
    return o;
}

/* Create a string object with EMBSTR encoding if it is smaller than
 * OBJ_ENCODING_EMBSTR_SIZE_LIMIT, otherwise the RAW encoding is
 * used.
 *
 * The current limit of 44 is chosen so that the biggest string object
 * we allocate as EMBSTR will still fit into the 64 byte arena of jemalloc. */
/**
 * sdshdr8的大小为3个字节，加上1个结束符共4个字节
 * redisObject的大小为16个字节
 * redis使用jemalloc内存分配器，且jemalloc会分配8，16，32，64等字节的内存
 * 一个embstr固定的大小为16+3+1 = 20个字节，因此一个最大的embstr字符串为64-20 = 44字节
 * */
#define OBJ_ENCODING_EMBSTR_SIZE_LIMIT 44
/**
 * 创建字符串对象，根据长度使用不同的编码类型
 * createRawStringObject和createEmbeddedStringObject的区别是：
 * createRawStringObject是当字符串长度大于44字节时，robj结构和sdshdr结构在内存上是分开的
 * createEmbeddedStringObject是当字符串长度小于等于44字节时，robj结构和sdshdr结构在内存上是连续的
 * */
robj *createStringObject(const char *ptr, size_t len) {
    if (len <= OBJ_ENCODING_EMBSTR_SIZE_LIMIT)
        return createEmbeddedStringObject(ptr,len);
    else
        return createRawStringObject(ptr,len);
}

/* Create a string object from a long long value. When possible returns a
 * shared integer object, or at least an integer encoded one.
 *
 * If valueobj is non zero, the function avoids returning a shared
 * integer, because the object is going to be used as value in the Redis key
 * space (for instance when the INCR command is used), so we want LFU/LRU
 * values specific for each key. */
/** 创建字符串对象，根据整数值 */
robj *createStringObjectFromLongLongWithOptions(long long value, int valueobj) {
    robj *o;

    if (server.maxmemory == 0 ||
        !(server.maxmemory_policy & MAXMEMORY_FLAG_NO_SHARED_INTEGERS))
    {
        /* If the maxmemory policy permits, we can still return shared integers
         * even if valueobj is true. */
        valueobj = 0;
    }

    //redis中[0, 10000)内的整数是共享的
    if (value >= 0 && value < OBJ_SHARED_INTEGERS && valueobj == 0) {//如果value属于redis共享整数的范围
        incrRefCount(shared.integers[value]);           //引用计数加1
        o = shared.integers[value];                     //返回一个编码类型为OBJ_ENCODING_INT的字符串对象

        //如果不在共享整数的范围
    } else {
        if (value >= LONG_MIN && value <= LONG_MAX) {               //value在long类型所表示的范围内
            o = createObject(OBJ_STRING, NULL);           //创建对象
            o->encoding = OBJ_ENCODING_INT;                         //编码类型为OBJ_ENCODING_INT
            o->ptr = (void*)((long)value);                          //指向这个value值
        } else {
            //value不在long类型所表示的范围内，将long long类型的整数转换为字符串
            //编码类型为OBJ_ENCODING_RAW
            o = createObject(OBJ_STRING,sdsfromlonglong(value));
        }
    }
    return o;
}

/* Wrapper for createStringObjectFromLongLongWithOptions() always demanding
 * to create a shared object if possible. */
robj *createStringObjectFromLongLong(long long value) {
    return createStringObjectFromLongLongWithOptions(value,0);
}

/* Wrapper for createStringObjectFromLongLongWithOptions() avoiding a shared
 * object when LFU/LRU info are needed, that is, when the object is used
 * as a value in the key space, and Redis is configured to evict based on
 * LFU/LRU. */
robj *createStringObjectFromLongLongForValue(long long value) {
    return createStringObjectFromLongLongWithOptions(value,1);
}

/* Create a string object from a long double. If humanfriendly is non-zero
 * it does not use exponential format and trims trailing zeroes at the end,
 * however this results in loss of precision. Otherwise exp format is used
 * and the output of snprintf() is not modified.
 *
 * The 'humanfriendly' option is used for INCRBYFLOAT and HINCRBYFLOAT. */
/** 将long double类型的value创建一个字符串对象 */
robj *createStringObjectFromLongDouble(long double value, int humanfriendly) {
    char buf[MAX_LONG_DOUBLE_CHARS];
    int len = ld2string(buf,sizeof(buf),value,humanfriendly? LD_STR_HUMAN: LD_STR_AUTO);    //将long double转化为buffer，并返回转化长度
    return createStringObject(buf,len);
}

/* Duplicate a string object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * This function also guarantees that duplicating a small integer object
 * (or a string object that contains a representation of a small integer)
 * will always result in a fresh object that is unshared (refcount == 1).
 *
 * The resulting object always has refcount set to 1. */
/** 返回 复制的o对象的副本的地址，且创建的对象非共享 */
robj *dupStringObject(const robj *o) {
    robj *d;

    serverAssert(o->type == OBJ_STRING);    //一定是OBJ_STRING类型

    switch(o->encoding) {                       //根据不同的编码类型
    case OBJ_ENCODING_RAW:
        return createRawStringObject(o->ptr,sdslen(o->ptr));        //创建的对象非共享
    case OBJ_ENCODING_EMBSTR:
        return createEmbeddedStringObject(o->ptr,sdslen(o->ptr));   //创建的对象非共享
    case OBJ_ENCODING_INT:                      //整数编码类型
        d = createObject(OBJ_STRING, NULL); //即使是共享整数范围内的整数，创建的对象也是非共享的
        d->encoding = OBJ_ENCODING_INT;
        d->ptr = o->ptr;
        return d;
    default:
        serverPanic("Wrong encoding.");
        break;
    }
}

/** 创建一个quicklist编码的列表对象 */
robj *createQuicklistObject(void) {
    quicklist *l = quicklistCreate();       //创建一个quicklist
    robj *o = createObject(OBJ_LIST,l);     //创建一个对象，对象的数据类型为OBJ_LIST
    o->encoding = OBJ_ENCODING_QUICKLIST;   //对象的编码类型OBJ_ENCODING_QUICKLIST
    return o;
}

/** 创建一个ziplist编码的列表对象 */
robj *createZiplistObject(void) {
    unsigned char *zl = ziplistNew();       //创建一个ziplist
    robj *o = createObject(OBJ_LIST,zl);    //创建一个对象，对象的数据类型为OBJ_LIST
    o->encoding = OBJ_ENCODING_ZIPLIST;     //对象的编码类型OBJ_ENCODING_ZIPLIST
    return o;
}

/** 创建一个ht编码的集合对象 */
robj *createSetObject(void) {
    dict *d = dictCreate(&setDictType,NULL);     //创建一个字典
    robj *o = createObject(OBJ_SET,d);                      //创建一个对象，对象的数据类型为OBJ_SET
    o->encoding = OBJ_ENCODING_HT;                          //对象的编码类型OBJ_ENCODING_HT
    return o;
}

/** 创建一个intset编码的集合对象 */
robj *createIntsetObject(void) {
    intset *is = intsetNew();               //创建一个整数集合
    robj *o = createObject(OBJ_SET,is);     //创建一个对象，对象的数据类型为OBJ_SET
    o->encoding = OBJ_ENCODING_INTSET;      //对象的编码类型OBJ_ENCODING_INTSET
    return o;
}

/** 创建一个ziplist编码的哈希对象 */
robj *createHashObject(void) {
    unsigned char *zl = ziplistNew();       //创建一个ziplist
    robj *o = createObject(OBJ_HASH, zl);   //创建一个对象，对象的数据类型为OBJ_HASH
    o->encoding = OBJ_ENCODING_ZIPLIST;     //对象的编码类型OBJ_ENCODING_ZIPLIST
    return o;
}

/** 创建一个skiplist编码的有序集合对象 */
robj *createZsetObject(void) {
    zset *zs = zmalloc(sizeof(*zs));
    robj *o;

    zs->dict = dictCreate(&zsetDictType,NULL);  //创建一个字典
    zs->zsl = zslCreate();                                 //创建一个跳跃表
    o = createObject(OBJ_ZSET,zs);                         //创建一个对象，对象的数据类型为OBJ_ZSET
    o->encoding = OBJ_ENCODING_SKIPLIST;                   //对象的编码类型OBJ_ENCODING_SKIPLIST
    return o;
}

/** 创建一个ziplist编码的有序集合对象 */
robj *createZsetZiplistObject(void) {
    unsigned char *zl = ziplistNew();       //创建一个ziplist
    robj *o = createObject(OBJ_ZSET,zl);    //创建一个对象，对象的数据类型为OBJ_ZSET
    o->encoding = OBJ_ENCODING_ZIPLIST;     //对象的编码类型OBJ_ENCODING_ZIPLIST
    return o;
}

/** 创建一个Stream对象 */
robj *createStreamObject(void) {
    stream *s = streamNew();                //创建一个Stream
    robj *o = createObject(OBJ_STREAM,s);   //创建一个对象，对象的数据类型为OBJ_STREAM
    o->encoding = OBJ_ENCODING_STREAM;      //对象的编码类型OBJ_ENCODING_STREAM
    return o;
}

/** 创建一个module对象 */
robj *createModuleObject(moduleType *mt, void *value) {
    moduleValue *mv = zmalloc(sizeof(*mv));
    mv->type = mt;
    mv->value = value;
    return createObject(OBJ_MODULE,mv);
}

/** 释放字符串对象ptr指向的对象 */
void freeStringObject(robj *o) {
    if (o->encoding == OBJ_ENCODING_RAW) {
        sdsfree(o->ptr);
    }
}

/** 释放列表对象ptr指向的对象 */
void freeListObject(robj *o) {
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistRelease(o->ptr);
    } else {
        serverPanic("Unknown list encoding type");
    }
}

/** 释放集合对象ptr指向的对象 */
void freeSetObject(robj *o) {
    switch (o->encoding) {
    case OBJ_ENCODING_HT:
        dictRelease((dict*) o->ptr);
        break;
    case OBJ_ENCODING_INTSET:
        zfree(o->ptr);
        break;
    default:
        serverPanic("Unknown set encoding type");
    }
}

/** 释放有序集合对象ptr指向的对象 */
void freeZsetObject(robj *o) {
    zset *zs;
    switch (o->encoding) {
    case OBJ_ENCODING_SKIPLIST:
        zs = o->ptr;
        dictRelease(zs->dict);
        zslFree(zs->zsl);
        zfree(zs);
        break;
    case OBJ_ENCODING_ZIPLIST:
        zfree(o->ptr);
        break;
    default:
        serverPanic("Unknown sorted set encoding");
    }
}

/** 释放哈希对象ptr指向的对象 */
void freeHashObject(robj *o) {
    switch (o->encoding) {
    case OBJ_ENCODING_HT:
        dictRelease((dict*) o->ptr);
        break;
    case OBJ_ENCODING_ZIPLIST:
        zfree(o->ptr);
        break;
    default:
        serverPanic("Unknown hash encoding type");
        break;
    }
}

void freeModuleObject(robj *o) {
    moduleValue *mv = o->ptr;
    mv->type->free(mv->value);
    zfree(mv);
}

void freeStreamObject(robj *o) {
    freeStream(o->ptr);
}

/** 引用计数加1 */
void incrRefCount(robj *o) {
    if (o->refcount < OBJ_FIRST_SPECIAL_REFCOUNT) {
        o->refcount++;
    } else {
        if (o->refcount == OBJ_SHARED_REFCOUNT) {
            /* Nothing to do: this refcount is immutable. */
        } else if (o->refcount == OBJ_STATIC_REFCOUNT) {
            serverPanic("You tried to retain an object allocated in the stack");
        }
    }
}

/** 引用计数减1 */
void decrRefCount(robj *o) {
    //当引用对象等于1时，在操作引用计数减1，直接释放对象的ptr和对象空间
    if (o->refcount == 1) {
        switch(o->type) {
        case OBJ_STRING: freeStringObject(o); break;
        case OBJ_LIST: freeListObject(o); break;
        case OBJ_SET: freeSetObject(o); break;
        case OBJ_ZSET: freeZsetObject(o); break;
        case OBJ_HASH: freeHashObject(o); break;
        case OBJ_MODULE: freeModuleObject(o); break;
        case OBJ_STREAM: freeStreamObject(o); break;
        default: serverPanic("Unknown object type"); break;
        }
        zfree(o);
    } else {
        if (o->refcount <= 0) serverPanic("decrRefCount against refcount <= 0");
        if (o->refcount != OBJ_SHARED_REFCOUNT) o->refcount--;  //否则减1
    }
}

/* This variant of decrRefCount() gets its argument as void, and is useful
 * as free method in data structures that expect a 'void free_object(void*)'
 * prototype for the free method. */
/** 释放特定的数据类型的封装 */
void decrRefCountVoid(void *o) {
    decrRefCount(o);
}

/* This function set the ref count to zero without freeing the object.
 * It is useful in order to pass a new object to functions incrementing
 * the ref count of the received object. Example:
 *
 *    functionThatWillIncrementRefCount(resetRefCount(CreateObject(...)));
 *
 * Otherwise you need to resort to the less elegant pattern:
 *
 *    *obj = createObject(...);
 *    functionThatWillIncrementRefCount(obj);
 *    decrRefCount(obj);
 */
/** 重置obj对象的引用计数为0，用于上面注释中的情况 */
robj *resetRefCount(robj *obj) {
    obj->refcount = 0;
    return obj;
}

/** 检查对象o的对象类型是否是type，是返回0，不是返回1，并向client发送一个错误 */
int checkType(client *c, robj *o, int type) {
    if (o->type != type) {
        addReply(c,shared.wrongtypeerr);
        return 1;
    }
    return 0;
}

/** 判断对象的ptr指向的值能否转换为long long类型，如果可以保存在llval中 */
int isSdsRepresentableAsLongLong(sds s, long long *llval) {
    return string2ll(s,sdslen(s),llval) ? C_OK : C_ERR;
}

int isObjectRepresentableAsLongLong(robj *o, long long *llval) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
    if (o->encoding == OBJ_ENCODING_INT) {
        if (llval) *llval = (long) o->ptr;
        return C_OK;
    } else {
        return isSdsRepresentableAsLongLong(o->ptr,llval);
    }
}

/* Optimize the SDS string inside the string object to require little space,
 * in case there is more than 10% of free space at the end of the SDS
 * string. This happens because SDS strings tend to overallocate to avoid
 * wasting too much time in allocations when appending to the string. */
/** 释放多余空间 */
void trimStringObjectIfNeeded(robj *o) {
    if (o->encoding == OBJ_ENCODING_RAW &&
        sdsavail(o->ptr) > sdslen(o->ptr)/10)
    {   //无法进行编码，但是如果s的未使用的空间大于使用空间的10分之1
        o->ptr = sdsRemoveFreeSpace(o->ptr);    //释放所有的未使用空间
    }
}

/* Try to encode a string object in order to save space */
/** 尝试对字符串对象进行编码以节省空间 */
robj *tryObjectEncoding(robj *o) {
    long value;
    sds s = o->ptr;
    size_t len;

    /* Make sure this is a string object, the only type we encode
     * in this function. Other types use encoded memory efficient
     * representations but are handled by the commands implementing
     * the type. */
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);

    /* We try some specialized encoding only for objects that are
     * RAW or EMBSTR encoded, in other words objects that are still
     * in represented by an actually array of chars. */
    //如果字符串对象的编码类型为RAW或EMBSTR时，才对其重新编码
    if (!sdsEncodedObject(o)) return o;

    /* It's not safe to encode shared objects: shared objects can be shared
     * everywhere in the "object space" of Redis and may end in places where
     * they are not handled. We handle them only as values in the keyspace. */
    //如果refcount大于1，则说明对象的ptr指向的值是共享的，不对共享对象进行编码
    if (o->refcount > 1) return o;

    /* Check if we can represent this string as a long integer.
     * Note that we are sure that a string larger than 20 chars is not
     * representable as a 32 nor 64 bit integer. */
    len = sdslen(s);    //获得字符串s的长度
    //如果len小于等于20，表示符合long long可以表示的范围，且可以转换为long类型的字符串进行编码
    if (len <= 20 && string2l(s,len,&value)) {
        /* This object is encodable as a long. Try to use a shared object.
         * Note that we avoid using shared integers when maxmemory is used
         * because every object needs to have a private LRU field for the LRU
         * algorithm to work well. */
        if ((server.maxmemory == 0 ||
            !(server.maxmemory_policy & MAXMEMORY_FLAG_NO_SHARED_INTEGERS)) &&
            value >= 0 &&
            value < OBJ_SHARED_INTEGERS)    //如果value处于共享整数的范围内
        {
            decrRefCount(o);                //原对象的引用计数减1，释放对象
            incrRefCount(shared.integers[value]);   //增加共享对象的引用计数
            return shared.integers[value];  //返回一个编码为整数的字符串对象
        } else {    //如果不处于共享整数的范围
            if (o->encoding == OBJ_ENCODING_RAW) {
                sdsfree(o->ptr);                //释放编码为OBJ_ENCODING_RAW的对象
                o->encoding = OBJ_ENCODING_INT; //转换为OBJ_ENCODING_INT编码
                o->ptr = (void*) value;         //指针ptr指向value对象
                return o;
            } else if (o->encoding == OBJ_ENCODING_EMBSTR) {
                decrRefCount(o);
                return createStringObjectFromLongLongForValue(value);
            }
        }
    }

    /* If the string is small and is still RAW encoded,
     * try the EMBSTR encoding which is more efficient.
     * In this representation the object and the SDS string are allocated
     * in the same chunk of memory to save space and cache misses. */
    //如果len小于44，44是最大的编码为EMBSTR类型的字符串对象长度
    if (len <= OBJ_ENCODING_EMBSTR_SIZE_LIMIT) {
        robj *emb;

        if (o->encoding == OBJ_ENCODING_EMBSTR) return o;//将RAW对象转换为OBJ_ENCODING_EMBSTR编码类型
        emb = createEmbeddedStringObject(s,sdslen(s)); //创建一个编码类型为OBJ_ENCODING_EMBSTR的字符串对象
        decrRefCount(o);    //释放之前的对象
        return emb;
    }

    /* We can't encode the object...
     *
     * Do the last try, and at least optimize the SDS string inside
     * the string object to require little space, in case there
     * is more than 10% of free space at the end of the SDS string.
     *
     * We do that only for relatively large strings as this branch
     * is only entered if the length of the string is greater than
     * OBJ_ENCODING_EMBSTR_SIZE_LIMIT. */
    trimStringObjectIfNeeded(o);

    /* Return the original object. */
    return o;
}

/**
 * 获取编码对象的解码版本（作为新对象返回）。
 * 如果该对象已经过原始编码，则只需增加引用计数即可。
 * */
robj *getDecodedObject(robj *o) {
    robj *dec;

    if (sdsEncodedObject(o)) {  //如果是OBJ_ENCODING_RAW或OBJ_ENCODING_EMBSTR类型的对象
        incrRefCount(o);        //增加应用计数
        return o;
    }
    if (o->type == OBJ_STRING && o->encoding == OBJ_ENCODING_INT) { //如果是整数对象
        char buf[32];

        ll2string(buf,32,(long)o->ptr); //将整数转换为字符串
        dec = createStringObject(buf,strlen(buf));  //创建一个字符串对象
        return dec;
    } else {
        serverPanic("Unknown encoding type");
    }
}

/* Compare two string objects via strcmp() or strcoll() depending on flags.
 * Note that the objects may be integer-encoded. In such a case we
 * use ll2string() to get a string representation of the numbers on the stack
 * and compare the strings, it's much faster than calling getDecodedObject().
 *
 * Important note: when REDIS_COMPARE_BINARY is used a binary-safe comparison
 * is used. */

#define REDIS_COMPARE_BINARY (1<<0) //以二进制方式进行比较
#define REDIS_COMPARE_COLL (1<<1)   //以本地指定的字符次序进行比较

/** 根据flags比较两个字符串对象a和b，返回0表示相等，非零表示不相等 */
int compareStringObjectsWithFlags(robj *a, robj *b, int flags) {
    serverAssertWithInfo(NULL,a,a->type == OBJ_STRING && b->type == OBJ_STRING);
    char bufa[128], bufb[128], *astr, *bstr;
    size_t alen, blen, minlen;

    if (a == b) return 0;    //如果是同一对象直接返回

    //如果是指向字符串值的两种OBJ_ENCODING_EMBSTR或OBJ_ENCODING_RAW的两类对象
    if (sdsEncodedObject(a)) {
        astr = a->ptr;
        alen = sdslen(astr);//获取字符串的长度
    } else {    //如果是整数类型的OBJ_ENCODING_INT编码
        alen = ll2string(bufa,sizeof(bufa),(long) a->ptr);
        astr = bufa;
    }

    //如果是指向字符串值的两种OBJ_ENCODING_EMBSTR或OBJ_ENCODING_RAW的两类对象
    if (sdsEncodedObject(b)) {
        bstr = b->ptr;
        blen = sdslen(bstr);    //获取字符串的长度
    } else {    //如果是整数类型的OBJ_ENCODING_INT编码
        blen = ll2string(bufb,sizeof(bufb),(long) b->ptr);  //转换为字符串
        bstr = bufb;
    }

    //以本地指定的字符次序进行比较
    if (flags & REDIS_COMPARE_COLL) {
        //strcoll()会依环境变量LC_COLLATE所指定的文字排列次序来比较两字符串
        return strcoll(astr,bstr);  //比较a和b的字符串对象，相等返回0
    } else {    //以二进制方式进行比较
        int cmp;

        minlen = (alen < blen) ? alen : blen;
        cmp = memcmp(astr,bstr,minlen);         //相等返回0，否则返回第一个字符串和第二个字符串的长度差
        if (cmp == 0) return alen-blen;
        return cmp;
    }

}

/* Wrapper for compareStringObjectsWithFlags() using binary comparison. */
/** 以二进制安全的方式进行对象的比较 */
int compareStringObjects(robj *a, robj *b) {
    return compareStringObjectsWithFlags(a,b,REDIS_COMPARE_BINARY);
}

/* Wrapper for compareStringObjectsWithFlags() using collation. */
/** 以制定的字符次序进行两个对象的比较 */
int collateStringObjects(robj *a, robj *b) {
    return compareStringObjectsWithFlags(a,b,REDIS_COMPARE_COLL);
}

/* Equal string objects return 1 if the two objects are the same from the
 * point of view of a string comparison, otherwise 0 is returned. Note that
 * this function is faster then checking for (compareStringObject(a,b) == 0)
 * because it can perform some more optimization. */
/** 比较两个字符串对象，以二进制安全的方式进行比较 */
int equalStringObjects(robj *a, robj *b) {
    if (a->encoding == OBJ_ENCODING_INT &&
        b->encoding == OBJ_ENCODING_INT){       //如果两个对象的编码都是整型INT
        /* If both strings are integer encoded just check if the stored
         * long is the same. */
        return a->ptr == b->ptr;                //直接比较数值大小
    } else {
        return compareStringObjects(a,b) == 0;  //否则进行二进制安全比较字符串
    }
}

/** 返回字符串对象的字符串长度 */
size_t stringObjectLen(robj *o) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
    //如果是字符串编码的两种类型
    if (sdsEncodedObject(o)) {
        return sdslen(o->ptr);  //如果是整数编码类型
    } else {
        return sdigits10((long)o->ptr); //计算出整数值的位数返回
    }
}

/** 从对象中将字符串值转换为double并存储在target中 */
int getDoubleFromObject(const robj *o, double *target) {
    double value;

    if (o == NULL) {    //对象不存在
        value = 0;
    } else {
        serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
        //如果是字符串编码的两种类型
        if (sdsEncodedObject(o)) {
            if (!string2d(o->ptr, sdslen(o->ptr), &value))
                return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) {//整数编码
            value = (long)o->ptr;                       //保存整数值
        } else {
            serverPanic("Unknown string encoding");
        }
    }
    *target = value;
    return C_OK;
}

/** 从对象中将字符串值转换为double并存储在target中，若失败，失败发送信息给client */
int getDoubleFromObjectOrReply(client *c, robj *o, double *target, const char *msg) {
    double value;
    if (getDoubleFromObject(o, &value) != C_OK) {//如果出错
        if (msg != NULL) {                  //msg不为空
            addReplyError(c,(char*)msg);    //发送指定的msg给client
        } else {
            addReplyError(c,"value is not a valid float");//发送普通字符串
        }
        return C_ERR;
    }
    *target = value;     //将转换成功的值存到传入参数中，返回0成功
    return C_OK;
}

/** 从对象中将字符串值转换为long double并存储在target中 */
int getLongDoubleFromObject(robj *o, long double *target) {
    long double value;

    if (o == NULL) {     //对象不存在
        value = 0;
    } else {
        serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
        //如果是字符串编码的两种类型
        if (sdsEncodedObject(o)) {
            if (!string2ld(o->ptr, sdslen(o->ptr), &value))
                return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) {
            value = (long)o->ptr;
        } else {
            serverPanic("Unknown string encoding");
        }
    }
    *target = value;
    return C_OK;
}

/** 从对象中将字符串值转换为long double并存储在target中，若失败，失败发送信息给client */
int getLongDoubleFromObjectOrReply(client *c, robj *o, long double *target, const char *msg) {
    long double value;
    if (getLongDoubleFromObject(o, &value) != C_OK) {//如果出错
        if (msg != NULL) {                  //msg不为空
            addReplyError(c,(char*)msg);    //发送指定的msg给client
        } else {
            addReplyError(c,"value is not a valid float");  //发送普通字符串
        }
        return C_ERR;
    }
    *target = value;//将转换成功的值存到传入参数中，返回0成功
    return C_OK;
}

/**
 * 从对象中将字符串值转换为long long并存储在target中
 * */
int getLongLongFromObject(robj *o, long long *target) {
    long long value;

    if (o == NULL) {
        value = 0;
    } else {
        serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
        //如果是字符串编码的两种类型
        if (sdsEncodedObject(o)) {
            if (string2ll(o->ptr,sdslen(o->ptr),&value) == 0) return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) {
            value = (long)o->ptr;
        } else {
            serverPanic("Unknown string encoding");
        }
    }
    if (target) *target = value;
    return C_OK;
}

/**
 * /从对象中将字符串值转换为long long并存储在target中，若失败，失败发送信息给client
 * */
int getLongLongFromObjectOrReply(client *c, robj *o, long long *target, const char *msg) {
    long long value;
    if (getLongLongFromObject(o, &value) != C_OK) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is not an integer or out of range");
        }
        return C_ERR;
    }
    *target = value;
    return C_OK;
}

/** 从对象中将字符串值转换为long并存储在target中，若失败，失败发送信息给client */
int getLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg) {
    long long value;

    //如果将字符串值转换为long long出错，发送错误信息msg，返回-1，否则保存整数值到value中
    if (getLongLongFromObjectOrReply(c, o, &value, msg) != C_OK) return C_ERR;
    if (value < LONG_MIN || value > LONG_MAX) {
        if (msg != NULL) {
            addReplyError(c,(char*)msg);
        } else {
            addReplyError(c,"value is out of range");
        }
        return C_ERR;
    }
    *target = value;//否则将long大小的value返回到传入参数
    return C_OK;
}

/** 返回编码类型的字符串表示 */
char *strEncoding(int encoding) {
    switch(encoding) {
    case OBJ_ENCODING_RAW: return "raw";
    case OBJ_ENCODING_INT: return "int";
    case OBJ_ENCODING_HT: return "hashtable";
    case OBJ_ENCODING_QUICKLIST: return "quicklist";
    case OBJ_ENCODING_ZIPLIST: return "ziplist";
    case OBJ_ENCODING_INTSET: return "intset";
    case OBJ_ENCODING_SKIPLIST: return "skiplist";
    case OBJ_ENCODING_EMBSTR: return "embstr";
    case OBJ_ENCODING_STREAM: return "stream";
    default: return "unknown";
    }
}

/* =========================== Memory introspection ========================= */


/* This is an helper function with the goal of estimating the memory
 * size of a radix tree that is used to store Stream IDs.
 *
 * Note: to guess the size of the radix tree is not trivial, so we
 * approximate it considering 16 bytes of data overhead for each
 * key (the ID), and then adding the number of bare nodes, plus some
 * overhead due by the data and child pointers. This secret recipe
 * was obtained by checking the average radix tree created by real
 * workloads, and then adjusting the constants to get numbers that
 * more or less match the real memory usage.
 *
 * Actually the number of nodes and keys may be different depending
 * on the insertion speed and thus the ability of the radix tree
 * to compress prefixes. */
size_t streamRadixTreeMemoryUsage(rax *rax) {
    size_t size;
    size = rax->numele * sizeof(streamID);
    size += rax->numnodes * sizeof(raxNode);
    /* Add a fixed overhead due to the aux data pointer, children, ... */
    size += rax->numnodes * sizeof(long)*30;
    return size;
}

/* Returns the size in bytes consumed by the key's value in RAM.
 * Note that the returned value is just an approximation, especially in the
 * case of aggregated data types where only "sample_size" elements
 * are checked and averaged to estimate the total size. */
#define OBJ_COMPUTE_SIZE_DEF_SAMPLES 5 /* Default sample size. */
size_t objectComputeSize(robj *o, size_t sample_size) {
    sds ele, ele2;
    dict *d;
    dictIterator *di;
    struct dictEntry *de;
    size_t asize = 0, elesize = 0, samples = 0;

    if (o->type == OBJ_STRING) {
        if(o->encoding == OBJ_ENCODING_INT) {
            asize = sizeof(*o);
        } else if(o->encoding == OBJ_ENCODING_RAW) {
            asize = sdsZmallocSize(o->ptr)+sizeof(*o);
        } else if(o->encoding == OBJ_ENCODING_EMBSTR) {
            asize = sdslen(o->ptr)+2+sizeof(*o);
        } else {
            serverPanic("Unknown string encoding");
        }
    } else if (o->type == OBJ_LIST) {
        if (o->encoding == OBJ_ENCODING_QUICKLIST) {
            quicklist *ql = o->ptr;
            quicklistNode *node = ql->head;
            asize = sizeof(*o)+sizeof(quicklist);
            do {
                elesize += sizeof(quicklistNode)+ziplistBlobLen(node->zl);
                samples++;
            } while ((node = node->next) && samples < sample_size);
            asize += (double)elesize/samples*ql->len;
        } else if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            asize = sizeof(*o)+ziplistBlobLen(o->ptr);
        } else {
            serverPanic("Unknown list encoding");
        }
    } else if (o->type == OBJ_SET) {
        if (o->encoding == OBJ_ENCODING_HT) {
            d = o->ptr;
            di = dictGetIterator(d);
            asize = sizeof(*o)+sizeof(dict)+(sizeof(struct dictEntry*)*dictSlots(d));
            while((de = dictNext(di)) != NULL && samples < sample_size) {
                ele = dictGetKey(de);
                elesize += sizeof(struct dictEntry) + sdsZmallocSize(ele);
                samples++;
            }
            dictReleaseIterator(di);
            if (samples) asize += (double)elesize/samples*dictSize(d);
        } else if (o->encoding == OBJ_ENCODING_INTSET) {
            intset *is = o->ptr;
            asize = sizeof(*o)+sizeof(*is)+is->encoding*is->length;
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (o->type == OBJ_ZSET) {
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            asize = sizeof(*o)+(ziplistBlobLen(o->ptr));
        } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
            d = ((zset*)o->ptr)->dict;
            zskiplist *zsl = ((zset*)o->ptr)->zsl;
            zskiplistNode *znode = zsl->header->level[0].forward;
            asize = sizeof(*o)+sizeof(zset)+sizeof(zskiplist)+sizeof(dict)+
                    (sizeof(struct dictEntry*)*dictSlots(d))+
                    zmalloc_size(zsl->header);
            while(znode != NULL && samples < sample_size) {
                elesize += sdsZmallocSize(znode->ele);
                elesize += sizeof(struct dictEntry) + zmalloc_size(znode);
                samples++;
                znode = znode->level[0].forward;
            }
            if (samples) asize += (double)elesize/samples*dictSize(d);
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else if (o->type == OBJ_HASH) {
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            asize = sizeof(*o)+(ziplistBlobLen(o->ptr));
        } else if (o->encoding == OBJ_ENCODING_HT) {
            d = o->ptr;
            di = dictGetIterator(d);
            asize = sizeof(*o)+sizeof(dict)+(sizeof(struct dictEntry*)*dictSlots(d));
            while((de = dictNext(di)) != NULL && samples < sample_size) {
                ele = dictGetKey(de);
                ele2 = dictGetVal(de);
                elesize += sdsZmallocSize(ele) + sdsZmallocSize(ele2);
                elesize += sizeof(struct dictEntry);
                samples++;
            }
            dictReleaseIterator(di);
            if (samples) asize += (double)elesize/samples*dictSize(d);
        } else {
            serverPanic("Unknown hash encoding");
        }
    } else if (o->type == OBJ_STREAM) {
        stream *s = o->ptr;
        asize = sizeof(*o);
        asize += streamRadixTreeMemoryUsage(s->rax);

        /* Now we have to add the listpacks. The last listpack is often non
         * complete, so we estimate the size of the first N listpacks, and
         * use the average to compute the size of the first N-1 listpacks, and
         * finally add the real size of the last node. */
        raxIterator ri;
        raxStart(&ri,s->rax);
        raxSeek(&ri,"^",NULL,0);
        size_t lpsize = 0, samples = 0;
        while(samples < sample_size && raxNext(&ri)) {
            unsigned char *lp = ri.data;
            lpsize += lpBytes(lp);
            samples++;
        }
        if (s->rax->numele <= samples) {
            asize += lpsize;
        } else {
            if (samples) lpsize /= samples; /* Compute the average. */
            asize += lpsize * (s->rax->numele-1);
            /* No need to check if seek succeeded, we enter this branch only
             * if there are a few elements in the radix tree. */
            raxSeek(&ri,"$",NULL,0);
            raxNext(&ri);
            asize += lpBytes(ri.data);
        }
        raxStop(&ri);

        /* Consumer groups also have a non trivial memory overhead if there
         * are many consumers and many groups, let's count at least the
         * overhead of the pending entries in the groups and consumers
         * PELs. */
        if (s->cgroups) {
            raxStart(&ri,s->cgroups);
            raxSeek(&ri,"^",NULL,0);
            while(raxNext(&ri)) {
                streamCG *cg = ri.data;
                asize += sizeof(*cg);
                asize += streamRadixTreeMemoryUsage(cg->pel);
                asize += sizeof(streamNACK)*raxSize(cg->pel);

                /* For each consumer we also need to add the basic data
                 * structures and the PEL memory usage. */
                raxIterator cri;
                raxStart(&cri,cg->consumers);
                raxSeek(&cri,"^",NULL,0);
                while(raxNext(&cri)) {
                    streamConsumer *consumer = cri.data;
                    asize += sizeof(*consumer);
                    asize += sdslen(consumer->name);
                    asize += streamRadixTreeMemoryUsage(consumer->pel);
                    /* Don't count NACKs again, they are shared with the
                     * consumer group PEL. */
                }
                raxStop(&cri);
            }
            raxStop(&ri);
        }
    } else if (o->type == OBJ_MODULE) {
        moduleValue *mv = o->ptr;
        moduleType *mt = mv->type;
        if (mt->mem_usage != NULL) {
            asize = mt->mem_usage(mv->value);
        } else {
            asize = 0;
        }
    } else {
        serverPanic("Unknown object type");
    }
    return asize;
}

/* Release data obtained with getMemoryOverheadData(). */
void freeMemoryOverheadData(struct redisMemOverhead *mh) {
    zfree(mh->db);
    zfree(mh);
}

/* Return a struct redisMemOverhead filled with memory overhead
 * information used for the MEMORY OVERHEAD and INFO command. The returned
 * structure pointer should be freed calling freeMemoryOverheadData(). */
struct redisMemOverhead *getMemoryOverheadData(void) {
    int j;
    size_t mem_total = 0;
    size_t mem = 0;
    size_t zmalloc_used = zmalloc_used_memory();
    struct redisMemOverhead *mh = zcalloc(sizeof(*mh));

    mh->total_allocated = zmalloc_used;
    mh->startup_allocated = server.initial_memory_usage;
    mh->peak_allocated = server.stat_peak_memory;
    mh->total_frag =
        (float)server.cron_malloc_stats.process_rss / server.cron_malloc_stats.zmalloc_used;
    mh->total_frag_bytes =
        server.cron_malloc_stats.process_rss - server.cron_malloc_stats.zmalloc_used;
    mh->allocator_frag =
        (float)server.cron_malloc_stats.allocator_active / server.cron_malloc_stats.allocator_allocated;
    mh->allocator_frag_bytes =
        server.cron_malloc_stats.allocator_active - server.cron_malloc_stats.allocator_allocated;
    mh->allocator_rss =
        (float)server.cron_malloc_stats.allocator_resident / server.cron_malloc_stats.allocator_active;
    mh->allocator_rss_bytes =
        server.cron_malloc_stats.allocator_resident - server.cron_malloc_stats.allocator_active;
    mh->rss_extra =
        (float)server.cron_malloc_stats.process_rss / server.cron_malloc_stats.allocator_resident;
    mh->rss_extra_bytes =
        server.cron_malloc_stats.process_rss - server.cron_malloc_stats.allocator_resident;

    mem_total += server.initial_memory_usage;

    mem = 0;
    if (server.repl_backlog)
        mem += zmalloc_size(server.repl_backlog);
    mh->repl_backlog = mem;
    mem_total += mem;

    /* Computing the memory used by the clients would be O(N) if done
     * here online. We use our values computed incrementally by
     * clientsCronTrackClientsMemUsage(). */
    mh->clients_slaves = server.stat_clients_type_memory[CLIENT_TYPE_SLAVE];
    mh->clients_normal = server.stat_clients_type_memory[CLIENT_TYPE_MASTER]+
                         server.stat_clients_type_memory[CLIENT_TYPE_PUBSUB]+
                         server.stat_clients_type_memory[CLIENT_TYPE_NORMAL];
    mem_total += mh->clients_slaves;
    mem_total += mh->clients_normal;

    mem = 0;
    if (server.aof_state != AOF_OFF) {
        mem += sdsZmallocSize(server.aof_buf);
        mem += aofRewriteBufferSize();
    }
    mh->aof_buffer = mem;
    mem_total+=mem;

    mem = server.lua_scripts_mem;
    mem += dictSize(server.lua_scripts) * sizeof(dictEntry) +
        dictSlots(server.lua_scripts) * sizeof(dictEntry*);
    mem += dictSize(server.repl_scriptcache_dict) * sizeof(dictEntry) +
        dictSlots(server.repl_scriptcache_dict) * sizeof(dictEntry*);
    if (listLength(server.repl_scriptcache_fifo) > 0) {
        mem += listLength(server.repl_scriptcache_fifo) * (sizeof(listNode) + 
            sdsZmallocSize(listNodeValue(listFirst(server.repl_scriptcache_fifo))));
    }
    mh->lua_caches = mem;
    mem_total+=mem;

    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        long long keyscount = dictSize(db->dict);
        if (keyscount==0) continue;

        mh->total_keys += keyscount;
        mh->db = zrealloc(mh->db,sizeof(mh->db[0])*(mh->num_dbs+1));
        mh->db[mh->num_dbs].dbid = j;

        mem = dictSize(db->dict) * sizeof(dictEntry) +
              dictSlots(db->dict) * sizeof(dictEntry*) +
              dictSize(db->dict) * sizeof(robj);
        mh->db[mh->num_dbs].overhead_ht_main = mem;
        mem_total+=mem;

        mem = dictSize(db->expires) * sizeof(dictEntry) +
              dictSlots(db->expires) * sizeof(dictEntry*);
        mh->db[mh->num_dbs].overhead_ht_expires = mem;
        mem_total+=mem;

        mh->num_dbs++;
    }

    mh->overhead_total = mem_total;
    mh->dataset = zmalloc_used - mem_total;
    mh->peak_perc = (float)zmalloc_used*100/mh->peak_allocated;

    /* Metrics computed after subtracting the startup memory from
     * the total memory. */
    size_t net_usage = 1;
    if (zmalloc_used > mh->startup_allocated)
        net_usage = zmalloc_used - mh->startup_allocated;
    mh->dataset_perc = (float)mh->dataset*100/net_usage;
    mh->bytes_per_key = mh->total_keys ? (net_usage / mh->total_keys) : 0;

    return mh;
}

/* Helper for "MEMORY allocator-stats", used as a callback for the jemalloc
 * stats output. */
void inputCatSds(void *result, const char *str) {
    /* result is actually a (sds *), so re-cast it here */
    sds *info = (sds *)result;
    *info = sdscat(*info, str);
}

/* This implements MEMORY DOCTOR. An human readable analysis of the Redis
 * memory condition. */
sds getMemoryDoctorReport(void) {
    int empty = 0;          /* Instance is empty or almost empty. */
    int big_peak = 0;       /* Memory peak is much larger than used mem. */
    int high_frag = 0;      /* High fragmentation. */
    int high_alloc_frag = 0;/* High allocator fragmentation. */
    int high_proc_rss = 0;  /* High process rss overhead. */
    int high_alloc_rss = 0; /* High rss overhead. */
    int big_slave_buf = 0;  /* Slave buffers are too big. */
    int big_client_buf = 0; /* Client buffers are too big. */
    int many_scripts = 0;   /* Script cache has too many scripts. */
    int num_reports = 0;
    struct redisMemOverhead *mh = getMemoryOverheadData();

    if (mh->total_allocated < (1024*1024*5)) {
        empty = 1;
        num_reports++;
    } else {
        /* Peak is > 150% of current used memory? */
        if (((float)mh->peak_allocated / mh->total_allocated) > 1.5) {
            big_peak = 1;
            num_reports++;
        }

        /* Fragmentation is higher than 1.4 and 10MB ?*/
        if (mh->total_frag > 1.4 && mh->total_frag_bytes > 10<<20) {
            high_frag = 1;
            num_reports++;
        }

        /* External fragmentation is higher than 1.1 and 10MB? */
        if (mh->allocator_frag > 1.1 && mh->allocator_frag_bytes > 10<<20) {
            high_alloc_frag = 1;
            num_reports++;
        }

        /* Allocator rss is higher than 1.1 and 10MB ? */
        if (mh->allocator_rss > 1.1 && mh->allocator_rss_bytes > 10<<20) {
            high_alloc_rss = 1;
            num_reports++;
        }

        /* Non-Allocator rss is higher than 1.1 and 10MB ? */
        if (mh->rss_extra > 1.1 && mh->rss_extra_bytes > 10<<20) {
            high_proc_rss = 1;
            num_reports++;
        }

        /* Clients using more than 200k each average? */
        long numslaves = listLength(server.slaves);
        long numclients = listLength(server.clients)-numslaves;
        if (mh->clients_normal / numclients > (1024*200)) {
            big_client_buf = 1;
            num_reports++;
        }

        /* Slaves using more than 10 MB each? */
        if (numslaves > 0 && mh->clients_slaves / numslaves > (1024*1024*10)) {
            big_slave_buf = 1;
            num_reports++;
        }

        /* Too many scripts are cached? */
        if (dictSize(server.lua_scripts) > 1000) {
            many_scripts = 1;
            num_reports++;
        }
    }

    sds s;
    if (num_reports == 0) {
        s = sdsnew(
        "Hi Sam, I can't find any memory issue in your instance. "
        "I can only account for what occurs on this base.\n");
    } else if (empty == 1) {
        s = sdsnew(
        "Hi Sam, this instance is empty or is using very little memory, "
        "my issues detector can't be used in these conditions. "
        "Please, leave for your mission on Earth and fill it with some data. "
        "The new Sam and I will be back to our programming as soon as I "
        "finished rebooting.\n");
    } else {
        s = sdsnew("Sam, I detected a few issues in this Redis instance memory implants:\n\n");
        if (big_peak) {
            s = sdscat(s," * Peak memory: In the past this instance used more than 150% the memory that is currently using. The allocator is normally not able to release memory after a peak, so you can expect to see a big fragmentation ratio, however this is actually harmless and is only due to the memory peak, and if the Redis instance Resident Set Size (RSS) is currently bigger than expected, the memory will be used as soon as you fill the Redis instance with more data. If the memory peak was only occasional and you want to try to reclaim memory, please try the MEMORY PURGE command, otherwise the only other option is to shutdown and restart the instance.\n\n");
        }
        if (high_frag) {
            s = sdscatprintf(s," * High total RSS: This instance has a memory fragmentation and RSS overhead greater than 1.4 (this means that the Resident Set Size of the Redis process is much larger than the sum of the logical allocations Redis performed). This problem is usually due either to a large peak memory (check if there is a peak memory entry above in the report) or may result from a workload that causes the allocator to fragment memory a lot. If the problem is a large peak memory, then there is no issue. Otherwise, make sure you are using the Jemalloc allocator and not the default libc malloc. Note: The currently used allocator is \"%s\".\n\n", ZMALLOC_LIB);
        }
        if (high_alloc_frag) {
            s = sdscatprintf(s," * High allocator fragmentation: This instance has an allocator external fragmentation greater than 1.1. This problem is usually due either to a large peak memory (check if there is a peak memory entry above in the report) or may result from a workload that causes the allocator to fragment memory a lot. You can try enabling 'activedefrag' config option.\n\n");
        }
        if (high_alloc_rss) {
            s = sdscatprintf(s," * High allocator RSS overhead: This instance has an RSS memory overhead is greater than 1.1 (this means that the Resident Set Size of the allocator is much larger than the sum what the allocator actually holds). This problem is usually due to a large peak memory (check if there is a peak memory entry above in the report), you can try the MEMORY PURGE command to reclaim it.\n\n");
        }
        if (high_proc_rss) {
            s = sdscatprintf(s," * High process RSS overhead: This instance has non-allocator RSS memory overhead is greater than 1.1 (this means that the Resident Set Size of the Redis process is much larger than the RSS the allocator holds). This problem may be due to Lua scripts or Modules.\n\n");
        }
        if (big_slave_buf) {
            s = sdscat(s," * Big replica buffers: The replica output buffers in this instance are greater than 10MB for each replica (on average). This likely means that there is some replica instance that is struggling receiving data, either because it is too slow or because of networking issues. As a result, data piles on the master output buffers. Please try to identify what replica is not receiving data correctly and why. You can use the INFO output in order to check the replicas delays and the CLIENT LIST command to check the output buffers of each replica.\n\n");
        }
        if (big_client_buf) {
            s = sdscat(s," * Big client buffers: The clients output buffers in this instance are greater than 200K per client (on average). This may result from different causes, like Pub/Sub clients subscribed to channels bot not receiving data fast enough, so that data piles on the Redis instance output buffer, or clients sending commands with large replies or very large sequences of commands in the same pipeline. Please use the CLIENT LIST command in order to investigate the issue if it causes problems in your instance, or to understand better why certain clients are using a big amount of memory.\n\n");
        }
        if (many_scripts) {
            s = sdscat(s," * Many scripts: There seem to be many cached scripts in this instance (more than 1000). This may be because scripts are generated and `EVAL`ed, instead of being parameterized (with KEYS and ARGV), `SCRIPT LOAD`ed and `EVALSHA`ed. Unless `SCRIPT FLUSH` is called periodically, the scripts' caches may end up consuming most of your memory.\n\n");
        }
        s = sdscat(s,"I'm here to keep you safe, Sam. I want to help you.\n");
    }
    freeMemoryOverheadData(mh);
    return s;
}

/* Set the object LRU/LFU depending on server.maxmemory_policy.
 * The lfu_freq arg is only relevant if policy is MAXMEMORY_FLAG_LFU.
 * The lru_idle and lru_clock args are only relevant if policy
 * is MAXMEMORY_FLAG_LRU.
 * Either or both of them may be <0, in that case, nothing is set. */
int objectSetLRUOrLFU(robj *val, long long lfu_freq, long long lru_idle,
                       long long lru_clock, int lru_multiplier) {
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        if (lfu_freq >= 0) {
            serverAssert(lfu_freq <= 255);
            val->lru = (LFUGetTimeInMinutes()<<8) | lfu_freq;
            return 1;
        }
    } else if (lru_idle >= 0) {
        /* Provided LRU idle time is in seconds. Scale
         * according to the LRU clock resolution this Redis
         * instance was compiled with (normally 1000 ms, so the
         * below statement will expand to lru_idle*1000/1000. */
        lru_idle = lru_idle*lru_multiplier/LRU_CLOCK_RESOLUTION;
        long lru_abs = lru_clock - lru_idle; /* Absolute access time. */
        /* If the LRU field underflows (since LRU it is a wrapping
         * clock), the best we can do is to provide a large enough LRU
         * that is half-way in the circlular LRU clock we use: this way
         * the computed idle time for this object will stay high for quite
         * some time. */
        if (lru_abs < 0)
            lru_abs = (lru_clock+(LRU_CLOCK_MAX/2)) % LRU_CLOCK_MAX;
        val->lru = lru_abs;
        return 1;
    }
    return 0;
}

/* ======================= The OBJECT and MEMORY commands =================== */

/* This is a helper function for the OBJECT command. We need to lookup keys
 * without any modification of LRU or other parameters. */
robj *objectCommandLookup(client *c, robj *key) {
    dictEntry *de;

    if ((de = dictFind(c->db->dict,key->ptr)) == NULL) return NULL;
    return (robj*) dictGetVal(de);
}

robj *objectCommandLookupOrReply(client *c, robj *key, robj *reply) {
    robj *o = objectCommandLookup(c,key);

    if (!o) addReply(c, reply);
    return o;
}

/* Object command allows to inspect the internals of a Redis Object.
 * Usage: OBJECT <refcount|encoding|idletime|freq> <key> */
void objectCommand(client *c) {
    robj *o;

    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"ENCODING <key> -- Return the kind of internal representation used in order to store the value associated with a key.",
"FREQ <key> -- Return the access frequency index of the key. The returned integer is proportional to the logarithm of the recent access frequency of the key.",
"IDLETIME <key> -- Return the idle time of the key, that is the approximated number of seconds elapsed since the last access to the key.",
"REFCOUNT <key> -- Return the number of references of the value associated with the specified key.",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"refcount") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.null[c->resp]))
                == NULL) return;
        addReplyLongLong(c,o->refcount);
    } else if (!strcasecmp(c->argv[1]->ptr,"encoding") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.null[c->resp]))
                == NULL) return;
        addReplyBulkCString(c,strEncoding(o->encoding));
    } else if (!strcasecmp(c->argv[1]->ptr,"idletime") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.null[c->resp]))
                == NULL) return;
        if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
            addReplyError(c,"An LFU maxmemory policy is selected, idle time not tracked. Please note that when switching between policies at runtime LRU and LFU data will take some time to adjust.");
            return;
        }
        addReplyLongLong(c,estimateObjectIdleTime(o)/1000);
    } else if (!strcasecmp(c->argv[1]->ptr,"freq") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.null[c->resp]))
                == NULL) return;
        if (!(server.maxmemory_policy & MAXMEMORY_FLAG_LFU)) {
            addReplyError(c,"An LFU maxmemory policy is not selected, access frequency not tracked. Please note that when switching between policies at runtime LRU and LFU data will take some time to adjust.");
            return;
        }
        /* LFUDecrAndReturn should be called
         * in case of the key has not been accessed for a long time,
         * because we update the access time only
         * when the key is read or overwritten. */
        addReplyLongLong(c,LFUDecrAndReturn(o));
    } else {
        addReplySubcommandSyntaxError(c);
    }
}

/* The memory command will eventually be a complete interface for the
 * memory introspection capabilities of Redis.
 *
 * Usage: MEMORY usage <key> */
void memoryCommand(client *c) {
    if (!strcasecmp(c->argv[1]->ptr,"help") && c->argc == 2) {
        const char *help[] = {
"DOCTOR - Return memory problems reports.",
"MALLOC-STATS -- Return internal statistics report from the memory allocator.",
"PURGE -- Attempt to purge dirty pages for reclamation by the allocator.",
"STATS -- Return information about the memory usage of the server.",
"USAGE <key> [SAMPLES <count>] -- Return memory in bytes used by <key> and its value. Nested values are sampled up to <count> times (default: 5).",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp(c->argv[1]->ptr,"usage") && c->argc >= 3) {
        dictEntry *de;
        long long samples = OBJ_COMPUTE_SIZE_DEF_SAMPLES;
        for (int j = 3; j < c->argc; j++) {
            if (!strcasecmp(c->argv[j]->ptr,"samples") &&
                j+1 < c->argc)
            {
                if (getLongLongFromObjectOrReply(c,c->argv[j+1],&samples,NULL)
                     == C_ERR) return;
                if (samples < 0) {
                    addReply(c,shared.syntaxerr);
                    return;
                }
                if (samples == 0) samples = LLONG_MAX;;
                j++; /* skip option argument. */
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }
        if ((de = dictFind(c->db->dict,c->argv[2]->ptr)) == NULL) {
            addReplyNull(c);
            return;
        }
        size_t usage = objectComputeSize(dictGetVal(de),samples);
        usage += sdsZmallocSize(dictGetKey(de));
        usage += sizeof(dictEntry);
        addReplyLongLong(c,usage);
    } else if (!strcasecmp(c->argv[1]->ptr,"stats") && c->argc == 2) {
        struct redisMemOverhead *mh = getMemoryOverheadData();

        addReplyMapLen(c,25+mh->num_dbs);

        addReplyBulkCString(c,"peak.allocated");
        addReplyLongLong(c,mh->peak_allocated);

        addReplyBulkCString(c,"total.allocated");
        addReplyLongLong(c,mh->total_allocated);

        addReplyBulkCString(c,"startup.allocated");
        addReplyLongLong(c,mh->startup_allocated);

        addReplyBulkCString(c,"replication.backlog");
        addReplyLongLong(c,mh->repl_backlog);

        addReplyBulkCString(c,"clients.slaves");
        addReplyLongLong(c,mh->clients_slaves);

        addReplyBulkCString(c,"clients.normal");
        addReplyLongLong(c,mh->clients_normal);

        addReplyBulkCString(c,"aof.buffer");
        addReplyLongLong(c,mh->aof_buffer);

        addReplyBulkCString(c,"lua.caches");
        addReplyLongLong(c,mh->lua_caches);

        for (size_t j = 0; j < mh->num_dbs; j++) {
            char dbname[32];
            snprintf(dbname,sizeof(dbname),"db.%zd",mh->db[j].dbid);
            addReplyBulkCString(c,dbname);
            addReplyMapLen(c,2);

            addReplyBulkCString(c,"overhead.hashtable.main");
            addReplyLongLong(c,mh->db[j].overhead_ht_main);

            addReplyBulkCString(c,"overhead.hashtable.expires");
            addReplyLongLong(c,mh->db[j].overhead_ht_expires);
        }

        addReplyBulkCString(c,"overhead.total");
        addReplyLongLong(c,mh->overhead_total);

        addReplyBulkCString(c,"keys.count");
        addReplyLongLong(c,mh->total_keys);

        addReplyBulkCString(c,"keys.bytes-per-key");
        addReplyLongLong(c,mh->bytes_per_key);

        addReplyBulkCString(c,"dataset.bytes");
        addReplyLongLong(c,mh->dataset);

        addReplyBulkCString(c,"dataset.percentage");
        addReplyDouble(c,mh->dataset_perc);

        addReplyBulkCString(c,"peak.percentage");
        addReplyDouble(c,mh->peak_perc);

        addReplyBulkCString(c,"allocator.allocated");
        addReplyLongLong(c,server.cron_malloc_stats.allocator_allocated);

        addReplyBulkCString(c,"allocator.active");
        addReplyLongLong(c,server.cron_malloc_stats.allocator_active);

        addReplyBulkCString(c,"allocator.resident");
        addReplyLongLong(c,server.cron_malloc_stats.allocator_resident);

        addReplyBulkCString(c,"allocator-fragmentation.ratio");
        addReplyDouble(c,mh->allocator_frag);

        addReplyBulkCString(c,"allocator-fragmentation.bytes");
        addReplyLongLong(c,mh->allocator_frag_bytes);

        addReplyBulkCString(c,"allocator-rss.ratio");
        addReplyDouble(c,mh->allocator_rss);

        addReplyBulkCString(c,"allocator-rss.bytes");
        addReplyLongLong(c,mh->allocator_rss_bytes);

        addReplyBulkCString(c,"rss-overhead.ratio");
        addReplyDouble(c,mh->rss_extra);

        addReplyBulkCString(c,"rss-overhead.bytes");
        addReplyLongLong(c,mh->rss_extra_bytes);

        addReplyBulkCString(c,"fragmentation"); /* this is the total RSS overhead, including fragmentation */
        addReplyDouble(c,mh->total_frag); /* it is kept here for backwards compatibility */

        addReplyBulkCString(c,"fragmentation.bytes");
        addReplyLongLong(c,mh->total_frag_bytes);

        freeMemoryOverheadData(mh);
    } else if (!strcasecmp(c->argv[1]->ptr,"malloc-stats") && c->argc == 2) {
#if defined(USE_JEMALLOC)
        sds info = sdsempty();
        je_malloc_stats_print(inputCatSds, &info, NULL);
        addReplyVerbatim(c,info,sdslen(info),"txt");
        sdsfree(info);
#else
        addReplyBulkCString(c,"Stats not supported for the current allocator");
#endif
    } else if (!strcasecmp(c->argv[1]->ptr,"doctor") && c->argc == 2) {
        sds report = getMemoryDoctorReport();
        addReplyVerbatim(c,report,sdslen(report),"txt");
        sdsfree(report);
    } else if (!strcasecmp(c->argv[1]->ptr,"purge") && c->argc == 2) {
        if (jemalloc_purge() == 0)
            addReply(c, shared.ok);
        else
            addReplyError(c, "Error purging dirty pages");
    } else {
        addReplyErrorFormat(c, "Unknown subcommand or wrong number of arguments for '%s'. Try MEMORY HELP", (char*)c->argv[1]->ptr);
    }
}
