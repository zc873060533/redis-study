#ifndef __CLUSTER_H
#define __CLUSTER_H

/*-----------------------------------------------------------------------------
 * Redis cluster data structures, defines, exported API.
 *----------------------------------------------------------------------------*/

// 槽数量
#define CLUSTER_SLOTS 16384
// 集群在线
#define CLUSTER_OK 0          /* Everything looks ok */
// 集群下线
#define CLUSTER_FAIL 1        /* The cluster can't work */
// 节点名字的长度
#define CLUSTER_NAMELEN 40    /* sha1 hex length */
// 集群的实际端口号 = 用户指定的端口号 + REDIS_CLUSTER_PORT_INCR
#define CLUSTER_PORT_INCR 10000 /* Cluster port = baseport + PORT_INCR */

/* The following defines are amount of time, sometimes expressed as
 * multiplicators of the node timeout value (when ending with MULT).
 *
 * 以下是和时间有关的一些常量，
 * 以 _MULTI 结尾的常量会作为时间值的乘法因子来使用。
 * */
// 检验下线报告的乘法因子
#define CLUSTER_FAIL_REPORT_VALIDITY_MULT 2 /* Fail report validity. */
// 撤销主节点 FAIL 状态的乘法因子
#define CLUSTER_FAIL_UNDO_TIME_MULT 2 /* Undo fail if master is back. */
// 撤销主节点 FAIL 状态的加法因子
#define CLUSTER_FAIL_UNDO_TIME_ADD 10 /* Some additional time. */
#define CLUSTER_FAILOVER_DELAY 5 /* Seconds */
// 默认节点超时时限
#define CLUSTER_MF_TIMEOUT 5000 /* Milliseconds to do a manual failover. */
// 检验下线报告的乘法因子
#define CLUSTER_MF_PAUSE_MULT 2 /* Master pause manual failover mult. */
// 在进行手动的故障转移之前，需要等待的超时时间
#define CLUSTER_SLAVE_MIGRATION_DELAY 5000 /* Delay for slave migration. */

/* Redirection errors returned by getNodeByQuery(). */
// 节点可以处理请求。
#define CLUSTER_REDIR_NONE 0          /* Node can serve the request. */
#define CLUSTER_REDIR_CROSS_SLOT 1    /* -CROSSSLOT request. */
// 需要TRYAGAIN重定向
#define CLUSTER_REDIR_UNSTABLE 2      /* -TRYAGAIN redirection required */
// 需要ASK重定向。
#define CLUSTER_REDIR_ASK 3           /* -ASK redirection required. */
// 需要移动重定向。
#define CLUSTER_REDIR_MOVED 4         /* -MOVED redirection required. */
#define CLUSTER_REDIR_DOWN_STATE 5    /* -CLUSTERDOWN, global state. */
#define CLUSTER_REDIR_DOWN_UNBOUND 6  /* -CLUSTERDOWN, unbound slot. */
#define CLUSTER_REDIR_DOWN_RO_STATE 7 /* -CLUSTERDOWN, allow reads. */

struct clusterNode;

/* clusterLink encapsulates everything needed to talk with a remote node. */
// clusterLink 包含了与其他节点进行通讯所需的全部信息
typedef struct clusterLink {

    // 连接创建的时间
    mstime_t ctime;             /* Link creation time */

    // TCP连接的文件描述符
    connection *conn;           /* Connection to remote node */

    // 输出缓冲区，保存着等待发送给其他节点的消息（message）。
    sds sndbuf;                 /* Packet send buffer */

    // 输入缓冲区，保存着从其他节点接收到的消息。
    sds rcvbuf;                 /* Packet reception buffer */

    // 与这个连接相关联的节点，如果没有的话就为 NULL
    struct clusterNode *node;   /* Node related to this link if any, or NULL */
} clusterLink;

/* Cluster node flags and macros. */
// 该节点为主节点
#define CLUSTER_NODE_MASTER 1     /* The node is a master */
// 该节点为从节点
#define CLUSTER_NODE_SLAVE 2      /* The node is a slave */
// 该节点疑似下线，需要对它的状态进行确认
#define CLUSTER_NODE_PFAIL 4      /* Failure? Need acknowledge */
// 该节点已下线
#define CLUSTER_NODE_FAIL 8       /* The node is believed to be malfunctioning */
// 该节点是当前节点自身
#define CLUSTER_NODE_MYSELF 16    /* This node is myself */
// 该节点还未与当前节点完成第一次 PING - PONG 通讯
#define CLUSTER_NODE_HANDSHAKE 32 /* We have still to exchange the first ping */
// 节点没有地址
#define CLUSTER_NODE_NOADDR   64  /* We don't know the address of this node */
// 当前节点还未与该节点进行过接触
// 带有这个标识会让当前节点发送 MEET 命令而不是 PING 命令
#define CLUSTER_NODE_MEET 128     /* Send a MEET message to this node */
// 主节点可以为复制操作将槽中的数据导出
#define CLUSTER_NODE_MIGRATE_TO 256 /* Master eligible for replica migration. */
// 从节点不会尝试故障转移
#define CLUSTER_NODE_NOFAILOVER 512 /* Slave will not try to failover. */
// 空名字（在节点为主节点时，用作消息中的 slaveof 属性的值）
#define CLUSTER_NODE_NULL_NAME "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"

// 用于判断节点身份和状态的一系列宏
#define nodeIsMaster(n) ((n)->flags & CLUSTER_NODE_MASTER)
#define nodeIsSlave(n) ((n)->flags & CLUSTER_NODE_SLAVE)
#define nodeInHandshake(n) ((n)->flags & CLUSTER_NODE_HANDSHAKE)
#define nodeHasAddr(n) (!((n)->flags & CLUSTER_NODE_NOADDR))
#define nodeWithoutAddr(n) ((n)->flags & CLUSTER_NODE_NOADDR)
#define nodeTimedOut(n) ((n)->flags & CLUSTER_NODE_PFAIL)
#define nodeFailed(n) ((n)->flags & CLUSTER_NODE_FAIL)
#define nodeCantFailover(n) ((n)->flags & CLUSTER_NODE_NOFAILOVER)

/* Reasons why a slave is not able to failover. */
#define CLUSTER_CANT_FAILOVER_NONE 0
#define CLUSTER_CANT_FAILOVER_DATA_AGE 1
#define CLUSTER_CANT_FAILOVER_WAITING_DELAY 2
#define CLUSTER_CANT_FAILOVER_EXPIRED 3
#define CLUSTER_CANT_FAILOVER_WAITING_VOTES 4
#define CLUSTER_CANT_FAILOVER_RELOG_PERIOD (60*5) /* seconds. */

/* clusterState todo_before_sleep flags. */
// 节点在结束一个事件循环后要做的工作
// 处理故障转移
#define CLUSTER_TODO_HANDLE_FAILOVER (1<<0)
// 更新状态
#define CLUSTER_TODO_UPDATE_STATE (1<<1)
// 保存配置
#define CLUSTER_TODO_SAVE_CONFIG (1<<2)
// 同步配置
#define CLUSTER_TODO_FSYNC_CONFIG (1<<3)

/* Message types.
 *
 * Note that the PING, PONG and MEET messages are actually the same exact
 * kind of packet. PONG is the reply to ping, in the exact format as a PING,
 * while MEET is a special PING that forces the receiver to add the sender
 * as a node (if it is not already in the list). */
// 注意，PING 、 PONG 和 MEET 实际上是同一种消息。
// PONG 是对 PING 的回复，它的实际格式也为 PING 消息，
// 而 MEET 则是一种特殊的 PING 消息，用于强制消息的接收者将消息的发送者添加到集群中
// （如果节点尚未在节点列表中的话）
// PING
#define CLUSTERMSG_TYPE_PING 0          /* Ping */
// PONG （回复 PING）
#define CLUSTERMSG_TYPE_PONG 1          /* Pong (reply to Ping) */
// 请求将某个节点添加到集群中
#define CLUSTERMSG_TYPE_MEET 2          /* Meet "let's join" message */
// 将某个节点标记为 FAIL
#define CLUSTERMSG_TYPE_FAIL 3          /* Mark node xxx as failing */
// 通过发布与订阅功能广播消息
#define CLUSTERMSG_TYPE_PUBLISH 4       /* Pub/Sub Publish propagation */
// 请求进行故障转移操作，要求消息的接收者通过投票来支持消息的发送者
#define CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST 5 /* May I failover? */
// 消息的接收者同意向消息的发送者投票
#define CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK 6     /* Yes, you have my vote */
// 槽布局已经发生变化，消息发送者要求消息接收者进行相应的更新
#define CLUSTERMSG_TYPE_UPDATE 7        /* Another node slots configuration */
// 为了进行手动故障转移，暂停各个客户端
#define CLUSTERMSG_TYPE_MFSTART 8       /* Pause clients for manual failover */
// 模块集群API消息。
#define CLUSTERMSG_TYPE_MODULE 9        /* Module cluster API message. */
// 消息类型总数。
#define CLUSTERMSG_TYPE_COUNT 10        /* Total number of message types. */

/* Flags that a module can set in order to prevent certain Redis Cluster
 * features to be enabled. Useful when implementing a different distributed
 * system on top of Redis Cluster message bus, using modules. */
#define CLUSTER_MODULE_FLAG_NONE 0
#define CLUSTER_MODULE_FLAG_NO_FAILOVER (1<<1)
#define CLUSTER_MODULE_FLAG_NO_REDIRECTION (1<<2)

/* This structure represent elements of node->fail_reports. */
// 每个 clusterNodeFailReport 结构保存了一条其他节点对目标节点的下线报告
// （认为目标节点已经下线）
typedef struct clusterNodeFailReport {

    // 报告的故障的节点
    struct clusterNode *node;  /* Node reporting the failure condition. */

    // 最后一次从 node 节点收到下线报告的时间
    // 程序使用这个时间戳来检查下线报告是否过期
    mstime_t time;             /* Time of the last report from this node. */

} clusterNodeFailReport;

// 节点状态
typedef struct clusterNode {

    // 节点创建的时间
    mstime_t ctime; /* Node object creation time. */

    // 节点的名字，由 40 个十六进制字符组成
    // 例如 68eef66df23420a5862208ef5b1a7005b806f2ff
    char name[CLUSTER_NAMELEN]; /* Node name, hex string, sha1-size */

    // 节点标识
    // 使用各种不同的标识值记录节点的角色（比如主节点或者从节点），
    // 以及节点目前所处的状态（比如在线或者下线）。
    int flags;      /* CLUSTER_NODE_... */

    // 节点当前的配置纪元，用于实现故障转移
    uint64_t configEpoch; /* Last configEpoch observed for this node */

    // 由这个节点负责处理的槽
    // 一共有 REDIS_CLUSTER_SLOTS / 8 个字节长
    // 每个字节的每个位记录了一个槽的保存状态
    // 位的值为 1 表示槽正由本节点处理，值为 0 则表示槽并非本节点处理
    // 比如 slots[0] 的第一个位保存了槽 0 的保存情况
    // slots[0] 的第二个位保存了槽 1 的保存情况，以此类推
    unsigned char slots[CLUSTER_SLOTS/8]; /* slots handled by this node */

    // 该节点负责处理的槽数量
    int numslots;   /* Number of slots handled by this node */

    // 如果本节点是主节点，那么用这个属性记录从节点的数量
    int numslaves;  /* Number of slave nodes, if this is a master */

    // 指针数组，指向各个从节点
    struct clusterNode **slaves; /* pointers to slave nodes */

    // 如果这是一个从节点，那么指向主节点
    struct clusterNode *slaveof; /* pointer to the master node. Note that it
                                    may be NULL even if the node is a slave
                                    if we don't have the master node in our
                                    tables. */

    // 最后一次发送 PING 命令的时间
    mstime_t ping_sent;      /* Unix time we sent latest ping */

    // 最后一次接收 PONG 回复的时间戳
    mstime_t pong_received;  /* Unix time we received the pong */

    // 收到数据的时间
    mstime_t data_received;  /* Unix time we received any data */

    // 最后一次被设置为 FAIL 状态的时间
    mstime_t fail_time;      /* Unix time when FAIL flag was set */

    // 最后一次给某个从节点投票的时间
    mstime_t voted_time;     /* Last time we voted for a slave of this master */

    // 最后一次从这个节点接收到复制偏移量的时间
    mstime_t repl_offset_time;  /* Unix time we received offset for this node */
    // 孤立的主节点迁移的时间
    mstime_t orphaned_time;     /* Starting time of orphaned master condition */

    // 这个节点的复制偏移量
    long long repl_offset;      /* Last known repl offset for this node. */

    // 节点的 IP 地址
    char ip[NET_IP_STR_LEN];  /* Latest known IP address of this node */

    // 节点的端口号
    int port;                   /* Latest known clients port of this node */
    int cport;                  /* Latest known cluster port of this node. */

    // 保存连接节点所需的有关信息
    clusterLink *link;          /* TCP/IP link with this node */

    // 一个链表，记录了所有其他节点对该节点的下线报告
    list *fail_reports;         /* List of nodes signaling this as failing */

} clusterNode;

// 集群状态，每个节点都保存着一个这样的状态，记录了它们眼中的集群的样子。
// 另外，虽然这个结构主要用于记录集群的属性，但是为了节约资源，
// 有些与节点有关的属性，比如 slots_to_keys 、 failover_auth_count
// 也被放到了这个结构里面。
typedef struct clusterState {

    // 指向当前节点的指针
    clusterNode *myself;  /* This node */

    // 集群当前的配置纪元，用于实现故障转移
    uint64_t currentEpoch;

    // 集群当前的状态：是在线还是下线
    int state;            /* CLUSTER_OK, CLUSTER_FAIL, ... */

    // 集群中至少处理着一个槽的节点的数量。
    int size;             /* Num of master nodes with at least one slot */

    // 集群节点名单（包括 myself 节点）
    // 字典的键为节点的名字，字典的值为 clusterNode 结构
    dict *nodes;          /* Hash table of name -> clusterNode structures */

    // 节点黑名单，用于 CLUSTER FORGET 命令
    // 防止被 FORGET 的命令重新被添加到集群里面
    dict *nodes_black_list; /* Nodes we don't re-add for a few seconds. */

    // 记录要从当前节点迁移到目标节点的槽，以及迁移的目标节点
    // migrating_slots_to[i] = NULL 表示槽 i 未被迁移
    // migrating_slots_to[i] = clusterNode_A 表示槽 i 要从本节点迁移至节点 A
    clusterNode *migrating_slots_to[CLUSTER_SLOTS];

    // 记录要从源节点迁移到本节点的槽，以及进行迁移的源节点
    // importing_slots_from[i] = NULL 表示槽 i 未进行导入
    // importing_slots_from[i] = clusterNode_A 表示正从节点 A 中导入槽 i
    clusterNode *importing_slots_from[CLUSTER_SLOTS];

    // 负责处理各个槽的节点
    // 例如 slots[i] = clusterNode_A 表示槽 i 由节点 A 处理
    clusterNode *slots[CLUSTER_SLOTS];

    // 负责槽中的key数量
    uint64_t slots_keys_count[CLUSTER_SLOTS];

    // 跳跃表，表中以槽作为分值，键作为成员，对槽进行有序排序
    // 当需要对某些槽进行区间（range）操作时，这个跳跃表可以提供方便
    // 具体操作定义在 db.c 里面
    rax *slots_to_keys;

    /* The following fields are used to take the slave state on elections. */
    // 以下这些域被用于进行故障转移选举

    // 上次执行选举或者下次执行选举的时间
    mstime_t failover_auth_time; /* Time of previous or next election. */

    // 节点获得支持的票数
    int failover_auth_count;    /* Number of votes received so far. */

    // 如果为真，表示本节点已经向其他节点发送了投票请求
    int failover_auth_sent;     /* True if we already asked for votes. */

    // 该从节点在当前请求中的排名
    int failover_auth_rank;     /* This slave rank for current auth request. */

    // 当前选举的纪元
    uint64_t failover_auth_epoch; /* Epoch of the current election. */

    // 从节点不能执行故障转移的原因
    int cant_failover_reason;   /* Why a slave is currently not able to
                                   failover. See the CANT_FAILOVER_* macros. */
    /* Manual failover state in common. */
    /* 共用的手动故障转移状态 */

    // 手动故障转移执行的时间限制
    mstime_t mf_end;            /* Manual failover time limit (ms unixtime).
                                   It is zero if there is no MF in progress. */
    /* Manual failover state of master. */
    /* 主服务器的手动故障转移状态 */
    clusterNode *mf_slave;      /* Slave performing the manual failover. */
    /* Manual failover state of slave. */
    // 从节点记录手动故障转移时的主节点偏移量
    long long mf_master_offset; /* Master offset the slave needs to start MF
                                   or zero if still not received. */
    // 指示手动故障转移是否可以开始的标志值
    // 值为非 0 时表示各个主服务器可以开始投票
    int mf_can_start;           /* If non-zero signal that the manual failover
                                   can start requesting masters vote. */
    /* The following fields are used by masters to take state on elections. */
    /* 以下这些域由主服务器使用，用于记录选举时的状态 */

    // 集群最后一次进行投票的纪元
    uint64_t lastVoteEpoch;     /* Epoch of the last vote granted. */

    // 在进入下个事件循环之前要做的事情，以各个 flag 来记录
    int todo_before_sleep; /* Things to do in clusterBeforeSleep(). */
    /* Messages received and sent by type. */
    // 发送的字节数
    long long stats_bus_messages_sent[CLUSTERMSG_TYPE_COUNT];
    // 通过Cluster接收到的消息数量
    long long stats_bus_messages_received[CLUSTERMSG_TYPE_COUNT];

    long long stats_pfail_nodes;    /* Number of nodes in PFAIL status,
                                       excluding nodes without address. */
} clusterState;

/* Redis cluster messages header */

/* Initially we don't know our "name", but we'll find it once we connect
 * to the first node, using the getsockname() function. Then we'll use this
 * address for all the next messages. */
typedef struct {

    // 节点的名字
    // 在刚开始的时候，节点的名字会是随机的
    // 当 MEET 信息发送并得到回复之后，集群就会为节点设置正式的名字
    char nodename[CLUSTER_NAMELEN];

    // 最后一次向该节点发送 PING 消息的时间戳
    uint32_t ping_sent;

    // 最后一次从该节点接收到 PONG 消息的时间戳
    uint32_t pong_received;

    // 节点的 IP 地址
    char ip[NET_IP_STR_LEN];  /* IP address last time it was seen */

    // 节点的端口号
    uint16_t port;              /* base port last time it was seen */

    // 最后一次获取到的集群端口
    uint16_t cport;             /* cluster port last time it was seen */

    // 节点的标识值
    uint16_t flags;             /* node->flags copy */

    // 对齐字节，不使用
    uint32_t notused1;

} clusterMsgDataGossip;

typedef struct {

    // 下线节点的名字
    char nodename[CLUSTER_NAMELEN];

} clusterMsgDataFail;

typedef struct {

    // 频道名长度
    uint32_t channel_len;

    // 消息长度
    uint32_t message_len;

    // 消息内容，格式为 频道名+消息
    // bulk_data[0:channel_len-1] 为频道名
    // bulk_data[channel_len:channel_len+message_len-1] 为消息
    unsigned char bulk_data[8]; /* 8 bytes just as placeholder. */

} clusterMsgDataPublish;

typedef struct {

    // 节点的配置纪元
    uint64_t configEpoch; /* Config epoch of the specified instance. */

    // 负责槽的节点名字
    char nodename[CLUSTER_NAMELEN]; /* Name of the slots owner. */

    // 节点的槽布局
    unsigned char slots[CLUSTER_SLOTS/8]; /* Slots bitmap. */

} clusterMsgDataUpdate;

typedef struct {
    uint64_t module_id;     /* ID of the sender module. */
    uint32_t len;           /* ID of the sender module. */
    uint8_t type;           /* Type from 0 to 255. */
    unsigned char bulk_data[3]; /* 3 bytes just as placeholder. */
} clusterMsgModule;

union clusterMsgData {
    /* PING, MEET and PONG */
    struct {
        /* Array of N clusterMsgDataGossip structures */
        // 每条消息都包含两个 clusterMsgDataGossip 结构
        clusterMsgDataGossip gossip[1];
    } ping;

    /* FAIL */
    struct {
        clusterMsgDataFail about;
    } fail;

    /* PUBLISH */
    struct {
        clusterMsgDataPublish msg;
    } publish;

    /* UPDATE */
    struct {
        clusterMsgDataUpdate nodecfg;
    } update;

    /* MODULE */
    struct {
        clusterMsgModule msg;
    } module;
};

#define CLUSTER_PROTO_VER 1 /* Cluster bus protocol version. */

// 用来表示集群消息的结构（消息头，header）
typedef struct {

    // "RCmb"的签名
    char sig[4];        /* Signature "RCmb" (Redis Cluster message bus). */

    // 消息的总长
    uint32_t totlen;    /* Total length of this message */

    // 协议版本，当前为0
    uint16_t ver;       /* Protocol version, currently set to 1. */
    uint16_t port;      /* TCP base port number. */

    // 消息类型
    uint16_t type;      /* Message type */

    // 消息正文包含的节点信息数量
    // 只在发送 MEET 、 PING 和 PONG 这三种 Gossip 协议消息时使用
    uint16_t count;     /* Only used for some kind of messages. */

    // 发送消息的当前纪元
    uint64_t currentEpoch;  /* The epoch accordingly to the sending node. */

    // 如果发送消息是主节点，则记录发送消息的节点配置纪元
    // 如果发送消息是从节点，则记录当前从节点的主节点的配置纪元
    uint64_t configEpoch;   /* The config epoch if it's a master, or the last
                               epoch advertised by its master if it is a
                               slave. */
    // 复制偏移量
    uint64_t offset;    /* Master replication offset if node is a master or
                           processed replication offset if node is a slave. */
    // 发送消息的节点的name
    char sender[CLUSTER_NAMELEN]; /* Name of the sender node */

    // 发送消息节点的槽位图信息
    unsigned char myslots[CLUSTER_SLOTS/8];

    // 如果发送消息的是主节点，那么保存的是正在复制的主节点的名字
    // 如果发送消息的是从节点，保存的是空
    char slaveof[CLUSTER_NAMELEN];

    char myip[NET_IP_STR_LEN];    /* Sender IP, if not all zeroed. */

    // 34字节未使用
    char notused1[34];  /* 34 bytes reserved for future usage. */

    // 消息发送者的端口号
    uint16_t cport;      /* Sender TCP cluster bus port */

    // 消息发送者的标识值
    uint16_t flags;      /* Sender node flags */

    // 消息发送者所处集群的状态
    unsigned char state; /* Cluster state from the POV of the sender */

    // 消息的标识
    unsigned char mflags[3]; /* Message flags: CLUSTERMSG_FLAG[012]_... */

    // 消息的正文（或者说，内容）
    union clusterMsgData data;

} clusterMsg;

#define CLUSTERMSG_MIN_LEN (sizeof(clusterMsg)-sizeof(union clusterMsgData))

/* Message flags better specify the packet content or are used to
 * provide some information about the node state. */
// 主节点暂停手动故障转移
#define CLUSTERMSG_FLAG0_PAUSED (1<<0) /* Master paused for manual failover. */
// 即使主节点在线，也要认证故障转移
#define CLUSTERMSG_FLAG0_FORCEACK (1<<1) /* Give ACK to AUTH_REQUEST even if
                                            master is up. */

/* ---------------------- API exported outside cluster.c -------------------- */
clusterNode *getNodeByQuery(client *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot, int *ask);
int clusterRedirectBlockedClientIfNeeded(client *c);
void clusterRedirectClient(client *c, clusterNode *n, int hashslot, int error_code);
unsigned long getClusterConnectionsCount(void);

#endif /* __CLUSTER_H */
