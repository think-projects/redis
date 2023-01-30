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

static int checkStringLength(client *c, long long size) {
    if (size > 512*1024*1024) { // size > 512M
        addReplyError(c,"string exceeds maximum allowed size (512MB)");
        return C_ERR;
    }
    return C_OK;
}

/* The setGenericCommand() function implements the SET operation with different
 * options and variants. This function is called in order to implement the
 * following commands: SET, SETEX, PSETEX, SETNX.
 *
 * 'flags' changes the behavior of the command (NX or XX, see belove).
 *
 * 'expire' represents an expire to set in form of a Redis object as passed
 * by the user. It is interpreted according to the specified 'unit'.
 *
 * 'ok_reply' and 'abort_reply' is what the function will reply to the client
 * if the operation is performed, or when it is not because of NX or
 * XX flags.
 *
 * If ok_reply is NULL "+OK" is used.
 * If abort_reply is NULL, "$-1" is used. */

#define OBJ_SET_NO_FLAGS 0
#define OBJ_SET_NX (1<<0)     /* Set if key not exists. */
#define OBJ_SET_XX (1<<1)     /* Set if key exists. */
#define OBJ_SET_EX (1<<2)     /* Set if time in seconds is given */
#define OBJ_SET_PX (1<<3)     /* Set if time in ms in given */
/**
 * 内部函数
 * @param c client
 * @param flags 标识
 * @param key
 * @param val
 * @param expire 过期时间
 * @param unit 单位 s ms
 * @param ok_reply 应答成功
 * @param abort_reply 应答失败
 */
void setGenericCommand(client *c, int flags, robj *key, robj *val, robj *expire, int unit, robj *ok_reply, robj *abort_reply) {
    long long milliseconds = 0; /* initialized to avoid any harmness warning */

    if (expire) { // 存在过期时间
        if (getLongLongFromObjectOrReply(c, expire, &milliseconds, NULL) != C_OK) // 将expire转化为milliseconds
            return;
        if (milliseconds <= 0) { // 过期时间小于等于0
            addReplyErrorFormat(c,"invalid expire time in %s",c->cmd->name); // 应答不合法过期时间
            return;
        }
        if (unit == UNIT_SECONDS) milliseconds *= 1000; // 如果单位是秒 则 milliseconds *= 1000 转为毫秒
    }

    if ((flags & OBJ_SET_NX && lookupKeyWrite(c->db,key) != NULL) || // flags是nx 并且key存在
        (flags & OBJ_SET_XX && lookupKeyWrite(c->db,key) == NULL)) // flags是xx 并且key不存在
    {
        addReply(c, abort_reply ? abort_reply : shared.nullbulk); // 应答abort_reply
        return;
    }
    setKey(c->db,key,val); // 成功设置 添加或修改
    server.dirty++;
    if (expire) setExpire(c,c->db,key,mstime()+milliseconds); // 如果有过期时间 设置key过期 mstime()+milliseconds
    notifyKeyspaceEvent(NOTIFY_STRING,"set",key,c->db->id); // 键空间通知 set
    if (expire) notifyKeyspaceEvent(NOTIFY_GENERIC,
        "expire",key,c->db->id); // 如果有过期时间 则expire
    addReply(c, ok_reply ? ok_reply : shared.ok); // 应答ok shared:共享对象
}

/* SET key value [NX] [XX] [EX <seconds>] [PX <milliseconds>] */
/**
 * 外层函数
 * @param c client
 */
void setCommand(client *c) {
    int j;
    robj *expire = NULL; // 过期时间
    int unit = UNIT_SECONDS; // 单位 默认 秒
    int flags = OBJ_SET_NO_FLAGS; // 标识 默认0

    for (j = 3; j < c->argc; j++) { // 跳过 set key value
        char *a = c->argv[j]->ptr;
        robj *next = (j == c->argc-1) ? NULL : c->argv[j+1]; // 过期时间

        if ((a[0] == 'n' || a[0] == 'N') &&
            (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
            !(flags & OBJ_SET_XX)) // 如果是nx或NX 并且flag不是XX
        {
            flags |= OBJ_SET_NX; // 标识为SET_NX
        } else if ((a[0] == 'x' || a[0] == 'X') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_NX)) // 如果是xx或XX 并且flag不是NX
        {
            flags |= OBJ_SET_XX; // 标识为SET_XX
        } else if ((a[0] == 'e' || a[0] == 'E') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_PX) && next) // 如果是ex或EX 并且flag不是PX 并且有过期时间
        {
            flags |= OBJ_SET_EX; // 标识为SET_NX
            unit = UNIT_SECONDS; // 单位是秒
            expire = next; // 设置过期时间
            j++; // EX或PX只能存在一个
        } else if ((a[0] == 'p' || a[0] == 'P') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_EX) && next) // 如果是px或PX 并且flag不是EX 并且有过期时间
        {
            flags |= OBJ_SET_PX; // 标识为SET_PX
            unit = UNIT_MILLISECONDS; // 单位是毫秒
            expire = next; // 设置过期时间
            j++; // EX或PX只能存在一个
        } else { // 语法错误
            addReply(c,shared.syntaxerr); // 返回客户端应答
            return;
        }
    }

    c->argv[2] = tryObjectEncoding(c->argv[2]); // 尝试将value转为数字
    setGenericCommand(c,flags,c->argv[1],c->argv[2],expire,unit,NULL,NULL); // 调用函数处理
}

void setnxCommand(client *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,OBJ_SET_NX,c->argv[1],c->argv[2],NULL,0,shared.cone,shared.czero);
}

void setexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_SECONDS,NULL,NULL);
}

void psetexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_MILLISECONDS,NULL,NULL);
}
/**
 * 处理函数
 * @param c
 * @return
 */
int getGenericCommand(client *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL) // 如果lookupKeyRead 如果obj时空 则应答NULL
        return C_OK;

    if (o->type != OBJ_STRING) { // o不为空 类型不是string
        addReply(c,shared.wrongtypeerr); // 应答类型错误
        return C_ERR;
    } else {
        addReplyBulk(c,o); // 块应答
        return C_OK;
    }
}
/**
 * get key
 * @param c
 */
void getCommand(client *c) {
    getGenericCommand(c);
}
/**
 * getset key value
 * @param c
 */
void getsetCommand(client *c) {
    if (getGenericCommand(c) == C_ERR) return; // 响应值对象错误 返回
    c->argv[2] = tryObjectEncoding(c->argv[2]); // 将value转为值对象
    setKey(c->db,c->argv[1],c->argv[2]); // 设置key的值对象
    notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[1],c->db->id); // 键空间通知
    server.dirty++; // 更新计数
}
/**
 * setrange key offset value
 * @param c
 */
void setrangeCommand(client *c) {
    robj *o;
    long offset;
    sds value = c->argv[3]->ptr; // 获得vlaue

    if (getLongFromObjectOrReply(c,c->argv[2],&offset,NULL) != C_OK) // 获得offset参数,获得参数的值
        return;

    if (offset < 0) {
        addReplyError(c,"offset is out of range");
        return;
    }

    o = lookupKeyWrite(c->db,c->argv[1]); // 获得key对应的值对象
    if (o == NULL) { // 值对象为空 kv不存在
        /* Return 0 when setting nothing on a non-existing string */
        if (sdslen(value) == 0) { // value是空
            addReply(c,shared.czero); // 响应0
            return;
        }

        /* Return when the resulting string exceeds allowed size */
        if (checkStringLength(c,offset+sdslen(value)) != C_OK) // value长度+offset>512M
            return;

        o = createObject(OBJ_STRING,sdsnewlen(NULL, offset+sdslen(value))); // 创建空对象
        dbAdd(c->db,c->argv[1],o); // kv添加到db中
    } else { // 值对象存在
        size_t olen;

        /* Key exists, check type */
        if (checkType(c,o,OBJ_STRING)) // 值对象类型不是字符串
            return;

        /* Return existing string length when setting nothing */
        olen = stringObjectLen(o); // 获得值对象的字符串长度
        if (sdslen(value) == 0) { // value是空
            addReplyLongLong(c,olen); // 返回原字符串长度
            return;
        }

        /* Return when the resulting string exceeds allowed size */
        if (checkStringLength(c,offset+sdslen(value)) != C_OK) // value长度+offset>512M
            return;

        /* Create a copy when the object is shared or encoded. */
        o = dbUnshareStringValue(c->db,c->argv[1],o); // 解除key的共享
    }

    if (sdslen(value) > 0) { // value不是空
        o->ptr = sdsgrowzero(o->ptr,offset+sdslen(value)); // 扩容或返回原来的sds
        memcpy((char*)o->ptr+offset,value,sdslen(value)); // 将value拷贝到字符串的offset位 覆盖value的长度
        signalModifiedKey(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_STRING,
            "setrange",c->argv[1],c->db->id);
        server.dirty++;
    }
    addReplyLongLong(c,sdslen(o->ptr)); // 响应新的sds长度
}
/**
 * getrange key start, end
 * @param c
 */
void getrangeCommand(client *c) {
    robj *o;
    long long start, end;
    char *str, llbuf[32];
    size_t strlen;

    if (getLongLongFromObjectOrReply(c,c->argv[2],&start,NULL) != C_OK) // 获得start参数 如果不是整数 则返回
        return;
    if (getLongLongFromObjectOrReply(c,c->argv[3],&end,NULL) != C_OK) // 获得end参数 如果不是整数 则返回
        return;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptybulk)) == NULL ||
        checkType(c,o,OBJ_STRING)) return; // key对应的值对象 是空或者不是空 不是字符串 则返回

    if (o->encoding == OBJ_ENCODING_INT) { // 是字符串 编码是整数
        str = llbuf;
        strlen = ll2string(llbuf,sizeof(llbuf),(long)o->ptr); // 整数转字符串获得长度
    } else { // 编码是字符串
        str = o->ptr;
        strlen = sdslen(str); // 直接获得字符串长度
    }

    /* Convert negative indexes */ // 负索引转正索引
    if (start < 0 && end < 0 && start > end) {
        addReply(c,shared.emptybulk);
        return;
    }
    if (start < 0) start = strlen+start; // 正索引=长度+负索引
    if (end < 0) end = strlen+end;
    if (start < 0) start = 0; // 算完后还是负值 则 设置为0
    if (end < 0) end = 0;
    if ((unsigned long long)end >= strlen) end = strlen-1; // end是最后一个

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * nothing can be returned is: start > end. */
    if (start > end || strlen == 0) { // 起始大于截止 或者 长度是0
        addReply(c,shared.emptybulk);
    } else { // 返回字符串内容
        addReplyBulkCBuffer(c,(char*)str+start,end-start+1);
    }
}
/**
 * mget key1 key2 ...
 * @param c
 */
void mgetCommand(client *c) {
    int j;

    addReplyMultiBulkLen(c,c->argc-1); // 应答结果数
    for (j = 1; j < c->argc; j++) {
        robj *o = lookupKeyRead(c->db,c->argv[j]); // 以读的方式查找
        if (o == NULL) { // 没找到
            addReply(c,shared.nullbulk); // 应答空
        } else {
            if (o->type != OBJ_STRING) { // 类型不是string
                addReply(c,shared.nullbulk); // 应答空
            } else {
                addReplyBulk(c,o); // 应答块
            }
        }
    }
}
/**
 * mset内部命令 实现
 * @param c
 * @param nx nx=1 则是msetnx
 */
void msetGenericCommand(client *c, int nx) {
    int j;

    if ((c->argc % 2) == 0) { // 参数个数是偶数
        addReplyError(c,"wrong number of arguments for MSET"); // 响应参数个数错误
        return; // 返回
    }

    /* Handle the NX flag. The MSETNX semantic is to return zero and don't
     * set anything if at least one key alerady exists. */
    if (nx) { // msetnx
        for (j = 1; j < c->argc; j += 2) { // 取出所有的key
            if (lookupKeyWrite(c->db,c->argv[j]) != NULL) { // 在db中查找key 找到了
                addReply(c, shared.czero); // key计数+1
                return;
            }
        }
    }

    for (j = 1; j < c->argc; j += 2) { // key不存在或mset 循环key和value
        c->argv[j+1] = tryObjectEncoding(c->argv[j+1]); // 把value转为redisObject
        setKey(c->db,c->argv[j],c->argv[j+1]); // 在db中设置key和value
        notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[j],c->db->id); // 通知键空间通知 set
    }
    server.dirty += (c->argc-1)/2; // 更新数据修改计数 参数/2
    addReply(c, nx ? shared.cone : shared.ok); // 响应
}
/**
 * mset key1 value1 key2 value2
 * @param c
 */
void msetCommand(client *c) {
    msetGenericCommand(c,0); // 调用内部命令 nx=0
}
/**
 * msetx key1 value1 key2 value2
 * @param c
 */
void msetnxCommand(client *c) {
    msetGenericCommand(c,1); // 调用内部命令 nx=1
}
/**
 * incr key
 * decr key
 * incrby key incr
 * decrby key decr
 * @param c
 * @param incr
 */
void incrDecrCommand(client *c, long long incr) {
    long long value, oldvalue;
    robj *o, *new;

    o = lookupKeyWrite(c->db,c->argv[1]); // 在db中获得key对应的值对象
    if (o != NULL && checkType(c,o,OBJ_STRING)) return; // 值对象存在并且类型不是字符串 返回
    if (getLongLongFromObjectOrReply(c,o,&value,NULL) != C_OK) return; // 获得值对象中的整数 不是整数 则返回

    oldvalue = value; // 旧的值
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) { // 不在main和max之间
        addReplyError(c,"increment or decrement would overflow");
        return;
    }
    value += incr; // 累加

    if (o && o->refcount == 1 && o->encoding == OBJ_ENCODING_INT &&
        (value < 0 || value >= OBJ_SHARED_INTEGERS) &&
        value >= LONG_MIN && value <= LONG_MAX) // 值对象存在 并且引用为1 并且编码是int 并且在区间中
    {
        new = o; // 把value赋给值对象
        o->ptr = (void*)((long)value);
    } else {
        new = createStringObjectFromLongLongForValue(value); // 创建新的值对象
        if (o) { // 值对象存在
            dbOverwrite(c->db,c->argv[1],new); //覆盖原值对象
        } else { // 值对象不存在
            dbAdd(c->db,c->argv[1],new); // 添加kv
        }
    }
    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"incrby",c->argv[1],c->db->id);
    server.dirty++;
    // 应答

    addReply(c,shared.colon);
    addReply(c,new);
    addReply(c,shared.crlf);
}
/**
 *  incr key
 * @param c
 */
void incrCommand(client *c) {
    incrDecrCommand(c,1); // 自增1
}
/**
 * decr key
 * @param c
 */
void decrCommand(client *c) {
    incrDecrCommand(c,-1); // 自减1
}
/**
 * incrby keyincr
 * @param c
 */
void incrbyCommand(client *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return; // 获得incr参数的整数值 incr不是整数 则返回
    incrDecrCommand(c,incr); // incr = incr
}
/**
 * decrby key decr
 * @param c
 */
void decrbyCommand(client *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return; // 获得incr参数的整数值 incr不是整数 则返回
    incrDecrCommand(c,-incr); // incr = -incr
}

void incrbyfloatCommand(client *c) {
    long double incr, value;
    robj *o, *new, *aux;

    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o != NULL && checkType(c,o,OBJ_STRING)) return;
    if (getLongDoubleFromObjectOrReply(c,o,&value,NULL) != C_OK ||
        getLongDoubleFromObjectOrReply(c,c->argv[2],&incr,NULL) != C_OK)
        return;

    value += incr;
    if (isnan(value) || isinf(value)) {
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }
    new = createStringObjectFromLongDouble(value,1);
    if (o)
        dbOverwrite(c->db,c->argv[1],new);
    else
        dbAdd(c->db,c->argv[1],new);
    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"incrbyfloat",c->argv[1],c->db->id);
    server.dirty++;
    addReplyBulk(c,new);

    /* Always replicate INCRBYFLOAT as a SET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
    aux = createStringObject("SET",3);
    rewriteClientCommandArgument(c,0,aux);
    decrRefCount(aux);
    rewriteClientCommandArgument(c,2,new);
}
/**
 * append key value
 * @param c
 */
void appendCommand(client *c) {
    size_t totlen;
    robj *o, *append;

    o = lookupKeyWrite(c->db,c->argv[1]); // 从db中获得key对应的值对象
    if (o == NULL) { // 值对象为空 kv不存在
        /* Create the key */
        c->argv[2] = tryObjectEncoding(c->argv[2]); // 把value转为值对象(尝试整型转化)
        dbAdd(c->db,c->argv[1],c->argv[2]); // kv添加到db
        incrRefCount(c->argv[2]);
        totlen = stringObjectLen(c->argv[2]); // 获得值对象的长度
    } else { // 值对象为空 kv存在
        /* Key exists, check type */
        if (checkType(c,o,OBJ_STRING)) // 检测值对象类型 不是字符串则返回
            return;

        /* "append" is an argument, so always an sds */
        append = c->argv[2]; // 获得append参数
        totlen = stringObjectLen(o)+sdslen(append->ptr); // 计算总长度=原值对象长度+append的长度
        if (checkStringLength(c,totlen) != C_OK) // 总长度大于512M
            return;

        /* Append the value */
        o = dbUnshareStringValue(c->db,c->argv[1],o); // 解除key共享 o是新的值对象
        o->ptr = sdscatlen(o->ptr,append->ptr,sdslen(append->ptr)); // 将值对象的sds和追加的sds拼接并赋给o
        totlen = sdslen(o->ptr); // 获得新的值兑现过的长度
    }
    signalModifiedKey(c->db,c->argv[1]); // 键修改信号
    notifyKeyspaceEvent(NOTIFY_STRING,"append",c->argv[1],c->db->id); // 键空间通知
    server.dirty++;
    addReplyLongLong(c,totlen); // 响应总长度
}
/**
 * strlen key
 * @param c
 */
void strlenCommand(client *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL || // 在db中查找key对应的值对象 如果没有则响应0
        checkType(c,o,OBJ_STRING)) return; // 有 类型不是字符串 则返回
    addReplyLongLong(c,stringObjectLen(o)); // 响应长度
}
