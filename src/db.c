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
#include "cluster.h"
#include "atomicvar.h"

#include <signal.h>
#include <ctype.h>

/*-----------------------------------------------------------------------------
 * C-level DB API
 *----------------------------------------------------------------------------*/

int keyIsExpired(redisDb *db, robj *key);

/* Update LFU when an object is accessed.
 * Firstly, decrement the counter if the decrement time is reached.
 * Then logarithmically increment the counter, and update the access time. */
/* 更新LFU淘汰算法 */
void updateLFU(robj *val) {
    unsigned long counter = LFUDecrAndReturn(val);
    counter = LFULogIncr(counter);
    val->lru = (LFUGetTimeInMinutes()<<8) | counter;
}

/* Low level key lookup API, not actually called directly from commands
 * implementations that should instead rely on lookupKeyRead(),
 * lookupKeyWrite() and lookupKeyReadWithFlags(). */
/*
 * 低级别的key查找方法
 * 该函数被lookupKeyRead()和lookupKeyWrite()和lookupKeyReadWithFlags()调用
 * 从数据库db中取出key的值对象，如果存在返回该对象，否则返回NULL
 * 返回key对象的值对象
 * */
robj *lookupKey(redisDb *db, robj *key, int flags) {
    //在数据库中查找key对象，返回保存该key的节点地址
    dictEntry *de = dictFind(db->dict,key->ptr);
    // 节点存在
    if (de) {
        // 获取字典对象的值
        robj *val = dictGetVal(de);

        /* Update the access time for the ageing algorithm.
         * Don't do it if we have a saving child, as this will trigger
         * a copy on write madness. */
        //更新key的最新访问时间
        if (!hasActiveChildProcess() && !(flags & LOOKUP_NOTOUCH)){
            if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {//当前的淘汰算法为LFU：最近最少使用，跟使用的次数有关，淘汰使用次数最少的。
                updateLFU(val);
            } else {//当前的淘汰算法为LRU：最近最不经常使用，跟使用的最后一次时间有关，淘汰最近使用时间离现在最久的。
                val->lru = LRU_CLOCK();
            }
        }
        return val;
    } else {
        return NULL;
    }
}

/* Lookup a key for read operations, or return NULL if the key is not found
 * in the specified DB.
 *
 * As a side effect of calling this function:
 * 1. A key gets expired if it reached it's TTL.
 * 2. The key last access time is updated.
 * 3. The global keys hits/misses stats are updated (reported in INFO).
 * 4. If keyspace notifications are enabled, a "keymiss" notification is fired.
 *
 * This API should not be used when we write to the key after obtaining
 * the object linked to the key, but only for read only operations.
 *
 * Flags change the behavior of this command:
 *
 *  LOOKUP_NONE (or zero): no special flags are passed.
 *  LOOKUP_NOTOUCH: don't alter the last access time of the key.
 *
 * Note: this function also returns NULL if the key is logically expired
 * but still existing, in case this is a slave, since this API is called only
 * for read operations. Even if the key expiry is master-driven, we can
 * correctly report a key is expired on slaves even if the master is lagging
 * expiring our key via DELs in the replication link. */
/*
 * 以读操作取出key的值对象，没找到返回NULL
 * 调用该函数的副作用如下：
 * 1.如果一个键的到达过期时间TTL，该键被设置为过期的
 * 2.键的使用时间信息被更新
 * 3.全局键 hits/misses 状态被更新
 * 注意：如果键在逻辑上已经过期但是仍然存在，函数返回NULL
 * */
robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags) {
    robj *val;

    if (expireIfNeeded(db,key) == 1) {
        // key达到过期时间，时效
        // 在一个master环境下，key确保被删除，所以返回null
        if (server.masterhost == NULL) {
            server.stat_keyspace_misses++;
            notifyKeyspaceEvent(NOTIFY_KEY_MISS, "keymiss", key, db->id);
            return NULL;
        }

        /* However if we are in the context of a slave, expireIfNeeded() will
         * not really try to expire the key, it only returns information
         * about the "logical" status of the key: key expiring is up to the
         * master in order to have a consistent view of master's data set.
         *
         * However, if the command caller is not the master, and as additional
         * safety measure, the command invoked is a read-only command, we can
         * safely return NULL here, and provide a more consistent behavior
         * to clients accessing expired values in a read-only fashion, that
         * will say the key as non existing.
         *
         * Notably this covers GETs when slaves are used to scale reads. */
        // 如果我们在从节点环境， expireIfNeeded()函数不会删除过期的键，它返回的仅仅是键是否被删除的逻辑值
        // 过期的键由主节点负责，为了保证主从节点数据的一致
        if (server.current_client &&
            server.current_client != server.master &&
            server.current_client->cmd &&
            server.current_client->cmd->flags & CMD_READONLY)
        {
            server.stat_keyspace_misses++;
            notifyKeyspaceEvent(NOTIFY_KEY_MISS, "keymiss", key, db->id);
            return NULL;
        }
    }
    //根据key获取val
    val = lookupKey(db,key,flags);
    // 更新读查询命中以及丢失次数
    if (val == NULL) {
        server.stat_keyspace_misses++;
        notifyKeyspaceEvent(NOTIFY_KEY_MISS, "keymiss", key, db->id);
    }
    else
        server.stat_keyspace_hits++;
    return val;
}

/* Like lookupKeyReadWithFlags(), but does not use any flag, which is the
 * common case. */
/* 以读操作取出key的值对象，会更新是否命中的信息 */
robj *lookupKeyRead(redisDb *db, robj *key) {
    return lookupKeyReadWithFlags(db,key,LOOKUP_NONE);
}

/* Lookup a key for write operations, and as a side effect, if needed, expires
 * the key if its TTL is reached.
 *
 * Returns the linked value object if the key exists or NULL if the key
 * does not exist in the specified DB. */
/*
 * 以写操作取出key的值对象，不更新是否命中的信息
 * */
robj *lookupKeyWriteWithFlags(redisDb *db, robj *key, int flags) {
    // 删除过期键
    expireIfNeeded(db,key);

    // 查找并返回 key 的值对象
    return lookupKey(db,key,flags);
}

/*
 * 在对应的数据库中寻找键值是否存在
 * */
robj *lookupKeyWrite(redisDb *db, robj *key) {
    return lookupKeyWriteWithFlags(db, key, LOOKUP_NONE);
}

/*
 * 为执行读取操作而从数据库中查找返回 key 的值。
 *
 * 如果 key 存在，那么返回 key 的值对象。
 *
 * 如果 key 不存在，那么向客户端发送 reply 参数中的信息，并返回 NULL 。
 */
robj *lookupKeyReadOrReply(client *c, robj *key, robj *reply) {

    // 查找
    robj *o = lookupKeyRead(c->db, key);

    // 决定是否发送信息
    if (!o) addReply(c,reply);

    return o;
}

/*
 * 为执行写入操作而从数据库中查找返回 key 的值。
 *
 * 如果 key 存在，那么返回 key 的值对象。
 *
 * 如果 key 不存在，那么向客户端发送 reply 参数中的信息，并返回 NULL 。
 */
robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply) {
    robj *o = lookupKeyWrite(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

/* Add the key to the DB. It's up to the caller to increment the reference
 * counter of the value if needed.
 *
 * 尝试将键值对 key 和 val 添加到数据库中。
 *
 * 调用者负责对 key 和 val 的引用计数进行增加。
 *
 * The program is aborted if the key already exists.
 *
 * 程序在键已经存在时会停止。
 */
void dbAdd(redisDb *db, robj *key, robj *val) {

    // 复制键名
    sds copy = sdsdup(key->ptr);

    // 尝试添加键值对
    int retval = dictAdd(db->dict, copy, val);

    // 如果键已经存在，那么停止
    serverAssertWithInfo(NULL,key,retval == DICT_OK);

    // 如果值对象是指定，有阻塞的命令，因此将key加入ready_keys字典中
    if (val->type == OBJ_LIST ||
        val->type == OBJ_ZSET ||
        val->type == OBJ_STREAM)
        signalKeyAsReady(db, key);
    ∂∂
    // 如果开启了集群模式，则讲key添加到槽中
    if (server.cluster_enabled) slotToKeyAdd(key->ptr);
}

/* This is a special version of dbAdd() that is used only when loading
 * keys from the RDB file: the key is passed as an SDS string that is
 * retained by the function (and not freed by the caller).
 *
 * Moreover this function will not abort if the key is already busy, to
 * give more control to the caller, nor will signal the key as ready
 * since it is not useful in this context.
 *
 * The function returns 1 if the key was added to the database, taking
 * ownership of the SDS string, otherwise 0 is returned, and is up to the
 * caller to free the SDS string. */
/* 该方法仅用来进行RDB扩散 */
int dbAddRDBLoad(redisDb *db, sds key, robj *val) {
    int retval = dictAdd(db->dict, key, val);
    if (retval != DICT_OK) return 0;
    if (server.cluster_enabled) slotToKeyAdd(key);
    return 1;
}

/* Overwrite an existing key with a new value. Incrementing the reference
 * count of the new value is up to the caller.
 *
 * 为已存在的键关联一个新值。
 *
 * 调用者负责对新值 val 的引用计数进行增加。
 *
 * This function does not modify the expire time of the existing key.
 *
 * 这个函数不会修改键的过期时间。
 *
 * The program is aborted if the key was not already present.
 *
 * 如果键不存在，那么函数停止。
 */
void dbOverwrite(redisDb *db, robj *key, robj *val) {
    // 找到保存key的节点地址
    dictEntry *de = dictFind(db->dict,key->ptr);

    // 节点必须存在，否则中止
    serverAssertWithInfo(NULL,key,de != NULL);
    dictEntry auxentry = *de;
    robj *old = dictGetVal(de);

    // 将原先value的lru淘汰策略转移到新value
    if (server.maxmemory_policy & MAXMEMORY_FLAG_LFU) {
        val->lru = old->lru;
    }

    //val设置
    dictSetVal(db->dict, de, val);

    if (server.lazyfree_lazy_server_del) {
        freeObjAsync(old);
        dictSetVal(db->dict, &auxentry, NULL);
    }

    dictFreeVal(db->dict, &auxentry);//内存释放
}

/* High level Set operation. This function can be used in order to set
 * a key, whatever it was existing or not, to a new object.
 *
 * 高层次的 SET 操作函数。
 *
 * 这个函数可以在不管键 key 是否存在的情况下，将它和 val 关联起来。
 *
 * 1) The ref count of the value object is incremented.
 *    值对象的引用计数会被增加
 *
 * 2) clients WATCHing for the destination key notified.
 *    监视键 key 的客户端会收到键已经被修改的通知
 *
 * 3) The expire time of the key is reset (the key is made persistent).
 *    键的过期时间会被移除（键变为持久的）
 */
void genericSetKey(client *c, redisDb *db, robj *key, robj *val, int keepttl, int signal) {
    //如果key不在数据库里，新建
    if (lookupKeyWrite(db,key) == NULL) {
        dbAdd(db,key,val);
    } else {//否则，用新值覆盖
        dbOverwrite(db,key,val);
    }
    incrRefCount(val);// 增加值的引用计数
    if (!keepttl) removeExpire(db,key); // 重置键在数据库里的过期时间
    if (signal) signalModifiedKey(c,db,key); // 发送修改键的通知
}

/* Common case for genericSetKey() where the TTL is not retained. */
/* 设置key，重置过期时间，并发送键通知 */
void setKey(client *c, redisDb *db, robj *key, robj *val) {
    genericSetKey(c,db,key,val,0,1);
}

/* Return true if the specified key exists in the specified database.
 * LRU/LFU info is not updated in any way. */
/* 检查键 key 是否存在于数据库中，存在返回 1 ，不存在返回 0 。 */
int dbExists(redisDb *db, robj *key) {
    return dictFind(db->dict,key->ptr) != NULL;
}

/* Return a random key, in form of a Redis object.
 * If there are no keys, NULL is returned.
 *
 * 随机从数据库中取出一个键，并以字符串对象的方式返回这个键。
 *
 * 如果数据库为空，那么返回 NULL 。
 *
 * The function makes sure to return keys not already expired.
 *
 * 这个函数保证被返回的键都是未过期的。
 */
robj *dbRandomKey(redisDb *db) {
    dictEntry *de;
    int maxtries = 100;
    int allvolatile = dictSize(db->dict) == dictSize(db->expires);

    while(1) {
        sds key;
        robj *keyobj;

        //从键值对字典中随机返回一个节点地址
        de = dictGetFairRandomKey(db->dict);
        if (de == NULL) return NULL;

        key = dictGetKey(de);
        //为key创建一个字符串对象
        keyobj = createStringObject(key,sdslen(key));
        //如果这个key在过期字典中，检查key是否过期，如果过期且被删除，则释放该key对象，并且重新随机返回一个key
        if (dictFind(db->expires,key)) {
            if (allvolatile && server.masterhost && --maxtries == 0) {
                /* If the DB is composed only of keys with an expire set,
                 * it could happen that all the keys are already logically
                 * expired in the slave, so the function cannot stop because
                 * expireIfNeeded() is false, nor it can stop because
                 * dictGetRandomKey() returns NULL (there are keys to return).
                 * To prevent the infinite loop we do some tries, but if there
                 * are the conditions for an infinite loop, eventually we
                 * return a key name that may be already expired. */
                return keyobj;
            }
            if (expireIfNeeded(db,keyobj)) {
                decrRefCount(keyobj);
                continue; /* search for another key. This expired. */
            }
        }
        return keyobj;  //返回对象
    }
}

/* Delete a key, value, and associated expiration entry if any, from the DB
 *
 * 从数据库中删除给定的键，键的值，以及键的过期时间。
 *
 * 删除成功返回 1 ，因为键不存在而导致删除失败时，返回 0 。
 */
int dbSyncDelete(redisDb *db, robj *key) {
    /* Deleting an entry from the expires dict will not free the sds of
     * the key, because it is shared with the main dictionary. */
    //如果在过期字典中发现该key并且该key的过期时间大于0。则删除过期字典中的key
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);
    //删除数据字典中的key
    if (dictDelete(db->dict,key->ptr) == DICT_OK) {
        //如果开启了集群模式，从槽位中删除该key
        if (server.cluster_enabled) slotToKeyDel(key->ptr);
        return 1;
    } else {
        return 0;
    }
}

/* This is a wrapper whose behavior depends on the Redis lazy free
 * configuration. Deletes the key synchronously or asynchronously. */
/* 删除一个key即过期时间 */
int dbDelete(redisDb *db, robj *key) {
    //根据配置判别是异步删除还是同步删除
    return server.lazyfree_lazy_server_del ? dbAsyncDelete(db,key) :
                                             dbSyncDelete(db,key);
}

/* Prepare the string object stored at 'key' to be modified destructively
 * to implement commands like SETBIT or APPEND.
 *
 * An object is usually ready to be modified unless one of the two conditions
 * are true:
 *
 * 1) The object 'o' is shared (refcount > 1), we don't want to affect
 *    other users.
 * 2) The object encoding is not "RAW".
 *
 * If the object is found in one of the above conditions (or both) by the
 * function, an unshared / not-encoded copy of the string object is stored
 * at 'key' in the specified 'db'. Otherwise the object 'o' itself is
 * returned.
 *
 * USAGE:
 *
 * The object 'o' is what the caller already obtained by looking up 'key'
 * in 'db', the usage pattern looks like this:
 *
 * o = lookupKeyWrite(db,key);
 * if (checkType(c,o,OBJ_STRING)) return;
 * o = dbUnshareStringValue(db,key,o);
 *
 * At this point the caller is ready to modify the object, for example
 * using an sdscat() call to append some data, or anything else.
 */
/* 解除key的值对象的共享，用于修改key、value的值 */
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o) {
    serverAssert(o->type == OBJ_STRING);
    //如果o对象是共享的(refcount > 1)，或者o对象的编码不是RAW的
    if (o->refcount != 1 || o->encoding != OBJ_ENCODING_RAW) {
        robj *decoded = getDecodedObject(o);    //获取o的字符串类型对象
        // 根据o的字符串类型对象新创建一个RAW对象
        o = createRawStringObject(decoded->ptr, sdslen(decoded->ptr));
        decrRefCount(decoded);  //原有的对象解除共享
        dbOverwrite(db,key,o);  //重写key的val对象此时val对象是唯一的
    }
    return o;
}

/* Remove all keys from all the databases in a Redis server.
 * If callback is given the function is called from time to time to
 * signal that work is in progress.
 *
 * The dbnum can be -1 if all the DBs should be flushed, or the specified
 * DB number if we want to flush only a single Redis database number.
 *
 * Flags are be EMPTYDB_NO_FLAGS if no special flags are specified or
 * 1. EMPTYDB_ASYNC if we want the memory to be freed in a different thread.
 * 2. EMPTYDB_BACKUP if we want to empty the backup dictionaries created by
 *    disklessLoadMakeBackups. In that case we only free memory and avoid
 *    firing module events.
 * and the function to return ASAP.
 *
 * On success the function returns the number of keys removed from the
 * database(s). Otherwise -1 is returned in the specific case the
 * DB number is out of range, and errno is set to EINVAL. */
/* 清空所有数据库，返回删除键的个数 */
long long emptyDbGeneric(redisDb *dbarray, int dbnum, int flags, void(callback)(void*)) {
    int async = (flags & EMPTYDB_ASYNC);
    //释放内存，别无其他
    int backup = (flags & EMPTYDB_BACKUP); /* Just free the memory, nothing else */
    RedisModuleFlushInfoV1 fi = {REDISMODULE_FLUSHINFO_VERSION,!async,dbnum};
    long long removed = 0;

    if (dbnum < -1 || dbnum >= server.dbnum) {
        errno = EINVAL;
        return -1;
    }

    /* Pre-flush actions */
    if (!backup) {
        /* Fire the flushdb modules event. */
        //触发flushdb模块事件。
        moduleFireServerEvent(REDISMODULE_EVENT_FLUSHDB,
                              REDISMODULE_SUBEVENT_FLUSHDB_START,
                              &fi);

        /* Make sure the WATCHed keys are affected by the FLUSH* commands.
         * Note that we need to call the function while the keys are still
         * there. */
        signalFlushedDb(dbnum);
    }

    int startdb, enddb;
    if (dbnum == -1) {
        startdb = 0;
        enddb = server.dbnum-1;
    } else {
        startdb = enddb = dbnum;
    }

    //遍历所有的数据库
    for (int j = startdb; j <= enddb; j++) {
        // 记录被删除键的数量
        removed += dictSize(dbarray[j].dict);
        if (async) {
            emptyDbAsync(&dbarray[j]);
        } else {
            // 删除当前数据库的键值对字典
            dictEmpty(dbarray[j].dict,callback);
            // 删除当前数据库的过期字典
            dictEmpty(dbarray[j].expires,callback);
        }
    }

    /* Post-flush actions */
    if (!backup) {
        //如果开启了集群模式，那么移除槽记录
        if (server.cluster_enabled) {
            if (async) {
                slotToKeyFlushAsync();
            } else {
                slotToKeyFlush();
            }
        }
        if (dbnum == -1) flushSlaveKeysWithExpireList();

        /* Also fire the end event. Note that this event will fire almost
         * immediately after the start event if the flush is asynchronous. */
        moduleFireServerEvent(REDISMODULE_EVENT_FLUSHDB,
                              REDISMODULE_SUBEVENT_FLUSHDB_END,
                              &fi);
    }

    return removed;
}

/* 清空数据库 */
long long emptyDb(int dbnum, int flags, void(callback)(void*)) {
    return emptyDbGeneric(server.db, dbnum, flags, callback);
}

/* 将客户端的目标数据库切换为 id 所指定的数据库 */
int selectDb(client *c, int id) {
    //数据库id检测
    if (id < 0 || id >= server.dbnum)
        return C_ERR;
    c->db = &server.db[id];//切换
    return C_OK;
}

/* 统计当前redis中一共有多少个key dbsize */
long long dbTotalServerKeyCount() {
    long long total = 0;
    int j;
    for (j = 0; j < server.dbnum; j++) {
        total += dictSize(server.db[j].dict);
    }
    return total;
}

/*-----------------------------------------------------------------------------
 * Hooks for key space changes.
 *
 * 键空间改动的钩子。
 *
 * Every time a key in the database is modified the function
 * signalModifiedKey() is called.
 *
 * 每当数据库中的键被改动时， signalModifiedKey() 函数都会被调用。
 *
 * Every time a DB is flushed the function signalFlushDb() is called.
 *
 * 每当一个数据库被清空时， signalFlushDb() 都会被调用。
 *----------------------------------------------------------------------------*/

/* Note that the 'c' argument may be NULL if the key was modified out of
 * a context of a client. */
/*
 * 每当key被修改时，就会调用此方法，
 * 客户端缓存相关的方法
 * */
void signalModifiedKey(client *c, redisDb *db, robj *key) {
    touchWatchedKey(db,key);//key监听通知
    trackingInvalidateKey(c,key);//失效key
}

/* 当数据库被清空，调用该函数 */
void signalFlushedDb(int dbid) {
    touchWatchedKeysOnFlush(dbid);
    trackingInvalidateKeysOnFlush(dbid);
}

/*-----------------------------------------------------------------------------
 * Type agnostic commands operating on the key space
 * 无类型命令的数据库操作
 *----------------------------------------------------------------------------*/

/* Return the set of flags to use for the emptyDb() call for FLUSHALL
 * and FLUSHDB commands.
 *
 * Currently the command just attempts to parse the "ASYNC" option. It
 * also checks if the command arity is wrong.
 *
 * On success C_OK is returned and the flags are stored in *flags, otherwise
 * C_ERR is returned and the function sends an error to the client. */
/* 返回用于FLUSHALL和FLUSHDB命令的emptyDb() 调用的标志集。 */
int getFlushCommandFlags(client *c, int *flags) {
    /* Parse the optional ASYNC option. */
    if (c->argc > 1) {
        if (c->argc > 2 || strcasecmp(c->argv[1]->ptr,"async")) {
            addReply(c,shared.syntaxerr);
            return C_ERR;
        }
        *flags = EMPTYDB_ASYNC;
    } else {
        *flags = EMPTYDB_NO_FLAGS;
    }
    return C_OK;
}

/* Flushes the whole server data set. */
/* 刷新整个服务器数据集。 */
void flushAllDataAndResetRDB(int flags) {
    //清空这个redis服务中的数据，所有的db
    server.dirty += emptyDb(-1,flags,NULL);
    //如果正在执行RDB，取消执行的进程
    if (server.rdb_child_pid != -1) killRDBChild();
    //更新RDB文件
    if (server.saveparamslen > 0) {
        /* Normally rdbSave() will reset dirty, but we don't want this here
         * as otherwise FLUSHALL will not be replicated nor put into the AOF. */
        //正常的rdbSave()将会重置脏键，为了将脏键值放入AOF，需要备份脏键值
        int saved_dirty = server.dirty;
        rdbSaveInfo rsi, *rsiptr;
        rsiptr = rdbPopulateSaveInfo(&rsi);
        //RDB持久化：程序将当前内存中的数据库快照保存到磁盘文件中
        rdbSave(server.rdb_filename,rsiptr);
        //还原脏键
        server.dirty = saved_dirty;
    }
    server.dirty++; //更新脏键
#if defined(USE_JEMALLOC)
    /* jemalloc 5 doesn't release pages back to the OS when there's no traffic.
     * for large databases, flushdb blocks for long anyway, so a bit more won't
     * harm and this way the flush and purge will be synchroneus. */
    if (!(flags & EMPTYDB_ASYNC))
        jemalloc_purge();
#endif
}

/* 清空当前选择的Redis数据库。（指定db） */
void flushdbCommand(client *c) {
    int flags;

    if (getFlushCommandFlags(c,&flags) == C_ERR) return;
    //执行真正的清空操作
    server.dirty += emptyDb(c->db->id,flags,NULL);
    addReply(c,shared.ok);
#if defined(USE_JEMALLOC)
    /* jemalloc 5 doesn't release pages back to the OS when there's no traffic.
     * for large databases, flushdb blocks for long anyway, so a bit more won't
     * harm and this way the flush and purge will be synchroneus. */
    if (!(flags & EMPTYDB_ASYNC))
        jemalloc_purge();
#endif
}

/* FLUSHALL [ASYNC]
 *
 * Flushes the whole server data set. */
/* 清空服务器内的所有数据库 */
void flushallCommand(client *c) {
    int flags;
    if (getFlushCommandFlags(c,&flags) == C_ERR) return;    //获取flags
    flushAllDataAndResetRDB(flags);     //执行清空命令
    addReply(c,shared.ok);
}

/* This command implements DEL and LAZYDEL. */
/*
 * DEL key [key ...]
 * DEL 命令实现
 * */
void delGenericCommand(client *c, int lazy) {
    int numdel = 0, j;

    //所有key遍历
    for (j = 1; j < c->argc; j++) {
        //检查是否过期，过期删除
        expireIfNeeded(c->db,c->argv[j]);
        //获取删除方式：异步or同步，并执行删除
        int deleted  = lazy ? dbAsyncDelete(c->db,c->argv[j]) :
                              dbSyncDelete(c->db,c->argv[j]);
        if (deleted) {  //删除成功
            //键被修改，发送信号
            signalModifiedKey(c,c->db,c->argv[j]);
            //发送"del"事件通知
            notifyKeyspaceEvent(NOTIFY_GENERIC,
                "del",c->argv[j],c->db->id);
            //更新脏键数量
            server.dirty++;
            //删除数量++
            numdel++;
        }
    }
    addReplyLongLong(c,numdel); //发送被删除键的数量给client，即客户端响应
}

/*
 * DEL key [key ...]
 * 同步删除命令
 * */
void delCommand(client *c) {
    delGenericCommand(c,server.lazyfree_lazy_user_del);
}

/*
 * DEL key [key ...]
 * 异步删除命令，即软删除
 * */
void unlinkCommand(client *c) {
    delGenericCommand(c,1);
}

/* EXISTS key1 key2 ... key_N.
 * Return value is the number of keys existing. */
/*
 * EXISTS key [key ...]
 * EXISTS 命令实现，判别key是否存在
 * 注意：在使用多个key的时候会仅仅返回存在几个
 * */
void existsCommand(client *c) {
    long long count = 0;
    int j;

    //遍历需要检查的key
    for (j = 1; j < c->argc; j++) {
        //以读的形式查询key是否存在
        if (lookupKeyRead(c->db,c->argv[j])) count++;
    }
    addReplyLongLong(c,count);  //响应客户端消息
}

/*
 * SELECT index
 * SELECT命令实现
 * 切换轩仔的数据库
 * */
void selectCommand(client *c) {
    long id;

    //类型转化检测
    if (getLongFromObjectOrReply(c, c->argv[1], &id,
        "invalid DB index") != C_OK)
        return;

    //如果当前开启了集群模式并且选择数据库id不为0，直接返回错误信息
    if (server.cluster_enabled && id != 0) {
        addReplyError(c,"SELECT is not allowed in cluster mode");
        return;
    }
    //切换数据库
    if (selectDb(c,id) == C_ERR) {
        addReplyError(c,"DB index is out of range");
    } else {
        //响应客户端 ok
        addReply(c,shared.ok);
    }
}

/*
 *  RANDOMKEY 命令实现 ，不删除返回的key
 *  从当前数据库中随机返回一个不过期key
 * */
void randomkeyCommand(client *c) {
    robj *key;

    //从当前字典中随机返回一个不过期的key
    if ((key = dbRandomKey(c->db)) == NULL) {
        addReplyNull(c);
        return;
    }

    addReplyBulk(c,key);    //将key回复给client
    decrRefCount(key);      //释放临时key对象

}

/*
 * KEYS pattern
 * KEYS 命令实现
 * */
void keysCommand(client *c) {
    dictIterator *di;
    dictEntry *de;
    sds pattern = c->argv[1]->ptr;  //保存pattern参数
    int plen = sdslen(pattern), allkeys;
    unsigned long numkeys = 0;
    void *replylen = addReplyDeferredLen(c);    //因为不知道有多少命令回复，那么创建一个空链表，之后将回复填入

    //初始化一个安全的字典迭代器
    di = dictGetSafeIterator(c->db->dict);
    //是否查询所有的key
    allkeys = (pattern[0] == '*' && plen == 1);
    //迭代字典中的节点
    while((de = dictNext(di)) != NULL) {
        sds key = dictGetKey(de);   //保存当前节点中的key
        robj *keyobj;

        // 如果有和pattern匹配的key
        if (allkeys || stringmatchlen(pattern,plen,key,sdslen(key),0)) {
            //创建字符串对象
            keyobj = createStringObject(key,sdslen(key));
            //检查是否可以对象过期，没有过期就将该键对象回复给client
            if (!keyIsExpired(c->db,keyobj)) {
                //响应客户端
                addReplyBulk(c,keyobj);
                numkeys++;
            }
            //释放临时的keyobj
            decrRefCount(keyobj);
        }
    }
    //释放字典迭代器
    dictReleaseIterator(di);
    //设置回复client的长度
    setDeferredArrayLen(c,replylen,numkeys);
}

/* This callback is used by scanGenericCommand in order to collect elements
 * returned by the dictionary iterator into a list. */
/* scanCallback函数被scanGenericCommand函数使用，为了保存被字典迭代器返回到列表中的元素 */
void scanCallback(void *privdata, const dictEntry *de) {
    void **pd = (void**) privdata;
    list *keys = pd[0];         //被迭代的元素列表
    robj *o = pd[1];            //当前值对象
    robj *key, *val = NULL;

    // 根据不同的编码类型，将字典节点de保存的键对象和值对象取出来，保存到key中，值对象保存到val中
    if (o == NULL) {
        sds sdskey = dictGetKey(de);
        key = createStringObject(sdskey, sdslen(sdskey));
    } else if (o->type == OBJ_SET) {
        sds keysds = dictGetKey(de);
        key = createStringObject(keysds,sdslen(keysds));
    } else if (o->type == OBJ_HASH) {
        sds sdskey = dictGetKey(de);
        sds sdsval = dictGetVal(de);
        key = createStringObject(sdskey,sdslen(sdskey));
        val = createStringObject(sdsval,sdslen(sdsval));
    } else if (o->type == OBJ_ZSET) {
        sds sdskey = dictGetKey(de);
        key = createStringObject(sdskey,sdslen(sdskey));
        val = createStringObjectFromLongDouble(*(double*)dictGetVal(de),0);
    } else {
        serverPanic("Type not handled in SCAN callback.");
    }

    // 将key保存到被迭代元素的列表中，如果有值val，同样加入到列表中
    listAddNodeTail(keys, key);
    if (val) listAddNodeTail(keys, val);
}

/* Try to parse a SCAN cursor stored at object 'o':
 * if the cursor is valid, store it as unsigned integer into *cursor and
 * returns C_OK. Otherwise return C_ERR and send an error to the
 * client. */
/* 获取scan命令的游标，尝试取解析一个保存在o中的游标，如果游标合法，保存到cursor中否则返回C_ERR */
int parseScanCursorOrReply(client *c, robj *o, unsigned long *cursor) {
    char *eptr;

    /* Use strtoul() because we need an *unsigned* long, so
     * getLongLongFromObject() does not cover the whole cursor space. */
    errno = 0;
    //将o对象的字符串类型值转换为unsigned  long int类型10进制数
    *cursor = strtoul(o->ptr, &eptr, 10);
    //转换错误检查
    if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' || errno == ERANGE)
    {
        addReplyError(c, "invalid cursor");
        return C_ERR;
    }
    return C_OK;
}

/* This command implements SCAN, HSCAN and SSCAN commands.
 *
 * 这是 SCAN 、 HSCAN 、 SSCAN 命令的实现函数。
 *
 * If object 'o' is passed, then it must be a Hash or Set object, otherwise
 * if 'o' is NULL the command will operate on the dictionary associated with
 * the current database.
 *
 * 如果给定了对象 o ，那么它必须是一个哈希对象或者集合对象，
 * 如果 o 为 NULL 的话，函数将使用当前数据库作为迭代对象。
 *
 * When 'o' is not NULL the function assumes that the first argument in
 * the client arguments vector is a key so it skips it before iterating
 * in order to parse options.
 *
 * 如果参数 o 不为 NULL ，那么说明它是一个键对象，函数将跳过这些键对象，
 * 对给定的命令选项进行分析（parse）。
 *
 * In the case of a Hash object the function returns both the field and value
 * of every element on the Hash.
 *
 * 如果被迭代的是哈希对象，那么函数返回的是键值对。
 */
/*
 * SCAN cursor [MATCH pattern] [COUNT count]
 * SCAN、HSCAN、SSCAN、ZSCAN一类命令底层实现
 * o对象必须是哈希对象或集合对象，否则命令将操作当前数据库
 * 如果o不是NULL，那么说明他是一个哈希或集合对象，函数将跳过这些键对象，对参数进行分析
 * 如果是哈希对象，返回返回的是键值对
 * */
void scanGenericCommand(client *c, robj *o, unsigned long cursor) {
    int i, j;
    list *keys = listCreate();  //创建一个列表
    listNode *node, *nextnode;
    long count = 10;
    sds pat = NULL;
    sds typename = NULL;
    int patlen = 0, use_pattern = 0;
    dict *ht;

    /* Object must be NULL (to iterate keys names), or the type of the object
     * must be Set, Sorted Set, or Hash. */
    // 输入类型的检查，要么迭代键名，要么当前集合对象，要么迭代哈希对象，要么迭代有序集合对象
    serverAssert(o == NULL || o->type == OBJ_SET || o->type == OBJ_HASH ||
                o->type == OBJ_ZSET);

    /* Set i to the first option argument. The previous one is the cursor. */
    // 计算第一个参数的下标，如果是键名，要条跳过该键
    i = (o == NULL) ? 2 : 3; /* Skip the key argument if needed. */

    /* Step 1: Parse options. */
    // 1. 解析选项
    while (i < c->argc) {
        j = c->argc - i;
        // 设定COUNT参数，COUNT 选项的作用就是让用户告知迭代命令， 在每次迭代中应该返回多少元素。
        if (!strcasecmp(c->argv[i]->ptr, "count") && j >= 2) {
            //保存个数到count
            if (getLongFromObjectOrReply(c, c->argv[i+1], &count, NULL)
                != C_OK)
            {
                goto cleanup;
            }

            // 如果个数小于1，语法错误
            if (count < 1) {
                addReply(c,shared.syntaxerr);
                goto cleanup;
            }

            i += 2; //参数跳过两个已经解析过的
            // 设定MATCH参数，让命令只返回和给定模式相匹配的元素。
        } else if (!strcasecmp(c->argv[i]->ptr, "match") && j >= 2) {
            pat = c->argv[i+1]->ptr;    //pattern字符串
            patlen = sdslen(pat);       //pattern字符串长度

            /* The pattern always matches if it is exactly "*", so it is
             * equivalent to disabling it. */
            // 如果pattern是"*"，就不用匹配，全部返回，设置为0
            use_pattern = !(pat[0] == '*' && patlen == 1);

            i += 2;
        } else if (!strcasecmp(c->argv[i]->ptr, "type") && o == NULL && j >= 2) {
            /* SCAN for a particular type only applies to the db dict */
            typename = c->argv[i+1]->ptr;
            i+= 2;
        } else {
            addReply(c,shared.syntaxerr);
            goto cleanup;
        }
    }

    /* Step 2: Iterate the collection.
     *
     * Note that if the object is encoded with a ziplist, intset, or any other
     * representation that is not a hash table, we are sure that it is also
     * composed of a small number of elements. So to avoid taking state we
     * just return everything inside the object in a single call, setting the
     * cursor to zero to signal the end of the iteration. */

    /* Handle the case of a hash table. */
    // 2.如果对象是ziplist、intset或其他而不是哈希表，那么这些类型只是包含少量的元素
    // 我们一次将其所有的元素全部返回给调用者，并设置游标cursor为0，标示迭代完成
    ht = NULL;
    // 迭代目标是数据库
    if (o == NULL) {
        ht = c->db->dict;
        // 迭代目标是HT编码的集合对象
    } else if (o->type == OBJ_SET && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr;
        // 迭代目标是HT编码的哈希对象
    } else if (o->type == OBJ_HASH && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr;
        count *= 2; /* We return key / value for this type. */
        // 迭代目标是skiplist编码的有序集合对象
    } else if (o->type == OBJ_ZSET && o->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = o->ptr;
        ht = zs->dict;
        count *= 2; /* We return key / value for this type. */
    }


    if (ht) {
        void *privdata[2];
        /* We set the max number of iterations to ten times the specified
         * COUNT, so if the hash table is in a pathological state (very
         * sparsely populated) we avoid to block too much time at the cost
         * of returning no or very few elements. */
        // 设置最大的迭代长度为10*count次
        long maxiterations = count*10;

        /* We pass two pointers to the callback: the list to which it will
         * add new elements, and the object containing the dictionary so that
         * it is possible to fetch more data in a type-dependent way. */
        // 回调函数scanCallback的参数privdata是一个数组，保存的是被迭代对象的键和值
        // 回调函数scanCallback的另一个参数，是一个字典对象
        // 回调函数scanCallback的作用，从字典对象中将键值对提取出来，不用管字典对象是什么数据类型
        privdata[0] = keys;
        privdata[1] = o;
        // 循环扫描ht，从游标cursor开始，调用指定的scanCallback函数，提出ht中的数据到刚开始创建的列表keys中
        do {
            cursor = dictScan(ht, cursor, scanCallback, NULL, privdata);
        } while (cursor &&
              maxiterations-- &&
              listLength(keys) < (unsigned long)count); //没迭代完，或没迭代够count，就继续循环
        // 如果是集合对象但编码不是HT是整数集合
    } else if (o->type == OBJ_SET) {
        int pos = 0;
        int64_t ll;

        // 将整数值取出来，构建成字符串对象加入到keys列表中，游标设置为0，表示迭代完成
        while(intsetGet(o->ptr,pos++,&ll))
            listAddNodeTail(keys,createStringObjectFromLongLong(ll));
        cursor = 0;
        // 如果是哈希对象，或有序集合对象，但是编码都不是HT，是ziplist
    } else if (o->type == OBJ_HASH || o->type == OBJ_ZSET) {
        unsigned char *p = ziplistIndex(o->ptr,0);
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;

        while(p) {
            // 将值取出来，根据不同类型的值，构建成相同的字符串对象，加入到keys列表中
            ziplistGet(p,&vstr,&vlen,&vll);
            listAddNodeTail(keys,
                (vstr != NULL) ? createStringObject((char*)vstr,vlen) :
                                 createStringObjectFromLongLong(vll));
            p = ziplistNext(o->ptr,p);
        }
        cursor = 0;
    } else {
        serverPanic("Not handled encoding in SCAN.");
    }

    /* Step 3: Filter elements. */
    // 3. 如果设置MATCH参数，要进行过滤
    node = listFirst(keys);  //链表首节点地址
    while (node) {
        robj *kobj = listNodeValue(node);   //key对象
        nextnode = listNextNode(node);      //下一个节点地址
        int filter = 0; //默认为不过滤

        /* Filter element if it does not match the pattern. */
        //pattern不是"*"因此要过滤
        if (!filter && use_pattern) {
            // 如果kobj是字符串对象
            if (sdsEncodedObject(kobj)) {
                // kobj的值不匹配pattern，设置过滤标志
                if (!stringmatchlen(pat, patlen, kobj->ptr, sdslen(kobj->ptr), 0))
                    filter = 1;
                // 如果kobj是整数对象
            } else {
                char buf[LONG_STR_SIZE];
                int len;

                serverAssert(kobj->encoding == OBJ_ENCODING_INT);
                // 将整数转换为字符串类型，保存到buf中
                len = ll2string(buf,sizeof(buf),(long)kobj->ptr);
                //buf的值不匹配pattern，设置过滤标志
                if (!stringmatchlen(pat, patlen, buf, len, 0)) filter = 1;
            }
        }

        /* Filter an element if it isn't the type we want. */
        if (!filter && o == NULL && typename){
            robj* typecheck = lookupKeyReadWithFlags(c->db, kobj, LOOKUP_NOTOUCH);
            char* type = getObjectTypeName(typecheck);
            if (strcasecmp((char*) typename, type)) filter = 1;
        }

        /* Filter element if it is an expired key. */
        // 迭代目标是数据库，如果kobj是过期键，则过滤
        if (!filter && o == NULL && expireIfNeeded(c->db, kobj)) filter = 1;

        /* Remove the element and its associated value if needed. */
        //如果该键满足了上述的过滤条件，那么将其从keys列表删除并释放
        if (filter) {
            decrRefCount(kobj);
            listDelNode(keys, node);
        }

        /* If this is a hash or a sorted set, we have a flat list of
         * key-value elements, so if this element was filtered, remove the
         * value, or skip it if it was not filtered: we only match keys. */
        //如果当前迭代目标是有序集合或哈希对象，因此keys列表中保存的是键值对，如果key键对象被过滤，值对象也应当被过滤
        if (o && (o->type == OBJ_ZSET || o->type == OBJ_HASH)) {
            node = nextnode;
            nextnode = listNextNode(node);   //值对象的节点地址
            // 如果该键满足了上述的过滤条件，那么将其从keys列表删除并释放
            if (filter) {
                kobj = listNodeValue(node); //取出值对象
                decrRefCount(kobj);
                listDelNode(keys, node);    //删除
            }
        }
        node = nextnode;
    }

    /* Step 4: Reply to the client. */
    // 4. 回复信息给client
    addReplyArrayLen(c, 2);     //2部分，一个是游标，一个是列表
    addReplyBulkLongLong(c,cursor);    //回复游标

    //回复列表长度
    addReplyArrayLen(c, listLength(keys));
    //循环回复列表中的元素，并释放
    while ((node = listFirst(keys)) != NULL) {
        robj *kobj = listNodeValue(node);
        addReplyBulk(c, kobj);
        decrRefCount(kobj);
        listDelNode(keys, node);
    }

//清理代码
cleanup:
    listSetFreeMethod(keys,decrRefCountVoid);   //设置特定的释放列表的方式decrRefCountVoid
    listRelease(keys);                          //释放
}

/*
 * SCAN cursor [MATCH pattern] [COUNT count]
 * SCAN 命令实现
 * */
void scanCommand(client *c) {
    unsigned long cursor;
    // 获取scan命令的游标，尝试取解析一个保存cursor参数中的游标，如果游标合法，保存到cursor中否则返回C_ERR
    if (parseScanCursorOrReply(c,c->argv[1],&cursor) == C_ERR) return;
    scanGenericCommand(c,NULL,cursor);
}

/*
 * DBSIZE 命令实现，返回当前数据库的 key 的数量。
 * */
void dbsizeCommand(client *c) {
    addReplyLongLong(c,dictSize(c->db->dict));  //回复数据库中键值对字典的大小值
}

/*
 * LASTSAVE 返回最近一次 Redis 成功将数据保存到磁盘上的时间，以 UNIX 时间戳格式表示。
 * */
void lastsaveCommand(client *c) {
    addReplyLongLong(c,server.lastsave);
}

/*
 * TYPE key 返回 key 所储存的值的类型。
 * TYPE 命令底层实现
 * */
char* getObjectTypeName(robj *o) {
    char* type;
    if (o == NULL) {
        type = "none";
    } else {
        switch(o->type) {
        case OBJ_STRING: type = "string"; break;
        case OBJ_LIST: type = "list"; break;
        case OBJ_SET: type = "set"; break;
        case OBJ_ZSET: type = "zset"; break;
        case OBJ_HASH: type = "hash"; break;
        case OBJ_STREAM: type = "stream"; break;
        case OBJ_MODULE: {
            moduleValue *mv = o->ptr;
            type = mv->type->name;
        }; break;
        default: type = "unknown"; break;
        }
    }
    return type;
}

/*
 * TYPE key 返回 key 所储存的值的类型。
 * TYPE 命令实现
 * */
void typeCommand(client *c) {
    robj *o;
    //以读操作取出key参数的值对象，并且不修改键的使用时间
    o = lookupKeyReadWithFlags(c->db,c->argv[1],LOOKUP_NOTOUCH);
    addReplyStatus(c, getObjectTypeName(o));
}

/*
 * SHUTDOWN [SAVE|NOSAVE]
 * 执行 SHUTDOWN SAVE 会强制让数据库执行保存操作，即使没有设定(configure)保存点
 * 执行 SHUTDOWN NOSAVE 会阻止数据库执行保存操作，即使已经设定有一个或多个保存点(你可以将这一用法看作是强制停止服务器的一个假想的 ABORT 命令)
 *
 * SHUTDOWN 命令实现
 * */
void shutdownCommand(client *c) {
    int flags = 0;

    if (c->argc > 2) {
        addReply(c,shared.syntaxerr);   //语法错误
        return;
    } else if (c->argc == 2) {
        //指定NOSAVE，停机不保存
        if (!strcasecmp(c->argv[1]->ptr,"nosave")) {
            flags |= SHUTDOWN_NOSAVE;
            // 指定SAVE，停机保存
        } else if (!strcasecmp(c->argv[1]->ptr,"save")) {
            flags |= SHUTDOWN_SAVE;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }
    //关闭服务器
    if (prepareForShutdown(flags) == C_OK) exit(0);
    addReplyError(c,"Errors trying to SHUTDOWN. Check logs.");
}

/*
 * RENAME key newkey
 * RENAMENX key newkey
 * RENAME、RENAMENX命令底层实现
 * */
void renameGenericCommand(client *c, int nx) {
    robj *o;
    long long expire;
    int samekey = 0;

    /* When source and dest key is the same, no operation is performed,
     * if the key exists, however we still return an error on unexisting key. */
    //key和newkey相同的话，设置samekey标志
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) samekey = 1;

    //以写操作读取key的值对象
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;

    //如果key和newkey相同，nx为1发送0，否则为ok
    if (samekey) {
        addReply(c,nx ? shared.czero : shared.ok);
        return;
    }

    //增加值对象的引用计数，保护起来，用于关联newkey，以防删除了key顺带将值对象也删除
    incrRefCount(o);
    //备份key的过期时间，将来作为newkey的过期时间
    expire = getExpire(c->db,c->argv[1]);
    //判断newkey的值对象是否存在
    if (lookupKeyWrite(c->db,c->argv[2]) != NULL) {
        //设置nx标志，则不符合已存在的条件，发送0
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        /* Overwrite: delete the old key before creating the new one
         * with the same name. */
        dbDelete(c->db,c->argv[2]); //将旧的newkey对象删除
    }
    //将newkey和key的值对象关联
    dbAdd(c->db,c->argv[2],o);
    //如果newkey设置过过期时间，则为newkey设置过期时间
    if (expire != -1) setExpire(c,c->db,c->argv[2],expire);
    //删除key
    dbDelete(c->db,c->argv[1]);
    //发送这两个键被修改的信号
    signalModifiedKey(c,c->db,c->argv[1]);
    signalModifiedKey(c,c->db,c->argv[2]);
    //发送不同命令的事件通知
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_from",
        c->argv[1],c->db->id);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_to",
        c->argv[2],c->db->id);
    server.dirty++;  //更新脏键
    addReply(c,nx ? shared.cone : shared.ok);
}

/*
 * 将 key 改名为 newkey 。
 * 当 key 和 newkey 相同，或者 key 不存在时，返回一个错误。
 * 当 newkey 已经存在时， RENAME 命令将覆盖旧值。
 * */
void renameCommand(client *c) {
    renameGenericCommand(c,0);
}

/*
 * 当且仅当 newkey 不存在时，将 key 改名为 newkey 。
 * 当 key 不存在时，返回一个错误。
 * */
void renamenxCommand(client *c) {
    renameGenericCommand(c,1);
}

/*
 * MOVE key db 将当前数据库的 key 移动到给定的数据库 db 当中。
 * MOVE 命令实现
 * */
void moveCommand(client *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid;
    long long dbid, expire;

    //服务器处于集群模式，不支持多数据库
    if (server.cluster_enabled) {
        addReplyError(c,"MOVE is not allowed in cluster mode");
        return;
    }

    /* Obtain source and target DB pointers */
    //获得源数据库和源数据库的id
    src = c->db;
    srcid = c->db->id;

    //将参数db的值保存到dbid，并且切换到该数据库中
    if (getLongLongFromObject(c->argv[2],&dbid) == C_ERR ||
        dbid < INT_MIN || dbid > INT_MAX ||
        selectDb(c,dbid) == C_ERR)
    {
        addReply(c,shared.outofrangeerr);
        return;
    }
    // 目标数据库
    dst = c->db;
    //切回来原数据库
    selectDb(c,srcid); /* Back to the source DB */

    /* If the user is moving using as target the same
     * DB as the source DB it is probably an error. */
    //如果前后切换的数据库相同，则返回有关错误
    if (src == dst) {
        addReply(c,shared.sameobjecterr);
        return;
    }

    /* Check if the element exists and get a reference */
    //以写操作取出源数据库的对象
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (!o) {
        addReply(c,shared.czero);   //不存在发送0
        return;
    }
    //获取原key过期时间
    expire = getExpire(c->db,c->argv[1]);

    /* Return zero if the key already exists in the target DB */
    //判断当前key是否存在于目标数据库，存在直接返回，发送0
    if (lookupKeyWrite(dst,c->argv[1]) != NULL) {
        addReply(c,shared.czero);
        return;
    }
    //将key-value对象添加到目标数据库中
    dbAdd(dst,c->argv[1],o);
    //设置移动后key的过期时间
    if (expire != -1) setExpire(c,dst,c->argv[1],expire);
    incrRefCount(o);    //增加引用计数

    /* OK! key moved, free the entry in the source DB */
    //从源数据库中将key和关联的值对象删除
    dbDelete(src,c->argv[1]);
    signalModifiedKey(c,src,c->argv[1]);
    signalModifiedKey(c,dst,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_GENERIC,
                "move_from",c->argv[1],src->id);
    notifyKeyspaceEvent(NOTIFY_GENERIC,
                "move_to",c->argv[1],dst->id);

    //更新脏键
    server.dirty++;
    addReply(c,shared.cone);    //客户端回复
}

/* Helper function for dbSwapDatabases(): scans the list of keys that have
 * one or more blocked clients for B[LR]POP or other blocking commands
 * and signal the keys as ready if they are of the right type. See the comment
 * where the function is used for more info. */
/* 检查blocking_keys，将符合的key加入到readylist，该函数是dbSwapDatabases的辅助函数 */
void scanDatabaseForReadyLists(redisDb *db) {
    dictEntry *de;
    //获取一个安全的字典迭代器
    dictIterator *di = dictGetSafeIterator(db->blocking_keys);
    while((de = dictNext(di)) != NULL) {    //迭代
        robj *key = dictGetKey(de);
        robj *value = lookupKey(db,key,LOOKUP_NOTOUCH);
        //如果该key在新的数据库中存在且类型为list加入到readylist
        if (value && (value->type == OBJ_LIST ||
                      value->type == OBJ_STREAM ||
                      value->type == OBJ_ZSET))
            signalKeyAsReady(db, key);
    }
    dictReleaseIterator(di);
}

/* Swap two databases at runtime so that all clients will magically see
 * the new database even if already connected. Note that the client
 * structure c->db points to a given DB, so we need to be smarter and
 * swap the underlying referenced structures, otherwise we would need
 * to fix all the references to the Redis DB structure.
 *
 * Returns C_ERR if at least one of the DB ids are out of range, otherwise
 * C_OK is returned. */
/* 交换两个数据库实现 */
int dbSwapDatabases(long id1, long id2) {
    if (id1 < 0 || id1 >= server.dbnum ||
        id2 < 0 || id2 >= server.dbnum) return C_ERR;
    if (id1 == id2) return C_OK;
    redisDb aux = server.db[id1];
    redisDb *db1 = &server.db[id1], *db2 = &server.db[id2];

    /* Swap hash tables. Note that we don't swap blocking_keys,
     * ready_keys and watched_keys, since we want clients to
     * remain in the same DB they were. */
    db1->dict = db2->dict;
    db1->expires = db2->expires;
    db1->avg_ttl = db2->avg_ttl;
    db1->expires_cursor = db2->expires_cursor;

    db2->dict = aux.dict;
    db2->expires = aux.expires;
    db2->avg_ttl = aux.avg_ttl;
    db2->expires_cursor = aux.expires_cursor;

    /* Now we need to handle clients blocked on lists: as an effect
     * of swapping the two DBs, a client that was waiting for list
     * X in a given DB, may now actually be unblocked if X happens
     * to exist in the new version of the DB, after the swap.
     *
     * However normally we only do this check for efficiency reasons
     * in dbAdd() when a list is created. So here we need to rescan
     * the list of clients blocked on lists and signal lists as ready
     * if needed. */
    //交换后的db，可能阻塞的key不在阻塞了，所以进行检查，将可能的key加入到readylist
    scanDatabaseForReadyLists(db1);
    scanDatabaseForReadyLists(db2);
    return C_OK;
}

/* SWAPDB db1 db2 */
/* 交换两个db指令实现 */
void swapdbCommand(client *c) {
    long id1, id2;

    /* Not allowed in cluster mode: we have just DB 0 there. */
    //在集群模式下不允许执行该指令
    if (server.cluster_enabled) {
        addReplyError(c,"SWAPDB is not allowed in cluster mode");
        return;
    }

    /* Get the two DBs indexes. */
    //获取两个db的id
    if (getLongFromObjectOrReply(c, c->argv[1], &id1,
        "invalid first DB index") != C_OK)
        return;

    if (getLongFromObjectOrReply(c, c->argv[2], &id2,
        "invalid second DB index") != C_OK)
        return;

    /* Swap... */
    //交换数据库
    if (dbSwapDatabases(id1,id2) == C_ERR) {
        addReplyError(c,"DB index is out of range");
        return;
    } else {
        RedisModuleSwapDbInfo si = {REDISMODULE_SWAPDBINFO_VERSION,id1,id2};
        moduleFireServerEvent(REDISMODULE_EVENT_SWAPDB,0,&si);
        server.dirty++;
        addReply(c,shared.ok);
    }
}

/*-----------------------------------------------------------------------------
 * Expires API
 *----------------------------------------------------------------------------*/

/* 移除key的过期时间，成功返回1 */
int removeExpire(redisDb *db, robj *key) {
    /* An expire may only be removed if there is a corresponding entry in the
     * main dict. Otherwise, the key will never be freed. */
    // key存在于键值对字典中
    serverAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    //从过期字典中删除key
    return dictDelete(db->expires,key->ptr) == DICT_OK;
}

/* Set an expire to the specified key. If the expire is set in the context
 * of an user calling a command 'c' is the client, otherwise 'c' is set
 * to NULL. The 'when' parameter is the absolute unix time in milliseconds
 * after which the key will no longer be considered valid. */
/* 设置过期时间 */
void setExpire(client *c, redisDb *db, robj *key, long long when) {
    dictEntry *kde, *de;

    /* Reuse the sds from the main dict in the expire dict */
    //查看该key是存在字典中存在
    kde = dictFind(db->dict,key->ptr);
    serverAssertWithInfo(NULL,key,kde != NULL);
    // 根据key在过期字典中查找或者添加，并返回dictEntry
    de = dictAddOrFind(db->expires,dictGetKey(kde));
    //设置当前节点的value值
    dictSetSignedIntegerVal(de,when);

    // 判别当前节点为 slave 并开启了写功能
    int writable_slave = server.masterhost && server.repl_slave_ro == 0;
    // 如果当前服务端是可写的从节点，则需要将过期数据专门记录下来
    if (c && writable_slave && !(c->flags & CLIENT_MASTER))
        rememberSlaveKeyWithExpire(db,key);
}

/* 返回指定密钥的到期时间，如果没有与此密钥相关联的到期时间，则返回-1（即密钥是非易失性的） */
long long getExpire(redisDb *db, robj *key) {
    dictEntry *de;

    /* 当前数据库的过期key字典大小，为0，则表示当前数据库不存在过期key，直接返回 */
    if (dictSize(db->expires) == 0 ||
       //在当前过期key字典集合中未找到指定key，直接返回-1
       (de = dictFind(db->expires,key->ptr)) == NULL) return -1;

    /* 该条目是在到期字典中找到的，这意味着它也应该出现在主字典中（安全检查）。*/
    serverAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    //返回当前节点的value
    return dictGetSignedIntegerVal(de);
}

/* Propagate expires into slaves and the AOF file.
 * When a key expires in the master, a DEL operation for this key is sent
 * to all the slaves and the AOF file if enabled.
 *
 * This way the key expiry is centralized in one place, and since both
 * AOF and the master->slave link guarantee operation ordering, everything
 * will be consistent even if we allow write operations against expiring
 * keys. */
/*
 * 过期扩散
 * 将过期时间传播到从节点和AOF文件
 * 当一个键在主节点中过期时，主节点会发送del命令给从节点和AOF文件
 * */
void propagateExpire(redisDb *db, robj *key, int lazy) {
    robj *argv[2];

    //直接删除还是软删除
    argv[0] = lazy ? shared.unlink : shared.del;
    argv[1] = key;
    incrRefCount(argv[0]);  //引用计数增加
    incrRefCount(argv[1]);

    if (server.aof_state != AOF_OFF)
        feedAppendOnlyFile(server.delCommand,db->id,argv,2);//AOF命令追加
    replicationFeedSlaves(server.slaves,db->id,argv,2);//主从数据同步

    //引用计数释放
    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

/*
 * 检查键是否过期，如果过期，从数据库中删除
 * 返回0表示没有过期或没有过期时间，返回1 表示键被删除
 * */
int keyIsExpired(redisDb *db, robj *key) {
    //获取当前key的过期时间，-1为不过期或者不存在
    mstime_t when = getExpire(db,key);
    mstime_t now;

    //直接返回key不过期
    if (when < 0) return 0; /* No expire for this key */

    /* 加载时不要过期。它将在以后完成。*/
    if (server.loading) return 0;

    /* If we are in the context of a Lua script, we pretend that time is
     * blocked to when the Lua script started. This way a key can expire
     * only the first time it is accessed and not in the middle of the
     * script execution, making propagation to slaves / AOF consistent.
     * See issue #1525 on Github for more information. */
    /* 保持在整个lua脚本执行过程中的原子性，以达到数据同步不出问题 */
    if (server.lua_caller) {
        now = server.lua_time_start;
    }
    /* If we are in the middle of a command execution, we still want to use
     * a reference time that does not change: in that case we just use the
     * cached time, that we update before each call in the call() function.
     * This way we avoid that commands such as RPOPLPUSH or similar, that
     * may re-open the same key multiple times, can invalidate an already
     * open object in a next call, if the next call will see the key expired,
     * while the first did not. */
    /* 刷新因为执行时差引起的异常情况 */
    else if (server.fixed_time_expire > 0) {
        now = server.mstime;
    }
    /* For the other cases, we want to use the most fresh time we have. */
    /* 除了上述情况之后，时间都默认为当前最新时间 */
    else {
        now = mstime();
    }

    /* The key expired if the current (virtual or real) time is greater
     * than the expire time of the key. */
    return now > when;
}

/* This function is called when we are going to perform some operation
 * in a given key, but such key may be already logically expired even if
 * it still exists in the database. The main way this function is called
 * is via lookupKey*() family of functions.
 *
 * The behavior of the function depends on the replication role of the
 * instance, because slave instances do not expire keys, they wait
 * for DELs from the master for consistency matters. However even
 * slaves will try to have a coherent return value for the function,
 * so that read commands executed in the slave side will be able to
 * behave like if the key is expired even if still present (because the
 * master has yet to propagate the DEL).
 *
 * In masters as a side effect of finding a key which is expired, such
 * key will be evicted from the database. Also this may trigger the
 * propagation of a DEL/UNLINK command in AOF / replication stream.
 *
 * The return value of the function is 0 if the key is still valid,
 * otherwise the function returns 1 if the key is expired. */
/*
 * 检查 key 是否已经过期，如果是的话，将它从数据库中删除。
 *
 * 返回 0 表示键没有过期时间，或者键未过期。
 *
 * 返回 1 表示键已经因为过期而被删除了。
 */
int expireIfNeeded(redisDb *db, robj *key) {
    //当前key是否存在过期情况
    if (!keyIsExpired(db,key)) return 0;

    /* If we are running in the context of a slave, instead of
     * evicting the expired key from the database, we return ASAP:
     * the slave key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys.
     *
     * Still we try to return the right information to the caller,
     * that is, 0 if we think the key should be still valid, 1 if
     * we think the key is expired at this time. */
    // 当服务器运行在 replication 模式时
    // 附属节点并不主动删除 key
    // 它只返回一个逻辑上正确的返回值
    // 真正的删除操作要等待主节点发来删除命令时才执行
    // 从而保证数据的同步
    if (server.masterhost != NULL) return 1;

    /* Delete the key */
    // 过期key删除++
    server.stat_expiredkeys++;

    // 将过期键key传播给AOF文件和从节点
    propagateExpire(db,key,server.lazyfree_lazy_expire);

    // 发送事件通知,所有订阅了该key的客户端都会收到该消息
    notifyKeyspaceEvent(NOTIFY_EXPIRED,
        "expired",key,db->id);

    //执行key删除
    int retval = server.lazyfree_lazy_expire ? dbAsyncDelete(db,key) :
                                               dbSyncDelete(db,key);

    if (retval) signalModifiedKey(NULL,db,key);

    return retval;
}

/* -----------------------------------------------------------------------------
 * API to get key arguments from commands
 * ---------------------------------------------------------------------------*/

/* Prepare the getKeysResult struct to hold numkeys, either by using the
 * pre-allocated keysbuf or by allocating a new array on the heap.
 *
 * This function must be called at least once before starting to populate
 * the result, and can be called repeatedly to enlarge the result array.
 */
/*  */
int *getKeysPrepareResult(getKeysResult *result, int numkeys) {
    /* GETKEYS_RESULT_INIT initializes keys to NULL, point it to the pre-allocated stack
     * buffer here. */
    if (!result->keys) {
        serverAssert(!result->numkeys);
        result->keys = result->keysbuf;
    }

    /* Resize if necessary */
    if (numkeys > result->size) {
        if (result->keys != result->keysbuf) {
            /* We're not using a static buffer, just (re)alloc */
            result->keys = zrealloc(result->keys, numkeys * sizeof(int));
        } else {
            /* We are using a static buffer, copy its contents */
            result->keys = zmalloc(numkeys * sizeof(int));
            if (result->numkeys)
                memcpy(result->keys, result->keysbuf, result->numkeys * sizeof(int));
        }
        result->size = numkeys;
    }

    return result->keys;
}

/* The base case is to use the keys position as given in the command table
 * (firstkey, lastkey, step). */
/* 获取命令中的所有 key */
int getKeysUsingCommandTable(struct redisCommand *cmd,robj **argv, int argc, getKeysResult *result) {
    int j, i = 0, last, *keys;
    UNUSED(argv);

    if (cmd->firstkey == 0) {
        result->numkeys = 0;
        return 0;
    }

    last = cmd->lastkey;
    if (last < 0) last = argc+last;

    int count = ((last - cmd->firstkey)+1);
    keys = getKeysPrepareResult(result, count);

    for (j = cmd->firstkey; j <= last; j += cmd->keystep) {
        if (j >= argc) {
            /* Modules commands, and standard commands with a not fixed number
             * of arguments (negative arity parameter) do not have dispatch
             * time arity checks, so we need to handle the case where the user
             * passed an invalid number of arguments here. In this case we
             * return no keys and expect the command implementation to report
             * an arity or syntax error. */
            if (cmd->flags & CMD_MODULE || cmd->arity < 0) {
                getKeysFreeResult(result);
                result->numkeys = 0;
                return 0;
            } else {
                serverPanic("Redis built-in command declared keys positions not matching the arity requirements.");
            }
        }
        keys[i++] = j;
    }
    result->numkeys = i;
    return i;
}

/* Return all the arguments that are keys in the command passed via argc / argv.
 *
 * The command returns the positions of all the key arguments inside the array,
 * so the actual return value is a heap allocated array of integers. The
 * length of the array is returned by reference into *numkeys.
 *
 * 'cmd' must be point to the corresponding entry into the redisCommand
 * table, according to the command name in argv[0].
 *
 * This function uses the command table if a command-specific helper function
 * is not required, otherwise it calls the command-specific function. */
/* 从argv和argc指定的参数列表中返回所有的键 */
int getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    if (cmd->flags & CMD_MODULE_GETKEYS) {
        return moduleGetCommandKeysViaAPI(cmd,argv,argc,result);
    } else if (!(cmd->flags & CMD_MODULE) && cmd->getkeys_proc) {
        return cmd->getkeys_proc(cmd,argv,argc,result);
    } else {
        return getKeysUsingCommandTable(cmd,argv,argc,result);
    }
}

/* Free the result of getKeysFromCommand. */
/* 释放整型数组空间 */
void getKeysFreeResult(getKeysResult *result) {
    if (result && result->keys != result->keysbuf)
        zfree(result->keys);
}

/* Helper function to extract keys from following commands:
 * ZUNIONSTORE <destkey> <num-keys> <key> <key> ... <key> <options>
 * ZINTERSTORE <destkey> <num-keys> <key> <key> ... <key> <options> */
/* 从ZUNIONSTORE、ZINTERSTORE命令中提取key的下标 */
int zunionInterGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num, *keys;
    UNUSED(cmd);

    //计算key的个数
    num = atoi(argv[2]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. */
    //语法检查
    if (num < 1 || num > (argc-3)) {
        result->numkeys = 0;
        return 0;
    }

    /* Keys in z{union,inter}store come from two places:
     * argv[1] = storage key,
     * argv[3...n] = keys to intersect */
    /* Total keys = {union,inter} keys + storage key */

    keys = getKeysPrepareResult(result, num+1);
    result->numkeys = num+1;

    /* Add all key positions for argv[3...n] to keys[] */
    //key的参数的下标，保存在*keys中
    for (i = 0; i < num; i++) keys[i] = 3+i;

    /* Finally add the argv[1] key position (the storage key target). */
    keys[num] = 1;//设置destkey的下标

    return result->numkeys;
}

/* Helper function to extract keys from the following commands:
 * EVAL <script> <num-keys> <key> <key> ... <key> [more stuff]
 * EVALSHA <script> <num-keys> <key> <key> ... <key> [more stuff] */
/*  从EVAL和EVALSHA命令中获取key的下标 */
int evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num, *keys;
    UNUSED(cmd);

    // 计算key的个数
    num = atoi(argv[2]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. */
    //语法检测
    if (num <= 0 || num > (argc-3)) {
        result->numkeys = 0;
        return 0;
    }

    // 分配空间
    keys = getKeysPrepareResult(result, num);
    result->numkeys = num;  //设置参数个数

    /* Add all key positions for argv[3...n] to keys[] */
    // key的参数的下标，保存在*keys中
    for (i = 0; i < num; i++) keys[i] = 3+i;

    return result->numkeys;
}

/* Helper function to extract keys from the SORT command.
 *
 * SORT <sort-key> ... STORE <store-key> ...
 *
 * The first argument of SORT is always a key, however a list of options
 * follow in SQL-alike style. Here we parse just the minimum in order to
 * correctly identify keys in the "STORE" option. */
/*
 * SORT key [BY pattern] [LIMIT offset count] [GET pattern [GET pattern ...]] [ASC | DESC] [ALPHA] [STORE destination]
 * 从SORT命令中获取key的下标
 * */
int sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, j, num, *keys, found_store = 0;
    UNUSED(cmd);    //不使用该指针，将其设置为void类型

    num = 0;
    //最多两个位置
    keys = getKeysPrepareResult(result, 2); /* Alloc 2 places for the worst case. */
    // <sort-key>的下标为1
    keys[num++] = 1; /* <sort-key> is always present. */

    /* Search for STORE option. By default we consider options to don't
     * have arguments, so if we find an unknown option name we scan the
     * next. However there are options with 1 or 2 arguments, so we
     * provide a list here in order to skip the right number of args. */
    //默认的的SORT命令是没有参数的，如果不是下面列表所有的参数，则
    struct {
        char *name;
        int skip;
    } skiplist[] = {
        {"limit", 2},
        {"get", 1},
        {"by", 1},
        {NULL, 0} /* End of elements. */
    };

    //从第三个参数开始遍历
    for (i = 2; i < argc; i++) {
        //遍历skiplist[]
        for (j = 0; skiplist[j].name != NULL; j++) {
            //如果当前选项等于skiplist[]的name
            if (!strcasecmp(argv[i]->ptr,skiplist[j].name)) {
                i += skiplist[j].skip;  //记录跳过的下标
                break;
                //如果是store选项
            } else if (!strcasecmp(argv[i]->ptr,"store") && i+1 < argc) {
                /* Note: we don't increment "num" here and continue the loop
                 * to be sure to process the *last* "STORE" option if multiple
                 * ones are provided. This is same behavior as SORT. */
                found_store = 1;     //设置发现store选项的标志
                // 将第二个key的下标保存在keys的第二个位置上
                keys[num] = i+1; /* <store-key> */
                break;
            }
        }
    }
    result->numkeys = num + found_store;
    return result->numkeys; //保存key的个数
}

/*
 * 将 key 原子性地从当前实例传送到目标实例的指定数据库上，一旦传送成功， key保证会出现在目标实例上，而当前实例上的 key 会被删除。
 * MIGRATE命令中获取key的下标
 * */
int migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num, first, *keys;
    UNUSED(cmd);     //不使用该指针，将其设置为void类型

    /* Assume the obvious form. */
    first = 3;  //第一个key的下标
    num = 1;    //key的计数器

    /* But check for the extended one with the KEYS option. */
    //MIGRATE是否有扩展的选项，如果有argc > 6
    if (argc > 6) {
        //遍历扩展的选项
        for (i = 6; i < argc; i++) {
            // 如果出现KEYS选项，且第三个参数长度为0，那么说明MIGRATE是批量迁移模式
            // 使用KEYS选项，并将普通键参数设置为空字符串。实际的键名将在KEYS参数本身之后提供
            if (!strcasecmp(argv[i]->ptr,"keys") &&
                sdslen(argv[3]->ptr) == 0)
            {
                first = i+1;        //第一个键的下标为KEYS选项之后的键的下标
                num = argc-first;   //key的个数
                break;
            }
        }
    }


    keys = getKeysPrepareResult(result, num);
    //设置下标
    for (i = 0; i < num; i++) keys[i] = first+i;
    result->numkeys = num;
    return num;
}

/* Helper function to extract keys from following commands:
 * GEORADIUS key x y radius unit [WITHDIST] [WITHHASH] [WITHCOORD] [ASC|DESC]
 *                             [COUNT count] [STORE key] [STOREDIST key]
 * GEORADIUSBYMEMBER key member radius unit ... options ... */
int georadiusGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num, *keys;
    UNUSED(cmd);

    /* Check for the presence of the stored key in the command */
    int stored_key = -1;
    for (i = 5; i < argc; i++) {
        char *arg = argv[i]->ptr;
        /* For the case when user specifies both "store" and "storedist" options, the
         * second key specified would override the first key. This behavior is kept
         * the same as in georadiusCommand method.
         */
        if ((!strcasecmp(arg, "store") || !strcasecmp(arg, "storedist")) && ((i+1) < argc)) {
            stored_key = i+1;
            i++;
        }
    }
    num = 1 + (stored_key == -1 ? 0 : 1);

    /* Keys in the command come from two places:
     * argv[1] = key,
     * argv[5...n] = stored key if present
     */
    keys = getKeysPrepareResult(result, num);

    /* Add all key positions to keys[] */
    keys[0] = 1;
    if(num > 1) {
         keys[1] = stored_key;
    }
    result->numkeys = num;
    return num;
}

/* LCS ... [KEYS <key1> <key2>] ... */
int lcsGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i;
    int *keys = getKeysPrepareResult(result, 2);
    UNUSED(cmd);

    /* We need to parse the options of the command in order to check for the
     * "KEYS" argument before the "STRINGS" argument. */
    for (i = 1; i < argc; i++) {
        char *arg = argv[i]->ptr;
        int moreargs = (argc-1) - i;

        if (!strcasecmp(arg, "strings")) {
            break;
        } else if (!strcasecmp(arg, "keys") && moreargs >= 2) {
            keys[0] = i+1;
            keys[1] = i+2;
            result->numkeys = 2;
            return result->numkeys;
        }
    }
    result->numkeys = 0;
    return result->numkeys;
}

/* Helper function to extract keys from memory command.
 * MEMORY USAGE <key> */
int memoryGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    UNUSED(cmd);

    getKeysPrepareResult(result, 1);
    if (argc >= 3 && !strcasecmp(argv[1]->ptr,"usage")) {
        result->keys[0] = 2;
        result->numkeys = 1;
        return result->numkeys;
    }
    result->numkeys = 0;
    return 0;
}

/* XREAD [BLOCK <milliseconds>] [COUNT <count>] [GROUP <groupname> <ttl>]
 *       STREAMS key_1 key_2 ... key_N ID_1 ID_2 ... ID_N */
int xreadGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result) {
    int i, num = 0, *keys;
    UNUSED(cmd);

    /* We need to parse the options of the command in order to seek the first
     * "STREAMS" string which is actually the option. This is needed because
     * "STREAMS" could also be the name of the consumer group and even the
     * name of the stream key. */
    int streams_pos = -1;
    for (i = 1; i < argc; i++) {
        char *arg = argv[i]->ptr;
        if (!strcasecmp(arg, "block")) {
            i++; /* Skip option argument. */
        } else if (!strcasecmp(arg, "count")) {
            i++; /* Skip option argument. */
        } else if (!strcasecmp(arg, "group")) {
            i += 2; /* Skip option argument. */
        } else if (!strcasecmp(arg, "noack")) {
            /* Nothing to do. */
        } else if (!strcasecmp(arg, "streams")) {
            streams_pos = i;
            break;
        } else {
            break; /* Syntax error. */
        }
    }
    if (streams_pos != -1) num = argc - streams_pos - 1;

    /* Syntax error. */
    if (streams_pos == -1 || num == 0 || num % 2 != 0) {
        result->numkeys = 0;
        return 0;
    }
    num /= 2; /* We have half the keys as there are arguments because
                 there are also the IDs, one per key. */

    keys = getKeysPrepareResult(result, num);
    for (i = streams_pos+1; i < argc-num; i++) keys[i-streams_pos-1] = i;
    result->numkeys = num;
    return num;
}

/* Slot to Key API. This is used by Redis Cluster in order to obtain in
 * a fast way a key that belongs to a specified hash slot. This is useful
 * while rehashing the cluster and in other conditions when we need to
 * understand if we have keys for a given hash slot. */
void slotToKeyUpdateKey(sds key, int add) {
    size_t keylen = sdslen(key);
    unsigned int hashslot = keyHashSlot(key,keylen);
    unsigned char buf[64];
    unsigned char *indexed = buf;

    server.cluster->slots_keys_count[hashslot] += add ? 1 : -1;
    if (keylen+2 > 64) indexed = zmalloc(keylen+2);
    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    memcpy(indexed+2,key,keylen);
    if (add) {
        raxInsert(server.cluster->slots_to_keys,indexed,keylen+2,NULL,NULL);
    } else {
        raxRemove(server.cluster->slots_to_keys,indexed,keylen+2,NULL);
    }
    if (indexed != buf) zfree(indexed);
}

void slotToKeyAdd(sds key) {
    slotToKeyUpdateKey(key,1);
}

void slotToKeyDel(sds key) {
    slotToKeyUpdateKey(key,0);
}

void slotToKeyFlush(void) {
    raxFree(server.cluster->slots_to_keys);
    server.cluster->slots_to_keys = raxNew();
    memset(server.cluster->slots_keys_count,0,
           sizeof(server.cluster->slots_keys_count));
}

/* Pupulate the specified array of objects with keys in the specified slot.
 * New objects are returned to represent keys, it's up to the caller to
 * decrement the reference count to release the keys names. */
/*
 * 记录count个属于hashslot槽的键到keys数组中
 * 返回 count 个 slot 槽中的键
 * */
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count) {
    raxIterator iter;
    int j = 0;
    unsigned char indexed[2];

    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    raxStart(&iter,server.cluster->slots_to_keys);
    raxSeek(&iter,">=",indexed,2);
    //遍历跳跃表
    while(count-- && raxNext(&iter)) {
        if (iter.key[0] != indexed[0] || iter.key[1] != indexed[1]) break;
        keys[j++] = createStringObject((char*)iter.key+2,iter.key_len-2);
    }
    raxStop(&iter);
    return j;
}

/* Remove all the keys in the specified hash slot.
 * The number of removed items is returned. */
/*
 * 删除指定hashslot中的所有key
 * */
unsigned int delKeysInSlot(unsigned int hashslot) {
    raxIterator iter;
    int j = 0;
    unsigned char indexed[2];

    indexed[0] = (hashslot >> 8) & 0xff;
    indexed[1] = hashslot & 0xff;
    raxStart(&iter,server.cluster->slots_to_keys);
    while(server.cluster->slots_keys_count[hashslot]) {
        raxSeek(&iter,">=",indexed,2);
        raxNext(&iter);

        robj *key = createStringObject((char*)iter.key+2,iter.key_len-2);
        dbDelete(&server.db[0],key);
        decrRefCount(key);
        j++;
    }
    raxStop(&iter);
    return j;
}

// 返回槽 slot 目前包含的键值对数量
unsigned int countKeysInSlot(unsigned int hashslot) {
    return server.cluster->slots_keys_count[hashslot];
}
