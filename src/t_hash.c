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
#include <math.h>

/*-----------------------------------------------------------------------------
 * Hash type API
 *----------------------------------------------------------------------------*/

/* Check the length of a number of objects to see if we need to convert a
 * ziplist to a real hash. Note that we only check string encoded objects
 * as their string length can be queried in constant time. */
/* Check the length of a number of objects to see if we need to convert a
 * ziplist to a real hash.
 *
 * 对 argv 数组中的多个对象进行检查，
 * 看是否需要将对象的编码从 REDIS_ENCODING_ZIPLIST 转换成 REDIS_ENCODING_HT
 *
 * Note that we only check string encoded objects
 * as their string length can be queried in constant time.
 *
 * 注意程序只检查字符串值，因为它们的长度可以在常数时间内取得。
 */
/* 当hashType为ziplist时，判断对象长度是否超出了服务端可接受的ziplist最大长度，超过则转成哈希字典类型 */
void hashTypeTryConversion(robj *o, robj **argv, int start, int end) {
    int i;

    // 当前是否为ziplist，不是，直接返回
    if (o->encoding != OBJ_ENCODING_ZIPLIST) return;

    for (i = start; i <= end; i++) {
        if (sdsEncodedObject(argv[i]) &&
            sdslen(argv[i]->ptr) > server.hash_max_ziplist_value)
        {
            // 进行类型转换
            hashTypeConvert(o, OBJ_ENCODING_HT);
            break;
        }
    }
}

/* 获取ziplist压缩列表中的某个索引位置上的值 */
int hashTypeGetFromZiplist(robj *o, sds field,
                           unsigned char **vstr,
                           unsigned int *vlen,
                           long long *vll)
{
    unsigned char *zl, *fptr = NULL, *vptr = NULL;
    int ret;

    serverAssert(o->encoding == OBJ_ENCODING_ZIPLIST);

    zl = o->ptr;
    // 获取当前ziplist的首位指针
    fptr = ziplistIndex(zl, ZIPLIST_HEAD);
    if (fptr != NULL) {
        // 查找到当前key所对应的 指针 ，跳跃1位，防止 value 干扰key
        fptr = ziplistFind(fptr, (unsigned char*)field, sdslen(field), 1);
        if (fptr != NULL) {
            /* 抓取指向值的指针（fptr指向字段） */
            vptr = ziplistNext(zl, fptr);   // 获取到value的指针
            serverAssert(vptr != NULL);
        }
    }

    if (vptr != NULL) {
        ret = ziplistGet(vptr, vstr, vlen, vll);//通过指针获取到真实的值
        serverAssert(ret);
        return 0;
    }

    return -1;
}

/* Get the value from a hash table encoded hash, identified by field.
 * Returns NULL when the field cannot be found, otherwise the SDS value
 * is returned.
 *
 * 从 REDIS_ENCODING_HT 编码的 hash 中取出和 field 相对应的值。
 *
 * 成功找到值时返回值 ，没找到返回 null 。
 */
sds hashTypeGetFromHashTable(robj *o, sds field) {
    dictEntry *de;

    // 确保编码正确
    serverAssert(o->encoding == OBJ_ENCODING_HT);

    // 在字典中查找域（键）
    de = dictFind(o->ptr, field);

    // 键不存在
    if (de == NULL) return NULL;

    // 取出域（键）的值，直接返回
    return dictGetVal(de);
}

/* Higher level function of hashTypeGet*() that returns the hash value
 * associated with the specified field. If the field is found C_OK
 * is returned, otherwise C_ERR. The returned object is returned by
 * reference in either *vstr and *vlen if it's returned in string form,
 * or stored in *vll if it's returned as a number.
 *
 * If *vll is populated *vstr is set to NULL, so the caller
 * can always check the function return by checking the return value
 * for C_OK and checking if vll (or vstr) is NULL. */
/* 从当前数据结构中获取对应field的value */
int hashTypeGetValue(robj *o, sds field, unsigned char **vstr, unsigned int *vlen, long long *vll) {
    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        *vstr = NULL;
        if (hashTypeGetFromZiplist(o, field, vstr, vlen, vll) == 0)//从ziplist中获取当前值
            return C_OK;
    } else if (o->encoding == OBJ_ENCODING_HT) {
        sds value;
        if ((value = hashTypeGetFromHashTable(o, field)) != NULL) {//从哈希表中获取当前值
            *vstr = (unsigned char*) value;
            *vlen = sdslen(value);
            return C_OK;
        }
    } else {
        serverPanic("Unknown hash encoding");
    }
    return C_ERR;
}

/* Like hashTypeGetValue() but returns a Redis object, which is useful for
 * interaction with the hash type outside t_hash.c.
 * The function returns NULL if the field is not found in the hash. Otherwise
 * a newly allocated string object with the value is returned. */
/* 从指定的数据结构中获取field对应的value，返回类型为robj */
robj *hashTypeGetValueObject(robj *o, sds field) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vll;

    //获取到标准value，string或者int，因为这这存储不一样，后续分开处理
    if (hashTypeGetValue(o,field,&vstr,&vlen,&vll) == C_ERR) return NULL;
    if (vstr) return createStringObject((char*)vstr,vlen);//处理string
    else return createStringObjectFromLongLong(vll);//处理int
}

/* Higher level function using hashTypeGet*() to return the length of the
 * object associated with the requested field, or 0 if the field does not
 * exist. */
/* 获取当前结构题中field对应的value长度 */
size_t hashTypeGetValueLength(robj *o, sds field) {
    size_t len = 0;
    if (o->encoding == OBJ_ENCODING_ZIPLIST) {//压缩列表
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        //获取value从ziplist中
        if (hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll) == 0)
            len = vstr ? vlen : sdigits10(vll);//string直接返回长度，int需要进行就会转化
    } else if (o->encoding == OBJ_ENCODING_HT) {//数据结构为哈希表
        sds aux;

        if ((aux = hashTypeGetFromHashTable(o, field)) != NULL)//获取value
            len = sdslen(aux);//直接返回长度
    } else {
        serverPanic("Unknown hash encoding");
    }
    return len;
}

/* Test if the specified field exists in the given hash. Returns 1 if the field
 * exists, and 0 when it doesn't. */
/* 指定field所对应的value存在不 */
int hashTypeExists(robj *o, sds field) {
    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        if (hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll) == 0) return 1;
    } else if (o->encoding == OBJ_ENCODING_HT) {
        if (hashTypeGetFromHashTable(o, field) != NULL) return 1;
    } else {
        serverPanic("Unknown hash encoding");
    }
    return 0;
}

/* Add a new field, overwrite the old with the new value if it already exists.
 * Return 0 on insert and 1 on update.
 *
 * By default, the key and value SDS strings are copied if needed, so the
 * caller retains ownership of the strings passed. However this behavior
 * can be effected by passing appropriate flags (possibly bitwise OR-ed):
 *
 * HASH_SET_TAKE_FIELD -- The SDS field ownership passes to the function.
 * HASH_SET_TAKE_VALUE -- The SDS value ownership passes to the function.
 *
 * When the flags are used the caller does not need to release the passed
 * SDS string(s). It's up to the function to use the string to create a new
 * entry or to free the SDS string before returning to the caller.
 *
 * HASH_SET_COPY corresponds to no flags passed, and means the default
 * semantics of copying the values if needed.
 *
 */
#define HASH_SET_TAKE_FIELD (1<<0)
#define HASH_SET_TAKE_VALUE (1<<1)
#define HASH_SET_COPY 0
/* 哈希设置键值对 */
int hashTypeSet(robj *o, sds field, sds value, int flags) {
    int update = 0;

    if (o->encoding == OBJ_ENCODING_ZIPLIST) {//当前数据库为ziplist
        unsigned char *zl, *fptr, *vptr;

        zl = o->ptr;
        fptr = ziplistIndex(zl, ZIPLIST_HEAD);//获取头节点指针
        if (fptr != NULL) {
            //在ziplist中查找当前所对应的field存在不，存在返回field所对应的指针
            fptr = ziplistFind(fptr, (unsigned char*)field, sdslen(field), 1);
            if (fptr != NULL) {//存在
                /* Grab pointer to the value (fptr points to the field) */
                vptr = ziplistNext(zl, fptr);//获取field所对应的value的指针
                serverAssert(vptr != NULL);
                update = 1;

                /* Delete value */
                zl = ziplistDelete(zl, &vptr);//删除该值

                /* Insert new value */
                zl = ziplistInsert(zl, vptr, (unsigned char*)value,
                        sdslen(value));//在该指针所对应的未知添加新值
            }
        }

        if (!update) {
            /* Push new field/value pair onto the tail of the ziplist */
            //从尾部添加对应的field与value
            zl = ziplistPush(zl, (unsigned char*)field, sdslen(field),
                    ZIPLIST_TAIL);
            zl = ziplistPush(zl, (unsigned char*)value, sdslen(value),
                    ZIPLIST_TAIL);
        }
        o->ptr = zl;

        /* Check if the ziplist needs to be converted to a hash table */
        //判别当前长度否已经满足ziplist转化哈希条件
        if (hashTypeLength(o) > server.hash_max_ziplist_entries)
            hashTypeConvert(o, OBJ_ENCODING_HT);
    } else if (o->encoding == OBJ_ENCODING_HT) {//当前数据库为ziplist
        dictEntry *de = dictFind(o->ptr,field);//查看该field在字典中是否存在
        if (de) {//存在
            sdsfree(dictGetVal(de));//释放原有的value
            if (flags & HASH_SET_TAKE_VALUE) {//重新设置
                dictGetVal(de) = value;
                value = NULL;
            } else {
                dictGetVal(de) = sdsdup(value);
            }
            update = 1;
        } else {//新增键值对
            sds f,v;
            if (flags & HASH_SET_TAKE_FIELD) {
                f = field;
                field = NULL;
            } else {
                f = sdsdup(field);
            }
            if (flags & HASH_SET_TAKE_VALUE) {
                v = value;
                value = NULL;
            } else {
                v = sdsdup(value);
            }
            dictAdd(o->ptr,f,v);
        }
    } else {
        serverPanic("Unknown hash encoding");
    }

    /* Free SDS strings we did not referenced elsewhere if the flags
     * want this function to be responsible. */
    if (flags & HASH_SET_TAKE_FIELD && field) sdsfree(field);//释放原有的field
    if (flags & HASH_SET_TAKE_VALUE && value) sdsfree(value);//释放原有的value
    return update;
}

/* Delete an element from a hash.
 * Return 1 on deleted and 0 on not found. */
/* 哈希删除 hdel */
int hashTypeDelete(robj *o, sds field) {
    int deleted = 0;

    if (o->encoding == OBJ_ENCODING_ZIPLIST) {//ziplist
        unsigned char *zl, *fptr;

        zl = o->ptr;
        fptr = ziplistIndex(zl, ZIPLIST_HEAD);//首节点指针
        if (fptr != NULL) {
            fptr = ziplistFind(fptr, (unsigned char*)field, sdslen(field), 1);//field指针
            if (fptr != NULL) {
                zl = ziplistDelete(zl,&fptr); /* Delete the key. */
                zl = ziplistDelete(zl,&fptr); /* Delete the value. */
                o->ptr = zl;
                deleted = 1;
            }
        }
    } else if (o->encoding == OBJ_ENCODING_HT) {//哈希
        if (dictDelete((dict*)o->ptr, field) == C_OK) {//直接删除
            deleted = 1;

            /* Always check if the dictionary needs a resize after a delete. */
            if (htNeedsResize(o->ptr)) dictResize(o->ptr);//重新计算哈希大小
        }

    } else {
        serverPanic("Unknown hash encoding");
    }
    return deleted;
}

/* Return the number of elements in a hash. */
/* 获取当前哈希表一共有多少个元素数 */
unsigned long hashTypeLength(const robj *o) {
    unsigned long length = ULONG_MAX;

    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        length = ziplistLen(o->ptr) / 2;//获取ziplist长度/2
    } else if (o->encoding == OBJ_ENCODING_HT) {
        length = dictSize((const dict*)o->ptr);//获取字典已被使用的节点数量
    } else {
        serverPanic("Unknown hash encoding");
    }
    return length;
}

/* 获取hashType-dict迭代器 */
hashTypeIterator *hashTypeInitIterator(robj *subject) {
    hashTypeIterator *hi = zmalloc(sizeof(hashTypeIterator));//分配字典迭代器
    hi->subject = subject;
    hi->encoding = subject->encoding;

    if (hi->encoding == OBJ_ENCODING_ZIPLIST) {
        hi->fptr = NULL;
        hi->vptr = NULL;
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        hi->di = dictGetIterator(subject->ptr);//初始化字典迭代器
    } else {
        serverPanic("Unknown hash encoding");
    }
    return hi;
}

/* 释放字典迭代器 */
void hashTypeReleaseIterator(hashTypeIterator *hi) {
    if (hi->encoding == OBJ_ENCODING_HT)
        dictReleaseIterator(hi->di);
    zfree(hi);
}

/* Move to the next entry in the hash. Return C_OK when the next entry
 * could be found and C_ERR when the iterator reaches the end. */
/* 获取当前迭代器的下一个节点 */
int hashTypeNext(hashTypeIterator *hi) {
    if (hi->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl;
        unsigned char *fptr, *vptr;

        zl = hi->subject->ptr;
        fptr = hi->fptr;
        vptr = hi->vptr;

        if (fptr == NULL) {//初始化
            /* Initialize cursor */
            serverAssert(vptr == NULL);
            fptr = ziplistIndex(zl, 0);
        } else {
            /* Advance cursor */
            serverAssert(vptr != NULL);
            fptr = ziplistNext(zl, vptr);//field节点指针
        }
        if (fptr == NULL) return C_ERR;

        /* Grab pointer to the value (fptr points to the field) */
        vptr = ziplistNext(zl, fptr);//value节点指针
        serverAssert(vptr != NULL);

        /* fptr, vptr now point to the first or next pair */
        hi->fptr = fptr;
        hi->vptr = vptr;
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        if ((hi->de = dictNext(hi->di)) == NULL) return C_ERR;
    } else {
        serverPanic("Unknown hash encoding");
    }
    return C_OK;
}

/* 解析光标处的值 */
void hashTypeCurrentFromZiplist(hashTypeIterator *hi, int what,
                                unsigned char **vstr,
                                unsigned int *vlen,
                                long long *vll)
{
    int ret;

    serverAssert(hi->encoding == OBJ_ENCODING_ZIPLIST);

    if (what & OBJ_HASH_KEY) {
        ret = ziplistGet(hi->fptr, vstr, vlen, vll);
        serverAssert(ret);
    } else {
        ret = ziplistGet(hi->vptr, vstr, vlen, vll);
        serverAssert(ret);
    }
}

/* Get the field or value at iterator cursor, for an iterator on a hash value
 * encoded as a hash table. Prototype is similar to
 * `hashTypeGetFromHashTable`. */
/* 根据当前迭代器的位置，获取当前dict的所在位置的key位置，或value该位置上的值 */
sds hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what) {
    serverAssert(hi->encoding == OBJ_ENCODING_HT);

    if (what & OBJ_HASH_KEY) {
        return dictGetKey(hi->de);
    } else {
        return dictGetVal(hi->de);
    }
}

/* Higher level function of hashTypeCurrent*() that returns the hash value
 * at current iterator position.
 *
 * The returned element is returned by reference in either *vstr and *vlen if
 * it's returned in string form, or stored in *vll if it's returned as
 * a number.
 *
 * If *vll is populated *vstr is set to NULL, so the caller
 * can always check the function return by checking the return value
 * type checking if vstr == NULL. */
/* 根据当前迭代器的位置，获取当前key对象 */
void hashTypeCurrentObject(hashTypeIterator *hi, int what, unsigned char **vstr, unsigned int *vlen, long long *vll) {
    if (hi->encoding == OBJ_ENCODING_ZIPLIST) {
        *vstr = NULL;
        hashTypeCurrentFromZiplist(hi, what, vstr, vlen, vll);
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        sds ele = hashTypeCurrentFromHashTable(hi, what);
        *vstr = (unsigned char*) ele;
        *vlen = sdslen(ele);
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/*
 * 作为新的返回当前迭代器位置的键或值SDS字符串。
 * */
sds hashTypeCurrentObjectNewSds(hashTypeIterator *hi, int what) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vll;

    hashTypeCurrentObject(hi,what,&vstr,&vlen,&vll);//从ziplist中解析当前的value
    if (vstr) return sdsnewlen(vstr,vlen);//值为string转化为sds返回
    return sdsfromlonglong(vll);//值为longlong转化为sds返回
}

/* 根据c客户端对象，找到key是否存在，创建或实现添加操作 */
robj *hashTypeLookupWriteOrCreate(client *c, robj *key) {
    robj *o = lookupKeyWrite(c->db,key);
    if (o == NULL) {//key不存在，直接添加
        o = createHashObject();
        dbAdd(c->db,key,o);
    } else {
        if (o->type != OBJ_HASH) {
            addReply(c,shared.wrongtypeerr);
            return NULL;
        }
    }
    return o;
}

/* 从ziplist压缩表到hashtable的转换 */
void hashTypeConvertZiplist(robj *o, int enc) {
    serverAssert(o->encoding == OBJ_ENCODING_ZIPLIST);//断言

    if (enc == OBJ_ENCODING_ZIPLIST) {
        /* Nothing to do... */

    } else if (enc == OBJ_ENCODING_HT) {//当前类别需要转化为hashtable
        hashTypeIterator *hi;
        dict *dict;
        int ret;

        hi = hashTypeInitIterator(o);//初始化dict迭代器
        dict = dictCreate(&hashDictType, NULL);//创建一个空的字典

        while (hashTypeNext(hi) != C_ERR) {//迭代
            sds key, value;

            key = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_KEY);
            value = hashTypeCurrentObjectNewSds(hi,OBJ_HASH_VALUE);
            ret = dictAdd(dict, key, value);
            if (ret != DICT_OK) {
                serverLogHexDump(LL_WARNING,"ziplist with dup elements dump",
                    o->ptr,ziplistBlobLen(o->ptr));
                serverPanic("Ziplist corruption detected");
            }
        }
        hashTypeReleaseIterator(hi);//释放迭代器资源
        zfree(o->ptr);//释放原有资源
        o->encoding = OBJ_ENCODING_HT;
        o->ptr = dict;
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/* 对象转换操作，例如从ziplist到dict的转换 */
void hashTypeConvert(robj *o, int enc) {
    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        hashTypeConvertZiplist(o, enc);
    } else if (o->encoding == OBJ_ENCODING_HT) {
        serverPanic("Not implemented");
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/*-----------------------------------------------------------------------------
 * Hash type commands
 *----------------------------------------------------------------------------*/

/* hsetnx客户端命令 */
void hsetnxCommand(client *c) {
    robj *o;
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;//写入或者创建key
    hashTypeTryConversion(o,c->argv,2,3);//判别当前的value是否超过了ziplist限制，超过了直接进行类型转换

    if (hashTypeExists(o, c->argv[2]->ptr)) {//存在，直接忽略，返回0
        addReply(c, shared.czero);
    } else {
        //哈希设置键值对
        hashTypeSet(o,c->argv[2]->ptr,c->argv[3]->ptr,HASH_SET_COPY);
        //返回1
        addReply(c, shared.cone);
        //key操作通知
        signalModifiedKey(c,c->db,c->argv[1]);
        //事件广播
        notifyKeyspaceEvent(NOTIFY_HASH,"hset",c->argv[1],c->db->id);
        server.dirty++;
    }
}

/* hset 客户端设置指令 */
void hsetCommand(client *c) {
    int i, created = 0;
    robj *o;

    if ((c->argc % 2) == 1) {//判别当前输入命令行参数数量
        addReplyErrorFormat(c,"wrong number of arguments for '%s' command",c->cmd->name);
        return;
    }

    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;//写入或者创建key
    hashTypeTryConversion(o,c->argv,2,c->argc-1);//判别当前的value是否超过了ziplist限制，超过了直接进行类型转换

    for (i = 2; i < c->argc; i += 2)
        created += !hashTypeSet(o,c->argv[i]->ptr,c->argv[i+1]->ptr,HASH_SET_COPY);//哈希设置键值对

    /* HMSET (deprecated) and HSET return value is different. */
    char *cmdname = c->argv[0]->ptr;
    if (cmdname[1] == 's' || cmdname[1] == 'S') {
        /* HSET */
        addReplyLongLong(c, created);//返回添加成功数量
    } else {
        /* HMSET */
        addReply(c, shared.ok);//返回ok
    }
    signalModifiedKey(c,c->db,c->argv[1]);//key操作事件
    notifyKeyspaceEvent(NOTIFY_HASH,"hset",c->argv[1],c->db->id);//广播事件
    server.dirty++;
}

/** hincrby 客户端指令 */
void hincrbyCommand(client *c) {
    long long value, incr, oldvalue;
    robj *o;
    sds new;
    unsigned char *vstr;
    unsigned int vlen;

    if (getLongLongFromObjectOrReply(c,c->argv[3],&incr,NULL) != C_OK) return;//数据转化
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;//写入或者创建key
    if (hashTypeGetValue(o,c->argv[2]->ptr,&vstr,&vlen,&value) == C_OK) {//当前value存在与否
        if (vstr) {
            if (string2ll((char*)vstr,vlen,&value) == 0) {//类型判别
                addReplyError(c,"hash value is not an integer");
                return;
            }
        } /* Else hashTypeGetValue() already stored it into &value */
    } else {
        value = 0;
    }

    oldvalue = value;
    //上下限判别
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }
    value += incr;//数据变化
    new = sdsfromlonglong(value);//转化
    hashTypeSet(o,c->argv[2]->ptr,new,HASH_SET_TAKE_VALUE);//设置
    addReplyLongLong(c,value);//响应处理
    signalModifiedKey(c,c->db,c->argv[1]);//key操作
    notifyKeyspaceEvent(NOTIFY_HASH,"hincrby",c->argv[1],c->db->id);//事件广播
    server.dirty++;
}

/* hincrbyfloat客户端命令 */
void hincrbyfloatCommand(client *c) {
    long double value, incr;
    long long ll;
    robj *o;
    sds new;
    unsigned char *vstr;
    unsigned int vlen;

    if (getLongDoubleFromObjectOrReply(c,c->argv[3],&incr,NULL) != C_OK) return;//参数转化
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;//写入或者创建key
    if (hashTypeGetValue(o,c->argv[2]->ptr,&vstr,&vlen,&ll) == C_OK) {//value获取
        if (vstr) {
            if (string2ld((char*)vstr,vlen,&value) == 0) {//value类别判别
                addReplyError(c,"hash value is not a float");
                return;
            }
        } else {
            value = (long double)ll;
        }
    } else {
        value = 0;
    }

    value += incr;
    if (isnan(value) || isinf(value)) {
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }

    char buf[MAX_LONG_DOUBLE_CHARS];
    int len = ld2string(buf,sizeof(buf),value,LD_STR_HUMAN);
    new = sdsnewlen(buf,len);//类型转化
    hashTypeSet(o,c->argv[2]->ptr,new,HASH_SET_TAKE_VALUE);//value重新设置
    addReplyBulkCBuffer(c,buf,len);
    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_HASH,"hincrbyfloat",c->argv[1],c->db->id);
    server.dirty++;

    /* Always replicate HINCRBYFLOAT as an HSET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
    /* 命令转化后在扩散，防止在AOF中因为扩散产生的京都不一致问题 */
    robj *aux, *newobj;
    aux = createStringObject("HSET",4);
    newobj = createRawStringObject(buf,len);
    rewriteClientCommandArgument(c,0,aux);
    decrRefCount(aux);
    rewriteClientCommandArgument(c,3,newobj);
    decrRefCount(newobj);
}

/*
 * 通过判断哈希对象的编码来决定使用什么函数来获取哈希对象具体field的值
 * 主要用于hget与hmget底层操作
 * */
static void addHashFieldToReply(client *c, robj *o, sds field) {
    int ret;

    if (o == NULL) {
        addReplyNull(c);
        return;
    }

    if (o->encoding == OBJ_ENCODING_ZIPLIST) {//压缩列表
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        ret = hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll);//根据field获取相对应的value
        if (ret < 0) {  //不存在
            addReplyNull(c);
        } else {
            if (vstr) {//为string类型
                addReplyBulkCBuffer(c, vstr, vlen);
            } else {//为longlong类型
                addReplyBulkLongLong(c, vll);
            }
        }

    } else if (o->encoding == OBJ_ENCODING_HT) {//哈希表
        sds value = hashTypeGetFromHashTable(o, field);//获取value
        if (value == NULL)
            addReplyNull(c);
        else
            addReplyBulkCBuffer(c, value, sdslen(value));//标准返回
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/* hget 客户端命令 */
void hgetCommand(client *c) {
    robj *o;

    //数据库检测 || 类型检测
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp])) == NULL ||
        checkType(c,o,OBJ_HASH)) return;

    //value获取
    addHashFieldToReply(c, o, c->argv[2]->ptr);
}

/* hmget客户端命令 */
void hmgetCommand(client *c) {
    robj *o;
    int i;

    /* Don't abort when the key cannot be found. Non-existing keys are empty
     * hashes, where HMGET should respond with a series of null bulks. */
    /*
     * 当找不到密钥时，请不要中止。 不存在的键是空散列，其中HMGET应该以一系列空散列作为响应。
     * */
    o = lookupKeyRead(c->db, c->argv[1]);
    if (o != NULL && o->type != OBJ_HASH) {
        addReply(c, shared.wrongtypeerr);
        return;
    }

    addReplyArrayLen(c, c->argc-2);
    for (i = 2; i < c->argc; i++) {//循环去取相对应的value
        addHashFieldToReply(c, o, c->argv[i]->ptr);
    }
}

/* hdel客户端 */
void hdelCommand(client *c) {
    robj *o;
    int j, deleted = 0, keyremoved = 0;

    //数据库查找 || 类型检测
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;

    for (j = 2; j < c->argc; j++) {
        if (hashTypeDelete(o,c->argv[j]->ptr)) {//循环哈希删除
            deleted++;
            if (hashTypeLength(o) == 0) {
                dbDelete(c->db,c->argv[1]);//数据库字典key移除
                keyremoved = 1;
                break;
            }
        }
    }
    if (deleted) {
        signalModifiedKey(c,c->db,c->argv[1]);//客户端缓存失效
        notifyKeyspaceEvent(NOTIFY_HASH,"hdel",c->argv[1],c->db->id);//键空间事件通知
        if (keyremoved) //key del事件通知
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],
                                c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c,deleted);//返回删除量
}

/** hlen 客户端命令 获取当前哈希长度 */
void hlenCommand(client *c) {
    robj *o;

    //数据库查找 || 类型检测
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;

    //获取哈希长度并响应
    addReplyLongLong(c,hashTypeLength(o));
}

/** hstrlen 获取指定feild对应的value长度 */
void hstrlenCommand(client *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;
    //获取当前结构题中field对应的value长度并做出响应
    addReplyLongLong(c,hashTypeGetValueLength(o,c->argv[2]->ptr));
}

/** 获取当前迭代器中的field 或者 value，取决于what */
static void addHashIteratorCursorToReply(client *c, hashTypeIterator *hi, int what) {
    if (hi->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        hashTypeCurrentFromZiplist(hi, what, &vstr, &vlen, &vll);
        if (vstr)
            addReplyBulkCBuffer(c, vstr, vlen);
        else
            addReplyBulkLongLong(c, vll);
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        sds value = hashTypeCurrentFromHashTable(hi, what);
        addReplyBulkCBuffer(c, value, sdslen(value));
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/** 哈希表的底层方法 */
void genericHgetallCommand(client *c, int flags) {
    robj *o;
    hashTypeIterator *hi;
    int length, count = 0;

    robj *emptyResp = (flags & OBJ_HASH_KEY && flags & OBJ_HASH_VALUE) ?
        shared.emptymap[c->resp] : shared.emptyarray;
    //key检测存在 || 类型检测
    if ((o = lookupKeyReadOrReply(c,c->argv[1],emptyResp))
        == NULL || checkType(c,o,OBJ_HASH)) return;

    /* We return a map if the user requested keys and values, like in the
     * HGETALL case. Otherwise to use a flat array makes more sense. */
    length = hashTypeLength(o);//获取元素个数
    if (flags & OBJ_HASH_KEY && flags & OBJ_HASH_VALUE) {
        addReplyMapLen(c, length);
    } else {
        addReplyArrayLen(c, length);
    }

    hi = hashTypeInitIterator(o);//获取哈希迭代器
    while (hashTypeNext(hi) != C_ERR) {//迭代
        if (flags & OBJ_HASH_KEY) {
            addHashIteratorCursorToReply(c, hi, OBJ_HASH_KEY);
            count++;
        }
        if (flags & OBJ_HASH_VALUE) {
            addHashIteratorCursorToReply(c, hi, OBJ_HASH_VALUE);
            count++;
        }
    }

    hashTypeReleaseIterator(hi);//释放迭代器

    /* Make sure we returned the right number of elements. */
    if (flags & OBJ_HASH_KEY && flags & OBJ_HASH_VALUE) count /= 2;
    serverAssert(count == length);
}

/**
 * hkeys 客户端指令
 * 返回哈希表 key 中的所有域。
 * */
void hkeysCommand(client *c) {
    genericHgetallCommand(c,OBJ_HASH_KEY);
}

/**
 * hvals 客户端指令
 * 返回哈希表 key 中的所有值。
 * */
void hvalsCommand(client *c) {
    genericHgetallCommand(c,OBJ_HASH_VALUE);
}

/**
 * HGETALL 客户端指令
 * 返回哈希表 key 中，所有的域和值。
 * */
void hgetallCommand(client *c) {
    genericHgetallCommand(c,OBJ_HASH_KEY|OBJ_HASH_VALUE);
}

/**
 * hexists 客户端指令
 * 检查给定域 field 是否存在于哈希表 hash 当中。
 * */
void hexistsCommand(client *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;

    addReply(c, hashTypeExists(o,c->argv[2]->ptr) ? shared.cone : shared.czero);
}

/**
 * hscan 客户端指令
 *
 * */
void hscanCommand(client *c) {
    robj *o;
    unsigned long cursor;

    if (parseScanCursorOrReply(c,c->argv[2],&cursor) == C_ERR) return;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptyscan)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;
    scanGenericCommand(c,o,cursor);
}
