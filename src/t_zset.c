/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
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

/*-----------------------------------------------------------------------------
 * Sorted set API
 *----------------------------------------------------------------------------*/

/* ZSETs are ordered sets using two data structures to hold the same elements
 * in order to get O(log(N)) INSERT and REMOVE operations into a sorted
 * data structure.
 *
 * The elements are added to a hash table mapping Redis objects to scores.
 * At the same time the elements are added to a skip list mapping scores
 * to Redis objects (so objects are sorted by scores in this "view").
 *
 * Note that the SDS string representing the element is the same in both
 * the hash table and skiplist in order to save memory. What we do in order
 * to manage the shared SDS string more easily is to free the SDS string
 * only in zslFreeNode(). The dictionary has no value free method set.
 * So we should always remove an element from the dictionary, and later from
 * the skiplist.
 *
 * This skiplist implementation is almost a C translation of the original
 * algorithm described by William Pugh in "Skip Lists: A Probabilistic
 * Alternative to Balanced Trees", modified in three ways:
 * a) this implementation allows for repeated scores.
 * b) the comparison is not just by key (our 'score') but by satellite data.
 * c) there is a back pointer, so it's a doubly linked list with the back
 * pointers being only at "level 1". This allows to traverse the list
 * from tail to head, useful for ZREVRANGE. */

#include "server.h"
#include <math.h>

/*-----------------------------------------------------------------------------
 * Skiplist implementation of the low level API
 *----------------------------------------------------------------------------*/

int zslLexValueGteMin(sds value, zlexrangespec *spec);
int zslLexValueLteMax(sds value, zlexrangespec *spec);
/**
 * 创建节点
 * @param level 层高
 * @param score 分值
 * @param ele 成员
 * @return
 */
/* Create a skiplist node with the specified number of levels.
 * The SDS string 'ele' is referenced by the node after the call. */
zskiplistNode *zslCreateNode(int level, double score, sds ele) {
    zskiplistNode *zn =
        zmalloc(sizeof(*zn)+level*sizeof(struct zskiplistLevel)); // 申请内存 node + level
    zn->score = score; // 分值
    zn->ele = ele; // 成员
    return zn;
}
/**
 * 创建一个跳跃表
 * @param void 无
 * @return zsl
 */
/* Create a new skiplist. */
zskiplist *zslCreate(void) {
    int j;
    zskiplist *zsl; // 定义一个跳跃表

    zsl = zmalloc(sizeof(*zsl)); // 分配内存
    zsl->level = 1; // 层高1
    zsl->length = 0; // 无节点
    zsl->header = zslCreateNode(ZSKIPLIST_MAXLEVEL,0,NULL); // 创建头节点
    for (j = 0; j < ZSKIPLIST_MAXLEVEL; j++) { // 初始化63层
        zsl->header->level[j].forward = NULL; // head的forwad都是空
        zsl->header->level[j].span = 0; // head的跨度(span)都是0
    }
    zsl->header->backward = NULL; // 后节点null
    zsl->tail = NULL; // 尾结点null
    return zsl;
}
/**
 * 释放节点内存
 * @param node
 */
/* Free the specified skiplist node. The referenced SDS string representation
 * of the element is freed too, unless node->ele is set to NULL before calling
 * this function. */
void zslFreeNode(zskiplistNode *node) {
    sdsfree(node->ele); // 释放ele
    zfree(node); // 释放nde
}

/**
 * 删除整个跳跃表
 * @param zsl
 */
/* Free a whole skiplist. */
void zslFree(zskiplist *zsl) {
    zskiplistNode *node = zsl->header->level[0].forward, *next; // 底层头节点

    zfree(zsl->header); // 释放头
    while(node) { // 如果有有节点
        next = node->level[0].forward; // next指向下一个
        zslFreeNode(node); // 释放当前节点
        node = next; // 指向下一个
    }
    zfree(zsl); // 节点全部释放完后释放zlsl
}

/* Returns a random level for the new skiplist node we are going to create.
 * The return value of this function is between 1 and ZSKIPLIST_MAXLEVEL
 * (both inclusive), with a powerlaw-alike distribution where higher
 * levels are less likely to be returned. */
int zslRandomLevel(void) { // 获得随机level
    int level = 1; // level赋初始值=1
    /**
     * 循环判断 (random()&0xFFFF) < (ZSKIPLIST_P * 0xFFFF)
     * 有1/4的概率使level++
     */
    while ((random()&0xFFFF) < (ZSKIPLIST_P * 0xFFFF))
        level += 1;
    return (level<ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL; // level < 64 返回level,否则返回64
}
/**
 * 插入节点
 * @param zsl
 * @param score 分值
 * @param ele
 * @return
 */
/* Insert a new node in the skiplist. Assumes the element does not already
 * exist (up to the caller to enforce that). The skiplist takes ownership
 * of the passed SDS string 'ele'. */
zskiplistNode *zslInsert(zskiplist *zsl, double score, sds ele) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x; // 记录搜索的节点
    unsigned int rank[ZSKIPLIST_MAXLEVEL]; // 计算span
    int i, level;

    serverAssert(!isnan(score));
    x = zsl->header; // 头
    for (i = zsl->level-1; i >= 0; i--) { // 从顶层遍历
        /* store rank that is crossed to reach the insert position */
        rank[i] = i == (zsl->level-1) ? 0 : rank[i+1]; // 不是顶层i层的起始rank值为下层rank
        /* 当前节点的下一个的分支小于指定分或分值相等但ele小 */
        while (x->level[i].forward &&
                (x->level[i].forward->score < score ||
                    (x->level[i].forward->score == score &&
                    sdscmp(x->level[i].forward->ele,ele) < 0)))
        {
            rank[i] += x->level[i].span; // 本层rank累加
            x = x->level[i].forward; // 本层下一个
        }
        update[i] = x; // 记录搜索的节点
    }
    /* we assume the element is not already inside, since we allow duplicated
     * scores, reinserting the same element should never happen since the
     * caller of zslInsert() should test in the hash table if the element is
     * already inside or not. */
    level = zslRandomLevel(); // 获得随机level
    if (level > zsl->level) { // 如果获得的level大于层高
        for (i = zsl->level; i < level; i++) { // 处理高于层高的节点
            rank[i] = 0;
            update[i] = zsl->header; // 经过的节点为头结点
            update[i]->level[i].span = zsl->length; // 头节点span为跳跃表长度
        }
        zsl->level = level; // 新的level
    }
    x = zslCreateNode(level,score,ele); // 创建新节点
    for (i = 0; i < level; i++) {
        x->level[i].forward = update[i]->level[i].forward; // 依次设置新节点的forward
        update[i]->level[i].forward = x; // 将沿途记录的各个节点的forward指向新节点

        /* update span covered by update[i] as x is inserted here */
        x->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]); // 计算新节点的跨度
        update[i]->level[i].span = (rank[0] - rank[i]) + 1; // 更新沿途的span值
    }
    /* 未接触的节点span+1 */
    /* increment span for untouched levels */
    for (i = level; i < zsl->level; i++) {
        update[i]->level[i].span++;
    }

    x->backward = (update[0] == zsl->header) ? NULL : update[0]; // 设置backwork
    if (x->level[0].forward)
        x->level[0].forward->backward = x;
    else
        zsl->tail = x;
    zsl->length++; // length+1
    return x; // 返回新节点
}

/* Internal function used by zslDelete, zslDeleteByScore and zslDeleteByRank */
void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update) {
    int i;
    // 从底层开始循环
    for (i = 0; i < zsl->level; i++) {
        /* 如果沿途节点的下一个是要删除的 */
        if (update[i]->level[i].forward == x) {
            update[i]->level[i].span += x->level[i].span - 1; // span累加当前要删除节点span减一
            update[i]->level[i].forward = x->level[i].forward; // 沿途节点的下一个节点是要删除节点的下一个节点
        } else {
            update[i]->level[i].span -= 1; // span -1
        }
    }
    if (x->level[0].forward) { // 不是最后一个节点
        x->level[0].forward->backward = x->backward; // 重置backward 删除节点的下一个节点的上一个节点 = 删除节点的上一个节点
    } else { // 最后一个节点
        zsl->tail = x->backward; // 尾设置为要删除节点的上一个节点
    }
    while(zsl->level > 1 && zsl->header->level[zsl->level-1].forward == NULL) // 如果删除接地那的level最大
        zsl->level--; // level层高减1
    zsl->length--; // length长度减1
}
/**
 * 删除节点
 * @param zsl
 * @param score 分值
 * @param ele
 * @param node
 * @return
 */
/* Delete an element with matching score/element from the skiplist.
 * The function returns 1 if the node was found and deleted, otherwise
 * 0 is returned.
 *
 * If 'node' is NULL the deleted node is freed by zslFreeNode(), otherwise
 * it is not freed (but just unlinked) and *node is set to the node pointer,
 * so that it is possible for the caller to reuse the node (including the
 * referenced SDS string at node->ele). */
int zslDelete(zskiplist *zsl, double score, sds ele, zskiplistNode **node) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x; // 记录沿途节点
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) { // 从顶层头节点开始
        /* 当前节点有下一个 && 当前节点下一个的分支小于或(等于指定分值但ele小于) */
        while (x->level[i].forward &&
                (x->level[i].forward->score < score ||
                    (x->level[i].forward->score == score &&
                     sdscmp(x->level[i].forward->ele,ele) < 0)))
        {
            x = x->level[i].forward; // 本层下一个
        }
        update[i] = x; // 大于 记录沿途节点
    }
    /* We may have multiple elements with the same score, what we need
     * is to find the element with both the right score and object. */
    x = x->level[0].forward;
    if (x && score == x->score && sdscmp(x->ele,ele) == 0) { // 分支相同 && ele相同
        zslDeleteNode(zsl, x, update); // 删除
        if (!node)
            zslFreeNode(x); // 释放内存
        else
            *node = x;
        return 1;
    }
    return 0; /* not found */
}

/* Update the score of an elmenent inside the sorted set skiplist.
 * Note that the element must exist and must match 'score'.
 * This function does not update the score in the hash table side, the
 * caller should take care of it.
 *
 * Note that this function attempts to just update the node, in case after
 * the score update, the node would be exactly at the same position.
 * Otherwise the skiplist is modified by removing and re-adding a new
 * element, which is more costly.
 *
 * The function returns the updated element skiplist node pointer. */
zskiplistNode *zslUpdateScore(zskiplist *zsl, double curscore, sds ele, double newscore) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    int i;

    /* We need to seek to element to update to start: this is useful anyway,
     * we'll have to update or remove it. */
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward &&
                (x->level[i].forward->score < curscore ||
                    (x->level[i].forward->score == curscore &&
                     sdscmp(x->level[i].forward->ele,ele) < 0)))
        {
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    /* Jump to our element: note that this function assumes that the
     * element with the matching score exists. */
    x = x->level[0].forward;
    serverAssert(x && curscore == x->score && sdscmp(x->ele,ele) == 0);

    /* If the node, after the score update, would be still exactly
     * at the same position, we can just update the score without
     * actually removing and re-inserting the element in the skiplist. */
    if ((x->backward == NULL || x->backward->score < newscore) &&
        (x->level[0].forward == NULL || x->level[0].forward->score > newscore))
    {
        x->score = newscore;
        return x;
    }

    /* No way to reuse the old node: we need to remove and insert a new
     * one at a different place. */
    zslDeleteNode(zsl, x, update);
    zskiplistNode *newnode = zslInsert(zsl,newscore,x->ele);
    /* We reused the old node x->ele SDS string, free the node now
     * since zslInsert created a new one. */
    x->ele = NULL;
    zslFreeNode(x);
    return newnode;
}

int zslValueGteMin(double value, zrangespec *spec) {
    return spec->minex ? (value > spec->min) : (value >= spec->min);
}

int zslValueLteMax(double value, zrangespec *spec) {
    return spec->maxex ? (value < spec->max) : (value <= spec->max);
}

/* Returns if there is a part of the zset is in range. */
int zslIsInRange(zskiplist *zsl, zrangespec *range) {
    zskiplistNode *x;

    /* Test for ranges that will always be empty. */
    if (range->min > range->max ||
            (range->min == range->max && (range->minex || range->maxex)))
        return 0;
    x = zsl->tail;
    if (x == NULL || !zslValueGteMin(x->score,range))
        return 0;
    x = zsl->header->level[0].forward;
    if (x == NULL || !zslValueLteMax(x->score,range))
        return 0;
    return 1;
}

/* Find the first node that is contained in the specified range.
 * Returns NULL when no element is contained in the range. */
zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range) {
    zskiplistNode *x;
    int i;

    /* If everything is out of range, return early. */
    if (!zslIsInRange(zsl,range)) return NULL;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        /* Go forward while *OUT* of range. */
        while (x->level[i].forward &&
            !zslValueGteMin(x->level[i].forward->score,range))
                x = x->level[i].forward;
    }

    /* This is an inner range, so the next node cannot be NULL. */
    x = x->level[0].forward;
    serverAssert(x != NULL);

    /* Check if score <= max. */
    if (!zslValueLteMax(x->score,range)) return NULL;
    return x;
}

/* Find the last node that is contained in the specified range.
 * Returns NULL when no element is contained in the range. */
zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range) {
    zskiplistNode *x;
    int i;

    /* If everything is out of range, return early. */
    if (!zslIsInRange(zsl,range)) return NULL;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        /* Go forward while *IN* range. */
        while (x->level[i].forward &&
            zslValueLteMax(x->level[i].forward->score,range))
                x = x->level[i].forward;
    }

    /* This is an inner range, so this node cannot be NULL. */
    serverAssert(x != NULL);

    /* Check if score >= min. */
    if (!zslValueGteMin(x->score,range)) return NULL;
    return x;
}

/* Delete all the elements with score between min and max from the skiplist.
 * Min and max are inclusive, so a score >= min || score <= max is deleted.
 * Note that this function takes the reference to the hash table view of the
 * sorted set, in order to remove the elements from the hash table too. */
unsigned long zslDeleteRangeByScore(zskiplist *zsl, zrangespec *range, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward && (range->minex ?
            x->level[i].forward->score <= range->min :
            x->level[i].forward->score < range->min))
                x = x->level[i].forward;
        update[i] = x;
    }

    /* Current node is the last with score < or <= min. */
    x = x->level[0].forward;

    /* Delete nodes while in range. */
    while (x &&
           (range->maxex ? x->score < range->max : x->score <= range->max))
    {
        zskiplistNode *next = x->level[0].forward;
        zslDeleteNode(zsl,x,update);
        dictDelete(dict,x->ele);
        zslFreeNode(x); /* Here is where x->ele is actually released. */
        removed++;
        x = next;
    }
    return removed;
}

unsigned long zslDeleteRangeByLex(zskiplist *zsl, zlexrangespec *range, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;


    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward &&
            !zslLexValueGteMin(x->level[i].forward->ele,range))
                x = x->level[i].forward;
        update[i] = x;
    }

    /* Current node is the last with score < or <= min. */
    x = x->level[0].forward;

    /* Delete nodes while in range. */
    while (x && zslLexValueLteMax(x->ele,range)) {
        zskiplistNode *next = x->level[0].forward;
        zslDeleteNode(zsl,x,update);
        dictDelete(dict,x->ele);
        zslFreeNode(x); /* Here is where x->ele is actually released. */
        removed++;
        x = next;
    }
    return removed;
}

/* Delete all the elements with rank between start and end from the skiplist.
 * Start and end are inclusive. Note that start and end need to be 1-based */
unsigned long zslDeleteRangeByRank(zskiplist *zsl, unsigned int start, unsigned int end, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long traversed = 0, removed = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward && (traversed + x->level[i].span) < start) {
            traversed += x->level[i].span;
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    traversed++;
    x = x->level[0].forward;
    while (x && traversed <= end) {
        zskiplistNode *next = x->level[0].forward;
        zslDeleteNode(zsl,x,update); // 删除节点
        dictDelete(dict,x->ele); // 删除dict的key
        zslFreeNode(x);
        removed++;
        traversed++;
        x = next;
    }
    return removed;
}

/**
 * 查找排位
 * @param zsl
 * @param score 分值
 * @param ele  ele
 * @return  rank
 */
/* Find the rank for an element by both score and key.
 * Returns 0 when the element cannot be found, rank otherwise.
 * Note that the rank is 1-based due to the span of zsl->header to the
 * first element. */
unsigned long zslGetRank(zskiplist *zsl, double score, sds ele) {
    zskiplistNode *x;
    unsigned long rank = 0; // 定义一个排位
    int i;

    x = zsl->header; // 从头节点开始
    for (i = zsl->level-1; i >= 0; i--) { // level数法:从0层开始,level是3,level层是2
        /* 下一个节点存在并且下一个节点的分值小于指定分值 || 分值相同 && ele小于给定(字典序) */
        while (x->level[i].forward &&
            (x->level[i].forward->score < score ||
                (x->level[i].forward->score == score &&
                sdscmp(x->level[i].forward->ele,ele) <= 0))) {
            rank += x->level[i].span; // 累加当前节点的span
            x = x->level[i].forward; // 本层的下一个节点
        }

        /* x might be equal to zsl->header, so test if obj is non-NULL */
        if (x->ele && sdscmp(x->ele,ele) == 0) { // score大于当前节点 && ele相同
            return rank;
        }
    }
    return 0;
}
/**
 * 根据rank获得节点
 * @param zsl
 * @param rank
 * @return 节点
 */
/* Finds an element by its rank. The rank argument needs to be 1-based. */
zskiplistNode* zslGetElementByRank(zskiplist *zsl, unsigned long rank) {
    zskiplistNode *x;
    unsigned long traversed = 0; // 累计节点span
    int i;

    x = zsl->header;
    /* 从顶层开始循环 */
    for (i = zsl->level-1; i >= 0; i--) {
        /* 有下一个节点 && 累计节点span + 当前节点span <= rank */
        while (x->level[i].forward && (traversed + x->level[i].span) <= rank)
        {
            traversed += x->level[i].span; // 累加span
            x = x->level[i].forward; // 本层下一个
        }
        if (traversed == rank) { // 累计节点psan等于rank
            return x; // 返回rank
        }
    }
    return NULL;
}

/* Populate the rangespec according to the objects min and max. */
static int zslParseRange(robj *min, robj *max, zrangespec *spec) {
    char *eptr;
    spec->minex = spec->maxex = 0;

    /* Parse the min-max interval. If one of the values is prefixed
     * by the "(" character, it's considered "open". For instance
     * ZRANGEBYSCORE zset (1.5 (2.5 will match min < x < max
     * ZRANGEBYSCORE zset 1.5 2.5 will instead match min <= x <= max */
    if (min->encoding == OBJ_ENCODING_INT) { // 包含
        spec->min = (long)min->ptr;
    } else { // 不包换
        if (((char*)min->ptr)[0] == '(') {
            spec->min = strtod((char*)min->ptr+1,&eptr);
            if (eptr[0] != '\0' || isnan(spec->min)) return C_ERR;
            spec->minex = 1; // 不包含
        } else {
            spec->min = strtod((char*)min->ptr,&eptr);
            if (eptr[0] != '\0' || isnan(spec->min)) return C_ERR;
        }
    }
    if (max->encoding == OBJ_ENCODING_INT) {
        spec->max = (long)max->ptr;
    } else {
        if (((char*)max->ptr)[0] == '(') {
            spec->max = strtod((char*)max->ptr+1,&eptr);
            if (eptr[0] != '\0' || isnan(spec->max)) return C_ERR;
            spec->maxex = 1;
        } else {
            spec->max = strtod((char*)max->ptr,&eptr);
            if (eptr[0] != '\0' || isnan(spec->max)) return C_ERR;
        }
    }

    return C_OK;
}

/* ------------------------ Lexicographic ranges ---------------------------- */

/* Parse max or min argument of ZRANGEBYLEX.
  * (foo means foo (open interval)
  * [foo means foo (closed interval)
  * - means the min string possible
  * + means the max string possible
  *
  * If the string is valid the *dest pointer is set to the redis object
  * that will be used for the comparison, and ex will be set to 0 or 1
  * respectively if the item is exclusive or inclusive. C_OK will be
  * returned.
  *
  * If the string is not a valid range C_ERR is returned, and the value
  * of *dest and *ex is undefined. */
int zslParseLexRangeItem(robj *item, sds *dest, int *ex) {
    char *c = item->ptr;

    switch(c[0]) {
    case '+': // 最大字母
        if (c[1] != '\0') return C_ERR;
        *ex = 1;
        *dest = shared.maxstring;
        return C_OK;
    case '-': // 最小字母
        if (c[1] != '\0') return C_ERR;
        *ex = 1;
        *dest = shared.minstring;
        return C_OK;
    case '(': // 不包含
        *ex = 1;
        *dest = sdsnewlen(c+1,sdslen(c)-1);
        return C_OK;
    case '[': // 包含
        *ex = 0;
        *dest = sdsnewlen(c+1,sdslen(c)-1);
        return C_OK;
    default:
        return C_ERR;
    }
}

/* Free a lex range structure, must be called only after zelParseLexRange()
 * populated the structure with success (C_OK returned). */
void zslFreeLexRange(zlexrangespec *spec) {
    if (spec->min != shared.minstring &&
        spec->min != shared.maxstring) sdsfree(spec->min);
    if (spec->max != shared.minstring &&
        spec->max != shared.maxstring) sdsfree(spec->max);
}

/* Populate the lex rangespec according to the objects min and max.
 *
 * Return C_OK on success. On error C_ERR is returned.
 * When OK is returned the structure must be freed with zslFreeLexRange(),
 * otherwise no release is needed. */
int zslParseLexRange(robj *min, robj *max, zlexrangespec *spec) {
    /* The range can't be valid if objects are integer encoded.
     * Every item must start with ( or [. */
    if (min->encoding == OBJ_ENCODING_INT ||
        max->encoding == OBJ_ENCODING_INT) return C_ERR;

    spec->min = spec->max = NULL;
    if (zslParseLexRangeItem(min, &spec->min, &spec->minex) == C_ERR ||
        zslParseLexRangeItem(max, &spec->max, &spec->maxex) == C_ERR) {
        zslFreeLexRange(spec);
        return C_ERR;
    } else {
        return C_OK;
    }
}

/* This is just a wrapper to sdscmp() that is able to
 * handle shared.minstring and shared.maxstring as the equivalent of
 * -inf and +inf for strings */
int sdscmplex(sds a, sds b) {
    if (a == b) return 0;
    if (a == shared.minstring || b == shared.maxstring) return -1;
    if (a == shared.maxstring || b == shared.minstring) return 1;
    return sdscmp(a,b);
}

int zslLexValueGteMin(sds value, zlexrangespec *spec) {
    return spec->minex ?
        (sdscmplex(value,spec->min) > 0) :
        (sdscmplex(value,spec->min) >= 0);
}

int zslLexValueLteMax(sds value, zlexrangespec *spec) {
    return spec->maxex ?
        (sdscmplex(value,spec->max) < 0) :
        (sdscmplex(value,spec->max) <= 0);
}

/* Returns if there is a part of the zset is in the lex range. */
int zslIsInLexRange(zskiplist *zsl, zlexrangespec *range) {
    zskiplistNode *x;

    /* Test for ranges that will always be empty. */
    int cmp = sdscmplex(range->min,range->max);
    if (cmp > 0 || (cmp == 0 && (range->minex || range->maxex)))
        return 0;
    x = zsl->tail;
    if (x == NULL || !zslLexValueGteMin(x->ele,range))
        return 0;
    x = zsl->header->level[0].forward;
    if (x == NULL || !zslLexValueLteMax(x->ele,range))
        return 0;
    return 1;
}

/* Find the first node that is contained in the specified lex range.
 * Returns NULL when no element is contained in the range. */
zskiplistNode *zslFirstInLexRange(zskiplist *zsl, zlexrangespec *range) {
    zskiplistNode *x;
    int i;

    /* If everything is out of range, return early. */
    if (!zslIsInLexRange(zsl,range)) return NULL;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        /* Go forward while *OUT* of range. */
        while (x->level[i].forward &&
            !zslLexValueGteMin(x->level[i].forward->ele,range))
                x = x->level[i].forward;
    }

    /* This is an inner range, so the next node cannot be NULL. */
    x = x->level[0].forward;
    serverAssert(x != NULL);

    /* Check if score <= max. */
    if (!zslLexValueLteMax(x->ele,range)) return NULL;
    return x;
}

/* Find the last node that is contained in the specified range.
 * Returns NULL when no element is contained in the range. */
zskiplistNode *zslLastInLexRange(zskiplist *zsl, zlexrangespec *range) {
    zskiplistNode *x;
    int i;

    /* If everything is out of range, return early. */
    if (!zslIsInLexRange(zsl,range)) return NULL;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        /* Go forward while *IN* range. */
        while (x->level[i].forward &&
            zslLexValueLteMax(x->level[i].forward->ele,range))
                x = x->level[i].forward;
    }

    /* This is an inner range, so this node cannot be NULL. */
    serverAssert(x != NULL);

    /* Check if score >= min. */
    if (!zslLexValueGteMin(x->ele,range)) return NULL;
    return x;
}

/*-----------------------------------------------------------------------------
 * Ziplist-backed sorted set API
 *----------------------------------------------------------------------------*/

double zzlGetScore(unsigned char *sptr) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    char buf[128];
    double score;

    serverAssert(sptr != NULL);
    serverAssert(ziplistGet(sptr,&vstr,&vlen,&vlong));

    if (vstr) {
        memcpy(buf,vstr,vlen);
        buf[vlen] = '\0';
        score = strtod(buf,NULL);
    } else {
        score = vlong;
    }

    return score;
}

/* Return a ziplist element as an SDS string. */
sds ziplistGetObject(unsigned char *sptr) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;

    serverAssert(sptr != NULL);
    serverAssert(ziplistGet(sptr,&vstr,&vlen,&vlong));

    if (vstr) {
        return sdsnewlen((char*)vstr,vlen);
    } else {
        return sdsfromlonglong(vlong);
    }
}

/* Compare element in sorted set with given element. */
int zzlCompareElements(unsigned char *eptr, unsigned char *cstr, unsigned int clen) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    unsigned char vbuf[32];
    int minlen, cmp;

    serverAssert(ziplistGet(eptr,&vstr,&vlen,&vlong));
    if (vstr == NULL) {
        /* Store string representation of long long in buf. */
        vlen = ll2string((char*)vbuf,sizeof(vbuf),vlong);
        vstr = vbuf;
    }

    minlen = (vlen < clen) ? vlen : clen;
    cmp = memcmp(vstr,cstr,minlen);
    if (cmp == 0) return vlen-clen;
    return cmp;
}

unsigned int zzlLength(unsigned char *zl) {
    return ziplistLen(zl)/2; // score member 所以除以2
}

/* Move to next entry based on the values in eptr and sptr. Both are set to
 * NULL when there is no next entry. */
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr) {
    unsigned char *_eptr, *_sptr;
    serverAssert(*eptr != NULL && *sptr != NULL);

    _eptr = ziplistNext(zl,*sptr);
    if (_eptr != NULL) {
        _sptr = ziplistNext(zl,_eptr);
        serverAssert(_sptr != NULL);
    } else {
        /* No next entry. */
        _sptr = NULL;
    }

    *eptr = _eptr;
    *sptr = _sptr;
}

/* Move to the previous entry based on the values in eptr and sptr. Both are
 * set to NULL when there is no next entry. */
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr) {
    unsigned char *_eptr, *_sptr;
    serverAssert(*eptr != NULL && *sptr != NULL);

    _sptr = ziplistPrev(zl,*eptr);
    if (_sptr != NULL) {
        _eptr = ziplistPrev(zl,_sptr);
        serverAssert(_eptr != NULL);
    } else {
        /* No previous entry. */
        _eptr = NULL;
    }

    *eptr = _eptr;
    *sptr = _sptr;
}

/* Returns if there is a part of the zset is in range. Should only be used
 * internally by zzlFirstInRange and zzlLastInRange. */
int zzlIsInRange(unsigned char *zl, zrangespec *range) {
    unsigned char *p;
    double score;

    /* Test for ranges that will always be empty. */
    if (range->min > range->max ||
            (range->min == range->max && (range->minex || range->maxex)))
        return 0;

    p = ziplistIndex(zl,-1); /* Last score. */
    if (p == NULL) return 0; /* Empty sorted set */
    score = zzlGetScore(p);
    if (!zslValueGteMin(score,range))
        return 0;

    p = ziplistIndex(zl,1); /* First score. */
    serverAssert(p != NULL);
    score = zzlGetScore(p);
    if (!zslValueLteMax(score,range))
        return 0;

    return 1;
}

/* Find pointer to the first element contained in the specified range.
 * Returns NULL when no element is contained in the range. */
unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec *range) {
    unsigned char *eptr = ziplistIndex(zl,0), *sptr;
    double score;

    /* If everything is out of range, return early. */
    if (!zzlIsInRange(zl,range)) return NULL;

    while (eptr != NULL) {
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);

        score = zzlGetScore(sptr);
        if (zslValueGteMin(score,range)) {
            /* Check if score <= max. */
            if (zslValueLteMax(score,range))
                return eptr;
            return NULL;
        }

        /* Move to next element. */
        eptr = ziplistNext(zl,sptr);
    }

    return NULL;
}

/* Find pointer to the last element contained in the specified range.
 * Returns NULL when no element is contained in the range. */
unsigned char *zzlLastInRange(unsigned char *zl, zrangespec *range) {
    unsigned char *eptr = ziplistIndex(zl,-2), *sptr;
    double score;

    /* If everything is out of range, return early. */
    if (!zzlIsInRange(zl,range)) return NULL;

    while (eptr != NULL) {
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);

        score = zzlGetScore(sptr);
        if (zslValueLteMax(score,range)) {
            /* Check if score >= min. */
            if (zslValueGteMin(score,range))
                return eptr;
            return NULL;
        }

        /* Move to previous element by moving to the score of previous element.
         * When this returns NULL, we know there also is no element. */
        sptr = ziplistPrev(zl,eptr);
        if (sptr != NULL)
            serverAssert((eptr = ziplistPrev(zl,sptr)) != NULL);
        else
            eptr = NULL;
    }

    return NULL;
}

int zzlLexValueGteMin(unsigned char *p, zlexrangespec *spec) {
    sds value = ziplistGetObject(p);
    int res = zslLexValueGteMin(value,spec);
    sdsfree(value);
    return res;
}

int zzlLexValueLteMax(unsigned char *p, zlexrangespec *spec) {
    sds value = ziplistGetObject(p);
    int res = zslLexValueLteMax(value,spec);
    sdsfree(value);
    return res;
}

/* Returns if there is a part of the zset is in range. Should only be used
 * internally by zzlFirstInRange and zzlLastInRange. */
int zzlIsInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *p;

    /* Test for ranges that will always be empty. */
    int cmp = sdscmplex(range->min,range->max);
    if (cmp > 0 || (cmp == 0 && (range->minex || range->maxex)))
        return 0;

    p = ziplistIndex(zl,-2); /* Last element. */
    if (p == NULL) return 0;
    if (!zzlLexValueGteMin(p,range))
        return 0;

    p = ziplistIndex(zl,0); /* First element. */
    serverAssert(p != NULL);
    if (!zzlLexValueLteMax(p,range))
        return 0;

    return 1;
}

/* Find pointer to the first element contained in the specified lex range.
 * Returns NULL when no element is contained in the range. */
unsigned char *zzlFirstInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *eptr = ziplistIndex(zl,0), *sptr;

    /* If everything is out of range, return early. */
    if (!zzlIsInLexRange(zl,range)) return NULL;

    while (eptr != NULL) {
        if (zzlLexValueGteMin(eptr,range)) {
            /* Check if score <= max. */
            if (zzlLexValueLteMax(eptr,range))
                return eptr;
            return NULL;
        }

        /* Move to next element. */
        sptr = ziplistNext(zl,eptr); /* This element score. Skip it. */
        serverAssert(sptr != NULL);
        eptr = ziplistNext(zl,sptr); /* Next element. */
    }

    return NULL;
}

/* Find pointer to the last element contained in the specified lex range.
 * Returns NULL when no element is contained in the range. */
unsigned char *zzlLastInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *eptr = ziplistIndex(zl,-2), *sptr;

    /* If everything is out of range, return early. */
    if (!zzlIsInLexRange(zl,range)) return NULL;

    while (eptr != NULL) {
        if (zzlLexValueLteMax(eptr,range)) {
            /* Check if score >= min. */
            if (zzlLexValueGteMin(eptr,range))
                return eptr;
            return NULL;
        }

        /* Move to previous element by moving to the score of previous element.
         * When this returns NULL, we know there also is no element. */
        sptr = ziplistPrev(zl,eptr);
        if (sptr != NULL)
            serverAssert((eptr = ziplistPrev(zl,sptr)) != NULL);
        else
            eptr = NULL;
    }

    return NULL;
}

unsigned char *zzlFind(unsigned char *zl, sds ele, double *score) {
    unsigned char *eptr = ziplistIndex(zl,0), *sptr; // 找ziplist的第一个元素

    while (eptr != NULL) {
        sptr = ziplistNext(zl,eptr); // 找下一个
        serverAssert(sptr != NULL);

        if (ziplistCompare(eptr,(unsigned char*)ele,sdslen(ele))) { // 元素与指定元素相同
            /* Matching element, pull out score. */
            if (score != NULL) *score = zzlGetScore(sptr); // 获得分支
            return eptr;
        }

        /* Move to next element. */
        eptr = ziplistNext(zl,sptr); // 下一个
    }
    return NULL;
}

/* Delete (element,score) pair from ziplist. Use local copy of eptr because we
 * don't want to modify the one given as argument. */
unsigned char *zzlDelete(unsigned char *zl, unsigned char *eptr) {
    unsigned char *p = eptr;

    /* TODO: add function to ziplist API to delete N elements from offset. */
    zl = ziplistDelete(zl,&p);
    zl = ziplistDelete(zl,&p);
    return zl;
}

unsigned char *zzlInsertAt(unsigned char *zl, unsigned char *eptr, sds ele, double score) {
    unsigned char *sptr;
    char scorebuf[128];
    int scorelen;
    size_t offset;

    scorelen = d2string(scorebuf,sizeof(scorebuf),score);
    if (eptr == NULL) { // 元素是空
        zl = ziplistPush(zl,(unsigned char*)ele,sdslen(ele),ZIPLIST_TAIL); // 插入ele
        zl = ziplistPush(zl,(unsigned char*)scorebuf,scorelen,ZIPLIST_TAIL); // 插入分支
    } else { // 元素不是空
        /* Keep offset relative to zl, as it might be re-allocated. */
        offset = eptr-zl;
        zl = ziplistInsert(zl,eptr,(unsigned char*)ele,sdslen(ele)); // 在eptr的位置插入ele 后续元素后裔
        eptr = zl+offset;

        /* Insert score after the element. */
        serverAssert((sptr = ziplistNext(zl,eptr)) != NULL);
        zl = ziplistInsert(zl,sptr,(unsigned char*)scorebuf,scorelen); // 插入score
    }
    return zl;
}

/* Insert (element,score) pair in ziplist. This function assumes the element is
 * not yet present in the list. */
/**
 * 插入元素和分值
 * @param zl 要插入dezl
 * @param ele 元素
 * @param score 分值
 * @return
 */
unsigned char *zzlInsert(unsigned char *zl, sds ele, double score) {
    unsigned char *eptr = ziplistIndex(zl,0), *sptr; // 找到的第一个元素
    double s;

    while (eptr != NULL) {
        sptr = ziplistNext(zl,eptr); // 指向eptr下一个元素 就是分值 score
        serverAssert(sptr != NULL);
        s = zzlGetScore(sptr); // 获得第一个元素的分值

        if (s > score) { // 第一个元素的分值>插入分支
            /* First element with score larger than score for element to be
             * inserted. This means we should take its spot in the list to
             * maintain ordering. */
            zl = zzlInsertAt(zl,eptr,ele,score); // 按照分支的从小到大的顺序插入 ele1 score1 ele2 score2 (score1<score2)
            break;
        } else if (s == score) { // 分值想到
            /* Ensure lexicographical ordering for elements. */
            if (zzlCompareElements(eptr,(unsigned char*)ele,sdslen(ele)) > 0) { // 比较ele的字典序
                zl = zzlInsertAt(zl,eptr,ele,score);
                break;
            }
        }

        /* Move to next element. */
        eptr = ziplistNext(zl,sptr);
    }

    /* Push on tail of list when it was not yet inserted. */
    if (eptr == NULL)
        zl = zzlInsertAt(zl,NULL,ele,score);
    return zl;
}

unsigned char *zzlDeleteRangeByScore(unsigned char *zl, zrangespec *range, unsigned long *deleted) {
    unsigned char *eptr, *sptr;
    double score;
    unsigned long num = 0;

    if (deleted != NULL) *deleted = 0;

    eptr = zzlFirstInRange(zl,range);
    if (eptr == NULL) return zl;

    /* When the tail of the ziplist is deleted, eptr will point to the sentinel
     * byte and ziplistNext will return NULL. */
    while ((sptr = ziplistNext(zl,eptr)) != NULL) {
        score = zzlGetScore(sptr);
        if (zslValueLteMax(score,range)) {
            /* Delete both the element and the score. */
            zl = ziplistDelete(zl,&eptr);
            zl = ziplistDelete(zl,&eptr);
            num++;
        } else {
            /* No longer in range. */
            break;
        }
    }

    if (deleted != NULL) *deleted = num;
    return zl;
}

unsigned char *zzlDeleteRangeByLex(unsigned char *zl, zlexrangespec *range, unsigned long *deleted) {
    unsigned char *eptr, *sptr;
    unsigned long num = 0;

    if (deleted != NULL) *deleted = 0;

    eptr = zzlFirstInLexRange(zl,range);
    if (eptr == NULL) return zl;

    /* When the tail of the ziplist is deleted, eptr will point to the sentinel
     * byte and ziplistNext will return NULL. */
    while ((sptr = ziplistNext(zl,eptr)) != NULL) {
        if (zzlLexValueLteMax(eptr,range)) {
            /* Delete both the element and the score. */
            zl = ziplistDelete(zl,&eptr);
            zl = ziplistDelete(zl,&eptr);
            num++;
        } else {
            /* No longer in range. */
            break;
        }
    }

    if (deleted != NULL) *deleted = num;
    return zl;
}

/* Delete all the elements with rank between start and end from the skiplist.
 * Start and end are inclusive. Note that start and end need to be 1-based */
unsigned char *zzlDeleteRangeByRank(unsigned char *zl, unsigned int start, unsigned int end, unsigned long *deleted) {
    unsigned int num = (end-start)+1;
    if (deleted) *deleted = num;
    zl = ziplistDeleteRange(zl,2*(start-1),2*num);
    return zl;
}

/*-----------------------------------------------------------------------------
 * Common sorted set API
 *----------------------------------------------------------------------------*/
/**
 * 获得长度
 * @param zobj
 * @return
 */
unsigned long zsetLength(const robj *zobj) {
    unsigned long length = 0;
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) { // 编码是ziplist
        length = zzlLength(zobj->ptr); // ziplis的长度/2
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) { // 编码是
        length = ((const zset*)zobj->ptr)->zsl->length; // skiplist的长度
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return length; // 返回长度
}

void zsetConvert(robj *zobj, int encoding) {
    zset *zs;
    zskiplistNode *node, *next;
    sds ele;
    double score;

    if (zobj->encoding == encoding) return;
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        if (encoding != OBJ_ENCODING_SKIPLIST)
            serverPanic("Unknown target encoding");

        zs = zmalloc(sizeof(*zs));
        zs->dict = dictCreate(&zsetDictType,NULL);
        zs->zsl = zslCreate();

        eptr = ziplistIndex(zl,0);
        serverAssertWithInfo(NULL,zobj,eptr != NULL);
        sptr = ziplistNext(zl,eptr);
        serverAssertWithInfo(NULL,zobj,sptr != NULL);

        while (eptr != NULL) {
            score = zzlGetScore(sptr);
            serverAssertWithInfo(NULL,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong));
            if (vstr == NULL)
                ele = sdsfromlonglong(vlong);
            else
                ele = sdsnewlen((char*)vstr,vlen);

            node = zslInsert(zs->zsl,score,ele);
            serverAssert(dictAdd(zs->dict,ele,&node->score) == DICT_OK);
            zzlNext(zl,&eptr,&sptr);
        }

        zfree(zobj->ptr);
        zobj->ptr = zs;
        zobj->encoding = OBJ_ENCODING_SKIPLIST;
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        unsigned char *zl = ziplistNew();

        if (encoding != OBJ_ENCODING_ZIPLIST)
            serverPanic("Unknown target encoding");

        /* Approach similar to zslFree(), since we want to free the skiplist at
         * the same time as creating the ziplist. */
        zs = zobj->ptr;
        dictRelease(zs->dict);
        node = zs->zsl->header->level[0].forward;
        zfree(zs->zsl->header);
        zfree(zs->zsl);

        while (node) {
            zl = zzlInsertAt(zl,NULL,node->ele,node->score);
            next = node->level[0].forward;
            zslFreeNode(node);
            node = next;
        }

        zfree(zs);
        zobj->ptr = zl;
        zobj->encoding = OBJ_ENCODING_ZIPLIST;
    } else {
        serverPanic("Unknown sorted set encoding");
    }
}

/* Convert the sorted set object into a ziplist if it is not already a ziplist
 * and if the number of elements and the maximum element size and total elements size
 * are within the expected ranges. */
void zsetConvertToZiplistIfNeeded(robj *zobj, size_t maxelelen, size_t totelelen) {
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) return;
    zset *zset = zobj->ptr;

    if (zset->zsl->length <= server.zset_max_ziplist_entries &&
        maxelelen <= server.zset_max_ziplist_value &&
        ziplistSafeToAdd(NULL, totelelen))
    {
        zsetConvert(zobj,OBJ_ENCODING_ZIPLIST);
    }
}

/* Return (by reference) the score of the specified member of the sorted set
 * storing it into *score. If the element does not exist C_ERR is returned
 * otherwise C_OK is returned and *score is correctly populated.
 * If 'zobj' or 'member' is NULL, C_ERR is returned. */
/**
 *
 * @param zobj 值对象
 * @param member 元素ele
 * @param score 分值(返回)
 * @return
 */
int zsetScore(robj *zobj, sds member, double *score) {
    if (!zobj || !member) return C_ERR; // 值对象为空 或 元素为空 则返回

    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) { // 编码是ziplist
        if (zzlFind(zobj->ptr, member, score) == NULL) return C_ERR; // 找到元素的分值
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) { // 编码是skiplist
        zset *zs = zobj->ptr;
        dictEntry *de = dictFind(zs->dict, member); // 在zset的dict种查找member对应的节点
        if (de == NULL) return C_ERR;
        *score = *(double*)dictGetVal(de); // 获得节点的值
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return C_OK;
}

/* Add a new element or update the score of an existing element in a sorted
 * set, regardless of its encoding.
 *
 * The set of flags change the command behavior. They are passed with an integer
 * pointer since the function will clear the flags and populate them with
 * other flags to indicate different conditions.
 *
 * The input flags are the following:
 *
 * ZADD_INCR: Increment the current element score by 'score' instead of updating
 *            the current element score. If the element does not exist, we
 *            assume 0 as previous score.
 * ZADD_NX:   Perform the operation only if the element does not exist.
 * ZADD_XX:   Perform the operation only if the element already exist.
 *
 * When ZADD_INCR is used, the new score of the element is stored in
 * '*newscore' if 'newscore' is not NULL.
 *
 * The returned flags are the following:
 *
 * ZADD_NAN:     The resulting score is not a number.
 * ZADD_ADDED:   The element was added (not present before the call).
 * ZADD_UPDATED: The element score was updated.
 * ZADD_NOP:     No operation was performed because of NX or XX.
 *
 * Return value:
 *
 * The function returns 1 on success, and sets the appropriate flags
 * ADDED or UPDATED to signal what happened during the operation (note that
 * none could be set if we re-added an element using the same score it used
 * to have, or in the case a zero increment is used).
 *
 * The function returns 0 on erorr, currently only when the increment
 * produces a NAN condition, or when the 'score' value is NAN since the
 * start.
 *
 * The commad as a side effect of adding a new element may convert the sorted
 * set internal encoding from ziplist to hashtable+skiplist.
 *
 * Memory managemnet of 'ele':
 *
 * The function does not take ownership of the 'ele' SDS string, but copies
 * it if needed. */
/**
 * 添加或修改
 * @param zobj 值对象(ziplist或dict_skiplist)
 * @param score 分值
 * @param ele 元素
 * @param flags 标识(返回)
 * @param newscore 新的分支(返回)
 * @return
 */
int zsetAdd(robj *zobj, double score, sds ele, int *flags, double *newscore) {
    /* Turn options into simple to check vars. */
    int incr = (*flags & ZADD_INCR) != 0;
    int nx = (*flags & ZADD_NX) != 0;
    int xx = (*flags & ZADD_XX) != 0;
    *flags = 0; /* We'll return our response flags. */
    double curscore;

    /* NaN as input is an error regardless of all the other parameters. */
    if (isnan(score)) { // score空
        *flags = ZADD_NAN;
        return 0; // 返回空
    }

    /* Update the sorted set according to its encoding. */
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) { // 编码是ziplist
        unsigned char *eptr;

        if ((eptr = zzlFind(zobj->ptr,ele,&curscore)) != NULL) { // 在ziplist中查找ele 返回curscore
            /* NX? Return, same element already exists. */
            if (nx) { // 找到了 如果是新增
                *flags |= ZADD_NOP; // 不操作返回
                return 1;
            }

            /* Prepare the score for the increment if needed. */
            if (incr) { // 自增
                score += curscore; // 自增score
                if (isnan(score)) { // score是空 则返回空
                    *flags |= ZADD_NAN;
                    return 0;
                }
                if (newscore) *newscore = score; // newscore赋值
            }

            /* Remove and re-insert when score changed. */
            if (score != curscore) { // 改的分值不是当前分值
                zobj->ptr = zzlDelete(zobj->ptr,eptr); // 删除节点
                zobj->ptr = zzlInsert(zobj->ptr,ele,score); // 新增新的节点
                *flags |= ZADD_UPDATED; // 标识为修改
            }
            return 1;
        } else if (!xx) { // 没找到 新增(不是修改标识)
            /* check if the element is too large or the list
             * becomes too long *before* executing zzlInsert. */
            if (zzlLength(zobj->ptr)+1 > server.zset_max_ziplist_entries || // ziplist的长度大于128
                sdslen(ele) > server.zset_max_ziplist_value || // 元素长度大于64
                !ziplistSafeToAdd(zobj->ptr, sdslen(ele)))
            {
                zsetConvert(zobj,OBJ_ENCODING_SKIPLIST); // 转为skiplist
            } else {
                zobj->ptr = zzlInsert(zobj->ptr,ele,score); // 在ziplist中新增元素
                if (newscore) *newscore = score; // newscore赋值
                *flags |= ZADD_ADDED; // 标识为新增
                return 1;
            }
        } else { // 是xx
            *flags |= ZADD_NOP; // 不操作
            return 1;
        }
    }

    /* Note that the above block handling ziplist would have either returned or
     * converted the key to skiplist. */
    if (zobj->encoding == OBJ_ENCODING_SKIPLIST) { // 编码是skiplist
        zset *zs = zobj->ptr;
        zskiplistNode *znode;
        dictEntry *de;

        de = dictFind(zs->dict,ele); // 找ele的值对象
        if (de != NULL) { // 找到了
            /* NX? Return, same element already exists. */
            if (nx) { // 是新增
                *flags |= ZADD_NOP; // 不操作
                return 1;
            }
            curscore = *(double*)dictGetVal(de); // 获得当前分值 dict的value

            /* Prepare the score for the increment if needed. */
            if (incr) { // 是自增
                score += curscore; // 累加分值
                if (isnan(score)) {
                    *flags |= ZADD_NAN;
                    return 0;
                }
                if (newscore) *newscore = score;
            }

            /* Remove and re-insert when score changes. */
            if (score != curscore) { // 分值变化
                znode = zslUpdateScore(zs->zsl,curscore,ele,score); // skiplist修改元素的分值(先del在insert)
                /* Note that we did not removed the original element from
                 * the hash table representing the sorted set, so we just
                 * update the score. */
                dictGetVal(de) = &znode->score; /* Update score ptr. */ // 修改dict的value
                *flags |= ZADD_UPDATED;
            }
            return 1;
        } else if (!xx) { // 没招到 不是修改标识
            ele = sdsdup(ele);
            znode = zslInsert(zs->zsl,score,ele); // 插入skiplist节点
            serverAssert(dictAdd(zs->dict,ele,&znode->score) == DICT_OK); // 在dict中添加kv
            *flags |= ZADD_ADDED; // 标识为新增
            if (newscore) *newscore = score;
            return 1;
        } else { // 是xx
            *flags |= ZADD_NOP; // 不操作
            return 1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return 0; /* Never reached. */
}

/* Delete the element 'ele' from the sorted set, returning 1 if the element
 * existed and was deleted, 0 otherwise (the element was not there). */
/**
 * 删除元素
 * @param zobj zset
 * @param ele 元素
 * @return
 */
int zsetDel(robj *zobj, sds ele) {
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) { // 编码是ziplist
        unsigned char *eptr;

        if ((eptr = zzlFind(zobj->ptr,ele,NULL)) != NULL) { // 在ziplist中找元素 找到了
            zobj->ptr = zzlDelete(zobj->ptr,eptr); // 删除元素
            return 1;
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) { // 编码是skiplist
        zset *zs = zobj->ptr; // 获得zset
        dictEntry *de;
        double score;

        de = dictUnlink(zs->dict,ele); // 在dict中移除key 返回节点
        if (de != NULL) { // 节点不是空
            /* Get the score in order to delete from the skiplist later. */
            score = *(double*)dictGetVal(de); // 获得分值 value

            /* Delete from the hash table and later from the skiplist.
             * Note that the order is important: deleting from the skiplist
             * actually releases the SDS string representing the element,
             * which is shared between the skiplist and the hash table, so
             * we need to delete from the skiplist as the final step. */
            dictFreeUnlinkedEntry(zs->dict,de); // 释放节点

            /* Delete from skiplist. */
            int retval = zslDelete(zs->zsl,score,ele,NULL); // 从ziplist中删除元素
            serverAssert(retval);

            if (htNeedsResize(zs->dict)) dictResize(zs->dict); // 如果需要resize dictResize
            return 1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return 0; /* No such element found. */
}

/* Given a sorted set object returns the 0-based rank of the object or
 * -1 if the object does not exist.
 *
 * For rank we mean the position of the element in the sorted collection
 * of elements. So the first element has rank 0, the second rank 1, and so
 * forth up to length-1 elements.
 *
 * If 'reverse' is false, the rank is returned considering as first element
 * the one with the lowest score. Otherwise if 'reverse' is non-zero
 * the rank is computed considering as element with rank 0 the one with
 * the highest score. */
/**
 * 获得元素排名
 * @param zobj 值对象
 * @param ele 指定元素
 * @param reverse 是否反向
 * @return
 */
long zsetRank(robj *zobj, sds ele, int reverse) {
    unsigned long llen;
    unsigned long rank;

    llen = zsetLength(zobj); // 获得长度

    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) { // 编码是ziplist
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;

        eptr = ziplistIndex(zl,0); // 获得ziplist的第一个元素
        serverAssert(eptr != NULL); // 获得下一个元素(score)
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);

        rank = 1;
        while(eptr != NULL) {
            if (ziplistCompare(eptr,(unsigned char*)ele,sdslen(ele))) // 第一个元素等于指定元素
                break; // 跳出循环
            rank++; // 排名+1
            zzlNext(zl,&eptr,&sptr); // 下一个
        }

        if (eptr != NULL) {
            if (reverse) // 从大到小
                return llen-rank; // 长度-rank
            else // 从小到大
                return rank-1;
        } else {
            return -1;
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) { // 编码是skiplist
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        dictEntry *de;
        double score;

        de = dictFind(zs->dict,ele); // 在zset的dict中查找ele的节点
        if (de != NULL) { // 找到节点
            score = *(double*)dictGetVal(de); // 获得节点的值 score
            rank = zslGetRank(zsl,score,ele); // 根据score获得rank
            /* Existing elements always have a rank. */
            serverAssert(rank != 0);
            if (reverse) // 从大到小
                return llen-rank;
            else // 从小到大
                return rank-1;
        } else {
            return -1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
}

/*-----------------------------------------------------------------------------
 * Sorted set commands
 *----------------------------------------------------------------------------*/

/* This generic command implements both ZADD and ZINCRBY. */
/**
 * 实现添加
 * @param c
 * @param flags
 */
void zaddGenericCommand(client *c, int flags) {
    static char *nanerr = "resulting score is not a number (NaN)";
    robj *key = c->argv[1]; // 从参数中获得key
    robj *zobj;
    sds ele;
    double score = 0, *scores = NULL;
    int j, elements;
    int scoreidx = 0;
    /* The following vars are used in order to track what the command actually
     * did during the execution, to reply to the client and to trigger the
     * notification of keyspace change. */
    int added = 0;      /* Number of new elements added. */
    int updated = 0;    /* Number of elements with updated score. */
    int processed = 0;  /* Number of elements processed, may remain zero with
                           options like XX. */

    /* Parse options. At the end 'scoreidx' is set to the argument position
     * of the score of the first score-element pair. */
    scoreidx = 2; // 从第3个参数开始
    while(scoreidx < c->argc) { // 循环参数
        char *opt = c->argv[scoreidx]->ptr;
        if (!strcasecmp(opt,"nx")) flags |= ZADD_NX; // 是nx 标识为只做添加
        else if (!strcasecmp(opt,"xx")) flags |= ZADD_XX; // 是xx 标识为只做修改
        else if (!strcasecmp(opt,"ch")) flags |= ZADD_CH; // 是ch 标识为返回添加的+已更新元素数
        else if (!strcasecmp(opt,"incr")) flags |= ZADD_INCR; // 是nx 标识为指定元素增加指定分值
        else break;
        scoreidx++;
    }

    /* Turn options into simple to check vars. */
    int incr = (flags & ZADD_INCR) != 0;
    int nx = (flags & ZADD_NX) != 0;
    int xx = (flags & ZADD_XX) != 0;
    int ch = (flags & ZADD_CH) != 0;

    /* After the options, we expect to have an even number of args, since
     * we expect any number of score-element pairs. */
    elements = c->argc-scoreidx; // 剩余参数数量
    if (elements % 2 || !elements) { // 不能被2整数或者是0
        addReply(c,shared.syntaxerr); // 返货错误
        return;
    }
    elements /= 2; /* Now this holds the number of score-element pairs. */ // 参数除以2

    /* Check for incompatible options. */
    if (nx && xx) { // nx和xx并存
        addReplyError(c,
            "XX and NX options at the same time are not compatible");
        return;
    }

    if (incr && elements > 1) { // incr 和两个参数并存
        addReplyError(c,
            "INCR option supports a single increment-element pair");
        return;
    }

    /* Start parsing all the scores, we need to emit any syntax error
     * before executing additions to the sorted set, as the command should
     * either execute fully or nothing at all. */
    scores = zmalloc(sizeof(double)*elements); // 申请scores的数组空间
    for (j = 0; j < elements; j++) { // 给scores数组赋值(score参数)
        if (getDoubleFromObjectOrReply(c,c->argv[scoreidx+j*2],&scores[j],NULL)
            != C_OK) goto cleanup;
    }

    /* Lookup the key and create the sorted set if does not exist. */
    zobj = lookupKeyWrite(c->db,key); // 从db中查找key对应的值对象zset
    if (zobj == NULL) { // 值对象不存在
        if (xx) goto reply_to_client; /* No key + XX option: nothing to do. */ // 是修改 不操作
        if (server.zset_max_ziplist_entries == 0 || // 设置的最大ziplist节点是0(默认128) 或者
            server.zset_max_ziplist_value < sdslen(c->argv[scoreidx+1]->ptr)) // 第一个ele大小>64
        {
            zobj = createZsetObject(); // 创建dict+skiplist编码的zset对象
        } else {
            zobj = createZsetZiplistObject(); // 创建ziplist编码的zset对象
        }
        dbAdd(c->db,key,zobj); // 在db中添加kv
    } else { // 值对象存在
        if (zobj->type != OBJ_ZSET) { // 类型不是zset
            addReply(c,shared.wrongtypeerr);
            goto cleanup; // 退出
        }
    }

    for (j = 0; j < elements; j++) { // 添加或修改
        double newscore;
        score = scores[j]; // 获得score flag ele
        int retflags = flags;

        ele = c->argv[scoreidx+1+j*2]->ptr;
        int retval = zsetAdd(zobj, score, ele, &retflags, &newscore); // 调用zsetAdd 添加或修改
        if (retval == 0) {
            addReplyError(c,nanerr);
            goto cleanup;
        }
        if (retflags & ZADD_ADDED) added++; // 是add 则add计数+1
        if (retflags & ZADD_UPDATED) updated++; // 是updated 则updated计数+1
        if (!(retflags & ZADD_NOP)) processed++; // 不是 则processed计数+1
        score = newscore; // 修改的分值覆盖传入的分值
    }
    server.dirty += (added+updated); // 修改数据计数累加新增+修改

reply_to_client:
    if (incr) { /* ZINCRBY or INCR option. */
        if (processed)
            addReplyDouble(c,score);
        else
            addReply(c,shared.nullbulk);
    } else { /* ZADD. */
        addReplyLongLong(c,ch ? added+updated : added); // 有ch 则回added+updated计数 否则会added计数
    }

cleanup:
    zfree(scores);
    if (added || updated) {
        signalModifiedKey(c->db,key);
        notifyKeyspaceEvent(NOTIFY_ZSET,
            incr ? "zincr" : "zadd", key, c->db->id);
    }
}
/**
 * zadd key [nx|xx] [ch] [incr] s1 m1 s2 m2 ...
 * @param c
 */
void zaddCommand(client *c) {
    zaddGenericCommand(c,ZADD_NONE);
}

void zincrbyCommand(client *c) {
    zaddGenericCommand(c,ZADD_INCR);
}
/**
 * zrem key member1 member2 ...
 * @param c
 */
void zremCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    int deleted = 0, keyremoved = 0, j;

    if ((zobj = lookupKeyWriteOrReply(c,key,shared.czero)) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) return; // 从db中获得key对应的值对象 如果是空或者不是空但类型不是zset 则返回

    for (j = 2; j < c->argc; j++) { // 参数循环 member1 member2 ...
        if (zsetDel(zobj,c->argv[j]->ptr)) deleted++; // 删除成功 删除标识+1
        if (zsetLength(zobj) == 0) { // zset为空
            dbDelete(c->db,key); // 删除db的key
            keyremoved = 1;
            break;
        }
    }

    if (deleted) { // 删除成功
        notifyKeyspaceEvent(NOTIFY_ZSET,"zrem",key,c->db->id); // 发送键空间通知 zrem
        if (keyremoved) // 删除db的kv
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",key,c->db->id); // 发送键空间通知 del
        signalModifiedKey(c->db,key);
        server.dirty += deleted;
    }
    addReplyLongLong(c,deleted); // 响应删除元素数
}

/* Implements ZREMRANGEBYRANK, ZREMRANGEBYSCORE, ZREMRANGEBYLEX commands. */
#define ZRANGE_RANK 0
#define ZRANGE_SCORE 1
#define ZRANGE_LEX 2
/**
 *
 * @param c
 * @param rangetype 0:rank 1:socre 2:lex
 */
void zremrangeGenericCommand(client *c, int rangetype) {
    robj *key = c->argv[1]; // 获得参数key
    robj *zobj;
    int keyremoved = 0;
    unsigned long deleted = 0;
    zrangespec range;
    zlexrangespec lexrange;
    long start, end, llen;

    /* Step 1: Parse the range. */
    if (rangetype == ZRANGE_RANK) { // type是rank
        if ((getLongFromObjectOrReply(c,c->argv[2],&start,NULL) != C_OK) ||
            (getLongFromObjectOrReply(c,c->argv[3],&end,NULL) != C_OK)) // 获得start和end参数 转成long
            return;
    } else if (rangetype == ZRANGE_SCORE) { // type是score
        if (zslParseRange(c->argv[2],c->argv[3],&range) != C_OK) { // 获得min分值和max分值 存入rang结构
            addReplyError(c,"min or max is not a float");
            return;
        }
    } else if (rangetype == ZRANGE_LEX) { // type是lex
        if (zslParseLexRange(c->argv[2],c->argv[3],&lexrange) != C_OK) { // 获得min字母和max字母 存入rang结构
            addReplyError(c,"min or max not valid string range item");
            return;
        }
    }

    /* Step 2: Lookup & range sanity checks if needed. */
    if ((zobj = lookupKeyWriteOrReply(c,key,shared.czero)) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) goto cleanup; // 在db中获得key对应的值对象 不存在 响应0 或者 类型不是zset 返回

    if (rangetype == ZRANGE_RANK) { // type是rank
        /* Sanitize indexes. */
        llen = zsetLength(zobj); // 获得长度
        // 负索引变正索引
        if (start < 0) start = llen+start;
        if (end < 0) end = llen+end;
        if (start < 0) start = 0; // 第一个

        /* Invariant: start >= 0, so this test will be true when end < 0.
         * The range is empty when start > end or start >= length. */
        if (start > end || start >= llen) { // 起始大于截止 起始大于长度
            addReply(c,shared.czero); // 响应0
            goto cleanup;
        }
        if (end >= llen) end = llen-1; // 截止大于长度 是最后一个
    }

    /* Step 3: Perform the range deletion operation. */
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) { // 编码是ziplist
        switch(rangetype) {
        case ZRANGE_RANK:
            zobj->ptr = zzlDeleteRangeByRank(zobj->ptr,start+1,end+1,&deleted); // ziplist的范围索引删除
            break;
        case ZRANGE_SCORE:
            zobj->ptr = zzlDeleteRangeByScore(zobj->ptr,&range,&deleted); // ziplist的范围分值删除
            break;
        case ZRANGE_LEX:
            zobj->ptr = zzlDeleteRangeByLex(zobj->ptr,&lexrange,&deleted); // ziplist的范围字母删除
            break;
        }
        if (zzlLength(zobj->ptr) == 0) { // ziplist长度0
            dbDelete(c->db,key); // 在db中删除key
            keyremoved = 1;
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) { // 编码是skiplist
        zset *zs = zobj->ptr;
        switch(rangetype) {
        case ZRANGE_RANK:
            deleted = zslDeleteRangeByRank(zs->zsl,start+1,end+1,zs->dict); // 在sl中索引范围删除,删除zset的dict的元素和分值
            break;
        case ZRANGE_SCORE:
            deleted = zslDeleteRangeByScore(zs->zsl,&range,zs->dict); // 在sl中分值范围删除,删除zset的dict的元素和分值
            break;
        case ZRANGE_LEX:
            deleted = zslDeleteRangeByLex(zs->zsl,&lexrange,zs->dict); // 在sl中字母范围删除,删除zset的dict的元素和分值
            break;
        }
        if (htNeedsResize(zs->dict)) dictResize(zs->dict); // dict需要resize dictResize
        if (dictSize(zs->dict) == 0) { // zset的dict大小为0
            dbDelete(c->db,key); // 在db中删除对应的kye
            keyremoved = 1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    /* Step 4: Notifications and reply. */
    if (deleted) { // 删除成功
        char *event[3] = {"zremrangebyrank","zremrangebyscore","zremrangebylex"}; // 范围删除事件
        signalModifiedKey(c->db,key);
        notifyKeyspaceEvent(NOTIFY_ZSET,event[rangetype],key,c->db->id); // 通知范围删除事件
        if (keyremoved) // key删除
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",key,c->db->id); // 通知del
    }
    server.dirty += deleted;
    addReplyLongLong(c,deleted); // 响应删除条数

cleanup:
    if (rangetype == ZRANGE_LEX) zslFreeLexRange(&lexrange);
}
/**
 * zremrangebyrank key min max
 * @param c
 */
void zremrangebyrankCommand(client *c) {
    zremrangeGenericCommand(c,ZRANGE_RANK);
}
/**
 * zremrangebyscore key min max
 * @param c
 */
void zremrangebyscoreCommand(client *c) {
    zremrangeGenericCommand(c,ZRANGE_SCORE);
}
/**
 * zremrangebylex key min max
 * @param c
 */
void zremrangebylexCommand(client *c) {
    zremrangeGenericCommand(c,ZRANGE_LEX);
}

typedef struct {
    robj *subject;
    int type; /* Set, sorted set */
    int encoding;
    double weight;

    union {
        /* Set iterators. */
        union _iterset {
            struct {
                intset *is;
                int ii;
            } is;
            struct {
                dict *dict;
                dictIterator *di;
                dictEntry *de;
            } ht;
        } set;

        /* Sorted set iterators. */
        union _iterzset {
            struct {
                unsigned char *zl;
                unsigned char *eptr, *sptr;
            } zl;
            struct {
                zset *zs;
                zskiplistNode *node;
            } sl;
        } zset;
    } iter;
} zsetopsrc;


/* Use dirty flags for pointers that need to be cleaned up in the next
 * iteration over the zsetopval. The dirty flag for the long long value is
 * special, since long long values don't need cleanup. Instead, it means that
 * we already checked that "ell" holds a long long, or tried to convert another
 * representation into a long long value. When this was successful,
 * OPVAL_VALID_LL is set as well. */
#define OPVAL_DIRTY_SDS 1
#define OPVAL_DIRTY_LL 2
#define OPVAL_VALID_LL 4

/* Store value retrieved from the iterator. */
typedef struct {
    int flags;
    unsigned char _buf[32]; /* Private buffer. */
    sds ele;
    unsigned char *estr;
    unsigned int elen;
    long long ell;
    double score;
} zsetopval;

typedef union _iterset iterset;
typedef union _iterzset iterzset;

void zuiInitIterator(zsetopsrc *op) {
    if (op->subject == NULL)
        return;

    if (op->type == OBJ_SET) {
        iterset *it = &op->iter.set;
        if (op->encoding == OBJ_ENCODING_INTSET) {
            it->is.is = op->subject->ptr;
            it->is.ii = 0;
        } else if (op->encoding == OBJ_ENCODING_HT) {
            it->ht.dict = op->subject->ptr;
            it->ht.di = dictGetIterator(op->subject->ptr);
            it->ht.de = dictNext(it->ht.di);
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (op->type == OBJ_ZSET) {
        iterzset *it = &op->iter.zset;
        if (op->encoding == OBJ_ENCODING_ZIPLIST) {
            it->zl.zl = op->subject->ptr;
            it->zl.eptr = ziplistIndex(it->zl.zl,0);
            if (it->zl.eptr != NULL) {
                it->zl.sptr = ziplistNext(it->zl.zl,it->zl.eptr);
                serverAssert(it->zl.sptr != NULL);
            }
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            it->sl.zs = op->subject->ptr;
            it->sl.node = it->sl.zs->zsl->header->level[0].forward;
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

void zuiClearIterator(zsetopsrc *op) {
    if (op->subject == NULL)
        return;

    if (op->type == OBJ_SET) {
        iterset *it = &op->iter.set;
        if (op->encoding == OBJ_ENCODING_INTSET) {
            UNUSED(it); /* skip */
        } else if (op->encoding == OBJ_ENCODING_HT) {
            dictReleaseIterator(it->ht.di);
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (op->type == OBJ_ZSET) {
        iterzset *it = &op->iter.zset;
        if (op->encoding == OBJ_ENCODING_ZIPLIST) {
            UNUSED(it); /* skip */
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            UNUSED(it); /* skip */
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

unsigned long zuiLength(zsetopsrc *op) {
    if (op->subject == NULL)
        return 0;

    if (op->type == OBJ_SET) {
        if (op->encoding == OBJ_ENCODING_INTSET) {
            return intsetLen(op->subject->ptr);
        } else if (op->encoding == OBJ_ENCODING_HT) {
            dict *ht = op->subject->ptr;
            return dictSize(ht);
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (op->type == OBJ_ZSET) {
        if (op->encoding == OBJ_ENCODING_ZIPLIST) {
            return zzlLength(op->subject->ptr);
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = op->subject->ptr;
            return zs->zsl->length;
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

/* Check if the current value is valid. If so, store it in the passed structure
 * and move to the next element. If not valid, this means we have reached the
 * end of the structure and can abort. */
int zuiNext(zsetopsrc *op, zsetopval *val) {
    if (op->subject == NULL)
        return 0;

    if (val->flags & OPVAL_DIRTY_SDS)
        sdsfree(val->ele);

    memset(val,0,sizeof(zsetopval));

    if (op->type == OBJ_SET) {
        iterset *it = &op->iter.set;
        if (op->encoding == OBJ_ENCODING_INTSET) {
            int64_t ell;

            if (!intsetGet(it->is.is,it->is.ii,&ell))
                return 0;
            val->ell = ell;
            val->score = 1.0;

            /* Move to next element. */
            it->is.ii++;
        } else if (op->encoding == OBJ_ENCODING_HT) {
            if (it->ht.de == NULL)
                return 0;
            val->ele = dictGetKey(it->ht.de);
            val->score = 1.0;

            /* Move to next element. */
            it->ht.de = dictNext(it->ht.di);
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (op->type == OBJ_ZSET) {
        iterzset *it = &op->iter.zset;
        if (op->encoding == OBJ_ENCODING_ZIPLIST) {
            /* No need to check both, but better be explicit. */
            if (it->zl.eptr == NULL || it->zl.sptr == NULL)
                return 0;
            serverAssert(ziplistGet(it->zl.eptr,&val->estr,&val->elen,&val->ell));
            val->score = zzlGetScore(it->zl.sptr);

            /* Move to next element. */
            zzlNext(it->zl.zl,&it->zl.eptr,&it->zl.sptr);
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            if (it->sl.node == NULL)
                return 0;
            val->ele = it->sl.node->ele;
            val->score = it->sl.node->score;

            /* Move to next element. */
            it->sl.node = it->sl.node->level[0].forward;
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
    return 1;
}

int zuiLongLongFromValue(zsetopval *val) {
    if (!(val->flags & OPVAL_DIRTY_LL)) {
        val->flags |= OPVAL_DIRTY_LL;

        if (val->ele != NULL) {
            if (string2ll(val->ele,sdslen(val->ele),&val->ell))
                val->flags |= OPVAL_VALID_LL;
        } else if (val->estr != NULL) {
            if (string2ll((char*)val->estr,val->elen,&val->ell))
                val->flags |= OPVAL_VALID_LL;
        } else {
            /* The long long was already set, flag as valid. */
            val->flags |= OPVAL_VALID_LL;
        }
    }
    return val->flags & OPVAL_VALID_LL;
}

sds zuiSdsFromValue(zsetopval *val) {
    if (val->ele == NULL) {
        if (val->estr != NULL) {
            val->ele = sdsnewlen((char*)val->estr,val->elen);
        } else {
            val->ele = sdsfromlonglong(val->ell);
        }
        val->flags |= OPVAL_DIRTY_SDS;
    }
    return val->ele;
}

/* This is different from zuiSdsFromValue since returns a new SDS string
 * which is up to the caller to free. */
sds zuiNewSdsFromValue(zsetopval *val) {
    if (val->flags & OPVAL_DIRTY_SDS) {
        /* We have already one to return! */
        sds ele = val->ele;
        val->flags &= ~OPVAL_DIRTY_SDS;
        val->ele = NULL;
        return ele;
    } else if (val->ele) {
        return sdsdup(val->ele);
    } else if (val->estr) {
        return sdsnewlen((char*)val->estr,val->elen);
    } else {
        return sdsfromlonglong(val->ell);
    }
}

int zuiBufferFromValue(zsetopval *val) {
    if (val->estr == NULL) {
        if (val->ele != NULL) {
            val->elen = sdslen(val->ele);
            val->estr = (unsigned char*)val->ele;
        } else {
            val->elen = ll2string((char*)val->_buf,sizeof(val->_buf),val->ell);
            val->estr = val->_buf;
        }
    }
    return 1;
}

/* Find value pointed to by val in the source pointer to by op. When found,
 * return 1 and store its score in target. Return 0 otherwise. */
int zuiFind(zsetopsrc *op, zsetopval *val, double *score) {
    if (op->subject == NULL)
        return 0;

    if (op->type == OBJ_SET) {
        if (op->encoding == OBJ_ENCODING_INTSET) {
            if (zuiLongLongFromValue(val) &&
                intsetFind(op->subject->ptr,val->ell))
            {
                *score = 1.0;
                return 1;
            } else {
                return 0;
            }
        } else if (op->encoding == OBJ_ENCODING_HT) {
            dict *ht = op->subject->ptr;
            zuiSdsFromValue(val);
            if (dictFind(ht,val->ele) != NULL) {
                *score = 1.0;
                return 1;
            } else {
                return 0;
            }
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (op->type == OBJ_ZSET) {
        zuiSdsFromValue(val);

        if (op->encoding == OBJ_ENCODING_ZIPLIST) {
            if (zzlFind(op->subject->ptr,val->ele,score) != NULL) {
                /* Score is already set by zzlFind. */
                return 1;
            } else {
                return 0;
            }
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = op->subject->ptr;
            dictEntry *de;
            if ((de = dictFind(zs->dict,val->ele)) != NULL) {
                *score = *(double*)dictGetVal(de);
                return 1;
            } else {
                return 0;
            }
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

int zuiCompareByCardinality(const void *s1, const void *s2) {
    unsigned long first = zuiLength((zsetopsrc*)s1);
    unsigned long second = zuiLength((zsetopsrc*)s2);
    if (first > second) return 1;
    if (first < second) return -1;
    return 0;
}

#define REDIS_AGGR_SUM 1
#define REDIS_AGGR_MIN 2
#define REDIS_AGGR_MAX 3
#define zunionInterDictValue(_e) (dictGetVal(_e) == NULL ? 1.0 : *(double*)dictGetVal(_e))

inline static void zunionInterAggregate(double *target, double val, int aggregate) {
    if (aggregate == REDIS_AGGR_SUM) {
        *target = *target + val;
        /* The result of adding two doubles is NaN when one variable
         * is +inf and the other is -inf. When these numbers are added,
         * we maintain the convention of the result being 0.0. */
        if (isnan(*target)) *target = 0.0;
    } else if (aggregate == REDIS_AGGR_MIN) {
        *target = val < *target ? val : *target;
    } else if (aggregate == REDIS_AGGR_MAX) {
        *target = val > *target ? val : *target;
    } else {
        /* safety net */
        serverPanic("Unknown ZUNION/INTER aggregate type");
    }
}

uint64_t dictSdsHash(const void *key);
int dictSdsKeyCompare(void *privdata, const void *key1, const void *key2);

dictType setAccumulatorDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    NULL,                      /* key destructor */
    NULL                       /* val destructor */
};

void zunionInterGenericCommand(client *c, robj *dstkey, int op) {
    int i, j;
    long setnum;
    int aggregate = REDIS_AGGR_SUM;
    zsetopsrc *src;
    zsetopval zval;
    sds tmp;
    size_t maxelelen = 0, totelelen = 0;
    robj *dstobj;
    zset *dstzset;
    zskiplistNode *znode;
    int touched = 0;

    /* expect setnum input keys to be given */
    if ((getLongFromObjectOrReply(c, c->argv[2], &setnum, NULL) != C_OK))
        return;

    if (setnum < 1) {
        addReplyError(c,
            "at least 1 input key is needed for ZUNIONSTORE/ZINTERSTORE");
        return;
    }

    /* test if the expected number of keys would overflow */
    if (setnum > c->argc-3) {
        addReply(c,shared.syntaxerr);
        return;
    }

    /* read keys to be used for input */
    src = zcalloc(sizeof(zsetopsrc) * setnum);
    for (i = 0, j = 3; i < setnum; i++, j++) {
        robj *obj = lookupKeyWrite(c->db,c->argv[j]);
        if (obj != NULL) {
            if (obj->type != OBJ_ZSET && obj->type != OBJ_SET) {
                zfree(src);
                addReply(c,shared.wrongtypeerr);
                return;
            }

            src[i].subject = obj;
            src[i].type = obj->type;
            src[i].encoding = obj->encoding;
        } else {
            src[i].subject = NULL;
        }

        /* Default all weights to 1. */
        src[i].weight = 1.0;
    }

    /* parse optional extra arguments */
    if (j < c->argc) {
        int remaining = c->argc - j;

        while (remaining) {
            if (remaining >= (setnum + 1) &&
                !strcasecmp(c->argv[j]->ptr,"weights"))
            {
                j++; remaining--;
                for (i = 0; i < setnum; i++, j++, remaining--) {
                    if (getDoubleFromObjectOrReply(c,c->argv[j],&src[i].weight,
                            "weight value is not a float") != C_OK)
                    {
                        zfree(src);
                        return;
                    }
                }
            } else if (remaining >= 2 &&
                       !strcasecmp(c->argv[j]->ptr,"aggregate"))
            {
                j++; remaining--;
                if (!strcasecmp(c->argv[j]->ptr,"sum")) {
                    aggregate = REDIS_AGGR_SUM;
                } else if (!strcasecmp(c->argv[j]->ptr,"min")) {
                    aggregate = REDIS_AGGR_MIN;
                } else if (!strcasecmp(c->argv[j]->ptr,"max")) {
                    aggregate = REDIS_AGGR_MAX;
                } else {
                    zfree(src);
                    addReply(c,shared.syntaxerr);
                    return;
                }
                j++; remaining--;
            } else {
                zfree(src);
                addReply(c,shared.syntaxerr);
                return;
            }
        }
    }

    /* sort sets from the smallest to largest, this will improve our
     * algorithm's performance */
    qsort(src,setnum,sizeof(zsetopsrc),zuiCompareByCardinality);

    dstobj = createZsetObject();
    dstzset = dstobj->ptr;
    memset(&zval, 0, sizeof(zval));

    if (op == SET_OP_INTER) {
        /* Skip everything if the smallest input is empty. */
        if (zuiLength(&src[0]) > 0) {
            /* Precondition: as src[0] is non-empty and the inputs are ordered
             * by size, all src[i > 0] are non-empty too. */
            zuiInitIterator(&src[0]);
            while (zuiNext(&src[0],&zval)) {
                double score, value;

                score = src[0].weight * zval.score;
                if (isnan(score)) score = 0;

                for (j = 1; j < setnum; j++) {
                    /* It is not safe to access the zset we are
                     * iterating, so explicitly check for equal object. */
                    if (src[j].subject == src[0].subject) {
                        value = zval.score*src[j].weight;
                        zunionInterAggregate(&score,value,aggregate);
                    } else if (zuiFind(&src[j],&zval,&value)) {
                        value *= src[j].weight;
                        zunionInterAggregate(&score,value,aggregate);
                    } else {
                        break;
                    }
                }

                /* Only continue when present in every input. */
                if (j == setnum) {
                    tmp = zuiNewSdsFromValue(&zval);
                    znode = zslInsert(dstzset->zsl,score,tmp);
                    dictAdd(dstzset->dict,tmp,&znode->score);
                    totelelen += sdslen(tmp);
                    if (sdslen(tmp) > maxelelen) maxelelen = sdslen(tmp);
                }
            }
            zuiClearIterator(&src[0]);
        }
    } else if (op == SET_OP_UNION) {
        dict *accumulator = dictCreate(&setAccumulatorDictType,NULL);
        dictIterator *di;
        dictEntry *de, *existing;
        double score;

        if (setnum) {
            /* Our union is at least as large as the largest set.
             * Resize the dictionary ASAP to avoid useless rehashing. */
            dictExpand(accumulator,zuiLength(&src[setnum-1]));
        }

        /* Step 1: Create a dictionary of elements -> aggregated-scores
         * by iterating one sorted set after the other. */
        for (i = 0; i < setnum; i++) {
            if (zuiLength(&src[i]) == 0) continue;

            zuiInitIterator(&src[i]);
            while (zuiNext(&src[i],&zval)) {
                /* Initialize value */
                score = src[i].weight * zval.score;
                if (isnan(score)) score = 0;

                /* Search for this element in the accumulating dictionary. */
                de = dictAddRaw(accumulator,zuiSdsFromValue(&zval),&existing);
                /* If we don't have it, we need to create a new entry. */
                if (!existing) {
                    tmp = zuiNewSdsFromValue(&zval);
                    /* Remember the longest single element encountered,
                     * to understand if it's possible to convert to ziplist
                     * at the end. */
                     totelelen += sdslen(tmp);
                     if (sdslen(tmp) > maxelelen) maxelelen = sdslen(tmp);
                    /* Update the element with its initial score. */
                    dictSetKey(accumulator, de, tmp);
                    dictSetDoubleVal(de,score);
                } else {
                    /* Update the score with the score of the new instance
                     * of the element found in the current sorted set.
                     *
                     * Here we access directly the dictEntry double
                     * value inside the union as it is a big speedup
                     * compared to using the getDouble/setDouble API. */
                    zunionInterAggregate(&existing->v.d,score,aggregate);
                }
            }
            zuiClearIterator(&src[i]);
        }

        /* Step 2: convert the dictionary into the final sorted set. */
        di = dictGetIterator(accumulator);

        /* We now are aware of the final size of the resulting sorted set,
         * let's resize the dictionary embedded inside the sorted set to the
         * right size, in order to save rehashing time. */
        dictExpand(dstzset->dict,dictSize(accumulator));

        while((de = dictNext(di)) != NULL) {
            sds ele = dictGetKey(de);
            score = dictGetDoubleVal(de);
            znode = zslInsert(dstzset->zsl,score,ele);
            dictAdd(dstzset->dict,ele,&znode->score);
        }
        dictReleaseIterator(di);
        dictRelease(accumulator);
    } else {
        serverPanic("Unknown operator");
    }

    if (dbDelete(c->db,dstkey))
        touched = 1;
    if (dstzset->zsl->length) {
        zsetConvertToZiplistIfNeeded(dstobj,maxelelen,totelelen);
        dbAdd(c->db,dstkey,dstobj);
        addReplyLongLong(c,zsetLength(dstobj));
        signalModifiedKey(c->db,dstkey);
        notifyKeyspaceEvent(NOTIFY_ZSET,
            (op == SET_OP_UNION) ? "zunionstore" : "zinterstore",
            dstkey,c->db->id);
        server.dirty++;
    } else {
        decrRefCount(dstobj);
        addReply(c,shared.czero);
        if (touched) {
            signalModifiedKey(c->db,dstkey);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",dstkey,c->db->id);
            server.dirty++;
        }
    }
    zfree(src);
}

void zunionstoreCommand(client *c) {
    zunionInterGenericCommand(c,c->argv[1], SET_OP_UNION);
}

void zinterstoreCommand(client *c) {
    zunionInterGenericCommand(c,c->argv[1], SET_OP_INTER);
}
/**
 *
 * @param c
 * @param reverse 1:从大到小
 */
void zrangeGenericCommand(client *c, int reverse) {
    robj *key = c->argv[1]; // 获得参数key
    robj *zobj;
    int withscores = 0;
    long start;
    long end;
    long llen;
    long rangelen;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)) return; // 获得start和end 并 转long

    if (c->argc == 5 && !strcasecmp(c->argv[4]->ptr,"withscores")) { // 参数是5 并且 withscores
        withscores = 1;
    } else if (c->argc >= 5) { // 没有withscores
        addReply(c,shared.syntaxerr);
        return;
    }

    if ((zobj = lookupKeyReadOrReply(c,key,shared.emptymultibulk)) == NULL
         || checkType(c,zobj,OBJ_ZSET)) return; // 在db中根据key 找到对应的值对象 是空 相应空 或者 类型不是zset 返回

    /* Sanitize indexes. */
    llen = zsetLength(zobj); // 获得值对象的长度
    // 负索引转正索引
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) { // 起始大于截止 或者 起始大于长度
        addReply(c,shared.emptymultibulk); // 响应空
        return;
    }
    if (end >= llen) end = llen-1; // 截止大于长度 截止等于最后一个节点
    rangelen = (end-start)+1; // 范围长度=截止-起始+1

    /* Return the result in form of a multi-bulk reply */
    addReplyMultiBulkLen(c, withscores ? (rangelen*2) : rangelen); // 响应长度 如果有withscores 则 为范围长度的2倍 否则 是范围长度

    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) { // 编码是ziplist
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        if (reverse) // 倒序
            eptr = ziplistIndex(zl,-2-(2*start)); // 获得从后起始元素
        else
            eptr = ziplistIndex(zl,2*start); // 获得从前起始元素

        serverAssertWithInfo(c,zobj,eptr != NULL);
        sptr = ziplistNext(zl,eptr); // 获取元素分值

        while (rangelen--) { // 循环范围
            serverAssertWithInfo(c,zobj,eptr != NULL && sptr != NULL);
            serverAssertWithInfo(c,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong)); // 获得元素数据 字符串:vstr 整数:vlong
            if (vstr == NULL)
                addReplyBulkLongLong(c,vlong); // 不是字符串则响应整数
            else
                addReplyBulkCBuffer(c,vstr,vlen); // 是字符串则响应字符串

            if (withscores) // 有分值
                addReplyDouble(c,zzlGetScore(sptr)); // 响应分值

            if (reverse) // 倒序
                zzlPrev(zl,&eptr,&sptr); // 找上一个
            else
                zzlNext(zl,&eptr,&sptr); // 找下一个
        }

    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) { // 编码是skiplist
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;
        sds ele;

        /* Check if starting point is trivial, before doing log(N) lookup. */
        if (reverse) { // 倒序
            ln = zsl->tail; // 获得尾节点
            if (start > 0)
                ln = zslGetElementByRank(zsl,llen-start); // 根据rank 获得节点
        } else {
            ln = zsl->header->level[0].forward; // 获得头的底层的下一个
            if (start > 0)
                ln = zslGetElementByRank(zsl,start+1); // 根据rank 获得节点
        }

        while(rangelen--) { // 循环范围
            serverAssertWithInfo(c,zobj,ln != NULL);
            ele = ln->ele; // 获得元素
            addReplyBulkCBuffer(c,ele,sdslen(ele)); // 响应元素
            if (withscores) // 有分值
                addReplyDouble(c,ln->score); // 响应分值
            ln = reverse ? ln->backward : ln->level[0].forward; // 倒序 找节点的上一个 否则 找节点的下一个
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
}
/**
 * zrang key start end [withscores]
 * @param c
 */
void zrangeCommand(client *c) {
    zrangeGenericCommand(c,0);
}

void zrevrangeCommand(client *c) {
    zrangeGenericCommand(c,1);
}

/* This command implements ZRANGEBYSCORE, ZREVRANGEBYSCORE. */
/**
 *
 * @param c 1:倒序 从大到小
 * @param reverse
 */
void genericZrangebyscoreCommand(client *c, int reverse) {
    zrangespec range;
    robj *key = c->argv[1]; // 获得参数key
    robj *zobj;
    long offset = 0, limit = -1; // limit:count
    int withscores = 0;
    unsigned long rangelen = 0;
    void *replylen = NULL;
    int minidx, maxidx;

    /* Parse the range arguments. */
    if (reverse) { // 倒序
        /* Range is given as [max,min] */
        maxidx = 2; minidx = 3; // 第2个参数是max,第3个参数是min
    } else {
        /* Range is given as [min,max] */
        minidx = 2; maxidx = 3; // 第2个参数是min,第3个参数是max
    }

    if (zslParseRange(c->argv[minidx],c->argv[maxidx],&range) != C_OK) { // 解析参数到rang结构体中
        addReplyError(c,"min or max is not a float");
        return;
    }

    /* Parse optional extra arguments. Note that ZCOUNT will exactly have
     * 4 arguments, so we'll never enter the following code path. */
    if (c->argc > 4) {  // 参数个数>4
        int remaining = c->argc - 4;
        int pos = 4;

        while (remaining) { // 有附加参数
            if (remaining >= 1 && !strcasecmp(c->argv[pos]->ptr,"withscores")) { // 第4个参数是withscores
                pos++; remaining--;
                withscores = 1; // 响应score
            } else if (remaining >= 3 && !strcasecmp(c->argv[pos]->ptr,"limit")) { // 第5个参数是limit
                if ((getLongFromObjectOrReply(c, c->argv[pos+1], &offset, NULL) // 第6个参数是offset转long
                        != C_OK) ||
                    (getLongFromObjectOrReply(c, c->argv[pos+2], &limit, NULL) // 第7个参数是count转long
                        != C_OK))
                {
                    return;
                }
                pos += 3; remaining -= 3;
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }
    }

    /* Ok, lookup the key and get the range */
    if ((zobj = lookupKeyReadOrReply(c,key,shared.emptymultibulk)) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) return; // zaidb中查找key对应的值对象 是空 返回空 或者 不是zset 返回

    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) { // 编码是ziplist
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        double score;

        /* If reversed, get the last node in range as starting point. */
        if (reverse) { // 倒序
            eptr = zzlLastInRange(zl,&range); // 取范围内的最后一个元素
        } else {
            eptr = zzlFirstInRange(zl,&range); // 取范围内的第一个元素
        }

        /* No "first" element in the specified interval. */
        if (eptr == NULL) { // 元素是空
            addReply(c, shared.emptymultibulk); // 响应空
            return;
        }

        /* Get score pointer for the first element. */
        serverAssertWithInfo(c,zobj,eptr != NULL);
        sptr = ziplistNext(zl,eptr); // 取元素的值

        /* We don't know in advance how many matching elements there are in the
         * list, so we push this object that will represent the multi-bulk
         * length in the output buffer, and will "fix" it later */
        replylen = addDeferredMultiBulkLength(c);

        /* If there is an offset, just traverse the number of elements without
         * checking the score because that is done in the next loop. */
        while (eptr && offset--) { // 循环zipilist 并且 有偏移量
            if (reverse) { // 倒序
                zzlPrev(zl,&eptr,&sptr); // 取上一个赋值元素和分值
            } else { // 正序
                zzlNext(zl,&eptr,&sptr); // 取下一个赋值元素和分值
            }
        }

        while (eptr && limit--) { // 循环ziplist 并且 取count个
            score = zzlGetScore(sptr); // 通过分值对象获得分值

            /* Abort when the node is no longer in range. */
            if (reverse) {
                if (!zslValueGteMin(score,&range)) break; // socre比范围内最小的分值还小
            } else {
                if (!zslValueLteMax(score,&range)) break; // socre比范围内最大的分值还大
            }

            /* We know the element exists, so ziplistGet should always succeed */
            serverAssertWithInfo(c,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong)); // 获取元素数据

            rangelen++;
            if (vstr == NULL) {
                addReplyBulkLongLong(c,vlong); // 不是sds 响应整型
            } else {
                addReplyBulkCBuffer(c,vstr,vlen); // 是sds 响应字符串和长度
            }

            if (withscores) { // 要响应分值
                addReplyDouble(c,score); // 响应分值
            }

            /* Move to next node */
            if (reverse) { // 倒序
                zzlPrev(zl,&eptr,&sptr); // 取上一个
            } else {
                zzlNext(zl,&eptr,&sptr); // 取下一个
            }
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) { // 编码是skiplist
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl; // 获得zset的skiplist
        zskiplistNode *ln; // 节点

        /* If reversed, get the last node in range as starting point. */
        if (reverse) { // 倒序
            ln = zslLastInRange(zsl,&range); // 获得范围内的最后一个节点
        } else {
            ln = zslFirstInRange(zsl,&range); // 获得范围内的第一个节点
        }

        /* No "first" element in the specified interval. */
        if (ln == NULL) { // 几点是空
            addReply(c, shared.emptymultibulk); // 响应空
            return;
        }

        /* We don't know in advance how many matching elements there are in the
         * list, so we push this object that will represent the multi-bulk
         * length in the output buffer, and will "fix" it later */
        replylen = addDeferredMultiBulkLength(c);

        /* If there is an offset, just traverse the number of elements without
         * checking the score because that is done in the next loop. */
        while (ln && offset--) { // 循环 到偏移量
            if (reverse) { // 倒序
                ln = ln->backward; // 前一个
            } else {
                ln = ln->level[0].forward; // 底层的后一个节点
            }
        }

        while (ln && limit--) { // 取count
            /* Abort when the node is no longer in range. */
            if (reverse) {
                if (!zslValueGteMin(ln->score,&range)) break; // socre比范围内最小的分值还小
            } else {
                if (!zslValueLteMax(ln->score,&range)) break; // socre比范围内最大的分值还大
            }

            rangelen++;
            addReplyBulkCBuffer(c,ln->ele,sdslen(ln->ele)); // 响应元素和长度

            if (withscores) { // 要响应分值
                addReplyDouble(c,ln->score); // 响应分值
            }

            /* Move to next node */
            if (reverse) { // 倒序
                ln = ln->backward; // 取前一个
            } else {
                ln = ln->level[0].forward; // 取底层的后一个
            }
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    if (withscores) { // 要响应分值
        rangelen *= 2; // 范围长度*2
    }

    setDeferredMultiBulkLength(c, replylen, rangelen);
}
/**
 * zrangebyscore key min max [withscores] [limit offset count]
 * @param c
 */
void zrangebyscoreCommand(client *c) {
    genericZrangebyscoreCommand(c,0);
}
/**
 * zrevrangebyscore key min max [withscores] [limit offset count]
 * @param c
 */
void zrevrangebyscoreCommand(client *c) {
    genericZrangebyscoreCommand(c,1);
}
/**
 * zcount key min max
 * @param c
 */
void zcountCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    zrangespec range; // range结构体
    unsigned long count = 0;

    /* Parse the range arguments */
    if (zslParseRange(c->argv[2],c->argv[3],&range) != C_OK) { // 解析min max 以及是否包含 写入 range结构体中
        addReplyError(c,"min or max is not a float");
        return;
    }

    /* Lookup the sorted set */
    if ((zobj = lookupKeyReadOrReply(c, key, shared.czero)) == NULL || // 在db中查找key对应的值对象 找不到或找到了 类型不是zset 则返回
        checkType(c, zobj, OBJ_ZSET)) return;

    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) { // 编码是ziplist
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        double score;

        /* Use the first element in range as the starting point */
        eptr = zzlFirstInRange(zl,&range); // 获得范围内的第一个元素

        /* No "first" element */
        if (eptr == NULL) {
            addReply(c, shared.czero);
            return;
        }

        /* First element is in range */ // 获得元素分值
        sptr = ziplistNext(zl,eptr);
        score = zzlGetScore(sptr);
        serverAssertWithInfo(c,zobj,zslValueLteMax(score,&range));

        /* Iterate over elements in range */
        while (eptr) { // 迭代ziplist
            score = zzlGetScore(sptr); // 获得元素分值

            /* Abort when the node is no longer in range. */
            if (!zslValueLteMax(score,&range)) { // 超过范围
                break; // 跳出循环
            } else { // 累加count
                count++;
                zzlNext(zl,&eptr,&sptr); // 找下一个
            }
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) { // 编码是skiplist
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *zn;
        unsigned long rank;

        /* Find first element in range */
        zn = zslFirstInRange(zsl, &range); // 找范围内的一个节点

        /* Use rank of first element, if any, to determine preliminary count */
        if (zn != NULL) {
            rank = zslGetRank(zsl, zn->score, zn->ele); // 根据分值获得排名
            count = (zsl->length - (rank - 1)); // 计算count

            /* Find last element in range */
            zn = zslLastInRange(zsl, &range); // 获得范围内最后一个节点

            /* Use rank of last element, if any, to determine the actual count */
            if (zn != NULL) {
                rank = zslGetRank(zsl, zn->score, zn->ele); // 根据score获得排名
                count -= (zsl->length - rank); // 计算count
            }
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    addReplyLongLong(c, count);
}

void zlexcountCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    zlexrangespec range;
    unsigned long count = 0;

    /* Parse the range arguments */
    if (zslParseLexRange(c->argv[2],c->argv[3],&range) != C_OK) {
        addReplyError(c,"min or max not valid string range item");
        return;
    }

    /* Lookup the sorted set */
    if ((zobj = lookupKeyReadOrReply(c, key, shared.czero)) == NULL ||
        checkType(c, zobj, OBJ_ZSET))
    {
        zslFreeLexRange(&range);
        return;
    }

    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;

        /* Use the first element in range as the starting point */
        eptr = zzlFirstInLexRange(zl,&range);

        /* No "first" element */
        if (eptr == NULL) {
            zslFreeLexRange(&range);
            addReply(c, shared.czero);
            return;
        }

        /* First element is in range */
        sptr = ziplistNext(zl,eptr);
        serverAssertWithInfo(c,zobj,zzlLexValueLteMax(eptr,&range));

        /* Iterate over elements in range */
        while (eptr) {
            /* Abort when the node is no longer in range. */
            if (!zzlLexValueLteMax(eptr,&range)) {
                break;
            } else {
                count++;
                zzlNext(zl,&eptr,&sptr);
            }
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *zn;
        unsigned long rank;

        /* Find first element in range */
        zn = zslFirstInLexRange(zsl, &range);

        /* Use rank of first element, if any, to determine preliminary count */
        if (zn != NULL) {
            rank = zslGetRank(zsl, zn->score, zn->ele);
            count = (zsl->length - (rank - 1));

            /* Find last element in range */
            zn = zslLastInLexRange(zsl, &range);

            /* Use rank of last element, if any, to determine the actual count */
            if (zn != NULL) {
                rank = zslGetRank(zsl, zn->score, zn->ele);
                count -= (zsl->length - rank);
            }
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    zslFreeLexRange(&range);
    addReplyLongLong(c, count);
}

/* This command implements ZRANGEBYLEX, ZREVRANGEBYLEX. */
void genericZrangebylexCommand(client *c, int reverse) {
    zlexrangespec range;
    robj *key = c->argv[1];
    robj *zobj;
    long offset = 0, limit = -1;
    unsigned long rangelen = 0;
    void *replylen = NULL;
    int minidx, maxidx;

    /* Parse the range arguments. */
    if (reverse) {
        /* Range is given as [max,min] */
        maxidx = 2; minidx = 3;
    } else {
        /* Range is given as [min,max] */
        minidx = 2; maxidx = 3;
    }

    if (zslParseLexRange(c->argv[minidx],c->argv[maxidx],&range) != C_OK) {
        addReplyError(c,"min or max not valid string range item");
        return;
    }

    /* Parse optional extra arguments. Note that ZCOUNT will exactly have
     * 4 arguments, so we'll never enter the following code path. */
    if (c->argc > 4) {
        int remaining = c->argc - 4;
        int pos = 4;

        while (remaining) {
            if (remaining >= 3 && !strcasecmp(c->argv[pos]->ptr,"limit")) {
                if ((getLongFromObjectOrReply(c, c->argv[pos+1], &offset, NULL) != C_OK) ||
                    (getLongFromObjectOrReply(c, c->argv[pos+2], &limit, NULL) != C_OK)) {
                    zslFreeLexRange(&range);
                    return;
                }
                pos += 3; remaining -= 3;
            } else {
                zslFreeLexRange(&range);
                addReply(c,shared.syntaxerr);
                return;
            }
        }
    }

    /* Ok, lookup the key and get the range */
    if ((zobj = lookupKeyReadOrReply(c,key,shared.emptymultibulk)) == NULL ||
        checkType(c,zobj,OBJ_ZSET))
    {
        zslFreeLexRange(&range);
        return;
    }

    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        /* If reversed, get the last node in range as starting point. */
        if (reverse) {
            eptr = zzlLastInLexRange(zl,&range);
        } else {
            eptr = zzlFirstInLexRange(zl,&range);
        }

        /* No "first" element in the specified interval. */
        if (eptr == NULL) {
            addReply(c, shared.emptymultibulk);
            zslFreeLexRange(&range);
            return;
        }

        /* Get score pointer for the first element. */
        serverAssertWithInfo(c,zobj,eptr != NULL);
        sptr = ziplistNext(zl,eptr);

        /* We don't know in advance how many matching elements there are in the
         * list, so we push this object that will represent the multi-bulk
         * length in the output buffer, and will "fix" it later */
        replylen = addDeferredMultiBulkLength(c);

        /* If there is an offset, just traverse the number of elements without
         * checking the score because that is done in the next loop. */
        while (eptr && offset--) {
            if (reverse) {
                zzlPrev(zl,&eptr,&sptr);
            } else {
                zzlNext(zl,&eptr,&sptr);
            }
        }

        while (eptr && limit--) {
            /* Abort when the node is no longer in range. */
            if (reverse) {
                if (!zzlLexValueGteMin(eptr,&range)) break;
            } else {
                if (!zzlLexValueLteMax(eptr,&range)) break;
            }

            /* We know the element exists, so ziplistGet should always
             * succeed. */
            serverAssertWithInfo(c,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong));

            rangelen++;
            if (vstr == NULL) {
                addReplyBulkLongLong(c,vlong);
            } else {
                addReplyBulkCBuffer(c,vstr,vlen);
            }

            /* Move to next node */
            if (reverse) {
                zzlPrev(zl,&eptr,&sptr);
            } else {
                zzlNext(zl,&eptr,&sptr);
            }
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;

        /* If reversed, get the last node in range as starting point. */
        if (reverse) {
            ln = zslLastInLexRange(zsl,&range);
        } else {
            ln = zslFirstInLexRange(zsl,&range);
        }

        /* No "first" element in the specified interval. */
        if (ln == NULL) {
            addReply(c, shared.emptymultibulk);
            zslFreeLexRange(&range);
            return;
        }

        /* We don't know in advance how many matching elements there are in the
         * list, so we push this object that will represent the multi-bulk
         * length in the output buffer, and will "fix" it later */
        replylen = addDeferredMultiBulkLength(c);

        /* If there is an offset, just traverse the number of elements without
         * checking the score because that is done in the next loop. */
        while (ln && offset--) {
            if (reverse) {
                ln = ln->backward;
            } else {
                ln = ln->level[0].forward;
            }
        }

        while (ln && limit--) {
            /* Abort when the node is no longer in range. */
            if (reverse) {
                if (!zslLexValueGteMin(ln->ele,&range)) break;
            } else {
                if (!zslLexValueLteMax(ln->ele,&range)) break;
            }

            rangelen++;
            addReplyBulkCBuffer(c,ln->ele,sdslen(ln->ele));

            /* Move to next node */
            if (reverse) {
                ln = ln->backward;
            } else {
                ln = ln->level[0].forward;
            }
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    zslFreeLexRange(&range);
    setDeferredMultiBulkLength(c, replylen, rangelen);
}

void zrangebylexCommand(client *c) {
    genericZrangebylexCommand(c,0);
}

void zrevrangebylexCommand(client *c) {
    genericZrangebylexCommand(c,1);
}
/**
 * zcard key
 * @param c
 */
void zcardCommand(client *c) {
    robj *key = c->argv[1]; // 获得参数key
    robj *zobj;

    if ((zobj = lookupKeyReadOrReply(c,key,shared.czero)) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) return; // 在db中查找key对应的值对象 找不到 或找到了 类型不是zset 返回

    addReplyLongLong(c,zsetLength(zobj)); // 响应长度
}
/**
 * zscore key member
 * @param c
 */
void zscoreCommand(client *c) {
    robj *key = c->argv[1]; // 获得参数key
    robj *zobj;
    double score;

    if ((zobj = lookupKeyReadOrReply(c,key,shared.nullbulk)) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) return;  // 在db中查找key对应的值对象 找不到 或找到了 类型不是zset 返回

    if (zsetScore(zobj,c->argv[2]->ptr,&score) == C_ERR) { // 获得元素分值 是-1
        addReply(c,shared.nullbulk); // 响应空
    } else {
        addReplyDouble(c,score); // 响应分值
    }
}
/**
 *
 * @param c
 * @param reverse 0:默认顺序,从小到大 1:从大到小
 */
void zrankGenericCommand(client *c, int reverse) {
    robj *key = c->argv[1]; // 获得参数key
    robj *ele = c->argv[2]; // 获得参数ele
    robj *zobj;
    long rank;

    if ((zobj = lookupKeyReadOrReply(c,key,shared.nullbulk)) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) return; // 找key对应的值对象 是空 或类型不是zset 则返回

    serverAssertWithInfo(c,ele,sdsEncodedObject(ele));
    rank = zsetRank(zobj,ele->ptr,reverse); // 获得元素排名
    if (rank >= 0) {
        addReplyLongLong(c,rank); // 响应排名
    } else {
        addReply(c,shared.nullbulk); // 响应空
    }
}
/**
 * zrank key member
 * @param c
 */
void zrankCommand(client *c) {
    zrankGenericCommand(c, 0);
}

void zrevrankCommand(client *c) {
    zrankGenericCommand(c, 1);
}
/**
 * zscan key cursor [match pattern] [count count]
 * @param c
 */
void zscanCommand(client *c) {
    robj *o;
    unsigned long cursor; // 定义游标

    if (parseScanCursorOrReply(c,c->argv[2],&cursor) == C_ERR) return; // 取出游标
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptyscan)) == NULL ||
        checkType(c,o,OBJ_ZSET)) return; // 在db中获得key对应的对象 如果是空 则响应emptyscan 或者 类型不是zset 返回
    scanGenericCommand(c,o,cursor); // 迭代游标 响应元素分值
}

/* This command implements the generic zpop operation, used by:
 * ZPOPMIN, ZPOPMAX, BZPOPMIN and BZPOPMAX. This function is also used
 * inside blocked.c in the unblocking stage of BZPOPMIN and BZPOPMAX.
 *
 * If 'emitkey' is true also the key name is emitted, useful for the blocking
 * behavior of BZPOP[MIN|MAX], since we can block into multiple keys.
 *
 * The synchronous version instead does not need to emit the key, but may
 * use the 'count' argument to return multiple items if available. */
void genericZpopCommand(client *c, robj **keyv, int keyc, int where, int emitkey, robj *countarg) {
    int idx;
    robj *key = NULL;
    robj *zobj = NULL;
    sds ele;
    double score;
    long count = 1;

    /* If a count argument as passed, parse it or return an error. */
    if (countarg) {
        if (getLongFromObjectOrReply(c,countarg,&count,NULL) != C_OK)
            return;
        if (count <= 0) {
            addReply(c,shared.emptymultibulk);
            return;
        }
    }

    /* Check type and break on the first error, otherwise identify candidate. */
    idx = 0;
    while (idx < keyc) {
        key = keyv[idx++];
        zobj = lookupKeyWrite(c->db,key);
        if (!zobj) continue;
        if (checkType(c,zobj,OBJ_ZSET)) return;
        break;
    }

    /* No candidate for zpopping, return empty. */
    if (!zobj) {
        addReply(c,shared.emptymultibulk);
        return;
    }

    void *arraylen_ptr = addDeferredMultiBulkLength(c);
    long arraylen = 0;

    /* We emit the key only for the blocking variant. */
    if (emitkey) addReplyBulk(c,key);

    /* Remove the element. */
    do {
        if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
            unsigned char *zl = zobj->ptr;
            unsigned char *eptr, *sptr;
            unsigned char *vstr;
            unsigned int vlen;
            long long vlong;

            /* Get the first or last element in the sorted set. */
            eptr = ziplistIndex(zl,where == ZSET_MAX ? -2 : 0);
            serverAssertWithInfo(c,zobj,eptr != NULL);
            serverAssertWithInfo(c,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong));
            if (vstr == NULL)
                ele = sdsfromlonglong(vlong);
            else
                ele = sdsnewlen(vstr,vlen);

            /* Get the score. */
            sptr = ziplistNext(zl,eptr);
            serverAssertWithInfo(c,zobj,sptr != NULL);
            score = zzlGetScore(sptr);
        } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = zobj->ptr;
            zskiplist *zsl = zs->zsl;
            zskiplistNode *zln;

            /* Get the first or last element in the sorted set. */
            zln = (where == ZSET_MAX ? zsl->tail :
                                       zsl->header->level[0].forward);

            /* There must be an element in the sorted set. */
            serverAssertWithInfo(c,zobj,zln != NULL);
            ele = sdsdup(zln->ele);
            score = zln->score;
        } else {
            serverPanic("Unknown sorted set encoding");
        }

        serverAssertWithInfo(c,zobj,zsetDel(zobj,ele));
        server.dirty++;

        if (arraylen == 0) { /* Do this only for the first iteration. */
            char *events[2] = {"zpopmin","zpopmax"};
            notifyKeyspaceEvent(NOTIFY_ZSET,events[where],key,c->db->id);
            signalModifiedKey(c->db,key);
        }

        addReplyBulkCBuffer(c,ele,sdslen(ele));
        addReplyDouble(c,score);
        sdsfree(ele);
        arraylen += 2;

        /* Remove the key, if indeed needed. */
        if (zsetLength(zobj) == 0) {
            dbDelete(c->db,key);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",key,c->db->id);
            break;
        }
    } while(--count);

    setDeferredMultiBulkLength(c,arraylen_ptr,arraylen + (emitkey != 0));
}

/* ZPOPMIN key [<count>] */
void zpopminCommand(client *c) {
    if (c->argc > 3) {
        addReply(c,shared.syntaxerr);
        return;
    }
    genericZpopCommand(c,&c->argv[1],1,ZSET_MIN,0,
        c->argc == 3 ? c->argv[2] : NULL);
}

/* ZMAXPOP key [<count>] */
void zpopmaxCommand(client *c) {
    if (c->argc > 3) {
        addReply(c,shared.syntaxerr);
        return;
    }
    genericZpopCommand(c,&c->argv[1],1,ZSET_MAX,0,
        c->argc == 3 ? c->argv[2] : NULL);
}

/* BZPOPMIN / BZPOPMAX actual implementation. */
void blockingGenericZpopCommand(client *c, int where) {
    robj *o;
    mstime_t timeout;
    int j;

    if (getTimeoutFromObjectOrReply(c,c->argv[c->argc-1],&timeout,UNIT_SECONDS)
        != C_OK) return;

    for (j = 1; j < c->argc-1; j++) {
        o = lookupKeyWrite(c->db,c->argv[j]);
        if (o != NULL) {
            if (o->type != OBJ_ZSET) {
                addReply(c,shared.wrongtypeerr);
                return;
            } else {
                if (zsetLength(o) != 0) {
                    /* Non empty zset, this is like a normal ZPOP[MIN|MAX]. */
                    genericZpopCommand(c,&c->argv[j],1,where,1,NULL);
                    /* Replicate it as an ZPOP[MIN|MAX] instead of BZPOP[MIN|MAX]. */
                    rewriteClientCommandVector(c,2,
                        where == ZSET_MAX ? shared.zpopmax : shared.zpopmin,
                        c->argv[j]);
                    return;
                }
            }
        }
    }

    /* If we are inside a MULTI/EXEC and the zset is empty the only thing
     * we can do is treating it as a timeout (even with timeout 0). */
    if (c->flags & CLIENT_MULTI) {
        addReply(c,shared.nullmultibulk);
        return;
    }

    /* If the keys do not exist we must block */
    blockForKeys(c,BLOCKED_ZSET,c->argv + 1,c->argc - 2,timeout,NULL,NULL);
}

// BZPOPMIN key [key ...] timeout
void bzpopminCommand(client *c) {
    blockingGenericZpopCommand(c,ZSET_MIN);
}

// BZPOPMAX key [key ...] timeout
void bzpopmaxCommand(client *c) {
    blockingGenericZpopCommand(c,ZSET_MAX);
}
