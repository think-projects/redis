/* Linux epoll(2) based ae.c module
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


#include <sys/epoll.h>
/**
 * 封装的epoll
 */
typedef struct aeApiState {
    int epfd; // epoll的文件描述符
    /**
     * int events: 事件类型 epollin:可读事件 epollout:可写事件
     * epoll_data_t data: fd:文件描述符 *ptr:具体的fd的指针
     */
    struct epoll_event *events; // epoll事件结构体 用于存放就绪的事件数组
} aeApiState;
/**
 * 创建epoll对象
 * @param eventLoop redis的事件处理
 * @return
 */
static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState)); // 给epoll的数据存储对象分配内存

    if (!state) return -1;
    state->events = zmalloc(sizeof(struct epoll_event)*eventLoop->setsize); // 给epoll的事件数组分配内存, 用于存储已就绪事件
    if (!state->events) {
        zfree(state);
        return -1;
    }
    state->epfd = epoll_create(1024); /* 1024 is just a hint for the kernel */ // 调用linux系统的epoll_create 创建了eventpoll  1024:默认创建的socket数
    if (state->epfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    eventLoop->apidata = state; // apidata:redis事件处理的io多路复用的对象 state:就是epoll的对象
    return 0;
}
/**
 * 重新分配内存大小
 * @param eventLoop redis事件处理
 * @param setsize 设置的内存大小
 * @return
 */
static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata; // 获得redis事件处理的io多路复用的对象

    state->events = zrealloc(state->events, sizeof(struct epoll_event)*setsize); // epoll的事件数组重新分配内存
    return 0;
}
/**
 * 释放epoll
 * @param eventLoop
 */
static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata; // 获得redis事件处理的io多路复用的对象

    close(state->epfd); // 关闭epoll
    zfree(state->events); // 释放事件数组
    zfree(state); // 释放epoll
}
/**
 * 添加或修改制定fd的监听
 * @param eventLoop redis事件处理
 * @param fd 指定fd
 * @param mask 添加标识
 * @return
 */
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata; // 获得redis事件处理的io多路复用的对象
    struct epoll_event ee = {0}; /* avoid valgrind warning */
    /* If the fd was already monitored for some event, we need a MOD
     * operation. Otherwise we need an ADD operation. */
    // op操作 EPOLL_CTL_ADD:添加 EPOLL_CTL_MOD:修改 EPOLL_CTL_DEL:删除
    int op = eventLoop->events[fd].mask == AE_NONE ?
            EPOLL_CTL_ADD : EPOLL_CTL_MOD; // 判断fd上的标识 如果没有监听 则添加监听 否则 修改监听

    ee.events = 0;
    mask |= eventLoop->events[fd].mask; /* Merge old events */
    // 转epoll的事件类型为ae的事件类型
    if (mask & AE_READABLE) ee.events |= EPOLLIN; // AE_READABLE -> EPOLLIN
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT; // AE_WRITABLE -> EPOLLOUT
    /*
     * epoll_data_t data
     *  fd:文件描述符
     *  ptr:指向文件的指针
     */
    ee.data.fd = fd; // state -- epoll_event --- data
    /*
     * 调用系统的epoll_ctrl
     * epfd: 就是epoll_create创建的epfd
     * op: 操作 注册 修改 删除
     * fd: socket的fd
     * event: 监控的事件
     */
    if (epoll_ctl(state->epfd,op,fd,&ee) == -1) return -1;
    return 0;
}
/**
 * 删除或修改指定fd的家庭
 * @param eventLoop redis的事件处理
 * @param fd 指定socket的fd
 * @param delmask 删除标识
 */
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata; // 获得redis事件处理的io多路复用的对象
    struct epoll_event ee = {0}; /* avoid valgrind warning */
    int mask = eventLoop->events[fd].mask & (~delmask); // events需要监控的事件素组

    ee.events = 0;
    if (mask & AE_READABLE) ee.events |= EPOLLIN; // 指定事件是READABLE则epoll事件是EPOLLIN
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT; // 指定事件是WRITABLE则epoll事件是EPOLLOUT
    ee.data.fd = fd; // 指定fd赋值给epoll的data的fd
    if (mask != AE_NONE) { // fd上有监听事件
        epoll_ctl(state->epfd,EPOLL_CTL_MOD,fd,&ee); // 监听事件的修改
    } else { // 没有监听事件
        /* Note, Kernel < 2.6.9 requires a non null event pointer even for
         * EPOLL_CTL_DEL. */
        epoll_ctl(state->epfd,EPOLL_CTL_DEL,fd,&ee); // 删除监听事件
    }
}
/**
 * 阻塞等待事件发生
 * @param eventLoop redis的事件处理
 * @param tvp 超时事件 正整数:超过tvp还没有时间发生,则返回超时 0:直接返回 -1:有事件发生才返回
 * @return
 */
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata; // 获得redis事件处理的io多路复用的对象
    int retval, numevents = 0;
    /*
     * 调用epoll_wait阻塞等待时间发生
     * epfd:epoll_create返回的epfd
     * events:就绪的事件
     * setsize:能处理的最大事件数目
     * tvp: 超时时间 毫秒
     */
    retval = epoll_wait(state->epfd,state->events,eventLoop->setsize,
            tvp ? (tvp->tv_sec*1000 + tvp->tv_usec/1000) : -1);
    if (retval > 0) {
        int j;

        numevents = retval; // 已就绪的数量
        for (j = 0; j < numevents; j++) { // 循环已就绪的事件数组
            int mask = 0;
            struct epoll_event *e = state->events+j;
            // 将epoll的事件类型转为ae的事件类型
            if (e->events & EPOLLIN) mask |= AE_READABLE;
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE;
            if (e->events & EPOLLERR) mask |= AE_WRITABLE;
            if (e->events & EPOLLHUP) mask |= AE_WRITABLE;
            eventLoop->fired[j].fd = e->data.fd; // ae的就绪事件的fd=epoll中就绪事件的fd
            eventLoop->fired[j].mask = mask; // ae的就是事件的类型
        }
    }
    return numevents;
}
/**
 * 获得io多路复用的名称
 * @return
 */
static char *aeApiName(void) {
    return "epoll";
}
