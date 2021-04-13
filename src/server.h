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

#ifndef __REDIS_H
#define __REDIS_H

#include "fmacros.h"
#include "config.h"
#include "solarisfixes.h"
#include "rio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <syslog.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <lua.h>
#include <signal.h>

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>
#endif

typedef long long mstime_t; /* millisecond time type. */
typedef long long ustime_t; /* microsecond time type. */

#include "ae.h"      /* Event driven programming library */
#include "sds.h"     /* Dynamic safe strings */
#include "dict.h"    /* Hash tables */
#include "adlist.h"  /* Linked lists */
#include "zmalloc.h" /* total memory usage aware version of malloc/free */
#include "anet.h"    /* Networking the easy way */
#include "ziplist.h" /* Compact list data structure */
#include "intset.h"  /* Compact integer set structure */
#include "version.h" /* Version macro */
#include "util.h"    /* Misc functions useful in many places */
#include "latency.h" /* Latency monitor API */
#include "sparkline.h" /* ASCII graphs API */
#include "quicklist.h"  /* Lists are encoded as linked lists of
                           N-elements flat arrays */
#include "rax.h"     /* Radix tree */
#include "connection.h" /* Connection abstraction */

#define REDISMODULE_CORE 1
#include "redismodule.h"    /* Redis modules API defines. */

/* Following includes allow test functions to be called from Redis main() */
#include "zipmap.h"
#include "sha1.h"
#include "endianconv.h"
#include "crc64.h"

/* Error codes */
#define C_OK                    0
#define C_ERR                   -1

/* Static server configuration */
//默认的频率：每10秒执行一次
#define CONFIG_DEFAULT_HZ        10             /* Time interrupt calls/sec. */
#define CONFIG_MIN_HZ            1
#define CONFIG_MAX_HZ            500
#define MAX_CLIENTS_PER_CLOCK_TICK 200          /* HZ is adapted based on that. */
#define CONFIG_MAX_LINE    1024
#define CRON_DBS_PER_CALL 16
#define NET_MAX_WRITES_PER_EVENT (1024*64)
#define PROTO_SHARED_SELECT_CMDS 10
#define OBJ_SHARED_INTEGERS 10000
#define OBJ_SHARED_BULKHDR_LEN 32
#define LOG_MAX_LEN    1024 /* Default maximum length of syslog messages.*/
#define AOF_REWRITE_ITEMS_PER_CMD 64
#define AOF_READ_DIFF_INTERVAL_BYTES (1024*10)
#define CONFIG_AUTHPASS_MAX_LEN 512
#define CONFIG_RUN_ID_SIZE 40
#define RDB_EOF_MARK_SIZE 40
#define CONFIG_REPL_BACKLOG_MIN_SIZE (1024*16)          /* 16k */
#define CONFIG_BGSAVE_RETRY_DELAY 5 /* Wait a few secs before trying again. */
#define CONFIG_DEFAULT_PID_FILE "/var/run/redis.pid"
#define CONFIG_DEFAULT_CLUSTER_CONFIG_FILE "nodes.conf"
#define CONFIG_DEFAULT_UNIX_SOCKET_PERM 0
#define CONFIG_DEFAULT_LOGFILE ""
#define NET_IP_STR_LEN 46 /* INET6_ADDRSTRLEN is 46, but we need to be sure */
#define NET_PEER_ID_LEN (NET_IP_STR_LEN+32) /* Must be enough for ip:port */
#define CONFIG_BINDADDR_MAX 16
#define CONFIG_MIN_RESERVED_FDS 32

#define ACTIVE_EXPIRE_CYCLE_SLOW 0
#define ACTIVE_EXPIRE_CYCLE_FAST 1

/* Children process will exit with this status code to signal that the
 * process terminated without an error: this is useful in order to kill
 * a saving child (RDB or AOF one), without triggering in the parent the
 * write protection that is normally turned on on write errors.
 * Usually children that are terminated with SIGUSR1 will exit with this
 * special code. */
#define SERVER_CHILD_NOERROR_RETVAL    255

/* Instantaneous metrics tracking. */
#define STATS_METRIC_SAMPLES 16     /* Number of samples per metric. */
#define STATS_METRIC_COMMAND 0      /* Number of commands executed. */
#define STATS_METRIC_NET_INPUT 1    /* Bytes read to network .*/
#define STATS_METRIC_NET_OUTPUT 2   /* Bytes written to network. */
#define STATS_METRIC_COUNT 3

/* Protocol and I/O related defines */
#define PROTO_MAX_QUERYBUF_LEN  (1024*1024*1024) /* 1GB max query buffer. */
//IO大小
#define PROTO_IOBUF_LEN         (1024*16)  /* Generic I/O buffer size */
//16k输出缓冲区的大小
#define PROTO_REPLY_CHUNK_BYTES (16*1024) /* 16k output buffer */
#define PROTO_INLINE_MAX_SIZE   (1024*64) /* Max size of inline reads */
#define PROTO_MBULK_BIG_ARG     (1024*32)
//long类型转换为字符串所能使用的最大字节数
#define LONG_STR_SIZE      21          /* Bytes needed for long -> str + '\0' */
#define REDIS_AUTOSYNC_BYTES (1024*1024*32) /* fdatasync every 32MB */

#define LIMIT_PENDING_QUERYBUF (4*1024*1024) /* 4mb */

/* When configuring the server eventloop, we setup it so that the total number
 * of file descriptors we can handle are server.maxclients + RESERVED_FDS +
 * a few more to stay safe. Since RESERVED_FDS defaults to 32, we add 96
 * in order to make sure of not over provisioning more than 128 fds. */
#define CONFIG_FDSET_INCR (CONFIG_MIN_RESERVED_FDS+96)

/* OOM Score Adjustment classes. */
#define CONFIG_OOM_MASTER 0
#define CONFIG_OOM_REPLICA 1
#define CONFIG_OOM_BGCHILD 2
#define CONFIG_OOM_COUNT 3

extern int configOOMScoreAdjValuesDefaults[CONFIG_OOM_COUNT];

/* Hash table parameters */
#define HASHTABLE_MIN_FILL        10      /* Minimal hash table fill 10% */

/* Command flags. Please check the command table defined in the server.c file
 * for more information about the meaning of every flag. */
#define CMD_WRITE (1ULL<<0)            /* "write" flag */
#define CMD_READONLY (1ULL<<1)         /* "read-only" flag */
#define CMD_DENYOOM (1ULL<<2)          /* "use-memory" flag */
#define CMD_MODULE (1ULL<<3)           /* Command exported by module. */
#define CMD_ADMIN (1ULL<<4)            /* "admin" flag */
#define CMD_PUBSUB (1ULL<<5)           /* "pub-sub" flag */
#define CMD_NOSCRIPT (1ULL<<6)         /* "no-script" flag */
#define CMD_RANDOM (1ULL<<7)           /* "random" flag */
#define CMD_SORT_FOR_SCRIPT (1ULL<<8)  /* "to-sort" flag */
#define CMD_LOADING (1ULL<<9)          /* "ok-loading" flag */
#define CMD_STALE (1ULL<<10)           /* "ok-stale" flag */
#define CMD_SKIP_MONITOR (1ULL<<11)    /* "no-monitor" flag */
#define CMD_SKIP_SLOWLOG (1ULL<<12)    /* "no-slowlog" flag */
#define CMD_ASKING (1ULL<<13)          /* "cluster-asking" flag */
#define CMD_FAST (1ULL<<14)            /* "fast" flag */
#define CMD_NO_AUTH (1ULL<<15)         /* "no-auth" flag */

/* Command flags used by the module system. */
#define CMD_MODULE_GETKEYS (1ULL<<16)  /* Use the modules getkeys interface. */
#define CMD_MODULE_NO_CLUSTER (1ULL<<17) /* Deny on Redis Cluster. */

/* Command flags that describe ACLs categories. */
#define CMD_CATEGORY_KEYSPACE (1ULL<<18)
#define CMD_CATEGORY_READ (1ULL<<19)
#define CMD_CATEGORY_WRITE (1ULL<<20)
#define CMD_CATEGORY_SET (1ULL<<21)
#define CMD_CATEGORY_SORTEDSET (1ULL<<22)
#define CMD_CATEGORY_LIST (1ULL<<23)
#define CMD_CATEGORY_HASH (1ULL<<24)
#define CMD_CATEGORY_STRING (1ULL<<25)
#define CMD_CATEGORY_BITMAP (1ULL<<26)
#define CMD_CATEGORY_HYPERLOGLOG (1ULL<<27)
#define CMD_CATEGORY_GEO (1ULL<<28)
#define CMD_CATEGORY_STREAM (1ULL<<29)
#define CMD_CATEGORY_PUBSUB (1ULL<<30)
#define CMD_CATEGORY_ADMIN (1ULL<<31)
#define CMD_CATEGORY_FAST (1ULL<<32)
#define CMD_CATEGORY_SLOW (1ULL<<33)
#define CMD_CATEGORY_BLOCKING (1ULL<<34)
#define CMD_CATEGORY_DANGEROUS (1ULL<<35)
#define CMD_CATEGORY_CONNECTION (1ULL<<36)
#define CMD_CATEGORY_TRANSACTION (1ULL<<37)
#define CMD_CATEGORY_SCRIPTING (1ULL<<38)

/* AOF states */
#define AOF_OFF 0             /* AOF is off */
#define AOF_ON 1              /* AOF is on */
#define AOF_WAIT_REWRITE 2    /* AOF waits rewrite to start appending */

/* Client flags */
// client是从节点服务器
#define CLIENT_SLAVE (1<<0)   /* This client is a repliaca */
// client是主节点服务器
#define CLIENT_MASTER (1<<1)  /* This client is a master */
// client是一个从节点监控器
#define CLIENT_MONITOR (1<<2) /* This client is a slave monitor, see MONITOR */
// client处于事务环境中
#define CLIENT_MULTI (1<<3)   /* This client is in a MULTI context */
#define CLIENT_BLOCKED (1<<4) /* The client is waiting in a blocking operation */
// 监视的键被修改，EXEC执行失败
#define CLIENT_DIRTY_CAS (1<<5) /* Watched keys modified. EXEC will fail. */
// 发送回复后要关闭client，当执行client kill命令等
#define CLIENT_CLOSE_AFTER_REPLY (1<<6) /* Close after writing entire reply. */
// 不被阻塞的client，保存在unblocked_clients中
#define CLIENT_UNBLOCKED (1<<7) /* This client was unblocked and is stored in
                                  server.unblocked_clients */
// 表示该客户端是一个专门处理lua脚本的伪客户端
#define CLIENT_LUA (1<<8) /* This is a non connected client used by Lua */
// 发送了ASKING命令
#define CLIENT_ASKING (1<<9)     /* Client issued the ASKING command */
// 正要关闭的client
#define CLIENT_CLOSE_ASAP (1<<10)/* Close this client ASAP */
#define CLIENT_UNIX_SOCKET (1<<11) /* Client connected via Unix domain socket */
// 命令入队时错误
#define CLIENT_DIRTY_EXEC (1<<12)  /* EXEC will fail for errors while queueing */
// 强制进行回复
#define CLIENT_MASTER_FORCE_REPLY (1<<13)  /* Queue replies even if is master */
// 强制将节点传播到AOF中
#define CLIENT_FORCE_AOF (1<<14)   /* Force AOF propagation of current cmd. */
// 强制将命令传播到从节点
#define CLIENT_FORCE_REPL (1<<15)  /* Force replication of current cmd. */
// Redis 2.8版本以前只有SYNC命令，该这个宏来标记client的版本以便选择不同的同步命令
#define CLIENT_PRE_PSYNC (1<<16)   /* Instance don't understand PSYNC. */
#define CLIENT_READONLY (1<<17)    /* Cluster client is in read-only state. */
#define CLIENT_PUBSUB (1<<18)      /* Client is in Pub/Sub mode. */
#define CLIENT_PREVENT_AOF_PROP (1<<19)  /* Don't propagate to AOF. */
#define CLIENT_PREVENT_REPL_PROP (1<<20)  /* Don't propagate to slaves. */
#define CLIENT_PREVENT_PROP (CLIENT_PREVENT_AOF_PROP|CLIENT_PREVENT_REPL_PROP)
// client还有输出的数据，但是没有设置写处理程序
#define CLIENT_PENDING_WRITE (1<<21) /* Client has output to send but a write
                                        handler is yet not installed. */
// 控制服务器是否回复客户端，默认为开启ON，
// 设置为不开启，服务器不会回复client命令
#define CLIENT_REPLY_OFF (1<<22)   /* Don't send replies to client. */
// 为下一条命令设置CLIENT_REPLY_SKIP标志
#define CLIENT_REPLY_SKIP_NEXT (1<<23)  /* Set CLIENT_REPLY_SKIP for next cmd */
// 设置为跳过该条回复，服务器会跳过这条命令的回复
#define CLIENT_REPLY_SKIP (1<<24)  /* Don't send just this reply. */
#define CLIENT_LUA_DEBUG (1<<25)  /* Run EVAL in debug mode. */
#define CLIENT_LUA_DEBUG_SYNC (1<<26)  /* EVAL debugging without fork() */
#define CLIENT_MODULE (1<<27) /* Non connected client used by some module. */
#define CLIENT_PROTECTED (1<<28) /* Client should not be freed for now. */
#define CLIENT_PENDING_READ (1<<29) /* The client has pending reads and was put
                                       in the list of clients we can read
                                       from. */
#define CLIENT_PENDING_COMMAND (1<<30) /* Used in threaded I/O to signal after
                                          we return single threaded that the
                                          client has already pending commands
                                          to be executed. */
#define CLIENT_TRACKING (1ULL<<31) /* Client enabled keys tracking in order to
                                   perform client side caching. */
#define CLIENT_TRACKING_BROKEN_REDIR (1ULL<<32) /* Target client is invalid. */
#define CLIENT_TRACKING_BCAST (1ULL<<33) /* Tracking in BCAST mode. */
#define CLIENT_TRACKING_OPTIN (1ULL<<34)  /* Tracking in opt-in mode. */
#define CLIENT_TRACKING_OPTOUT (1ULL<<35) /* Tracking in opt-out mode. */
#define CLIENT_TRACKING_CACHING (1ULL<<36) /* CACHING yes/no was given,
                                              depending on optin/optout mode. */
#define CLIENT_TRACKING_NOLOOP (1ULL<<37) /* Don't send invalidation messages
                                             about writes performed by myself.*/
#define CLIENT_IN_TO_TABLE (1ULL<<38) /* This client is in the timeout table. */
#define CLIENT_PROTOCOL_ERROR (1ULL<<39) /* Protocol error chatting with it. */
#define CLIENT_CLOSE_AFTER_COMMAND (1ULL<<40) /* Close after executing commands
                                               * and writing entire reply. */

/* Client block type (btype field in client structure)
 * if CLIENT_BLOCKED flag is set. */
#define BLOCKED_NONE 0    /* Not blocked, no CLIENT_BLOCKED flag set. */
#define BLOCKED_LIST 1    /* BLPOP & co. */
#define BLOCKED_WAIT 2    /* WAIT for synchronous replication. */
#define BLOCKED_MODULE 3  /* Blocked by a loadable module. */
#define BLOCKED_STREAM 4  /* XREAD. */
#define BLOCKED_ZSET 5    /* BZPOP et al. */
#define BLOCKED_NUM 6     /* Number of blocked states. */

/* Client request types */
#define PROTO_REQ_INLINE 1
#define PROTO_REQ_MULTIBULK 2

/* Client classes for client limits, currently used only for
 * the max-client-output-buffer limit implementation. */
#define CLIENT_TYPE_NORMAL 0 /* Normal req-reply clients + MONITORs */
#define CLIENT_TYPE_SLAVE 1  /* Slaves. */
#define CLIENT_TYPE_PUBSUB 2 /* Clients subscribed to PubSub channels. */
#define CLIENT_TYPE_MASTER 3 /* Master. */
#define CLIENT_TYPE_COUNT 4  /* Total number of client types. */
#define CLIENT_TYPE_OBUF_COUNT 3 /* Number of clients to expose to output
                                    buffer configuration. Just the first
                                    three: normal, slave, pubsub. */

/* Slave replication state. Used in server.repl_state for slaves to remember
 * what to do next. */
// replication关闭状态
#define REPL_STATE_NONE 0 /* No active replication */
// 必须重新连接主节点
#define REPL_STATE_CONNECT 1 /* Must connect to master */
// 处于和主节点正在连接的状态
#define REPL_STATE_CONNECTING 2 /* Connecting to master */
/* --- Handshake states, must be ordered --- */
// 握手状态，有序
// 等待主节点回复PING命令一个PONG
#define REPL_STATE_RECEIVE_PONG 3 /* Wait for PING reply */
// 发送认证命令AUTH给主节点
#define REPL_STATE_SEND_AUTH 4 /* Send AUTH to master */
// 设置状态为等待接受认证回复
#define REPL_STATE_RECEIVE_AUTH 5 /* Wait for AUTH reply */
// 发送从节点的端口号
#define REPL_STATE_SEND_PORT 6 /* Send REPLCONF listening-port */
// 接受一个从节点监听端口号
#define REPL_STATE_RECEIVE_PORT 7 /* Wait for REPLCONF reply */
#define REPL_STATE_SEND_IP 8 /* Send REPLCONF ip-address */
#define REPL_STATE_RECEIVE_IP 9 /* Wait for REPLCONF reply */
// 发送一个
#define REPL_STATE_SEND_CAPA 10 /* Send REPLCONF capa */
#define REPL_STATE_RECEIVE_CAPA 11 /* Wait for REPLCONF reply */
#define REPL_STATE_SEND_PSYNC 12 /* Send PSYNC */
// 等待一个PSYNC回复
#define REPL_STATE_RECEIVE_PSYNC 13 /* Wait for PSYNC reply */
/* --- End of handshake states --- */
// 正从主节点接受RDB文件
#define REPL_STATE_TRANSFER 14 /* Receiving .rdb from master */
// 和主节点保持连接
#define REPL_STATE_CONNECTED 15 /* Connected to master */

/* State of slaves from the POV of the master. Used in client->replstate.
 * In SEND_BULK and ONLINE state the slave receives new updates
 * in its output queue. In the WAIT_BGSAVE states instead the server is waiting
 * to start the next background saving in order to send updates to it. */
//从服务器节点等待BGSAVE节点的开始，因此要生成一个新的RDB文件
#define SLAVE_STATE_WAIT_BGSAVE_START 6 /* We need to produce a new RDB file. */
//已经创建子进程执行写RDB操作，等待完成
#define SLAVE_STATE_WAIT_BGSAVE_END 7 /* Waiting RDB file creation to finish. */
//正在发送RDB文件给从节点
#define SLAVE_STATE_SEND_BULK 8 /* Sending RDB file to slave. */
//RDB文件传输完成
#define SLAVE_STATE_ONLINE 9 /* RDB file transmitted, sending just updates. */

/* Slave capabilities. */
#define SLAVE_CAPA_NONE 0
//能够解析出RDB文件的EOF流格式
#define SLAVE_CAPA_EOF (1<<0)    /* Can parse the RDB EOF streaming format. */
#define SLAVE_CAPA_PSYNC2 (1<<1) /* Supports PSYNC2 protocol. */

/* Synchronous read timeout - slave side */
#define CONFIG_REPL_SYNCIO_TIMEOUT 5

/* List related stuff */
#define LIST_HEAD 0 //列表头
#define LIST_TAIL 1 //列表尾部
#define ZSET_MIN 0
#define ZSET_MAX 1

/* Sort operations */
#define SORT_OP_GET 0

/* Log levels */
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2      //日志通知
#define LL_WARNING 3     //日志警告
#define LL_RAW (1<<10) /* Modifier to log without timestamp */

/* Supervision options */
#define SUPERVISED_NONE 0
#define SUPERVISED_AUTODETECT 1
#define SUPERVISED_SYSTEMD 2
#define SUPERVISED_UPSTART 3

/* Anti-warning macro... */
#define UNUSED(V) ((void) V)

#define ZSKIPLIST_MAXLEVEL 32 /* Should be enough for 2^64 elements */
#define ZSKIPLIST_P 0.25      /* Skiplist P = 1/4 */

/* Append only defines */
#define AOF_FSYNC_NO 0          //不执行同步，由系统执行
#define AOF_FSYNC_ALWAYS 1      //每次写入都执行同步
#define AOF_FSYNC_EVERYSEC 2    //每秒同步一次

/* Replication diskless load defines */
#define REPL_DISKLESS_LOAD_DISABLED 0
#define REPL_DISKLESS_LOAD_WHEN_DB_EMPTY 1
#define REPL_DISKLESS_LOAD_SWAPDB 2

/* TLS Client Authentication */
#define TLS_CLIENT_AUTH_NO 0
#define TLS_CLIENT_AUTH_YES 1
#define TLS_CLIENT_AUTH_OPTIONAL 2

/* Sets operations codes */
#define SET_OP_UNION 0
#define SET_OP_DIFF 1
#define SET_OP_INTER 2

/* Redis maxmemory strategies. Instead of using just incremental number
 * for this defines, we use a set of flags so that testing for certain
 * properties common to multiple policies is faster. */
#define MAXMEMORY_FLAG_LRU (1<<0)
#define MAXMEMORY_FLAG_LFU (1<<1)
#define MAXMEMORY_FLAG_ALLKEYS (1<<2)
#define MAXMEMORY_FLAG_NO_SHARED_INTEGERS \
    (MAXMEMORY_FLAG_LRU|MAXMEMORY_FLAG_LFU)

#define MAXMEMORY_VOLATILE_LRU ((0<<8)|MAXMEMORY_FLAG_LRU)
#define MAXMEMORY_VOLATILE_LFU ((1<<8)|MAXMEMORY_FLAG_LFU)
#define MAXMEMORY_VOLATILE_TTL (2<<8)
#define MAXMEMORY_VOLATILE_RANDOM (3<<8)
#define MAXMEMORY_ALLKEYS_LRU ((4<<8)|MAXMEMORY_FLAG_LRU|MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_ALLKEYS_LFU ((5<<8)|MAXMEMORY_FLAG_LFU|MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_ALLKEYS_RANDOM ((6<<8)|MAXMEMORY_FLAG_ALLKEYS)
#define MAXMEMORY_NO_EVICTION (7<<8)

/* Units */
#define UNIT_SECONDS 0          //单位是秒
#define UNIT_MILLISECONDS 1     //单位是毫秒

/* SHUTDOWN flags */
#define SHUTDOWN_NOFLAGS 0      /* 不指定标志 No flags. */
#define SHUTDOWN_SAVE 1         /* 指定停机保存标志即使配置了SHUTDOWN_NOSAVE标志 Force SAVE on SHUTDOWN even if no save
                                   points are configured. */
#define SHUTDOWN_NOSAVE 2       /* 指定停机不保存标志 Don't SAVE on SHUTDOWN. */

/* Command call flags, see call() function */
#define CMD_CALL_NONE 0
#define CMD_CALL_SLOWLOG (1<<0)
#define CMD_CALL_STATS (1<<1)
#define CMD_CALL_PROPAGATE_AOF (1<<2)
#define CMD_CALL_PROPAGATE_REPL (1<<3)
#define CMD_CALL_PROPAGATE (CMD_CALL_PROPAGATE_AOF|CMD_CALL_PROPAGATE_REPL)
#define CMD_CALL_FULL (CMD_CALL_SLOWLOG | CMD_CALL_STATS | CMD_CALL_PROPAGATE)
#define CMD_CALL_NOWRAP (1<<4)  /* Don't wrap also propagate array into
                                   MULTI/EXEC: the caller will handle it.  */

/* Command propagation flags, see propagate() function */
#define PROPAGATE_NONE 0
#define PROPAGATE_AOF 1
#define PROPAGATE_REPL 2

/* RDB active child save type. */
#define RDB_CHILD_TYPE_NONE 0
#define RDB_CHILD_TYPE_DISK 1     /* RDB is written to disk. */
#define RDB_CHILD_TYPE_SOCKET 2   /* RDB is written to slave socket. */

/* Keyspace changes notification classes. Every class is associated with a
 * character for configuration purposes. */
// 键空间通知的类型，每个类型都关联着一个有目的的字符
#define NOTIFY_KEYSPACE (1<<0)    /* K */   //键空间
#define NOTIFY_KEYEVENT (1<<1)    /* E */   //键事件
#define NOTIFY_GENERIC (1<<2)     /* g */   //通用无类型通知
#define NOTIFY_STRING (1<<3)      /* $ */   //字符串类型键通知
#define NOTIFY_LIST (1<<4)        /* l */   //列表键通知
#define NOTIFY_SET (1<<5)         /* s */   //集合键通知
#define NOTIFY_HASH (1<<6)        /* h */   //哈希键通知
#define NOTIFY_ZSET (1<<7)        /* z */   //有序集合键通知
#define NOTIFY_EXPIRED (1<<8)     /* x */   //过期有关的键通知
#define NOTIFY_EVICTED (1<<9)     /* e */   //驱逐有关的键通知
#define NOTIFY_STREAM (1<<10)     /* t */
#define NOTIFY_KEY_MISS (1<<11)   /* m (Note: This one is excluded from NOTIFY_ALL on purpose) */
#define NOTIFY_LOADED (1<<12)     /* module only key space notification, indicate a key loaded from rdb */
#define NOTIFY_ALL (NOTIFY_GENERIC | NOTIFY_STRING | NOTIFY_LIST | NOTIFY_SET | NOTIFY_HASH | NOTIFY_ZSET | NOTIFY_EXPIRED | NOTIFY_EVICTED | NOTIFY_STREAM) /* A flag */

/* Get the first bind addr or NULL */
#define NET_FIRST_BIND_ADDR (server.bindaddr_count ? server.bindaddr[0] : NULL)

/* Using the following macro you can run code inside serverCron() with the
 * specified period, specified in milliseconds.
 * The actual resolution depends on server.hz. */
// 用这个宏来控制if条件中的代码执行的频率
#define run_with_period(_ms_) if ((_ms_ <= 1000/server.hz) || !(server.cronloops%((_ms_)/(1000/server.hz))))

/* We can print the stacktrace, so our assert is defined this way: */
#define serverAssertWithInfo(_c,_o,_e) ((_e)?(void)0 : (_serverAssertWithInfo(_c,_o,#_e,__FILE__,__LINE__),_exit(1)))
#define serverAssert(_e) ((_e)?(void)0 : (_serverAssert(#_e,__FILE__,__LINE__),_exit(1)))
#define serverPanic(...) _serverPanic(__FILE__,__LINE__,__VA_ARGS__),_exit(1)

/*-----------------------------------------------------------------------------
 * Data types
 *----------------------------------------------------------------------------*/

/* A redis object, that is a type able to hold a string / list / set */

/* The actual Redis Object */
#define OBJ_STRING 0    /* String object. */
#define OBJ_LIST 1      /* List object. */
#define OBJ_SET 2       /* Set object. */
#define OBJ_ZSET 3      /* Sorted set object. */
#define OBJ_HASH 4      /* Hash object. */

/* The "module" object type is a special one that signals that the object
 * is one directly managed by a Redis module. In this case the value points
 * to a moduleValue struct, which contains the object value (which is only
 * handled by the module itself) and the RedisModuleType struct which lists
 * function pointers in order to serialize, deserialize, AOF-rewrite and
 * free the object.
 *
 * Inside the RDB file, module types are encoded as OBJ_MODULE followed
 * by a 64 bit module type ID, which has a 54 bits module-specific signature
 * in order to dispatch the loading to the right module, plus a 10 bits
 * encoding version. */
#define OBJ_MODULE 5    /* Module object. */
#define OBJ_STREAM 6    /* Stream object. */

/* Extract encver / signature from a module type ID. */
#define REDISMODULE_TYPE_ENCVER_BITS 10
#define REDISMODULE_TYPE_ENCVER_MASK ((1<<REDISMODULE_TYPE_ENCVER_BITS)-1)
#define REDISMODULE_TYPE_ENCVER(id) (id & REDISMODULE_TYPE_ENCVER_MASK)
#define REDISMODULE_TYPE_SIGN(id) ((id & ~((uint64_t)REDISMODULE_TYPE_ENCVER_MASK)) >>REDISMODULE_TYPE_ENCVER_BITS)

/* Bit flags for moduleTypeAuxSaveFunc */
#define REDISMODULE_AUX_BEFORE_RDB (1<<0)
#define REDISMODULE_AUX_AFTER_RDB (1<<1)

struct RedisModule;
struct RedisModuleIO;
struct RedisModuleDigest;
struct RedisModuleCtx;
struct redisObject;

/* Each module type implementation should export a set of methods in order
 * to serialize and deserialize the value in the RDB file, rewrite the AOF
 * log, create the digest for "DEBUG DIGEST", and free the value when a key
 * is deleted. */
typedef void *(*moduleTypeLoadFunc)(struct RedisModuleIO *io, int encver);
typedef void (*moduleTypeSaveFunc)(struct RedisModuleIO *io, void *value);
typedef int (*moduleTypeAuxLoadFunc)(struct RedisModuleIO *rdb, int encver, int when);
typedef void (*moduleTypeAuxSaveFunc)(struct RedisModuleIO *rdb, int when);
typedef void (*moduleTypeRewriteFunc)(struct RedisModuleIO *io, struct redisObject *key, void *value);
typedef void (*moduleTypeDigestFunc)(struct RedisModuleDigest *digest, void *value);
typedef size_t (*moduleTypeMemUsageFunc)(const void *value);
typedef void (*moduleTypeFreeFunc)(void *value);

/* A callback that is called when the client authentication changes. This
 * needs to be exposed since you can't cast a function pointer to (void *) */
typedef void (*RedisModuleUserChangedFunc) (uint64_t client_id, void *privdata);


/* The module type, which is referenced in each value of a given type, defines
 * the methods and links to the module exporting the type. */
typedef struct RedisModuleType {
    uint64_t id; /* Higher 54 bits of type ID + 10 lower bits of encoding ver. */
    struct RedisModule *module;
    moduleTypeLoadFunc rdb_load;
    moduleTypeSaveFunc rdb_save;
    moduleTypeRewriteFunc aof_rewrite;
    moduleTypeMemUsageFunc mem_usage;
    moduleTypeDigestFunc digest;
    moduleTypeFreeFunc free;
    moduleTypeAuxLoadFunc aux_load;
    moduleTypeAuxSaveFunc aux_save;
    int aux_save_triggers;
    char name[10]; /* 9 bytes name + null term. Charset: A-Z a-z 0-9 _- */
} moduleType;

/* In Redis objects 'robj' structures of type OBJ_MODULE, the value pointer
 * is set to the following structure, referencing the moduleType structure
 * in order to work with the value, and at the same time providing a raw
 * pointer to the value, as created by the module commands operating with
 * the module type.
 *
 * So for example in order to free such a value, it is possible to use
 * the following code:
 *
 *  if (robj->type == OBJ_MODULE) {
 *      moduleValue *mt = robj->ptr;
 *      mt->type->free(mt->value);
 *      zfree(mt); // We need to release this in-the-middle struct as well.
 *  }
 */
typedef struct moduleValue {
    moduleType *type;
    void *value;
} moduleValue;

/* This is a wrapper for the 'rio' streams used inside rdb.c in Redis, so that
 * the user does not have to take the total count of the written bytes nor
 * to care about error conditions. */
typedef struct RedisModuleIO {
    size_t bytes;       /* Bytes read / written so far. */
    rio *rio;           /* Rio stream. */
    moduleType *type;   /* Module type doing the operation. */
    int error;          /* True if error condition happened. */
    int ver;            /* Module serialization version: 1 (old),
                         * 2 (current version with opcodes annotation). */
    struct RedisModuleCtx *ctx; /* Optional context, see RM_GetContextFromIO()*/
    struct redisObject *key;    /* Optional name of key processed */
} RedisModuleIO;

/* Macro to initialize an IO context. Note that the 'ver' field is populated
 * inside rdb.c according to the version of the value to load. */
#define moduleInitIOContext(iovar,mtype,rioptr,keyptr) do { \
    iovar.rio = rioptr; \
    iovar.type = mtype; \
    iovar.bytes = 0; \
    iovar.error = 0; \
    iovar.ver = 0; \
    iovar.key = keyptr; \
    iovar.ctx = NULL; \
} while(0);

/* This is a structure used to export DEBUG DIGEST capabilities to Redis
 * modules. We want to capture both the ordered and unordered elements of
 * a data structure, so that a digest can be created in a way that correctly
 * reflects the values. See the DEBUG DIGEST command implementation for more
 * background. */
typedef struct RedisModuleDigest {
    unsigned char o[20];    /* Ordered elements. */
    unsigned char x[20];    /* Xored elements. */
} RedisModuleDigest;

/* Just start with a digest composed of all zero bytes. */
#define moduleInitDigestContext(mdvar) do { \
    memset(mdvar.o,0,sizeof(mdvar.o)); \
    memset(mdvar.x,0,sizeof(mdvar.x)); \
} while(0);

/* Objects encoding. Some kind of objects like Strings and Hashes can be
 * internally represented in multiple ways. The 'encoding' field of the object
 * is set to one of this fields for this object. */
#define OBJ_ENCODING_RAW 0     /* Raw representation */
#define OBJ_ENCODING_INT 1     /* Encoded as integer */
#define OBJ_ENCODING_HT 2      /* Encoded as hash table */
#define OBJ_ENCODING_ZIPMAP 3  /* Encoded as zipmap */
#define OBJ_ENCODING_LINKEDLIST 4 /* No longer used: old list encoding. */
#define OBJ_ENCODING_ZIPLIST 5 /* Encoded as ziplist */
#define OBJ_ENCODING_INTSET 6  /* Encoded as intset */
#define OBJ_ENCODING_SKIPLIST 7  /* Encoded as skiplist */
#define OBJ_ENCODING_EMBSTR 8  /* Embedded sds string encoding */
#define OBJ_ENCODING_QUICKLIST 9 /* Encoded as linked list of ziplists */
#define OBJ_ENCODING_STREAM 10 /* Encoded as a radix tree of listpacks */

#define LRU_BITS 24
#define LRU_CLOCK_MAX ((1<<LRU_BITS)-1) /* Max value of obj->lru */
#define LRU_CLOCK_RESOLUTION 1000 /* LRU clock resolution in ms */

#define OBJ_SHARED_REFCOUNT INT_MAX     /* Global object never destroyed. */
#define OBJ_STATIC_REFCOUNT (INT_MAX-1) /* Object allocated in the stack. */
#define OBJ_FIRST_SPECIAL_REFCOUNT OBJ_STATIC_REFCOUNT
typedef struct redisObject {
    //对象的数据类型，占4bits，共5种类型
    unsigned type:4;
    //对象的编码，占4bits，共10种类型
    unsigned encoding:4;

    //least recently used
    //实用LRU算法计算相对server.lruclock的LRU时间
    unsigned lru:LRU_BITS; /* LRU time (relative to global lru_clock) or
                            * LFU data (least significant 8 bits frequency
                            * and most significant 16 bits access time). */
    //引用计数
    int refcount;
    //指向底层数据实现的指针
    void *ptr;
} robj;

/* The a string name for an object's type as listed above
 * Native types are checked against the OBJ_STRING, OBJ_LIST, OBJ_* defines,
 * and Module types have their registered name returned. */
char *getObjectTypeName(robj*);

/* Macro used to initialize a Redis object allocated on the stack.
 * Note that this macro is taken near the structure definition to make sure
 * we'll update it when the structure is changed, to avoid bugs like
 * bug #85 introduced exactly in this way. */
//初始化一个在栈中分配的Redis对象
#define initStaticStringObject(_var,_ptr) do { \
    _var.refcount = OBJ_STATIC_REFCOUNT; \
    _var.type = OBJ_STRING; \
    _var.encoding = OBJ_ENCODING_RAW; \
    _var.ptr = _ptr; \
} while(0)

struct evictionPoolEntry; /* Defined in evict.c */

/* This structure is used in order to represent the output buffer of a client,
 * which is actually a linked list of blocks like that, that is: client->reply. */
typedef struct clientReplyBlock {
    size_t size, used;
    char buf[];
} clientReplyBlock;

/* Redis database representation. There are multiple databases identified
 * by integers from 0 (the default database) up to the max configured
 * database. The database number is the 'id' field in the structure. */
typedef struct redisDb {
    // 键值对字典，保存数据库中所有的键值对
    dict *dict;                 /* The keyspace for this DB */
    // 过期字典，保存着设置过期的键和键的过期时间
    dict *expires;              /* Timeout of keys with a timeout set */
    // 保存着 所有造成客户端阻塞的键和被阻塞的客户端
    dict *blocking_keys;        /* Keys with clients waiting for data (BLPOP)*/
    // 保存着 处于阻塞状态的键，value为NULL
    dict *ready_keys;           /* Blocked keys that received a PUSH */
    // 事务模块，用于保存被WATCH命令所监控的键
    dict *watched_keys;         /* WATCHED keys for MULTI/EXEC CAS */
    //数据库ID
    int id;                     /* Database ID */
    // 键的平均过期时间
    long long avg_ttl;          /* Average TTL, just for stats */
    //活动到期周期的游标。
    unsigned long expires_cursor; /* Cursor of the active expire cycle. */
    //尝试逐步进行碎片整理的键名列表。
    list *defrag_later;         /* List of key names to attempt to defrag one by one, gradually. */
} redisDb;

/* Client MULTI/EXEC state */
/** 事务命令状态 */
typedef struct multiCmd {
    // 命令的参数列表
    robj **argv;
    // 命令的参数个数
    int argc;
    // 命令函数指针
    struct redisCommand *cmd;
} multiCmd;

/** 事务状态 */
typedef struct multiState {
    // 事务命令队列数组
    multiCmd *commands;     /* Array of MULTI commands */
    // 事务命令的个数
    int count;              /* Total number of MULTI commands */
    // 累积的命令标志一起进行或运算。 因此，如果至少一个命令具有给定标志，它将在此字段中设置。
    int cmd_flags;          /* The accumulated command flags OR-ed together.
                               So if at least a command has a given flag, it
                               will be set in this field. */
    // 与cmd_flags相同，或〜标志。 这样就可以知道所有命令是否都具有特定标志。
    int cmd_inv_flags;      /* Same as cmd_flags, OR-ing the ~flags. so that it
                               is possible to know if all the commands have a
                               certain flag. */
    // 同步复制的标识
    int minreplicas;        /* MINREPLICAS for synchronous replication */
    // 同步复制的超时时间
    time_t minreplicas_timeout; /* MINREPLICAS timeout as unixtime. */
} multiState;

/* This structure holds the blocking operation state for a client.
 * The fields used depend on client->btype. */
typedef struct blockingState {
    /* Generic fields. */
    //阻塞的时间
    mstime_t timeout;       /* Blocking operation timeout. If UNIX current time
                             * is > timeout then the operation timed out. */

    /* BLOCKED_LIST, BLOCKED_ZSET and BLOCKED_STREAM */
    //造成阻塞的键
    dict *keys;             /* The keys we are waiting to terminate a blocking
                             * operation such as BLPOP or XREAD. Or NULL. */
    //用于BRPOPLPUSH命令
    //用于保存PUSH入元素的键，也就是dstkey
    robj *target;           /* The key that should receive the element,
                             * for BRPOPLPUSH. */

    /* BLOCK_STREAM */
    size_t xread_count;     /* XREAD COUNT option. */
    robj *xread_group;      /* XREADGROUP group name. */
    robj *xread_consumer;   /* XREADGROUP consumer name. */
    mstime_t xread_retry_time, xread_retry_ttl;
    int xread_group_noack;

    /* BLOCKED_WAIT */
    /* BLOCKED_WAIT */
    // 阻塞状态
    int numreplicas;        /* Number of replicas we are waiting for ACK. */
    // 要达到的复制偏移量
    long long reploffset;   /* Replication offset to reach. */

    /* BLOCKED_MODULE */
    void *module_blocked_handle; /* RedisModuleBlockedClient structure.
                                    which is opaque for the Redis core, only
                                    handled in module.c. */
} blockingState;

/* The following structure represents a node in the server.ready_keys list,
 * where we accumulate all the keys that had clients blocked with a blocking
 * operation such as B[LR]POP, but received new data in the context of the
 * last executed command.
 *
 * After the execution of every command or script, we run this list to check
 * if as a result we should serve data to clients blocked, unblocking them.
 * Note that server.ready_keys will not have duplicates as there dictionary
 * also called ready_keys in every structure representing a Redis database,
 * where we make sure to remember if a given key was already added in the
 * server.ready_keys list. */
typedef struct readyList {
    redisDb *db;
    robj *key;
} readyList;

/* This structure represents a Redis user. This is useful for ACLs, the
 * user is associated to the connection after the connection is authenticated.
 * If there is no associated user, the connection uses the default user. */
#define USER_COMMAND_BITS_COUNT 1024    /* The total number of command bits
                                           in the user structure. The last valid
                                           command ID we can set in the user
                                           is USER_COMMAND_BITS_COUNT-1. */
#define USER_FLAG_ENABLED (1<<0)        /* The user is active. */
#define USER_FLAG_DISABLED (1<<1)       /* The user is disabled. */
#define USER_FLAG_ALLKEYS (1<<2)        /* The user can mention any key. */
#define USER_FLAG_ALLCOMMANDS (1<<3)    /* The user can run all commands. */
#define USER_FLAG_NOPASS      (1<<4)    /* The user requires no password, any
                                           provided password will work. For the
                                           default user, this also means that
                                           no AUTH is needed, and every
                                           connection is immediately
                                           authenticated. */
typedef struct {
    sds name;       /* The username as an SDS string. */
    uint64_t flags; /* See USER_FLAG_* */

    /* The bit in allowed_commands is set if this user has the right to
     * execute this command. In commands having subcommands, if this bit is
     * set, then all the subcommands are also available.
     *
     * If the bit for a given command is NOT set and the command has
     * subcommands, Redis will also check allowed_subcommands in order to
     * understand if the command can be executed. */
    uint64_t allowed_commands[USER_COMMAND_BITS_COUNT/64];

    /* This array points, for each command ID (corresponding to the command
     * bit set in allowed_commands), to an array of SDS strings, terminated by
     * a NULL pointer, with all the sub commands that can be executed for
     * this command. When no subcommands matching is used, the field is just
     * set to NULL to avoid allocating USER_COMMAND_BITS_COUNT pointers. */
    sds **allowed_subcommands;
    list *passwords; /* A list of SDS valid passwords for this user. */
    list *patterns;  /* A list of allowed key patterns. If this field is NULL
                        the user cannot mention any key in a command, unless
                        the flag ALLKEYS is set in the user. */
} user;

/* With multiplexing we need to take per-client state.
 * Clients are taken in a linked list. */

#define CLIENT_ID_AOF (UINT64_MAX) /* Reserved ID for the AOF client. If you
                                      need more reserved IDs use UINT64_MAX-1,
                                      -2, ... and so forth. */

typedef struct client {
    // client独一无二的ID
    uint64_t id;            /* Client incremental unique ID. */
    connection *conn;
    //RESP协议版本。 可以是2或3。
    int resp;               /* RESP protocol version. Can be 2 or 3. */
    // 指向当前的数据库
    redisDb *db;            /* Pointer to currently SELECTed DB. */
    // client的名字
    robj *name;             /* As set by CLIENT SETNAME. */
    // 输入缓冲区
    sds querybuf;           /* Buffer we use to accumulate client queries. */
    // 输入缓存的峰值
    size_t qb_pos;          /* The position we have read in querybuf. */
    //如果将此客户端标记为主机，则此缓冲区表示我们从主机接收的复制流中尚未应用的部分。
    sds pending_querybuf;   /* If this client is flagged as master, this buffer
                               represents the yet not applied portion of the
                               replication stream that we are receiving from
                               the master. */
    size_t querybuf_peak;   /* Recent (100ms or more) peak of querybuf size. */
    // client输入命令时，参数的数量
    int argc;               /* Num of arguments of current command. */
    // client输入命令的参数列表
    robj **argv;            /* Arguments of current command. */
    //argv列表中对象的长度总和。
    size_t argv_len_sum;    /* Sum of lengths of objects in argv list. */
    // 保存客户端执行命令的历史记录
    struct redisCommand *cmd, *lastcmd;  /* Last command executed. */
    //与此连接关联的用户。 如果用户设置为NULL，则该连接可以执行任何操作（管理员）。
    user *user;             /* User associated with this connection. If the
                               user is set to NULL the connection can do
                               anything (admin). */
    // 请求协议类型，内联或者多条命令
    int reqtype;            /* Request protocol type: PROTO_REQ_* */
    // 参数列表中未读取命令参数的数量，读取一个，该值减1
    int multibulklen;       /* Number of multi bulk arguments left to read. */
    // 命令内容的长度
    long bulklen;           /* Length of bulk argument in multi bulk request. */
    // 回复缓存列表，用于发送大于固定回复缓冲区的回复
    list *reply;            /* List of reply objects to send to the client. */
    // 回复缓存列表对象的总字节数
    unsigned long long reply_bytes; /* Tot bytes of objects in reply list. */
    // 已发送的字节数或对象的字节数
    size_t sentlen;         /* Amount of bytes already sent in the current
                               buffer or object being sent. */
    // client创建所需时间
    time_t ctime;           /* Client creation time. */
    // 最后一次和服务器交互的时间
    time_t lastinteraction; /* Time of the last interaction, used for timeout */
    // 客户端的输出缓冲区超过软性限制的时间，记录输出缓冲区第一次到达软性限制的时间
    time_t obuf_soft_limit_reached_time;
    // client状态的标志
    uint64_t flags;         /* Client flags: CLIENT_* macros. */
    // 认证标志，0表示未认证，1表示已认证
    int authenticated;      /* Needed when the default user requires auth. */
    // 从节点的复制状态
    int replstate;          /* Replication state if this is a slave. */
    // 在ack上设置从节点的写处理器，是否在slave向master发送ack，
    int repl_put_online_on_ack; /* Install slave write handler on first ACK. */
    // 保存主服务器传来的RDB文件的文件描述符
    int repldbfd;           /* Replication DB file descriptor. */
    // 读取主服务器传来的RDB文件的偏移量
    off_t repldboff;        /* Replication DB file offset. */
    // 主服务器传来的RDB文件的大小
    off_t repldbsize;       /* Replication DB file size. */
    // 主服务器传来的RDB文件的大小，符合协议的字符串形式
    sds replpreamble;       /* Replication DB preamble. */
    //如果这是一个主副本，则读取复制偏移量。
    long long read_reploff; /* Read replication offset if this is a master. */
    // replication复制的偏移量
    long long reploff;      /* Applied replication offset if this is a master. */
    // 通过ack命令接收到的偏移量
    long long repl_ack_off; /* Replication ack offset, if this is a slave. */
    // 通过ack命令接收到的偏移量所用的时间
    long long repl_ack_time;/* Replication ack time, if this is a slave. */
    // FULLRESYNC回复给从节点的offset
    long long psync_initial_offset; /* FULLRESYNC reply offset other slaves
                                       copying this slave output buffer
                                       should use. */
    char replid[CONFIG_RUN_ID_SIZE+1]; /* Master replication ID (if master). */
    // 从节点的端口号
    int slave_listening_port; /* As configured with: REPLCONF listening-port */
    // 从节点IP地址
    char slave_ip[NET_IP_STR_LEN]; /* Optionally given by REPLCONF ip-address */
    // 从节点的功能
    int slave_capa;         /* Slave capabilities: SLAVE_CAPA_* bitwise OR. */
    // 事物状态
    multiState mstate;      /* MULTI/EXEC state */
    // 阻塞类型
    int btype;              /* Type of blocking op if CLIENT_BLOCKED. */
    // 阻塞的状态
    blockingState bpop;     /* blocking state */
    // 最近一个写全局的复制偏移量
    long long woff;         /* Last write global replication offset. */
    // 监控列表
    list *watched_keys;     /* Keys WATCHED for MULTI/EXEC CAS */
    // 订阅频道
    dict *pubsub_channels;  /* channels a client is interested in (SUBSCRIBE) */
    // 订阅的模式
    list *pubsub_patterns;  /* patterns a client is interested in (SUBSCRIBE) */
    // 被缓存的ID
    sds peerid;             /* Cached peer ID. */
    listNode *client_list_node; /* list node in client list */
    RedisModuleUserChangedFunc auth_callback; /* Module callback to execute
                                               * when the authenticated user
                                               * changes. */
    void *auth_callback_privdata; /* Private data that is passed when the auth
                                   * changed callback is executed. Opaque for
                                   * Redis Core. */
    void *auth_module;      /* The module that owns the callback, which is used
                             * to disconnect the client if the module is
                             * unloaded for cleanup. Opaque for Redis Core.*/

    /* If this client is in tracking mode and this field is non zero,
     * invalidation messages for keys fetched by this client will be send to
     * the specified client ID. */
    uint64_t client_tracking_redirection;
    rax *client_tracking_prefixes; /* A dictionary of prefixes we are already
                                      subscribed to in BCAST mode, in the
                                      context of client side caching. */
    /* In clientsCronTrackClientsMemUsage() we track the memory usage of
     * each client and add it to the sum of all the clients of a given type,
     * however we need to remember what was the old contribution of each
     * client, and in which categoty the client was, in order to remove it
     * before adding it the new value. */
    uint64_t client_cron_last_memory_usage;
    int      client_cron_last_memory_type;
    /* Response buffer */
    // 回复固定缓冲区的偏移量
    int bufpos;
    // 回复固定缓冲区
    char buf[PROTO_REPLY_CHUNK_BYTES];
} client;

// SAVE 900 1
// SAVE 300 10
// SAVE 60 10000
// 服务器在900秒之内，对数据库执行了至少1次修改
struct saveparam {
    time_t seconds; //秒数
    int changes;    //修改的次数
};

struct moduleLoadQueueEntry {
    sds path;
    int argc;
    robj **argv;
};

struct sharedObjectsStruct {
    robj *crlf, *ok, *err, *emptybulk, *czero, *cone, *pong, *space,
    *colon, *queued, *null[4], *nullarray[4], *emptymap[4], *emptyset[4],
    *emptyarray, *wrongtypeerr, *nokeyerr, *syntaxerr, *sameobjecterr,
    *outofrangeerr, *noscripterr, *loadingerr, *slowscripterr, *bgsaveerr,
    *masterdownerr, *roslaveerr, *execaborterr, *noautherr, *noreplicaserr,
    *busykeyerr, *oomerr, *plus, *messagebulk, *pmessagebulk, *subscribebulk,
    *unsubscribebulk, *psubscribebulk, *punsubscribebulk, *del, *unlink,
    *rpop, *lpop, *lpush, *rpoplpush, *zpopmin, *zpopmax, *emptyscan,
    *multi, *exec,
    *select[PROTO_SHARED_SELECT_CMDS],
    *integers[OBJ_SHARED_INTEGERS],
    *mbulkhdr[OBJ_SHARED_BULKHDR_LEN], /* "*<value>\r\n" */
    *bulkhdr[OBJ_SHARED_BULKHDR_LEN];  /* "$<value>\r\n" */
    sds minstring, maxstring;
};

/*
 * ZSET（有序集合）使用的特殊版本的跳跃表
 * https://zcchizhang-1302900328.cos.ap-nanjing.myqcloud.com/redis_data_structure_10.png
 * */
// 有序集合跳跃表节点结构
typedef struct zskiplistNode {
    sds ele;                            // 节点数据对象，指向一个sds字符串对象
    double score;                       // 节点分值，跳跃表中的所有节点都按照分值从小到大排序
    struct zskiplistNode *backward;     // 前置节点
    struct zskiplistLevel {
        struct zskiplistNode *forward;  // 后置节点
        unsigned long span;             // 该层跨越的节点数量
    } level[];                          // 跳跃表层结构，一个zskiplistLevel数组
} zskiplistNode;

// 有序集合跳跃表结构
typedef struct zskiplist {
    struct zskiplistNode *header, *tail;    // 跳跃表表头节点指针和表尾节点指针
    unsigned long length;                   // 跳跃表的长度，即跳跃表当前的节点数量（表头节点不算在内）
    int level;                              // 当前跳跃表中层数最大的节点的层数（表头节点的层数不算在内）
} zskiplist;

//有序集合类型
typedef struct zset {
    dict *dict;     //字典
    zskiplist *zsl; //跳跃表
} zset;

typedef struct clientBufferLimitsConfig {
    unsigned long long hard_limit_bytes;
    unsigned long long soft_limit_bytes;
    time_t soft_limit_seconds;
} clientBufferLimitsConfig;

extern clientBufferLimitsConfig clientBufferLimitsDefaults[CLIENT_TYPE_OBUF_COUNT];

/* The redisOp structure defines a Redis Operation, that is an instance of
 * a command with an argument vector, database ID, propagation target
 * (PROPAGATE_*), and command pointer.
 *
 * Currently only used to additionally propagate more commands to AOF/Replication
 * after the propagation of the executed command. */
typedef struct redisOp {
    robj **argv;                //命令的参数列表
    int argc, dbid, target;     //参数个数，数据库的ID，传播的目标
    struct redisCommand *cmd;   //命令指针
} redisOp;  //redis操作

/* Defines an array of Redis operations. There is an API to add to this
 * structure in an easy way.
 *
 * redisOpArrayInit();
 * redisOpArrayAppend();
 * redisOpArrayFree();
 */
typedef struct redisOpArray {
    redisOp *ops;       //数组指针
    int numops;         //数组个数
} redisOpArray;         //redis操作数组

/* This structure is returned by the getMemoryOverheadData() function in
 * order to return memory overhead information. */
/* 主要定义一些内存开销信息 */
struct redisMemOverhead {
    size_t peak_allocated;
    size_t total_allocated;
    size_t startup_allocated;
    size_t repl_backlog;
    size_t clients_slaves;
    size_t clients_normal;
    size_t aof_buffer;
    size_t lua_caches;
    size_t overhead_total;
    size_t dataset;
    size_t total_keys;
    size_t bytes_per_key;
    float dataset_perc;
    float peak_perc;
    float total_frag;
    ssize_t total_frag_bytes;
    float allocator_frag;
    ssize_t allocator_frag_bytes;
    float allocator_rss;
    ssize_t allocator_rss_bytes;
    float rss_extra;
    size_t rss_extra_bytes;
    size_t num_dbs;
    struct {
        size_t dbid;
        size_t overhead_ht_main;
        size_t overhead_ht_expires;
    } *db;
};

/* This structure can be optionally passed to RDB save/load functions in
 * order to implement additional functionalities, by storing and loading
 * metadata to the RDB file.
 *
 * Currently the only use is to select a DB at load time, useful in
 * replication in order to make sure that chained slaves (slaves of slaves)
 * select the correct DB and are able to accept the stream coming from the
 * top-level master. */
typedef struct rdbSaveInfo {
    /* Used saving and loading. */
    int repl_stream_db;  /* DB to select in server.master client. */

    /* Used only loading. */
    int repl_id_is_set;  /* True if repl_id field is set. */
    char repl_id[CONFIG_RUN_ID_SIZE+1];     /* Replication ID. */
    long long repl_offset;                  /* Replication offset. */
} rdbSaveInfo;

#define RDB_SAVE_INFO_INIT {-1,0,"000000000000000000000000000000",-1}

struct malloc_stats {
    size_t zmalloc_used;
    size_t process_rss;
    size_t allocator_allocated;
    size_t allocator_active;
    size_t allocator_resident;
};

/*-----------------------------------------------------------------------------
 * TLS Context Configuration
 *----------------------------------------------------------------------------*/

typedef struct redisTLSContextConfig {
    char *cert_file;
    char *key_file;
    char *dh_params_file;
    char *ca_cert_file;
    char *ca_cert_dir;
    char *protocols;
    char *ciphers;
    char *ciphersuites;
    int prefer_server_ciphers;
    int session_caching;
    int session_cache_size;
    int session_cache_timeout;
} redisTLSContextConfig;

/*-----------------------------------------------------------------------------
 * Global server state
 *----------------------------------------------------------------------------*/

struct clusterState;

/* AIX defines hz to __hz, we don't use this define and in order to allow
 * Redis build on AIX we need to undef it. */
#ifdef _AIX
#undef hz
#endif

#define CHILD_INFO_MAGIC 0xC17DDA7A12345678LL
#define CHILD_TYPE_NONE 0
#define CHILD_TYPE_RDB 1
#define CHILD_TYPE_AOF 2
#define CHILD_TYPE_LDB 3
#define CHILD_TYPE_MODULE 4

struct redisServer {
    /* General */
    //主进程id
    pid_t pid;                  /* Main process pid. */
    //主线程id
    pthread_t main_thread_id;         /* Main thread id */
    // 配置文件的绝对路径
    char *configfile;           /* Absolute config file path, or NULL */
    // 可执行文件的绝对路径
    char *executable;           /* Absolute executable file path. */
    // 执行executable文件的参数
    char **exec_argv;           /* Executable argv vector (copy). */
    // 根据客户数量更改hz值。
    int dynamic_hz;             /* Change hz value depending on # of clients. */
    // 配置的hz值。 如果启用了dynamic-hz，则可能与实际的“hz”字段值不同。
    int config_hz;              /* Configured HZ value. May be different than
                                   the actual 'hz' field value if dynamic-hz
                                   is enabled. */
    // serverCron()调用的频率
    int hz;                     /* serverCron() calls frequency in hertz */
    // 表明是否为fork的子进程
    int in_fork_child;          /* indication that this is a fork child */
    // 数据库数组，长度为16
    redisDb *db;
    // 命令表
    dict *commands;             /* Command table */
    // rename之前的命令表
    dict *orig_commands;        /* Command table before command renaming. */
    // 事件循环
    aeEventLoop *el;
    // 服务器的LRU时钟
    _Atomic unsigned int lruclock; /* Clock for LRU eviction */
    // 立即关闭服务器
    volatile sig_atomic_t shutdown_asap; /* SHUTDOWN needed ASAP */
    // 主动rehashing的标志
    int activerehashing;        /* Incremental rehash in serverCron() */
    int active_defrag_running;  /* Active defragmentation running (holds current scan aggressiveness) */
    // 保存子进程pid文件
    char *pidfile;              /* PID file path */
    int arch_bits;              /* 32 or 64 depending on sizeof(long) */
    // serverCron()函数运行的次数
    int cronloops;              /* Number of times the cron function run */
    // 服务器每次重启都会分配一个ID
    char runid[CONFIG_RUN_ID_SIZE+1];  /* ID always different at every exec. */
    // 哨兵模式
    int sentinel_mode;          /* True if this instance is a Sentinel. */
    size_t initial_memory_usage; /* Bytes used after initialization. */
    int always_show_logo;       /* Show logo even for non-stdout logging. */
    /* Modules */
    dict *moduleapi;            /* Exported core APIs dictionary for modules. */
    dict *sharedapi;            /* Like moduleapi but containing the APIs that
                                   modules share with each other. */
    list *loadmodule_queue;     /* List of modules to load at startup. */
    int module_blocked_pipe[2]; /* Pipe used to awake the event loop if a
                                   client blocked on a module command needs
                                   to be processed. */
    pid_t module_child_pid;     /* PID of module child */
    /* Networking */
    // TCP监听的端口
    int port;                   /* TCP listening port */
    int tls_port;               /* TLS listening port */
    // listen()函数的backlog参数，提示系统该进程要入队的未完成连接请求的数量。默认值为128
    int tcp_backlog;            /* TCP listen() backlog */
    char *bindaddr[CONFIG_BINDADDR_MAX]; /* Addresses we should bind to */
    // 绑定地址的数量
    int bindaddr_count;         /* Number of addresses in server.bindaddr[] */
    // Unix socket的路径
    char *unixsocket;           /* UNIX socket path */
    mode_t unixsocketperm;      /* UNIX socket permission */
    int ipfd[CONFIG_BINDADDR_MAX]; /* TCP socket file descriptors */
    int ipfd_count;             /* Used slots in ipfd[] */
    int tlsfd[CONFIG_BINDADDR_MAX]; /* TLS socket file descriptors */
    int tlsfd_count;            /* Used slots in tlsfd[] */
    // 本地Unix连接的fd
    int sofd;                   /* Unix socket file descriptor */
    // 集群的fd
    int cfd[CONFIG_BINDADDR_MAX];/* Cluster bus listening socket */
    // 集群的fd个数
    int cfd_count;              /* Used slots in cfd[] */
    list *clients;              /* List of active clients */
    // 所有待关闭的client链表
    list *clients_to_close;     /* Clients to close asynchronously */
    // 要写或者安装写处理程序的client链表
    list *clients_pending_write; /* There is to write or install handler. */
    list *clients_pending_read;  /* Client has pending read socket buffers. */
    // 从节点列表和监视器列表
    list *slaves, *monitors;    /* List of slaves and MONITORs */
    // 当前的client，被用于崩溃报告
    client *current_client;     /* Current client executing the command. */
    rax *clients_timeout_table; /* Radix tree for blocked clients timeouts. */
    long fixed_time_expire;     /* If > 0, expire keys against server.mstime. */
    rax *clients_index;         /* Active clients dictionary by client ID. */
    // 如果当前client正处于暂停状态，则设置为真
    int clients_paused;         /* True if clients are currently paused */
    // 取消暂停状态的时间
    mstime_t clients_pause_end_time; /* Time when we undo clients_paused */
    char neterr[ANET_ERR_LEN];   /* Error buffer for anet.c */
    // 迁移缓存套接字的字典，键是host:ip，值是TCP socket的的结构
    dict *migrate_cached_sockets;/* MIGRATE cached sockets */
    _Atomic uint64_t next_client_id; /* Next client unique ID. Incremental. */
    // 受保护模式，不接受外部的连接
    int protected_mode;         /* Don't accept external connections. */
    int gopher_enabled;         /* If true the server will reply to gopher
                                   queries. Will still serve RESP2 queries. */
    int io_threads_num;         /* Number of IO threads to use. */
    int io_threads_do_reads;    /* Read and parse from IO threads? */
    int io_threads_active;      /* Is IO threads currently active? */
    long long events_processed_while_blocked; /* processEventsWhileBlocked() */

    /* RDB / AOF loading information */
    // 正在载入状态
    volatile sig_atomic_t loading; /* We are loading data from disk if true */
    // 设置载入的总字节
    off_t loading_total_bytes;
    // 已载入的字节数
    off_t loading_loaded_bytes;
    // 载入的开始时间
    time_t loading_start_time;
    // 在load时，用来设置读或写的最大字节数max_processing_chunk
    off_t loading_process_events_interval_bytes;
    /* Fast pointers to often looked up command */
    struct redisCommand *delCommand, *multiCommand, *lpushCommand,
                        *lpopCommand, *rpopCommand, *zpopminCommand,
                        *zpopmaxCommand, *sremCommand, *execCommand,
                        *expireCommand, *pexpireCommand, *xclaimCommand,
                        *xgroupCommand, *rpoplpushCommand;
    /* Fields used only for stats */
    time_t stat_starttime;          /* Server start time */
    // 命令执行的次数
    long long stat_numcommands;     /* Number of processed commands */
    // 连接接受的数量
    long long stat_numconnections;  /* Number of connections received */
    // 过期键的数量
    long long stat_expiredkeys;     /* Number of expired keys */
    double stat_expired_stale_perc; /* Percentage of keys probably expired */
    long long stat_expired_time_cap_reached_count; /* Early expire cylce stops.*/
    long long stat_expire_cycle_time_used; /* Cumulative microseconds used. */
    // 回收键的个数
    long long stat_evictedkeys;     /* Number of evicted keys (maxmemory) */
    long long stat_keyspace_hits;   /* Number of successful lookups of keys */
    long long stat_keyspace_misses; /* Number of failed lookups of keys */
    long long stat_active_defrag_hits;      /* number of allocations moved */
    long long stat_active_defrag_misses;    /* number of allocations scanned but not moved */
    long long stat_active_defrag_key_hits;  /* number of keys with moved allocations */
    long long stat_active_defrag_key_misses;/* number of keys scanned and not moved */
    long long stat_active_defrag_scanned;   /* number of dictEntries scanned */
    // 服务器内存使用的
    size_t stat_peak_memory;        /* Max used memory record */
    // 计算fork()消耗的时间
    long long stat_fork_time;       /* Time needed to perform latest fork() */
    // 计算fork的速率，GB/每秒
    double stat_fork_rate;          /* Fork rate in GB/sec. */
    // 因为最大连接数限制而被拒绝的client个数
    long long stat_rejected_conn;   /* Clients rejected because of maxclients */
    // 执行全量重同步的次数
    long long stat_sync_full;       /* Number of full resyncs with slaves. */
    // 接受PSYNC请求的个数
    long long stat_sync_partial_ok; /* Number of accepted PSYNC requests. */
    long long stat_sync_partial_err;/* Number of unaccepted PSYNC requests. */
    list *slowlog;                  /* SLOWLOG list of commands */
    long long slowlog_entry_id;     /* SLOWLOG current entry ID */
    long long slowlog_log_slower_than; /* SLOWLOG time limit (to get logged) */
    unsigned long slowlog_max_len;     /* SLOWLOG max number of items logged */
    struct malloc_stats cron_malloc_stats; /* sampled in serverCron(). */
    // 从网络读的字节数
    _Atomic long long stat_net_input_bytes; /* Bytes read from network. */
    // 已经写到网络的字节数
    _Atomic long long stat_net_output_bytes; /* Bytes written to network. */
    size_t stat_rdb_cow_bytes;      /* Copy on write bytes during RDB saving. */
    size_t stat_aof_cow_bytes;      /* Copy on write bytes during AOF rewrite. */
    size_t stat_module_cow_bytes;   /* Copy on write bytes during module fork. */
    uint64_t stat_clients_type_memory[CLIENT_TYPE_COUNT];/* Mem usage by type */
    long long stat_unexpected_error_replies; /* Number of unexpected (aof-loading, replica to master, etc.) error replies */
    long long stat_io_reads_processed; /* Number of read events processed by IO / Main threads */
    long long stat_io_writes_processed; /* Number of write events processed by IO / Main threads */
    _Atomic long long stat_total_reads_processed; /* Total number of read events processed */
    _Atomic long long stat_total_writes_processed; /* Total number of write events processed */
    /* The following two are used to track instantaneous metrics, like
     * number of operations per second, network traffic. */
    struct {
        // 上一个进行抽样的时间戳
        long long last_sample_time; /* Timestamp of last sample in ms */
        // 上一个抽样时的样品数量
        long long last_sample_count;/* Count in last sample */
        // 样品表
        long long samples[STATS_METRIC_SAMPLES];
        // 样品表的下标
        int idx;
    } inst_metric[STATS_METRIC_COUNT];
    /* Configuration */
    // 可见性日志，日志级别
    int verbosity;                  /* Loglevel in redis.conf */
    // client超过的最大时间，单位秒
    int maxidletime;                /* Client timeout in seconds */
    int tcpkeepalive;               /* Set SO_KEEPALIVE if non-zero. */
    // 关闭测试，默认初始化为1
    int active_expire_enabled;      /* Can be disabled for testing purposes. */
    int active_expire_effort;       /* From 1 (default) to 10, active effort. */
    int active_defrag_enabled;
    int jemalloc_bg_thread;         /* Enable jemalloc background thread */
    size_t active_defrag_ignore_bytes; /* minimum amount of fragmentation waste to start active defrag */
    int active_defrag_threshold_lower; /* minimum percentage of fragmentation to start active defrag */
    int active_defrag_threshold_upper; /* maximum percentage of fragmentation at which we use maximum effort */
    int active_defrag_cycle_min;       /* minimal effort for defrag in CPU percentage */
    int active_defrag_cycle_max;       /* maximal effort for defrag in CPU percentage */
    unsigned long active_defrag_max_scan_fields; /* maximum number of fields of set/hash/zset/list to process from within the main dict scan */
    _Atomic size_t client_max_querybuf_len; /* Limit for client query buffer length */
    int dbnum;                      /* Total number of configured DBs */
    // 1表示被监视，否则0
    int supervised;                 /* 1 if supervised, 0 otherwise. */
    // 监视模式
    int supervised_mode;            /* See SUPERVISED_* */
    // 如果是以守护进程运行，则为真
    int daemonize;                  /* True if running as a daemon */
    // 不同类型的client的输出缓冲区限制
    clientBufferLimitsConfig client_obuf_limits[CLIENT_TYPE_OBUF_COUNT];
    /* AOF persistence */
    // AOF的状态，开启|关闭|等待重写
    int aof_enabled;                /* AOF configuration */
    // 同步状态
    int aof_state;                  /* AOF_(ON|OFF|WAIT_REWRITE) */
    // 同步策略
    int aof_fsync;                  /* Kind of fsync() policy */
    // AOf文件的名字
    char *aof_filename;             /* Name of the AOF file */
    // 如果正在执行AOF重写，设置为1
    int aof_no_fsync_on_rewrite;    /* Don't fsync if a rewrite is in prog. */
    // AOF文件所增长的比率，默认为100
    int aof_rewrite_perc;           /* Rewrite AOF if % growth is > M and... */
    // AOF文件最下的大小
    off_t aof_rewrite_min_size;     /* the AOF file is at least N bytes. */
    // AOF文件在重写或启动后的大小
    off_t aof_rewrite_base_size;    /* AOF size on latest startup or rewrite. */
    // AOF文件当前的大小
    off_t aof_current_size;         /* AOF current size. */
    off_t aof_fsync_offset;         /* AOF offset which is already synced to disk. */
    int aof_flush_sleep;            /* Micros to sleep before flush. (used by tests) */
    int aof_rewrite_scheduled;      /* Rewrite once BGSAVE terminates. */
    // 将AOF重写提上日程，当RDB的BGSAVE结束后，立即执行AOF重写
    pid_t aof_child_pid;            /* PID if rewriting process */
    // AOF缓冲块的链表
    list *aof_rewrite_buf_blocks;   /* Hold changes during an AOF rewrite. */
    // AOF缓冲区，在进入事件loop之前写入
    sds aof_buf;      /* AOF buffer, written before entering the event loop */
    // AOF文件的文件描述符
    int aof_fd;       /* File descriptor of currently selected AOF file */
    // 执行AOF时，当前的数据库id
    int aof_selected_db; /* Currently selected DB in AOF */
    // 延迟执行flush操作的开始时间
    time_t aof_flush_postponed_start; /* UNIX time of postponed AOF flush */
    // AOF最近一次同步的时间
    time_t aof_last_fsync;            /* UNIX time of last fsync() */
    // AOF开始的时间
    time_t aof_rewrite_time_last;   /* Time used by last AOF rewrite run. */
    time_t aof_rewrite_time_start;  /* Current AOF rewrite start time. */
    int aof_lastbgrewrite_status;   /* C_OK or C_ERR */
    // 延迟fsync的次数
    unsigned long aof_delayed_fsync;  /* delayed AOF fsync() counter */
    // 重写时是否开启增量式同步，每次写入AOF_AUTOSYNC_BYTES个字节，就执行一次同步
    int aof_rewrite_incremental_fsync;/* fsync incrementally while aof rewriting? */
    int rdb_save_incremental_fsync;   /* fsync incrementally while rdb saving? */
    // 上一次AOF操作的状态
    int aof_last_write_status;      /* C_OK or C_ERR */
    // 如果AOF最近一个写状态为错误的，则为真
    int aof_last_write_errno;       /* Valid if aof_last_write_status is ERR */
    // 在不是所预期的AOF结尾的地方继续加载
    // 如果发现末尾命令不完整则自动截掉,成功加载前面正确的数据。
    int aof_load_truncated;         /* Don't stop on unexpected AOF EOF. */
    int aof_use_rdb_preamble;       /* Use RDB preamble on AOF rewrites. */
    /* AOF pipes used to communicate between parent and child during rewrite. */
    int aof_pipe_write_data_to_child;   //父进程写给子进程的文件描述符
    int aof_pipe_read_data_from_parent; //子进程从父进程读的文件描述符
    int aof_pipe_write_ack_to_parent;   //子进程写ack给父进程的文件描述符
    int aof_pipe_read_ack_from_child;   //父进程从子进程读ack的文件描述符
    int aof_pipe_write_ack_to_child;    //父进程写ack给子进程的文件描述符
    int aof_pipe_read_ack_from_parent;  //子进程从父进程读ack的文件描述符
    // 如果为真，则停止发送累计的不同数据给子进程
    int aof_stop_sending_diff;     /* If true stop sending accumulated diffs
                                      to child process. */
    // 保存子进程AOF时差异累计数据的sds
    sds aof_child_diff;             /* AOF diff accumulator child side. */
    /* RDB persistence */
    // 脏键，记录数据库被修改的次数
    long long dirty;                /* Changes to DB from the last save */
    // 在BGSAVE之前要备份脏键dirty的值，如果BGSAVE失败会还原
    long long dirty_before_bgsave;  /* Used to restore dirty on failed BGSAVE */
    // 执行BGSAVE的子进程的pid
    pid_t rdb_child_pid;            /* PID of RDB saving child */
    // 保存save参数的数组
    struct saveparam *saveparams;   /* Save points array for RDB */
    // 数组长度
    int saveparamslen;              /* Number of saving points */
    // RDB文件的名字，默认为dump.rdb
    char *rdb_filename;             /* Name of RDB file */
    // 是否采用LZF压缩算法压缩RDB文件，默认yes
    int rdb_compression;            /* Use compression in RDB? */
    // RDB文件是否使用校验和，默认yes
    int rdb_checksum;               /* Use RDB checksum? */
    int rdb_del_sync_files;         /* Remove RDB files used only for SYNC if
                                       the instance does not use persistence. */
    // 上一次执行SAVE成功的时间
    time_t lastsave;                /* Unix time of last successful save */
    // 最近一个尝试执行BGSAVE的时间
    time_t lastbgsave_try;          /* Unix time of last attempted bgsave */
    // 最近执行BGSAVE的时间
    time_t rdb_save_time_last;      /* Time used by last RDB save run. */
    // BGSAVE开始的时间
    time_t rdb_save_time_start;     /* Current RDB save start time. */
    // 当rdb_bgsave_scheduled为真时，才能开始BGSAVE
    int rdb_bgsave_scheduled;       /* BGSAVE when possible if true. */
    // rdb执行的类型，是写入磁盘，还是写入从节点的socket
    int rdb_child_type;             /* Type of save by active child. */
    // BGSAVE执行完的状态
    int lastbgsave_status;          /* C_OK or C_ERR */
    // BGSAVE 出错，不允许写命令
    int stop_writes_on_bgsave_err;  /* Don't allow writes if can't BGSAVE */
    // 无磁盘同步，管道的写端
    int rdb_pipe_write;             /* RDB pipes used to transfer the rdb */
    // 无磁盘同步，管道的读端
    int rdb_pipe_read;              /* data to the parent process in diskless repl. */
    connection **rdb_pipe_conns;    /* Connections which are currently the */
    int rdb_pipe_numconns;          /* target of diskless rdb fork child. */
    int rdb_pipe_numconns_writing;  /* Number of rdb conns with pending writes. */
    char *rdb_pipe_buff;            /* In diskless replication, this buffer holds data */
    int rdb_pipe_bufflen;           /* that was read from the the rdb pipe. */
    int rdb_key_save_delay;         /* Delay in microseconds between keys while
                                     * writing the RDB. (for testings) */
    int key_load_delay;             /* Delay in microseconds between keys while
                                     * loading aof or rdb. (for testings) */
    /* Pipe and data structures for child -> parent info sharing. */
    int child_info_pipe[2];         /* Pipe used to write the child_info_data. */
    struct {
        int process_type;           /* AOF or RDB child? */
        size_t cow_size;            /* Copy on write size. */
        unsigned long long magic;   /* Magic value to make sure data is valid. */
    } child_info_data;
    /* Propagation of commands in AOF / replication */
    // 将命令加到Redis操作数组中
    redisOpArray also_propagate;    /* Additional command to propagate. */
    /* Logging */
    // 日志文件的路径
    char *logfile;                  /* Path of log file */
    // 是否开启系统日志
    int syslog_enabled;             /* Is syslog enabled? */
    // 系统日志标识
    char *syslog_ident;             /* Syslog ident */
    int syslog_facility;            /* Syslog facility */
    /* Replication (master) */
    char replid[CONFIG_RUN_ID_SIZE+1];  /* My current replication ID. */
    char replid2[CONFIG_RUN_ID_SIZE+1]; /* replid inherited from master*/
    // 全局的复制偏移量
    long long master_repl_offset;   /* My current replication offset */
    long long second_replid_offset; /* Accept offsets up to this for replid2. */
    // 复制缓冲区最近一次使用的数据库
    int slaveseldb;                 /* Last SELECTed DB in replication output */
    // 每N秒，主节点PING从节点
    int repl_ping_slave_period;     /* Master pings the slave every N seconds */
    // 复制积压缓冲区backlog，用于局部同步
    char *repl_backlog;             /* Replication backlog for partial syncs */
    // 复制积压缓冲区backlog的大小
    long long repl_backlog_size;    /* Backlog circular buffer size */
    // 复制积压缓冲区backlog中实际的数据长度
    long long repl_backlog_histlen; /* Backlog actual data length */
    // 复制积压缓冲区backlog当前的偏移量，下次写操作的下标
    long long repl_backlog_idx;     /* Backlog circular buffer current offset,
                                       that is the next byte will'll write to.*/
    // 记录复制的偏移量，backlog的第一个字节的逻辑位置是master_repl_offset的下一个字节
    long long repl_backlog_off;     /* Replication "master offset" of first
                                       byte in the replication backlog buffer.*/
    // 没有从节点，backlog被释放的时间
    time_t repl_backlog_time_limit; /* Time without slaves after the backlog
                                       gets released. */
    // 没有从节点的时间
    time_t repl_no_slaves_since;    /* We have no slaves since that time.
                                       Only valid if server.slaves len is 0. */
    // 执行写操作的最少的从节点个数
    int repl_min_slaves_to_write;   /* Min number of slaves to write. */
    // 最小数量的从节点的最大延迟值
    int repl_min_slaves_max_lag;    /* Max lag of <count> slaves to write. */
    int repl_good_slaves_count;     /* Number of slaves with lag <= max_lag. */
    // 无盘同步，直接将RDB文件发送给socket
    int repl_diskless_sync;         /* Master send RDB to slaves sockets directly. */
    int repl_diskless_load;         /* Slave parse RDB directly from the socket.
                                     * see REPL_DISKLESS_LOAD_* enum */
    // 延迟开始无盘同步的时间
    int repl_diskless_sync_delay;   /* Delay to start a diskless repl BGSAVE. */
    /* Replication (slave) */
    char *masteruser;               /* AUTH with this user and masterauth with master */
    // 认证密码
    char *masterauth;               /* AUTH with this password with master */
    // 主节点的主机名(IP)
    char *masterhost;               /* Hostname of master */
    // 主节点的端口
    int masterport;                 /* Port of master */
    // 复制主从连接超时时间
    int repl_timeout;               /* Timeout after N seconds of master idle */
    // client是主服务器，对于这个从节点
    client *master;     /* Client that is master for this slave */
    // 主节点的同步缓存
    client *cached_master; /* Cached master to be reused for PSYNC. */
    // 同步IO调用的超时时间
    int repl_syncio_timeout; /* Timeout for synchronous I/O calls */
    int repl_state;          /* Replication status if the instance is a slave */
    // 同步期间从主节点读到的RDB的大小
    off_t repl_transfer_size; /* Size of RDB to read from master during sync. */
    // 同步期间从主节点读到的RDB的总量
    off_t repl_transfer_read; /* Amount of RDB read from master during sync. */
    // 最近一个执行fsync的偏移量
    off_t repl_transfer_last_fsync_off; /* Offset when we fsync-ed last time. */
    // 从节点和主节点的同步套接字
    connection *repl_transfer_s;     /* Slave -> Master SYNC connection */
    // 保存RDB文件的临时文件描述符
    int repl_transfer_fd;    /* Slave -> Master SYNC temp file descriptor */
    // 保存RDB文件的临时文件名
    char *repl_transfer_tmpfile; /* Slave-> master SYNC temp file name */
    // 最近一次读到RDB文件内容的时间
    time_t repl_transfer_lastio; /* Unix time of the latest read, for timeout */
    // 当连接下线后，是否维护旧的数据
    int repl_serve_stale_data; /* Serve stale data when link is down? */
    int repl_slave_ro;          /* Slave is read only? */
    int repl_slave_ignore_maxmemory;    /* If true slaves do not evict. */
    time_t repl_down_since; /* Unix time at which link with master went down */
    // 连接断开的时长
    int repl_disable_tcp_nodelay;   /* Disable TCP_NODELAY after SYNC? */
    int slave_priority;             /* Reported in INFO and used by Sentinel. */
    // 发送给主节点的从节点端口
    int slave_announce_port;        /* Give the master this listening port. */
    // 发送给主节点的IP
    char *slave_announce_ip;        /* Give the master this ip address. */
    /* The following two fields is where we store master PSYNC replid/offset
     * while the PSYNC is in progress. At the end we'll copy the fields into
     * the server->master client structure. */
    // 主节点的运行ID
    char master_replid[CONFIG_RUN_ID_SIZE+1];  /* Master PSYNC runid. */
    // 主节点PSYNC的偏移量
    long long master_initial_offset;           /* Master PSYNC offset. */
    int repl_slave_lazy_flush;          /* Lazy FLUSHALL before loading DB? */
    /* Replication script cache. */
    // 保存SHA1键的字典
    dict *repl_scriptcache_dict;        /* SHA1 all slaves are aware of. */
    // 用作环形队列的链表
    list *repl_scriptcache_fifo;        /* First in, first out LRU eviction. */
    // 最大缓存脚本数
    unsigned int repl_scriptcache_size; /* Max number of elements. */
    /* Synchronous replication. */
    // 等待 WAIT 命令的client链表
    list *clients_waiting_acks;         /* Clients waiting in WAIT command. */
    // 如果为真，则发送REPLCONF GETACK
    int get_ack_from_slaves;            /* If true we send REPLCONF GETACK. */
    /* Limits */
    // 同时最多连接的client的数量
    unsigned int maxclients;            /* Max number of simultaneous clients */
    // 服务器最多使用的内存字节数
    unsigned long long maxmemory;   /* Max number of memory bytes to use */
    // 键占用内存的回收策略
    int maxmemory_policy;           /* Policy for key eviction */
    // 随机抽样的个数
    int maxmemory_samples;          /* Precision of random sampling */
    int lfu_log_factor;             /* LFU logarithmic counter factor. */
    int lfu_decay_time;             /* LFU counter decay factor. */
    long long proto_max_bulk_len;   /* Protocol bulk length maximum size. */
    int oom_score_adj_base;         /* Base oom_score_adj value, as observed on startup */
    int oom_score_adj_values[CONFIG_OOM_COUNT];   /* Linux oom_score_adj configuration */
    int oom_score_adj;                            /* If true, oom_score_adj is managed */
    /* Blocked clients */
    unsigned int blocked_clients;   /* # of clients executing a blocking cmd.*/
    unsigned int blocked_clients_by_type[BLOCKED_NUM];
    // 非阻塞的client链表
    list *unblocked_clients; /* list of clients to unblock before next loop */
    // BLPOP 命令
    list *ready_keys;        /* List of readyList structures for BLPOP & co */
    /* Client side caching. */
    unsigned int tracking_clients;  /* # of clients with tracking enabled.*/
    size_t tracking_table_max_keys; /* Max number of keys in tracking table. */
    /* Sort parameters - qsort_r() is only available under BSD so we
     * have to take this state global, in order to pass it to sortCompare() */
    int sort_desc;
    int sort_alpha;
    int sort_bypattern;
    int sort_store;
    /* Zip structure config, see redis.conf for more information  */
    size_t hash_max_ziplist_entries;
    size_t hash_max_ziplist_value;
    size_t set_max_intset_entries;
    size_t zset_max_ziplist_entries;
    size_t zset_max_ziplist_value;
    size_t hll_sparse_max_bytes;
    size_t stream_node_max_bytes;
    long long stream_node_max_entries;
    /* List parameters */
    int list_max_ziplist_size;
    int list_compress_depth;
    /* time cache */
    // 保存秒单位的Unix时间戳的缓存，每次重启时设置
    _Atomic time_t unixtime;    /* Unix time sampled every cron cycle. */
    time_t timezone;            /* Cached timezone. As set by tzset(). */
    int daylight_active;        /* Currently in daylight saving time. */
    // 保存毫秒单位的Unix时间戳的缓存
    mstime_t mstime;            /* 'unixtime' in milliseconds. */
    ustime_t ustime;            /* 'unixtime' in microseconds. */
    /* Pubsub */
    dict *pubsub_channels;  /* Map channels to list of subscribed clients */
    list *pubsub_patterns;  /* A list of pubsub_patterns */
    dict *pubsub_patterns_dict;  /* A dict of pubsub_patterns */
    int notify_keyspace_events; /* Events to propagate via Pub/Sub. This is an
                                   xor of NOTIFY_... flags. */
    /* Cluster */
    // 集群模式是否开启
    int cluster_enabled;      /* Is cluster enabled? */
    // 集群节点超时时间
    mstime_t cluster_node_timeout; /* Cluster node timeout. */
    char *cluster_configfile; /* Cluster auto-generated config file name. */
    // 集群的状态
    struct clusterState *cluster;  /* State of the cluster */
    // 集群迁移障碍的节点数
    int cluster_migration_barrier; /* Cluster replicas migration barrier. */
    // 从节点故障转移的最长复制数据时间
    int cluster_slave_validity_factor; /* Slave max data age for failover. */
    // 如果为真，那么：如果有一个未指定的槽，那么将集群设置为下线状态
    int cluster_require_full_coverage; /* If true, put the cluster down if
                                          there is at least an uncovered slot.*/
    int cluster_slave_no_failover;  /* Prevent slave from starting a failover
                                       if the master is in failure state. */
    char *cluster_announce_ip;  /* IP address to announce on cluster bus. */
    int cluster_announce_port;     /* base port to announce on cluster bus. */
    int cluster_announce_bus_port; /* bus port to announce on cluster bus. */
    int cluster_module_flags;      /* Set of flags that Redis modules are able
                                      to set in order to suppress certain
                                      native Redis Cluster features. Check the
                                      REDISMODULE_CLUSTER_FLAG_*. */
    int cluster_allow_reads_when_down; /* Are reads allowed when the cluster
                                        is down? */
    int cluster_config_file_lock_fd;   /* cluster config fd, will be flock */
    /* Scripting */
    lua_State *lua; /* The Lua interpreter. We use just one for all clients */
    client *lua_client;   /* The "fake client" to query Redis from Lua */
    // client正在执行 EVAL 命令
    client *lua_caller;   /* The client running EVAL right now, or NULL */
    char* lua_cur_script; /* SHA1 of the script currently running, or NULL */
    dict *lua_scripts;         /* A dictionary of SHA1 -> Lua scripts */
    unsigned long long lua_scripts_mem;  /* Cached scripts' memory + oh */
    mstime_t lua_time_limit;  /* Script timeout in milliseconds */
    mstime_t lua_time_start;  /* Start time of script, milliseconds time */
    int lua_write_dirty;  /* True if a write command was called during the
                             execution of the current script. */
    int lua_random_dirty; /* True if a random command was called during the
                             execution of the current script. */
    int lua_replicate_commands; /* True if we are doing single commands repl. */
    int lua_multi_emitted;/* True if we already propagated MULTI. */
    int lua_repl;         /* Script replication flags for redis.set_repl(). */
    // 执行脚本超时
    int lua_timedout;     /* True if we reached the time limit for script
                             execution. */
    int lua_kill;         /* Kill the script if true. */
    int lua_always_replicate_commands; /* Default replication type. */
    int lua_oom;          /* OOM detected when script start? */
    /* Lazy free */
    int lazyfree_lazy_eviction;
    int lazyfree_lazy_expire;
    int lazyfree_lazy_server_del;
    int lazyfree_lazy_user_del;
    /* Latency monitor */
    long long latency_monitor_threshold;
    dict *latency_events;
    /* ACLs */
    char *acl_filename;     /* ACL Users file. NULL if not configured. */
    unsigned long acllog_max_len; /* Maximum length of the ACL LOG list. */
    sds requirepass;        /* Remember the cleartext password set with the
                               old "requirepass" directive for backward
                               compatibility with Redis <= 5. */
    /* Assert & bug reporting */
    const char *assert_failed;
    const char *assert_file;
    int assert_line;
    int bug_report_start; /* True if bug report header was already logged. */
    int watchdog_period;  /* Software watchdog period in ms. 0 = off */
    /* System hardware info */
    size_t system_memory_size;  /* Total memory in system as reported by OS */
    /* TLS Configuration */
    int tls_cluster;
    int tls_replication;
    int tls_auth_clients;
    redisTLSContextConfig tls_ctx_config;
    /* cpu affinity */
    char *server_cpulist; /* cpu affinity list of redis server main/io thread. */
    char *bio_cpulist; /* cpu affinity list of bio thread. */
    char *aof_rewrite_cpulist; /* cpu affinity list of aof rewrite process. */
    char *bgsave_cpulist; /* cpu affinity list of bgsave process. */
};

typedef struct pubsubPattern {
    client *client;
    robj *pattern;
} pubsubPattern;

#define MAX_KEYS_BUFFER 256

/* A result structure for the various getkeys function calls. It lists the
 * keys as indices to the provided argv.
 */
typedef struct {
    int keysbuf[MAX_KEYS_BUFFER];       /* Pre-allocated buffer, to save heap allocations */
    int *keys;                          /* Key indices array, points to keysbuf or heap */
    int numkeys;                        /* Number of key indices return */
    int size;                           /* Available array size */
} getKeysResult;
#define GETKEYS_RESULT_INIT { {0}, NULL, 0, MAX_KEYS_BUFFER }

typedef void redisCommandProc(client *c);
typedef int redisGetKeysProc(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

/* redis命令结构 */
struct redisCommand {
    // 命令名称
    char *name;
    // proc函数指针，指向返回值为void，参数为client *c的函数
    // 指向实现命令的函数
    redisCommandProc *proc;
    // 参数个数
    int arity;
    // 字符串形式的标示值
    char *sflags;   /* Flags as string representation, one char per flag. */
    // 实际的标示值
    uint64_t flags; /* The actual flags, obtained from the 'sflags' field. */
    /* Use a function to determine keys arguments in a command line.
     * Used for Redis Cluster redirect. */
    // getkeys_proc是函数指针，返回值是有个整型的数组
    // 从命令行判断该命令的参数
    redisGetKeysProc *getkeys_proc;
    /* What keys should be loaded in background when calling this command? */
    // 指定哪些参数是key
    int firstkey; /* 第一个参数是 key The first argument that's a key (0 = no keys) */
    int lastkey;  /* 最后一个参数是 key The last argument that's a key */
    int keystep;  /* 第一个参数和最后一个参数的步长  The step between first and last key */
    // microseconds记录执行命令的耗费总时长
    // calls记录命令被执行的总次数
    long long microseconds, calls;
    int id;     /* Command ID. This is a progressive ID starting from 0 that
                   is assigned at runtime, and is used in order to check
                   ACLs. A connection is able to execute a given command if
                   the user associated to the connection has this command
                   bit set in the bitmap of allowed commands. */
};

struct redisFunctionSym {
    char *name;
    unsigned long pointer;
};

// 排序操作
typedef struct _redisSortObject {
    // 用户给定的模式
    robj *obj;
    union {
        double score;
        robj *cmpobj;
    } u;
} redisSortObject;

typedef struct _redisSortOperation {
    int type;
    robj *pattern;
} redisSortOperation;

/* Structure to hold list iteration abstraction. */
typedef struct {
    robj *subject;          //迭代器指向的对象
    unsigned char encoding; //迭代器指向对象的编码类型
    unsigned char direction;//迭代器的方向
    quicklistIter *iter;    //quicklist的迭代器
} listTypeIterator;         //列表类型迭代器

/* Structure for an entry while iterating over a list. */
typedef struct {
    listTypeIterator *li;   //所属的列表类型迭代器
    quicklistEntry entry;   //quicklist中的entry结构
} listTypeEntry;            //列表类型的entry结构

/* Structure to hold set iteration abstraction. */
typedef struct {
    robj *subject;                  //所属的集合对象
    int encoding;                   //集合对象编码类型
    int ii; /* intset iterator */   //整数集合的迭代器，编码为INTSET使用
    dictIterator *di;               //字典的迭代器，编码为HT使用
} setTypeIterator;

/* Structure to hold hash iteration abstraction. Note that iteration over
 * hashes involves both fields and values. Because it is possible that
 * not both are required, store pointers in the iterator to avoid
 * unnecessary memory allocation for fields/values. */
typedef struct {
    robj *subject;              // 哈希类型迭代器所属的哈希对象
    int encoding;               // 哈希对象的编码类型

    //ziplist
    unsigned char *fptr, *vptr; // 指向当前的key和value节点的地址，ziplist类型编码时使用

    //字典
    dictIterator *di;           // 迭代HT类型的哈希对象时的字典迭代器
    dictEntry *de;              // 指向当前的哈希表节点
} hashTypeIterator;

#include "stream.h"  /* Stream data type header file. */

#define OBJ_HASH_KEY 1
#define OBJ_HASH_VALUE 2

/*-----------------------------------------------------------------------------
 * Extern declarations
 *----------------------------------------------------------------------------*/

extern struct redisServer server;
extern struct sharedObjectsStruct shared;
extern dictType objectKeyPointerValueDictType;
extern dictType objectKeyHeapPointerValueDictType;
extern dictType setDictType;
extern dictType zsetDictType;
extern dictType clusterNodesDictType;
extern dictType clusterNodesBlackListDictType;
extern dictType dbDictType;
extern dictType shaScriptObjectDictType;
extern double R_Zero, R_PosInf, R_NegInf, R_Nan;
extern dictType hashDictType;
extern dictType replScriptCacheDictType;
extern dictType keyptrDictType;
extern dictType modulesDictType;

/*-----------------------------------------------------------------------------
 * Functions prototypes
 *----------------------------------------------------------------------------*/

/* Modules */
void moduleInitModulesSystem(void);
int moduleLoad(const char *path, void **argv, int argc);
void moduleLoadFromQueue(void);
int moduleGetCommandKeysViaAPI(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
moduleType *moduleTypeLookupModuleByID(uint64_t id);
void moduleTypeNameByID(char *name, uint64_t moduleid);
void moduleFreeContext(struct RedisModuleCtx *ctx);
void unblockClientFromModule(client *c);
void moduleHandleBlockedClients(void);
void moduleBlockedClientTimedOut(client *c);
void moduleBlockedClientPipeReadable(aeEventLoop *el, int fd, void *privdata, int mask);
size_t moduleCount(void);
void moduleAcquireGIL(void);
int moduleTryAcquireGIL(void);
void moduleReleaseGIL(void);
void moduleNotifyKeyspaceEvent(int type, const char *event, robj *key, int dbid);
void moduleCallCommandFilters(client *c);
void ModuleForkDoneHandler(int exitcode, int bysignal);
int TerminateModuleForkChild(int child_pid, int wait);
ssize_t rdbSaveModulesAux(rio *rdb, int when);
int moduleAllDatatypesHandleErrors();
sds modulesCollectInfo(sds info, const char *section, int for_crash_report, int sections);
void moduleFireServerEvent(uint64_t eid, int subid, void *data);
void processModuleLoadingProgressEvent(int is_aof);
int moduleTryServeClientBlockedOnKey(client *c, robj *key);
void moduleUnblockClient(client *c);
int moduleClientIsBlockedOnKeys(client *c);
void moduleNotifyUserChanged(client *c);

/* Utils */
long long ustime(void);
long long mstime(void);
void getRandomHexChars(char *p, size_t len);
void getRandomBytes(unsigned char *p, size_t len);
uint64_t crc64(uint64_t crc, const unsigned char *s, uint64_t l);
void exitFromChild(int retcode);
size_t redisPopcount(void *s, long count);
void redisSetProcTitle(char *title);
int redisCommunicateSystemd(const char *sd_notify_msg);
void redisSetCpuAffinity(const char *cpulist);

/* networking.c -- Networking and Client related operations */
client *createClient(connection *conn);
void closeTimedoutClients(void);
void freeClient(client *c);
void freeClientAsync(client *c);
void resetClient(client *c);
void sendReplyToClient(connection *conn);
void *addReplyDeferredLen(client *c);
void setDeferredArrayLen(client *c, void *node, long length);
void setDeferredMapLen(client *c, void *node, long length);
void setDeferredSetLen(client *c, void *node, long length);
void setDeferredAttributeLen(client *c, void *node, long length);
void setDeferredPushLen(client *c, void *node, long length);
void processInputBuffer(client *c);
void processGopherRequest(client *c);
void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptTLSHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void readQueryFromClient(connection *conn);
void addReplyNull(client *c);
void addReplyNullArray(client *c);
void addReplyBool(client *c, int b);
void addReplyVerbatim(client *c, const char *s, size_t len, const char *ext);
void addReplyProto(client *c, const char *s, size_t len);
void AddReplyFromClient(client *c, client *src);
void addReplyBulk(client *c, robj *obj);
void addReplyBulkCString(client *c, const char *s);
void addReplyBulkCBuffer(client *c, const void *p, size_t len);
void addReplyBulkLongLong(client *c, long long ll);
void addReply(client *c, robj *obj);
void addReplySds(client *c, sds s);
void addReplyBulkSds(client *c, sds s);
void addReplyErrorObject(client *c, robj *err);
void addReplyErrorSds(client *c, sds err);
void addReplyError(client *c, const char *err);
void addReplyStatus(client *c, const char *status);
void addReplyDouble(client *c, double d);
void addReplyHumanLongDouble(client *c, long double d);
void addReplyLongLong(client *c, long long ll);
void addReplyArrayLen(client *c, long length);
void addReplyMapLen(client *c, long length);
void addReplySetLen(client *c, long length);
void addReplyAttributeLen(client *c, long length);
void addReplyPushLen(client *c, long length);
void addReplyHelp(client *c, const char **help);
void addReplySubcommandSyntaxError(client *c);
void addReplyLoadedModules(client *c);
void copyClientOutputBuffer(client *dst, client *src);
size_t sdsZmallocSize(sds s);
size_t getStringObjectSdsUsedMemory(robj *o);
void freeClientReplyValue(void *o);
void *dupClientReplyValue(void *o);
void getClientsMaxBuffers(unsigned long *longest_output_list,
                          unsigned long *biggest_input_buffer);
char *getClientPeerId(client *client);
sds catClientInfoString(sds s, client *client);
sds getAllClientsInfoString(int type);
void rewriteClientCommandVector(client *c, int argc, ...);
void rewriteClientCommandArgument(client *c, int i, robj *newval);
void replaceClientCommandVector(client *c, int argc, robj **argv);
unsigned long getClientOutputBufferMemoryUsage(client *c);
int freeClientsInAsyncFreeQueue(void);
void asyncCloseClientOnOutputBufferLimitReached(client *c);
int getClientType(client *c);
int getClientTypeByName(char *name);
char *getClientTypeName(int class);
void flushSlavesOutputBuffers(void);
void disconnectSlaves(void);
int listenToPort(int port, int *fds, int *count);
void pauseClients(mstime_t duration);
int clientsArePaused(void);
void processEventsWhileBlocked(void);
int handleClientsWithPendingWrites(void);
int handleClientsWithPendingWritesUsingThreads(void);
int handleClientsWithPendingReadsUsingThreads(void);
int stopThreadedIOIfNeeded(void);
int clientHasPendingReplies(client *c);
void unlinkClient(client *c);
int writeToClient(client *c, int handler_installed);
void linkClient(client *c);
void protectClient(client *c);
void unprotectClient(client *c);
void initThreadedIO(void);
client *lookupClientByID(uint64_t id);

#ifdef __GNUC__
void addReplyErrorFormat(client *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void addReplyStatusFormat(client *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void addReplyErrorFormat(client *c, const char *fmt, ...);
void addReplyStatusFormat(client *c, const char *fmt, ...);
#endif

/* Client side caching (tracking mode) */
void enableTracking(client *c, uint64_t redirect_to, uint64_t options, robj **prefix, size_t numprefix);
void disableTracking(client *c);
void trackingRememberKeys(client *c);
void trackingInvalidateKey(client *c, robj *keyobj);
void trackingInvalidateKeysOnFlush(int dbid);
void trackingLimitUsedSlots(void);
uint64_t trackingGetTotalItems(void);
uint64_t trackingGetTotalKeys(void);
uint64_t trackingGetTotalPrefixes(void);
void trackingBroadcastInvalidationMessages(void);

/* List data type */
void listTypeTryConversion(robj *subject, robj *value);
void listTypePush(robj *subject, robj *value, int where);
robj *listTypePop(robj *subject, int where);
unsigned long listTypeLength(const robj *subject);
listTypeIterator *listTypeInitIterator(robj *subject, long index, unsigned char direction);
void listTypeReleaseIterator(listTypeIterator *li);
int listTypeNext(listTypeIterator *li, listTypeEntry *entry);
robj *listTypeGet(listTypeEntry *entry);
void listTypeInsert(listTypeEntry *entry, robj *value, int where);
int listTypeEqual(listTypeEntry *entry, robj *o);
void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry);
void listTypeConvert(robj *subject, int enc);
void unblockClientWaitingData(client *c);
void popGenericCommand(client *c, int where);

/* MULTI/EXEC/WATCH... */
void unwatchAllKeys(client *c);
void initClientMultiState(client *c);
void freeClientMultiState(client *c);
void queueMultiCommand(client *c);
void touchWatchedKey(redisDb *db, robj *key);
void touchWatchedKeysOnFlush(int dbid);
void discardTransaction(client *c);
void flagTransaction(client *c);
void execCommandAbort(client *c, sds error);
void execCommandPropagateMulti(client *c);
void execCommandPropagateExec(client *c);

/* Redis object implementation */
void decrRefCount(robj *o);
void decrRefCountVoid(void *o);
void incrRefCount(robj *o);
robj *makeObjectShared(robj *o);
robj *resetRefCount(robj *obj);
void freeStringObject(robj *o);
void freeListObject(robj *o);
void freeSetObject(robj *o);
void freeZsetObject(robj *o);
void freeHashObject(robj *o);
robj *createObject(int type, void *ptr);
robj *createStringObject(const char *ptr, size_t len);
robj *createRawStringObject(const char *ptr, size_t len);
robj *createEmbeddedStringObject(const char *ptr, size_t len);
robj *dupStringObject(const robj *o);
int isSdsRepresentableAsLongLong(sds s, long long *llval);
int isObjectRepresentableAsLongLong(robj *o, long long *llongval);
robj *tryObjectEncoding(robj *o);
robj *getDecodedObject(robj *o);
size_t stringObjectLen(robj *o);
robj *createStringObjectFromLongLong(long long value);
robj *createStringObjectFromLongLongForValue(long long value);
robj *createStringObjectFromLongDouble(long double value, int humanfriendly);
robj *createQuicklistObject(void);
robj *createZiplistObject(void);
robj *createSetObject(void);
robj *createIntsetObject(void);
robj *createHashObject(void);
robj *createZsetObject(void);
robj *createZsetZiplistObject(void);
robj *createStreamObject(void);
robj *createModuleObject(moduleType *mt, void *value);
int getLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg);
int checkType(client *c, robj *o, int type);
int getLongLongFromObjectOrReply(client *c, robj *o, long long *target, const char *msg);
int getDoubleFromObjectOrReply(client *c, robj *o, double *target, const char *msg);
int getDoubleFromObject(const robj *o, double *target);
int getLongLongFromObject(robj *o, long long *target);
int getLongDoubleFromObject(robj *o, long double *target);
int getLongDoubleFromObjectOrReply(client *c, robj *o, long double *target, const char *msg);
char *strEncoding(int encoding);
int compareStringObjects(robj *a, robj *b);
int collateStringObjects(robj *a, robj *b);
int equalStringObjects(robj *a, robj *b);
unsigned long long estimateObjectIdleTime(robj *o);
void trimStringObjectIfNeeded(robj *o);
#define sdsEncodedObject(objptr) (objptr->encoding == OBJ_ENCODING_RAW || objptr->encoding == OBJ_ENCODING_EMBSTR)

/* Synchronous I/O with timeout */
ssize_t syncWrite(int fd, char *ptr, ssize_t size, long long timeout);
ssize_t syncRead(int fd, char *ptr, ssize_t size, long long timeout);
ssize_t syncReadLine(int fd, char *ptr, ssize_t size, long long timeout);

/* Replication */
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc);
void replicationFeedSlavesFromMasterStream(list *slaves, char *buf, size_t buflen);
void replicationFeedMonitors(client *c, list *monitors, int dictid, robj **argv, int argc);
void updateSlavesWaitingBgsave(int bgsaveerr, int type);
void replicationCron(void);
void replicationHandleMasterDisconnection(void);
void replicationCacheMaster(client *c);
void resizeReplicationBacklog(long long newsize);
void replicationSetMaster(char *ip, int port);
void replicationUnsetMaster(void);
void refreshGoodSlavesCount(void);
void replicationScriptCacheInit(void);
void replicationScriptCacheFlush(void);
void replicationScriptCacheAdd(sds sha1);
int replicationScriptCacheExists(sds sha1);
void processClientsWaitingReplicas(void);
void unblockClientWaitingReplicas(client *c);
int replicationCountAcksByOffset(long long offset);
void replicationSendNewlineToMaster(void);
long long replicationGetSlaveOffset(void);
char *replicationGetSlaveName(client *c);
long long getPsyncInitialOffset(void);
int replicationSetupSlaveForFullResync(client *slave, long long offset);
void changeReplicationId(void);
void clearReplicationId2(void);
void chopReplicationBacklog(void);
void replicationCacheMasterUsingMyself(void);
void feedReplicationBacklog(void *ptr, size_t len);
void showLatestBacklog(void);
void rdbPipeReadHandler(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
void rdbPipeWriteHandlerConnRemoved(struct connection *conn);

/* Generic persistence functions */
void startLoadingFile(FILE* fp, char* filename, int rdbflags);
void startLoading(size_t size, int rdbflags);
void loadingProgress(off_t pos);
void stopLoading(int success);
void startSaving(int rdbflags);
void stopSaving(int success);
int allPersistenceDisabled(void);

#define DISK_ERROR_TYPE_AOF 1       /* Don't accept writes: AOF errors. */
#define DISK_ERROR_TYPE_RDB 2       /* Don't accept writes: RDB errors. */
#define DISK_ERROR_TYPE_NONE 0      /* No problems, we can accept writes. */
int writeCommandsDeniedByDiskError(void);

/* RDB persistence */
#include "rdb.h"
void killRDBChild(void);
int bg_unlink(const char *filename);

/* AOF persistence */
void flushAppendOnlyFile(int force);
void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc);
void aofRemoveTempFile(pid_t childpid);
int rewriteAppendOnlyFileBackground(void);
int loadAppendOnlyFile(char *filename);
void stopAppendOnly(void);
int startAppendOnly(void);
void backgroundRewriteDoneHandler(int exitcode, int bysignal);
void aofRewriteBufferReset(void);
unsigned long aofRewriteBufferSize(void);
ssize_t aofReadDiffFromParent(void);
void killAppendOnlyChild(void);
void restartAOFAfterSYNC();

/* Child info */
void openChildInfoPipe(void);
void closeChildInfoPipe(void);
void sendChildInfo(int process_type);
void receiveChildInfo(void);

/* Fork helpers */
int redisFork(int type);
int hasActiveChildProcess();
void sendChildCOWInfo(int ptype, char *pname);

/* acl.c -- Authentication related prototypes. */
extern rax *Users;
extern user *DefaultUser;
void ACLInit(void);
/* Return values for ACLCheckUserCredentials(). */
#define ACL_OK 0
#define ACL_DENIED_CMD 1
#define ACL_DENIED_KEY 2
#define ACL_DENIED_AUTH 3 /* Only used for ACL LOG entries. */
int ACLCheckUserCredentials(robj *username, robj *password);
int ACLAuthenticateUser(client *c, robj *username, robj *password);
unsigned long ACLGetCommandID(const char *cmdname);
user *ACLGetUserByName(const char *name, size_t namelen);
int ACLCheckCommandPerm(client *c, int *keyidxptr);
int ACLSetUser(user *u, const char *op, ssize_t oplen);
sds ACLDefaultUserFirstPassword(void);
uint64_t ACLGetCommandCategoryFlagByName(const char *name);
int ACLAppendUserForLoading(sds *argv, int argc, int *argc_err);
char *ACLSetUserStringError(void);
int ACLLoadConfiguredUsers(void);
sds ACLDescribeUser(user *u);
void ACLLoadUsersAtStartup(void);
void addReplyCommandCategories(client *c, struct redisCommand *cmd);
user *ACLCreateUnlinkedUser();
void ACLFreeUserAndKillClients(user *u);
void addACLLogEntry(client *c, int reason, int keypos, sds username);

/* Sorted sets data type */

/* Input flags. */
#define ZADD_NONE 0
#define ZADD_INCR (1<<0)    /* Increment the score instead of setting it. */
#define ZADD_NX (1<<1)      /* Don't touch elements not already existing. */
#define ZADD_XX (1<<2)      /* Only touch elements already existing. */

/* Output flags. */
#define ZADD_NOP (1<<3)     /* Operation not performed because of conditionals.*/
#define ZADD_NAN (1<<4)     /* Only touch elements already existing. */
#define ZADD_ADDED (1<<5)   /* The element was new and was added. */
#define ZADD_UPDATED (1<<6) /* The element already existed, score updated. */

/* Flags only used by the ZADD command but not by zsetAdd() API: */
#define ZADD_CH (1<<16)      /* Return num of elements added or updated. */

/* Struct to hold an inclusive/exclusive range spec by score comparison. */
typedef struct {
    double min, max;     //最小值和最大值

    //minex表示最小值是否包含在这个范围，1表示不包含，0表示包含
    //同理maxex表示最大值
    int minex, maxex; /* are min or max exclusive? */
} zrangespec;

/* Struct to hold an inclusive/exclusive range spec by lexicographic comparison. */
typedef struct {
    sds min, max;     /* May be set to shared.(minstring|maxstring) */ //最小值和最大值

    //minex表示最小值是否包含在这个范围，1表示不包含，0表示包含
    //同理maxex表示最大值
    int minex, maxex; /* are min or max exclusive? */
} zlexrangespec;

//创建返回一个跳跃表 表头zskiplist
zskiplist *zslCreate(void);
void zslFree(zskiplist *zsl);
zskiplistNode *zslInsert(zskiplist *zsl, double score, sds ele);
unsigned char *zzlInsert(unsigned char *zl, sds ele, double score);
int zslDelete(zskiplist *zsl, double score, sds ele, zskiplistNode **node);
zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range);
zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range);
double zzlGetScore(unsigned char *sptr);
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec *range);
unsigned char *zzlLastInRange(unsigned char *zl, zrangespec *range);
unsigned long zsetLength(const robj *zobj);
void zsetConvert(robj *zobj, int encoding);
void zsetConvertToZiplistIfNeeded(robj *zobj, size_t maxelelen);
int zsetScore(robj *zobj, sds member, double *score);
unsigned long zslGetRank(zskiplist *zsl, double score, sds o);
int zsetAdd(robj *zobj, double score, sds ele, int *flags, double *newscore);
long zsetRank(robj *zobj, sds ele, int reverse);
int zsetDel(robj *zobj, sds ele);
void genericZpopCommand(client *c, robj **keyv, int keyc, int where, int emitkey, robj *countarg);
sds ziplistGetObject(unsigned char *sptr);
int zslValueGteMin(double value, zrangespec *spec);//判断给定值value是否大于或等于范围spec中的min，返回1表示value大于或等于min，否则返回0。
int zslValueLteMax(double value, zrangespec *spec);//判断给定值value是否小于或等于范围spec中的max，返回1表示value小于或等于max，否则返回0。
void zslFreeLexRange(zlexrangespec *spec);
int zslParseLexRange(robj *min, robj *max, zlexrangespec *spec);
unsigned char *zzlFirstInLexRange(unsigned char *zl, zlexrangespec *range);
unsigned char *zzlLastInLexRange(unsigned char *zl, zlexrangespec *range);
zskiplistNode *zslFirstInLexRange(zskiplist *zsl, zlexrangespec *range);
zskiplistNode *zslLastInLexRange(zskiplist *zsl, zlexrangespec *range);
int zzlLexValueGteMin(unsigned char *p, zlexrangespec *spec);
int zzlLexValueLteMax(unsigned char *p, zlexrangespec *spec);
int zslLexValueGteMin(sds value, zlexrangespec *spec);
int zslLexValueLteMax(sds value, zlexrangespec *spec);

/* Core functions */
int getMaxmemoryState(size_t *total, size_t *logical, size_t *tofree, float *level);
size_t freeMemoryGetNotCountedMemory();
int freeMemoryIfNeeded(void);
int freeMemoryIfNeededAndSafe(void);
int processCommand(client *c);
void setupSignalHandlers(void);
struct redisCommand *lookupCommand(sds name);
struct redisCommand *lookupCommandByCString(const char *s);
struct redisCommand *lookupCommandOrOriginal(sds name);
void call(client *c, int flags);
void propagate(struct redisCommand *cmd, int dbid, robj **argv, int argc, int flags);
void alsoPropagate(struct redisCommand *cmd, int dbid, robj **argv, int argc, int target);
void redisOpArrayInit(redisOpArray *oa);
void redisOpArrayFree(redisOpArray *oa);
void forceCommandPropagation(client *c, int flags);
void preventCommandPropagation(client *c);
void preventCommandAOF(client *c);
void preventCommandReplication(client *c);
int prepareForShutdown(int flags);
#ifdef __GNUC__
void serverLog(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void serverLog(int level, const char *fmt, ...);
#endif
void serverLogRaw(int level, const char *msg);
void serverLogFromHandler(int level, const char *msg);
void usage(void);
void updateDictResizePolicy(void);
int htNeedsResize(dict *dict);
void populateCommandTable(void);
void resetCommandTableStats(void);
void adjustOpenFilesLimit(void);
void closeListeningSockets(int unlink_unix_socket);
void updateCachedTime(int update_daylight_info);
void resetServerStats(void);
void activeDefragCycle(void);
unsigned int getLRUClock(void);
unsigned int LRU_CLOCK(void);
const char *evictPolicyToString(void);
struct redisMemOverhead *getMemoryOverheadData(void);
void freeMemoryOverheadData(struct redisMemOverhead *mh);
void checkChildrenDone(void);
int setOOMScoreAdj(int process_class);

#define RESTART_SERVER_NONE 0
#define RESTART_SERVER_GRACEFULLY (1<<0)     /* Do proper shutdown. */
#define RESTART_SERVER_CONFIG_REWRITE (1<<1) /* CONFIG REWRITE before restart.*/
int restartServer(int flags, mstime_t delay);

/* Set data type */
// 创建一个保存value的集合
robj *setTypeCreate(sds value);
int setTypeAdd(robj *subject, sds value);
int setTypeRemove(robj *subject, sds value);
int setTypeIsMember(robj *subject, sds value);
setTypeIterator *setTypeInitIterator(robj *subject);
void setTypeReleaseIterator(setTypeIterator *si);
int setTypeNext(setTypeIterator *si, sds *sdsele, int64_t *llele);
sds setTypeNextObject(setTypeIterator *si);
int setTypeRandomElement(robj *setobj, sds *sdsele, int64_t *llele);
unsigned long setTypeRandomElements(robj *set, unsigned long count, robj *aux_set);
unsigned long setTypeSize(const robj *subject);
void setTypeConvert(robj *subject, int enc);

/* Hash data type */
#define HASH_SET_TAKE_FIELD (1<<0)
#define HASH_SET_TAKE_VALUE (1<<1)
#define HASH_SET_COPY 0

void hashTypeConvert(robj *o, int enc);
void hashTypeTryConversion(robj *subject, robj **argv, int start, int end);
int hashTypeExists(robj *o, sds key);
int hashTypeDelete(robj *o, sds key);
unsigned long hashTypeLength(const robj *o);
hashTypeIterator *hashTypeInitIterator(robj *subject);
void hashTypeReleaseIterator(hashTypeIterator *hi);
int hashTypeNext(hashTypeIterator *hi);
void hashTypeCurrentFromZiplist(hashTypeIterator *hi, int what,
                                unsigned char **vstr,
                                unsigned int *vlen,
                                long long *vll);
sds hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what);
void hashTypeCurrentObject(hashTypeIterator *hi, int what, unsigned char **vstr, unsigned int *vlen, long long *vll);
sds hashTypeCurrentObjectNewSds(hashTypeIterator *hi, int what);
robj *hashTypeLookupWriteOrCreate(client *c, robj *key);
robj *hashTypeGetValueObject(robj *o, sds field);
int hashTypeSet(robj *o, sds field, sds value, int flags);

/* Pub / Sub */
int pubsubUnsubscribeAllChannels(client *c, int notify);
int pubsubUnsubscribeAllPatterns(client *c, int notify);
void freePubsubPattern(void *p);
int listMatchPubsubPattern(void *a, void *b);
int pubsubPublishMessage(robj *channel, robj *message);
void addReplyPubsubMessage(client *c, robj *channel, robj *msg);

/* Keyspace events notification */
void notifyKeyspaceEvent(int type, char *event, robj *key, int dbid);
int keyspaceEventsStringToFlags(char *classes);
sds keyspaceEventsFlagsToString(int flags);

/* Configuration */
void loadServerConfig(char *filename, char *options);
void appendServerSaveParams(time_t seconds, int changes);
void resetServerSaveParams(void);
struct rewriteConfigState; /* Forward declaration to export API. */
void rewriteConfigRewriteLine(struct rewriteConfigState *state, const char *option, sds line, int force);
int rewriteConfig(char *path, int force_all);
void initConfigValues();

/* db.c -- Keyspace access API */
int removeExpire(redisDb *db, robj *key);
void propagateExpire(redisDb *db, robj *key, int lazy);
int expireIfNeeded(redisDb *db, robj *key);
long long getExpire(redisDb *db, robj *key);
void setExpire(client *c, redisDb *db, robj *key, long long when);
int checkAlreadyExpired(long long when);
robj *lookupKey(redisDb *db, robj *key, int flags);
robj *lookupKeyRead(redisDb *db, robj *key);
robj *lookupKeyWrite(redisDb *db, robj *key);
robj *lookupKeyReadOrReply(client *c, robj *key, robj *reply);
robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply);
robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags);
robj *lookupKeyWriteWithFlags(redisDb *db, robj *key, int flags);
robj *objectCommandLookup(client *c, robj *key);
robj *objectCommandLookupOrReply(client *c, robj *key, robj *reply);
int objectSetLRUOrLFU(robj *val, long long lfu_freq, long long lru_idle,
                       long long lru_clock, int lru_multiplier);
#define LOOKUP_NONE 0
#define LOOKUP_NOTOUCH (1<<0)
void dbAdd(redisDb *db, robj *key, robj *val);
int dbAddRDBLoad(redisDb *db, sds key, robj *val);
void dbOverwrite(redisDb *db, robj *key, robj *val);
void genericSetKey(client *c, redisDb *db, robj *key, robj *val, int keepttl, int signal);
void setKey(client *c, redisDb *db, robj *key, robj *val);
int dbExists(redisDb *db, robj *key);
robj *dbRandomKey(redisDb *db);
int dbSyncDelete(redisDb *db, robj *key);
int dbDelete(redisDb *db, robj *key);
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o);

#define EMPTYDB_NO_FLAGS 0      /* No flags. */
#define EMPTYDB_ASYNC (1<<0)    /* Reclaim memory in another thread. */
#define EMPTYDB_BACKUP (1<<2)   /* DB array is a backup for REPL_DISKLESS_LOAD_SWAPDB. */
long long emptyDb(int dbnum, int flags, void(callback)(void*));
long long emptyDbGeneric(redisDb *dbarray, int dbnum, int flags, void(callback)(void*));
void flushAllDataAndResetRDB(int flags);
long long dbTotalServerKeyCount();

int selectDb(client *c, int id);
void signalModifiedKey(client *c, redisDb *db, robj *key);
void signalFlushedDb(int dbid);
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count);
unsigned int countKeysInSlot(unsigned int hashslot);
unsigned int delKeysInSlot(unsigned int hashslot);
int verifyClusterConfigWithData(void);
void scanGenericCommand(client *c, robj *o, unsigned long cursor);
int parseScanCursorOrReply(client *c, robj *o, unsigned long *cursor);
void slotToKeyAdd(sds key);
void slotToKeyDel(sds key);
void slotToKeyFlush(void);
int dbAsyncDelete(redisDb *db, robj *key);
void emptyDbAsync(redisDb *db);
void slotToKeyFlushAsync(void);
size_t lazyfreeGetPendingObjectsCount(void);
void freeObjAsync(robj *o);

/* API to get key arguments from commands */
int *getKeysPrepareResult(getKeysResult *result, int numkeys);
int getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
void getKeysFreeResult(getKeysResult *result);
int zunionInterGetKeys(struct redisCommand *cmd,robj **argv, int argc, getKeysResult *result);
int evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int georadiusGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int xreadGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int memoryGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);
int lcsGetKeys(struct redisCommand *cmd, robj **argv, int argc, getKeysResult *result);

/* Cluster */
void clusterInit(void);
unsigned short crc16(const char *buf, int len);
unsigned int keyHashSlot(char *key, int keylen);
void clusterCron(void);
void clusterPropagatePublish(robj *channel, robj *message);
void migrateCloseTimedoutSockets(void);
void clusterBeforeSleep(void);
int clusterSendModuleMessageToTarget(const char *target, uint64_t module_id, uint8_t type, unsigned char *payload, uint32_t len);

/* Sentinel */
void initSentinelConfig(void);
void initSentinel(void);
void sentinelTimer(void);
char *sentinelHandleConfiguration(char **argv, int argc);
void sentinelIsRunning(void);

/* redis-check-rdb & aof */
int redis_check_rdb(char *rdbfilename, FILE *fp);
int redis_check_rdb_main(int argc, char **argv, FILE *fp);
int redis_check_aof_main(int argc, char **argv);

/* Scripting */
void scriptingInit(int setup);
int ldbRemoveChild(pid_t pid);
void ldbKillForkedSessions(void);
int ldbPendingChildren(void);
sds luaCreateFunction(client *c, lua_State *lua, robj *body);

/* Blocked clients */
void processUnblockedClients(void);
void blockClient(client *c, int btype);
void unblockClient(client *c);
void queueClientForReprocessing(client *c);
void replyToBlockedClientTimedOut(client *c);
int getTimeoutFromObjectOrReply(client *c, robj *object, mstime_t *timeout, int unit);
void disconnectAllBlockedClients(void);
void handleClientsBlockedOnKeys(void);
void signalKeyAsReady(redisDb *db, robj *key);
void blockForKeys(client *c, int btype, robj **keys, int numkeys, mstime_t timeout, robj *target, streamID *ids);

/* timeout.c -- Blocked clients timeout and connections timeout. */
void addClientToTimeoutTable(client *c);
void removeClientFromTimeoutTable(client *c);
void handleBlockedClientsTimeout(void);
int clientsCronHandleTimeout(client *c, mstime_t now_ms);

/* expire.c -- Handling of expired keys */
void activeExpireCycle(int type);
void expireSlaveKeys(void);
void rememberSlaveKeyWithExpire(redisDb *db, robj *key);
void flushSlaveKeysWithExpireList(void);
size_t getSlaveKeyWithExpireCount(void);

/* evict.c -- maxmemory handling and LRU eviction. */
void evictionPoolAlloc(void);
#define LFU_INIT_VAL 5
unsigned long LFUGetTimeInMinutes(void);
uint8_t LFULogIncr(uint8_t value);
unsigned long LFUDecrAndReturn(robj *o);

/* Keys hashing / comparison functions for dict.c hash tables. */
uint64_t dictSdsHash(const void *key);
int dictSdsKeyCompare(void *privdata, const void *key1, const void *key2);
void dictSdsDestructor(void *privdata, void *val);

/* Git SHA1 */
char *redisGitSHA1(void);
char *redisGitDirty(void);
uint64_t redisBuildId(void);
char *redisBuildIdString(void);

/* Commands prototypes */
void authCommand(client *c);
void pingCommand(client *c);
void echoCommand(client *c);
void commandCommand(client *c);
void setCommand(client *c);
void setnxCommand(client *c);
void setexCommand(client *c);
void psetexCommand(client *c);
void getCommand(client *c);
void delCommand(client *c);
void unlinkCommand(client *c);
void existsCommand(client *c);
void setbitCommand(client *c);
void getbitCommand(client *c);
void bitfieldCommand(client *c);
void bitfieldroCommand(client *c);
void setrangeCommand(client *c);
void getrangeCommand(client *c);
void incrCommand(client *c);
void decrCommand(client *c);
void incrbyCommand(client *c);
void decrbyCommand(client *c);
void incrbyfloatCommand(client *c);
void selectCommand(client *c);
void swapdbCommand(client *c);
void randomkeyCommand(client *c);
void keysCommand(client *c);
void scanCommand(client *c);
void dbsizeCommand(client *c);
void lastsaveCommand(client *c);
void saveCommand(client *c);
void bgsaveCommand(client *c);
void bgrewriteaofCommand(client *c);
void shutdownCommand(client *c);
void moveCommand(client *c);
void renameCommand(client *c);
void renamenxCommand(client *c);
void lpushCommand(client *c);
void rpushCommand(client *c);
void lpushxCommand(client *c);
void rpushxCommand(client *c);
void linsertCommand(client *c);
void lpopCommand(client *c);
void rpopCommand(client *c);
void llenCommand(client *c);
void lindexCommand(client *c);
void lrangeCommand(client *c);
void ltrimCommand(client *c);
void typeCommand(client *c);
void lsetCommand(client *c);
void saddCommand(client *c);
void sremCommand(client *c);
void smoveCommand(client *c);
void sismemberCommand(client *c);
void scardCommand(client *c);
void spopCommand(client *c);
void srandmemberCommand(client *c);
void sinterCommand(client *c);
void sinterstoreCommand(client *c);
void sunionCommand(client *c);
void sunionstoreCommand(client *c);
void sdiffCommand(client *c);
void sdiffstoreCommand(client *c);
void sscanCommand(client *c);
void syncCommand(client *c);
void flushdbCommand(client *c);
void flushallCommand(client *c);
void sortCommand(client *c);
void lremCommand(client *c);
void lposCommand(client *c);
void rpoplpushCommand(client *c);
void infoCommand(client *c);
void mgetCommand(client *c);
void monitorCommand(client *c);
void expireCommand(client *c);
void expireatCommand(client *c);
void pexpireCommand(client *c);
void pexpireatCommand(client *c);
void getsetCommand(client *c);
void ttlCommand(client *c);
void touchCommand(client *c);
void pttlCommand(client *c);
void persistCommand(client *c);
void replicaofCommand(client *c);
void roleCommand(client *c);
void debugCommand(client *c);
void msetCommand(client *c);
void msetnxCommand(client *c);
void zaddCommand(client *c);
void zincrbyCommand(client *c);
void zrangeCommand(client *c);
void zrangebyscoreCommand(client *c);
void zrevrangebyscoreCommand(client *c);
void zrangebylexCommand(client *c);
void zrevrangebylexCommand(client *c);
void zcountCommand(client *c);
void zlexcountCommand(client *c);
void zrevrangeCommand(client *c);
void zcardCommand(client *c);
void zremCommand(client *c);
void zscoreCommand(client *c);
void zremrangebyscoreCommand(client *c);
void zremrangebylexCommand(client *c);
void zpopminCommand(client *c);
void zpopmaxCommand(client *c);
void bzpopminCommand(client *c);
void bzpopmaxCommand(client *c);
void multiCommand(client *c);
void execCommand(client *c);
void discardCommand(client *c);
void blpopCommand(client *c);
void brpopCommand(client *c);
void brpoplpushCommand(client *c);
void appendCommand(client *c);
void strlenCommand(client *c);
void zrankCommand(client *c);
void zrevrankCommand(client *c);
void hsetCommand(client *c);
void hsetnxCommand(client *c);
void hgetCommand(client *c);
void hmsetCommand(client *c);
void hmgetCommand(client *c);
void hdelCommand(client *c);
void hlenCommand(client *c);
void hstrlenCommand(client *c);
void zremrangebyrankCommand(client *c);
void zunionstoreCommand(client *c);
void zinterstoreCommand(client *c);
void zscanCommand(client *c);
void hkeysCommand(client *c);
void hvalsCommand(client *c);
void hgetallCommand(client *c);
void hexistsCommand(client *c);
void hscanCommand(client *c);
void configCommand(client *c);
void hincrbyCommand(client *c);
void hincrbyfloatCommand(client *c);
void subscribeCommand(client *c);
void unsubscribeCommand(client *c);
void psubscribeCommand(client *c);
void punsubscribeCommand(client *c);
void publishCommand(client *c);
void pubsubCommand(client *c);
void watchCommand(client *c);
void unwatchCommand(client *c);
void clusterCommand(client *c);
void restoreCommand(client *c);
void migrateCommand(client *c);
void askingCommand(client *c);
void readonlyCommand(client *c);
void readwriteCommand(client *c);
void dumpCommand(client *c);
void objectCommand(client *c);
void memoryCommand(client *c);
void clientCommand(client *c);
void helloCommand(client *c);
void evalCommand(client *c);
void evalShaCommand(client *c);
void scriptCommand(client *c);
void timeCommand(client *c);
void bitopCommand(client *c);
void bitcountCommand(client *c);
void bitposCommand(client *c);
void replconfCommand(client *c);
void waitCommand(client *c);
void geoencodeCommand(client *c);
void geodecodeCommand(client *c);
void georadiusbymemberCommand(client *c);
void georadiusbymemberroCommand(client *c);
void georadiusCommand(client *c);
void georadiusroCommand(client *c);
void geoaddCommand(client *c);
void geohashCommand(client *c);
void geoposCommand(client *c);
void geodistCommand(client *c);
void pfselftestCommand(client *c);
void pfaddCommand(client *c);
void pfcountCommand(client *c);
void pfmergeCommand(client *c);
void pfdebugCommand(client *c);
void latencyCommand(client *c);
void moduleCommand(client *c);
void securityWarningCommand(client *c);
void xaddCommand(client *c);
void xrangeCommand(client *c);
void xrevrangeCommand(client *c);
void xlenCommand(client *c);
void xreadCommand(client *c);
void xgroupCommand(client *c);
void xsetidCommand(client *c);
void xackCommand(client *c);
void xpendingCommand(client *c);
void xclaimCommand(client *c);
void xinfoCommand(client *c);
void xdelCommand(client *c);
void xtrimCommand(client *c);
void lolwutCommand(client *c);
void aclCommand(client *c);
void stralgoCommand(client *c);

#if defined(__GNUC__)
void *calloc(size_t count, size_t size) __attribute__ ((deprecated));
void free(void *ptr) __attribute__ ((deprecated));
void *malloc(size_t size) __attribute__ ((deprecated));
void *realloc(void *ptr, size_t size) __attribute__ ((deprecated));
#endif

/* Debugging stuff */
void _serverAssertWithInfo(const client *c, const robj *o, const char *estr, const char *file, int line);
void _serverAssert(const char *estr, const char *file, int line);
#ifdef __GNUC__
void _serverPanic(const char *file, int line, const char *msg, ...)
    __attribute__ ((format (printf, 3, 4)));
#else
void _serverPanic(const char *file, int line, const char *msg, ...);
#endif
void bugReportStart(void);
void serverLogObjectDebugInfo(const robj *o);
void sigsegvHandler(int sig, siginfo_t *info, void *secret);
sds genRedisInfoString(const char *section);
sds genModulesInfoString(sds info);
void enableWatchdog(int period);
void disableWatchdog(void);
void watchdogScheduleSignal(int period);
void serverLogHexDump(int level, char *descr, void *value, size_t len);
int memtest_preserving_test(unsigned long *m, size_t bytes, int passes);
void mixDigest(unsigned char *digest, void *ptr, size_t len);
void xorDigest(unsigned char *digest, void *ptr, size_t len);
int populateCommandTableParseFlags(struct redisCommand *c, char *strflags);
void killIOThreads(void);
void killThreads(void);
void makeThreadKillable(void);

/* TLS stuff */
void tlsInit(void);
int tlsConfigure(redisTLSContextConfig *ctx_config);

#define redisDebug(fmt, ...) \
    printf("DEBUG %s:%d > " fmt "\n", __FILE__, __LINE__, __VA_ARGS__)
#define redisDebugMark() \
    printf("-- MARK %s:%d --\n", __FILE__, __LINE__)

int iAmMaster(void);

#endif
