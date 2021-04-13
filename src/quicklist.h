/* quicklist.h - A generic doubly linked quicklist implementation
 *
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this quicklist of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this quicklist of conditions and the following disclaimer in the
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

#include <stdint.h> // for UINTPTR_MAX

#ifndef __QUICKLIST_H__
#define __QUICKLIST_H__

/* Node, quicklist, and Iterator are the only data structures used currently. */

/* quicklistNode is a 32 byte struct describing a ziplist for a quicklist.
 * We use bit fields keep the quicklistNode at 32 bytes.
 * count: 16 bits, max 65536 (max zl bytes is 65k, so max count actually < 32k).
 * encoding: 2 bits, RAW=1, LZF=2.
 * container: 2 bits, NONE=1, ZIPLIST=2.
 * recompress: 1 bit, bool, true if node is temporary decompressed for usage.
 * attempted_compress: 1 bit, boolean, used for verifying during testing.
 * extra: 10 bits, free for future use; pads out the remainder of 32 bits */
/**
 * 快速列表节点
 * quicklistNode是一个32字节的结构，用于描述快速列表的ziplist。
 * 我们使用位字段将quicklistNode保持为32个字节。
 * */
typedef struct quicklistNode {
    struct quicklistNode *prev;     /* 指向上一个节点 */
    struct quicklistNode *next;     /* 指向下一个节点 */
    unsigned char *zl;              /* 当节点保存的是压缩 ziplist 时，指向 quicklistLZF，否则指向 ziplist */
    unsigned int sz;                /* ziplist 的大小（字节为单位） */
    unsigned int count : 16;        /* 表示在 ziplist 中 zlentry 个数(zllen) */
    unsigned int encoding : 2;      /* 表示所包含的 ziplist 是否被压缩，值为 1 表示未被压缩，值为 2 表示已使用 LZF 压缩算法压缩 */
    unsigned int container : 2;     /* 表示 quicklistNode 所包装的数据类型，目前值固定为 2，表示包装的数据类型为 ziplist */
    unsigned int recompress : 1;    /* 当我们访问 ziplist 时，需要解压数据，该参数表示 ziplist 是否被临时解压 */
    //测试时使用
    unsigned int attempted_compress : 1; /* 节点太小，不压缩 */
    //额外扩展位，占10bits长度
    unsigned int extra : 10;        /* 未使用，保留字段 */
} quicklistNode;

/* quicklistLZF is a 4+N byte struct holding 'sz' followed by 'compressed'.
 * 'sz' is byte length of 'compressed' field.
 * 'compressed' is LZF data with total (compressed) length 'sz'
 * NOTE: uncompressed length is stored in quicklistNode->sz.
 * When quicklistNode->zl is compressed, node->zl points to a quicklistLZF */
/** 压缩列表 */
typedef struct quicklistLZF {
    unsigned int sz; /* 表示压缩后的ziplist大小*/
    char compressed[];  /* 是个柔性数组（flexible array member），存放压缩后的ziplist字节数组。 */
} quicklistLZF;

/* Bookmarks are padded with realloc at the end of of the quicklist struct.
 * They should only be used for very big lists if thousands of nodes were the
 * excess memory usage is negligible, and there's a real need to iterate on them
 * in portions.
 * When not used, they don't add any memory overhead, but when used and then
 * deleted, some overhead remains (to avoid resonance).
 * The number of bookmarks used should be kept to minimum since it also adds
 * overhead on node deletion (searching for a bookmark to update). */
/**
 * quicklist 中最后一个 field 为一个柔性数组成员 bookmarks，bookmark_count 保存该数组的长度。
 * 当 quicklist 长度特别长，需要迭代遍历时，会使用到该数组作为缓存。
 * 该数组的长度应保持在较小值，以供高效查找更新
 * */
typedef struct quicklistBookmark {
    quicklistNode *node;
    char *name;
} quicklistBookmark;

#if UINTPTR_MAX == 0xffffffff
/* 32-bit */
#   define QL_FILL_BITS 14
#   define QL_COMP_BITS 14
#   define QL_BM_BITS 4
#elif UINTPTR_MAX == 0xffffffffffffffff
/* 64-bit */
#   define QL_FILL_BITS 16
#   define QL_COMP_BITS 16
#   define QL_BM_BITS 4 /* we can encode more, but we rather limit the user
                           since they cause performance degradation. */
#else
#   error unknown arch bits count
#endif

/* quicklist is a 40 byte struct (on 64-bit systems) describing a quicklist.
 * 'count' is the number of total entries.
 * 'len' is the number of quicklist nodes.
 * 'compress' is: 0 if compression disabled, otherwise it's the number
 *                of quicklistNodes to leave uncompressed at ends of quicklist.
 * 'fill' is the user-requested (or default) fill factor.
 * 'bookmakrs are an optional feature that is used by realloc this struct,
 *      so that they don't consume memory when not used. */
/**
 * 快速链表结构体
 * fill: 其用于限制该快速链表中每个 quicklistNode 中 ziplist 的长度
 *      - 从 ziplist 那一章节中我们知道 ziplist 每次更改操作耗时和其长度成正比，因此 ziplist 不应过长，但是如果其过短的话就无法达到节省内存空间的效果，特别地，当 fill = 1 时，quicklist 退化成普通的双向链表
 *      - 可在 redis.conf 中通过设置 list-max-ziplist-size 进行配置
 *          其值为正数时，表示的是每个 ziplist 中 zlentry 个数(zllen)上限
 *          其值为负数时，表示的是每个 ziplist 所用字节总数(zlbytes)上限
 * compress: 链表的读写通常情况下集中在两端，因此在 quicklist 中，可能会对中间的 quicklistNode 节点使用无损数据压缩算法 LZF 对其包含的 ziplist 进行压缩以进一步节省内存使用。
 * compress 的大小含义如下：
 *        - 0: 表示不进行压缩
 *        - Z+: 表示首尾 compress 个 quicklistNode 不会被压缩
 *   可以看出，head 和 tail 节点永远不会被压缩
 * */
typedef struct quicklist {
    quicklistNode *head;        /* 快速链表首节点指针 */
    quicklistNode *tail;        /* 快速链表尾节点指针 */
    unsigned long count;        /* 该快速链表中所有 ziplist 的 zlentry 总数 */
    unsigned long len;          /* 该快速链表中 quicklistNode 个数 */
    int fill : QL_FILL_BITS;              /* 保存ziplist的大小，配置文件设定，占16bits */
    unsigned int compress : QL_COMP_BITS; /* 保存压缩程度值，配置文件设定，占16bits，0表示不压缩  */
    unsigned int bookmark_count: QL_BM_BITS;
    quicklistBookmark bookmarks[];
} quicklist;

/** quicklist迭代器 */
typedef struct quicklistIter {
    const quicklist *quicklist;     //当前迭代的快速链表
    quicklistNode *current;         //当前迭代的 quicklistNode
    unsigned char *zi;              //指向当前quicklist节点中迭代的ziplist
    long offset;                    //当前访问的 zlentry 在 current->zl 中的偏移量  /* offset in current ziplist */
    int direction;                  //迭代方向
} quicklistIter;

/**
 * 用于描述 quicklist 中的每个 zlentry 的状态。
 * 当 zlentry 的数据类型为字符串时，value 和 sz 保存了 zlentry 的值
 * 当 zlentry 的数据类型为整型时，longval 保存了 zlentry 的值
 * */
typedef struct quicklistEntry {
    const quicklist *quicklist;     //指向该 zlentry 所在的快速链表
    quicklistNode *node;            //指向该 zlentry 所在的 quicklistNode
    unsigned char *zi;              //指向该 zlentry
    unsigned char *value;
    long long longval;
    unsigned int sz;
    int offset;                     //该 zlentry 在 node->zl 中的偏移量
} quicklistEntry;

#define QUICKLIST_HEAD 0
#define QUICKLIST_TAIL -1

/* quicklist node encodings */
#define QUICKLIST_NODE_ENCODING_RAW 1   //没有被压缩
#define QUICKLIST_NODE_ENCODING_LZF 2   //被LZF算法压缩

/* quicklist compression disable */
#define QUICKLIST_NOCOMPRESS 0

/* quicklist container formats */
#define QUICKLIST_NODE_CONTAINER_NONE 1     //quicklist节点直接保存对象
#define QUICKLIST_NODE_CONTAINER_ZIPLIST 2  //quicklist节点采用ziplist保存对象

//测试quicklist节点是否被压缩，返回1 表示被压缩，否则返回0
#define quicklistNodeIsCompressed(node)                                        \
    ((node)->encoding == QUICKLIST_NODE_ENCODING_LZF)

/* Prototypes */
quicklist *quicklistCreate(void);                                   //创建一个新的quicklist，并初始化成员
quicklist *quicklistNew(int fill, int compress);                    //创建一个quicklist，并且设置默认的参数
void quicklistSetCompressDepth(quicklist *quicklist, int depth);    //设置压缩程度
void quicklistSetFill(quicklist *quicklist, int fill);              //设置ziplist结构的大小
void quicklistSetOptions(quicklist *quicklist, int fill, int depth);//设置压缩列表表头的fill和compress成员
void quicklistRelease(quicklist *quicklist);                        //释放整个quicklist
int quicklistPushHead(quicklist *quicklist, void *value, const size_t sz);  //push一个entry节点到quicklist的头部
int quicklistPushTail(quicklist *quicklist, void *value, const size_t sz);  //push一个entry节点到quicklist的尾部
void quicklistPush(quicklist *quicklist, void *value, const size_t sz,
                   int where);                                              //将push函数封装起来，通过where 表示push头部或push尾部
void quicklistAppendZiplist(quicklist *quicklist, unsigned char *zl);       //追加quicklist一个quicklist节点
quicklist *quicklistAppendValuesFromZiplist(quicklist *quicklist,
                                            unsigned char *zl);             //在quicklist末尾追加一个entry
quicklist *quicklistCreateFromZiplist(int fill, int compress,
                                      unsigned char *zl);                   //创建一个quicklist，并将zl追加到其中
void quicklistInsertAfter(quicklist *quicklist, quicklistEntry *node,
                          void *value, const size_t sz);                    //将entry后插入封装起来
void quicklistInsertBefore(quicklist *quicklist, quicklistEntry *node,
                           void *value, const size_t sz);                   //将entry前插入封装起来
void quicklistDelEntry(quicklistIter *iter, quicklistEntry *entry);         //删除一个entry通过quicklistEntry结构的形式
int quicklistReplaceAtIndex(quicklist *quicklist, long index, void *data,
                            int sz);                                        //下标为index的entry被data替换
int quicklistDelRange(quicklist *quicklist, const long start, const long stop);//删除一个范围内的entry节点，返回1表示全部被删除，返回0表示什么都没有删除
quicklistIter *quicklistGetIterator(const quicklist *quicklist, int direction);//返回当前节点的迭代器的地址
quicklistIter *quicklistGetIteratorAtIdx(const quicklist *quicklist,
                                         int direction, const long long idx);   //将迭代器和一个quicklistNode结合
int quicklistNext(quicklistIter *iter, quicklistEntry *node);               //将迭代器当前指向的节点的信息读到quicklistEntry结构中，并且指向下一个节点
void quicklistReleaseIterator(quicklistIter *iter);                         //释放迭代器
quicklist *quicklistDup(quicklist *orig);                                   //复制一个quicklist，并返回
int quicklistIndex(const quicklist *quicklist, const long long index,
                   quicklistEntry *entry);                                  //查找下标为idx的entry，返回1 表示找到，0表示没找到
void quicklistRewind(quicklist *quicklist, quicklistIter *li);              //
void quicklistRewindTail(quicklist *quicklist, quicklistIter *li);          //
void quicklistRotate(quicklist *quicklist);                                 //将尾quicklistNode节点的尾entry节点旋转到头quicklistNode节点的头部
int quicklistPopCustom(quicklist *quicklist, int where, unsigned char **data,
                       unsigned int *sz, long long *sval,
                       void *(*saver)(unsigned char *data, unsigned int sz));//从quicklist的头节点或尾节点pop弹出出一个entry，并将value保存在传入传出参数
int quicklistPop(quicklist *quicklist, int where, unsigned char **data,
                 unsigned int *sz, long long *slong);                       //pop一个entry值，调用quicklistPopCustom，封装起来
unsigned long quicklistCount(const quicklist *ql);                          //返回ziplist中entry节点的个数
int quicklistCompare(unsigned char *p1, unsigned char *p2, int p2_len);     //将ziplist的比较两个ziplist的函数封装成quicklistCompare
size_t quicklistGetLzf(const quicklistNode *node, void **data);             //返回压缩过的ziplist结构的大小，并且将压缩过后的ziplist地址保存到*data中

/* bookmarks */
int quicklistBookmarkCreate(quicklist **ql_ref, const char *name, quicklistNode *node);//在列表中创建或更新bookmarks，当引用的书签被删除时，bookmarks将自动更新到下一个节点。
int quicklistBookmarkDelete(quicklist *ql, const char *name);               //删除一个已命名的bookmarks。
quicklistNode *quicklistBookmarkFind(quicklist *ql, const char *name);      //查找到一个指定的bookmarks。
void quicklistBookmarksClear(quicklist *ql);                                //释放快速列表中的bookmarks。

#ifdef REDIS_TEST
int quicklistTest(int argc, char *argv[]);
#endif

/* Directions for iterators */
#define AL_START_HEAD 0
#define AL_START_TAIL 1

#endif /* __QUICKLIST_H__ */
