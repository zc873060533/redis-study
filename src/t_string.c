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
#include <math.h> /* isnan(), isinf() */

/*-----------------------------------------------------------------------------
 * String Commands
 *----------------------------------------------------------------------------*/

/**
 * 检测当前长度是否在最大字符串允许范围内
 * */
static int checkStringLength(client *c, long long size) {
    if (!(c->flags & CLIENT_MASTER) && size > server.proto_max_bulk_len) {
        addReplyError(c,"string exceeds maximum allowed size (proto-max-bulk-len)");
        return C_ERR;
    }
    return C_OK;
}

/* The setGenericCommand() function implements the SET operation with different
 * options and variants. This function is called in order to implement the
 * following commands: SET, SETEX, PSETEX, SETNX.
 *
 *  setGenericCommand() 函数实现了 SET 、 SETEX 、 PSETEX 和 SETNX 命令。
 *
 * 'flags' changes the behavior of the command (NX or XX, see below).
 *
 * flags 参数的值可以是 NX 或 XX ，它们的意义请见下文。
 *
 * 'expire' represents an expire to set in form of a Redis object as passed
 * by the user. It is interpreted according to the specified 'unit'.
 *
 * expire 定义了 Redis 对象的过期时间。
 *
 * 而这个过期时间的格式由 unit 参数指定。
 *
 * 'ok_reply' and 'abort_reply' is what the function will reply to the client
 * if the operation is performed, or when it is not because of NX or
 * XX flags.
 *
 * ok_reply 和 abort_reply 决定了命令回复的内容，
 * NX 参数和 XX 参数也会改变回复。
 *
 * If ok_reply is NULL "+OK" is used.
 * If abort_reply is NULL, "$-1" is used.
 *
 * 如果 ok_reply 为 NULL ，那么 "+OK" 被返回。
 * 如果 abort_reply 为 NULL ，那么 "$-1" 被返回。
 *
 * */

#define OBJ_SET_NO_FLAGS 0
#define OBJ_SET_NX (1<<0)          /* Set if key not exists. */
#define OBJ_SET_XX (1<<1)          /* Set if key exists. */
#define OBJ_SET_EX (1<<2)          /* Set if time in seconds is given */
#define OBJ_SET_PX (1<<3)          /* Set if time in ms in given */
#define OBJ_SET_KEEPTTL (1<<4)     /* Set and keep the ttl */

/**
 * 设置通用命令
 *  - 首先判断 set 的类型是 set_nx 还是 set_xx，如果是 nx 并且 key 已经存在则直接返回；如果是 xx 并且 key 不存在则直接返回。
 *  - 调用 setKey 方法将键值添加到对应的 Redis 数据库中。
 *  - 如果有过期时间，则调用 setExpire 将设置过期时间
 *  - 进行键空间通知
 *  - 返回对应的值给客户端。
 */
void setGenericCommand(client *c, int flags, robj *key, robj *val, robj *expire, int unit, robj *ok_reply, robj *abort_reply) {
    long long milliseconds = 0; /* initialized to avoid any harmness warning */

    //设置了过期时间；expire是 robj 类型，获取整数值
    if (expire) {

        //过期时间转换为long long类型，并存储在&milliseconds 中
        if (getLongLongFromObjectOrReply(c, expire, &milliseconds, NULL) != C_OK)
            return;

        //处理非法的时间值
        if (milliseconds <= 0) {
            addReplyErrorFormat(c,"invalid expire time in %s",c->cmd->name);
            return;
        }

        // 不论输入的过期时间是秒还是毫秒
        // Redis 实际都以毫秒的形式保存过期时间
        // 如果输入的过期时间为秒，那么将它转换为毫秒
        if (unit == UNIT_SECONDS) milliseconds *= 1000;
    }

    // NX，key存在时直接返回；XX，key不存在时直接返回
    // lookupKeyWrite 是在对应的数据库中寻找键值是否存在
    // 在条件不符合时报错，报错的内容由 abort_reply 参数决定
    if ((flags & OBJ_SET_NX && lookupKeyWrite(c->db,key) != NULL) ||
        (flags & OBJ_SET_XX && lookupKeyWrite(c->db,key) == NULL))
    {
        addReply(c, abort_reply ? abort_reply : shared.null[c->resp]);
        return;
    }

    // 添加到数据字典
    genericSetKey(c,c->db,key,val,flags & OBJ_SET_KEEPTTL,1);
    server.dirty++;

    // 过期时间添加到过期字典
    if (expire) setExpire(c,c->db,key,mstime()+milliseconds);

    // 键空间通知 通知监听了key的客户端，key被操作了set、expire命令
    notifyKeyspaceEvent(NOTIFY_STRING,"set",key,c->db->id);
    if (expire) notifyKeyspaceEvent(NOTIFY_GENERIC,
        "expire",key,c->db->id);

    //返回值
    addReply(c, ok_reply ? ok_reply : shared.ok);
}

/* SET key value [NX] [XX] [KEEPTTL] [EX <seconds>] [PX <milliseconds>] */
/**
 * set 命令
 * EX second ：设置键的过期时间为 second 秒。 SET key value EX second 效果等同于 SETEX key second value 。
 * PX millisecond ：设置键的过期时间为 millisecond 毫秒。 SET key value PX millisecond 效果等同于 PSETEX key millisecond value 。
 * NX ：只在键不存在时，才对键进行设置操作。 SET key value NX 效果等同于 SETNX key value 。
 * XX ：只在键已经存在时，才对键进行设置操作。
 * */
void setCommand(client *c) {
    int j;
    robj *expire = NULL;
    int unit = UNIT_SECONDS;
    int flags = OBJ_SET_NO_FLAGS;

    // 设置选项参数
    for (j = 3; j < c->argc; j++) {
        char *a = c->argv[j]->ptr;
        robj *next = (j == c->argc-1) ? NULL : c->argv[j+1];

        // NX 与 XX 互斥
        if ((a[0] == 'n' || a[0] == 'N') &&
            (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
            !(flags & OBJ_SET_XX))
        {
            flags |= OBJ_SET_NX;
        } else if ((a[0] == 'x' || a[0] == 'X') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_NX))
        {
            flags |= OBJ_SET_XX;
        } else if (!strcasecmp(c->argv[j]->ptr,"KEEPTTL") &&
                   !(flags & OBJ_SET_EX) && !(flags & OBJ_SET_PX))
        {
            flags |= OBJ_SET_KEEPTTL;
            // PX 与 EX 互斥
        } else if ((a[0] == 'e' || a[0] == 'E') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_KEEPTTL) &&
                   !(flags & OBJ_SET_PX) && next)
        {
            flags |= OBJ_SET_EX;
            unit = UNIT_SECONDS;
            expire = next;
            j++;
        } else if ((a[0] == 'p' || a[0] == 'P') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_KEEPTTL) &&
                   !(flags & OBJ_SET_EX) && next)
        {
            flags |= OBJ_SET_PX;
            unit = UNIT_MILLISECONDS;
            expire = next;
            j++;
        } else {
            //发送响应信息
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    // 尝试压缩 value 值以节省空间 (原始命令: set key value)
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,flags,c->argv[1],c->argv[2],expire,unit,NULL,NULL);
}

/** 设置string，并当key不存在时进行设置 */
void setnxCommand(client *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,OBJ_SET_NX,c->argv[1],c->argv[2],NULL,0,shared.cone,shared.czero);
}

/** 设置string，并携带过期时间，单位秒 */
void setexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_SECONDS,NULL,NULL);
}

/** 设置string，并携带过期时间，单位毫秒 */
void psetexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_MILLISECONDS,NULL,NULL);
}

/**
 * 获取通用指令
 * */
int getGenericCommand(client *c) {
    robj *o;

    // 尝试从数据库中取出键 c->argv[1] 对应的值对象
    // 如果键不存在时，向客户端发送回复信息，并返回 NULL
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.null[c->resp])) == NULL)
        return C_OK;

    // 值对象存在，检查它的类型
    if (o->type != OBJ_STRING) {
        // 类型错误
        addReply(c,shared.wrongtypeerr);
        return C_ERR;
    } else {
        // 类型正确，向客户端返回对象的值
        addReplyBulk(c,o);
        return C_OK;
    }
}

/**
* get命令
* 调用getGenericCommand函数实现具体操作
*/
void getCommand(client *c) {
    getGenericCommand(c);
}

/**
 * 以原子方式设置key为value并返回存储在中的旧值key。key存在但不包含字符串值时返回错误。
 * 请勿使用该命令，该命令将在6.2版本中废弃
 * */
void getsetCommand(client *c) {

    // 取出并返回键的值对象
    if (getGenericCommand(c) == C_ERR) return;

    // 编码键的新值 c->argv[2]
    c->argv[2] = tryObjectEncoding(c->argv[2]);

    // 将数据库中关联键 c->argv[1] 和新值对象 c->argv[2]
    setKey(c,c->db,c->argv[1],c->argv[2]);

    // 发送事件通知
    notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[1],c->db->id);

    // 更新服务器脏键
    server.dirty++;

}

/**
 * 偏移覆盖命令
 * redis> SET key1 "Hello World"
 * "OK"
 * redis> SETRANGE key1 6 "Redis"
 * (integer) 11
 * redis> GET key1
 * "Hello Redis"
 * */
void setrangeCommand(client *c) {
    robj *o;
    long offset;
    sds value = c->argv[3]->ptr;

    // 取出 offset 参数
    if (getLongFromObjectOrReply(c,c->argv[2],&offset,NULL) != C_OK)
        return;

    //如果偏移量小于0，直接返回
    if (offset < 0) {
        addReplyError(c,"offset is out of range");
        return;
    }

    //在对应的数据库中寻找键值是否存在
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {

        // 键不存在于数据库中。。。

        /* Return 0 when setting nothing on a non-existing string */
        // value 为空，没有什么可设置的，向客户端返回 0
        if (sdslen(value) == 0) {
            addReply(c,shared.czero);
            return;
        }

        // 当结果字符串超过允许的大小时返回（如果设置后的长度会超过 Redis 的限制的话 。  那么放弃设置，向客户端发送一个出错回复）
        if (checkStringLength(c,offset+sdslen(value)) != C_OK)
            return;

        // 如果 value 没有问题，可以设置，那么创建一个空字符串值对象
        // 并在数据库中关联键 c->argv[1] 和这个空字符串对象
        o = createObject(OBJ_STRING,sdsnewlen(NULL, offset+sdslen(value)));
        dbAdd(c->db,c->argv[1],o);
    } else {
        size_t olen;

        // 值对象存在。。。

        // 检查值对象的类型
        if (checkType(c,o,OBJ_STRING))
            return;

        // 取出原有字符串的长度
        olen = stringObjectLen(o);

        // 当前value为空，没有什么好设置的，直接返回
        if (sdslen(value) == 0) {
            addReplyLongLong(c,olen);
            return;
        }

        // 当结果字符串超过允许的大小时返回（如果设置后的长度会超过 Redis 的限制的话 。  那么放弃设置，向客户端发送一个出错回复）
        if (checkStringLength(c,offset+sdslen(value)) != C_OK)
            return;

        /* Create a copy when the object is shared or encoded. */
        o = dbUnshareStringValue(c->db,c->argv[1],o);
    }

    // 这里的 sdslen(value) > 0 其实可以去掉
    // 前面已经做了检测了
    if (sdslen(value) > 0) {
        // 扩展字符串值对象
        o->ptr = sdsgrowzero(o->ptr,offset+sdslen(value));
        // 将 value 复制到字符串中的指定的位置（赋值）
        memcpy((char*)o->ptr+offset,value,sdslen(value));

        // 向数据库发送键被修改的信号
        signalModifiedKey(c,c->db,c->argv[1]);

        // 发送事件通知
        notifyKeyspaceEvent(NOTIFY_STRING,
            "setrange",c->argv[1],c->db->id);

        // 更新服务器脏键
        server.dirty++;
    }

    // 设置成功，返回新的字符串值给客户端
    addReplyLongLong(c,sdslen(o->ptr));

}

/**
 * 返回键 key 储存的字符串值的指定部分， 字符串的截取范围由 start 和 end 两个偏移量决定 (包括 start 和 end 在内)。
 * GETRANGE key start end
 * redis> SET greeting "hello, my friend"
 * OK
 *
 * redis> GETRANGE greeting 0 4          # 返回索引0-4的字符，包括4。
 * "hello"
 *
 * redis> GETRANGE greeting -1 -5        # 不支持回绕操作
 * ""
 *
 * redis> GETRANGE greeting -3 -1        # 负数索引
 * "end"
 *
 * redis> GETRANGE greeting 0 -1         # 从第一个到最后一个
 * "hello, my friend"
 *
 * redis> GETRANGE greeting 0 1008611    # 值域范围不超过实际字符串，超过部分自动被符略
 * "hello, my friend"
 * */
void getrangeCommand(client *c) {
    robj *o;
    long long start, end;
    char *str, llbuf[32];
    size_t strlen;

    //解析开始偏移量
    if (getLongLongFromObjectOrReply(c,c->argv[2],&start,NULL) != C_OK)
        return;

    //解析结束偏移量
    if (getLongLongFromObjectOrReply(c,c->argv[3],&end,NULL) != C_OK)
        return;

    //查找数据库中指定key的对象并返回，查询出来的对象用于读操作 || 检测操作类型
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptybulk)) == NULL ||
        checkType(c,o,OBJ_STRING)) return;

    // 根据编码，对对象的值进行处理
    if (o->encoding == OBJ_ENCODING_INT) {//int类型
        str = llbuf;
        strlen = ll2string(llbuf,sizeof(llbuf),(long)o->ptr);
    } else {//string类型
        str = o->ptr;
        strlen = sdslen(str);
    }

    /* 偏移量检测 */
    if (start < 0 && end < 0 && start > end) {
        addReply(c,shared.emptybulk);
        return;
    }
    // 将负数索引转换为整数索引
    if (start < 0) start = strlen+start;
    if (end < 0) end = strlen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if ((unsigned long long)end >= strlen) end = strlen-1;

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * nothing can be returned is: start > end. */
    if (start > end || strlen == 0) {
        // 处理索引范围为空的情况
        addReply(c,shared.emptybulk);
    } else {
        // 向客户端返回给定范围内的字符串内容
        addReplyBulkCBuffer(c,(char*)str+start,end-start+1);
    }
}

/**
 * 返回给定的一个或多个字符串键的值。
 *
 * redis> SET redis redis.com
 * OK
 *
 * redis> SET mongodb mongodb.org
 * OK
 *
 * redis> MGET redis mongodb
 * 1) "redis.com"
 * 2) "mongodb.org"
 *
 * redis> MGET redis mongodb mysql     # 不存在的 mysql 返回 nil
 * 1) "redis.com"
 * 2) "mongodb.org"
 * 3) (nil)
 */
void mgetCommand(client *c) {
    int j;

    addReplyArrayLen(c,c->argc-1);
    // 查找并返回所有输入键的值
    for (j = 1; j < c->argc; j++) {
        // 查找键 c->argc[j] 的值
        robj *o = lookupKeyRead(c->db,c->argv[j]);
        if (o == NULL) {
            // 值不存在，向客户端发送空回复
            addReplyNull(c);
        } else {
            if (o->type != OBJ_STRING) {
                // 值存在，但不是字符串类型
                addReplyNull(c);
            } else {
                // 值存在，并且是字符串
                addReplyBulk(c,o);
            }
        }
    }

}

/**
 * 同时为多个键设置值。
 * */
void msetGenericCommand(client *c, int nx) {
    int j;

    // 键值参数不是成相成对出现的，格式不正确
    if ((c->argc % 2) == 0) {
        addReplyError(c,"wrong number of arguments for MSET");
        return;
    }

    /* Handle the NX flag. The MSETNX semantic is to return zero and don't
     * set anything if at least one key already exists. */
    // 如果 nx 参数为真，那么检查所有输入键在数据库中是否存在
    // 只要有一个键是存在的，那么就向客户端发送空回复
    // 并放弃执行接下来的设置操作
    if (nx) {
        for (j = 1; j < c->argc; j += 2) {
            // 键存在
            // 发送空白回复，并放弃执行接下来的设置操作
            if (lookupKeyWrite(c->db,c->argv[j]) != NULL) {
                addReply(c, shared.czero);
                return;
            }
        }
    }

    // 设置所有键值对
    for (j = 1; j < c->argc; j += 2) {

        // 对值对象进行解码
        c->argv[j+1] = tryObjectEncoding(c->argv[j+1]);

        // 将键值对关联到数据库
        // c->argc[j] 为键
        // c->argc[j+1] 为值
        setKey(c,c->db,c->argv[j],c->argv[j+1]);

        // 空间消息推送
        notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[j],c->db->id);
    }

    // 更新服务器脏键数量
    server.dirty += (c->argc-1)/2;

    // 返回客户端结果
    addReply(c, nx ? shared.cone : shared.ok);

}

/**
 * MSET key value [key value …]
 * 同时为多个键设置值。
 * 如果某个给定键已经存在， 那么 MSET 将使用新值去覆盖旧值， 如果这不是你所希望的效果， 请考虑使用 MSETNX 命令， 这个命令只会在所有给定键都不存在的情况下进行设置。
 * */
void msetCommand(client *c) {
    msetGenericCommand(c,0);
}

/**
 * MSET key value [key value …]
 * 同时为多个键设置值。
 * */
void msetnxCommand(client *c) {
    msetGenericCommand(c,1);
}

/**
 * 增减
 * 底层操作方法
 * 整型
 * */
void incrDecrCommand(client *c, long long incr) {
    long long value, oldvalue;
    robj *o, *new;

    // 取出值对象
    o = lookupKeyWrite(c->db,c->argv[1]);

    // 检查对象是否存在，以及类型是否正确
    if (o != NULL && checkType(c,o,OBJ_STRING)) return;

    // 取出对象的整数值，并保存到 value 参数中
    if (getLongLongFromObjectOrReply(c,o,&value,NULL) != C_OK) return;

    // 检查加法操作执行之后值释放会溢出
    // 如果是的话，就向客户端发送一个出错回复，并放弃设置操作
    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) { //判断不能不能越界，超出long long范围
        addReplyError(c,"increment or decrement would overflow");
        return;
    }

    // 进行加法计算，并将值保存到新的值对象中
    value += incr;

    //判别是否大于对象共享整型，value 是否在long最大与最小之间。共享对象整型大于10000。
    if (o && o->refcount == 1 && o->encoding == OBJ_ENCODING_INT &&
        (value < 0 || value >= OBJ_SHARED_INTEGERS) &&
        value >= LONG_MIN && value <= LONG_MAX)
    {
        new = o;
        o->ptr = (void*)((long)value);
    } else {
        // 然后用新的值对象替换原来的值对象
        new = createStringObjectFromLongLongForValue(value);
        // 是否存在可以写入
        if (o) {
            // 存在键值覆盖原来的字典
            dbOverwrite(c->db,c->argv[1],new);
        } else {
            // 原值不存在，进行初始化设置
            dbAdd(c->db,c->argv[1],new);
        }
    }

    // 向数据库发送键被修改的信号
    signalModifiedKey(c,c->db,c->argv[1]);

    // 发送事件通知
    notifyKeyspaceEvent(NOTIFY_STRING,"incrby",c->argv[1],c->db->id);

    // 更逊服务器脏键数量
    server.dirty++;

    // 返回回复
    addReply(c,shared.colon);
    addReply(c,new);
    addReply(c,shared.crlf);

}

/**
 * 自增
 * */
void incrCommand(client *c) {
    incrDecrCommand(c,1);
}

/**
 * 自减
 * */
void decrCommand(client *c) {
    incrDecrCommand(c,-1);
}

/**
 * 为键 key 储存的数字值加上增量 increment 。
 * */
void incrbyCommand(client *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;//变量获取
    incrDecrCommand(c,incr);
}

/**
 * 为键 key 储存的数字值加上减少 increment 。
 * */
void decrbyCommand(client *c) {
    long long incr;

    //标准转化incr为longlong类型
    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;
    incrDecrCommand(c,-incr);
}

/**
 * 增减
 * 底层操作方法
 * 浮点型
 * */
void incrbyfloatCommand(client *c) {
    long double incr, value;
    robj *o, *new, *aux1, *aux2;

    // 取出值对象
    o = lookupKeyWrite(c->db,c->argv[1]);

    // 检查对象是否存在，以及类型是否正确
    if (o != NULL && checkType(c,o,OBJ_STRING)) return;

    // 将对象的整数值保存到 value 参数中
    // 并取出 incr 参数的值
    if (getLongDoubleFromObjectOrReply(c,o,&value,NULL) != C_OK ||
        getLongDoubleFromObjectOrReply(c,c->argv[2],&incr,NULL) != C_OK)
        return;

    // 进行加法计算，并检查是否溢出
    value += incr;
    if (isnan(value) || isinf(value)) {//叠加类型判别
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }

    // 用一个包含新值的新对象替换现有的值对象
    new = createStringObjectFromLongDouble(value,1);
    if (o)
        // 增量数据直接重写
        dbOverwrite(c->db,c->argv[1],new);
    else
        // 新数据进行添加操作
        dbAdd(c->db,c->argv[1],new);

    // 向数据库发送键被修改的信号
    signalModifiedKey(c,c->db,c->argv[1]);

    // 发送事件通知
    notifyKeyspaceEvent(NOTIFY_STRING,"incrbyfloat",c->argv[1],c->db->id);

    // 数据库脏键增加
    server.dirty++;

    // 回复
    addReplyBulk(c,new);

    /* Always replicate INCRBYFLOAT as a SET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
    // 解决浮点型在AOF追加中产生的精确度问题，转化为set命令
    // 在传播 INCRBYFLOAT 命令时，总是用 SET 命令来替换 INCRBYFLOAT 命令
    // 从而防止因为不同的浮点精度和格式化造成 AOF 重启时的数据不一致
    aux1 = createStringObject("SET",3);
    rewriteClientCommandArgument(c,0,aux1);
    decrRefCount(aux1);
    rewriteClientCommandArgument(c,2,new);
    aux2 = createStringObject("KEEPTTL",7);
    rewriteClientCommandArgument(c,3,aux2);
    decrRefCount(aux2);
}

/**
 * 字符串追加
 * 如果键 key 已经存在并且它的值是一个字符串， APPEND 命令将把 value 追加到键 key 现有值的末尾。
 * 如果 key 不存在， APPEND 就简单地将键 key 的值设为 value ， 就像执行 SET key value 一样。
 * 返回
 *    追加后的字符串长度
 * */
void appendCommand(client *c) {
    size_t totlen;
    robj *o, *append;

    o = lookupKeyWrite(c->db,c->argv[1]);             // 取出键相应的值对象
    if (o == NULL) {                                       // 不存在，直接创建
        /* Create the key */
        c->argv[2] = tryObjectEncoding(c->argv[2]);     // 尝试压缩字符串
        dbAdd(c->db,c->argv[1],c->argv[2]);       // 数据库添加
        incrRefCount(c->argv[2]);                       // 引用叠加
        totlen = stringObjectLen(c->argv[2]);           // 获取value长度
    } else {
        /* Key exists, check type */
        if (checkType(c,o,OBJ_STRING))                      // 检测类型是否为字符串类型
            return;

        /* "append" is an argument, so always an sds */
        append = c->argv[2];
        totlen = stringObjectLen(o)+sdslen(append->ptr);    // 获取新value长度
        if (checkStringLength(c,totlen) != C_OK)            // 检查追加操作之后，字符串的长度是否符合 Redis 的限制
            return;

        /* Append the value */
        // 解除原有 value 的数据共享
        o = dbUnshareStringValue(c->db,c->argv[1],o);

        // value拼接，sdscatlen 可能会改变指针，需要重新接收，真是的数据操作
        o->ptr = sdscatlen(o->ptr,append->ptr,sdslen(append->ptr));
        totlen = sdslen(o->ptr);
    }

    // 向数据库发送键被修改的信号
    signalModifiedKey(c,c->db,c->argv[1]);

    // 发送事件通知
    notifyKeyspaceEvent(NOTIFY_STRING,"append",c->argv[1],c->db->id);

    // 更新服务器脏键数量
    server.dirty++;

    // 客户端相应
    addReplyLongLong(c,totlen);
}

/** 返回键 key 储存的字符串值的长度。 */
void strlenCommand(client *c) {
    robj *o;

    //key查找 || 类型检测
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_STRING)) return;

    // 返回value长度
    addReplyLongLong(c,stringObjectLen(o));
}


/* STRALGO -- Implement complex algorithms on strings.
 *
 * STRALGO <algorithm> ... arguments ... */
void stralgoLCS(client *c);     /* This implements the LCS algorithm. */
void stralgoCommand(client *c) {
    /* Select the algorithm. */
    if (!strcasecmp(c->argv[1]->ptr,"lcs")) {
        stralgoLCS(c);
    } else {
        addReply(c,shared.syntaxerr);
    }
}

/* STRALGO <algo> [IDX] [MINMATCHLEN <len>] [WITHMATCHLEN]
 *     STRINGS <string> <string> | KEYS <keya> <keyb>
 */
void stralgoLCS(client *c) {
    uint32_t i, j;
    long long minmatchlen = 0;
    sds a = NULL, b = NULL;
    int getlen = 0, getidx = 0, withmatchlen = 0;
    robj *obja = NULL, *objb = NULL;

    for (j = 2; j < (uint32_t)c->argc; j++) {
        char *opt = c->argv[j]->ptr;
        int moreargs = (c->argc-1) - j;

        if (!strcasecmp(opt,"IDX")) {
            getidx = 1;
        } else if (!strcasecmp(opt,"LEN")) {
            getlen = 1;
        } else if (!strcasecmp(opt,"WITHMATCHLEN")) {
            withmatchlen = 1;
        } else if (!strcasecmp(opt,"MINMATCHLEN") && moreargs) {
            if (getLongLongFromObjectOrReply(c,c->argv[j+1],&minmatchlen,NULL)
                != C_OK) goto cleanup;
            if (minmatchlen < 0) minmatchlen = 0;
            j++;
        } else if (!strcasecmp(opt,"STRINGS") && moreargs > 1) {
            if (a != NULL) {
                addReplyError(c,"Either use STRINGS or KEYS");
                goto cleanup;
            }
            a = c->argv[j+1]->ptr;
            b = c->argv[j+2]->ptr;
            j += 2;
        } else if (!strcasecmp(opt,"KEYS") && moreargs > 1) {
            if (a != NULL) {
                addReplyError(c,"Either use STRINGS or KEYS");
                goto cleanup;
            }
            obja = lookupKeyRead(c->db,c->argv[j+1]);
            objb = lookupKeyRead(c->db,c->argv[j+2]);
            if ((obja && obja->type != OBJ_STRING) ||
                (objb && objb->type != OBJ_STRING))
            {
                addReplyError(c,
                    "The specified keys must contain string values");
                /* Don't cleanup the objects, we need to do that
                 * only after callign getDecodedObject(). */
                obja = NULL;
                objb = NULL;
                goto cleanup;
            }
            obja = obja ? getDecodedObject(obja) : createStringObject("",0);
            objb = objb ? getDecodedObject(objb) : createStringObject("",0);
            a = obja->ptr;
            b = objb->ptr;
            j += 2;
        } else {
            addReply(c,shared.syntaxerr);
            goto cleanup;
        }
    }

    /* Complain if the user passed ambiguous parameters. */
    if (a == NULL) {
        addReplyError(c,"Please specify two strings: "
                        "STRINGS or KEYS options are mandatory");
        goto cleanup;
    } else if (getlen && getidx) {
        addReplyError(c,
            "If you want both the length and indexes, please "
            "just use IDX.");
        goto cleanup;
    }

    /* Compute the LCS using the vanilla dynamic programming technique of
     * building a table of LCS(x,y) substrings. */
    uint32_t alen = sdslen(a);
    uint32_t blen = sdslen(b);

    /* Setup an uint32_t array to store at LCS[i,j] the length of the
     * LCS A0..i-1, B0..j-1. Note that we have a linear array here, so
     * we index it as LCS[j+(blen+1)*j] */
    uint32_t *lcs = zmalloc((alen+1)*(blen+1)*sizeof(uint32_t));
    #define LCS(A,B) lcs[(B)+((A)*(blen+1))]

    /* Start building the LCS table. */
    for (uint32_t i = 0; i <= alen; i++) {
        for (uint32_t j = 0; j <= blen; j++) {
            if (i == 0 || j == 0) {
                /* If one substring has length of zero, the
                 * LCS length is zero. */
                LCS(i,j) = 0;
            } else if (a[i-1] == b[j-1]) {
                /* The len LCS (and the LCS itself) of two
                 * sequences with the same final character, is the
                 * LCS of the two sequences without the last char
                 * plus that last char. */
                LCS(i,j) = LCS(i-1,j-1)+1;
            } else {
                /* If the last character is different, take the longest
                 * between the LCS of the first string and the second
                 * minus the last char, and the reverse. */
                uint32_t lcs1 = LCS(i-1,j);
                uint32_t lcs2 = LCS(i,j-1);
                LCS(i,j) = lcs1 > lcs2 ? lcs1 : lcs2;
            }
        }
    }

    /* Store the actual LCS string in "result" if needed. We create
     * it backward, but the length is already known, we store it into idx. */
    uint32_t idx = LCS(alen,blen);
    sds result = NULL;        /* Resulting LCS string. */
    void *arraylenptr = NULL; /* Deffered length of the array for IDX. */
    uint32_t arange_start = alen, /* alen signals that values are not set. */
             arange_end = 0,
             brange_start = 0,
             brange_end = 0;

    /* Do we need to compute the actual LCS string? Allocate it in that case. */
    int computelcs = getidx || !getlen;
    if (computelcs) result = sdsnewlen(SDS_NOINIT,idx);

    /* Start with a deferred array if we have to emit the ranges. */
    uint32_t arraylen = 0;  /* Number of ranges emitted in the array. */
    if (getidx) {
        addReplyMapLen(c,2);
        addReplyBulkCString(c,"matches");
        arraylenptr = addReplyDeferredLen(c);
    }

    i = alen, j = blen;
    while (computelcs && i > 0 && j > 0) {
        int emit_range = 0;
        if (a[i-1] == b[j-1]) {
            /* If there is a match, store the character and reduce
             * the indexes to look for a new match. */
            result[idx-1] = a[i-1];

            /* Track the current range. */
            if (arange_start == alen) {
                arange_start = i-1;
                arange_end = i-1;
                brange_start = j-1;
                brange_end = j-1;
            } else {
                /* Let's see if we can extend the range backward since
                 * it is contiguous. */
                if (arange_start == i && brange_start == j) {
                    arange_start--;
                    brange_start--;
                } else {
                    emit_range = 1;
                }
            }
            /* Emit the range if we matched with the first byte of
             * one of the two strings. We'll exit the loop ASAP. */
            if (arange_start == 0 || brange_start == 0) emit_range = 1;
            idx--; i--; j--;
        } else {
            /* Otherwise reduce i and j depending on the largest
             * LCS between, to understand what direction we need to go. */
            uint32_t lcs1 = LCS(i-1,j);
            uint32_t lcs2 = LCS(i,j-1);
            if (lcs1 > lcs2)
                i--;
            else
                j--;
            if (arange_start != alen) emit_range = 1;
        }

        /* Emit the current range if needed. */
        uint32_t match_len = arange_end - arange_start + 1;
        if (emit_range) {
            if (minmatchlen == 0 || match_len >= minmatchlen) {
                if (arraylenptr) {
                    addReplyArrayLen(c,2+withmatchlen);
                    addReplyArrayLen(c,2);
                    addReplyLongLong(c,arange_start);
                    addReplyLongLong(c,arange_end);
                    addReplyArrayLen(c,2);
                    addReplyLongLong(c,brange_start);
                    addReplyLongLong(c,brange_end);
                    if (withmatchlen) addReplyLongLong(c,match_len);
                    arraylen++;
                }
            }
            arange_start = alen; /* Restart at the next match. */
        }
    }

    /* Signal modified key, increment dirty, ... */

    /* Reply depending on the given options. */
    if (arraylenptr) {
        addReplyBulkCString(c,"len");
        addReplyLongLong(c,LCS(alen,blen));
        setDeferredArrayLen(c,arraylenptr,arraylen);
    } else if (getlen) {
        addReplyLongLong(c,LCS(alen,blen));
    } else {
        addReplyBulkSds(c,result);
        result = NULL;
    }

    /* Cleanup. */
    sdsfree(result);
    zfree(lcs);

cleanup:
    if (obja) decrRefCount(obja);
    if (objb) decrRefCount(objb);
    return;
}

