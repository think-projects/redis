#ifndef STREAM_H
#define STREAM_H

#include "rax.h"
#include "listpack.h"
/* 消息ID */
/* Stream item ID: a 128 bit number composed of a milliseconds time and
 * a sequence counter. IDs generated in the same millisecond (or in a past
 * millisecond if the clock jumped backward) will use the millisecond time
 * of the latest generated ID and an incremented sequence. */
typedef struct streamID {
    uint64_t ms;        /* Unix time in milliseconds. */ // 从19770年1月1日至今的毫秒数
    uint64_t seq;       /* Sequence number. */ // 序号
} streamID;
/* 消息流 */
typedef struct stream {
    rax *rax;               /* The radix tree holding the stream. */ // 以streamId为key listpack为value 生成rax树
    uint64_t length;        /* Number of elements inside this stream. */ // 元素个数
    streamID last_id;       /* Zero if there are yet no items. */ // 最后的消息ID
    rax *cgroups;           /* Consumer groups dictionary: name -> streamCG */ // 消费组 以消费组名称为key streamCG为value的rax树
} stream;
/* stream迭代器 */
/* We define an iterator to iterate stream items in an abstract way, without
 * caring about the radix tree + listpack representation. Technically speaking
 * the iterator is only used inside streamReplyWithRange(), so could just
 * be implemented inside the function, but practically there is the AOF
 * rewriting code that also needs to iterate the stream to emit the XADD
 * commands. */
typedef struct streamIterator {
    stream *stream;         /* The stream we are iterating. */ // 指向当前迭代的stream
    streamID master_id;     /* ID of the master entry at listpack head. */ // master entry的StreamId
    uint64_t master_fields_count;       /* Master entries # of fields. */ // master entry field域个数
    unsigned char *master_fields_start; /* Master entries start in listpack. */ // master entry field域起始地址
    unsigned char *master_fields_ptr;   /* Master field to emit next. */ // master entry field域待遍历的field
    int entry_flags;                    /* Flags of entry we are emitting. */ // 遍历的节点标识
    int rev;                /* True if iterating end to start (reverse). */ // 正向或反向遍历 1:反向
    uint64_t start_key[2];  /* Start key as 128 bit big endian. */ // 起始的消息id
    uint64_t end_key[2];    /* End key as 128 bit big endian. */ // 结束的消息id
    raxIterator ri;         /* Rax iterator. */ // 包括rax迭代器用于遍历key
    unsigned char *lp;      /* Current listpack. */ // 指向当前的listpack
    unsigned char *lp_ele;  /* Current listpack cursor. */ // 指向当前的listpack游标(遍历到的节点)
    unsigned char *lp_flags; /* Current entry flags pointer. */ // 指向当前节点的flag
    /* Buffers used to hold the string of lpGet() when the element is
     * integer encoded, so that there is no string representation of the
     * element inside the listpack itself. */
    unsigned char field_buf[LP_INTBUF_SIZE]; // 缓存读取的field域数据
    unsigned char value_buf[LP_INTBUF_SIZE]; // 缓存读取的value数据
} streamIterator;
/* 消费组 */
/* Consumer group. */
typedef struct streamCG {
    streamID last_id;       /* Last delivered (not acknowledged) ID for this
                               group. Consumers that will just ask for more
                               messages will served with IDs > than this. */ // 最后发送消息ID(未映带)
    rax *pel;               /* Pending entries list. This is a radix tree that
                               has every message delivered to consumers (without
                               the NOACK option) that was yet not acknowledged
                               as processed. The key of the radix tree is the
                               ID as a 64 bit big endian number, while the
                               associated value is a streamNACK structure.*/ // 未应答消息 嗲确认消息列表 streamID为key streamNACK为value rax树
    rax *consumers;         /* A radix tree representing the consumers by name
                               and their associated representation in the form
                               of streamConsumer structures. */ // 消费组中的消费者 消费者name为key streamConsumer为value的rax树
} streamCG;
/* 消费组中的消费者
/* A specific consumer in a consumer group.  */
typedef struct streamConsumer {
    mstime_t seen_time;         /* Last time this consumer was active. */ // 消费者最后一次活跃时间(消费活ack)
    sds name;                   /* Consumer name. This is how the consumer
                                   will be identified in the consumer group
                                   protocol. Case sensitive. */ // 消费者名称
    rax *pel;                   /* Consumer specific pending entries list: all
                                   the pending messages delivered to this
                                   consumer not yet acknowledged. Keys are
                                   big endian message IDs, while values are
                                   the same streamNACK structure referenced
                                   in the "pel" of the conumser group structure
                                   itself, so the value is shared. */ // 待确认消息列表 streamID为key streamNACK为value rax树
} streamConsumer;
/* 在消费组中未确认的消息应答 */
/* Pending (yet not acknowledged) message in a consumer group. */
typedef struct streamNACK {
    mstime_t delivery_time;     /* Last time this message was delivered. */ // 发送消息的最后时间
    uint64_t delivery_count;    /* Number of times this message was delivered.*/ // 消息被发送的次数
    streamConsumer *consumer;   /* The consumer this message was delivered to
                                   in the last delivery. */ // 消息发送的对象
} streamNACK;

/* Stream propagation informations, passed to functions in order to propagate
 * XCLAIM commands to AOF and slaves. */
typedef struct sreamPropInfo {
    robj *keyname;
    robj *groupname;
} streamPropInfo;

/* Prototypes of exported APIs. */
struct client;

/* Flags for streamLookupConsumer */
#define SLC_NONE      0
#define SLC_NOCREAT   (1<<0) /* Do not create the consumer if it doesn't exist */
#define SLC_NOREFRESH (1<<1) /* Do not update consumer's seen-time */

stream *streamNew(void);
void freeStream(stream *s);
unsigned long streamLength(const robj *subject);
size_t streamReplyWithRange(client *c, stream *s, streamID *start, streamID *end, size_t count, int rev, streamCG *group, streamConsumer *consumer, int flags, streamPropInfo *spi);
void streamIteratorStart(streamIterator *si, stream *s, streamID *start, streamID *end, int rev);
int streamIteratorGetID(streamIterator *si, streamID *id, int64_t *numfields);
void streamIteratorGetField(streamIterator *si, unsigned char **fieldptr, unsigned char **valueptr, int64_t *fieldlen, int64_t *valuelen);
void streamIteratorStop(streamIterator *si);
streamCG *streamLookupCG(stream *s, sds groupname);
streamConsumer *streamLookupConsumer(streamCG *cg, sds name, int flags);
streamCG *streamCreateCG(stream *s, char *name, size_t namelen, streamID *id);
streamNACK *streamCreateNACK(streamConsumer *consumer);
void streamDecodeID(void *buf, streamID *id);
int streamCompareID(streamID *a, streamID *b);
void streamIncrID(streamID *id);

#endif
