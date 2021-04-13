/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdint.h>

#ifndef __DICT_H
#define __DICT_H

#define DICT_OK 0
#define DICT_ERR 1

/* Unused arguments generate annoying warnings... */
#define DICT_NOTUSED(V) ((void) V)

/* 保存key-value对的结构体 */
typedef struct dictEntry {
    void *key;          //字典键
    union {             //value是一个集合
        void *val;      //空类型指针一枚
        uint64_t u64;   //无符号整型一枚
        int64_t s64;    //有符号整型一枚
        double d;       //双精度浮点数一枚
    } v;
    struct dictEntry *next;//指向下一个键值对节点的指针
} dictEntry;

/* 字典操作的方法 */
typedef struct dictType {
    uint64_t (*hashFunction)(const void *key);                          //哈希函数指针，使用key来计算哈希值
    void *(*keyDup)(void *privdata, const void *key);                   //复制key的函数指针
    void *(*valDup)(void *privdata, const void *obj);                   //复制value的函数指针
    int (*keyCompare)(void *privdata, const void *key1, const void *key2);// 比较两个key的函数指针
    void (*keyDestructor)(void *privdata, void *key);                   //销毁key的函数指针
    void (*valDestructor)(void *privdata, void *obj);                   //销毁value的函数指针
} dictType;

/* This is our hash table structure. Every dictionary has two of this as we
 * implement incremental rehashing, for the old to the new table. */
/* 这是我们的哈希表结构。每个字典都有两个这样的结构，因为
 * 我们实现了从旧哈希表迁移数据到新哈希表的增量rehash。
*/
/* 哈希表结构 */
typedef struct dictht {
    dictEntry **table;      //散列数组
    unsigned long size;     //散列数组长度
    unsigned long sizemask; //散列数组长度掩码 = 散列数组长度-1
    unsigned long used;     //散列数组中已经被使用的节点数量
} dictht;

/* 字典结构 */
typedef struct dict {
    dictType *type;     //字典类型
    void *privdata;     //私有数据
    dictht ht[2];       //一个字典中有两个哈希表，原因如上述
    long rehashidx; /* rehashing not in progress if rehashidx == -1 */      // 数据rehash的当前索引位置
    unsigned long iterators; /* number of iterators currently running */    // 当前使用的迭代器数量
} dict;

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
/* 如果safe被设置成1则表示这是一个安全的迭代器，这意味着你可以在迭代字典时调用
 * dictAdd、dictFind等一些函数。否则这是一个不安全的迭代器，只能在迭代时调用
 * dictNext()函数。
*/
/* 字典迭代器 */
typedef struct dictIterator {
    dict *d;                // 字典指针
    long index;             // 散列数组的当前索引值
    int table, safe;        //哈希表编号（0／1）和安全标志
    dictEntry *entry, *nextEntry;   //当前键值对结构体指针，下一个键值对结构体指针
    /* unsafe iterator fingerprint for misuse detection. */
    long long fingerprint;          //字典的指纹
} dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de);
typedef void (dictScanBucketFunction)(void *privdata, dictEntry **bucketref);

/* This is the initial size of every hash table */
//散列数组的初始大小
#define DICT_HT_INITIAL_SIZE     4

/* ------------------------------- Macros ------------------------------------*/
#define dictFreeVal(d, entry) \
    if ((d)->type->valDestructor) \
        (d)->type->valDestructor((d)->privdata, (entry)->v.val)

#define dictSetVal(d, entry, _val_) do { \
    if ((d)->type->valDup) \
        (entry)->v.val = (d)->type->valDup((d)->privdata, _val_); \
    else \
        (entry)->v.val = (_val_); \
} while(0)

#define dictSetSignedIntegerVal(entry, _val_) \
    do { (entry)->v.s64 = _val_; } while(0)

#define dictSetUnsignedIntegerVal(entry, _val_) \
    do { (entry)->v.u64 = _val_; } while(0)

#define dictSetDoubleVal(entry, _val_) \
    do { (entry)->v.d = _val_; } while(0)

#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d)->privdata, (entry)->key)

#define dictSetKey(d, entry, _key_) do { \
    if ((d)->type->keyDup) \
        (entry)->key = (d)->type->keyDup((d)->privdata, _key_); \
    else \
        (entry)->key = (_key_); \
} while(0)

#define dictCompareKeys(d, key1, key2) \
    (((d)->type->keyCompare) ? \
        (d)->type->keyCompare((d)->privdata, key1, key2) : \
        (key1) == (key2))

#define dictHashKey(d, key) (d)->type->hashFunction(key)    //获取指定key的哈希值
#define dictGetKey(he) ((he)->key)                          //获取指定节点的key
#define dictGetVal(he) ((he)->v.val)                        //获取指定节点的value
#define dictGetSignedIntegerVal(he) ((he)->v.s64)           //获取指定节点的value，值为signed int
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)         //获取指定节点的value，值为unsigned int
#define dictGetDoubleVal(he) ((he)->v.d)                    //获取指定节点的value，值为double
#define dictSlots(d) ((d)->ht[0].size+(d)->ht[1].size)      //获取字典中哈希表的总长度，总长度=哈希表1散列数组长度+哈希表2散列数组长度
#define dictSize(d) ((d)->ht[0].used+(d)->ht[1].used)       //获取字典中哈希表已被使用的节点数量，已被使用的节点数量=哈希表1散列数组已被使用的节点数量+哈希表2散列数组已被使用的节点数量
#define dictIsRehashing(d) ((d)->rehashidx != -1)           //字典当前是否正在进行rehash操作

/* API */
dict *dictCreate(dictType *type, void *privDataPtr);        //创建一个字典
int dictExpand(dict *d, unsigned long size);                //字典扩容
int dictAdd(dict *d, void *key, void *val);                 //向字典中添加键值对
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing);//向字典中添加一个key，当key已经存在时，将该节点赋予existing
dictEntry *dictAddOrFind(dict *d, void *key);               //向字典中添加一个key，并返回所对应的字典，内部调用dictAddRaw
int dictReplace(dict *d, void *key, void *val);             //设置/替换指定key的value（key不存在就设置key-value，存在则替换value）
int dictDelete(dict *d, const void *key);                   //根据key删除字典中的一个key-value对
dictEntry *dictUnlink(dict *ht, const void *key);           //根据key删除字典中的一个key-value对，但并不释放相应的key和value
void dictFreeUnlinkedEntry(dict *d, dictEntry *he);         //释放字典中的一个key-value对
void dictRelease(dict *d);                                  //释放一个字典
dictEntry * dictFind(dict *d, const void *key);             //根据key在字典中查找一个key-value对
void *dictFetchValue(dict *d, const void *key);             //根据key从字典中获取它对应的value
int dictResize(dict *d);                                    //重新计算并设置字典的哈希数组大小，调整到能包含所有元素的最小大小
dictIterator *dictGetIterator(dict *d);                     //获取一个字典的普通（非安全）迭代器
dictIterator *dictGetSafeIterator(dict *d);                 //获取一个字典的安全迭代器
dictEntry *dictNext(dictIterator *iter);                    //获取迭代器的下一个key-value对
void dictReleaseIterator(dictIterator *iter);               //释放字典迭代器
dictEntry *dictGetRandomKey(dict *d);                       //随机获取字典中的一个key-value对
dictEntry *dictGetFairRandomKey(dict *d);                   //加权获取字典中的一个key-value对
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);//从字典中随机取样count个key-value对
void dictGetStats(char *buf, size_t bufsize, dict *d);      //获取字典状态
uint64_t dictGenHashFunction(const void *key, int len);     //一种哈希算法
uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len);//对大小写不敏感的哈希算法
void dictEmpty(dict *d, void(callback)(void*));             //清空字典数据并调用回调函数
void dictEnableResize(void);                                //开启字典resize
void dictDisableResize(void);                               //禁用字典resize
int dictRehash(dict *d, int n);                             //字典rehash
int dictRehashMilliseconds(dict *d, int ms);                //在ms时间内rehash，超过则停止
void dictSetHashFunctionSeed(uint8_t *seed);                //设置rehash函数种子
uint8_t *dictGetHashFunctionSeed(void);                     //获取rehash函数种子
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, dictScanBucketFunction *bucketfn, void *privdata);//遍历整个字典，每次访问一个元素都会调用fn操作其数据
uint64_t dictGetHash(dict *d, const void *key);             //获取当前字典指定key的哈希值
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash);   //通过使用指针和预先计算的哈希查找dictEntry引用

/* Hash table types */
/* 哈希表类型 */
extern dictType dictTypeHeapStringCopyKey;
extern dictType dictTypeHeapStrings;
extern dictType dictTypeHeapStringCopyKeyValue;

#endif /* __DICT_H */
