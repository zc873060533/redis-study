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
 * Set Commands
 *----------------------------------------------------------------------------*/

void sunionDiffGenericCommand(client *c, robj **setkeys, int setnum,
                              robj *dstkey, int op);

/* Factory method to return a set that *can* hold "value".
 *
 * 返回一个可以保存值 value 的集合。
 *
 * When the object has an integer-encodable value,
 * an intset will be returned. Otherwise a regular hash table.
 *
 * 当对象的值可以被编码为整数时，返回 intset ，
 * 否则，返回普通的哈希表。
 */
robj *setTypeCreate(sds value) {

    //如果value对象的值可以转换为long long 类型的整数，则创建一个整数集合intset
    if (isSdsRepresentableAsLongLong(value,NULL) == C_OK)
        return createIntsetObject();

    // 否则创建一个哈希表类型的集合
    return createSetObject();
}

/*
 * 多态 add 操作
 *
 * 添加成功返回 1 ，如果元素已经存在，返回 0 。
 */
int setTypeAdd(robj *subject, sds value) {
    long long llval;

    //如果是字典构成的集合
    if (subject->encoding == OBJ_ENCODING_HT) {
        dict *ht = subject->ptr;

        //将value加入集合作为哈希键，哈希值为NULL
        dictEntry *de = dictAddRaw(ht,value,NULL);
        if (de) {
            dictSetKey(ht,de,sdsdup(value));
            dictSetVal(ht,de,NULL);
            return 1;
        }

        //如果是整数构成的集合
    } else if (subject->encoding == OBJ_ENCODING_INTSET) {
        //如果要添加的value可以转换为整数，则保存在llval中
        if (isSdsRepresentableAsLongLong(value,&llval) == C_OK) {
            uint8_t success = 0;
            //将转换为整数的value加入集合，添加成功，讲success赋值为1
            subject->ptr = intsetAdd(subject->ptr,llval,&success);
            //如果添加成功
            if (success) {
                /* Convert to regular set when the intset contains
                 * too many entries. */
                //查看整数集合的元素个数是否大于配置的最大个数
                if (intsetLen(subject->ptr) > server.set_max_intset_entries)
                    setTypeConvert(subject,OBJ_ENCODING_HT);
                return 1;
            }
        } else {
            /* Failed to get integer from object, convert to regular set. */
            //不能将value转换为longlong可以表示整数，那么需要将intset类型转换成字典类型集合
            setTypeConvert(subject,OBJ_ENCODING_HT);

            /* The set *was* an intset and this value is not integer
             * encodable, so dictAdd should always work. */
            //确保执行向字典中添加了value
            serverAssert(dictAdd(subject->ptr,sdsdup(value),NULL) == DICT_OK);
            return 1;
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}

/* 从集合对象中删除一个值为value的元素，删除成功返回1，失败返回0 */
int setTypeRemove(robj *setobj, sds value) {
    long long llval;

    //从字典中删除一个元素
    if (setobj->encoding == OBJ_ENCODING_HT) {
        //如果成功从字典中删除了元素，需要进行判断是否缩小字典的大小
        if (dictDelete(setobj->ptr,value) == DICT_OK) {
            if (htNeedsResize(setobj->ptr)) dictResize(setobj->ptr);
            return 1;
        }

        //从整数集合中删除一个元素
    } else if (setobj->encoding == OBJ_ENCODING_INTSET) {
        //如果value可以转换为整型值，则进行删除
        if (isSdsRepresentableAsLongLong(value,&llval) == C_OK) {
            int success;
            setobj->ptr = intsetRemove(setobj->ptr,llval,&success);
            if (success) return 1;
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}

/*
 * 集合中是否存在值为value的元素，存在返回1，否则返回0
 * */
int setTypeIsMember(robj *subject, sds value) {
    long long llval;
    //从字典集合中查找
    if (subject->encoding == OBJ_ENCODING_HT) {
        return dictFind((dict*)subject->ptr,value) != NULL;
        //从整数集合中查找
    } else if (subject->encoding == OBJ_ENCODING_INTSET) {
        //必须转换为整数类型进行查找
        if (isSdsRepresentableAsLongLong(value,&llval) == C_OK) {
            return intsetFind((intset*)subject->ptr,llval);
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}

/* 创建并初始化一个集合类型的迭代器 */
setTypeIterator *setTypeInitIterator(robj *subject) {
    //分配空间并初始化成员
    setTypeIterator *si = zmalloc(sizeof(setTypeIterator));
    si->subject = subject;
    si->encoding = subject->encoding;

    //初始化字典的迭代器
    if (si->encoding == OBJ_ENCODING_HT) {
        si->di = dictGetIterator(subject->ptr);
        //初始化集合的迭代器，该成员为集合的下标
    } else if (si->encoding == OBJ_ENCODING_INTSET) {
        si->ii = 0;
    } else {
        serverPanic("Unknown set encoding");
    }
    return si;
}

/* 释放迭代器空间 */
void setTypeReleaseIterator(setTypeIterator *si) {

    //如果是字典类型，需要先释放字典类型的迭代器
    if (si->encoding == OBJ_ENCODING_HT)
        dictReleaseIterator(si->di);

    zfree(si);
}

/* Move to the next entry in the set. Returns the object at the current
 * position.
 *
 * 取出被迭代器指向的当前集合元素。
 *
 * Since set elements can be internally be stored as redis objects or
 * simple arrays of integers, setTypeNext returns the encoding of the
 * set object you are iterating, and will populate the appropriate pointer
 * (eobj) or (llobj) accordingly.
 *
 * 因为集合即可以编码为 intset ，也可以编码为哈希表，
 * 所以程序会根据集合的编码，选择将值保存到那个参数里：
 *
 *  - 当编码为 intset 时，元素被指向到 llobj 参数
 *
 *  - 当编码为哈希表时，元素被指向到 eobj 参数
 *
 * 并且函数会返回被迭代集合的编码，方便识别。
 *
 * When there are no longer elements -1 is returned.
 *
 * 当集合中的元素全部被迭代完毕时，函数返回 -1 。
 *
 * Returned objects ref count is not incremented, so this function is
 * copy on write friendly.
 *
 * 因为被返回的对象是没有被增加引用计数的，
 * 所以这个函数是对 copy-on-write 友好的。
 */
int setTypeNext(setTypeIterator *si, sds *sdsele, int64_t *llele) {
    //迭代字典
    if (si->encoding == OBJ_ENCODING_HT) {

        //得到下一个节点地址，更新迭代器
        dictEntry *de = dictNext(si->di);

        // 字典已迭代完
        if (de == NULL) return -1;

        //保存元素
        *sdsele = dictGetKey(de);
        *llele = -123456789; /* Not needed. Defensive. */

        //迭代整数集合
    } else if (si->encoding == OBJ_ENCODING_INTSET) {
        //从intset中保存元素到llele中
        if (!intsetGet(si->subject->ptr,si->ii++,llele))
            return -1;
        *sdsele = NULL; /* Not needed. Defensive. */
    } else {
        serverPanic("Wrong set encoding in setTypeNext");
    }
    return si->encoding;//返回编码类型
}

/* The not copy on write friendly version but easy to use version
 * of setTypeNext() is setTypeNextObject(), returning new objects
 * or incrementing the ref count of returned objects. So if you don't
 * retain a pointer to this object you should call decrRefCount() against it.
 *
 * setTypeNext 的非 copy-on-write 友好版本，
 * 总是返回一个新的、或者已经增加过引用计数的对象。
 *
 * 调用者在使用完对象之后，应该对对象调用 decrRefCount() 。
 *
 * This function is the way to go for write operations where COW is not
 * an issue as the result will be anyway of incrementing the ref count.
 *
 * 这个函数应该在非 copy-on-write 时调用。
 */
sds setTypeNextObject(setTypeIterator *si) {
    int64_t intele;
    sds sdsele;
    int encoding;

    //得到当前集合对象的编码类型
    encoding = setTypeNext(si,&sdsele,&intele);
    switch(encoding) {
        case -1:    return NULL;            //迭代完成
        case OBJ_ENCODING_INTSET:           //整数集合返回一个字符串类型的对象
            return sdsfromlonglong(intele);
        case OBJ_ENCODING_HT:               //字典集合，返回共享的该对象
            return sdsdup(sdsele);
        default:
            serverPanic("Unsupported encoding");
    }
    return NULL; /* just to suppress warnings */
}

/* Return random element from a non empty set.
 *
 * 从非空集合中随机取出一个元素。
 *
 * The returned element can be a int64_t value if the set is encoded
 * as an "intset" blob of integers, or a redis object if the set
 * is a regular set.
 *
 * 如果集合的编码为 intset ，那么将元素指向 int64_t 指针 llele 。
 * 如果集合的编码为 HT ，那么将元素对象指向对象指针 objele 。
 *
 * The caller provides both pointers to be populated with the right
 * object. The return value of the function is the object->encoding
 * field of the object and is used by the caller to check if the
 * int64_t pointer or the redis object pointer was populated.
 *
 * 函数的返回值为集合的编码方式，通过这个返回值可以知道那个指针保存了元素的值。
 *
 * When an object is returned (the set was a real set) the ref count
 * of the object is not incremented so this function can be considered
 * copy on write friendly.
 *
 * 因为被返回的对象是没有被增加引用计数的，
 * 所以这个函数是对 copy-on-write 友好的。
 */
int setTypeRandomElement(robj *setobj, sds *sdsele, int64_t *llele) {
    //从字典中返回
    if (setobj->encoding == OBJ_ENCODING_HT) {
        dictEntry *de = dictGetFairRandomKey(setobj->ptr);  //随机返回一个节点的地址
        *sdsele = dictGetKey(de);                           //取出该节点的哈希键保存到参数中
        *llele = -123456789; /* Not needed. Defensive. */
    } else if (setobj->encoding == OBJ_ENCODING_INTSET) {   //随机返回一个集合元素保存在参数中
        *llele = intsetRandom(setobj->ptr);
        *sdsele = NULL; /* Not needed. Defensive. */
    } else {
        serverPanic("Unknown set encoding");
    }
    return setobj->encoding;        //返回集合的编码类型
}

/* 返回集合的元素数量 */
unsigned long setTypeSize(const robj *subject) {
    //返回字典中的节点数量
    if (subject->encoding == OBJ_ENCODING_HT) {
        return dictSize((const dict*)subject->ptr);
    //返回整数集合中的元素数量
    } else if (subject->encoding == OBJ_ENCODING_INTSET) {
        return intsetLen((const intset*)subject->ptr);
    } else {
        serverPanic("Unknown set encoding");
    }
}

/* Convert the set to specified encoding.
 *
 * 将集合对象 setobj 的编码转换为 REDIS_ENCODING_HT 。
 *
 * The resulting dict (when converting to a hash table)
 * is presized to hold the number of elements in the original set.
 *
 * 新创建的结果字典会被预先分配为和原来的集合一样大。
 */
void setTypeConvert(robj *setobj, int enc) {
    setTypeIterator *si;
    serverAssertWithInfo(NULL,setobj,setobj->type == OBJ_SET &&
                             setobj->encoding == OBJ_ENCODING_INTSET);

    // 转换成OBJ_ENCODING_HT字典类型的编码
    if (enc == OBJ_ENCODING_HT) {
        int64_t intele;
        //创建一个字典
        dict *d = dictCreate(&setDictType,NULL);
        sds element;

        /* Presize the dict to avoid rehashing */
        // 扩展字典的大小
        dictExpand(d,intsetLen(setobj->ptr));

        /* To add the elements we extract integers and create redis objects */

        //创建并初始化一个集合类型的迭代器
        si = setTypeInitIterator(setobj);

        // 迭代器整数集合
        while (setTypeNext(si,&element,&intele) != -1) {

            // 将当前集合中的元素转换为字符串类型对象
            element = sdsfromlonglong(intele);
            serverAssert(dictAdd(d,element,NULL) == DICT_OK);
        }

        // 释放迭代器空间
        setTypeReleaseIterator(si);

        // 设置转换后的集合对象的编码类型
        setobj->encoding = OBJ_ENCODING_HT;
        // 更新集合对象的值对象
        zfree(setobj->ptr);
        setobj->ptr = d;
    } else {
        serverPanic("Unsupported set conversion");
    }
}

/*
 * SADD key member [member ...]
 * SADD命令的实现
 * */
void saddCommand(client *c) {
    robj *set;
    int j, added = 0;

    // 以写的方式取出集合对象
    set = lookupKeyWrite(c->db,c->argv[1]);

    // 如果当前key不存在，则创建一个集合对象，并将新建的集合对象加入到数据库中
    if (set == NULL) {
        set = setTypeCreate(c->argv[2]->ptr);
        dbAdd(c->db,c->argv[1],set);
        // 对象存在，检查集合的类型
    } else {
        // 如果取出的对象不是集合类型，则发送类型错误信息
        if (set->type != OBJ_SET) {
            addReply(c,shared.wrongtypeerr);
            return;
        }
    }

    // 遍历所有的member
    for (j = 2; j < c->argc; j++) {
        // 将当前member加入到集合中
        if (setTypeAdd(set,c->argv[j]->ptr)) added++;
    }

    // 如果有至少一个元素被成功添加，那么执行以下程序
    if (added) {
        // 键被修改需要发送信号
        signalModifiedKey(c,c->db,c->argv[1]);
        // 发送"sadd"事件通知
        notifyKeyspaceEvent(NOTIFY_SET,"sadd",c->argv[1],c->db->id);
    }

    //更新脏键
    server.dirty += added;

    //发送添加成功的个数给client
    addReplyLongLong(c,added);
}

//SREM key member [member ...]
//SREM命令的实现
void sremCommand(client *c) {
    robj *set;
    int j, deleted = 0, keyremoved = 0;

    //以写操作读取出集合对象并检查类型
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,OBJ_SET)) return;

    //遍历所有要删除的member
    for (j = 2; j < c->argc; j++) {
        //删除成功，更新删除成功的计数器
        if (setTypeRemove(set,c->argv[j]->ptr)) {
            deleted++;
            //如果集合被删除空，则讲集合对象从数据库中删除
            if (setTypeSize(set) == 0) {
                dbDelete(c->db,c->argv[1]);
                keyremoved = 1;     //设置key被删除的标志
                break;
            }
        }
    }
    // 只要删除一个元素成功
    if (deleted) {
        // 键被修改需要发送信号，发送"srem"事件通知
        signalModifiedKey(c,c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_SET,"srem",c->argv[1],c->db->id);
        // 如果删除了集合对象，发送"del"事件通知
        if (keyremoved)
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],
                                c->db->id);
        server.dirty += deleted;    //更新脏键
    }
    addReplyLongLong(c,deleted);    //发送删除的元素个数给client
}

// SMOVE source destination member
// SMOVE命令实现
void smoveCommand(client *c) {
    robj *srcset, *dstset, *ele;
    //以读操作取出srcset源集合和dstset目标集合的集合对象
    srcset = lookupKeyWrite(c->db,c->argv[1]);
    dstset = lookupKeyWrite(c->db,c->argv[2]);
    //尝试对要移动的元素进行优化编码
    ele = c->argv[3];

    /* If the source key does not exist return 0 */
    //源集合对象不存在则发送0给client
    if (srcset == NULL) {
        addReply(c,shared.czero);
        return;
    }

    /* If the source key has the wrong type, or the destination key
     * is set and has the wrong type, return with an error. */
    // 如果源集合的类型错误
    // 或者目标集合存在、但是类型错误
    // 那么直接返回
    if (checkType(c,srcset,OBJ_SET) ||
        (dstset && checkType(c,dstset,OBJ_SET))) return;

    /* If srcset and dstset are equal, SMOVE is a no-op */
    // 目标和源集合是同一个集合
    // 如果是要移动的元素存在，则发送1否则发送0给client
    if (srcset == dstset) {
        addReply(c,setTypeIsMember(srcset,ele->ptr) ?
            shared.cone : shared.czero);
        return;
    }

    /* If the element cannot be removed from the src set, return 0. */
    // 如果源集合和目标集合相等，那么直接返回
    if (!setTypeRemove(srcset,ele->ptr)) {
        addReply(c,shared.czero);
        return;
    }

    //从源集合删除成功，发送"srem"事件通知
    notifyKeyspaceEvent(NOTIFY_SET,"srem",c->argv[1],c->db->id);

    /* Remove the src set from the database when empty */
    //如果源集合被删除空，则删除从数据库中元集合对象，并发送"del"事件通知
    if (setTypeSize(srcset) == 0) {
        // 删除集合对象
        dbDelete(c->db,c->argv[1]);
        // 发送事件通知
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }

    /* Create the destination set when it doesn't exist */
    //当目标集合不存在，则新创建一个目标集合，并将新建的集合加入到数据库中
    if (!dstset) {
        dstset = setTypeCreate(ele->ptr);
        dbAdd(c->db,c->argv[2],dstset);
    }

    //键被修改需要发送信号，并更新脏键
    signalModifiedKey(c,c->db,c->argv[1]);
    signalModifiedKey(c,c->db,c->argv[2]);
    server.dirty++;

    /* An extra key has changed when ele was successfully added to dstset */
    // 将元素添加到目标集合
    if (setTypeAdd(dstset,ele->ptr)) {
        //更新脏键，并发送"sadd"事件通知
        server.dirty++;
        notifyKeyspaceEvent(NOTIFY_SET,"sadd",c->argv[2],c->db->id);
    }

    //发送1给client
    addReply(c,shared.cone);
}

// SISMEMBER key member
// SISMEMBER 命令实现
void sismemberCommand(client *c) {
    robj *set;

    //以读操作读取出集合对象并检查编码类型
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,set,OBJ_SET)) return;

    //如果集合中存在member，则发送1否则发送0给client
    if (setTypeIsMember(set,c->argv[2]->ptr))
        addReply(c,shared.cone);
    else
        addReply(c,shared.czero);
}

// SCARD key
// SCARD 命令实现
void scardCommand(client *c) {
    robj *o;

    //以读操作读取出集合对象并检查编码类型
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_SET)) return;

    //发送集合元素的个数给client
    addReplyLongLong(c,setTypeSize(o));
}

/* Handle the "SPOP key <count>" variant. The normal version of the
 * command is handled by the spopCommand() function itself. */

/* How many times bigger should be the set compared to the remaining size
 * for us to use the "create new set" strategy? Read later in the
 * implementation for more info. */
#define SPOP_MOVE_STRATEGY_MUL 5

/*
 * SPOP key [count]
 * SPOP命令的变种实现
 * 从的设置值存储中删除并返回一个或多个随机成员key。
 * */
void spopWithCountCommand(client *c) {
    long l;
    unsigned long count, size;
    robj *set;

    /* Get the count argument */
    //将要弹出的元素个数保存在l中,并且涮带验证数量参数
    if (getLongFromObjectOrReply(c,c->argv[2],&l,NULL) != C_OK) return;
    //如果l为负数则发送越界错误信息
    if (l >= 0) {
        count = (unsigned long) l;
    } else {
        addReply(c,shared.outofrangeerr);
        return;
    }

    /* Make sure a key with the name inputted exists, and that it's type is
     * indeed a set. Otherwise, return nil */
    //以读操作取出集合对象并检查对象的编码类型
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.emptyset[c->resp]))
        == NULL || checkType(c,set,OBJ_SET)) return;

    /* If count is zero, serve an empty set ASAP to avoid special
     * cases later. */
    //count数量为0，之间发送空信息后返回
    if (count == 0) {
        addReply(c,shared.emptyset[c->resp]);
        return;
    }

    //获取集合的元素个数
    size = setTypeSize(set);

    /* Generate an SPOP keyspace notification */
    //发送"spop"的事件
    notifyKeyspaceEvent(NOTIFY_SET,"spop",c->argv[1],c->db->id);
    //更新脏键
    server.dirty += count;

    /* CASE 1:
     * The number of requested elements is greater than or equal to
     * the number of elements inside the set: simply return the whole set. */
    //1.如果弹出的元素数量大于集合中的元素数量
    if (count >= size) {
        /* We just return the entire set */
        //弹出整个集合的所有元素
        sunionDiffGenericCommand(c,c->argv+1,1,NULL,SET_OP_UNION);

        /* Delete the set as it is now empty */
        //将空的集合键从数据库中删除
        dbDelete(c->db,c->argv[1]);
        //发送"del"事件通知
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);

        /* Propagate this command as a DEL operation */
        //将该命令从client的参数列表中修改成del操作
        rewriteClientCommandVector(c,2,shared.del,c->argv[1]);
        //键被修改需要发送信号，并更新脏键
        signalModifiedKey(c,c->db,c->argv[1]);
        server.dirty++;
        return;
    }

    /* Case 2 and 3 require to replicate SPOP as a set of SREM commands.
     * Prepare our replication argument vector. Also send the array length
     * which is common to both the code paths. */
    //第2和第3种情况需要将SPOP命令复制为SREM命令
    robj *propargv[3];      //参数列表
    propargv[0] = createStringObject("SREM",4);     //创建一个"SREM"命令字符串对象，设置参数列表的命令
    propargv[1] = c->argv[1];                                //设置参数列表的key
    addReplySetLen(c,count);                                 //发送要弹出元素个数给client

    /* Common iteration vars. */
    sds sdsele;
    robj *objele;
    int encoding;
    int64_t llele;
    //剩下的元素数量
    unsigned long remaining = size-count; /* Elements left after SPOP. */

    /* If we are here, the number of requested elements is less than the
     * number of elements inside the set. Also we are sure that count < size.
     * Use two different strategies.
     *
     * CASE 2: The number of elements to return is small compared to the
     * set size. We can just extract random elements and return them to
     * the set. */
    // 执行到这里，我们确保要弹出的元素count一定小于集合的元素数量，下面展示了两种不同的删除策略
    // remaining × 5 > count，如果剩下的元素大于弹出元素数量的五分之一
    if (remaining*SPOP_MOVE_STRATEGY_MUL > count) {
        //遍历count次数
        while(count--) {
            /* Emit and remove. */
            // 从集合中随机弹出一个元素
            encoding = setTypeRandomElement(set,&objele,&llele);
            // 根据不同的编码类型，创建发送给client的对象，对象的值为弹出的元素的值
            // 将弹出的元素对象从源集合中删除
            if (encoding == OBJ_ENCODING_INTSET) {
                addReplyBulkLongLong(c,llele);
                objele = createStringObjectFromLongLong(llele);
                set->ptr = intsetRemove(set->ptr,llele,NULL);
            } else {
                addReplyBulkCBuffer(c,sdsele,sdslen(sdsele));
                objele = createStringObject(sdsele,sdslen(sdsele));
                setTypeRemove(set,sdsele);
            }

            /* Replicate/AOF this command as an SREM operation */
            //将SREM命令传播到AOF和REPL
            propargv[2] = objele;
            alsoPropagate(server.sremCommand,c->db->id,propargv,3,
                PROPAGATE_AOF|PROPAGATE_REPL);
            //释放创建的临时对象
            decrRefCount(objele);
        }
    } else {
    /* CASE 3: The number of elements to return is very big, approaching
     * the size of the set itself. After some time extracting random elements
     * from such a set becomes computationally expensive, so we use
     * a different strategy, we extract random elements that we don't
     * want to return (the elements that will remain part of the set),
     * creating a new set as we do this (that will be stored as the original
     * set). Then we return the elements left in the original set and
     * release it. */
    //剩下的元素不到被删除元素的五分之一，意味着弹出元素的数量几乎快和集合元素数量相等
        robj *newset = NULL;

        /* Create a new set with just the remaining elements. */
        //创建一个新的集合保存剩下的元素
        while(remaining--) {
            //弹出一个元素
            encoding = setTypeRandomElement(set,&sdsele,&llele);
            //根据不同的编码类型，将弹出的元素封装成对象
            if (encoding == OBJ_ENCODING_INTSET) {
                sdsele = sdsfromlonglong(llele);
            } else {
                sdsele = sdsdup(sdsele);
            }
            //创建一个新集合
            if (!newset) newset = setTypeCreate(sdsele);
            //将弹出元素的对象加入到新集合中
            setTypeAdd(newset,sdsele);
            //将弹出的元素对象从源集合中删除
            setTypeRemove(set,sdsele);
            //释放创建的临时对象
            sdsfree(sdsele);
        }

        /* Transfer the old set to the client. */
        setTypeIterator *si;
        //创建并初始化一个集合类型的迭代器
        si = setTypeInitIterator(set);
        //迭代原来的集合
        while((encoding = setTypeNext(si,&sdsele,&llele)) != -1) {
            //根据不同的编码类型，创建弹出值的对象
            if (encoding == OBJ_ENCODING_INTSET) {
                addReplyBulkLongLong(c,llele);
                objele = createStringObjectFromLongLong(llele);
            } else {
                addReplyBulkCBuffer(c,sdsele,sdslen(sdsele));
                objele = createStringObject(sdsele,sdslen(sdsele));
            }

            /* Replicate/AOF this command as an SREM operation */
            propargv[2] = objele;
            alsoPropagate(server.sremCommand,c->db->id,propargv,3,
                PROPAGATE_AOF|PROPAGATE_REPL);
            decrRefCount(objele);
        }
        //释放迭代器
        setTypeReleaseIterator(si);

        /* Assign the new set as the key value. */
        dbOverwrite(c->db,c->argv[1],newset);
    }

    /* Don't propagate the command itself even if we incremented the
     * dirty counter. We don't want to propagate an SPOP command since
     * we propagated the command as a set of SREMs operations using
     * the alsoPropagate() API. */
    decrRefCount(propargv[0]);
    preventCommandPropagation(c);
    //发送信号和更新脏键
    signalModifiedKey(c,c->db,c->argv[1]);
    server.dirty++;
}

// SPOP key
// SPOP命令的实现
void spopCommand(client *c) {
    robj *set, *ele, *aux;
    sds sdsele;
    int64_t llele;
    int encoding;

    //有count参数时，弹出并移除多个元素
    if (c->argc == 3) {
        spopWithCountCommand(c);
        return;
        //参数太多，发送错误信息
    } else if (c->argc > 3) {
        addReply(c,shared.syntaxerr);
        return;
    }

    /* Make sure a key with the name inputted exists, and that it's type is
     * indeed a set */
    //以写方式获取集合对象并检查对象的数据类型
    if ((set = lookupKeyWriteOrReply(c,c->argv[1],shared.null[c->resp]))
         == NULL || checkType(c,set,OBJ_SET)) return;

    /* Get a random element from the set */
    //随机弹出一个元素保存在参数中
    encoding = setTypeRandomElement(set,&sdsele,&llele);

    /* Remove the element from the set */
    //从集合中将弹出元素删除，需要先讲元素构建成对象
    if (encoding == OBJ_ENCODING_INTSET) {
        ele = createStringObjectFromLongLong(llele);
        set->ptr = intsetRemove(set->ptr,llele,NULL);
    } else {
        ele = createStringObject(sdsele,sdslen(sdsele));
        setTypeRemove(set,ele->ptr);
    }

    //发送"spop"事件通知
    notifyKeyspaceEvent(NOTIFY_SET,"spop",c->argv[1],c->db->id);

    /* Replicate/AOF this command as an SREM operation */
    // 将这个命令当作 SREM 来传播，防止产生有害的随机性，导致数据不一致
    // （不同的服务器随机删除不同的元素）
    aux = createStringObject("SREM",4);
    rewriteClientCommandVector(c,3,aux,c->argv[1],ele);
    decrRefCount(aux);

    /* Add the element to the reply */
    //将弹出的元素发送给client
    addReplyBulk(c,ele);
    decrRefCount(ele);

    /* Delete the set if it's empty */
    //如果弹出元素后，集合变为空集合，则从数据库中删除集合
    if (setTypeSize(set) == 0) {
        // 删除集合
        dbDelete(c->db,c->argv[1]);
        // 发送事件通知
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],c->db->id);
    }

    /* Set has been modified */
    //键被修改，发送信号
    signalModifiedKey(c,c->db,c->argv[1]);

    //更新脏键
    server.dirty++;
}

/* handle the "SRANDMEMBER key <count>" variant. The normal version of the
 * command is handled by the srandmemberCommand() function itself.
 *
 * 实现 SRANDMEMBER key <count> 变种，
 * 原本的 SRANDMEMBER key 由 srandmemberCommand() 实现
 */

/* How many times bigger should be the set compared to the requested size
 * for us to don't use the "remove elements" strategy? Read later in the
 * implementation for more info.
 *
 * 如果 count 参数乘以这个常量所得的积，比集合的基数要大，
 * 那么程序就不使用“删除元素”的策略。
 *
 * 更多信息请参考后面的函数定义。
 */
#define SRANDMEMBER_SUB_STRATEGY_MUL 3

// SRANDMEMBER key [count]
// SRANDMEMBER 命令的变种实现
void srandmemberWithCountCommand(client *c) {
    long l;
    unsigned long count, size;
    int uniq = 1;   //集合元素唯一性标志
    robj *set;
    sds ele;
    int64_t llele;
    int encoding;

    dict *d;

    // 将要弹出的元素个数保存在l中
    if (getLongFromObjectOrReply(c,c->argv[2],&l,NULL) != C_OK) return;
    // 如果个数大于0，则表示返回不相同的元素
    if (l >= 0) {
        count = (unsigned long) l;
    } else {
        /* A negative count means: return the same elements multiple times
         * (i.e. don't remove the extracted element after every extraction). */
        // 如果为负数，则表示返回的结果可以有重复的元素
        count = -l;
        uniq = 0;   //将元素唯一标志置位0
    }

    // 以读操作取出集合对象并检查对象的数据类型
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.emptyset[c->resp]))
        == NULL || checkType(c,set,OBJ_SET)) return;
    // 获取集合的元素个数
    size = setTypeSize(set);

    /* If count is zero, serve it ASAP to avoid special cases later. */
    // count为0，发送空信息直接返回
    if (count == 0) {
        addReply(c,shared.emptyset[c->resp]);
        return;
    }

    /* CASE 1: The count was negative, so the extraction method is just:
    * "return N random elements" sampling the whole set every time.
    * This case is trivial and can be served without auxiliary data
    * structures.
    *
    * 情形 1：count 为负数，结果集可以带有重复元素
    * 直接从集合中取出并返回 N 个随机元素就可以了
    *
    * 这种情形不需要额外的结构来保存结果集
    */
    if (!uniq) {
        addReplySetLen(c,count);//发送弹出数量的信息个client
        // 弹出count个元素
        while(count--) {
            // 随机弹出一个元素保存在参数中
            encoding = setTypeRandomElement(set,&ele,&llele);
            // 发送保存在参数中的元素值
            if (encoding == OBJ_ENCODING_INTSET) {
                addReplyBulkLongLong(c,llele);
            } else {
                addReplyBulkCBuffer(c,ele,sdslen(ele));
            }
        }
        return;
    }

    /* CASE 2:
     * The number of requested elements is greater than the number of
     * elements inside the set: simply return the whole set.
     *
     * 如果 count 比集合的基数要大，那么直接返回整个集合
     * */
    if (count >= size) {
        sunionDiffGenericCommand(c,c->argv+1,1,NULL,SET_OP_UNION);
        return;
    }

    /* For CASE 3 and CASE 4 we need an auxiliary dictionary. */
    // 为3和4创建一个字典
    d = dictCreate(&objectKeyPointerValueDictType,NULL);

    /* CASE 3:
     *
     * 情形 3：
     *
     * The number of elements inside the set is not greater than
     * SRANDMEMBER_SUB_STRATEGY_MUL times the number of requested elements.
     *
     * count 参数乘以 SRANDMEMBER_SUB_STRATEGY_MUL 的积比集合的基数要大。
     *
     * In this case we create a set from scratch with all the elements, and
     * subtract random elements to reach the requested number of elements.
     *
     * 在这种情况下，程序创建一个集合的副本，
     * 并从集合中删除元素，直到集合的基数等于 count 参数指定的数量为止。
     *
     * This is done because if the number of requsted elements is just
     * a bit less than the number of elements in the set, the natural approach
     * used into CASE 3 is highly inefficient.
     *
     * 使用这种做法的原因是，当 count 的数量接近于集合的基数时，
     * 从集合中随机取出 count 个参数的方法是非常低效的。
     */
    if (count*SRANDMEMBER_SUB_STRATEGY_MUL > size) {
        setTypeIterator *si;

        /* Add all the elements into the temporary dictionary. */
        // 创建集合类型的迭代器
        si = setTypeInitIterator(set);
        // 遍历集合，将迭代器指向当前的元素保存在参数中
        while((encoding = setTypeNext(si,&ele,&llele)) != -1) {
            int retval = DICT_ERR;

            // 将参数中的元素值添加到字典d中
            if (encoding == OBJ_ENCODING_INTSET) {
                retval = dictAdd(d,createStringObjectFromLongLong(llele),NULL);
            } else {
                retval = dictAdd(d,createStringObject(ele,sdslen(ele)),NULL);
            }
            serverAssert(retval == DICT_OK);
        }
        // 释放迭代器
        setTypeReleaseIterator(si);
        serverAssert(dictSize(d) == size);

        /* Remove random elements to reach the right count. */
        // 随机删除元素，保留count个元素
        while(size > count) {
            dictEntry *de;

            de = dictGetRandomKey(d);       //随机返回一个节点
            dictDelete(d,dictGetKey(de));   //从字典中删除节点的键值对
            size--;
        }
    }

    /* CASE 4: We have a big set compared to the requested number of elements.
    *
    * 情形 4 ： count 参数要比集合基数小很多。
    *
    * In this case we can simply get random elements from the set and add
    * to the temporary set, trying to eventually get enough unique elements
    * to reach the specified count.
    *
    * 在这种情况下，我们可以直接从集合中随机地取出元素，
    * 并将它添加到结果集合中，直到结果集的基数等于 count 为止。
    */
    else {
        unsigned long added = 0;
        robj *objele;

        //循环弹出元素
        while(added < count) {
            //随机弹出一个元素保存在参数中
            encoding = setTypeRandomElement(set,&ele,&llele);
            //将弹出的元素构建成字符串对象
            if (encoding == OBJ_ENCODING_INTSET) {
                objele = createStringObjectFromLongLong(llele);
            } else {
                objele = createStringObject(ele,sdslen(ele));
            }
            /* Try to add the object to the dictionary. If it already exists
             * free it, otherwise increment the number of objects we have
             * in the result dictionary. */
            // 尝试将元素添加到字典中
            // dictAdd 只有在元素不存在于字典时，才会返回 1
            // 如果如果结果集已经有同样的元素，那么程序会执行 else 部分
            // 只有元素不存在于结果集时，添加才会成功
            if (dictAdd(d,objele,NULL) == DICT_OK)
                added++;        //更新添加元素对象成功的个数
            else
                decrRefCount(objele);
        }
    }

    /* CASE 3 & 4: send the result to the user. */
    //将结果返回给client
    {
        dictIterator *di;
        dictEntry *de;

        //发送弹出的个数给client
        addReplySetLen(c,count);
        //创建字典迭代器，遍历字典中的所有节点，将节点中的哈希值返回给client
        di = dictGetIterator(d);
        while((de = dictNext(di)) != NULL)
            addReplyBulk(c,dictGetKey(de));
        //释放迭代器和字典的空间
        dictReleaseIterator(di);
        dictRelease(d);
    }
}

/*
 * SRANDMEMBER key [count]
 * SRANDMEMBER命令实现
 * 如果 count 为正数，且小于集合基数，那么命令返回一个包含 count 个元素的数组，数组中的元素各不相同。如果 count 大于等于集合基数，那么返回整个集合。
 * 如果 count 为负数，那么命令返回一个数组，数组中的元素可能会重复出现多次，而数组的长度为 count 的绝对值。
 * 该操作和 SPOP 相似，但 SPOP 将随机元素从集合中移除并返回，而 Srandmember 则仅仅返回随机元素，而不对集合进行任何改动。
 * */
void srandmemberCommand(client *c) {
    robj *set;
    sds ele;
    int64_t llele;
    int encoding;

    // 如果带有 count 参数，那么调用 srandmemberWithCountCommand 来处理
    if (c->argc == 3) {
        srandmemberWithCountCommand(c);
        return;

        //发送语法错误信息
    } else if (c->argc > 3) {
        addReply(c,shared.syntaxerr);
        return;
    }

    //以读操作读取出集合对象并检查对象的数据类型
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp]))
        == NULL || checkType(c,set,OBJ_SET)) return;

    //随机返回一个元素，保存在参数中
    encoding = setTypeRandomElement(set,&ele,&llele);
    //发送参数中的值给client
    if (encoding == OBJ_ENCODING_INTSET) {
        addReplyBulkLongLong(c,llele);
    } else {
        addReplyBulkCBuffer(c,ele,sdslen(ele));
    }
}

/* 计算集合 s1 的基数减去集合 s2 的基数之差 */
int qsortCompareSetsByCardinality(const void *s1, const void *s2) {
    if (setTypeSize(*(robj**)s1) > setTypeSize(*(robj**)s2)) return 1;
    if (setTypeSize(*(robj**)s1) < setTypeSize(*(robj**)s2)) return -1;
    return 0;
}

/* This is used by SDIFF and in this case we can receive NULL that should
 * be handled as empty sets.
 *
 * 计算集合 s2 的基数减去集合 s1 的基数之差
 * */
int qsortCompareSetsByRevCardinality(const void *s1, const void *s2) {
    robj *o1 = *(robj**)s1, *o2 = *(robj**)s2;
    unsigned long first = o1 ? setTypeSize(o1) : 0;
    unsigned long second = o2 ? setTypeSize(o2) : 0;

    if (first < second) return 1;
    if (first > second) return -1;
    return 0;
}

// SINTER key [key ...]
// SINTERSTORE destination key [key ...]
// SINTER、SINTERSTORE一类命令的底层实现
void sinterGenericCommand(client *c, robj **setkeys,
                          unsigned long setnum, robj *dstkey) {
    //分配存储集合的数组
    robj **sets = zmalloc(sizeof(robj*)*setnum);
    setTypeIterator *si;
    robj *dstset = NULL;
    sds elesds;
    int64_t intobj;
    void *replylen = NULL;
    unsigned long j, cardinality = 0;
    int encoding;

    //遍历集合数组
    for (j = 0; j < setnum; j++) {
        // 如果dstkey为空，则是SINTER命令，不为空则是SINTERSTORE命令
        // 如果是SINTER命令，则以读操作读取出集合对象，否则以写操作读取出集合对象
        robj *setobj = dstkey ?
            lookupKeyWrite(c->db,setkeys[j]) :
            lookupKeyRead(c->db,setkeys[j]);

        // 读取的集合对象不存在，执行清理操作
        if (!setobj) {
            zfree(sets);    //释放集合数组空间
            // 如果是SINTERSTORE命令
            zfree(sets);
            if (dstkey) {
                // 从数据库中删除存储的目标集合对象dstkey
                if (dbDelete(c->db,dstkey)) {
                    // 发送信号表示数据库键被修改，并更新脏键
                    signalModifiedKey(c,c->db,dstkey);
                    server.dirty++;
                }
                addReply(c,shared.czero);//发送0给client
                // 如果是SINTER命令，发送空回复
            } else {
                addReply(c,shared.emptyset[c->resp]);
            }
            return;
        }
        if (checkType(c,setobj,OBJ_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;
    }
    /* Sort sets from the smallest to largest, this will improve our
     * algorithm's performance */
    //从小到大排序集合数组中的集合大小，能够提高算法的性能
    qsort(sets,setnum,sizeof(robj*),qsortCompareSetsByCardinality);

    /* The first thing we should output is the total number of elements...
     * since this is a multi-bulk write, but at this stage we don't know
     * the intersection set size, so we use a trick, append an empty object
     * to the output list and save the pointer to later modify it with the
     * right length */
    // 因为不知道结果集会有多少个元素，所有没有办法直接设置回复的数量
    // 这里使用了一个小技巧，直接使用一个 BUFF 列表，
    // 然后将之后的回复都添加到列表中
    if (!dstkey) {
        replylen = addReplyDeferredLen(c);  // STINER命令创建一个链表
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with an empty set inside */
        dstset = createIntsetObject();  //STINERSTORE命令创建要给整数集合对象
    }

    /* Iterate all the elements of the first (smallest) set, and test
     * the element against all the other sets, if at least one set does
     * not include the element it is discarded */
    // 迭代第一个也是集合元素数量最小的集合的每一个元素，将该集合中的所有元素和其他集合作比较
    // 如果至少有一个集合不包括该元素，则该元素不属于交集
    si = setTypeInitIterator(sets[0]);
    // 创建集合类型的迭代器并迭代器集合数组中的第一个集合的所有元素
    while((encoding = setTypeNext(si,&elesds,&intobj)) != -1) {
        for (j = 1; j < setnum; j++) {
            if (sets[j] == sets[0]) continue;
            // 当前元素为INTSET类型
            if (encoding == OBJ_ENCODING_INTSET) {
                /* intset with intset is simple... and fast */
                //如果在当前intset集合中没有找到该元素则直接跳过当前元素，操作下一个元素
                if (sets[j]->encoding == OBJ_ENCODING_INTSET &&
                    !intsetFind((intset*)sets[j]->ptr,intobj))
                {
                    break;
                /* in order to compare an integer with an object we
                 * have to use the generic function, creating an object
                 * for this */
                //在字典中查找
                } else if (sets[j]->encoding == OBJ_ENCODING_HT) {
                    //创建字符串对象
                    elesds = sdsfromlonglong(intobj);
                    //如果当前元素不是当前集合中的元素，则释放字符串对象跳过for循环体，操作下一个元素
                    if (!setTypeIsMember(sets[j],elesds)) {
                        sdsfree(elesds);
                        break;
                    }
                    sdsfree(elesds);
                }
                //当前元素为HT字典类型
            } else if (encoding == OBJ_ENCODING_HT) {
                if (!setTypeIsMember(sets[j],elesds)) {
                    break;
                }
            }
        }

        /* Only take action when all sets contain the member */
        //执行到这里，该元素为结果集合中的元素
        if (j == setnum) {
            //如果是SINTER命令，回复集合
            if (!dstkey) {
                if (encoding == OBJ_ENCODING_HT)
                    addReplyBulkCBuffer(c,elesds,sdslen(elesds));
                else
                    addReplyBulkLongLong(c,intobj);
                cardinality++;
                //如果是SINTERSTORE命令，先将结果添加到集合中，因为还要store到数据库中
            } else {
                if (encoding == OBJ_ENCODING_INTSET) {
                    elesds = sdsfromlonglong(intobj);
                    setTypeAdd(dstset,elesds);
                    sdsfree(elesds);
                } else {
                    setTypeAdd(dstset,elesds);
                }
            }
        }
    }
    setTypeReleaseIterator(si);//释放迭代器

    // SINTERSTORE命令，要将结果的集合添加到数据库中
    if (dstkey) {
        /* Store the resulting set into the target, if the intersection
         * is not an empty set. */
        //如果之前存在该集合则先删除
        int deleted = dbDelete(c->db,dstkey);
        //结果集大小非空，则将其添加到数据库中
        if (setTypeSize(dstset) > 0) {
            dbAdd(c->db,dstkey,dstset);
            // 回复结果集的大小
            addReplyLongLong(c,setTypeSize(dstset));
            // 发送"sinterstore"事件通知
            notifyKeyspaceEvent(NOTIFY_SET,"sinterstore",
                dstkey,c->db->id);
            // 结果集为空，释放空间
        } else {
            decrRefCount(dstset);
            // 发送0给client
            addReply(c,shared.czero);
            // 发送"del"事件通知
            if (deleted)
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                    dstkey,c->db->id);
        }
        // 键被修改，发送信号。更新脏键
        signalModifiedKey(c,c->db,dstkey);
        server.dirty++;
    } else {
        // SINTER命令，回复结果集合给client
        setDeferredSetLen(c,replylen,cardinality);
    }
    //释放集合数组空间
    zfree(sets);
}

/*
 * SINTER key [key …]
 * 时间复杂度: O(N * M)， N 为给定集合当中基数最小的集合， M 为给定集合的个数。
 * 返回一个集合的全部成员，该集合是所有给定集合的交集。
 * */
void sinterCommand(client *c) {
    sinterGenericCommand(c,c->argv+1,c->argc-1,NULL);
}

/*
 * SINTERSTORE destination key [key …]
 * 时间复杂度: O(N * M)， N 为给定集合当中基数最小的集合， M 为给定集合的个数。
 * 这个命令类似于 SINTER key [key …] 命令，但它将结果保存到 destination 集合，而不是简单地返回结果集。
 * */
void sinterstoreCommand(client *c) {
    sinterGenericCommand(c,c->argv+2,c->argc-2,c->argv[1]);
}

#define SET_OP_UNION 0
#define SET_OP_DIFF 1
#define SET_OP_INTER 2

/*
 * SUNION key [key ...]
 * SUNIONSTORE destination key [key ...]
 * SDIFF key [key ...]
 * SDIFFSTORE destination key [key ...]
 * 并集、差集命令的底层实现
 * */
void sunionDiffGenericCommand(client *c, robj **setkeys, int setnum,
                              robj *dstkey, int op) {
    //分配集合数组的空间
    robj **sets = zmalloc(sizeof(robj*)*setnum);
    setTypeIterator *si;
    robj *dstset = NULL;
    sds ele;
    int j, cardinality = 0;
    int diff_algo = 1;

    // 遍历数组中集合键对象
    for (j = 0; j < setnum; j++) {
        // 如果dstkey为空，则是SUNION或SDIFF命令，不为空则是SUNIONSTORE或SDIFFSTORE命令
        // 如果是SUNION或SDIFF命令，则以读操作读取出集合对象，否则以写操作读取出集合对象
        robj *setobj = dstkey ?
            lookupKeyWrite(c->db,setkeys[j]) :
            lookupKeyRead(c->db,setkeys[j]);
        // 不存在的集合键设置为空
        if (!setobj) {
            sets[j] = NULL;
            continue;
        }
        // 检查存在的集合键是否是集合对象，不是则释放空间
        if (checkType(c,setobj,OBJ_SET)) {
            zfree(sets);
            return;
        }
        sets[j] = setobj;   //保存到集合数组中
    }

    /* Select what DIFF algorithm to use.
      *
      * 选择使用那个算法来执行计算
      *
      * Algorithm 1 is O(N*M) where N is the size of the element first set
      * and M the total number of sets.
      *
      * 算法 1 的复杂度为 O(N*M) ，其中 N 为第一个集合的基数，
      * 而 M 则为其他集合的数量。
      *
      * Algorithm 2 is O(N) where N is the total number of elements in all
      * the sets.
      *
      * 算法 2 的复杂度为 O(N) ，其中 N 为所有集合中的元素数量总数。
      *
      * We compute what is the best bet with the current input here.
      *
      * 程序通过考察输入来决定使用那个算法
      */
    if (op == SET_OP_DIFF && sets[0]) {
        long long algo_one_work = 0, algo_two_work = 0;

        //遍历集合数组
        for (j = 0; j < setnum; j++) {
            if (sets[j] == NULL) continue;

            // 计算sets[0] × setnum的值
            algo_one_work += setTypeSize(sets[0]);
            // 计算所有集合的元素总个数
            algo_two_work += setTypeSize(sets[j]);
        }

        /* Algorithm 1 has better constant times and performs less operations
         * if there are elements in common. Give it some advantage. */
        // 算法 1 的常数比较低，优先考虑算法 1
        algo_one_work /= 2;
        //根据algo_one_work和algo_two_work选择不同算法
        diff_algo = (algo_one_work <= algo_two_work) ? 1 : 2;

        //如果是算法1，M较小，执行操作少
        if (diff_algo == 1 && setnum > 1) {
            /* With algorithm 1 it is better to order the sets to subtract
             * by decreasing size, so that we are more likely to find
             * duplicated elements ASAP. */
            // 如果使用的是算法 1 ，那么最好对 sets[0] 以外的其他集合进行排序
            // 这样有助于优化算法的性能
            qsort(sets+1,setnum-1,sizeof(robj*),
                qsortCompareSetsByRevCardinality);
        }
    }

    /* We need a temp set object to store our union. If the dstkey
    * is not NULL (that is, we are inside an SUNIONSTORE operation) then
    * this set object will be the resulting object to set into the target key
    *
    * 使用一个临时集合来保存结果集，如果程序执行的是 SUNIONSTORE 命令，
    * 那么这个结果将会成为将来的集合值对象。
    */
    dstset = createIntsetObject();

    //执行并集操作
    if (op == SET_OP_UNION) {
        /* Union is trivial, just add every element of every set to the
         * temporary set. */
        // 仅仅讲每一个集合中的每一个元素加入到结果集中
        // 遍历每一个集合
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue; /* non existing keys are like empty sets */

            // 创建一个集合类型的迭代器
            si = setTypeInitIterator(sets[j]);
            // 遍历当前集合中的所有元素
            while((ele = setTypeNextObject(si)) != NULL) {
                // 讲迭代器指向的当前元素对象加入到结果集中
                if (setTypeAdd(dstset,ele)) cardinality++;//如果结果集中不存在新加入的元素，则更新结果集的元素个数计数器
                sdsfree(ele);
            }
            setTypeReleaseIterator(si);//释放迭代器空间
        }
        // 执行差集操作并且使用算法1
    } else if (op == SET_OP_DIFF && sets[0] && diff_algo == 1) {
        /* DIFF Algorithm 1:
         *
         * 差集算法 1 ：
         *
         * We perform the diff by iterating all the elements of the first set,
         * and only adding it to the target set if the element does not exist
         * into all the other sets.
         *
         * 程序遍历 sets[0] 集合中的所有元素，
         * 并将这个元素和其他集合的所有元素进行对比，
         * 只有这个元素不存在于其他所有集合时，
         * 才将这个元素添加到结果集。
         *
         * This way we perform at max N*M operations, where N is the size of
         * the first set, and M the number of sets.
         *
         * 这个算法执行最多 N*M 步， N 是第一个集合的基数，
         * 而 M 是其他集合的数量。
         */
        si = setTypeInitIterator(sets[0]);
        // 创建集合类型迭代器遍历第一个集合中的所有元素
        while((ele = setTypeNextObject(si)) != NULL) {
            // 遍历集合数组中的除了第一个的所有集合，检查元素是否存在在每一个集合
            for (j = 1; j < setnum; j++) {
                if (!sets[j]) continue; /* no key is an empty set. */   //集合键不存在跳过本次循环
                if (sets[j] == sets[0]) break; /* same set! */          //相同的集合没必要比较
                if (setTypeIsMember(sets[j],ele)) break;                //如果元素存在后面的集合中，遍历下一个元素
            }
            //执行到这里，说明当前元素不存在于 除了第一个的所有集合
            if (j == setnum) {
                /* There is no other set with this element. Add it. */
                //因此将当前元素添加到结果集合中，更新计数器
                setTypeAdd(dstset,ele);
                cardinality++;
            }
            sdsfree(ele);//释放元素对象空间
        }
        setTypeReleaseIterator(si);//释放迭代器空间
        //执行差集操作并且使用算法2
    } else if (op == SET_OP_DIFF && sets[0] && diff_algo == 2) {
        /* DIFF Algorithm 2:
        *
        * 差集算法 2 ：
        *
        * Add all the elements of the first set to the auxiliary set.
        * Then remove all the elements of all the next sets from it.
        *
        * 将 sets[0] 的所有元素都添加到结果集中，
        * 然后遍历其他所有集合，将相同的元素从结果集中删除。
        *
        * This is O(N) where N is the sum of all the elements in every set.
        *
        * 算法复杂度为 O(N) ，N 为所有集合的基数之和。
        */
        for (j = 0; j < setnum; j++) {
            if (!sets[j]) continue; /* non existing keys are like empty sets */

            si = setTypeInitIterator(sets[j]);
            //创建集合类型迭代器遍历每一个集合中的所有元素
            while((ele = setTypeNextObject(si)) != NULL) {
                //如果是第一个集合，将每一个元素加入到结果集中
                if (j == 0) {
                    if (setTypeAdd(dstset,ele)) cardinality++;
                    // 如果是其后的集合，将当前元素从结果集中删除，如结果集中有的话
                } else {
                    if (setTypeRemove(dstset,ele)) cardinality--;
                }
                sdsfree(ele);
            }
            setTypeReleaseIterator(si);//释放迭代器空间

            /* Exit if result set is empty as any additional removal
             * of elements will have no effect. */
            if (cardinality == 0) break;//只要结果集为空，那么差集结果就为空，不用比较后续的集合
        }
    }

    /* Output the content of the resulting set, if not in STORE mode */
    // 执行的是 SDIFF 或者 SUNION
    // 打印结果集中的所有元素
    if (!dstkey) {

        //发送结果集的元素个数给client
        addReplySetLen(c,cardinality);

        // 遍历结果集中的每一个元素，并发送给client
        si = setTypeInitIterator(dstset);
        while((ele = setTypeNextObject(si)) != NULL) {
            addReplyBulkCBuffer(c,ele,sdslen(ele));
            // 发送完要释放空间
            sdsfree(ele);
        }

        // 释放迭代器
        setTypeReleaseIterator(si);

        //发送集合后要释放结果集的空间
        server.lazyfree_lazy_server_del ? freeObjAsync(dstset) :
                                          decrRefCount(dstset);

        // 执行的是 SDIFFSTORE 或者 SUNIONSTORE
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with the result set inside */
        // 先将目标集合从数据库中删除，如果存在的话
        int deleted = dbDelete(c->db,dstkey);
        // 如果结果集合非空
        if (setTypeSize(dstset) > 0) {
            dbAdd(c->db,dstkey,dstset);     //将结果集加入到数据库中
            addReplyLongLong(c,setTypeSize(dstset));    //发送结果集的元素个数给client
            // 发送对应的事件通知
            notifyKeyspaceEvent(NOTIFY_SET,
                op == SET_OP_UNION ? "sunionstore" : "sdiffstore",
                dstkey,c->db->id);
            //结果集为空，则释放空间
        } else {
            decrRefCount(dstset);
            addReply(c,shared.czero);   //发送0给client
            // 发送"del"事件通知
            if (deleted)
                notifyKeyspaceEvent(NOTIFY_GENERIC,"del",
                    dstkey,c->db->id);
        }
        //键被修改，发送信号通知，更新脏键
        signalModifiedKey(c,c->db,dstkey);
        server.dirty++;
    }
    zfree(sets);//释放集合数组空间
}

// SUNION key [key ...]
// SUNION命令实现
// 时间复杂度: O(N)， N 是所有给定集合的成员数量之和。
// 返回一个集合的全部成员，该集合是所有给定集合的并集。
void sunionCommand(client *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,SET_OP_UNION);
}

// SUNIONSTORE destination key [key ...]
// SUNIONSTORE命令实现
void sunionstoreCommand(client *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],SET_OP_UNION);
}

// SDIFF key [key ...]
// SDIFF命令实现
void sdiffCommand(client *c) {
    sunionDiffGenericCommand(c,c->argv+1,c->argc-1,NULL,SET_OP_DIFF);
}

// SDIFFSTORE destination key [key ...]
// SDIFFSTORE命令实现
void sdiffstoreCommand(client *c) {
    sunionDiffGenericCommand(c,c->argv+2,c->argc-2,c->argv[1],SET_OP_DIFF);
}

// SSCAN key cursor [MATCH pattern] [COUNT count]
// SSCAN命令实
void sscanCommand(client *c) {
    robj *set;
    unsigned long cursor;

    // 获取scan的游标
    if (parseScanCursorOrReply(c,c->argv[2],&cursor) == C_ERR) return;
    // 以读操作读取集合对象，并且检查其数据类型
    if ((set = lookupKeyReadOrReply(c,c->argv[1],shared.emptyscan)) == NULL ||
        checkType(c,set,OBJ_SET)) return;
    scanGenericCommand(c,set,cursor);
}
