/* String -> String Map data structure optimized for size.
 * This file implements a data structure mapping strings to other strings
 * implementing an O(n) lookup data structure designed to be very memory
 * efficient.
 *
 * The Redis Hash type uses this data structure for hashes composed of a small
 * number of elements, to switch to a hash table once a given number of
 * elements is reached.
 *
 * Given that many times Redis Hashes are used to represent objects composed
 * of few fields, this is a very big win in terms of used memory.
 *
 * --------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

/*
 *
 * zipmap是一个空间效率非常高的数据结构，它的key查找时间复杂度是O(n)。
 *
 * 举个例子，一个zipmap，其中的key-value映射关系为：”foo” => “bar”, “hello” => “world”，它的内存布局如下：
 *
 * \<zmlen>\<len>“foo”\<len>\<free>“bar”\<len>“hello”\<len>\<free>“world”
 *
 * 一个zipmap由下面几个部分组成：
 *
 * \<zmlen>：保存zipmap当前的元素数量，占用1字节。当zipmap中元素数量大于或等于254时，这个字段不再有效，取而代之的是我们需要遍历整个zipmap来计算它的元素数量。
 *
 * \<len>：保存了它后面的字符串(key或value)的长度。\<len>的长度是1字节或5字节。如果它的第一个字节的值在0~253之间，它是的值就是这个字节的大小。如果第一个字节是254，则它的大小是后面四个字节表示的值。单字节值为255则表示zipmap的结束。
 *
 * \<free>：表示字符串后面未被使用的空闲字节，修改一个key的value会产生空闲字节。比如先把key “foo”的值设为”bar”，然后再把key “foo”的值设为”hi”，就会产生一个空闲字节。\<free>总是一个8 bit的无符号数，因为如果一个更新操作产生了很多空闲字节，zipmap将重新对这个字符串分配内存以确保空间的紧凑性。
 *
 * 以上两个元素在哈希表中最紧凑的表达方式实际上是：
 *
 * “\x02\x03foo\x03\x00bar\x05hello\x05\x00world\xff”
 *
 * 需要注意的是，由于key和value都有一个长度前缀，因此在zipmap中查找一个key的时间复杂度是O(N)，其中N为zipmap中元素的数量，而不是zipmap所占用的字节数。这将大大降低key查找的开销。
 */

#include <stdio.h>
#include <string.h>
#include "zmalloc.h"
#include "endianconv.h"

#define ZIPMAP_BIGLEN 254// zipmap最大的节点数量
#define ZIPMAP_END 255// zipmap尾部标志

/* 表示<free>字段的最大值，该字段表示value后面的空闲字节数，<free>占用1字节，
 * 当用1字节无法表示时，zipmap会重新分配内存，以保证字符串尽量紧凑。 */
#define ZIPMAP_VALUE_MAX_FREE 4

/* 以下的宏返回编码整数_l的长度需要的字节数，当长度小于ZIPMAP_BIGLEN时返回1，其他值返回5。 */
#define ZIPMAP_LEN_BYTES(_l) (((_l) < ZIPMAP_BIGLEN) ? 1 : sizeof(unsigned int)+1)

/* 创建一个空的zipmap。*/
unsigned char *zipmapNew(void) {
    unsigned char *zm = zmalloc(2);// 为zipmap分配空间

    // 空的zipmap只需要zmlen和end两个字节
    zm[0] = 0; /* Length */
    zm[1] = ZIPMAP_END;
    return zm;
}

/* 返回p指向的数据的长度。 */
static unsigned int zipmapDecodeLength(unsigned char *p) {
    unsigned int len = *p;

    if (len < ZIPMAP_BIGLEN) return len;// 如果长度小于ZIPMAP_BIGLEN，编码该长度只需要1字节，直接返回该长度
    memcpy(&len,p+1,sizeof(unsigned int));// 否则编码该长度需要5字节
    memrev32ifbe(&len);
    return len;
}

/* 计算编码len长度需要的字节数，并保存在p中。如果p为NULL，直接返回这个字节数。 */
static unsigned int zipmapEncodeLength(unsigned char *p, unsigned int len) {
    if (p == NULL) {
        return ZIPMAP_LEN_BYTES(len);// p为NULL时直接返回编码len所需的字节数
    } else {
        if (len < ZIPMAP_BIGLEN) {// len小于ZIPMAP_BIGLEN时，编码只需要1字节
            p[0] = len;
            return 1;
        } else {// len大于或等于ZIPMAP_BIGLEN时，编码需要5字节
            p[0] = ZIPMAP_BIGLEN;
            memcpy(p+1,&len,sizeof(len));
            memrev32ifbe(p+1);
            return 1+sizeof(len);
        }
    }
}

/*
 * 在zipmap中查找匹配的key，找到就返回该节点的指针，否则返回NULL。
 * 如果没有找到（返回NULL）且totlen不为NULL，就把totlen设置为zipmap占用的字节数，
 * 这样调用者就可以对原zipmap进行realloc使得它可以容纳更多元素。
 * */
static unsigned char *zipmapLookupRaw(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned int *totlen) {
    // p: 当前节点指针，k: 匹配到的节点指针
    unsigned char *p = zm+1, *k = NULL;
    unsigned int l,llen;

    while(*p != ZIPMAP_END) {// 遍历整个zipmap
        unsigned char free;

        /* Match or skip the key */
        l = zipmapDecodeLength(p);// 计算p指向节点的key的长度
        llen = zipmapEncodeLength(NULL,l);  // 编码l需要的字节数
        // p+llen是当前节点key的地址，l是当前节点的key的长度
        if (key != NULL && k == NULL && l == klen && !memcmp(p+llen,key,l)) {
            /* total不为NULL时，用户需要知道zipmap占用的字节数， 因此需要继续往下遍历，所以用k先保存匹配的节点的指针，p用于继续往下遍历。 */
            if (totlen != NULL) {
                k = p;
            } else {// total为NULL时，说明用户不关心zipmap占用的字节数，直接返回找到节点指针即可
                return p;
            }
        }
        p += llen+l;// llen+l = 编码当前节点key需要的字节数+key长度，更新以后p指向当前节点的value_len字段
        /* 跳过当前节点的value字段 */
        l = zipmapDecodeLength(p);// 计算p指向节点的value的长度
        p += zipmapEncodeLength(NULL,l);// 计算编码p指向节点的value的长度所需的字节数，并更新p指向当前节点的free
        free = p[0];
        p += l+1+free; //  跳过当前节点的free字段、free指明的空闲大小和value的长度
    }
    if (totlen != NULL) *totlen = (unsigned int)(p-zm)+1;// 赋值totlen
    return k;
}

/* 计算以klen为长度的key和以vlen为长度的value的节点需要的空间大小。 */
static unsigned long zipmapRequiredLength(unsigned int klen, unsigned int vlen) {
    unsigned int l;

    l = klen+vlen+3;  // 编码key_len需要的空间(最少1字节) + 编码value_len需要的空间(最少1字节) + free(1字节)
    if (klen >= ZIPMAP_BIGLEN) l += 4;  // key_len所能编码的大小超过ZIPMAP_BIGLEN时，需要扩容到5字节
    if (vlen >= ZIPMAP_BIGLEN) l += 4;  // value_len所能编码的大小超过ZIPMAP_BIGLEN时，需要扩容到5字节
    return l;
}

/* 返回指定节点的key占用的空间大小。 */
static unsigned int zipmapRawKeyLength(unsigned char *p) {
    unsigned int l = zipmapDecodeLength(p);  // key本身的长度
    return zipmapEncodeLength(NULL,l) + l;  // key占用的空间大小 = key本身的长度 + 编码key的长度所需的字节数
}

/* 返回value占用的总空间（value_len + free（本身和其表示的大小之和） + value）。 */
static unsigned int zipmapRawValueLength(unsigned char *p) {
    unsigned int l = zipmapDecodeLength(p);  // value本身的长度
    unsigned int used;  // value使用的空间大小

    used = zipmapEncodeLength(NULL,l);  // 编码value长度所需的字节数，即value_len
    used += p[used] + 1 + l;  // p[used]表示free字段中保存的value后的空闲空间大小，1表示free本身占用1字节，l为value长度
    return used;
}

/* 如果p指向一个key，此函数返回该节点占用的空间大小（节点占用空间 = key占用大小 + value占用大小 + 尾部空闲空间大小）。 */
static unsigned int zipmapRawEntryLength(unsigned char *p) {
    unsigned int l = zipmapRawKeyLength(p);// key占用的空间大小
    return l + zipmapRawValueLength(p+l);// value占用的空间大小（已经包括尾部空闲空间）
}

/* 调整zipmap空间大小，len是新的大小。 */
static inline unsigned char *zipmapResize(unsigned char *zm, unsigned int len) {
    zm = zrealloc(zm, len);
    zm[len-1] = ZIPMAP_END;
    return zm;
}

/* 对指定的key设置value，如果key不存在就创建key。如果update非空且key已经存在， *update被设置为1（意味着是更新key而不是创建key），否则为0。 */
unsigned char *zipmapSet(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned char *val, unsigned int vlen, int *update) {
    unsigned int zmlen, offset;
    // reqlen: key-value对占用的空间
    unsigned int freelen, reqlen = zipmapRequiredLength(klen,vlen);
    unsigned int empty, vempty;
    unsigned char *p;

    freelen = reqlen;
    if (update) *update = 0;
    p = zipmapLookupRaw(zm,key,klen,&zmlen); // 查找指定key，zmlen中保存了zipmap占用的空间大小
    if (p == NULL) {
        /* 没有找到key，扩大zipmap大小 */
        zm = zipmapResize(zm, zmlen+reqlen);
        p = zm+zmlen-1;
        zmlen = zmlen+reqlen;  // 新的zipmap大小 = 原zipmap大小 + 新key-value pair大小

        /* 增加zipmap节点数量 */
        if (zm[0] < ZIPMAP_BIGLEN) zm[0]++;
    } else {
        /* 找到key的节点，需要判断是否有足够空间存放新的value */
        if (update) *update = 1;
        freelen = zipmapRawEntryLength(p);  // 计算找到的节点的总长度
        if (freelen < reqlen) {
            offset = p-zm;  // 保存这个节点相对于zipmap首地址的偏移量
            zm = zipmapResize(zm, zmlen-freelen+reqlen);  // 扩容zipmap，增加reqlen-freelen大小的空间
            p = zm+offset;  // 恢复这个节点的指针

            /* 当前节点后面的节点地址为p+freelen，把它移动到新的位置(p+reqlen) */
            memmove(p+reqlen, p+freelen, zmlen-(offset+freelen+1));
            zmlen = zmlen-freelen+reqlen;  // 新的zipmap占用空间大小
            freelen = reqlen;
        }
    }

    /* 现在我们有足够的空间来容纳key-value pair了。此时如果空闲空间太多，
     * 需要把后面的节点前移，并且缩小zipmap的大小以让空间更加紧凑。 */
    empty = freelen-reqlen;
    if (empty >= ZIPMAP_VALUE_MAX_FREE) {  // 空闲空间大于预设值，freelen > reqlen
        /* 首先，把节点尾部的空闲字节 */
        offset = p-zm;  // 当前节点相对于zipmap的偏移量
        /* p+reqlen: 被更新节点的尾指针
         * p+freelen: 如果原zipmap中key不存在，则在此处reqlen=freelen，
         * 如果是更新key的value，执行到这里说明之前节点长度大于或等于reqlen，
         * 下面的memmove操作相当于把这个节点之后的所有数据前移freelen-reqlen个字节。
         */
        memmove(p+reqlen, p+freelen, zmlen-(offset+freelen+1));
        zmlen -= empty;  // 更新zipmap的长度（减去压缩的空闲空间大小）
        zm = zipmapResize(zm, zmlen);  // 调整zipmap大小
        p = zm+offset;  // 恢复p指向当前节点
        vempty = 0;  // 经过数据迁移调整以后，这个节点已经没有空闲空间了
    } else {
        vempty = empty;  // 这个节点的空闲空间大小
    }

    /* Key: */
    /* 设置key */
    p += zipmapEncodeLength(p,klen);  // 编码klen长度需要的字节数，p跳过这个长度
    memcpy(p,key,klen);  // 设置key
    p += klen;  // 跳过key的长度
    /* Value: */
    /* 设置Value */
    p += zipmapEncodeLength(p,vlen);  // 编码vlen长度需要的字节数，p跳过这个长度
    *p++ = vempty;  // 设置value，同事p跳过free的长度（1字节）
    memcpy(p,val,vlen);  // 设置value
    return zm;
}

/* 删除指定的key，如果deleted非空且没有找到指定key，则把*deleted设置为0，如果找到此key且成功删除则设为1。*/
unsigned char *zipmapDel(unsigned char *zm, unsigned char *key, unsigned int klen, int *deleted) {
    unsigned int zmlen, freelen;
    // 查找指定key，并获取zipmap占用的空间大小
    unsigned char *p = zipmapLookupRaw(zm,key,klen,&zmlen);
    if (p) {  // 找到该key
        freelen = zipmapRawEntryLength(p);  // 目标节点的长度
        memmove(p, p+freelen, zmlen-((p-zm)+freelen+1));  // 把目标节点后的所有数据迁移到目标节点的首地址，覆盖数据
        zm = zipmapResize(zm, zmlen-freelen);  // 调整zipmap大小

        /* Decrease zipmap length */
        if (zm[0] < ZIPMAP_BIGLEN) zm[0]--;  // 减少zipmap节点数量

        if (deleted) *deleted = 1;  // 删除了1个节点
    } else {
        if (deleted) *deleted = 0;  // 没有找到key
    }
    return zm;
}

/* 在使用zipmapNext()函数遍历zipmap之前调用，用于跳过zipmap开头1字节的zmlen。 */
unsigned char *zipmapRewind(unsigned char *zm) {
    return zm+1;
}

/* This function is used to iterate through all the zipmap elements.
 * In the first call the first argument is the pointer to the zipmap + 1.
 * In the next calls what zipmapNext returns is used as first argument.
 * Example:
 *
 * unsigned char *i = zipmapRewind(my_zipmap);
 * while((i = zipmapNext(i,&key,&klen,&value,&vlen)) != NULL) {
 *     printf("%d bytes key at $p\n", klen, key);
 *     printf("%d bytes value at $p\n", vlen, value);
 * }
 */
/* 遍历整个zipmap的所有元素。一次调用时，第一个参数指向的是zipmap+1。接下来的所有调用的返回值会作为下一次调用时的第一个参数。例子： */
unsigned char *zipmapNext(unsigned char *zm, unsigned char **key, unsigned int *klen, unsigned char **value, unsigned int *vlen) {
    if (zm[0] == ZIPMAP_END) return NULL;  // 已经到zipmap尾部，结束遍历，返回NULL
    if (key) {
        *key = zm;
        *klen = zipmapDecodeLength(zm);  // 获取当前节点key的长度
        *key += ZIPMAP_LEN_BYTES(*klen);  // 跳过编码klen所需要的字节数，此时key指向当前节点key的地址
    }
    zm += zipmapRawKeyLength(zm);  // 更新zm指针，跳过当前节点的key占用的空间大小，此时zm指向当前节点value_len的地址
    if (value) {
        *value = zm+1;  // 跳过free（1字节）
        *vlen = zipmapDecodeLength(zm);  // 获取当前节点value的长度
        *value += ZIPMAP_LEN_BYTES(*vlen);  // 跳过编码vlen所需要的字节数，此时value指向当前节点value的地址
    }
    zm += zipmapRawValueLength(zm);  // 更新zm指针，跳过当前节点的value占用的空间大小，此时zm指向下一个节点的首地址
    return zm;
}

/* 在zipmap中查找指定key，并获取它的value和value的长度，如果找到这个key返回1，否则返回0。 */
int zipmapGet(unsigned char *zm, unsigned char *key, unsigned int klen, unsigned char **value, unsigned int *vlen) {
    unsigned char *p;

    if ((p = zipmapLookupRaw(zm,key,klen,NULL)) == NULL) return 0;  // 找不到指定key，返回0，否则返回目标节点的指针
    p += zipmapRawKeyLength(p);  // 跳过key_len字段
    *vlen = zipmapDecodeLength(p);  // 获取目标节点value_len
    *value = p + ZIPMAP_LEN_BYTES(*vlen) + 1;  // 计算编码目标节点value_len所需字节数，跳过这个字节数和free的1字节，此时指向value
    return 1;
}

/* 查询key是或否存在，存在返回1，否则返回0。 */
int zipmapExists(unsigned char *zm, unsigned char *key, unsigned int klen) {
    return zipmapLookupRaw(zm,key,klen,NULL) != NULL;// 内部调用zipmap的key查找函数
}

/* 返回zipmap的节点数量。 */
unsigned int zipmapLen(unsigned char *zm) {
    unsigned int len = 0;
    if (zm[0] < ZIPMAP_BIGLEN) {  // zm_len小于ZIPMAP_BIGLEN时，节点数量就是它的值
        len = zm[0];
    } else {  // 否则需要遍历zipmap计算节点数量
        unsigned char *p = zipmapRewind(zm);
        while((p = zipmapNext(p,NULL,NULL,NULL,NULL)) != NULL) len++;

        if (len < ZIPMAP_BIGLEN) zm[0] = len;  // 遍历完更新zm_len
    }
    return len;
}

/* 返回zipmap占用的字节数，我们可以把zipmap序列化到磁盘（或者其他什么地方），只需要以zipmap头部指针为开始，顺序把它所占用字节数存储起来即可。 */
size_t zipmapBlobLen(unsigned char *zm) {
    unsigned int totlen;
    zipmapLookupRaw(zm,NULL,0,&totlen);
    return totlen;
}

#ifdef REDIS_TEST
static void zipmapRepr(unsigned char *p) {
    unsigned int l;

    printf("{status %u}",*p++);
    while(1) {
        if (p[0] == ZIPMAP_END) {
            printf("{end}");
            break;
        } else {
            unsigned char e;

            l = zipmapDecodeLength(p);
            printf("{key %u}",l);
            p += zipmapEncodeLength(NULL,l);
            if (l != 0 && fwrite(p,l,1,stdout) == 0) perror("fwrite");
            p += l;

            l = zipmapDecodeLength(p);
            printf("{value %u}",l);
            p += zipmapEncodeLength(NULL,l);
            e = *p++;
            if (l != 0 && fwrite(p,l,1,stdout) == 0) perror("fwrite");
            p += l+e;
            if (e) {
                printf("[");
                while(e--) printf(".");
                printf("]");
            }
        }
    }
    printf("\n");
}

#define UNUSED(x) (void)(x)
int zipmapTest(int argc, char *argv[]) {
    unsigned char *zm;

    UNUSED(argc);
    UNUSED(argv);

    zm = zipmapNew();

    zm = zipmapSet(zm,(unsigned char*) "name",4, (unsigned char*) "foo",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "surname",7, (unsigned char*) "foo",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "age",3, (unsigned char*) "foo",3,NULL);
    zipmapRepr(zm);

    zm = zipmapSet(zm,(unsigned char*) "hello",5, (unsigned char*) "world!",6,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "bar",3,NULL);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "!",1,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "foo",3, (unsigned char*) "12345",5,NULL);
    zipmapRepr(zm);
    zm = zipmapSet(zm,(unsigned char*) "new",3, (unsigned char*) "xx",2,NULL);
    zm = zipmapSet(zm,(unsigned char*) "noval",5, (unsigned char*) "",0,NULL);
    zipmapRepr(zm);
    zm = zipmapDel(zm,(unsigned char*) "new",3,NULL);
    zipmapRepr(zm);

    printf("\nLook up large key:\n");
    {
        unsigned char buf[512];
        unsigned char *value;
        unsigned int vlen, i;
        for (i = 0; i < 512; i++) buf[i] = 'a';

        zm = zipmapSet(zm,buf,512,(unsigned char*) "long",4,NULL);
        if (zipmapGet(zm,buf,512,&value,&vlen)) {
            printf("  <long key> is associated to the %d bytes value: %.*s\n",
                vlen, vlen, value);
        }
    }

    printf("\nPerform a direct lookup:\n");
    {
        unsigned char *value;
        unsigned int vlen;

        if (zipmapGet(zm,(unsigned char*) "foo",3,&value,&vlen)) {
            printf("  foo is associated to the %d bytes value: %.*s\n",
                vlen, vlen, value);
        }
    }
    printf("\nIterate through elements:\n");
    {
        unsigned char *i = zipmapRewind(zm);
        unsigned char *key, *value;
        unsigned int klen, vlen;

        while((i = zipmapNext(i,&key,&klen,&value,&vlen)) != NULL) {
            printf("  %d:%.*s => %d:%.*s\n", klen, klen, key, vlen, vlen, value);
        }
    }
    return 0;
}
#endif
