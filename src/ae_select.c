/* Select()-based ae.c module.
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


#include <sys/select.h>
#include <string.h>
/**
 * select
 *
 */
typedef struct aeApiState {
    fd_set /* 读fd集合 */rfds, /* 写fd集合 */wfds;
    /* We need to have a copy of the fd sets as it's not safe to reuse
     * FD sets after select(). */
    fd_set _rfds, _wfds; // 拷贝
} aeApiState;
/**
 * 创建select
 * @param eventLoop
 * @return
 */
static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState)); // 申请内存

    if (!state) return -1;
    FD_ZERO(&state->rfds); // 清空读集合
    FD_ZERO(&state->wfds); // 清空写集合
    eventLoop->apidata = state; // redis时间处理的IO多路复用对象是select
    return 0;
}
/**
 * 重新设置大小
 * @param eventLoop
 * @param setsize
 * @return
 */
static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    /* Just ensure we have enough room in the fd_set type. */
    if (setsize >= FD_SETSIZE) return -1;
    return 0;
}
/**
 * 释放select
 * @param eventLoop
 */
static void aeApiFree(aeEventLoop *eventLoop) {
    zfree(eventLoop->apidata);
}
/**
 * 添加要监听的fd
 * @param eventLoop
 * @param fd
 * @param mask
 * @return
 */
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata; // 获得redis时间处理的IO多路复用对象是select

    if (mask & AE_READABLE) FD_SET(fd,&state->rfds); // 标识是可读,则把fd添加到读集合
    if (mask & AE_WRITABLE) FD_SET(fd,&state->wfds); // 标识是可写,则把fd添加到写集合
    return 0;
}
/**
 * 删除要监听的fd
 * @param eventLoop
 * @param tvp
 * @return
 */
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata; // 获得redis时间处理的IO多路复用对象是select

    if (mask & AE_READABLE) FD_CLR(fd,&state->rfds); // 标识是可读,则从读集合中删除fd
    if (mask & AE_WRITABLE) FD_CLR(fd,&state->wfds); // 标识是可写,则从写集合中删除fd
}
/**
 * 阻塞等待事件发生
 * @param eventLoop
 * @param tvp
 * @return
 */
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata; // 获得redis时间处理的IO多路复用对象是select
    int retval, j, numevents = 0;
    // 从内核拷贝到用户空间
    memcpy(&state->_rfds,&state->rfds,sizeof(fd_set)); // 把rfds中的fd拷贝到_rfds中
    memcpy(&state->_wfds,&state->wfds,sizeof(fd_set)); // 把wfds中的fd拷贝到_wfds中

    retval = select(eventLoop->maxfd+1,
                &state->_rfds,&state->_wfds,NULL,tvp); // 监听所有的fd maxfd的最大数量(从0开始) 超时返回
    if (retval > 0) { // 事件到达
        for (j = 0; j <= eventLoop->maxfd; j++) { // 循环所有的fd
            int mask = 0;
            aeFileEvent *fe = &eventLoop->events[j]; // 获得文件事件

            if (fe->mask == AE_NONE) continue; // 设么都没有 继续
            if (fe->mask & AE_READABLE && FD_ISSET(j,&state->_rfds)) // 标识可读并且在集合中是可读
                mask |= AE_READABLE; // 标志转发为AE_READABLE
            if (fe->mask & AE_WRITABLE && FD_ISSET(j,&state->_wfds)) // 标识可写并且在集合中是可读写
                mask |= AE_WRITABLE; // 标志转发为AE_WRITABLE
            eventLoop->fired[numevents].fd = j; // fd添加到ae的就绪事件fd
            eventLoop->fired[numevents].mask = mask; // 标识添加到ae的就绪事件标识
            numevents++;
        }
    }
    return numevents;
}

static char *aeApiName(void) {
    return "select";
}
