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

/*-----------------------------------------------------------------------------
 * List API
 *----------------------------------------------------------------------------*/

/* The function pushes an element to the specified list object 'subject',
 * at head or tail position as specified by 'where'.
 *
 * 将给定元素添加到列表的表头或表尾。
 *
 * 参数 where 决定了新元素添加的位置：
 *
 *  - REDIS_HEAD 将新元素添加到表头
 *
 *  - REDIS_TAIL 将新元素添加到表尾
 *
 * There is no need for the caller to increment the refcount of 'value' as
 * the function takes care of it if needed.
 *
 * 调用者无须担心 value 的引用计数，因为这个函数会负责这方面的工作。
 */
void listTypePush(robj *subject, robj *value, int where) {
    //对列表对象编码为quicklist类型操作
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {

        //根据where保存quicklist的头节点地址或尾节点地址
        int pos = (where == LIST_HEAD) ? QUICKLIST_HEAD : QUICKLIST_TAIL;

        //获得value编码为RAW的字符串对象
        value = getDecodedObject(value);

        //保存value的长度
        size_t len = sdslen(value->ptr);

        //PUSH value的值到quicklist的头或尾
        quicklistPush(subject->ptr, value->ptr, len, pos);

        //value的引用计数减1
        decrRefCount(value);
    } else {
        serverPanic("Unknown list encoding");//不是quicklist类型的编码则发送错误信息
    }
}

/* 拷贝对象类型的方法，用于listTypePop函数的调用 */
void *listPopSaver(unsigned char *data, unsigned int sz) {
    return createStringObject((char*)data,sz);
}

/* 列表类型的从where弹出一个value，POP命令底层实现  */
robj *listTypePop(robj *subject, int where) {
    long long vlong;
    robj *value = NULL;

    //获得POP的位置，quicklist的头部或尾部
    int ql_where = where == LIST_HEAD ? QUICKLIST_HEAD : QUICKLIST_TAIL;

    //对列表对象编码为quicklist类型操作
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        //从ql_where位置POP出一个entry节点，保存在value或vlong中
        if (quicklistPopCustom(subject->ptr, ql_where, (unsigned char **)&value,
                               NULL, &vlong, listPopSaver)) {
            if (!value)     //如果弹出的entry节点是整型的
                //则根据整型值创建一个字符串对象
                value = createStringObjectFromLongLong(vlong);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return value;
}

/* 返回对象的长度，entry节点个数 */
unsigned long listTypeLength(const robj *subject) {
    //对列表对象编码为quicklist类型操作
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        return quicklistCount(subject->ptr);    //返回对象的entry节点个数
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Initialize an iterator at the specified index. */
/* 初始化列表类型的迭代器为一个指定的下标 */
listTypeIterator *listTypeInitIterator(robj *subject, long index,
                                       unsigned char direction) {
    listTypeIterator *li = zmalloc(sizeof(listTypeIterator));//分配空间
    //设置迭代器的各个成员的初始值
    li->subject = subject;
    li->encoding = subject->encoding;
    li->direction = direction;
    li->iter = NULL;    //quicklist迭代器为空
    /* LIST_HEAD means start at TAIL and move *towards* head.
     * LIST_TAIL means start at HEAD and move *towards tail. */
    //获得迭代方向
    int iter_direction =
        direction == LIST_HEAD ? AL_START_TAIL : AL_START_HEAD;

    //对列表对象编码为quicklist类型操作
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        //将迭代器和下标为index的quicklistNode结合，迭代器指向该节点
        li->iter = quicklistGetIteratorAtIdx(li->subject->ptr,
                                             iter_direction, index);
    } else {
        serverPanic("Unknown list encoding");
    }
    return li;
}

/* Clean up the iterator. */
/* 释放迭快速列表代器 */
void listTypeReleaseIterator(listTypeIterator *li) {
    zfree(li->iter);    //释放quicklist迭代器
    zfree(li);          //释放列表类型迭代器
}

/* Stores pointer to current the entry in the provided entry structure
 * and advances the position of the iterator. Returns 1 when the current
 * entry is in fact an entry, 0 otherwise. */
/* 将列表类型的迭代器指向的entry保存在提供的listTypeEntry结构中，并且更新迭代器，1表示成功，0失败 */
int listTypeNext(listTypeIterator *li, listTypeEntry *entry) {
    /* Protect from converting when iterating */
    //确保对象编码类型和迭代器中encoding成员相等
    serverAssert(li->subject->encoding == li->encoding);

    //设置listTypeEntry的entry成员关联到当前列表类型的迭代器
    entry->li = li;
    //对列表对象编码为quicklist类型操作
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        //保存当前的entry到listTypeEntry的entry成员，并更新迭代器
        return quicklistNext(li->iter, &entry->entry);
    } else {
        serverPanic("Unknown list encoding");
    }
    return 0;
}

/* Return entry or NULL at the current position of the iterator. */
/* 返回一个节点的value对象，根据当前的迭代器 */
robj *listTypeGet(listTypeEntry *entry) {
    robj *value = NULL;
    //对列表对象编码为quicklist类型操作
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        if (entry->entry.value) {   //创建一个字符串对象保存列表类型的entry结构所指向的entry节点的字符串值
            value = createStringObject((char *)entry->entry.value,
                                       entry->entry.sz);
        } else {
            //创建一个字符串对象保存列表类型的entry结构所指向的entry节点的整型值
            value = createStringObjectFromLongLong(entry->entry.longval);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return value;
}

/* 列表类型的插入操作，将value对象插到where */
void listTypeInsert(listTypeEntry *entry, robj *value, int where) {
    //对列表对象编码为quicklist类型操作
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        value = getDecodedObject(value);    //解码对象vlaue为字符串类型
        sds str = value->ptr;               //获得value对象所保存的值
        size_t len = sdslen(str);           //获得value值的长度
        if (where == LIST_TAIL) {           //给定的where为列表尾部
            //尾插
            quicklistInsertAfter((quicklist *)entry->entry.quicklist,
                                 &entry->entry, str, len);
        } else if (where == LIST_HEAD) {//给定的where为列表头部
            //头插
            quicklistInsertBefore((quicklist *)entry->entry.quicklist,
                                  &entry->entry, str, len);
        }
        decrRefCount(value);    //引用计数减1，释放value对象
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Compare the given object with the entry at the current position. */
/* 比较列表类型的entry结构与对象的entry节点的值是否等，相等返回1 */
int listTypeEqual(listTypeEntry *entry, robj *o) {
    //对列表对象编码为quicklist类型操作
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        //确保objptr的编码类型是简单动态字符串类型的RAW或EMBSTR
        serverAssertWithInfo(NULL,o,sdsEncodedObject(o));

        //比较listTypeEntry结构中的entry值和给定的对象的值
        return quicklistCompare(entry->entry.zi,o->ptr,sdslen(o->ptr));
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Delete the element pointed to. */
/* 删除迭代器指向的entry */
void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry) {
    //对列表对象编码为quicklist类型操作
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        //删除entry节点，更新迭代器
        quicklistDelEntry(iter->iter, &entry->entry);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Create a quicklist from a single ziplist */
/* 转换ZIPLIST编码类型为quicklist类型，enc指定OBJ_ENCODING_QUICKLIST */
void listTypeConvert(robj *subject, int enc) {
    //确保subject的类型为列表类型，且编码为ziplist类型
    serverAssertWithInfo(NULL,subject,subject->type==OBJ_LIST);
    serverAssertWithInfo(NULL,subject,subject->encoding==OBJ_ENCODING_ZIPLIST);

    //对列表对象编码为quicklist类型操作
    if (enc == OBJ_ENCODING_QUICKLIST) {
        //以下两行都是由配置文件制定的参数
        size_t zlen = server.list_max_ziplist_size; //最大ziplist大小
        int depth = server.list_compress_depth;     //压缩深度
        //创建一个quicklist并将ptr指向的entry追加在quicklist末尾
        subject->ptr = quicklistCreateFromZiplist(zlen, depth, subject->ptr);
        //设置新的编码类型为OBJ_ENCODING_QUICKLIST
        subject->encoding = OBJ_ENCODING_QUICKLIST;
    } else {
        serverPanic("Unsupported list conversion");
    }
}

/*-----------------------------------------------------------------------------
 * List Commands
 *----------------------------------------------------------------------------*/

/* PUSH命令的底层实现，where保存push的位置 */
void pushGenericCommand(client *c, int where) {
    int j, pushed = 0;
    robj *lobj = lookupKeyWrite(c->db,c->argv[1]);  //以写操作读取key对象的value

    //如果value对象不是列表类型则发送错误信息，返回
    if (lobj && lobj->type != OBJ_LIST) {
        addReply(c,shared.wrongtypeerr);
        return;
    }

    //从第一个value开始遍历
    for (j = 2; j < c->argc; j++) {
        //如果没有找到key对象
        if (!lobj) {
            //创建一个quicklist类型的对象
            lobj = createQuicklistObject();
            //设置ziplist最大的长度和压缩程度，配置文件指定
            quicklistSetOptions(lobj->ptr, server.list_max_ziplist_size,
                                server.list_compress_depth);
            //将新的key对象和优化编码过的value对象进行组成键值对
            dbAdd(c->db,c->argv[1],lobj);
        }
        //在where推入一个value对象
        listTypePush(lobj,c->argv[j],where);
        //更新计数器
        pushed++;
    }

    //发送当前列表中元素的个数
    addReplyLongLong(c, (lobj ? listTypeLength(lobj) : 0));

    //如果推入元素成功
    if (pushed) {
        char *event = (where == LIST_HEAD) ? "lpush" : "rpush";

        //当数据库的键被改动，则会调用该函数发送信号
        signalModifiedKey(c,c->db,c->argv[1]);

        //发送"lpush"或"rpush"事件通知
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
    }
    server.dirty += pushed;//更新脏键
}

/*
 * LPUSH命令实现
 * LPUSH key value [value ...]
 * */
void lpushCommand(client *c) {
    pushGenericCommand(c,LIST_HEAD);
}

/*
 * RPUSH命令实现
 * RPUSH key value [value ...]
 * */
void rpushCommand(client *c) {
    pushGenericCommand(c,LIST_TAIL);
}

/* 当key存在时则push，PUSHX，INSERT命令的底层实现 */
void pushxGenericCommand(client *c, int where) {
    int j, pushed = 0;
    robj *subject;

    //以写操作读取key对象的value
    //如果读取失败或读取的value对象不是列表类型则返回
    if ((subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,subject,OBJ_LIST)) return;

    //将val对象推入到列表的头部或尾部
    for (j = 2; j < c->argc; j++) {
        listTypePush(subject,c->argv[j],where);
        pushed++;
    }

    //发送当前对象长度
    addReplyLongLong(c,listTypeLength(subject));

    //追加成功
    if (pushed) {
        char *event = (where == LIST_HEAD) ? "lpush" : "rpush";

        //当数据库的键被改动，则会调用该函数发送信号
        signalModifiedKey(c,c->db,c->argv[1]);

        //发送"lpush"或"rpush"事件通知
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
    }
    server.dirty += pushed;//更新脏键
}

/*
 * LPUSHX key value1
 * LPUSHX命令的实现
 * */
void lpushxCommand(client *c) {
    pushxGenericCommand(c,LIST_HEAD);
}

/*
 * RPUSHX key value1
 * RPUSHX命令的实现
 * */
void rpushxCommand(client *c) {
    pushxGenericCommand(c,LIST_TAIL);
}

/*
 * LINSERT命令的实现
 * LINSERT key BEFORE|AFTER pivot(基准值) value
 * */
void linsertCommand(client *c) {
    int where;
    robj *subject;
    listTypeIterator *iter;
    listTypeEntry entry;
    int inserted = 0;

    //比较where字符串，忽略大小写
    if (strcasecmp(c->argv[2]->ptr,"after") == 0) {
        //如果是after，则是在基准值的后插入
        where = LIST_TAIL;
    } else if (strcasecmp(c->argv[2]->ptr,"before") == 0) {
        //如果是before，则是在基准值的前插入
        where = LIST_HEAD;
    } else {
        addReply(c,shared.syntaxerr);
        return;
    }

    //以写操作读取key对象的value
    //如果读取失败或读取的value对象不是列表类型则返回
    if ((subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,subject,OBJ_LIST)) return;

    /* Seek pivot from head to tail */
    //初始化列表类型的迭代器
    iter = listTypeInitIterator(subject,0,LIST_TAIL);
    while (listTypeNext(iter,&entry)) { //迭代
        if (listTypeEqual(&entry,c->argv[3])) {//匹配目标entry
            listTypeInsert(&entry,c->argv[4],where);//新value插入快速列表的ziplist指定位置
            inserted = 1;
            break;
        }
    }
    listTypeReleaseIterator(iter);//迭代器释放

    if (inserted) {//如果有插入
        //当数据库的键被改动，则会调用该函数发送信号
        signalModifiedKey(c,c->db,c->argv[1]);

        //linsert事件通知
        notifyKeyspaceEvent(NOTIFY_LIST,"linsert",
                            c->argv[1],c->db->id);
        server.dirty++;//更新脏键
    } else {
        /* Notify client of a failed insert */
        addReplyLongLong(c,-1);
        return;
    }

    //发送当前对象长度
    addReplyLongLong(c,listTypeLength(subject));

}

/*
 * LLEN命令实现
 * LLEN key
 * */
void llenCommand(client *c) {
    //以读操作取出key大小的value值
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.czero);
    //如果key没找到或value大小不是列表类型则直接返回
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    //发送列表中的元素数量给client
    addReplyLongLong(c,listTypeLength(o));
}

/*
 * LINDEX命令的实现
 * LINDEX key index
 * */
void lindexCommand(client *c) {
    //以读操作取出key对象的value值
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp]);
    //如果key没找到或value对象不是列表类型则直接返回
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    long index;
    robj *value = NULL;

    //将index参数转换为long类型的整数，保存在index中
    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != C_OK))
        return;

    //只对编码为quicklist类型的value对象操作
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistEntry entry;
        //将下标为index的entry节点保存到entry中
        if (quicklistIndex(o->ptr, index, &entry)) {
            if (entry.value) {  //如果vlaue是字符串类型
                //创建一个字符串类型的对象，保存value值
                value = createStringObject((char*)entry.value,entry.sz);
            } else {
                //将整型的value值转换为字符串类型并创建字符串类型的对象
                value = createStringObjectFromLongLong(entry.longval);
            }
            addReplyBulk(c,value);  //发送value对象
            decrRefCount(value);    //释放value对象
        } else {
            addReplyNull(c);        //如果下标为index没找到，则发送空信息
        }
    } else {
        serverPanic("Unknown list encoding");   //发送未知的列表编码类型
    }
}

/*
 * LSET key index value
 * LSET命令实现
 * */
void lsetCommand(client *c) {
    //以写操作取出key对象的value值
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr);
    //如果key没找到或value对象不是列表类型则直接返回
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;
    long index;
    robj *value = c->argv[3];

    //将index参数转换为long类型的整数，保存在index中
    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != C_OK))
        return;

    //只对编码为quicklist类型的value对象操作
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklist *ql = o->ptr;
        //将下标为index的entry替换成value对象的值
        int replaced = quicklistReplaceAtIndex(ql, index,
                                               value->ptr, sdslen(value->ptr));
        if (!replaced) {
            //如果替换失败，则发送下标越界错误信息
            addReply(c,shared.outofrangeerr);
        } else {
            //替换成功，则发送ok
            addReply(c,shared.ok);
            //当数据库的键被改动，则会调用该函数发送信号
            signalModifiedKey(c->db,c->argv[1]);
            //发送"lset"时间通知
            notifyKeyspaceEvent(NOTIFY_LIST,"lset",c->argv[1],c->db->id);
            //更新脏键
            server.dirty++;
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* POP命令的底层实现，where保存pop的位置 */
void popGenericCommand(client *c, int where) {
    //以写操作取出key对象的value值
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.null[c->resp]);
    //如果key没找到或value对象不是列表类型则直接返回
    if (o == NULL || checkType(c,o,OBJ_LIST)) return;

    //从where 弹出一个value
    robj *value = listTypePop(o,where);
    //如果value为空，则发送空信息
    if (value == NULL) {
        addReplyNull(c);
    } else {
        //保存时间名称
        char *event = (where == LIST_HEAD) ? "lpop" : "rpop";

        //发送value给client
        addReplyBulk(c,value);
        //释放value对象
        decrRefCount(value);
        //发送事件通知
        notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
        //如果弹出一个元素后，列表为空
        if (listTypeLength(o) == 0) {
            //发送"del"时间通知
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                c->argv[1],c->db->id);
            //从数据库中删除当前的key
            dbDelete(c->db,c->argv[1]);
        }
        //当数据库的键被改动，则会调用该函数发送信号
        signalModifiedKey(c,c->db,c->argv[1]);
        //更新脏键
        server.dirty++;
    }
}

/*
 * LPOP命令的实现
 * LPOP key
 * */
void lpopCommand(client *c) {
    popGenericCommand(c,LIST_HEAD);
}

/*
 * RPOP命令的实现
 * RPOP key
 * */
void rpopCommand(client *c) {
    popGenericCommand(c,LIST_TAIL);
}

/*
 * LRANGE key start stop
 * LRANGE命令的实现
 * */
void lrangeCommand(client *c) {
    robj *o;
    long start, end, llen, rangelen;

    //将字符串类型起始地址start和结束地址end转换为long类型保存在start和end中
    //如果任意失败，则直接返回
    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)) return;

    //以读操作取出key大小的value值，如果value对象不是列表类型，直接返回
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptyarray)) == NULL
         || checkType(c,o,OBJ_LIST)) return;
    //获取列表元素数量
    llen = listTypeLength(o);

    /* convert negative indexes */
    //将负数范围转换成合法范围
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    //不合理的范围，发送空信息
    if (start > end || start >= llen) {
        addReply(c,shared.emptyarray);
        return;
    }
    //end不能超过元素个数
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;

    /* Return the result in form of a multi-bulk reply */
    //发送最后的范围值给client
    addReplyArrayLen(c,rangelen);
    //只对编码为quicklist类型的value对象操作
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        //创建迭代器，指向start起始的位置
        listTypeIterator *iter = listTypeInitIterator(o, start, LIST_TAIL);

        //遍历要找范围的大小次
        while(rangelen--) {
            listTypeEntry entry;
            //保存当前指向的entry节点值到entry中，并且指向下一个entry节点
            listTypeNext(iter, &entry);
            quicklistEntry *qe = &entry.entry;
            //若是是字符串类型的vlaue
            if (qe->value) {
                //发送字符串类型的值给client
                addReplyBulkCBuffer(c,qe->value,qe->sz);
            } else {
                //否则，发送整型的值给client
                addReplyBulkLongLong(c,qe->longval);
            }
        }
        //释放迭代器
        listTypeReleaseIterator(iter);
    } else {
        serverPanic("List encoding is not QUICKLIST!");
    }
}

/*
 * LTRIM key start stop
 * LTRIM命令实现
 * */
void ltrimCommand(client *c) {
    robj *o;
    long start, end, llen, ltrim, rtrim;

    //将字符串类型起始地址start和结束地址end转换为long类型保存在start和end中
    //如果任意失败，则直接返回
    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)) return;

    // 以读操作取出key大小的value值，如果value对象不是列表类型，直接返回
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.ok)) == NULL ||
        checkType(c,o,OBJ_LIST)) return;
    //获取列表元素数量
    llen = listTypeLength(o);

    /* convert negative indexes */
    // 将负数范围转换成合法范围
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    //不合理的范围，移除所有的元素
    if (start > end || start >= llen) {
        /* Out of range start or start > end result in empty list */
        ltrim = llen;
        rtrim = 0;
    } else {
        if (end >= llen) end = llen-1;//end值不能超过元素个数
        ltrim = start;      //左边界
        rtrim = llen-end-1; //右边界
    }

    /* Remove list elements to perform the trim */
    //只对编码为quicklist类型的value对象操作
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistDelRange(o->ptr,0,ltrim);//删除左边界以左的所有元素
        quicklistDelRange(o->ptr,-rtrim,rtrim); //删除左边界以右的所有元素
    } else {
        serverPanic("Unknown list encoding");
    }

    //发送"ltrim"事件通知
    notifyKeyspaceEvent(NOTIFY_LIST,"ltrim",c->argv[1],c->db->id);
    //如果将所有元素全部删除完了
    if (listTypeLength(o) == 0) {
        //从数据库中删除该key
        dbDelete(c->db,c->argv[1]);
        //发送"del"时间通知
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }
    //当数据库的键被改动，则会调用该函数发送信号
    signalModifiedKey(c,c->db,c->argv[1]);
    //更新脏键
    server.dirty++;
    //发送ok信息给client
    addReply(c,shared.ok);
}

/* LPOS key element [RANK rank] [COUNT num-matches] [MAXLEN len]
 *
 * The "rank" is the position of the match, so if it is 1, the first match
 * is returned, if it is 2 the second match is returned and so forth.
 * It is 1 by default. If negative has the same meaning but the search is
 * performed starting from the end of the list.
 *
 * If COUNT is given, instead of returning the single element, a list of
 * all the matching elements up to "num-matches" are returned. COUNT can
 * be combiled with RANK in order to returning only the element starting
 * from the Nth. If COUNT is zero, all the matching elements are returned.
 *
 * MAXLEN tells the command to scan a max of len elements. If zero (the
 * default), all the elements in the list are scanned if needed.
 *
 * The returned elements indexes are always referring to what LINDEX
 * would return. So first element from head is 0, and so forth. */

void lposCommand(client *c) {
    robj *o, *ele;
    ele = c->argv[2];
    int direction = LIST_TAIL;
    long rank = 1, count = -1, maxlen = 0; /* Count -1: option not given. */

    /* Parse the optional arguments. */
    for (int j = 3; j < c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc-1)-j;

        if (!strcasecmp(opt,"RANK") && moreargs) {
            j++;
            if (getLongFromObjectOrReply(c, c->argv[j], &rank, NULL) != C_OK)
                return;
            if (rank == 0) {
                addReplyError(c,"RANK can't be zero: use 1 to start from "
                                "the first match, 2 from the second, ...");
                return;
            }
        } else if (!strcasecmp(opt,"COUNT") && moreargs) {
            j++;
            if (getLongFromObjectOrReply(c, c->argv[j], &count, NULL) != C_OK)
                return;
            if (count < 0) {
                addReplyError(c,"COUNT can't be negative");
                return;
            }
        } else if (!strcasecmp(opt,"MAXLEN") && moreargs) {
            j++;
            if (getLongFromObjectOrReply(c, c->argv[j], &maxlen, NULL) != C_OK)
                return;
            if (maxlen < 0) {
                addReplyError(c,"MAXLEN can't be negative");
                return;
            }
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    /* A negative rank means start from the tail. */
    if (rank < 0) {
        rank = -rank;
        direction = LIST_HEAD;
    }

    /* We return NULL or an empty array if there is no such key (or
     * if we find no matches, depending on the presence of the COUNT option. */
    if ((o = lookupKeyRead(c->db,c->argv[1])) == NULL) {
        if (count != -1)
            addReply(c,shared.emptyarray);
        else
            addReply(c,shared.null[c->resp]);
        return;
    }
    if (checkType(c,o,OBJ_LIST)) return;

    /* If we got the COUNT option, prepare to emit an array. */
    void *arraylenptr = NULL;
    if (count != -1) arraylenptr = addReplyDeferredLen(c);

    /* Seek the element. */
    listTypeIterator *li;
    li = listTypeInitIterator(o,direction == LIST_HEAD ? -1 : 0,direction);
    listTypeEntry entry;
    long llen = listTypeLength(o);
    long index = 0, matches = 0, matchindex = -1, arraylen = 0;
    while (listTypeNext(li,&entry) && (maxlen == 0 || index < maxlen)) {
        if (listTypeEqual(&entry,ele)) {
            matches++;
            matchindex = (direction == LIST_TAIL) ? index : llen - index - 1;
            if (matches >= rank) {
                if (arraylenptr) {
                    arraylen++;
                    addReplyLongLong(c,matchindex);
                    if (count && matches-rank+1 >= count) break;
                } else {
                    break;
                }
            }
        }
        index++;
        matchindex = -1; /* Remember if we exit the loop without a match. */
    }
    listTypeReleaseIterator(li);

    /* Reply to the client. Note that arraylenptr is not NULL only if
     * the COUNT option was selected. */
    if (arraylenptr != NULL) {
        setDeferredArrayLen(c,arraylenptr,arraylen);
    } else {
        if (matchindex != -1)
            addReplyLongLong(c,matchindex);
        else
            addReply(c,shared.null[c->resp]);
    }
}

/*
 * LREM key count value
 * LREM命令
 * */
void lremCommand(client *c) {
    robj *subject, *obj;
    obj = c->argv[3];
    long toremove;
    long removed = 0;

    //将字符串类型的count参数转换为long类型的整数，保存在toremove中
    if ((getLongFromObjectOrReply(c, c->argv[2], &toremove, NULL) != C_OK))
        return;

    //以写操作读取出key对象的value值
    subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero);
    //如果key不存在或value对象不是列表类型则直接返回
    if (subject == NULL || checkType(c,subject,OBJ_LIST)) return;

    listTypeIterator *li;
    if (toremove < 0) {
        //如果toremove小于零，则从尾部向头部删除
        toremove = -toremove;
        //创建迭代器，指向尾部元素
        li = listTypeInitIterator(subject,-1,LIST_HEAD);
    } else {
        //如果toremove大于等于零，则从头部向尾部删除，创建迭代器
        li = listTypeInitIterator(subject,0,LIST_TAIL);
    }

    listTypeEntry entry;
    //遍历列表，保存迭代器当前指向的entry
    while (listTypeNext(li,&entry)) {
        if (listTypeEqual(&entry,obj)) {//如果当前entry的值是obj
            //删除当前的entry
            listTypeDelete(li, &entry);
            //更新脏键
            server.dirty++;
            //更新计数器
            removed++;
            //如果删除了count个，则跳出循环
            if (toremove && removed == toremove) break;
        }
    }
    //释放迭代器
    listTypeReleaseIterator(li);

    //如果删除成功
    if (removed) {
        //当数据库的键被改动，则会调用该函数发送信号
        signalModifiedKey(c,c->db,c->argv[1]);
        //发送"lrem"时间通知
        notifyKeyspaceEvent(NOTIFY_LIST,"lrem",c->argv[1],c->db->id);
    }

    //如果将列表中的元素全部删除完了
    if (listTypeLength(subject) == 0) {
        //从数据库中删除键key
        dbDelete(c->db,c->argv[1]);
        //发送"del"时间通知
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }

    //发送删除元素的个数给client
    addReplyLongLong(c,removed);
}

/* This is the semantic of this command:
 *  RPOPLPUSH srclist dstlist:
 *    IF LLEN(srclist) > 0
 *      element = RPOP srclist
 *      LPUSH dstlist element
 *      RETURN element
 *    ELSE
 *      RETURN nil
 *    END
 *  END
 *
 * The idea is to be able to get an element from a list in a reliable way
 * since the element is not just returned but pushed against another list
 * as well. This command was originally proposed by Ezra Zygmuntowicz.
 */

/* 将一个value推入到列表头部，被rpoplpushCommand调用 */
void rpoplpushHandlePush(client *c, robj *dstkey, robj *dstobj, robj *value) {
    /* Create the list if the key does not exist */
    //如果目标dstkey不存在
    if (!dstobj) {
        //创建一个quicklist对象
        dstobj = createQuicklistObject();
        //设置ziplist的最大长度和压缩程度
        quicklistSetOptions(dstobj->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);
        //将key添加到数据库中
        dbAdd(c->db,dstkey,dstobj);
    }
    //当数据库的键被改动，则会调用该函数发送信号
    signalModifiedKey(c,c->db,dstkey);
    //将vlaue推入到列表的头部
    listTypePush(dstobj,value,LIST_HEAD);
    //发送"lpush"时间通知
    notifyKeyspaceEvent(NOTIFY_LIST,"lpush",dstkey,c->db->id);
    /* Always send the pushed value to the client. */
    //将value值发送给client
    addReplyBulk(c,value);
}

/*
 * RPOPLPUSH source destination
 * RPOPLPUSH命令的实现
 * */
void rpoplpushCommand(client *c) {
    robj *sobj, *value;
    //以写操作读取source对象的值，并且检查数据类型是否为OBJ_LIST
    if ((sobj = lookupKeyWriteOrReply(c,c->argv[1],shared.null[c->resp]))
        == NULL || checkType(c,sobj,OBJ_LIST)) return;

    //如果列表长度为0，没有元素，直接发送空信息
    if (listTypeLength(sobj) == 0) {
        /* This may only happen after loading very old RDB files. Recent
         * versions of Redis delete keys of empty lists. */
        addReplyNull(c);
    } else {
        //以写操作读取destination对象的值
        robj *dobj = lookupKeyWrite(c->db,c->argv[2]);
        robj *touchedkey = c->argv[1];

        //如果目标对象类型是否是列表类型
        if (dobj && checkType(c,dobj,OBJ_LIST)) return;
        //从source尾部弹出一个元素
        value = listTypePop(sobj,LIST_TAIL);
        /* We saved touched key, and protect it, since rpoplpushHandlePush
         * may change the client command argument vector (it does not
         * currently). */
        //备份一份source键，因为rpoplpushHandlePush可能会更改client命令行参数
        incrRefCount(touchedkey);
        //将一个value推入到destination列表头部，如果destination列表不存在，则新创建一个
        rpoplpushHandlePush(c,c->argv[2],dobj,value);

        /* listTypePop returns an object with its refcount incremented */
        decrRefCount(value);    //将弹出的value释放

        /* Delete the source list when it is empty */
        //发送"rpop"时间通知
        notifyKeyspaceEvent(NOTIFY_LIST,"rpop",touchedkey,c->db->id);
        //如果source列表为空了，则删除key
        if (listTypeLength(sobj) == 0) {
            //删除之前备份的key键
            dbDelete(c->db,touchedkey);
            //发送"rpop"时间通知
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                touchedkey,c->db->id);
        }
        //当数据库的键被改动，则会调用该函数发送信号
        signalModifiedKey(c,c->db,touchedkey);
        //释放备份的source键
        decrRefCount(touchedkey);
        server.dirty++;
        if (c->cmd->proc == brpoplpushCommand) {
            rewriteClientCommandVector(c,3,shared.rpoplpush,c->argv[1],c->argv[2]);
        }
    }
}

/*-----------------------------------------------------------------------------
 * Blocking POP operations
 *----------------------------------------------------------------------------*/

/* This is a helper function for handleClientsBlockedOnKeys(). Its work
 * is to serve a specific client (receiver) that is blocked on 'key'
 * in the context of the specified 'db', doing the following:
 *
 * 1) Provide the client with the 'value' element.
 * 2) If the dstkey is not NULL (we are serving a BRPOPLPUSH) also push the
 *    'value' element on the destination list (the LPUSH side of the command).
 * 3) Propagate the resulting BRPOP, BLPOP and additional LPUSH if any into
 *    the AOF and replication channel.
 *
 * The argument 'where' is LIST_TAIL or LIST_HEAD, and indicates if the
 * 'value' element was popped from the head (BLPOP) or tail (BRPOP) so that
 * we can propagate the command properly.
 *
 * The function returns C_OK if we are able to serve the client, otherwise
 * C_ERR is returned to signal the caller that the list POP operation
 * should be undone as the client was not served: This only happens for
 * BRPOPLPUSH that fails to push the value to the destination key as it is
 * of the wrong type. */
/*
 * receiver是被阻塞的客户端，key是造成阻塞的键，db是key所在的数据库，value是被提供给客户端的值
 * 如果dstkey不为空，则将value推入到dstkey中
 * */
int serveClientBlockedOnList(client *receiver, robj *key, robj *dstkey, redisDb *db, robj *value, int where)
{
    robj *argv[3];

    //如果dstkey为空，则执行的是BLPOP或BRPOP
    if (dstkey == NULL) {
        /* Propagate the [LR]POP operation. */
        //根据where判断出是LPOP还是RPOP命令
        argv[0] = (where == LIST_HEAD) ? shared.lpop :
                                          shared.rpop;
        //弹出元素的key
        argv[1] = key;
        //将[LR]POP命令传播到AOF和REPL
        propagate((where == LIST_HEAD) ?
            server.lpopCommand : server.rpopCommand,
            db->id,argv,2,PROPAGATE_AOF|PROPAGATE_REPL);

        /* BRPOP/BLPOP */
        //发送回复信息
        addReplyArrayLen(receiver,2);
        addReplyBulk(receiver,key);
        addReplyBulk(receiver,value);

        /* Notify event. */
        char *event = (where == LIST_HEAD) ? "lpop" : "rpop";
        notifyKeyspaceEvent(NOTIFY_LIST,event,key,receiver->db->id);
        //dstkey不为空，执行BRPOPLPUSH命令
    } else {
        /* BRPOPLPUSH */
        //以读操作将dstkey对象的值读出来
        robj *dstobj =
            lookupKeyWrite(receiver->db,dstkey);
        //如果dstobj对象是列表类型，将BRPOPLPUSH命令分为RPOP和LPUSH分别处理
        if (!(dstobj &&
             checkType(receiver,dstobj,OBJ_LIST)))
        {
            rpoplpushHandlePush(receiver,dstkey,dstobj,
                value);
            /* Propagate the RPOPLPUSH operation. */
            //保存LPOP命令和目标键和弹出元素的键
            argv[0] = shared.rpoplpush;
            argv[1] = key;
            argv[2] = dstkey;
            //将LPUSH命令传播到AOF和REPL
            propagate(server.rpoplpushCommand,
                db->id,argv,3,
                PROPAGATE_AOF|
                PROPAGATE_REPL);

            /* Notify event ("lpush" was notified by rpoplpushHandlePush). */
            notifyKeyspaceEvent(NOTIFY_LIST,"rpop",key,receiver->db->id);
        } else {
            /* BRPOPLPUSH failed because of wrong
             * destination type. */
            return C_ERR;
        }
    }
    return C_OK;
}

/* Blocking RPOP/LPOP */
/*
 * BLPOP key [key ...] timeout
 * BRPOP BLPOP 命令的底层实现
 * */
void blockingPopGenericCommand(client *c, int where) {
    robj *o;
    mstime_t timeout;
    int j;

    // 以秒为单位取出timeout值
    if (getTimeoutFromObjectOrReply(c,c->argv[c->argc-1],&timeout,UNIT_SECONDS)
        != C_OK) return;

    //遍历所有的key
    for (j = 1; j < c->argc-1; j++) {
        //以写操作取出当前key的值
        o = lookupKeyWrite(c->db,c->argv[j]);
        // value对象不为空
        if (o != NULL) {
            // 如果value对象的类型不是列表类型，发送类型错误信息
            if (o->type != OBJ_LIST) {
                addReply(c,shared.wrongtypeerr);
                return;
            } else {
                // 列表长度不为0
                if (listTypeLength(o) != 0) {
                    /* Non empty list, this is like a normal [LR]POP. */
                    // 保存事件名称
                    char *event = (where == LIST_HEAD) ? "lpop" : "rpop";
                    // 保存弹出的value对象
                    robj *value = listTypePop(o,where);
                    serverAssert(value != NULL);

                    // 发送回复给client
                    addReplyArrayLen(c,2);
                    addReplyBulk(c,c->argv[j]);
                    addReplyBulk(c,value);
                    // 释放value
                    decrRefCount(value);
                    // 发送事件通知
                    notifyKeyspaceEvent(NOTIFY_LIST,event,
                                        c->argv[j],c->db->id);
                    //如果弹出元素后列表为空
                    if (listTypeLength(o) == 0) {
                        //从数据库中删除当前的key
                        dbDelete(c->db,c->argv[j]);
                        // 发送"del"的事件通知
                        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                                            c->argv[j],c->db->id);
                    }
                    //当数据库的键被改动，则会调用该函数发送信号
                    signalModifiedKey(c,c->db,c->argv[j]);
                    server.dirty++;

                    /* Replicate it as an [LR]POP instead of B[LR]POP. */
                    // 传播一个[LR]POP 而不是B[LR]POP
                    rewriteClientCommandVector(c,2,
                        (where == LIST_HEAD) ? shared.lpop : shared.rpop,
                        c->argv[j]);
                    return;
                }
            }
        }
    }

    /* If we are inside a MULTI/EXEC and the list is empty the only thing
     * we can do is treating it as a timeout (even with timeout 0). */
    if (c->flags & CLIENT_MULTI) {
        addReplyNullArray(c);
        return;
    }

    /* If the keys do not exist we must block */
    blockForKeys(c,BLOCKED_LIST,c->argv + 1,c->argc - 2,timeout,NULL,NULL);
}

/* BLPOP命令的实现 */
void blpopCommand(client *c) {
    blockingPopGenericCommand(c,LIST_HEAD);
}

/* RPOP命令的实现 */
void brpopCommand(client *c) {
    blockingPopGenericCommand(c,LIST_TAIL);
}

/*
 * BRPOPLPUSH source destination timeout
 * BRPOPLPUSH命令的实现
 * */
void brpoplpushCommand(client *c) {
    mstime_t timeout;

    //以秒为单位取出超时时间
    if (getTimeoutFromObjectOrReply(c,c->argv[3],&timeout,UNIT_SECONDS)
        != C_OK) return;

    //以写操作读取出 source的值
    robj *key = lookupKeyWrite(c->db, c->argv[1]);

    //如果键为空，阻塞
    if (key == NULL) {
        // 如果命令在一个事务中执行，则发送一个空回复以避免死等待
        if (c->flags & CLIENT_MULTI) {
            /* Blocking against an empty list in a multi state
             * returns immediately. */
            addReplyNull(c);
        } else {
            /* The list is empty and the client blocks. */
            // 列表为空，则将client阻塞
            blockForKeys(c,BLOCKED_LIST,c->argv + 1,1,timeout,c->argv[2],NULL);
        }
        //如果键不为空，指向RPOPLPUSH
    } else {
        //判断取出的value对象是否为列表类型，不是的话发送类型错误信息
        if (key->type != OBJ_LIST) {
            addReply(c, shared.wrongtypeerr);
        } else {
            /* The list exists and has elements, so
             * the regular rpoplpushCommand is executed. */
            // value对象的列表存在且有元素，所以调用普通的rpoplpush命令
            serverAssertWithInfo(c,key,listTypeLength(key) > 0);
            rpoplpushCommand(c);
        }
    }
}
