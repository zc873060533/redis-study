/* Asynchronous replication implementation.
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
#include "cluster.h"
#include "bio.h"

#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>

void replicationDiscardCachedMaster(void);
void replicationResurrectCachedMaster(connection *conn);
void replicationSendAck(void);
void putSlaveOnline(client *slave);
int cancelReplicationHandshake(void);

/* We take a global flag to remember if this instance generated an RDB
 * because of replication, so that we can remove the RDB file in case
 * the instance is configured to have no persistence. */
int RDBGeneratedByReplication = 0;

/* --------------------------- Utility functions ---------------------------- */

/* Return the pointer to a string representing the slave ip:listening_port
 * pair. Mostly useful for logging, since we want to log a slave using its
 * IP address and its listening port which is more clear for the user, for
 * example: "Closing connection with replica 10.1.2.3:6380". */
// 返回一个字符串指针，指向从节点的ip:listening_port。对日志记录非常有用，因为我们想使用其IP地址和其监听端口来记录从节点，这对用户来说更为清晰。例如："Closing connection with slave 10.1.2.3:6380"
char *replicationGetSlaveName(client *c) {
    static char buf[NET_PEER_ID_LEN];
    char ip[NET_IP_STR_LEN];

    ip[0] = '\0';
    buf[0] = '\0';
    // 如果slave_ip保存有从节点的IP，则不执行anetPeerToString
    // 否则，获取连接client的IP和端口号，这里只获取IP保存在ip数组中
    if (c->slave_ip[0] != '\0' ||
        connPeerToString(c->conn,ip,sizeof(ip),NULL) != -1)
    {
        /* Note that the 'ip' buffer is always larger than 'c->slave_ip' */
        // 如果slave_ip保存有IP，则将从节点的IP拷贝到ip数组中
        if (c->slave_ip[0] != '\0') memcpy(ip,c->slave_ip,sizeof(c->slave_ip));

        // 如果slave_listening_port保存有从节点的端口号，则将ip和port以"[%s]:%d"的格式写入buf
        if (c->slave_listening_port)
            anetFormatAddr(buf,sizeof(buf),ip,c->slave_listening_port);
        else
            snprintf(buf,sizeof(buf),"%s:<unknown-replica-port>",ip);
    } else {
        snprintf(buf,sizeof(buf),"client id #%llu",
            (unsigned long long) c->id);
    }
    return buf;
}

/* Plain unlink() can block for quite some time in order to actually apply
 * the file deletion to the filesystem. This call removes the file in a
 * background thread instead. We actually just do close() in the thread,
 * by using the fact that if there is another instance of the same file open,
 * the foreground unlink() will only remove the fs name, and deleting the
 * file's storage space will only happen once the last reference is lost. */
int bg_unlink(const char *filename) {
    int fd = open(filename,O_RDONLY|O_NONBLOCK);
    if (fd == -1) {
        /* Can't open the file? Fall back to unlinking in the main thread. */
        return unlink(filename);
    } else {
        /* The following unlink() removes the name but doesn't free the
         * file contents because a process still has it open. */
        int retval = unlink(filename);
        if (retval == -1) {
            /* If we got an unlink error, we just return it, closing the
             * new reference we have to the file. */
            int old_errno = errno;
            close(fd);  /* This would overwrite our errno. So we saved it. */
            errno = old_errno;
            return -1;
        }
        bioCreateBackgroundJob(BIO_CLOSE_FILE,(void*)(long)fd,NULL,NULL);
        return 0; /* Success. */
    }
}

/* ---------------------------------- MASTER -------------------------------- */
// 创建复制操作的积压缓冲区Backlog
void createReplicationBacklog(void) {
    serverAssert(server.repl_backlog == NULL);
    // 复制操作的积压缓冲区分配空间，默认为1M大小
    server.repl_backlog = zmalloc(server.repl_backlog_size);
    // 复制积压缓冲区backlog中实际的数据长度为0
    server.repl_backlog_histlen = 0;
    // 复制积压缓冲区backlog当前的偏移量，下次写操作的下标为0
    server.repl_backlog_idx = 0;

    /* We don't have any data inside our buffer, but virtually the first
     * byte we have is the next byte that will be generated for the
     * replication stream. */
    // repl_backlog_off记录的是积压缓冲区针对复制的最近一部分备份的偏移量
    // 我们的缓冲区中没有任何数据，但实际上backlog的第一个字节的逻辑位置是master_repl_offset的下一个字节
    server.repl_backlog_off = server.master_repl_offset+1;
}

/* This function is called when the user modifies the replication backlog
 * size at runtime. It is up to the function to both update the
 * server.repl_backlog_size and to resize the buffer and setup it so that
 * it contains the same data as the previous one (possibly less data, but
 * the most recent bytes, or the same data and more free space in case the
 * buffer is enlarged). */
// 动态调整backlog的大小。
// 如果backlog是被扩大时，原有的数据会被保留
void resizeReplicationBacklog(long long newsize) {
    // 调整的大小不能小于最小的16k
    if (newsize < CONFIG_REPL_BACKLOG_MIN_SIZE)
        newsize = CONFIG_REPL_BACKLOG_MIN_SIZE;
    // 大小相等，直接返回
    if (server.repl_backlog_size == newsize) return;

    // 设置新的大小
    server.repl_backlog_size = newsize;
    if (server.repl_backlog != NULL) {
        /* What we actually do is to flush the old buffer and realloc a new
         * empty one. It will refill with new data incrementally.
         * The reason is that copying a few gigabytes adds latency and even
         * worse often we need to alloc additional space before freeing the
         * old buffer. */
        zfree(server.repl_backlog);
        // 重新分配newsize大小的空间
        server.repl_backlog = zmalloc(server.repl_backlog_size);
        // 初始化数据长度和下标
        server.repl_backlog_histlen = 0;
        server.repl_backlog_idx = 0;
        /* Next byte we have is... the next since the buffer is empty. */
        // 设置复制的偏移量 = 全局复制偏移量 + 1
        server.repl_backlog_off = server.master_repl_offset+1;
    }
}

// 释放复制积压缓冲区
void freeReplicationBacklog(void) {
    serverAssert(listLength(server.slaves) == 0);
    zfree(server.repl_backlog);
    server.repl_backlog = NULL;
}

/* Add data to the replication backlog.
 * This function also increments the global replication offset stored at
 * server.master_repl_offset, because there is no case where we want to feed
 * the backlog without incrementing the offset. */
// 添加数据到backlog中，并且会根据增量更新server.master_repl_offset的偏移量
void feedReplicationBacklog(void *ptr, size_t len) {
    unsigned char *p = ptr;

    // 更新全局复制的偏移量
    server.master_repl_offset += len;

    /* This is a circular buffer, so write as much data we can at every
     * iteration and rewind the "idx" index if we reach the limit. */
    // 环形的缓冲区，每次迭代时尽可能的写更多的数据。如果写到尾部要将下标idx重置
    while(len) {
        // 计算环形缓冲区还有多少空间
        size_t thislen = server.repl_backlog_size - server.repl_backlog_idx;
        // 如果空间足够，设置thislen写的长度为len
        if (thislen > len) thislen = len;
        // 空间不足够或着刚刚好，那么只写入剩余的空间数，等待下次循环时写入
        // 将数据拷贝到复制积压缓冲区中
        memcpy(server.repl_backlog+server.repl_backlog_idx,p,thislen);
        // 更新下次写的下标
        server.repl_backlog_idx += thislen;
        // 如果idx已经到达缓冲区的尾部，那么重置它
        if (server.repl_backlog_idx == server.repl_backlog_size)
            server.repl_backlog_idx = 0;
        // 更新未写入的数据长度
        len -= thislen;
        // 更新未写入数据的地址
        p += thislen;
        // 更新实际数据的长度
        server.repl_backlog_histlen += thislen;
    }
    // 实际数据的长度最大只能为复制缓冲区的大小，因为之后环形写入时会覆盖开头位置的数据
    if (server.repl_backlog_histlen > server.repl_backlog_size)
        server.repl_backlog_histlen = server.repl_backlog_size;
    /* Set the offset of the first byte we have in the backlog. */
    // 设置backlog所备份已复制的数据的偏移量，用于处理复制时的断线
    server.repl_backlog_off = server.master_repl_offset -
                              server.repl_backlog_histlen + 1;
}

/* Wrapper for feedReplicationBacklog() that takes Redis string objects
 * as input. */
// 使用字符串大小来封装feedReplicationBacklog()作为输入
void feedReplicationBacklogWithObject(robj *o) {
    char llstr[LONG_STR_SIZE];
    void *p;
    size_t len;

    // 整型编码转为字符串
    if (o->encoding == OBJ_ENCODING_INT) {
        len = ll2string(llstr,sizeof(llstr),(long)o->ptr);
        p = llstr;
    } else {
        len = sdslen(o->ptr);
        p = o->ptr;
    }
    // 写入backlog中
    feedReplicationBacklog(p,len);
}

/* Propagate write commands to slaves, and populate the replication backlog
 * as well. This function is used if the instance is a master: we use
 * the commands received by our clients in order to create the replication
 * stream. Instead if the instance is a slave and has sub-slaves attached,
 * we use replicationFeedSlavesFromMasterStream() */
// 将参数列表中的参数发送给从服务器
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int j, len;
    char llstr[LONG_STR_SIZE];

    /* If the instance is not a top level master, return ASAP: we'll just proxy
     * the stream of data we receive from our master instead, in order to
     * propagate *identical* replication stream. In this way this slave can
     * advertise the same replication ID as the master (since it shares the
     * master replication history and has the same backlog and offsets). */
    if (server.masterhost != NULL) return;

    /* If there aren't slaves, and there is no backlog buffer to populate,
     * we can return ASAP. */
    // 如果没有backlog且没有从节点服务器，直接返回
    if (server.repl_backlog == NULL && listLength(slaves) == 0) return;

    /* We can't have slaves attached and no backlog. */
    serverAssert(!(listLength(slaves) != 0 && server.repl_backlog == NULL));

    /* Send SELECT command to every slave if needed. */
    // 如果当前从节点使用的数据库不是目标的数据库，则要生成一个select命令
    if (server.slaveseldb != dictid) {
        robj *selectcmd;

        /* For a few DBs we have pre-computed SELECT command. */
        // 0 <= id < 10 ，可以使用共享的select命令对象
        if (dictid >= 0 && dictid < PROTO_SHARED_SELECT_CMDS) {
            selectcmd = shared.select[dictid];
            // 否则自行按照协议格式构建select命令对象
        } else {
            int dictid_len;

            dictid_len = ll2string(llstr,sizeof(llstr),dictid);
            selectcmd = createObject(OBJ_STRING,
                sdscatprintf(sdsempty(),
                "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n",
                dictid_len, llstr));
        }

        /* Add the SELECT command into the backlog. */
        // 将select 命令添加到backlog中
        if (server.repl_backlog) feedReplicationBacklogWithObject(selectcmd);

        /* Send it to slaves. */
        // 发送给从服务器
        listRewind(slaves,&li);
        // 遍历所有的从服务器节点
        while((ln = listNext(&li))) {
            client *slave = ln->value;
            // 从节点服务器状态为等待BGSAVE的开始，因此跳过回复，遍历下一个节点
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) continue;
            // 添加select命令到当前从节点的回复中
            addReply(slave,selectcmd);
        }

        // 释放临时对象
        if (dictid < 0 || dictid >= PROTO_SHARED_SELECT_CMDS)
            decrRefCount(selectcmd);
    }
    // 设置当前从节点使用的数据库ID
    server.slaveseldb = dictid;

    /* Write the command to the replication backlog if any. */
    // 将命令写到backlog中
    if (server.repl_backlog) {
        char aux[LONG_STR_SIZE+3];

        /* Add the multi bulk reply length. */
        // 将参数个数构建成协议标准的字符串
        // *<argc>\r\n
        aux[0] = '*';
        len = ll2string(aux+1,sizeof(aux)-1,argc);
        aux[len+1] = '\r';
        aux[len+2] = '\n';
        // 添加到backlog中
        feedReplicationBacklog(aux,len+3);

        // 遍历所有的参数
        for (j = 0; j < argc; j++) {
            // 返回参数对象的长度
            long objlen = stringObjectLen(argv[j]);

            /* We need to feed the buffer with the object as a bulk reply
             * not just as a plain string, so create the $..CRLF payload len
             * and add the final CRLF */
            // 构建成协议标准的字符串，并添加到backlog中
            // $<len>\r\n<argv>\r\n
            aux[0] = '$';
            len = ll2string(aux+1,sizeof(aux)-1,objlen);
            aux[len+1] = '\r';
            aux[len+2] = '\n';
            // 添加$<len>\r\n
            feedReplicationBacklog(aux,len+3);
            // 添加参数对象<argv>
            feedReplicationBacklogWithObject(argv[j]);
            // 添加\r\n
            feedReplicationBacklog(aux+len+1,2);
        }
    }

    /* Write the command to every slave. */
    // 将命令写到每一个从节点中
    listRewind(slaves,&li);
    // 遍历从节点链表
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        /* Don't feed slaves that are still waiting for BGSAVE to start. */
        // 从节点服务器状态为等待BGSAVE的开始，因此跳过回复，遍历下一个节点
        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) continue;

        /* Feed slaves that are waiting for the initial SYNC (so these commands
         * are queued in the output buffer until the initial SYNC completes),
         * or are already in sync with the master. */
        // 将命令写给正在等待初次SYNC的从节点（所以这些命令在输出缓冲区中排队，直到初始SYNC完成），或已经与主节点同步

        /* Add the multi bulk length. */
        // 添加回复的长度
        addReplyArrayLen(slave,argc);

        /* Finally any additional argument that was not stored inside the
         * static buffer if any (from j to argc). */
        // 将所有的参数列表添加到从节点的输出缓冲区
        for (j = 0; j < argc; j++)
            addReplyBulk(slave,argv[j]);
    }
}

/* This is a debugging function that gets called when we detect something
 * wrong with the replication protocol: the goal is to peek into the
 * replication backlog and show a few final bytes to make simpler to
 * guess what kind of bug it could be. */
void showLatestBacklog(void) {
    if (server.repl_backlog == NULL) return;

    long long dumplen = 256;
    if (server.repl_backlog_histlen < dumplen)
        dumplen = server.repl_backlog_histlen;

    /* Identify the first byte to dump. */
    long long idx =
      (server.repl_backlog_idx + (server.repl_backlog_size - dumplen)) %
       server.repl_backlog_size;

    /* Scan the circular buffer to collect 'dumplen' bytes. */
    sds dump = sdsempty();
    while(dumplen) {
        long long thislen =
            ((server.repl_backlog_size - idx) < dumplen) ?
            (server.repl_backlog_size - idx) : dumplen;

        dump = sdscatrepr(dump,server.repl_backlog+idx,thislen);
        dumplen -= thislen;
        idx = 0;
    }

    /* Finally log such bytes: this is vital debugging info to
     * understand what happened. */
    serverLog(LL_WARNING,"Latest backlog is: '%s'", dump);
    sdsfree(dump);
}

/* This function is used in order to proxy what we receive from our master
 * to our sub-slaves. */
#include <ctype.h>
void replicationFeedSlavesFromMasterStream(list *slaves, char *buf, size_t buflen) {
    listNode *ln;
    listIter li;

    /* Debugging: this is handy to see the stream sent from master
     * to slaves. Disabled with if(0). */
    if (0) {
        printf("%zu:",buflen);
        for (size_t j = 0; j < buflen; j++) {
            printf("%c", isprint(buf[j]) ? buf[j] : '.');
        }
        printf("\n");
    }

    if (server.repl_backlog) feedReplicationBacklog(buf,buflen);
    listRewind(slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        /* Don't feed slaves that are still waiting for BGSAVE to start. */
        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) continue;
        addReplyProto(slave,buf,buflen);
    }
}

// 将参数列表中的参数发送给监控器
void replicationFeedMonitors(client *c, list *monitors, int dictid, robj **argv, int argc) {
    listNode *ln;
    listIter li;
    int j;
    sds cmdrepr = sdsnew("+");
    robj *cmdobj;
    struct timeval tv;

    // 获取当前时间
    gettimeofday(&tv,NULL);
    // 将时间保存在cmdrepr中
    cmdrepr = sdscatprintf(cmdrepr,"%ld.%06ld ",(long)tv.tv_sec,(long)tv.tv_usec);
    // 根据client不同的状态，将不同信息追加到cmdrepr中
    if (c->flags & CLIENT_LUA) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d lua] ",dictid);
    } else if (c->flags & CLIENT_UNIX_SOCKET) {
        cmdrepr = sdscatprintf(cmdrepr,"[%d unix:%s] ",dictid,server.unixsocket);
    } else {
        cmdrepr = sdscatprintf(cmdrepr,"[%d %s] ",dictid,getClientPeerId(c));
    }

    // 遍历所有的参数，将参数添加到cmdrepr中
    for (j = 0; j < argc; j++) {
        if (argv[j]->encoding == OBJ_ENCODING_INT) {
            cmdrepr = sdscatprintf(cmdrepr, "\"%ld\"", (long)argv[j]->ptr);
        } else {
            cmdrepr = sdscatrepr(cmdrepr,(char*)argv[j]->ptr,
                        sdslen(argv[j]->ptr));
        }
        if (j != argc-1)
            cmdrepr = sdscatlen(cmdrepr," ",1);
    }
    cmdrepr = sdscatlen(cmdrepr,"\r\n",2);
    // 将cmdrepr构建成字符串对象
    cmdobj = createObject(OBJ_STRING,cmdrepr);

    listRewind(monitors,&li);
    // 遍历监控器链表
    while((ln = listNext(&li))) {
        client *monitor = ln->value;
        // 将命令对象添加到当前监控器的回复中
        addReply(monitor,cmdobj);
    }
    decrRefCount(cmdobj);
}

/* Feed the slave 'c' with the replication backlog starting from the
 * specified 'offset' up to the end of the backlog. */
// 将backlog所备份已复制数据的一部分，按照指定的offset发送给client
// 返回写入client的数据长度
long long addReplyReplicationBacklog(client *c, long long offset) {
    long long j, skip, len;

    serverLog(LL_DEBUG, "[PSYNC] Replica request offset: %lld", offset);

    // backlog中没有数据，返回0
    if (server.repl_backlog_histlen == 0) {
        serverLog(LL_DEBUG, "[PSYNC] Backlog history len is zero");
        return 0;
    }

    serverLog(LL_DEBUG, "[PSYNC] Backlog size: %lld",
             server.repl_backlog_size);
    serverLog(LL_DEBUG, "[PSYNC] First byte: %lld",
             server.repl_backlog_off);
    serverLog(LL_DEBUG, "[PSYNC] History len: %lld",
             server.repl_backlog_histlen);
    serverLog(LL_DEBUG, "[PSYNC] Current index: %lld",
             server.repl_backlog_idx);

    /* Compute the amount of bytes we need to discard. */
    // 计算需要跳过的数据长度
    skip = offset - server.repl_backlog_off;
    serverLog(LL_DEBUG, "[PSYNC] Skipping: %lld", skip);

    /* Point j to the oldest byte, that is actually our
     * server.repl_backlog_off byte. */
    // 继续写入数据的起始下标
    j = (server.repl_backlog_idx +
        (server.repl_backlog_size-server.repl_backlog_histlen)) %
        server.repl_backlog_size;
    serverLog(LL_DEBUG, "[PSYNC] Index of first byte: %lld", j);

    /* Discard the amount of data to seek to the specified 'offset'. */
    // 根据offset要跳过一些数据
    j = (j + skip) % server.repl_backlog_size;

    /* Feed slave with data. Since it is a circular buffer we have to
     * split the reply in two parts if we are cross-boundary. */
    // 计算要写入数据的长度
    len = server.repl_backlog_histlen - skip;
    serverLog(LL_DEBUG, "[PSYNC] Reply total length: %lld", len);
    while(len) {
        // 计算backlog最多写入的长度
        long long thislen =
            ((server.repl_backlog_size - j) < len) ?
            (server.repl_backlog_size - j) : len;

        serverLog(LL_DEBUG, "[PSYNC] addReply() length: %lld", thislen);
        // 写入到client的输出缓冲区中
        addReplySds(c,sdsnewlen(server.repl_backlog + j, thislen));
        len -= thislen;
        j = 0;
    }
    // 返回写入的长度
    return server.repl_backlog_histlen - skip;
}

/* Return the offset to provide as reply to the PSYNC command received
 * from the slave. The returned value is only valid immediately after
 * the BGSAVE process started and before executing any other command
 * from clients. */
long long getPsyncInitialOffset(void) {
    return server.master_repl_offset;
}

/* Send a FULLRESYNC reply in the specific case of a full resynchronization,
 * as a side effect setup the slave for a full sync in different ways:
 *
 * 1) Remember, into the slave client structure, the replication offset
 *    we sent here, so that if new slaves will later attach to the same
 *    background RDB saving process (by duplicating this client output
 *    buffer), we can get the right offset from this slave.
 * 2) Set the replication state of the slave to WAIT_BGSAVE_END so that
 *    we start accumulating differences from this point.
 * 3) Force the replication stream to re-emit a SELECT statement so
 *    the new slave incremental differences will start selecting the
 *    right database number.
 *
 * Normally this function should be called immediately after a successful
 * BGSAVE for replication was started, or when there is one already in
 * progress that we attached our slave to. */
// 在完全重新同步的特定情况下发送FULLRESYNC回复，以以下不同的方式设置从服务器进行完全同步：
/*
    1.我们在这里发送的偏移量到从节点的客户端结构中，以便如果新的从节点随后将附加到相同的BGSAVE进程（通过复制此客户端输出缓冲区），我们可以从该从节点获得正确的偏移量
    2.将从节点的复制状态设置为WAIT_BGSAVE_END，以便从此开始积累差异
    3.强制复制流重新发出SELECT命令，以便新的从节点增量差异可以选择正确的数据库编号
*/
// 通常在启动成功的BGSAVE进行复制后立即调用此函数，或者当我们的从节点已经有一个在执行BGSAVE
// 设置全量重同步从节点的状态
int replicationSetupSlaveForFullResync(client *slave, long long offset) {
    char buf[128];
    int buflen;

    // 设置全量重同步的偏移量
    slave->psync_initial_offset = offset;
    // 设置从节点复制状态，开始累计差异数据
    slave->replstate = SLAVE_STATE_WAIT_BGSAVE_END;
    /* We are going to accumulate the incremental changes for this
     * slave as well. Set slaveseldb to -1 in order to force to re-emit
     * a SELECT statement in the replication stream. */
    // 将slaveseldb设置为-1，是为了强制发送一个select命令在复制流中
    server.slaveseldb = -1;

    /* Don't send this reply to slaves that approached us with
     * the old SYNC command. */
    // 如果从节点的状态是CLIENT_PRE_PSYNC，则表示是Redis是2.8之前的版本，则不将这些信息发送给从节点。
    // 因为在2.8之前只支持SYNC的全量复制同步，而在之后的版本提供了部分的重同步
    if (!(slave->flags & CLIENT_PRE_PSYNC)) {
        buflen = snprintf(buf,sizeof(buf),"+FULLRESYNC %s %lld\r\n",
                          server.replid,offset);
        // 否则会将全量复制的信息写给从节点
        if (connWrite(slave->conn,buf,buflen) != buflen) {
            freeClientAsync(slave);
            return C_ERR;
        }
    }
    return C_OK;
}

/* This function handles the PSYNC command from the point of view of a
 * master receiving a request for partial resynchronization.
 *
 * On success return C_OK, otherwise C_ERR is returned and we proceed
 * with the usual full resync. */
// 该函数从主节点接收到部分重新同步请求的角度处理PSYNC命令
// 成功返回C_OK，否则返回C_ERR
int masterTryPartialResynchronization(client *c) {
    long long psync_offset, psync_len;
    char *master_replid = c->argv[1]->ptr;   //主节点的运行ID
    char buf[128];
    int buflen;

    /* Parse the replication offset asked by the slave. Go to full sync
     * on parse error: this should never happen but we try to handle
     * it in a robust way compared to aborting. */
    if (getLongLongFromObjectOrReply(c,c->argv[2],&psync_offset,NULL) !=
       C_OK) goto need_full_resync;

    /* Is the replication ID of this master the same advertised by the wannabe
     * slave via PSYNC? If the replication ID changed this master has a
     * different replication history, and there is no way to continue.
     *
     * Note that there are two potentially valid replication IDs: the ID1
     * and the ID2. The ID2 however is only valid up to a specific offset. */
    // 主节点的运行ID是否和从节点执行PSYNC的参数提供的运行ID相同。
    // 如果运行ID发生了改变，则主节点是一个不同的实例，那么就不能进行继续执行原有的复制进程
    if (strcasecmp(master_replid, server.replid) &&
        (strcasecmp(master_replid, server.replid2) ||
         psync_offset > server.second_replid_offset))
    {
        /* Replid "?" is used by slaves that want to force a full resync. */
        // 如果从节点的运行ID是"?"，表示想要强制进行一个全量同步
        if (master_replid[0] != '?') {
            if (strcasecmp(master_replid, server.replid) &&
                strcasecmp(master_replid, server.replid2))
            {
                serverLog(LL_NOTICE,"Partial resynchronization not accepted: "
                    "Replication ID mismatch (Replica asked for '%s', my "
                    "replication IDs are '%s' and '%s')",
                    master_replid, server.replid, server.replid2);
            } else {
                serverLog(LL_NOTICE,"Partial resynchronization not accepted: "
                    "Requested offset for second ID was %lld, but I can reply "
                    "up to %lld", psync_offset, server.second_replid_offset);
            }
        } else {
            serverLog(LL_NOTICE,"Full resync requested by replica %s",
                replicationGetSlaveName(c));
        }
        goto need_full_resync;
    }

    /* We still have the data our slave is asking for? */
    // 如果psync_offset小于repl_backlog_off，说明backlog所备份的数据的已经太新了，有一些数据被覆盖，则需要进行全量复制
    // 如果psync_offset大于(server.repl_backlog_off + server.repl_backlog_histlen)，表示当前backlog的数据不够全，则需要进行全量复制
    if (!server.repl_backlog ||
        psync_offset < server.repl_backlog_off ||
        psync_offset > (server.repl_backlog_off + server.repl_backlog_histlen))
    {
        serverLog(LL_NOTICE,
            "Unable to partial resync with replica %s for lack of backlog (Replica request was: %lld).", replicationGetSlaveName(c), psync_offset);
        if (psync_offset > server.master_repl_offset) {
            serverLog(LL_WARNING,
                "Warning: replica %s tried to PSYNC with an offset that is greater than the master replication offset.", replicationGetSlaveName(c));
        }
        goto need_full_resync;
    }

    /* If we reached this point, we are able to perform a partial resync:
     * 1) Set client state to make it a slave.
     * 2) Inform the client we can continue with +CONTINUE
     * 3) Send the backlog data (from the offset to the end) to the slave. */
    // 执行到这里，则可以进行部分重同步
    // 1. 设置client状态为从节点
    // 2. 向从节点发送 +CONTINUE 表示接受 partial resync 被接受
    // 3. 发送backlog的数据给从节点

    // 设置client状态为从节点
    c->flags |= CLIENT_SLAVE;
    // 设置复制状态为在线，此时RDB文件传输完成，发送差异数据
    c->replstate = SLAVE_STATE_ONLINE;
    // 设置从节点收到ack的时间
    c->repl_ack_time = server.unixtime;
    // slave向master发送ack标志设置为0
    c->repl_put_online_on_ack = 0;
    // 将当前client加入到从节点链表中
    listAddNodeTail(server.slaves,c);
    /* We can't use the connection buffers since they are used to accumulate
     * new commands at this stage. But we are sure the socket send buffer is
     * empty so this write will never fail actually. */
    // 向从节点发送 +CONTINUE
    if (c->slave_capa & SLAVE_CAPA_PSYNC2) {
        buflen = snprintf(buf,sizeof(buf),"+CONTINUE %s\r\n", server.replid);
    } else {
        buflen = snprintf(buf,sizeof(buf),"+CONTINUE\r\n");
    }
    if (connWrite(c->conn,buf,buflen) != buflen) {
        freeClientAsync(c);
        return C_OK;
    }
    // 将backlog的数据发送从节点
    psync_len = addReplyReplicationBacklog(c,psync_offset);
    serverLog(LL_NOTICE,
        "Partial resynchronization request from %s accepted. Sending %lld bytes of backlog starting from offset %lld.",
            replicationGetSlaveName(c),
            psync_len, psync_offset);
    /* Note that we don't need to set the selected DB at server.slaveseldb
     * to -1 to force the master to emit SELECT, since the slave already
     * has this state from the previous connection with the master. */

    // 计算延迟值小于min-slaves-max-lag的从节点的个数
    refreshGoodSlavesCount();

    /* Fire the replica change modules event. */
    moduleFireServerEvent(REDISMODULE_EVENT_REPLICA_CHANGE,
                          REDISMODULE_SUBEVENT_REPLICA_CHANGE_ONLINE,
                          NULL);

    return C_OK; /* The caller can return, no full resync needed. */

need_full_resync:
    /* We need a full resync for some reason... Note that we can't
     * reply to PSYNC right now if a full SYNC is needed. The reply
     * must include the master offset at the time the RDB file we transfer
     * is generated, so we need to delay the reply to that moment. */
    return C_ERR;
}

/* Start a BGSAVE for replication goals, which is, selecting the disk or
 * socket target depending on the configuration, and making sure that
 * the script cache is flushed before to start.
 *
 * The mincapa argument is the bitwise AND among all the slaves capabilities
 * of the slaves waiting for this BGSAVE, so represents the slave capabilities
 * all the slaves support. Can be tested via SLAVE_CAPA_* macros.
 *
 * Side effects, other than starting a BGSAVE:
 *
 * 1) Handle the slaves in WAIT_START state, by preparing them for a full
 *    sync if the BGSAVE was successfully started, or sending them an error
 *    and dropping them from the list of slaves.
 *
 * 2) Flush the Lua scripting script cache if the BGSAVE was actually
 *    started.
 *
 * Returns C_OK on success or C_ERR otherwise. */
// 开始为复制执行BGSAVE，根据配置选择磁盘或套接字作为RDB发送的目标，在开始之前确保冲洗脚本缓存
// mincapa参数是SLAVE_CAPA_*按位与的结果
int startBgsaveForReplication(int mincapa) {
    int retval;
    // 是否直接写到socket
    int socket_target = server.repl_diskless_sync && (mincapa & SLAVE_CAPA_EOF);
    listIter li;
    listNode *ln;

    serverLog(LL_NOTICE,"Starting BGSAVE for SYNC with target: %s",
        socket_target ? "replicas sockets" : "disk");

    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);
    /* Only do rdbSave* when rsiptr is not NULL,
     * otherwise slave will miss repl-stream-db. */
    if (rsiptr) {
        if (socket_target)
            // 直接写到socket中
            // fork一个子进程将rdb写到 状态为等待BGSAVE开始 的从节点的socket中
            retval = rdbSaveToSlavesSockets(rsiptr);
        else
            // 否则后台进行RDB持久化BGSAVE操作，保存到磁盘上
            retval = rdbSaveBackground(server.rdb_filename,rsiptr);
    } else {
        serverLog(LL_WARNING,"BGSAVE for replication: replication information not available, can't generate the RDB file right now. Try later.");
        retval = C_ERR;
    }

    /* If we succeeded to start a BGSAVE with disk target, let's remember
     * this fact, so that we can later delete the file if needed. Note
     * that we don't set the flag to 1 if the feature is disabled, otherwise
     * it would never be cleared: the file is not deleted. This way if
     * the user enables it later with CONFIG SET, we are fine. */
    if (retval == C_OK && !socket_target && server.rdb_del_sync_files)
        RDBGeneratedByReplication = 1;

    /* If we failed to BGSAVE, remove the slaves waiting for a full
     * resynchronization from the list of slaves, inform them with
     * an error about what happened, close the connection ASAP. */
    // BGSAVE执行错误，将等待全量同步的从节点从从节点链表中删除，打印发生错误，立即关闭连接
    if (retval == C_ERR) {
        serverLog(LL_WARNING,"BGSAVE for replication failed");
        listRewind(server.slaves,&li);
        // 遍历从节点链表
        while((ln = listNext(&li))) {
            client *slave = ln->value;

            // 将等待全量同步的从节点从从节点链表中删除
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                slave->replstate = REPL_STATE_NONE;
                slave->flags &= ~CLIENT_SLAVE;
                listDelNode(server.slaves,ln);
                addReplyError(slave,
                    "BGSAVE failed, replication can't continue");
                // 立即关闭client的连接
                slave->flags |= CLIENT_CLOSE_AFTER_REPLY;
            }
        }
        return retval;
    }

    /* If the target is socket, rdbSaveToSlavesSockets() already setup
     * the slaves for a full resync. Otherwise for disk target do it now.*/
    // 如果是直接写到socket中，rdbSaveToSlavesSockets()已经会设置从节点为全量复制
    // 否则直接写到磁盘上，执行以下代码
    if (!socket_target) {
        listRewind(server.slaves,&li);
        // 遍历从节点链表
        while((ln = listNext(&li))) {
            client *slave = ln->value;

            // 设置等待全量同步的从节点的状态
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                // 设置要执行全量重同步从节点的状态
                    replicationSetupSlaveForFullResync(slave,
                            getPsyncInitialOffset());
            }
        }
    }

    /* Flush the script cache, since we need that slave differences are
     * accumulated without requiring slaves to match our cached scripts. */
    // 刷新脚本的缓存
    if (retval == C_OK) replicationScriptCacheFlush();
    return retval;
}

/* SYNC and PSYNC command implementation. */
// SYNC and PSYNC 命令实现
void syncCommand(client *c) {
    /* ignore SYNC if already slave or in monitor mode */
    // 如果client是从节点，那么忽略同步命令
    if (c->flags & CLIENT_SLAVE) return;

    /* Refuse SYNC requests if we are a slave but the link with our master
     * is not ok... */
    // 如果服务器是从节点，但是状态未处于和主节点连接状态，则发送错误回复，直接返回
    if (server.masterhost && server.repl_state != REPL_STATE_CONNECTED) {
        addReplySds(c,sdsnew("-NOMASTERLINK Can't SYNC while not connected with my master\r\n"));
        return;
    }

    /* SYNC can't be issued when the server has pending data to send to
     * the client about already issued commands. We need a fresh reply
     * buffer registering the differences between the BGSAVE and the current
     * dataset, so that we can copy to other slaves if needed. */
    // 如果指定的client的回复缓冲区中还有数据，则不能执行同步
    if (clientHasPendingReplies(c)) {
        addReplyError(c,"SYNC and PSYNC are invalid with pending output");
        return;
    }

    serverLog(LL_NOTICE,"Replica %s asks for synchronization",
        replicationGetSlaveName(c));

    /* Try a partial resynchronization if this is a PSYNC command.
     * If it fails, we continue with usual full resynchronization, however
     * when this happens masterTryPartialResynchronization() already
     * replied with:
     *
     * +FULLRESYNC <replid> <offset>
     *
     * So the slave knows the new replid and offset to try a PSYNC later
     * if the connection with the master is lost. */
    // 尝试执行一个部分同步PSYNC的命令，则masterTryPartialResynchronization()会回复一个 "+FULLRESYNC <runid> <offset>",如果失败则执行全量同步
    // 所以，从节点会如果和主节点连接断开，从节点会知道runid和offset，随后会尝试执行PSYNC

    // 如果是执行PSYNC命令
    if (!strcasecmp(c->argv[0]->ptr,"psync")) {
        // 主节点尝试执行部分重同步，执行成功返回C_OK
        if (masterTryPartialResynchronization(c) == C_OK) {
            // 可以执行PSYNC命令，则将接受PSYNC命令的个数加1
            server.stat_sync_partial_ok++;
            // 不需要执行后面的全量同步，直接返回
            return; /* No full resync needed, return. */
            // 不能执行PSYNC部分重同步，需要进行全量同步
        } else {
            char *master_replid = c->argv[1]->ptr;

            /* Increment stats for failed PSYNCs, but only if the
             * replid is not "?", as this is used by slaves to force a full
             * resync on purpose when they are not albe to partially
             * resync. */
            // 从节点以强制全量同步为目的，所以不能执行部分重同步，因此增加PSYNC命令失败的次数
            if (master_replid[0] != '?') server.stat_sync_partial_err++;
        }
        // 执行SYNC命令
    } else {
        /* If a slave uses SYNC, we are dealing with an old implementation
         * of the replication protocol (like redis-cli --slave). Flag the client
         * so that we don't expect to receive REPLCONF ACK feedbacks. */
        // 设置标识，执行SYNC命令，不接受REPLCONF ACK
        c->flags |= CLIENT_PRE_PSYNC;
    }

    /* Full resynchronization. */
    // 全量重同步次数加1
    server.stat_sync_full++;

    /* Setup the slave as one waiting for BGSAVE to start. The following code
     * paths will change the state if we handle the slave differently. */
    // 设置client状态为：从服务器节点等待BGSAVE节点的开始
    c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
    // 执行SYNC命令后是否关闭TCP_NODELAY
    if (server.repl_disable_tcp_nodelay)
        // 是的话，则启用nagle算法
        connDisableTcpNoDelay(c->conn); /* Non critical if it fails. */
    // 保存主服务器传来的RDB文件的fd，设置为-1
    c->repldbfd = -1;
    // 设置client状态为从节点，标识client是一个从服务器
    c->flags |= CLIENT_SLAVE;
    // 添加到服务器从节点链表中
    listAddNodeTail(server.slaves,c);

    /* Create the replication backlog if needed. */
    if (listLength(server.slaves) == 1 && server.repl_backlog == NULL) {
        /* When we create the backlog from scratch, we always use a new
         * replication ID and clear the ID2, since there is no valid
         * past history. */
        changeReplicationId();
        clearReplicationId2();
        createReplicationBacklog();
        serverLog(LL_NOTICE,"Replication backlog created, my new "
                            "replication IDs are '%s' and '%s'",
                            server.replid, server.replid2);
    }

    /* CASE 1: BGSAVE is in progress, with disk target. */
    // 情况1. 正在执行 BGSAVE ，且是同步到磁盘上
    if (server.rdb_child_pid != -1 &&
        server.rdb_child_type == RDB_CHILD_TYPE_DISK)
    {
        /* Ok a background save is in progress. Let's check if it is a good
         * one for replication, i.e. if there is another slave that is
         * registering differences since the server forked to save. */
        client *slave;
        listNode *ln;
        listIter li;

        listRewind(server.slaves,&li);
        // 遍历从节点链表
        while((ln = listNext(&li))) {
            slave = ln->value;
            // 如果有从节点已经创建子进程执行写RDB操作，等待完成，那么退出循环
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) break;
        }
        /* To attach this slave, we check that it has at least all the
         * capabilities of the slave that triggered the current BGSAVE. */
        // 对于这个从节点，我们检查它是否具有触发当前BGSAVE操作的能力
        if (ln && ((c->slave_capa & slave->slave_capa) == slave->slave_capa)) {
            /* Perfect, the server is already registering differences for
             * another slave. Set the right state, and copy the buffer. */
            // 将slave的输出缓冲区所有内容拷贝给c的所有输出缓冲区中
            copyClientOutputBuffer(c,slave);
            // 设置全量重同步从节点的状态，设置部分重同步的偏移量
            replicationSetupSlaveForFullResync(c,slave->psync_initial_offset);
            serverLog(LL_NOTICE,"Waiting for end of BGSAVE for SYNC");
        } else {
            /* No way, we need to wait for the next BGSAVE in order to
             * register differences. */
            serverLog(LL_NOTICE,"Can't attach the replica to the current BGSAVE. Waiting for next BGSAVE for SYNC");
        }

    /* CASE 2: BGSAVE is in progress, with socket target. */
        // 情况2. 正在执行BGSAVE，且是无盘同步，直接写到socket中
    } else if (server.rdb_child_pid != -1 &&
               server.rdb_child_type == RDB_CHILD_TYPE_SOCKET)
    {
        /* There is an RDB child process but it is writing directly to
         * children sockets. We need to wait for the next BGSAVE
         * in order to synchronize. */
        // 虽然有子进程在执行写RDB，但是它直接写到socket中，所以等待下次执行BGSAVE
        serverLog(LL_NOTICE,"Current BGSAVE has socket target. Waiting for next BGSAVE for SYNC");

    /* CASE 3: There is no BGSAVE is progress. */
    // 情况3：没有执行BGSAVE的进程
    } else {
        // 服务器支持无盘同步
        if (server.repl_diskless_sync && (c->slave_capa & SLAVE_CAPA_EOF)) {
            /* Diskless replication RDB child is created inside
             * replicationCron() since we want to delay its start a
             * few seconds to wait for more slaves to arrive. */
            // 无盘同步复制的子进程被创建在replicationCron()中，因为想等待更多的从节点可以到来而延迟
            if (server.repl_diskless_sync_delay)
                serverLog(LL_NOTICE,"Delay next BGSAVE for diskless SYNC");
            // 服务器不支持无盘复制
        } else {
            /* Target is disk (or the slave is not capable of supporting
             * diskless replication) and we don't have a BGSAVE in progress,
             * let's start one. */
            // 如果没有正在执行BGSAVE，且没有进行写AOF文件，则开始为复制执行BGSAVE，并且是将RDB文件写到磁盘上
            if (!hasActiveChildProcess()) {
                startBgsaveForReplication(c->slave_capa);
            } else {
                serverLog(LL_NOTICE,
                    "No BGSAVE in progress, but another BG operation is active. "
                    "BGSAVE for replication delayed");
            }
        }
    }
    return;
}

/* REPLCONF <option> <value> <option> <value> ...
 * This command is used by a slave in order to configure the replication
 * process before starting it with the SYNC command.
 *
 * Currently the only use of this command is to communicate to the master
 * what is the listening port of the Slave redis instance, so that the
 * master can accurately list slaves and their listening ports in
 * the INFO output.
 *
 * In the future the same command can be used in order to configure
 * the replication to initiate an incremental replication instead of a
 * full resync. */
// REPLCONF <option> <value> <option> <value> ... 命令实现
// 被从节点使用来去配置复制进程，在SYNC之前
// 唯一使用的是用来让从节点告知主机点它的监听端口，以便主节点可以通过INFO命令来输出
void replconfCommand(client *c) {
    int j;

    // 检查参数个数是否正确，每一个<option>都要对应<value>
    if ((c->argc % 2) == 0) {
        /* Number of arguments must be odd to make sure that every
         * option has a corresponding value. */
        addReply(c,shared.syntaxerr);
        return;
    }

    /* Process every option-value pair. */
    // 处理每一个对<option> <value>
    for (j = 1; j < c->argc; j+=2) {
        // REPLCONF listening-port <port> 命令
        if (!strcasecmp(c->argv[j]->ptr,"listening-port")) {
            long port;

            // 获取端口号
            if ((getLongFromObjectOrReply(c,c->argv[j+1],
                    &port,NULL) != C_OK))
                return;
            // 设置从节点监听的端口号
            c->slave_listening_port = port;
            // REPLCONF ip-address ip
        } else if (!strcasecmp(c->argv[j]->ptr,"ip-address")) {
            sds ip = c->argv[j+1]->ptr;
            // 设置从节点的ip
            if (sdslen(ip) < sizeof(c->slave_ip)) {
                memcpy(c->slave_ip,ip,sdslen(ip)+1);
            } else {
                addReplyErrorFormat(c,"REPLCONF ip-address provided by "
                    "replica instance is too long: %zd bytes", sdslen(ip));
                return;
            }
            // REPLCONF capa eof
        } else if (!strcasecmp(c->argv[j]->ptr,"capa")) {
            /* Ignore capabilities not understood by this master. */
            // 设置client的能力值capa，忽略其他的capabilities
            if (!strcasecmp(c->argv[j+1]->ptr,"eof"))
                c->slave_capa |= SLAVE_CAPA_EOF;
            else if (!strcasecmp(c->argv[j+1]->ptr,"psync2"))
                c->slave_capa |= SLAVE_CAPA_PSYNC2;
            // REPLCONF ack <offset>
        } else if (!strcasecmp(c->argv[j]->ptr,"ack")) {
            /* REPLCONF ACK is used by slave to inform the master the amount
             * of replication stream that it processed so far. It is an
             * internal only command that normal clients should never use. */
            // 从节点使用REPLCONF ACK通知主机到目前为止处理的复制偏移量。 这是一个内部唯一的命令，普通客户端不应该使用它
            long long offset;

            // client不是从节点，直接返回
            if (!(c->flags & CLIENT_SLAVE)) return;
            // 获取offset
            if ((getLongLongFromObject(c->argv[j+1], &offset) != C_OK))
                return;
            // 设置从节点通过ack命令接收到的偏移量
            if (offset > c->repl_ack_off)
                c->repl_ack_off = offset;
            // 通过ack命令接收到的偏移量所用的时间
            c->repl_ack_time = server.unixtime;
            /* If this was a diskless replication, we need to really put
             * the slave online when the first ACK is received (which
             * confirms slave is online and ready to get more data). This
             * allows for simpler and less CPU intensive EOF detection
             * when streaming RDB files. */
            // 如果这是一个无盘复制，我们需要在接收到第一个ACK时确实将从节点设置为在线状态
            // （这确认从节点在线并准备好获取更多的数据）
            // 将从节点设置为在线状态
            if (c->repl_put_online_on_ack && c->replstate == SLAVE_STATE_ONLINE)
                putSlaveOnline(c);
            /* Note: this command does not reply anything! */
            return;
            // REPLCONF getack
        } else if (!strcasecmp(c->argv[j]->ptr,"getack")) {
            /* REPLCONF GETACK is used in order to request an ACK ASAP
             * to the slave. */
            // REPLCONF GETACK 被用来取请求一个ACK给从节点
            if (server.masterhost && server.master) replicationSendAck();
            return;
        } else {
            addReplyErrorFormat(c,"Unrecognized REPLCONF option: %s",
                (char*)c->argv[j]->ptr);
            return;
        }
    }
    addReply(c,shared.ok);
}

/* This function puts a replica in the online state, and should be called just
 * after a replica received the RDB file for the initial synchronization, and
 * we are finally ready to send the incremental stream of commands.
 *
 * It does a few things:
 *
 * 1) Put the slave in ONLINE state. Note that the function may also be called
 *    for a replicas that are already in ONLINE state, but having the flag
 *    repl_put_online_on_ack set to true: we still have to install the write
 *    handler in that case. This function will take care of that.
 * 2) Make sure the writable event is re-installed, since calling the SYNC
 *    command disables it, so that we can accumulate output buffer without
 *    sending it to the replica.
 * 3) Update the count of "good replicas". */
// 该函数将从节点置于在线状态，并且应该在从节点接收到初始同步的RDB文件之后调用，并且我们终于准备好发送增量命令流
/*
    1. 将从节点设置为ONLINE状态，除非当函数被调用时，由于状态已经是ONLINE而是repl_put_online_on_ack为真
    2. 确保可写事件已经被重新设置，因为调用SYNC命令不能使用它，所以我们可以累加输出缓冲区而不将其发送到从节点
    3. 更新当前状态良好的从节点的个数
*/
void putSlaveOnline(client *slave) {
    // 设置从节点的状态为ONLINE
    slave->replstate = SLAVE_STATE_ONLINE;
    // 不设置从节点的写处理器
    slave->repl_put_online_on_ack = 0;
    // 设置通过ack命令接收到的偏移量所用的时间
    slave->repl_ack_time = server.unixtime; /* Prevent false timeout. */
    // 重新设置文件的可写事件的处理程序为sendReplyToClient
    if (connSetWriteHandler(slave->conn, sendReplyToClient) == C_ERR) {
        serverLog(LL_WARNING,"Unable to register writable event for replica bulk transfer: %s", strerror(errno));
        freeClient(slave);
        return;
    }
    // 更新当前状态良好的从节点的个数
    refreshGoodSlavesCount();
    /* Fire the replica change modules event. */
    moduleFireServerEvent(REDISMODULE_EVENT_REPLICA_CHANGE,
                          REDISMODULE_SUBEVENT_REPLICA_CHANGE_ONLINE,
                          NULL);
    serverLog(LL_NOTICE,"Synchronization with replica %s succeeded",
        replicationGetSlaveName(slave));
}

/* We call this function periodically to remove an RDB file that was
 * generated because of replication, in an instance that is otherwise
 * without any persistence. We don't want instances without persistence
 * to take RDB files around, this violates certain policies in certain
 * environments. */
void removeRDBUsedToSyncReplicas(void) {
    /* If the feature is disabled, return ASAP but also clear the
     * RDBGeneratedByReplication flag in case it was set. Otherwise if the
     * feature was enabled, but gets disabled later with CONFIG SET, the
     * flag may remain set to one: then next time the feature is re-enabled
     * via CONFIG SET we have have it set even if no RDB was generated
     * because of replication recently. */
    if (!server.rdb_del_sync_files) {
        RDBGeneratedByReplication = 0;
        return;
    }

    if (allPersistenceDisabled() && RDBGeneratedByReplication) {
        client *slave;
        listNode *ln;
        listIter li;

        int delrdb = 1;
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            slave = ln->value;
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START ||
                slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END ||
                slave->replstate == SLAVE_STATE_SEND_BULK)
            {
                delrdb = 0;
                break; /* No need to check the other replicas. */
            }
        }
        if (delrdb) {
            struct stat sb;
            if (lstat(server.rdb_filename,&sb) != -1) {
                RDBGeneratedByReplication = 0;
                serverLog(LL_NOTICE,
                    "Removing the RDB file used to feed replicas "
                    "in a persistence-less instance");
                bg_unlink(server.rdb_filename);
            }
        }
    }
}

// 发送多条回复给从节点
void sendBulkToSlave(connection *conn) {
    client *slave = connGetPrivateData(conn);
    char buf[PROTO_IOBUF_LEN];
    ssize_t nwritten, buflen;

    /* Before sending the RDB file, we send the preamble as configured by the
     * replication process. Currently the preamble is just the bulk count of
     * the file in the form "$<length>\r\n". */
    // 发送"$<length>\r\n"表示即将发送RDB文件的大小
    if (slave->replpreamble) {
        nwritten = connWrite(conn,slave->replpreamble,sdslen(slave->replpreamble));
        if (nwritten == -1) {
            serverLog(LL_VERBOSE,
                "Write error sending RDB preamble to replica: %s",
                connGetLastError(conn));
            freeClient(slave);
            return;
        }
        // 更新已经写到网络的字节数
        server.stat_net_output_bytes += nwritten;
        // 保留未写的字节，删除已写的字节
        sdsrange(slave->replpreamble,nwritten,-1);
        // 如果已经写完了，则释放replpreamble
        if (sdslen(slave->replpreamble) == 0) {
            sdsfree(slave->replpreamble);
            slave->replpreamble = NULL;
            /* fall through sending data. */
        } else {
            return;
        }
    }

    /* If the preamble was already transferred, send the RDB bulk data. */
    // 将文件指针移动到刚才发送replpreamble的下一个字节，准备写回复
    lseek(slave->repldbfd,slave->repldboff,SEEK_SET);
    // 将repldbfd读出RDB文件中的内容保存在buf中
    buflen = read(slave->repldbfd,buf,PROTO_IOBUF_LEN);
    if (buflen <= 0) {
        serverLog(LL_WARNING,"Read error sending DB to replica: %s",
            (buflen == 0) ? "premature EOF" : strerror(errno));
        freeClient(slave);
        return;
    }
    // 将保存RDB文件数据的buf写到从节点中
    if ((nwritten = connWrite(conn,buf,buflen)) == -1) {
        if (connGetState(conn) != CONN_STATE_CONNECTED) {
            serverLog(LL_WARNING,"Write error sending DB to replica: %s",
                connGetLastError(conn));
            freeClient(slave);
        }
        return;
    }
    // 更新从节点读取主服务器传来的RDB文件的字节数
    slave->repldboff += nwritten;
    // 更新服务器已经写到网络的字节数
    server.stat_net_output_bytes += nwritten;
    // 如果写入完成。从网络读到的大小等于文件大小
    if (slave->repldboff == slave->repldbsize) {
        // 关闭RDB文件描述符
        close(slave->repldbfd);
        slave->repldbfd = -1;
        // 删除等待从节点的文件可读事件
        connSetWriteHandler(slave->conn,NULL);
        // 将从节点置于在线状态
        putSlaveOnline(slave);
    }
}

/* Remove one write handler from the list of connections waiting to be writable
 * during rdb pipe transfer. */
void rdbPipeWriteHandlerConnRemoved(struct connection *conn) {
    if (!connHasWriteHandler(conn))
        return;
    connSetWriteHandler(conn, NULL);
    server.rdb_pipe_numconns_writing--;
    /* if there are no more writes for now for this conn, or write error: */
    if (server.rdb_pipe_numconns_writing == 0) {
        if (aeCreateFileEvent(server.el, server.rdb_pipe_read, AE_READABLE, rdbPipeReadHandler,NULL) == AE_ERR) {
            serverPanic("Unrecoverable error creating server.rdb_pipe_read file event.");
        }
    }
}

/* Called in diskless master during transfer of data from the rdb pipe, when
 * the replica becomes writable again. */
void rdbPipeWriteHandler(struct connection *conn) {
    serverAssert(server.rdb_pipe_bufflen>0);
    client *slave = connGetPrivateData(conn);
    int nwritten;
    if ((nwritten = connWrite(conn, server.rdb_pipe_buff + slave->repldboff,
                              server.rdb_pipe_bufflen - slave->repldboff)) == -1)
    {
        if (connGetState(conn) == CONN_STATE_CONNECTED)
            return; /* equivalent to EAGAIN */
        serverLog(LL_WARNING,"Write error sending DB to replica: %s",
            connGetLastError(conn));
        freeClient(slave);
        return;
    } else {
        slave->repldboff += nwritten;
        server.stat_net_output_bytes += nwritten;
        if (slave->repldboff < server.rdb_pipe_bufflen)
            return; /* more data to write.. */
    }
    rdbPipeWriteHandlerConnRemoved(conn);
}

/* When the the pipe serving diskless rdb transfer is drained (write end was
 * closed), we can clean up all the temporary variables, and cleanup after the
 * fork child. */
void RdbPipeCleanup() {
    close(server.rdb_pipe_read);
    zfree(server.rdb_pipe_conns);
    server.rdb_pipe_conns = NULL;
    server.rdb_pipe_numconns = 0;
    server.rdb_pipe_numconns_writing = 0;
    zfree(server.rdb_pipe_buff);
    server.rdb_pipe_buff = NULL;
    server.rdb_pipe_bufflen = 0;

    /* Since we're avoiding to detect the child exited as long as the pipe is
     * not drained, so now is the time to check. */
    checkChildrenDone();
}

/* Called in diskless master, when there's data to read from the child's rdb pipe */
void rdbPipeReadHandler(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask) {
    UNUSED(mask);
    UNUSED(clientData);
    UNUSED(eventLoop);
    int i;
    if (!server.rdb_pipe_buff)
        server.rdb_pipe_buff = zmalloc(PROTO_IOBUF_LEN);
    serverAssert(server.rdb_pipe_numconns_writing==0);

    while (1) {
        server.rdb_pipe_bufflen = read(fd, server.rdb_pipe_buff, PROTO_IOBUF_LEN);
        if (server.rdb_pipe_bufflen < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            serverLog(LL_WARNING,"Diskless rdb transfer, read error sending DB to replicas: %s", strerror(errno));
            for (i=0; i < server.rdb_pipe_numconns; i++) {
                connection *conn = server.rdb_pipe_conns[i];
                if (!conn)
                    continue;
                client *slave = connGetPrivateData(conn);
                freeClient(slave);
                server.rdb_pipe_conns[i] = NULL;
            }
            killRDBChild();
            return;
        }

        if (server.rdb_pipe_bufflen == 0) {
            /* EOF - write end was closed. */
            int stillUp = 0;
            aeDeleteFileEvent(server.el, server.rdb_pipe_read, AE_READABLE);
            for (i=0; i < server.rdb_pipe_numconns; i++)
            {
                connection *conn = server.rdb_pipe_conns[i];
                if (!conn)
                    continue;
                stillUp++;
            }
            serverLog(LL_WARNING,"Diskless rdb transfer, done reading from pipe, %d replicas still up.", stillUp);
            RdbPipeCleanup();
            return;
        }

        int stillAlive = 0;
        for (i=0; i < server.rdb_pipe_numconns; i++)
        {
            int nwritten;
            connection *conn = server.rdb_pipe_conns[i];
            if (!conn)
                continue;

            client *slave = connGetPrivateData(conn);
            if ((nwritten = connWrite(conn, server.rdb_pipe_buff, server.rdb_pipe_bufflen)) == -1) {
                if (connGetState(conn) != CONN_STATE_CONNECTED) {
                    serverLog(LL_WARNING,"Diskless rdb transfer, write error sending DB to replica: %s",
                        connGetLastError(conn));
                    freeClient(slave);
                    server.rdb_pipe_conns[i] = NULL;
                    continue;
                }
                /* An error and still in connected state, is equivalent to EAGAIN */
                slave->repldboff = 0;
            } else {
                slave->repldboff = nwritten;
                server.stat_net_output_bytes += nwritten;
            }
            /* If we were unable to write all the data to one of the replicas,
             * setup write handler (and disable pipe read handler, below) */
            if (nwritten != server.rdb_pipe_bufflen) {
                server.rdb_pipe_numconns_writing++;
                connSetWriteHandler(conn, rdbPipeWriteHandler);
            }
            stillAlive++;
        }

        if (stillAlive == 0) {
            serverLog(LL_WARNING,"Diskless rdb transfer, last replica dropped, killing fork child.");
            killRDBChild();
            RdbPipeCleanup();
        }
        /*  Remove the pipe read handler if at least one write handler was set. */
        if (server.rdb_pipe_numconns_writing || stillAlive == 0) {
            aeDeleteFileEvent(server.el, server.rdb_pipe_read, AE_READABLE);
            break;
        }
    }
}

/* This function is called at the end of every background saving,
 * or when the replication RDB transfer strategy is modified from
 * disk to socket or the other way around.
 *
 * The goal of this function is to handle slaves waiting for a successful
 * background saving in order to perform non-blocking synchronization, and
 * to schedule a new BGSAVE if there are slaves that attached while a
 * BGSAVE was in progress, but it was not a good one for replication (no
 * other slave was accumulating differences).
 *
 * The argument bgsaveerr is C_OK if the background saving succeeded
 * otherwise C_ERR is passed to the function.
 * The 'type' argument is the type of the child that terminated
 * (if it had a disk or socket target). */
// 这个函数每次执行完BGSAVE后被调用，或者当RDB传输的策略从有盘传输改为无盘传输或者其他方式
// 此函数的目标是处理等待成功执行BGSAVE的从节点以执行非阻塞同步，并且如果在BGSAVE正在进行时连接上了新的从节点，则安排新的BGSAVE，但这不是一个好的复制情况（没有其他从节点积累的差异数据）
void updateSlavesWaitingBgsave(int bgsaveerr, int type) {
    listNode *ln;
    int startbgsave = 0;
    int mincapa = -1;
    listIter li;

    listRewind(server.slaves,&li);
    // 遍历所有的从节点
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        // 如果当前从节点的复制状态为，从服务器节点等待BGSAVE节点的开始，因此要生成一个新的RDB文件
        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
            // 设置开始BGSAVE的标志
            startbgsave = 1;
            mincapa = (mincapa == -1) ? slave->slave_capa :
                                        (mincapa & slave->slave_capa);
            // 如果当前从节点的复制状态为，已经创建子进程执行写RDB操作，等待完成
        } else if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) {
            struct redis_stat buf;

            if (bgsaveerr != C_OK) {
                freeClient(slave);
                serverLog(LL_WARNING,"SYNC failed. BGSAVE child returned an error");
                continue;
            }

            /* If this was an RDB on disk save, we have to prepare to send
             * the RDB from disk to the slave socket. Otherwise if this was
             * already an RDB -> Slaves socket transfer, used in the case of
             * diskless replication, our work is trivial, we can just put
             * the slave online. */
            // 如果这是将RDB文件写到磁盘上，那么我们必须准备将RDB文件从磁盘发送到从节点的socket中
            // 否则如果已经是RDB直接写到从节点的socket上，即无盘同步，那么我们所做的很少，只能将从节点设置为online状态
            // 如果是将 RDB 被写到从节点的套接字中，无盘复制
            if (type == RDB_CHILD_TYPE_SOCKET) {
                serverLog(LL_NOTICE,
                    "Streamed RDB transfer with replica %s succeeded (socket). Waiting for REPLCONF ACK from slave to enable streaming",
                        replicationGetSlaveName(slave));
                /* Note: we wait for a REPLCONF ACK message from the replica in
                 * order to really put it online (install the write handler
                 * so that the accumulated data can be transferred). However
                 * we change the replication state ASAP, since our slave
                 * is technically online now.
                 *
                 * So things work like that:
                 *
                 * 1. We end trasnferring the RDB file via socket.
                 * 2. The replica is put ONLINE but the write handler
                 *    is not installed.
                 * 3. The replica however goes really online, and pings us
                 *    back via REPLCONF ACK commands.
                 * 4. Now we finally install the write handler, and send
                 *    the buffers accumulated so far to the replica.
                 *
                 * But why we do that? Because the replica, when we stream
                 * the RDB directly via the socket, must detect the RDB
                 * EOF (end of file), that is a special random string at the
                 * end of the RDB (for streamed RDBs we don't know the length
                 * in advance). Detecting such final EOF string is much
                 * simpler and less CPU intensive if no more data is sent
                 * after such final EOF. So we don't want to glue the end of
                 * the RDB trasfer with the start of the other replication
                 * data. */
                // 将从节点状态设置为online
                slave->replstate = SLAVE_STATE_ONLINE;
                // 设置从节点的写处理器的标志
                slave->repl_put_online_on_ack = 1;
                // 设置相应时间
                slave->repl_ack_time = server.unixtime; /* Timeout otherwise. */
                // 如果是将 RDB 被写到磁盘上
            } else {
                // 打开RDB文件，设置复制的fd
                if ((slave->repldbfd = open(server.rdb_filename,O_RDONLY)) == -1 ||
                    redis_fstat(slave->repldbfd,&buf) == -1) {
                    freeClient(slave);
                    serverLog(LL_WARNING,"SYNC failed. Can't open/stat DB after BGSAVE: %s", strerror(errno));
                    continue;
                }
                // 设置主服务器传来的RDB文件复制偏移量
                slave->repldboff = 0;
                // 设置RDB文件大小
                slave->repldbsize = buf.st_size;
                // 更新状态，正在发送RDB文件给从节点
                slave->replstate = SLAVE_STATE_SEND_BULK;
                // 设置主服务器传来的RDB文件的大小，符合协议的字符串形式
                slave->replpreamble = sdscatprintf(sdsempty(),"$%lld\r\n",
                    (unsigned long long) slave->repldbsize);

                // 清除之前的可写的处理程序
                connSetWriteHandler(slave->conn,NULL);
                // 设置sendBulkToSlave为新的处理写操作的程序，sendBulkToSlave会将RDB文件写给slave
                if (connSetWriteHandler(slave->conn,sendBulkToSlave) == C_ERR) {
                    freeClient(slave);
                    continue;
                }
            }
        }
    }
    // 开始为复制执行BGSAVE，根据配置选择磁盘或套接字作为RDB发送的目标，在开始之前确保冲洗脚本缓存
    if (startbgsave) startBgsaveForReplication(mincapa);
}

/* Change the current instance replication ID with a new, random one.
 * This will prevent successful PSYNCs between this master and other
 * slaves, so the command should be called when something happens that
 * alters the current story of the dataset. */
void changeReplicationId(void) {
    getRandomHexChars(server.replid,CONFIG_RUN_ID_SIZE);
    server.replid[CONFIG_RUN_ID_SIZE] = '\0';
}

/* Clear (invalidate) the secondary replication ID. This happens, for
 * example, after a full resynchronization, when we start a new replication
 * history. */
void clearReplicationId2(void) {
    memset(server.replid2,'0',sizeof(server.replid));
    server.replid2[CONFIG_RUN_ID_SIZE] = '\0';
    server.second_replid_offset = -1;
}

/* Use the current replication ID / offset as secondary replication
 * ID, and change the current one in order to start a new history.
 * This should be used when an instance is switched from slave to master
 * so that it can serve PSYNC requests performed using the master
 * replication ID. */
void shiftReplicationId(void) {
    memcpy(server.replid2,server.replid,sizeof(server.replid));
    /* We set the second replid offset to the master offset + 1, since
     * the slave will ask for the first byte it has not yet received, so
     * we need to add one to the offset: for example if, as a slave, we are
     * sure we have the same history as the master for 50 bytes, after we
     * are turned into a master, we can accept a PSYNC request with offset
     * 51, since the slave asking has the same history up to the 50th
     * byte, and is asking for the new bytes starting at offset 51. */
    server.second_replid_offset = server.master_repl_offset+1;
    changeReplicationId();
    serverLog(LL_WARNING,"Setting secondary replication ID to %s, valid up to offset: %lld. New replication ID is %s", server.replid2, server.second_replid_offset, server.replid);
}

/* ----------------------------------- SLAVE -------------------------------- */

/* Returns 1 if the given replication state is a handshake state,
 * 0 otherwise. */
// 如果给定的复制状态是握手状态，则返回1，否则返回0
int slaveIsInHandshakeState(void) {
    return server.repl_state >= REPL_STATE_RECEIVE_PONG &&
           server.repl_state <= REPL_STATE_RECEIVE_PSYNC;
}

/* Avoid the master to detect the slave is timing out while loading the
 * RDB file in initial synchronization. We send a single newline character
 * that is valid protocol but is guaranteed to either be sent entirely or
 * not, since the byte is indivisible.
 *
 * The function is called in two contexts: while we flush the current
 * data with emptyDb(), and while we load the new data received as an
 * RDB file from the master. */
// 在初始同步加载RDB文件时，避免主节点检测到从节点处于超时状态。 我们发送一个有效协议的单个换行字符，但是保证要么完全被发送或完全不被不发送，因为该字节是不可分割的
void replicationSendNewlineToMaster(void) {
    static time_t newline_sent;
    // 设置写新行的时间
    if (time(NULL) != newline_sent) {
        newline_sent = time(NULL);
        /* Pinging back in this stage is best-effort. */
        // 写一个换行
        if (server.repl_transfer_s) connWrite(server.repl_transfer_s, "\n", 1);
    }
}

/* Callback used by emptyDb() while flushing away old data to load
 * the new dataset received by the master. */
// 在清空旧数据以加载被主节点数据接收的新数据集时，由emptyDb()使用回调
void replicationEmptyDbCallback(void *privdata) {
    UNUSED(privdata);
    if (server.repl_state == REPL_STATE_TRANSFER)
        replicationSendNewlineToMaster();
}

/* Once we have a link with the master and the synchronization was
 * performed, this function materializes the master client we store
 * at server.master, starting from the specified file descriptor. */
// 一旦我们与主节点建立了链接，并且执行了同步，这个函数实现了我们在server.master中存储的主节点客户端，从指定的文件描述符开始
void replicationCreateMasterClient(connection *conn, int dbid) {
    // 创建一个client
    server.master = createClient(conn);
    if (conn)
        connSetReadHandler(server.master->conn, readQueryFromClient);
    // 设置为主节点client的状态
    server.master->flags |= CLIENT_MASTER;
    // client认证通过
    server.master->authenticated = 1;
    // 主节点PSYNC的偏移量拷贝给client保存的复制偏移量
    server.master->reploff = server.master_initial_offset;
    server.master->read_reploff = server.master->reploff;
    server.master->user = NULL; /* This client can do everything. */
    // 拷贝主节点的运行ID给client
    memcpy(server.master->replid, server.master_replid,
        sizeof(server.master_replid));
    /* If master offset is set to -1, this master is old and is not
     * PSYNC capable, so we flag it accordingly. */
    // 如果主节点的偏移量是-1，那么将client设置为CLIENT_PRE_PSYNC，适用于旧的Redis版本，执行执行SYNC命令
    if (server.master->reploff == -1)
        server.master->flags |= CLIENT_PRE_PSYNC;
    if (dbid != -1) selectDb(server.master,dbid);
}

/* This function will try to re-enable the AOF file after the
 * master-replica synchronization: if it fails after multiple attempts
 * the replica cannot be considered reliable and exists with an
 * error. */
void restartAOFAfterSYNC() {
    unsigned int tries, max_tries = 10;
    for (tries = 0; tries < max_tries; ++tries) {
        if (startAppendOnly() == C_OK) break;
        serverLog(LL_WARNING,
            "Failed enabling the AOF after successful master synchronization! "
            "Trying it again in one second.");
        sleep(1);
    }
    if (tries == max_tries) {
        serverLog(LL_WARNING,
            "FATAL: this replica instance finished the synchronization with "
            "its master, but the AOF can't be turned on. Exiting now.");
        exit(1);
    }
}

static int useDisklessLoad() {
    /* compute boolean decision to use diskless load */
    int enabled = server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB ||
           (server.repl_diskless_load == REPL_DISKLESS_LOAD_WHEN_DB_EMPTY && dbTotalServerKeyCount()==0);
    /* Check all modules handle read errors, otherwise it's not safe to use diskless load. */
    if (enabled && !moduleAllDatatypesHandleErrors()) {
        serverLog(LL_WARNING,
            "Skipping diskless-load because there are modules that don't handle read errors.");
        enabled = 0;
    }
    return enabled;
}

/* Helper function for readSyncBulkPayload() to make backups of the current
 * DBs before socket-loading the new ones. The backups may be restored later
 * or freed by disklessLoadRestoreBackups(). */
redisDb *disklessLoadMakeBackups(void) {
    redisDb *backups = zmalloc(sizeof(redisDb)*server.dbnum);
    for (int i=0; i<server.dbnum; i++) {
        backups[i] = server.db[i];
        server.db[i].dict = dictCreate(&dbDictType,NULL);
        server.db[i].expires = dictCreate(&keyptrDictType,NULL);
    }
    return backups;
}

/* Helper function for readSyncBulkPayload(): when replica-side diskless
 * database loading is used, Redis makes a backup of the existing databases
 * before loading the new ones from the socket.
 *
 * If the socket loading went wrong, we want to restore the old backups
 * into the server databases. This function does just that in the case
 * the 'restore' argument (the number of DBs to replace) is non-zero.
 *
 * When instead the loading succeeded we want just to free our old backups,
 * in that case the function will do just that when 'restore' is 0. */
void disklessLoadRestoreBackups(redisDb *backup, int restore, int empty_db_flags)
{
    if (restore) {
        /* Restore. */
        emptyDbGeneric(server.db,-1,empty_db_flags,replicationEmptyDbCallback);
        for (int i=0; i<server.dbnum; i++) {
            dictRelease(server.db[i].dict);
            dictRelease(server.db[i].expires);
            server.db[i] = backup[i];
        }
    } else {
        /* Delete (Pass EMPTYDB_BACKUP in order to avoid firing module events) . */
        emptyDbGeneric(backup,-1,empty_db_flags|EMPTYDB_BACKUP,replicationEmptyDbCallback);
        for (int i=0; i<server.dbnum; i++) {
            dictRelease(backup[i].dict);
            dictRelease(backup[i].expires);
        }
    }
    zfree(backup);
}

/* Asynchronously read the SYNC payload we receive from a master */
// 异步读取从主节点接收到SYNC
#define REPL_MAX_WRITTEN_BEFORE_FSYNC (1024*1024*8) /* 8 MB */
void readSyncBulkPayload(connection *conn) {
    char buf[PROTO_IOBUF_LEN];
    ssize_t nread, readlen, nwritten;
    int use_diskless_load = useDisklessLoad();
    redisDb *diskless_load_backup = NULL;
    int empty_db_flags = server.repl_slave_lazy_flush ? EMPTYDB_ASYNC :
                                                        EMPTYDB_NO_FLAGS;
    off_t left;

    /* Static vars used to hold the EOF mark, and the last bytes received
     * from the server: when they match, we reached the end of the transfer. */
    // 用于保存EOF标记的静态变量和从节点服务器接收的最后40个字节：当它们匹配时，表示传输结束
    static char eofmark[CONFIG_RUN_ID_SIZE];
    static char lastbytes[CONFIG_RUN_ID_SIZE];
    static int usemark = 0;

    /* If repl_transfer_size == -1 we still have to read the bulk length
     * from the master reply. */
    // 即使repl_transfer_size==-1，我们仍然从主节点读取一个长度的回复，该长度是RDB文件的大小
    if (server.repl_transfer_size == -1) {
        // 从fd中同步读取数据到buf中，读取1024个字节
        if (connSyncReadLine(conn,buf,1024,server.repl_syncio_timeout*1000) == -1) {
            serverLog(LL_WARNING,
                "I/O error reading bulk count from MASTER: %s",
                strerror(errno));
            goto error;
        }

        // 读出的数据出错
        if (buf[0] == '-') {
            serverLog(LL_WARNING,
                "MASTER aborted replication with an error: %s",
                buf+1);
            goto error;
            // 读出了一个'\0'
        } else if (buf[0] == '\0') {
            /* At this stage just a newline works as a PING in order to take
             * the connection live. So we refresh our last interaction
             * timestamp. */
            // 只是读出了一个作用和PING相同的字符，是为了测试连接是否中断
            // 所以更新最近交互的时间戳，直接返回
            server.repl_transfer_lastio = server.unixtime;
            return;
            // 读出的长度按照协议格式是：$<length>\r\n，所以第一个字符不是'$'就出错
        } else if (buf[0] != '$') {
            serverLog(LL_WARNING,"Bad protocol from MASTER, the first byte is not '$' (we received '%s'), are you sure the host and port are right?", buf);
            goto error;
        }

        /* There are two possible forms for the bulk payload. One is the
         * usual $<count> bulk format. The other is used for diskless transfers
         * when the master does not know beforehand the size of the file to
         * transfer. In the latter case, the following format is used:
         *
         * $EOF:<40 bytes delimiter>
         *
         * At the end of the file the announced delimiter is transmitted. The
         * delimiter is long and random enough that the probability of a
         * collision with the actual file content can be ignored. */
        // 这有两种可能的情况。一种是通常的 $<count> 格式。
        // 另一种是用于主节点事先不知道RDB文件的大小的无盘传输，格式是 $EOF:<40 bytes delimiter>
        // 在文件结尾处，传送了分界符。 分界符长度随机，可以忽略与实际文件内容的冲突概率。
        // 如果读出了无盘传输的流格式"EOF:"
        if (strncmp(buf+1,"EOF:",4) == 0 && strlen(buf+5) >= CONFIG_RUN_ID_SIZE) {
            usemark = 1;
            // 将40字节长的分隔符保存到eofmark静态数组中
            memcpy(eofmark,buf+5,CONFIG_RUN_ID_SIZE);
            // 将保存从服务器接收的最后40个字节的数组初始化为0
            memset(lastbytes,0,CONFIG_RUN_ID_SIZE);
            /* Set any repl_transfer_size to avoid entering this code path
             * at the next call. */
            // 同步期间从主节点读到的RDB的大小设置为0，因为是无盘传输的方式
            server.repl_transfer_size = 0;
            serverLog(LL_NOTICE,
                "MASTER <-> REPLICA sync: receiving streamed RDB from master with EOF %s",
                use_diskless_load? "to parser":"to disk");
            // 从RDB文件读，读出RDB文件长度
        } else {
            usemark = 0;
            // 从主节点读到的RDB的大小
            server.repl_transfer_size = strtol(buf+1,NULL,10);
            serverLog(LL_NOTICE,
                "MASTER <-> REPLICA sync: receiving %lld bytes from master %s",
                (long long) server.repl_transfer_size,
                use_diskless_load? "to parser":"to disk");
        }
        return;
    }


    if (!use_diskless_load) {
        /* Read the data from the socket, store it to a file and search
         * for the EOF. */
        // usemark在无盘传输读取长度是被设置为1
        if (usemark) {
            // 计算数据大小，也就是读的长度
            readlen = sizeof(buf);
            // 从RDB文件读
        } else {
            // 计算读的长度
            left = server.repl_transfer_size - server.repl_transfer_read;
            readlen = (left < (signed)sizeof(buf)) ? left : (signed)sizeof(buf);
        }

        // 从RDB文件中读到buf
        nread = connRead(conn,buf,readlen);
        if (nread <= 0) {
            if (connGetState(conn) == CONN_STATE_CONNECTED) {
                /* equivalent to EAGAIN */
                return;
            }
            serverLog(LL_WARNING,"I/O error trying to sync with MASTER: %s",
                (nread == -1) ? strerror(errno) : "connection lost");
            cancelReplicationHandshake();
            return;
        }
        // 更新从网络读的字节数
        server.stat_net_input_bytes += nread;

        /* When a mark is used, we want to detect EOF asap in order to avoid
         * writing the EOF mark into the file... */
        // EOF标志是否到达
        int eof_reached = 0;

        if (usemark) {
            /* Update the last bytes array, and check if it matches our
             * delimiter. */
            // 将所读的nread长度的后40字节拷贝到lastbytes数组中
            if (nread >= CONFIG_RUN_ID_SIZE) {
                memcpy(lastbytes,buf+nread-CONFIG_RUN_ID_SIZE,
                       CONFIG_RUN_ID_SIZE);
                // 读到的不足40字节，那就补足40字节
            } else {
                int rem = CONFIG_RUN_ID_SIZE-nread;
                memmove(lastbytes,lastbytes+nread,rem);
                memcpy(lastbytes+rem,buf,nread);
            }
            // 比较是否匹配，如果相同表示传输结束，到达EOF结尾
            if (memcmp(lastbytes,eofmark,CONFIG_RUN_ID_SIZE) == 0)
                eof_reached = 1;
        }

        /* Update the last I/O time for the replication transfer (used in
         * order to detect timeouts during replication), and write what we
         * got from the socket to the dump file on disk. */
        // 更新最近一次读到RDB文件内容的时间
        server.repl_transfer_lastio = server.unixtime;
        // 将buf中的数据写到临时RDB文件中，只是写到系统内核的缓冲区中，等待sync
        if ((nwritten = write(server.repl_transfer_fd,buf,nread)) != nread) {
            serverLog(LL_WARNING,
                "Write error or short write writing to the DB dump file "
                "needed for MASTER <-> REPLICA synchronization: %s",
                (nwritten == -1) ? strerror(errno) : "short write");
            goto error;
        }
        // 更新同步期间从主节点读到的RDB文件的总量
        server.repl_transfer_read += nread;

        /* Delete the last 40 bytes from the file if we reached EOF. */
        // 如果是无盘同步，且到达了EOF
        if (usemark && eof_reached) {
            // 删除后40字节
            if (ftruncate(server.repl_transfer_fd,
                server.repl_transfer_read - CONFIG_RUN_ID_SIZE) == -1)
            {
                serverLog(LL_WARNING,
                    "Error truncating the RDB file received from the master "
                    "for SYNC: %s", strerror(errno));
                goto error;
            }
        }

        /* Sync data on disk from time to time, otherwise at the end of the
         * transfer we may suffer a big delay as the memory buffers are copied
         * into the actual disk. */
        // 定期将文件写入磁盘，避免阻塞在IO上
        // Redis同步缓冲区到磁盘上，并不是每次写到同步，而是当写够8M大小，调用sync_file_range函数，一次性的冲刷数据，这样大大提高IO的性能
        if (server.repl_transfer_read >=
            server.repl_transfer_last_fsync_off + REPL_MAX_WRITTEN_BEFORE_FSYNC)
        {
            off_t sync_size = server.repl_transfer_read -
                              server.repl_transfer_last_fsync_off;
            rdb_fsync_range(server.repl_transfer_fd,
                server.repl_transfer_last_fsync_off, sync_size);
            // 更新最近一个执行fsync的偏移量
            server.repl_transfer_last_fsync_off += sync_size;
        }

        /* Check if the transfer is now complete */
        // 从RDB文件中读这种类型的同步，是否完成
        if (!usemark) {
            // 如果完成设置eof_reached
            if (server.repl_transfer_read == server.repl_transfer_size)
                eof_reached = 1;
        }

        /* If the transfer is yet not complete, we need to read more, so
         * return ASAP and wait for the handler to be called again. */
        if (!eof_reached) return;
    }

    /* We reach this point in one of the following cases:
     *
     * 1. The replica is using diskless replication, that is, it reads data
     *    directly from the socket to the Redis memory, without using
     *    a temporary RDB file on disk. In that case we just block and
     *    read everything from the socket.
     *
     * 2. Or when we are done reading from the socket to the RDB file, in
     *    such case we want just to read the RDB file in memory. */
    serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Flushing old data");

    /* We need to stop any AOF rewriting child before flusing and parsing
     * the RDB, otherwise we'll create a copy-on-write disaster. */
    if (server.aof_state != AOF_OFF) stopAppendOnly();

    /* When diskless RDB loading is used by replicas, it may be configured
     * in order to save the current DB instead of throwing it away,
     * so that we can restore it in case of failed transfer. */
    if (use_diskless_load &&
        server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB)
    {
        /* Create a backup of server.db[] and initialize to empty
         * dictionaries */
        diskless_load_backup = disklessLoadMakeBackups();
    }
    /* We call to emptyDb even in case of REPL_DISKLESS_LOAD_SWAPDB
     * (Where disklessLoadMakeBackups left server.db empty) because we
     * want to execute all the auxiliary logic of emptyDb (Namely,
     * fire module events) */
    emptyDb(-1,empty_db_flags,replicationEmptyDbCallback);

    /* Before loading the DB into memory we need to delete the readable
     * handler, otherwise it will get called recursively since
     * rdbLoad() will call the event loop to process events from time to
     * time for non blocking loading. */
    connSetReadHandler(conn, NULL);
    serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Loading DB in memory");
    rdbSaveInfo rsi = RDB_SAVE_INFO_INIT;
    if (use_diskless_load) {
        rio rdb;
        rioInitWithConn(&rdb,conn,server.repl_transfer_size);

        /* Put the socket in blocking mode to simplify RDB transfer.
         * We'll restore it when the RDB is received. */
        connBlock(conn);
        connRecvTimeout(conn, server.repl_timeout*1000);
        startLoading(server.repl_transfer_size, RDBFLAGS_REPLICATION);

        if (rdbLoadRio(&rdb,RDBFLAGS_REPLICATION,&rsi) != C_OK) {
            /* RDB loading failed. */
            stopLoading(0);
            serverLog(LL_WARNING,
                "Failed trying to load the MASTER synchronization DB "
                "from socket");
            cancelReplicationHandshake();
            rioFreeConn(&rdb, NULL);
            if (server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB) {
                /* Restore the backed up databases. */
                disklessLoadRestoreBackups(diskless_load_backup,1,
                                           empty_db_flags);
            } else {
                /* Remove the half-loaded data in case we started with
                 * an empty replica. */
                emptyDb(-1,empty_db_flags,replicationEmptyDbCallback);
            }

            /* Note that there's no point in restarting the AOF on SYNC
             * failure, it'll be restarted when sync succeeds or the replica
             * gets promoted. */
            return;
        }
        stopLoading(1);

        /* RDB loading succeeded if we reach this point. */
        if (server.repl_diskless_load == REPL_DISKLESS_LOAD_SWAPDB) {
            /* Delete the backup databases we created before starting to load
             * the new RDB. Now the RDB was loaded with success so the old
             * data is useless. */
            disklessLoadRestoreBackups(diskless_load_backup,0,empty_db_flags);
        }

        /* Verify the end mark is correct. */
        if (usemark) {
            if (!rioRead(&rdb,buf,CONFIG_RUN_ID_SIZE) ||
                memcmp(buf,eofmark,CONFIG_RUN_ID_SIZE) != 0)
            {
                serverLog(LL_WARNING,"Replication stream EOF marker is broken");
                cancelReplicationHandshake();
                rioFreeConn(&rdb, NULL);
                return;
            }
        }

        /* Cleanup and restore the socket to the original state to continue
         * with the normal replication. */
        rioFreeConn(&rdb, NULL);
        connNonBlock(conn);
        connRecvTimeout(conn,0);
    } else {
        /* Ensure background save doesn't overwrite synced data */
        if (server.rdb_child_pid != -1) {
            serverLog(LL_NOTICE,
                "Replica is about to load the RDB file received from the "
                "master, but there is a pending RDB child running. "
                "Killing process %ld and removing its temp file to avoid "
                "any race",
                    (long) server.rdb_child_pid);
            killRDBChild();
        }

        /* Make sure the new file (also used for persistence) is fully synced
         * (not covered by earlier calls to rdb_fsync_range). */
        if (fsync(server.repl_transfer_fd) == -1) {
            serverLog(LL_WARNING,
                "Failed trying to sync the temp DB to disk in "
                "MASTER <-> REPLICA synchronization: %s",
                strerror(errno));
            cancelReplicationHandshake();
            return;
        }

        /* Rename rdb like renaming rewrite aof asynchronously. */
        int old_rdb_fd = open(server.rdb_filename,O_RDONLY|O_NONBLOCK);
        if (rename(server.repl_transfer_tmpfile,server.rdb_filename) == -1) {
            serverLog(LL_WARNING,
                "Failed trying to rename the temp DB into %s in "
                "MASTER <-> REPLICA synchronization: %s",
                server.rdb_filename, strerror(errno));
            cancelReplicationHandshake();
            if (old_rdb_fd != -1) close(old_rdb_fd);
            return;
        }
        /* Close old rdb asynchronously. */
        if (old_rdb_fd != -1) bioCreateBackgroundJob(BIO_CLOSE_FILE,(void*)(long)old_rdb_fd,NULL,NULL);

        if (rdbLoad(server.rdb_filename,&rsi,RDBFLAGS_REPLICATION) != C_OK) {
            serverLog(LL_WARNING,
                "Failed trying to load the MASTER synchronization "
                "DB from disk");
            cancelReplicationHandshake();
            if (server.rdb_del_sync_files && allPersistenceDisabled()) {
                serverLog(LL_NOTICE,"Removing the RDB file obtained from "
                                    "the master. This replica has persistence "
                                    "disabled");
                bg_unlink(server.rdb_filename);
            }
            /* Note that there's no point in restarting the AOF on sync failure,
               it'll be restarted when sync succeeds or replica promoted. */
            return;
        }

        /* Cleanup. */
        if (server.rdb_del_sync_files && allPersistenceDisabled()) {
            serverLog(LL_NOTICE,"Removing the RDB file obtained from "
                                "the master. This replica has persistence "
                                "disabled");
            bg_unlink(server.rdb_filename);
        }

        zfree(server.repl_transfer_tmpfile);
        close(server.repl_transfer_fd);
        server.repl_transfer_fd = -1;
        server.repl_transfer_tmpfile = NULL;
    }

    /* Final setup of the connected slave <- master link */
    replicationCreateMasterClient(server.repl_transfer_s,rsi.repl_stream_db);
    server.repl_state = REPL_STATE_CONNECTED;
    server.repl_down_since = 0;

    /* Fire the master link modules event. */
    moduleFireServerEvent(REDISMODULE_EVENT_MASTER_LINK_CHANGE,
                          REDISMODULE_SUBEVENT_MASTER_LINK_UP,
                          NULL);

    /* After a full resynchronization we use the replication ID and
     * offset of the master. The secondary ID / offset are cleared since
     * we are starting a new history. */
    memcpy(server.replid,server.master->replid,sizeof(server.replid));
    server.master_repl_offset = server.master->reploff;
    clearReplicationId2();

    /* Let's create the replication backlog if needed. Slaves need to
     * accumulate the backlog regardless of the fact they have sub-slaves
     * or not, in order to behave correctly if they are promoted to
     * masters after a failover. */
    if (server.repl_backlog == NULL) createReplicationBacklog();
    serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Finished with success");

    if (server.supervised_mode == SUPERVISED_SYSTEMD) {
        redisCommunicateSystemd("STATUS=MASTER <-> REPLICA sync: Finished with success. Ready to accept connections.\n");
        redisCommunicateSystemd("READY=1\n");
    }

    /* Restart the AOF subsystem now that we finished the sync. This
     * will trigger an AOF rewrite, and when done will start appending
     * to the new file. */
    if (server.aof_enabled) restartAOFAfterSYNC();
    return;

error:
    cancelReplicationHandshake();
    return;
}

/* Send a synchronous command to the master. Used to send AUTH and
 * REPLCONF commands before starting the replication with SYNC.
 *
 * The command returns an sds string representing the result of the
 * operation. On error the first byte is a "-".
 */
// 发送一个同步命令给主节点，用于在使用SYNC开始复制之前发送AUTH和REPLCONF命令
// 命令返回一个字符串表示的操作的结果，如果第一个字符是"-"表示一个错误
#define SYNC_CMD_READ (1<<0)
#define SYNC_CMD_WRITE (1<<1)
#define SYNC_CMD_FULL (SYNC_CMD_READ|SYNC_CMD_WRITE)
char *sendSynchronousCommand(int flags, connection *conn, ...) {

    /* Create the command to send to the master, we use redis binary
     * protocol to make sure correct arguments are sent. This function
     * is not safe for all binary data. */
    /* Create the command to send to the master, we use simple inline
    * protocol for simplicity as currently we only send simple strings. */
    // 构造一个命令发送给主节点，使用简单的内联协议作为当前要发送的字符串
    // 如果是同步写命令
    if (flags & SYNC_CMD_WRITE) {
        char *arg;
        va_list ap;
        sds cmd = sdsempty();
        sds cmdargs = sdsempty();
        size_t argslen = 0;
        va_start(ap,conn);

        // 构建一个协议字符串
        while(1) {
            arg = va_arg(ap, char*);
            if (arg == NULL) break;

            cmdargs = sdscatprintf(cmdargs,"$%zu\r\n%s\r\n",strlen(arg),arg);
            argslen++;
        }

        va_end(ap);

        cmd = sdscatprintf(cmd,"*%zu\r\n",argslen);
        cmd = sdscatsds(cmd,cmdargs);
        sdsfree(cmdargs);

        /* Transfer command to the server. */
        // 将cmd字符串命令同步写给主节点
        if (connSyncWrite(conn,cmd,sdslen(cmd),server.repl_syncio_timeout*1000)
            == -1)
        {
            sdsfree(cmd);
            return sdscatprintf(sdsempty(),"-Writing to master: %s",
                    connGetLastError(conn));
        }
        sdsfree(cmd);
    }

    /* Read the reply from the server. */
    // 如果是同步读命令
    if (flags & SYNC_CMD_READ) {
        char buf[256];

        // 从主节点同步读一行
        if (connSyncReadLine(conn,buf,sizeof(buf),server.repl_syncio_timeout*1000)
            == -1)
        {
            return sdscatprintf(sdsempty(),"-Reading from master: %s",
                    strerror(errno));
        }
        // 更新读RDB文件的时间
        server.repl_transfer_lastio = server.unixtime;
        // 返回读到的返回
        return sdsnew(buf);
    }
    // 写成功会返回一个NULL
    return NULL;
}

/* Try a partial resynchronization with the master if we are about to reconnect.
 * If there is no cached master structure, at least try to issue a
 * "PSYNC ? -1" command in order to trigger a full resync using the PSYNC
 * command in order to obtain the master replid and the master replication
 * global offset.
 *
 * This function is designed to be called from syncWithMaster(), so the
 * following assumptions are made:
 *
 * 1) We pass the function an already connected socket "fd".
 * 2) This function does not close the file descriptor "fd". However in case
 *    of successful partial resynchronization, the function will reuse
 *    'fd' as file descriptor of the server.master client structure.
 *
 * The function is split in two halves: if read_reply is 0, the function
 * writes the PSYNC command on the socket, and a new function call is
 * needed, with read_reply set to 1, in order to read the reply of the
 * command. This is useful in order to support non blocking operations, so
 * that we write, return into the event loop, and read when there are data.
 *
 * When read_reply is 0 the function returns PSYNC_WRITE_ERR if there
 * was a write error, or PSYNC_WAIT_REPLY to signal we need another call
 * with read_reply set to 1. However even when read_reply is set to 1
 * the function may return PSYNC_WAIT_REPLY again to signal there were
 * insufficient data to read to complete its work. We should re-enter
 * into the event loop and wait in such a case.
 *
 * The function returns:
 *
 * PSYNC_CONTINUE: If the PSYNC command succeeded and we can continue.
 * PSYNC_FULLRESYNC: If PSYNC is supported but a full resync is needed.
 *                   In this case the master replid and global replication
 *                   offset is saved.
 * PSYNC_NOT_SUPPORTED: If the server does not understand PSYNC at all and
 *                      the caller should fall back to SYNC.
 * PSYNC_WRITE_ERROR: There was an error writing the command to the socket.
 * PSYNC_WAIT_REPLY: Call again the function with read_reply set to 1.
 * PSYNC_TRY_LATER: Master is currently in a transient error condition.
 *
 * Notable side effects:
 *
 * 1) As a side effect of the function call the function removes the readable
 *    event handler from "fd", unless the return value is PSYNC_WAIT_REPLY.
 * 2) server.master_initial_offset is set to the right value according
 *    to the master reply. This will be used to populate the 'server.master'
 *    structure replication offset.
 */
// 如果打算重新连接，那么就尝试进行一个部分的重同步，如果主节点的缓存为空，那么可以同步"PSYNC ? -1"去触发一个全量同步，让主节点的运行ID和复制偏移量发送给从节点
// 该函数被syncWithMaster()调用，所以有以下假设：
/*
    1. 参数fd是一个已经处于连接状态的套接字
    2. 该函数不会释放fd。当成功执行部分重同步时，函数会将fd设置为server.master的文件描述符
*/
//函数被分成两部分：如果read_reply为0，则该函数往socket上会写入一个PSYNC命令，并且需要调用一个新函数，还将read_reply设置为1，以便读取命令的回复。这对于支持非阻塞操作是有用的，以便我们写入，返回到事件循环，并在有数据时读取
// 当read_reply为0时，如果有写操作错误，则函数返回PSYNC_WRITE_ERR，或者返回PSYNC_WAIT_REPLY给我们发送一个信号，将read_reply设置为1，然后再次调用。但是，即使read_reply设置为1，该函数也可能再次返回PSYNC_WAIT_REPLY，又一次给我们发送信号，表示数据不够不能完成操作，我们应该重新进入事件循环等待事件发生。
// 函数有如下返回值：
/*
    1.PSYNC_CONTINUE：PSYNC命令执行成功，可以继续执行
    2.PSYNC_FULLRESYNC：主节点支持PSYNC命令，但是需要一个全量同步，在这种情况下，要保存run_id 和全部复制偏移量
    3.PSYNC_WAIT_REPLY：将read_reply设置为1，再一次调用函数
*/
// 注意：
/*
    1.函数可能会删除fd的可读事件处理程序，除非返回值是PSYNC_WAIT_REPLY
    2.server.repl_master_initial_offset根据主节点的返回被设置一个正确的值，这要用于设置server.master结构的复制偏移量
*/
#define PSYNC_WRITE_ERROR 0
#define PSYNC_WAIT_REPLY 1
#define PSYNC_CONTINUE 2
#define PSYNC_FULLRESYNC 3
#define PSYNC_NOT_SUPPORTED 4
#define PSYNC_TRY_LATER 5
// 从节点发送PSYNC命令尝试进行部分重同步
int slaveTryPartialResynchronization(connection *conn, int read_reply) {
    char *psync_replid;
    char psync_offset[32];
    sds reply;

    /* Writing half */
    // 如果read_reply为0，则该函数往socket上会写入一个PSYNC命令
    if (!read_reply) {
        /* Initially set master_initial_offset to -1 to mark the current
         * master replid and offset as not valid. Later if we'll be able to do
         * a FULL resync using the PSYNC command we'll set the offset at the
         * right value, so that this information will be propagated to the
         * client structure representing the master into server.master. */
        // 将repl_master_initial_offset设置为-1表示主节点的run_id和全局复制偏移量是无效的。
        // 如果能使用PSYNC命令执行一个全量同步，会正确设置全复制偏移量，以便这个信息被正确传播主节点的所有从节点中
        server.master_initial_offset = -1;

        // 主节点的缓存不为空，可以尝试进行部分重同步。PSYNC <master_run_id> <repl_offset>
        if (server.cached_master) {
            // 保存缓存runid
            psync_replid = server.cached_master->replid;
            // 获取已经复制的偏移量
            snprintf(psync_offset,sizeof(psync_offset),"%lld", server.cached_master->reploff+1);
            serverLog(LL_NOTICE,"Trying a partial resynchronization (request %s:%s).", psync_replid, psync_offset);
            // 主节点的缓存为空，发送PSYNC ? -1。请求全量同步
        } else {
            serverLog(LL_NOTICE,"Partial resynchronization not possible (no cached master)");
            psync_replid = "?";
            memcpy(psync_offset,"-1",3);
        }

        /* Issue the PSYNC command */
        // 发送一个PSYNC命令给主节点
        reply = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"PSYNC",psync_replid,psync_offset,NULL);
        // 写成功失败会返回一个"-"开头的字符串
        if (reply != NULL) {
            serverLog(LL_WARNING,"Unable to send PSYNC to master: %s",reply);
            sdsfree(reply);
            // 删除文件的可读事件，返回写错误PSYNC_WRITE_ERROR
            connSetReadHandler(conn, NULL);
            return PSYNC_WRITE_ERROR;
        }
        // 返回等待回复的标识PSYNC_WAIT_REPLY，调用者会将read_reply设置为1，然后再次调用该函数，执行下面的读部分。
        return PSYNC_WAIT_REPLY;
    }

    /* Reading half */
    // 从主节点读一个命令保存在reply中
    reply = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);
    if (sdslen(reply) == 0) {
        /* The master may send empty newlines after it receives PSYNC
         * and before to reply, just to keep the connection alive. */
        // 主节点为了保持连接的状态，可能会在接收到PSYNC命令后发送一个空行
        sdsfree(reply);
        // 所以就返回PSYNC_WAIT_REPLY，调用者会将read_reply设置为1，然后再次调用该函数。
        return PSYNC_WAIT_REPLY;
    }

    // 如果读到了一个命令，删除fd的可读事件
    connSetReadHandler(conn, NULL);

    // 接受到的是"+FULLRESYNC"，表示进行一次全量同步
    if (!strncmp(reply,"+FULLRESYNC",11)) {
        char *replid = NULL, *offset = NULL;

        /* FULL RESYNC, parse the reply in order to extract the replid
         * and the replication offset. */
        // 解析回复中的内容，将runid和复制偏移量提取出来
        replid = strchr(reply,' ');
        if (replid) {
            replid++;    //定位到runid的地址
            offset = strchr(replid,' ');
            if (offset) offset++;    //定位offset
        }
        // 如果runid和offset任意为空，那么发生不期望错误
        if (!replid || !offset || (offset-replid-1) != CONFIG_RUN_ID_SIZE) {
            serverLog(LL_WARNING,
                "Master replied with wrong +FULLRESYNC syntax.");
            /* This is an unexpected condition, actually the +FULLRESYNC
             * reply means that the master supports PSYNC, but the reply
             * format seems wrong. To stay safe we blank the master
             * replid to make sure next PSYNCs will fail. */
            // 将主节点的运行ID重置为0
            memset(server.master_replid,0,CONFIG_RUN_ID_SIZE+1);
            // runid和offset获取成功
        } else {
            // 设置服务器保存的主节点的运行ID
            memcpy(server.master_replid, replid, offset-replid-1);
            server.master_replid[CONFIG_RUN_ID_SIZE] = '\0';
            // 主节点的偏移量
            server.master_initial_offset = strtoll(offset,NULL,10);
            serverLog(LL_NOTICE,"Full resync from master: %s:%lld",
                server.master_replid,
                server.master_initial_offset);
        }
        /* We are going to full resync, discard the cached master structure. */
        // 执行全量同步，所以缓存的主节点结构没用了，将其清空
        replicationDiscardCachedMaster();
        sdsfree(reply);
        // 返回执行的状态
        return PSYNC_FULLRESYNC;
    }

    // 接受到的是"+CONTINUE"，表示进行一次部分重同步
    if (!strncmp(reply,"+CONTINUE",9)) {
        /* Partial resync was accepted. */
        serverLog(LL_NOTICE,
            "Successful partial resynchronization with master.");

        /* Check the new replication ID advertised by the master. If it
         * changed, we need to set the new ID as primary ID, and set or
         * secondary ID as the old master ID up to the current offset, so
         * that our sub-slaves will be able to PSYNC with us after a
         * disconnection. */
        char *start = reply+10;
        char *end = reply+9;
        while(end[0] != '\r' && end[0] != '\n' && end[0] != '\0') end++;
        if (end-start == CONFIG_RUN_ID_SIZE) {
            char new[CONFIG_RUN_ID_SIZE+1];
            memcpy(new,start,CONFIG_RUN_ID_SIZE);
            new[CONFIG_RUN_ID_SIZE] = '\0';

            if (strcmp(new,server.cached_master->replid)) {
                /* Master ID changed. */
                serverLog(LL_WARNING,"Master replication ID changed to %s",new);

                /* Set the old ID as our ID2, up to the current offset+1. */
                memcpy(server.replid2,server.cached_master->replid,
                    sizeof(server.replid2));
                server.second_replid_offset = server.master_repl_offset+1;

                /* Update the cached master ID and our own primary ID to the
                 * new one. */
                memcpy(server.replid,new,sizeof(server.replid));
                memcpy(server.cached_master->replid,new,sizeof(server.replid));

                /* Disconnect all the sub-slaves: they need to be notified. */
                disconnectSlaves();
            }
        }

        /* Setup the replication to continue. */
        sdsfree(reply);
        // 因为执行部分重同步，因此要使用缓存的主节点结构，所以将其设置为当前的主节点，被同步的主节点
        replicationResurrectCachedMaster(conn);

        /* If this instance was restarted and we read the metadata to
         * PSYNC from the persistence file, our replication backlog could
         * be still not initialized. Create it. */
        if (server.repl_backlog == NULL) createReplicationBacklog();
        // 返回执行的状态
        return PSYNC_CONTINUE;
    }

    /* If we reach this point we received either an error (since the master does
     * not understand PSYNC or because it is in a special state and cannot
     * serve our request), or an unexpected reply from the master.
     *
     * Return PSYNC_NOT_SUPPORTED on errors we don't understand, otherwise
     * return PSYNC_TRY_LATER if we believe this is a transient error. */

    if (!strncmp(reply,"-NOMASTERLINK",13) ||
        !strncmp(reply,"-LOADING",8))
    {
        serverLog(LL_NOTICE,
            "Master is currently unable to PSYNC "
            "but should be in the future: %s", reply);
        sdsfree(reply);
        return PSYNC_TRY_LATER;
    }

    // 接收到了错误，两种情况。
    // 1. 主节点不支持PSYNC命令，Redis版本低于2.8
    // 2. 从主节点读取了一个不期望的回复
    if (strncmp(reply,"-ERR",4)) {
        /* If it's not an error, log the unexpected event. */
        serverLog(LL_WARNING,
            "Unexpected reply to PSYNC from master: %s", reply);
    } else {
        serverLog(LL_NOTICE,
            "Master does not support PSYNC or is in "
            "error state (reply: %s)", reply);
    }
    sdsfree(reply);
    // 执行全量同步，所以缓存的主节点结构没用了，将其清空
    replicationDiscardCachedMaster();
    // 发送不支持PSYNC命令的状态
    return PSYNC_NOT_SUPPORTED;
}

/* This handler fires when the non blocking connect was able to
 * establish a connection with the master. */
// 从节点同步主节点的回调函数
void syncWithMaster(connection *conn) {
    char tmpfile[256], *err = NULL;
    int dfd = -1, maxtries = 5;
    int psync_result;

    /* If this event fired after the user turned the instance into a master
     * with SLAVEOF NO ONE we must just return ASAP. */
    // 如果服务器处于replication关闭状态，那么直接返回
    if (server.repl_state == REPL_STATE_NONE) {
        connClose(conn);
        return;
    }

    /* Check for errors in the socket: after a non blocking connect() we
     * may find that the socket is in error state. */
    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_WARNING,"Error condition on socket for SYNC: %s",
                connGetLastError(conn));
        goto error;
    }

    /* Send a PING to check the master is able to reply without errors. */
    // 如果复制的状态为REPL_STATE_CONNECTING，发送一个PING去检查主节点是否能正确回复一个PONG
    if (server.repl_state == REPL_STATE_CONNECTING) {
        serverLog(LL_NOTICE,"Non blocking connect for SYNC fired the event.");
        /* Delete the writable event so that the readable event remains
         * registered and we can wait for the PONG reply. */
        connSetReadHandler(conn, syncWithMaster);
        connSetWriteHandler(conn, NULL);
        // 设置复制状态为等待PONG回复
        server.repl_state = REPL_STATE_RECEIVE_PONG;
        /* Send the PING, don't check for errors at all, we have the timeout
         * that will take care about this. */
        // 发送一个PING命令
        err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"PING",NULL);
        if (err) goto write_error;
        return;
    }

    /* Receive the PONG command. */
    // 如果复制的状态为REPL_STATE_RECEIVE_PONG，等待接受PONG命令
    if (server.repl_state == REPL_STATE_RECEIVE_PONG) {
        // 从主节点读一个PONG命令
        err = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);

        /* We accept only two replies as valid, a positive +PONG reply
         * (we just check for "+") or an authentication error.
         * Note that older versions of Redis replied with "operation not
         * permitted" instead of using a proper error code, so we test
         * both. */
        // 只接受两种有效的回复。一种是 "+PONG"，一种是认证错误"-NOAUTH"。
        // 旧版本的返回有"-ERR operation not permitted"
        if (err[0] != '+' &&
            strncmp(err,"-NOAUTH",7) != 0 &&
            strncmp(err,"-NOPERM",7) != 0 &&
            strncmp(err,"-ERR operation not permitted",28) != 0)
        {
            // 没有收到正确的PING命令的回复
            serverLog(LL_WARNING,"Error reply to PING from master: '%s'",err);
            sdsfree(err);
            goto error;
        } else {
            serverLog(LL_NOTICE,
                "Master replied to PING, replication can continue...");
        }
        sdsfree(err);
        // 已经收到PONG，更改状态设置为发送认证命令AUTH给主节点
        server.repl_state = REPL_STATE_SEND_AUTH;
    }

    /* AUTH with the master if required. */
    // 如果需要，发送AUTH认证给主节点
    if (server.repl_state == REPL_STATE_SEND_AUTH) {
        if (server.masteruser && server.masterauth) {
            err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"AUTH",
                                         server.masteruser,server.masterauth,NULL);
            if (err) goto write_error;
            server.repl_state = REPL_STATE_RECEIVE_AUTH;
            return;
            // 如果服务器设置了认证密码
        } else if (server.masterauth) {
            // 写AUTH给主节点
            err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"AUTH",server.masterauth,NULL);
            if (err) goto write_error;
            // 设置状态为等待接受认证回复
            server.repl_state = REPL_STATE_RECEIVE_AUTH;
            return;
            // 如果没有设置认证密码，直接设置复制状态为发送端口号给主节点
        } else {
            server.repl_state = REPL_STATE_SEND_PORT;
        }
    }

    /* Receive AUTH reply. */
    // 接受AUTH认证的回复
    if (server.repl_state == REPL_STATE_RECEIVE_AUTH) {
        // 从主节点读回复
        err = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);
        // 回复错误，认证失败
        if (err[0] == '-') {
            serverLog(LL_WARNING,"Unable to AUTH to MASTER: %s",err);
            sdsfree(err);
            goto error;
        }
        sdsfree(err);
        // 设置复制状态为发送端口号给主节点
        server.repl_state = REPL_STATE_SEND_PORT;
    }

    /* Set the slave port, so that Master's INFO command can list the
     * slave listening port correctly. */
    // 如果复制状态是，发送从节点端口号给主节点，主节点的INFO命令就能够列出从节点正在监听的端口号
    if (server.repl_state == REPL_STATE_SEND_PORT) {
        int port;
        // 获取端口号
        if (server.slave_announce_port) port = server.slave_announce_port;
        else if (server.tls_replication && server.tls_port) port = server.tls_port;
        else port = server.port;
        sds portstr = sdsfromlonglong(port);
        // 将端口号写给主节点
        err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"REPLCONF",
                "listening-port",portstr, NULL);
        sdsfree(portstr);
        if (err) goto write_error;
        sdsfree(err);
        // 设置复制状态为接受端口号
        server.repl_state = REPL_STATE_RECEIVE_PORT;
        return;
    }

    /* Receive REPLCONF listening-port reply. */
    // 复制状态为接受端口号
    if (server.repl_state == REPL_STATE_RECEIVE_PORT) {
        // 从主节点读取端口号
        err = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);
        /* Ignore the error if any, not all the Redis versions support
         * REPLCONF listening-port. */
        // 忽略所有的错误，因为不是所有的Redis版本都支持REPLCONF listening-port命令
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                "REPLCONF listening-port: %s", err);
        }
        sdsfree(err);
        // 设置复制状态为发送IP
        server.repl_state = REPL_STATE_SEND_IP;
    }

    /* Skip REPLCONF ip-address if there is no slave-announce-ip option set. */
    // 复制状态为发送IP且服务器的slave_announce_ip没有保存发送给主节点的IP，直接设置复制状态
    if (server.repl_state == REPL_STATE_SEND_IP &&
        server.slave_announce_ip == NULL)
    {
            // 设置复制状态为发送一个capa（能力？能否解析出RDB文件的EOF流格式）
            server.repl_state = REPL_STATE_SEND_CAPA;
    }

    /* Set the slave ip, so that Master's INFO command can list the
     * slave IP address port correctly in case of port forwarding or NAT. */
    // 复制状态为发送IP
    if (server.repl_state == REPL_STATE_SEND_IP) {
        // 将IP写给主节点
        err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"REPLCONF",
                "ip-address",server.slave_announce_ip, NULL);
        if (err) goto write_error;
        sdsfree(err);
        // 设置复制状态为接受IP
        server.repl_state = REPL_STATE_RECEIVE_IP;
        return;
    }

    /* Receive REPLCONF ip-address reply. */
    // 复制状态为接受IP回复
    if (server.repl_state == REPL_STATE_RECEIVE_IP) {
        // 从主节点读一个IP回复
        err = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);
        /* Ignore the error if any, not all the Redis versions support
         * REPLCONF listening-port. */
        // 错误回复
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                "REPLCONF ip-address: %s", err);
        }
        sdsfree(err);
        // 设置复制状态为发送一个capa（能力？能否解析出RDB文件的EOF流格式）
        server.repl_state = REPL_STATE_SEND_CAPA;
    }

    /* Inform the master of our (slave) capabilities.
     *
     * EOF: supports EOF-style RDB transfer for diskless replication.
     * PSYNC2: supports PSYNC v2, so understands +CONTINUE <new repl ID>.
     *
     * The master will ignore capabilities it does not understand. */
    // 复制状态为发送capa，通知主节点从节点的能力
    if (server.repl_state == REPL_STATE_SEND_CAPA) {
        // 将从节点的capa写给主节点
        err = sendSynchronousCommand(SYNC_CMD_WRITE,conn,"REPLCONF",
                "capa","eof","capa","psync2",NULL);
        if (err) goto write_error;
        sdsfree(err);
        // 设置复制状态为接受从节点的capa
        server.repl_state = REPL_STATE_RECEIVE_CAPA;
        return;
    }

    /* Receive CAPA reply. */
    // 复制状态为接受从节点的capa回复
    if (server.repl_state == REPL_STATE_RECEIVE_CAPA) {
        // 从主节点读取capa回复
        err = sendSynchronousCommand(SYNC_CMD_READ,conn,NULL);
        /* Ignore the error if any, not all the Redis versions support
         * REPLCONF capa. */
        // 错误回复
        if (err[0] == '-') {
            serverLog(LL_NOTICE,"(Non critical) Master does not understand "
                                  "REPLCONF capa: %s", err);
        }
        sdsfree(err);
        // 设置复制状态为发送PSYNC命令
        server.repl_state = REPL_STATE_SEND_PSYNC;
    }

    /* Try a partial resynchonization. If we don't have a cached master
     * slaveTryPartialResynchronization() will at least try to use PSYNC
     * to start a full resynchronization so that we get the master replid
     * and the global offset, to try a partial resync at the next
     * reconnection attempt. */
    // 复制状态为发送PSYNC命令。尝试进行部分重同步。
    // 如果没有缓冲主节点的结构，slaveTryPartialResynchronization()函数将会至少尝试使用PSYNC去进行一个全同步，这样就能得到主节点的运行runid和全局复制偏移量。并且在下次重连接时可以尝试进行部分重同步。
    if (server.repl_state == REPL_STATE_SEND_PSYNC) {
        // 向主节点发送一个部分重同步命令PSYNC，参数0表示不读主节点的回复，只获取主节点的运行runid和全局复制偏移量
        if (slaveTryPartialResynchronization(conn,0) == PSYNC_WRITE_ERROR) {
            // 发送PSYNC出错
            err = sdsnew("Write error sending the PSYNC command.");
            goto write_error;
        }
        // 设置复制状态为等待接受一个PSYNC回复
        server.repl_state = REPL_STATE_RECEIVE_PSYNC;
        return;
    }

    /* If reached this point, we should be in REPL_STATE_RECEIVE_PSYNC. */
    // 到达这里，服务器应该在REPL_STATE_RECEIVE_PSYNC状态中，如果不是，则出错
    if (server.repl_state != REPL_STATE_RECEIVE_PSYNC) {
        serverLog(LL_WARNING,"syncWithMaster(): state machine error, "
                             "state should be RECEIVE_PSYNC but is %d",
                             server.repl_state);
        goto error;
    }

    // 那么尝试进行第二次部分重同步，从主节点读取指令来决定执行部分重同步还是全量同步
    psync_result = slaveTryPartialResynchronization(conn,1);
    // 如果返回PSYNC_WAIT_REPLY，则重新执行该函数
    if (psync_result == PSYNC_WAIT_REPLY) return; /* Try again later... */

    /* If the master is in an transient error, we should try to PSYNC
     * from scratch later, so go to the error path. This happens when
     * the server is loading the dataset or is not connected with its
     * master and so forth. */
    if (psync_result == PSYNC_TRY_LATER) goto error;

    /* Note: if PSYNC does not return WAIT_REPLY, it will take care of
     * uninstalling the read handler from the file descriptor. */

    // 返回PSYNC_CONTINUE，表示可以执行部分重同步，直接返回
    if (psync_result == PSYNC_CONTINUE) {
        serverLog(LL_NOTICE, "MASTER <-> REPLICA sync: Master accepted a Partial Resynchronization.");
        if (server.supervised_mode == SUPERVISED_SYSTEMD) {
            redisCommunicateSystemd("STATUS=MASTER <-> REPLICA sync: Partial Resynchronization accepted. Ready to accept connections.\n");
            redisCommunicateSystemd("READY=1\n");
        }
        return;
    }

    /* PSYNC failed or is not supported: we want our slaves to resync with us
     * as well, if we have any sub-slaves. The master may transfer us an
     * entirely different data set and we have no way to incrementally feed
     * our slaves after that. */
    // PSYNC执行失败，或者是不支持该命令。
    // 关闭所有从节点服务器的连接，强制从节点服务器进行重新同步操作
    disconnectSlaves(); /* Force our slaves to resync with us as well. */
    // 释放复制积压缓冲区，禁止从节点执行PSYNC
    freeReplicationBacklog(); /* Don't allow our chained slaves to PSYNC. */

    /* Fall back to SYNC if needed. Otherwise psync_result == PSYNC_FULLRESYNC
     * and the server.master_replid and master_initial_offset are
     * already populated. */
    // 主节点不支持PSYNC命令，那发送版本兼容的SYNC命令
    if (psync_result == PSYNC_NOT_SUPPORTED) {
        serverLog(LL_NOTICE,"Retrying with SYNC...");
        // 将SYNC命令写给主节点
        if (connSyncWrite(conn,"SYNC\r\n",6,server.repl_syncio_timeout*1000) == -1) {
            serverLog(LL_WARNING,"I/O error writing to MASTER: %s",
                strerror(errno));
            goto error;
        }
    }

    /* Prepare a suitable temp file for bulk transfer */
    if (!useDisklessLoad()) {
        // 执行到这里，psync_result == PSYNC_FULLRESYNC或PSYNC_NOT_SUPPORTED
        // 准备一个合适临时文件用来写入和保存主节点传来的RDB文件数据
        while(maxtries--) {
            // 设置文件的名字
            snprintf(tmpfile,256,
                "temp-%d.%ld.rdb",(int)server.unixtime,(long int)getpid());
            // 以读写，可执行权限打开临时文件
            dfd = open(tmpfile,O_CREAT|O_WRONLY|O_EXCL,0644);
            // 打开成功，跳出循环
            if (dfd != -1) break;
            sleep(1);
        }
        if (dfd == -1) {
            serverLog(LL_WARNING,"Opening the temp file needed for MASTER <-> REPLICA synchronization: %s",strerror(errno));
            goto error;
        }
        server.repl_transfer_tmpfile = zstrdup(tmpfile);
        server.repl_transfer_fd = dfd;
    }

    /* Setup the non blocking download of the bulk file. */
    // 监听一个fd的读事件，并设置该事件的处理程序为readSyncBulkPayload
    if (connSetReadHandler(conn, readSyncBulkPayload)
            == C_ERR)
    {
        char conninfo[CONN_INFO_LEN];
        serverLog(LL_WARNING,
            "Can't create readable event for SYNC: %s (%s)",
            strerror(errno), connGetInfo(conn, conninfo, sizeof(conninfo)));
        goto error;
    }

    // 复制状态为正从主节点接受RDB文件
    server.repl_state = REPL_STATE_TRANSFER;
    // 初始化RDB文件的大小
    server.repl_transfer_size = -1;
    // 已读的大小
    server.repl_transfer_read = 0;
    // 最近一个执行fsync的偏移量为0
    server.repl_transfer_last_fsync_off = 0;
    // 最近一次读到RDB文件内容的时间
    server.repl_transfer_lastio = server.unixtime;
    return;

error:
    // 关闭fd
    if (dfd != -1) close(dfd);
    connClose(conn);
    server.repl_transfer_s = NULL;
    if (server.repl_transfer_fd != -1)
        close(server.repl_transfer_fd);
    if (server.repl_transfer_tmpfile)
        zfree(server.repl_transfer_tmpfile);
    server.repl_transfer_tmpfile = NULL;
    // 从节点和主节点的同步套接字设置为-1
    server.repl_transfer_fd = -1;
    // 复制状态为必须重新连接主节点
    server.repl_state = REPL_STATE_CONNECT;
    return;

// 写错误处理
write_error: /* Handle sendSynchronousCommand(SYNC_CMD_WRITE) errors. */
    serverLog(LL_WARNING,"Sending command to master in replication handshake: %s", err);
    sdsfree(err);
    goto error;
}

// 以非阻塞的方式连接主节点
int connectWithMaster(void) {
    // 获取当前连接方式
    server.repl_transfer_s = server.tls_replication ? connCreateTLS() : connCreateSocket();
    // 链接主节点
    if (connConnect(server.repl_transfer_s, server.masterhost, server.masterport,
                NET_FIRST_BIND_ADDR, syncWithMaster) == C_ERR) {
        serverLog(LL_WARNING,"Unable to connect to MASTER: %s",
                connGetLastError(server.repl_transfer_s));
        connClose(server.repl_transfer_s);
        server.repl_transfer_s = NULL;
        return C_ERR;
    }

    // 最近一次读到RDB文件内容的时间
    server.repl_transfer_lastio = server.unixtime;
    // 处于和主节点正在连接的状态
    server.repl_state = REPL_STATE_CONNECTING;
    return C_OK;
}

/* This function can be called when a non blocking connection is currently
 * in progress to undo it.
 * Never call this function directly, use cancelReplicationHandshake() instead.
 */
// 该函数在一个非阻塞连接的进程中被调用，不能直接使用，使用cancelReplicationHandshake()代替
// 取消已连接主节点的状态
void undoConnectWithMaster(void) {
    connClose(server.repl_transfer_s);
    server.repl_transfer_s = NULL;
}

/* Abort the async download of the bulk dataset while SYNC-ing with master.
 * Never call this function directly, use cancelReplicationHandshake() instead.
 */
// 在与主节点同步时，中止批量数据集的异步下载，该函数不能直接使用，使用cancelReplicationHandshake()代替
void replicationAbortSyncTransfer(void) {
    // 保存复制处于传输状态
    serverAssert(server.repl_state == REPL_STATE_TRANSFER);
    // 关闭从节点和主节点的连接
    undoConnectWithMaster();
    if (server.repl_transfer_fd!=-1) {
        // 关闭传输RDB文件的临时文件描述符
        close(server.repl_transfer_fd);
        // 删除临时的RDB文件
        bg_unlink(server.repl_transfer_tmpfile);
        zfree(server.repl_transfer_tmpfile);
        server.repl_transfer_tmpfile = NULL;
        server.repl_transfer_fd = -1;
    }
}

/* This function aborts a non blocking replication attempt if there is one
 * in progress, by canceling the non-blocking connect attempt or
 * the initial bulk transfer.
 *
 * If there was a replication handshake in progress 1 is returned and
 * the replication state (server.repl_state) set to REPL_STATE_CONNECT.
 *
 * Otherwise zero is returned and no operation is performed at all. */
// 如果一个非阻塞复制正在进行，该函数可以中断它。通过取消非阻塞连接的尝试或初始化传输
// 如果复制在握手阶段被取消，返回1并且复制状态设置为REPL_STATE_CONNECT
// 否则返回0，什么也没执行
int cancelReplicationHandshake(void) {
    // 如果复制处于传输状态，已经握手完成
    if (server.repl_state == REPL_STATE_TRANSFER) {
        // 中断复制操作
        replicationAbortSyncTransfer();
        // 设置复制必须重新连接主节点
        server.repl_state = REPL_STATE_CONNECT;
        // 如果复制已处于和主节点正在连接的状态或者复制状态处于握手过程中的状态
    } else if (server.repl_state == REPL_STATE_CONNECTING ||
               slaveIsInHandshakeState())
    {
        // 取消和主节点的连接
        undoConnectWithMaster();
        // 设置复制必须重新连接主节点
        server.repl_state = REPL_STATE_CONNECT;
    } else {
        return 0;
    }
    return 1;
}

/* Set replication to the specified master address and port. */
// 设置复制操作的主节点IP和端口
void replicationSetMaster(char *ip, int port) {
    int was_master = server.masterhost == NULL;

    // 按需清除原来的主节点信息
    sdsfree(server.masterhost);
    // 设置ip和端口
    server.masterhost = sdsnew(ip);
    server.masterport = port;
    // 如果有其他的主节点，在释放
    // 例如服务器1是服务器2的主节点，现在服务器2要同步服务器3，服务器3要成为服务器2的主节点，因此要释放服务器1
    if (server.master) {
        freeClient(server.master);
    }
    // 解除所有客户端的阻塞状态
    disconnectAllBlockedClients(); /* Clients blocked in master, now slave. */

    /* Update oom_score_adj */
    setOOMScoreAdj(-1);

    /* Force our slaves to resync with us as well. They may hopefully be able
     * to partially resync with us, but we can notify the replid change. */
    // 关闭所有从节点服务器的连接，强制从节点服务器进行重新同步操作
    disconnectSlaves();
    // 取消执行复制操作
    cancelReplicationHandshake();
    /* Before destroying our master state, create a cached master using
     * our own parameters, to later PSYNC with the new master. */
    if (was_master) {
        // 释放主节点结构的缓存，不会执行部分重同步PSYNC
        replicationDiscardCachedMaster();
        replicationCacheMasterUsingMyself();
    }

    /* Fire the role change modules event. */
    moduleFireServerEvent(REDISMODULE_EVENT_REPLICATION_ROLE_CHANGED,
                          REDISMODULE_EVENT_REPLROLECHANGED_NOW_REPLICA,
                          NULL);

    /* Fire the master link modules event. */
    if (server.repl_state == REPL_STATE_CONNECTED)
        moduleFireServerEvent(REDISMODULE_EVENT_MASTER_LINK_CHANGE,
                              REDISMODULE_SUBEVENT_MASTER_LINK_DOWN,
                              NULL);

    server.repl_state = REPL_STATE_CONNECT;
}

/* Cancel replication, setting the instance as a master itself. */
// 取消复制操作，设置服务器为主服务器
void replicationUnsetMaster(void) {
    if (server.masterhost == NULL) return; /* Nothing to do. */

    /* Fire the master link modules event. */
    if (server.repl_state == REPL_STATE_CONNECTED)
        moduleFireServerEvent(REDISMODULE_EVENT_MASTER_LINK_CHANGE,
                              REDISMODULE_SUBEVENT_MASTER_LINK_DOWN,
                              NULL);

    // 释放主节点ip并设置为空
    sdsfree(server.masterhost);
    server.masterhost = NULL;
    // 释放主节点的client
    if (server.master) freeClient(server.master);
    // 释放主节点结构的缓存，当在重连接后不在进行重同步时调用
    replicationDiscardCachedMaster();
    // 取消执行复制操作
    cancelReplicationHandshake();
    /* When a slave is turned into a master, the current replication ID
     * (that was inherited from the master at synchronization time) is
     * used as secondary ID up to the current offset, and a new replication
     * ID is created to continue with a new replication history.
     *
     * NOTE: this function MUST be called after we call
     * freeClient(server.master), since there we adjust the replication
     * offset trimming the final PINGs. See Github issue #7320. */
    shiftReplicationId();
    /* Disconnecting all the slaves is required: we need to inform slaves
     * of the replication ID change (see shiftReplicationId() call). However
     * the slaves will be able to partially resync with us, so it will be
     * a very fast reconnection. */
    disconnectSlaves();
    server.repl_state = REPL_STATE_NONE;

    /* We need to make sure the new master will start the replication stream
     * with a SELECT statement. This is forced after a full resync, but
     * with PSYNC version 2, there is no need for full resync after a
     * master switch. */
    server.slaveseldb = -1;

    /* Update oom_score_adj */
    setOOMScoreAdj(-1);

    /* Once we turn from slave to master, we consider the starting time without
     * slaves (that is used to count the replication backlog time to live) as
     * starting from now. Otherwise the backlog will be freed after a
     * failover if slaves do not connect immediately. */
    server.repl_no_slaves_since = server.unixtime;

    /* Fire the role change modules event. */
    moduleFireServerEvent(REDISMODULE_EVENT_REPLICATION_ROLE_CHANGED,
                          REDISMODULE_EVENT_REPLROLECHANGED_NOW_MASTER,
                          NULL);

    /* Restart the AOF subsystem in case we shut it down during a sync when
     * we were still a slave. */
    if (server.aof_enabled && server.aof_state == AOF_OFF) restartAOFAfterSYNC();
}

/* This function is called when the slave lose the connection with the
 * master into an unexpected way. */
// 该函数当从节点和主节点断开连接后被调用
void replicationHandleMasterDisconnection(void) {
    /* Fire the master link modules event. */
    if (server.repl_state == REPL_STATE_CONNECTED)
        moduleFireServerEvent(REDISMODULE_EVENT_MASTER_LINK_CHANGE,
                              REDISMODULE_SUBEVENT_MASTER_LINK_DOWN,
                              NULL);

    server.master = NULL;
    // 设置复制状态为必须重新连接主节点
    server.repl_state = REPL_STATE_CONNECT;
    // 设置连接断开的时长
    server.repl_down_since = server.unixtime;
    /* We lost connection with our master, don't disconnect slaves yet,
     * maybe we'll be able to PSYNC with our master later. We'll disconnect
     * the slaves only if we'll have to do a full resync with our master. */
}

// slaveof 命令实现
void replicaofCommand(client *c) {
    /* SLAVEOF is not allowed in cluster mode as replication is automatically
     * configured using the current address of the master node. */
    // 如果当前服务为集群模式，不能进行复制操作
    if (server.cluster_enabled) {
        addReplyError(c,"REPLICAOF not allowed in cluster mode.");
        return;
    }

    /* The special host/port combination "NO" "ONE" turns the instance
     * into a master. Otherwise the new master address is set. */
    // SLAVEOF NO ONE命令使得这个从节点关闭复制功能，并从从节点转变回主节点，原来同步所得的数据集不会被丢弃。
    if (!strcasecmp(c->argv[1]->ptr,"no") &&
        !strcasecmp(c->argv[2]->ptr,"one")) {
        // 如果保存了主节点IP
        if (server.masterhost) {
            // 取消复制操作，设置服务器为主服务器
            replicationUnsetMaster();
            // 获取client的每种信息，并以sds形式返回，并打印到日志中
            sds client = catClientInfoString(sdsempty(),c);
            serverLog(LL_NOTICE,"MASTER MODE enabled (user request from '%s')",
                client);
            sdsfree(client);
        }
        // SLAVEOF host port
    } else {
        long port;

        if (c->flags & CLIENT_SLAVE)
        {
            /* If a client is already a replica they cannot run this command,
             * because it involves flushing all replicas (including this
             * client) */
            addReplyError(c, "Command is not valid when client is a replica.");
            return;
        }

        // 获取端口号
        if ((getLongFromObjectOrReply(c, c->argv[2], &port, NULL) != C_OK))
            return;

        /* Check if we are already attached to the specified slave */
        // 如果已存在从属于masterhost主节点且命令参数指定的主节点和masterhost相等，端口也相等，直接返回
        if (server.masterhost && !strcasecmp(server.masterhost,c->argv[1]->ptr)
            && server.masterport == port) {
            serverLog(LL_NOTICE,"REPLICAOF would result into synchronization "
                                "with the master we are already connected "
                                "with. No operation performed.");
            addReplySds(c,sdsnew("+OK Already connected to specified "
                                 "master\r\n"));
            return;
        }
        /* There was no previous master or the user specified a different one,
         * we can continue. */
        // 第一次执行设置端口和ip，或者是重新设置端口和IP
        // 设置服务器复制操作的主节点IP和端口
        replicationSetMaster(c->argv[1]->ptr, port);
        // 获取client的每种信息，并以sds形式返回，并打印到日志中
        sds client = catClientInfoString(sdsempty(),c);
        serverLog(LL_NOTICE,"REPLICAOF %s:%d enabled (user request from '%s')",
            server.masterhost, server.masterport, client);
        sdsfree(client);
    }
    // 回复ok
    addReply(c,shared.ok);
}

/* ROLE command: provide information about the role of the instance
 * (master or slave) and additional information related to replication
 * in an easy to process format. */
//  Role 命令查看主从实例所属的角色，角色有master, slave, sentinel。
void roleCommand(client *c) {
    if (server.masterhost == NULL) {
        listIter li;
        listNode *ln;
        void *mbcount;
        int slaves = 0;

        addReplyArrayLen(c,3);
        addReplyBulkCBuffer(c,"master",6);
        addReplyLongLong(c,server.master_repl_offset);
        mbcount = addReplyDeferredLen(c);
        listRewind(server.slaves,&li);
        while((ln = listNext(&li))) {
            client *slave = ln->value;
            char ip[NET_IP_STR_LEN], *slaveip = slave->slave_ip;

            if (slaveip[0] == '\0') {
                if (connPeerToString(slave->conn,ip,sizeof(ip),NULL) == -1)
                    continue;
                slaveip = ip;
            }
            if (slave->replstate != SLAVE_STATE_ONLINE) continue;
            addReplyArrayLen(c,3);
            addReplyBulkCString(c,slaveip);
            addReplyBulkLongLong(c,slave->slave_listening_port);
            addReplyBulkLongLong(c,slave->repl_ack_off);
            slaves++;
        }
        setDeferredArrayLen(c,mbcount,slaves);
    } else {
        char *slavestate = NULL;

        addReplyArrayLen(c,5);
        addReplyBulkCBuffer(c,"slave",5);
        addReplyBulkCString(c,server.masterhost);
        addReplyLongLong(c,server.masterport);
        if (slaveIsInHandshakeState()) {
            slavestate = "handshake";
        } else {
            switch(server.repl_state) {
            case REPL_STATE_NONE: slavestate = "none"; break;
            case REPL_STATE_CONNECT: slavestate = "connect"; break;
            case REPL_STATE_CONNECTING: slavestate = "connecting"; break;
            case REPL_STATE_TRANSFER: slavestate = "sync"; break;
            case REPL_STATE_CONNECTED: slavestate = "connected"; break;
            default: slavestate = "unknown"; break;
            }
        }
        addReplyBulkCString(c,slavestate);
        addReplyLongLong(c,server.master ? server.master->reploff : -1);
    }
}

/* Send a REPLCONF ACK command to the master to inform it about the current
 * processed offset. If we are not connected with a master, the command has
 * no effects. */
// 发送一个REPLCONF ACK命令给主节点去报告关于当前处理的offset。如果没有连接主节点，该命令没效果
void replicationSendAck(void) {
    client *c = server.master;

    if (c != NULL) {
        // 设置强制回复的标志
        c->flags |= CLIENT_MASTER_FORCE_REPLY;
        // 回复REPLCONF ACK offset指令取报告主节点当前的偏移量
        addReplyArrayLen(c,3);
        addReplyBulkCString(c,"REPLCONF");
        addReplyBulkCString(c,"ACK");
        addReplyBulkLongLong(c,c->reploff);
        // 取消强制回复的标志
        c->flags &= ~CLIENT_MASTER_FORCE_REPLY;
    }
}

/* ---------------------- MASTER CACHING FOR PSYNC -------------------------- */

/* In order to implement partial synchronization we need to be able to cache
 * our master's client structure after a transient disconnection.
 * It is cached into server.cached_master and flushed away using the following
 * functions. */

/* This function is called by freeClient() in order to cache the master
 * client structure instead of destroying it. freeClient() will return
 * ASAP after this function returns, so every action needed to avoid problems
 * with a client that is really "suspended" has to be done by this function.
 *
 * The other functions that will deal with the cached master are:
 *
 * replicationDiscardCachedMaster() that will make sure to kill the client
 * as for some reason we don't want to use it in the future.
 *
 * replicationResurrectCachedMaster() that is used after a successful PSYNC
 * handshake in order to reactivate the cached master.
 */
// 为了实现部分重同步，我们需要在主节点断线时将主节点client的结构缓存起来。保存到server.cached_master
// 该函数被freeClient()调用，目的是缓存主节点client的结构。
// replicationDiscardCachedMaster() 确认清空整个缓存的master
// replicationResurrectCachedMaster() 用于在PSYNC成功后将缓存中的从节点提取出来，成为新的主节点
void replicationCacheMaster(client *c) {
    serverAssert(server.master != NULL && server.cached_master == NULL);
    serverLog(LL_NOTICE,"Caching the disconnected master state.");

    /* Unlink the client from the server structures. */
    // 从服务器中删除主节点client
    unlinkClient(c);

    /* Reset the master client so that's ready to accept new commands:
     * we want to discard te non processed query buffers and non processed
     * offsets, including pending transactions, already populated arguments,
     * pending outputs to the master. */
    sdsclear(server.master->querybuf);
    sdsclear(server.master->pending_querybuf);
    server.master->read_reploff = server.master->reploff;
    if (c->flags & CLIENT_MULTI) discardTransaction(c);
    listEmpty(c->reply);
    c->sentlen = 0;
    c->reply_bytes = 0;
    c->bufpos = 0;
    resetClient(c);

    /* Save the master. Server.master will be set to null later by
     * replicationHandleMasterDisconnection(). */
    // 缓存主节点client到cached_master
    server.cached_master = server.master;

    /* Invalidate the Peer ID cache. */
    // 释放peerid的缓存
    if (c->peerid) {
        sdsfree(c->peerid);
        c->peerid = NULL;
    }

    /* Caching the master happens instead of the actual freeClient() call,
     * so make sure to adjust the replication state. This function will
     * also set server.master to NULL. */
    // 调整从节点和主节点断开连接后的状态
    replicationHandleMasterDisconnection();
}

/* This function is called when a master is turend into a slave, in order to
 * create from scratch a cached master for the new client, that will allow
 * to PSYNC with the slave that was promoted as the new master after a
 * failover.
 *
 * Assuming this instance was previously the master instance of the new master,
 * the new master will accept its replication ID, and potentiall also the
 * current offset if no data was lost during the failover. So we use our
 * current replication ID and offset in order to synthesize a cached master. */
void replicationCacheMasterUsingMyself(void) {
    serverLog(LL_NOTICE,
        "Before turning into a replica, using my own master parameters "
        "to synthesize a cached master: I may be able to synchronize with "
        "the new master with just a partial transfer.");

    /* This will be used to populate the field server.master->reploff
     * by replicationCreateMasterClient(). We'll later set the created
     * master as server.cached_master, so the replica will use such
     * offset for PSYNC. */
    server.master_initial_offset = server.master_repl_offset;

    /* The master client we create can be set to any DBID, because
     * the new master will start its replication stream with SELECT. */
    replicationCreateMasterClient(NULL,-1);

    /* Use our own ID / offset. */
    memcpy(server.master->replid, server.replid, sizeof(server.replid));

    /* Set as cached master. */
    unlinkClient(server.master);
    server.cached_master = server.master;
    server.master = NULL;
}

/* Free a cached master, called when there are no longer the conditions for
 * a partial resync on reconnection. */
// 释放主节点结构的缓存，当在重连接后不在进行重同步时调用
void replicationDiscardCachedMaster(void) {
    if (server.cached_master == NULL) return;

    serverLog(LL_NOTICE,"Discarding previously cached master state.");
    // 取消缓存的主节点client的主节点标识
    server.cached_master->flags &= ~CLIENT_MASTER;
    // 释放缓存的主节点client并设置为空
    freeClient(server.cached_master);
    server.cached_master = NULL;
}

/* Turn the cached master into the current master, using the file descriptor
 * passed as argument as the socket for the new master.
 *
 * This function is called when successfully setup a partial resynchronization
 * so the stream of data that we'll receive will start from were this
 * master left. */
// 将缓存的主节点设置为当前的主节点，使用newfd作为新主节点的socket
void replicationResurrectCachedMaster(connection *conn) {
    // 设置缓存的主节点client为当前的主节点client
    server.master = server.cached_master;
    // 清空缓存
    server.cached_master = NULL;
    // 设置新主节点的socket
    server.master->conn = conn;
    connSetPrivateData(server.master->conn, server.master);
    // 取消主节点client立即关闭的标识
    server.master->flags &= ~(CLIENT_CLOSE_AFTER_REPLY|CLIENT_CLOSE_ASAP);
    // 认证通过
    server.master->authenticated = 1;
    // 设置交互时间
    server.master->lastinteraction = server.unixtime;
    // 设置复制状态为等待重连接
    server.repl_state = REPL_STATE_CONNECTED;
    server.repl_down_since = 0;

    /* Fire the master link modules event. */
    moduleFireServerEvent(REDISMODULE_EVENT_MASTER_LINK_CHANGE,
                          REDISMODULE_SUBEVENT_MASTER_LINK_UP,
                          NULL);

    /* Re-add to the list of clients. */
    // 将新的主节点的client加入到服务器的client链表中
    linkClient(server.master);
    // 监听主节点的读事件，处理程序设置为readQueryFromClient
    if (connSetReadHandler(server.master->conn, readQueryFromClient)) {
        serverLog(LL_WARNING,"Error resurrecting the cached master, impossible to add the readable handler: %s", strerror(errno));
        freeClientAsync(server.master); /* Close ASAP. */
    }

    /* We may also need to install the write handler as well if there is
     * pending data in the write buffers. */
    // 如果主节点的client中的回复缓冲区中还有数据
    if (clientHasPendingReplies(server.master)) {
        // 监听主节点的写事件，处理程序设置为sendReplyToClient
        if (connSetWriteHandler(server.master->conn, sendReplyToClient)) {
            serverLog(LL_WARNING,"Error resurrecting the cached master, impossible to add the writable handler: %s", strerror(errno));
            freeClientAsync(server.master); /* Close ASAP. */
        }
    }
}

/* ------------------------- MIN-SLAVES-TO-WRITE  --------------------------- */

/* This function counts the number of slaves with lag <= min-slaves-max-lag.
 * If the option is active, the server will prevent writes if there are not
 * enough connected slaves with the specified lag (or less). */
// 更新延迟至log小于min-slaves-max-lag的从服务器数量
void refreshGoodSlavesCount(void) {
    listIter li;
    listNode *ln;
    int good = 0;

    // 没设置限制则返回
    if (!server.repl_min_slaves_to_write ||
        !server.repl_min_slaves_max_lag) return;

    listRewind(server.slaves,&li);
    // 遍历所有的从节点client
    while((ln = listNext(&li))) {
        client *slave = ln->value;
        // 计算延迟值
        time_t lag = server.unixtime - slave->repl_ack_time;

        // 计数小于延迟限制的个数
        if (slave->replstate == SLAVE_STATE_ONLINE &&
            lag <= server.repl_min_slaves_max_lag) good++;
    }
    server.repl_good_slaves_count = good;
}

/* ----------------------- REPLICATION SCRIPT CACHE --------------------------
 * The goal of this code is to keep track of scripts already sent to every
 * connected slave, in order to be able to replicate EVALSHA as it is without
 * translating it to EVAL every time it is possible.
 *
 * We use a capped collection implemented by a hash table for fast lookup
 * of scripts we can send as EVALSHA, plus a linked list that is used for
 * eviction of the oldest entry when the max number of items is reached.
 *
 * We don't care about taking a different cache for every different slave
 * since to fill the cache again is not very costly, the goal of this code
 * is to avoid that the same big script is transmitted a big number of times
 * per second wasting bandwidth and processor speed, but it is not a problem
 * if we need to rebuild the cache from scratch from time to time, every used
 * script will need to be transmitted a single time to reappear in the cache.
 *
 * This is how the system works:
 *
 * 1) Every time a new slave connects, we flush the whole script cache.
 * 2) We only send as EVALSHA what was sent to the master as EVALSHA, without
 *    trying to convert EVAL into EVALSHA specifically for slaves.
 * 3) Every time we transmit a script as EVAL to the slaves, we also add the
 *    corresponding SHA1 of the script into the cache as we are sure every
 *    slave knows about the script starting from now.
 * 4) On SCRIPT FLUSH command, we replicate the command to all the slaves
 *    and at the same time flush the script cache.
 * 5) When the last slave disconnects, flush the cache.
 * 6) We handle SCRIPT LOAD as well since that's how scripts are loaded
 *    in the master sometimes.
 */
// 这里的代码是为了将已经发送给从节点的脚本保存到缓存中，这样执行过一次EVAL之后，其他时候都可以直接发送给EVALSHA
// 使用了一个固定集合(capped collection)。集合由一个负责快速查找的哈希表和用作环形队列的链表组成
// 我们不为每个不同的从节点保存独立的缓存，因为再次填充缓存并不是非常昂贵的，这个代码的目的是避免同样的大脚本每秒丢弃大量的浪费带宽和 处理器速度，但是如果我们需要不时地从头重新构建缓存，这不是问题，每个使用的脚本将需要一次传输以重新出现在缓存中
// 以下是系统工作的方式
/*
    1. 每一次有新的从节点连接，刷新整个脚本缓存
    2. 程序只在主节点接收到EVALSHA时才向从节点发送EVALSHA，不会尝试将EVAL转换为EVALSHA
    3. 每一次将脚本作为EVAL发送给从节点，要将脚本的SHA1键保存到脚本字典中，字典键为SHA1，值为NULL
    4. 当client执行SCRIPT FLUSH时，服务器将该命令复制给所有的从节点，让他们也刷新自己的脚本缓存
    5. 当所有的服务器都断开连接时，清空脚本缓存
    6. SCRIPT FLUSH命令对这个脚本的缓存作用和EVAL一样
*/

/* Initialize the script cache, only called at startup. */
// 初始化脚本缓存，只在服务器启动时调用
void replicationScriptCacheInit(void) {
    // 最大缓存脚本数
    server.repl_scriptcache_size = 10000;
    // 创建一个负责快速查找的字典
    server.repl_scriptcache_dict = dictCreate(&replScriptCacheDictType,NULL);
    // 创建用作环形队列的链表
    server.repl_scriptcache_fifo = listCreate();
}

/* Empty the script cache. Should be called every time we are no longer sure
 * that every slave knows about all the scripts in our set, or when the
 * current AOF "context" is no longer aware of the script. In general we
 * should flush the cache:
 *
 * 1) Every time a new slave reconnects to this master and performs a
 *    full SYNC (PSYNC does not require flushing).
 * 2) Every time an AOF rewrite is performed.
 * 3) Every time we are left without slaves at all, and AOF is off, in order
 *    to reclaim otherwise unused memory.
 */
// 清空脚本缓存。适用于以下三种情况
/*
    1. 每次有新从节点重连接进来，且执行全量同步，部分重同步PSYNC不需要进行清空缓存
    2. 每次AOF重写执行
    3. 没有任何从节点时，AOF关闭的时候，为了节省内存而清空缓存
*/
void replicationScriptCacheFlush(void) {
    dictEmpty(server.repl_scriptcache_dict,NULL);
    listRelease(server.repl_scriptcache_fifo);
    server.repl_scriptcache_fifo = listCreate();
}

/* Add an entry into the script cache, if we reach max number of entries the
 * oldest is removed from the list. */
// 将脚本的SHA1加入到脚本缓存中，如果到达最大数量的限制，则删除最旧的脚本
void replicationScriptCacheAdd(sds sha1) {
    int retval;
    sds key = sdsdup(sha1);

    /* Evict oldest. */
    // 如果当前已经达到最大缓存脚本数，删除oldest
    if (listLength(server.repl_scriptcache_fifo) == server.repl_scriptcache_size)
    {
        listNode *ln = listLast(server.repl_scriptcache_fifo);
        sds oldest = listNodeValue(ln);

        // 删除最旧的脚本缓存
        retval = dictDelete(server.repl_scriptcache_dict,oldest);
        serverAssert(retval == DICT_OK);
        listDelNode(server.repl_scriptcache_fifo,ln);
    }

    /* Add current. */
    // 将新的脚本缓存的SHA1添加到字典中
    retval = dictAdd(server.repl_scriptcache_dict,key,NULL);
    listAddNodeHead(server.repl_scriptcache_fifo,key);
    serverAssert(retval == DICT_OK);
}

/* Returns non-zero if the specified entry exists inside the cache, that is,
 * if all the slaves are aware of this script SHA1. */
// sha1脚本是否在缓存中，返回1表示存在，否则返回0
int replicationScriptCacheExists(sds sha1) {
    return dictFind(server.repl_scriptcache_dict,sha1) != NULL;
}

/* ----------------------- SYNCHRONOUS REPLICATION --------------------------
 * Redis synchronous replication design can be summarized in points:
 *
 * - Redis masters have a global replication offset, used by PSYNC.
 * - Master increment the offset every time new commands are sent to slaves.
 * - Slaves ping back masters with the offset processed so far.
 *
 * So synchronous replication adds a new WAIT command in the form:
 *
 *   WAIT <num_replicas> <milliseconds_timeout>
 *
 * That returns the number of replicas that processed the query when
 * we finally have at least num_replicas, or when the timeout was
 * reached.
 *
 * The command is implemented in this way:
 *
 * - Every time a client processes a command, we remember the replication
 *   offset after sending that command to the slaves.
 * - When WAIT is called, we ask slaves to send an acknowledgement ASAP.
 *   The client is blocked at the same time (see blocked.c).
 * - Once we receive enough ACKs for a given offset or when the timeout
 *   is reached, the WAIT command is unblocked and the reply sent to the
 *   client.
 */

/* This just set a flag so that we broadcast a REPLCONF GETACK command
 * to all the slaves in the beforeSleep() function. Note that this way
 * we "group" all the clients that want to wait for synchronous replication
 * in a given event loop iteration, and send a single GETACK for them all. */
// 设置传播REPLCONF GETACK命令到所有从节点的flag
void replicationRequestAckFromSlaves(void) {
    server.get_ack_from_slaves = 1;
}

/* Return the number of slaves that already acknowledged the specified
 * replication offset. */
// 返回已经确认指定复制偏移量的从节点数量
int replicationCountAcksByOffset(long long offset) {
    listIter li;
    listNode *ln;
    int count = 0;

    listRewind(server.slaves,&li);
    // 遍历所有的从节点
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        // 跳过没有完成RDB文件传输的从节点
        if (slave->replstate != SLAVE_STATE_ONLINE) continue;
        // 如果通过ack命令接收到的偏移量大于指定的offset，则计数
        if (slave->repl_ack_off >= offset) count++;
    }
    return count;
}

/* WAIT for N replicas to acknowledge the processing of our latest
 * write command (and all the previous commands). */
// WAIT N个从节点，确认处理我们最新的写入命令（以及所有以前的命令）。
void waitCommand(client *c) {
    mstime_t timeout;
    long numreplicas, ackreplicas;
    long long offset = c->woff;

    if (server.masterhost) {
        addReplyError(c,"WAIT cannot be used with replica instances. Please also note that since Redis 4.0 if a replica is configured to be writable (which is not the default) writes to replicas are just local and are not propagated.");
        return;
    }

    /* Argument parsing. */
    // 解析参数，保存numslaves和timeout
    if (getLongFromObjectOrReply(c,c->argv[1],&numreplicas,NULL) != C_OK)
        return;
    if (getTimeoutFromObjectOrReply(c,c->argv[2],&timeout,UNIT_MILLISECONDS)
        != C_OK) return;

    /* First try without blocking at all. */
    // 返回已经确认指定复制偏移量的从节点数量
    ackreplicas = replicationCountAcksByOffset(c->woff);
    // 如果client处于事务环境中，或等待到了多于numreplicas个的从节点，发送回复，返回
    if (ackreplicas >= numreplicas || c->flags & CLIENT_MULTI) {
        addReplyLongLong(c,ackreplicas);
        return;
    }

    /* Otherwise block the client and put it into our list of clients
     * waiting for ack from slaves. */
    // 否则阻塞client，将其放到等待 WAIT 命令的client链表clients_waiting_acks中，等待从节点的ack
    c->bpop.timeout = timeout;
    c->bpop.reploffset = offset;
    c->bpop.numreplicas = numreplicas;
    listAddNodeTail(server.clients_waiting_acks,c);
    blockClient(c,BLOCKED_WAIT);

    /* Make sure that the server will send an ACK request to all the slaves
     * before returning to the event loop. */
    // 设置传播REPLCONF GETACK命令到所有从节点的flag
    replicationRequestAckFromSlaves();
}

/* This is called by unblockClient() to perform the blocking op type
 * specific cleanup. We just remove the client from the list of clients
 * waiting for replica acks. Never call it directly, call unblockClient()
 * instead. */
// 解除阻塞等待 WAIT 命令的client
void unblockClientWaitingReplicas(client *c) {
    listNode *ln = listSearchKey(server.clients_waiting_acks,c);
    serverAssert(ln != NULL);
    listDelNode(server.clients_waiting_acks,ln);
}

/* Check if there are clients blocked in WAIT that can be unblocked since
 * we received enough ACKs from slaves. */
// 检查是否有被WAIT阻塞的客户端可以被解除阻塞，因为我们从从节点收到足够的ACK。
void processClientsWaitingReplicas(void) {
    long long last_offset = 0;
    int last_numreplicas = 0;

    listIter li;
    listNode *ln;

    listRewind(server.clients_waiting_acks,&li);
    // 遍历所有被wait命令阻塞的client链表
    while((ln = listNext(&li))) {
        client *c = ln->value;

        /* Every time we find a client that is satisfied for a given
         * offset and number of replicas, we remember it so the next client
         * may be unblocked without calling replicationCountAcksByOffset()
         * if the requested offset / replicas were equal or less. */
        // 每次我们找到一个满足给定偏移量和复制数量的客户端时，我们记住它，所以如果所请求的偏移量/副本等于或小于下限，则下一个客户端可能会被解除阻塞而不调用replicationCountAcksByOffset（）
        // 如果找到client满足条件，则解除client的阻塞
        if (last_offset && last_offset > c->bpop.reploffset &&
                           last_numreplicas > c->bpop.numreplicas)
        {
            unblockClient(c);
            addReplyLongLong(c,last_numreplicas);
        } else {
            int numreplicas = replicationCountAcksByOffset(c->bpop.reploffset);

            // 记住满足给定偏移量和复制数量的客户端的个数
            if (numreplicas >= c->bpop.numreplicas) {
                last_offset = c->bpop.reploffset;
                last_numreplicas = numreplicas;
                unblockClient(c);
                addReplyLongLong(c,numreplicas);
            }
        }
    }
}

/* Return the slave replication offset for this instance, that is
 * the offset for which we already processed the master replication stream. */
// 返回从节点复制偏移量
long long replicationGetSlaveOffset(void) {
    long long offset = 0;

    if (server.masterhost != NULL) {
        // 获取已经复制的偏移量
        if (server.master) {
            offset = server.master->reploff;
            // 部分复制的偏移量
        } else if (server.cached_master) {
            offset = server.cached_master->reploff;
        }
    }
    /* offset may be -1 when the master does not support it at all, however
     * this function is designed to return an offset that can express the
     * amount of data processed by the master, so we return a positive
     * integer. */
    if (offset < 0) offset = 0;
    return offset;
}

/* --------------------------- REPLICATION CRON  ---------------------------- */

/* Replication cron function, called 1 time per second. */
// 复制周期执行的函数，每秒调用1次
void replicationCron(void) {
    static long long replication_cron_loops = 0;

    /* Non blocking connection timeout? */
    // 如果处于正在连接状态或给定的复制状态是握手状态，且连接超时
    if (server.masterhost &&
        (server.repl_state == REPL_STATE_CONNECTING ||
         slaveIsInHandshakeState()) &&
         (time(NULL)-server.repl_transfer_lastio) > server.repl_timeout)
    {
        serverLog(LL_WARNING,"Timeout connecting to the MASTER...");
        // 中断非阻塞连接的复制，取消握手状态
        cancelReplicationHandshake();
    }

    /* Bulk transfer I/O timeout? */
    // 如果正在从主节点接受RDB文件而且超时
    if (server.masterhost && server.repl_state == REPL_STATE_TRANSFER &&
        (time(NULL)-server.repl_transfer_lastio) > server.repl_timeout)
    {
        serverLog(LL_WARNING,"Timeout receiving bulk data from MASTER... If the problem persists try to set the 'repl-timeout' parameter in redis.conf to a larger value.");
        // 中断一个非阻塞复制正在进行
        cancelReplicationHandshake();
    }

    /* Timed out master when we are an already connected slave? */
    // 每隔10s，主节点想从节点发送PING命令，从节点每隔1s，向主节点报告复制偏移量
    // 当我们已经和主节点连接上，但是很久没有收到PING命令，则连接超时
    if (server.masterhost && server.repl_state == REPL_STATE_CONNECTED &&
        (time(NULL)-server.master->lastinteraction) > server.repl_timeout)
    {
        serverLog(LL_WARNING,"MASTER timeout: no data nor PING received...");
        // 释放主节点
        freeClient(server.master);
    }

    /* Check if we should connect to a MASTER */
    // 如果处于要必须连接主节点的状态，尝试连接
    if (server.repl_state == REPL_STATE_CONNECT) {
        serverLog(LL_NOTICE,"Connecting to MASTER %s:%d",
            server.masterhost, server.masterport);
        // 以非阻塞的方式连接主节点
        if (connectWithMaster() == C_OK) {
            serverLog(LL_NOTICE,"MASTER <-> REPLICA sync started");
        }
    }

    /* Send ACK to master from time to time.
     * Note that we do not send periodic acks to masters that don't
     * support PSYNC and replication offsets. */
    // 定期发送ack给主节点，旧版本的Redis除外
    if (server.masterhost && server.master &&
        !(server.master->flags & CLIENT_PRE_PSYNC))
        replicationSendAck();

    /* If we have attached slaves, PING them from time to time.
     * So slaves can implement an explicit timeout to masters, and will
     * be able to detect a link disconnection even if the TCP connection
     * will not actually go down. */
    // 如果服务器有从节点client，定期发送PING命令
    // 所以从节点可以实现一个显式的timeout去判断和主节点的超时连接，即使TCP连接不会中断，也能察觉一个连接的重新连接
    listIter li;
    listNode *ln;
    robj *ping_argv[1];

    /* First, send PING according to ping_slave_period. */
    // 首先，根据当前节点发送PING命令给从节点的频率发送PING命令
    // 如果当前节点是某以节点的 主节点 ，那么发送PING给从节点
    if ((replication_cron_loops % server.repl_ping_slave_period) == 0 &&
        listLength(server.slaves))
    {
        /* Note that we don't send the PING if the clients are paused during
         * a Redis Cluster manual failover: the PING we send will otherwise
         * alter the replication offsets of master and slave, and will no longer
         * match the one stored into 'mf_master_offset' state. */
        int manual_failover_in_progress =
            server.cluster_enabled &&
            server.cluster->mf_end &&
            clientsArePaused();

        if (!manual_failover_in_progress) {
            // 创建PING命令对象
            ping_argv[0] = createStringObject("PING",4);
            // 将PING发送给从服务器
            replicationFeedSlaves(server.slaves, server.slaveseldb,
                ping_argv, 1);
            decrRefCount(ping_argv[0]);
        }
    }

    /* Second, send a newline to all the slaves in pre-synchronization
     * stage, that is, slaves waiting for the master to create the RDB file.
     *
     * Also send the a newline to all the chained slaves we have, if we lost
     * connection from our master, to keep the slaves aware that their
     * master is online. This is needed since sub-slaves only receive proxied
     * data from top-level masters, so there is no explicit pinging in order
     * to avoid altering the replication offsets. This special out of band
     * pings (newlines) can be sent, they will have no effect in the offset.
     *
     * The newline will be ignored by the slave but will refresh the
     * last interaction timer preventing a timeout. In this case we ignore the
     * ping period and refresh the connection once per second since certain
     * timeouts are set at a few seconds (example: PSYNC response). */
    // 其次，向正等待主节点创建RDB文件的从节点发送换行符'\n'。这个换行符会被从节点忽略掉，但是会更新最近一个使用IO的时间，防止超时
    // 在这种情况下，我们忽略ping周期，并且每秒刷新一次连接，因为某些超时设置为几秒钟（例如：PSYNC response）。
    listRewind(server.slaves,&li);
    // 遍历所有的从节点
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        // 如果当前从节点的复制状态处于等待RDB文件被主节点创建的状态，无论是等待BGSAVE开始还是正在执行BGSAVE等待结束
        // 并且执行RDB的类型不是无盘传输
        int is_presync =
            (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START ||
            (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END &&
             server.rdb_child_type != RDB_CHILD_TYPE_SOCKET));

        if (is_presync) {
            // 都要发送一个'\n'，保持活跃主从连接的状态
            connWrite(slave->conn, "\n", 1);
        }
    }

    /* Disconnect timedout slaves. */
    // 断开超时的从节点服务器
    if (listLength(server.slaves)) {
        listIter li;
        listNode *ln;

        listRewind(server.slaves,&li);
        // 遍历所有的从节点
        while((ln = listNext(&li))) {
            client *slave = ln->value;

            // 跳过没有完成RDB文件传输的从节点
            if (slave->replstate != SLAVE_STATE_ONLINE) continue;
            // 如果是旧版本的Redis，也跳过
            if (slave->flags & CLIENT_PRE_PSYNC) continue;
            // 如果接收ack命令所用的时间超时则释放从节点client
            if ((server.unixtime - slave->repl_ack_time) > server.repl_timeout)
            {
                serverLog(LL_WARNING, "Disconnecting timedout replica: %s",
                    replicationGetSlaveName(slave));
                freeClient(slave);
            }
        }
    }

    /* If this is a master without attached slaves and there is a replication
     * backlog active, in order to reclaim memory we can free it after some
     * (configured) time. Note that this cannot be done for slaves: slaves
     * without sub-slaves attached should still accumulate data into the
     * backlog, in order to reply to PSYNC queries if they are turned into
     * masters after a failover. */
    // 如果没有一个从属的从节点，释放backlog
    if (listLength(server.slaves) == 0 && server.repl_backlog_time_limit &&
        server.repl_backlog && server.masterhost == NULL)
    {
        // 计算从节点为0的时间
        time_t idle = server.unixtime - server.repl_no_slaves_since;

        // 超过repl_backlog_time_limit限制则释放backlog
        if (idle > server.repl_backlog_time_limit) {
            /* When we free the backlog, we always use a new
             * replication ID and clear the ID2. This is needed
             * because when there is no backlog, the master_repl_offset
             * is not updated, but we would still retain our replication
             * ID, leading to the following problem:
             *
             * 1. We are a master instance.
             * 2. Our slave is promoted to master. It's repl-id-2 will
             *    be the same as our repl-id.
             * 3. We, yet as master, receive some updates, that will not
             *    increment the master_repl_offset.
             * 4. Later we are turned into a slave, connect to the new
             *    master that will accept our PSYNC request by second
             *    replication ID, but there will be data inconsistency
             *    because we received writes. */
            changeReplicationId();
            clearReplicationId2();
            freeReplicationBacklog();
            serverLog(LL_NOTICE,
                "Replication backlog freed after %d seconds "
                "without connected replicas.",
                (int) server.repl_backlog_time_limit);
        }
    }

    /* If AOF is disabled and we no longer have attached slaves, we can
     * free our Replication Script Cache as there is no need to propagate
     * EVALSHA at all. */
    // 如果AOF处于关闭状态，而且没有从属的从节点，释放复制脚本缓存，因为没有传播EVALSHA的必要
    if (listLength(server.slaves) == 0 &&
        server.aof_state == AOF_OFF &&
        listLength(server.repl_scriptcache_fifo) != 0)
    {
        replicationScriptCacheFlush();
    }

    /* Start a BGSAVE good for replication if we have slaves in
     * WAIT_BGSAVE_START state.
     *
     * In case of diskless replication, we make sure to wait the specified
     * number of seconds (according to configuration) so that other slaves
     * have the time to arrive before we start streaming. */
    // 如果有从节点处于WAIT_BGSAVE_START状态，则为了复制操作开始执行一个BGSAVE
    // 无盘复制的情况下，根据配置确定等待指定的时间以便其他的从节点有时间在开始复制流之前连接进来
    // 如果现在没有子进程在进行RDB和AOF持久化
    if (!hasActiveChildProcess()) {
        time_t idle, max_idle = 0;
        int slaves_waiting = 0;
        int mincapa = -1;
        listNode *ln;
        listIter li;

        listRewind(server.slaves,&li);
        // 遍历所有的从节点，记录等待BGSAVE的开始的从节点个数
        while((ln = listNext(&li))) {
            client *slave = ln->value;
            // 如果从节点的复制状态处于等待BGSAVE的开始
            if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
                // 计算当前从节点空转时间
                idle = server.unixtime - slave->lastinteraction;
                // 记录最大的空转时间
                if (idle > max_idle) max_idle = idle;
                // 计算等待BGSAVE开始的从节点的个数
                slaves_waiting++;
                // 获取当前从节点的功能capa
                mincapa = (mincapa == -1) ? slave->slave_capa :
                                            (mincapa & slave->slave_capa);
            }
        }

        // 如果至少有一个等待BGSAVE的开始，并且服务器不支持无盘复制或最大空转时间超过无盘复制的延迟时限
        if (slaves_waiting &&
            (!server.repl_diskless_sync ||
             max_idle > server.repl_diskless_sync_delay))
        {
            /* Start the BGSAVE. The called function may start a
             * BGSAVE with socket target or disk target depending on the
             * configuration and slaves capabilities. */
            // 开始执行BGSAVE，BGSAVE将RDB文件写到socket还是磁盘上，取决于配置和从节点的capa
            startBgsaveForReplication(mincapa);
        }
    }

    /* Remove the RDB file used for replication if Redis is not running
     * with any persistence. */
    // 如果Redis没有持久性运行，则删除用于复制的RDB文件。
    removeRDBUsedToSyncReplicas();

    /* Refresh the number of slaves with lag <= min-slaves-max-lag. */
    // 更新延迟至log小于min-slaves-max-lag的从服务器数量
    refreshGoodSlavesCount();
    // 更新cron函数执行的次数，因为它是静态的
    replication_cron_loops++; /* Incremented with frequency 1 HZ. */
}
