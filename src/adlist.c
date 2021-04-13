/* adlist.c - A generic doubly linked list implementation
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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


#include <stdlib.h>
#include "adlist.h"
#include "zmalloc.h"

/* Create a new list. The created list can be freed with
 * listRelease(), but private value of every node need to be freed
 * by the user before to call listRelease(), or by setting a free method using
 * listSetFreeMethod.
 *
 * On error, NULL is returned. Otherwise the pointer to the new list. */
/* 创建一个链表 */
list *listCreate(void)
{
    struct list *list;

    //分配链表初始化空间
    if ((list = zmalloc(sizeof(*list))) == NULL)
        return NULL;
    list->head = list->tail = NULL;  //为链表结构开辟内存空间
    list->len = 0;                   // 初始化头尾节点为NULL
    list->len = 0;                   // 初始化链表长度为0
    list->dup = NULL;                // 初始化链表复制函数为NULL
    list->free = NULL;               // 初始化节点数据域释放函数为NULL
    list->match = NULL;              // 初始化节点比较函数为NULL
    return list;
}

/* Remove all the elements from the list without destroying the list itself. */
/* 从列表中删除所有元素，而不破坏列表本身 */
void listEmpty(list *list)
{
    unsigned long len;
    listNode *current, *next;

    current = list->head;//获取链表头节点
    len = list->len;//获取链表长度
    while(len--) {//迭代
        next = current->next;
        if (list->free) list->free(current->value);//调用链表节点释放函数，释放节点
        zfree(current);//释放节点所对应的内存空间
        current = next;//自我迭代
    }
    list->head = list->tail = NULL;
    list->len = 0;
}

/* Free the whole list.
 *
 * This function can't fail. */
/* 链表释放 */
void listRelease(list *list)
{
    listEmpty(list);//释放链表中的所有元素
    zfree(list);//释放链表所占用的空间
}

/* Add a new node to the list, to head, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
/* 在链表头添加一个数据域包含指向’value’指针的新节点。出错时，会返回NULL且不会执行任何操作(链表不会有任何改变)。成功时，会返回你传入的’list’指针。 */
list *listAddNodeHead(list *list, void *value)
{
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)//初始化一个新节点
        return NULL;
    node->value = value;//置新节点的数据域为指定值
    if (list->len == 0) {//如果当前链表长度为0，头尾节点同时指向新节点
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {//如果当前链表长度大于0，设置新节点为头节点
        node->prev = NULL;
        node->next = list->head;
        list->head->prev = node;
        list->head = node;
    }
    list->len++;//更新链表长度
    return list;
}

/* Add a new node to the list, to tail, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
/* 在链表尾添加一个数据域包含指向’value’指针的新节点。出错时，会返回NULL且不会执行任何操作(链表不会有任何改变)。成功时，会返回你传入的’list’指针。 */
list *listAddNodeTail(list *list, void *value)
{
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)//初始化一个新节点
        return NULL;
    node->value = value;//置新节点的数据域为指定值
    if (list->len == 0) {//如果当前链表长度为0，头尾节点同时指向新节点
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {//如果当前链表长度大于0，设置新节点为尾节点
        node->prev = list->tail;
        node->next = NULL;
        list->tail->next = node;
        list->tail = node;
    }
    list->len++;
    return list;
}

/* 插入新节点到链表中某个节点的指定位置(前/后)。 */
list *listInsertNode(list *list, listNode *old_node, void *value, int after) {
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)//初始化一个新节点
        return NULL;
    node->value = value;//置新节点的数据域为指定值
    if (after) {//插入到老节点的后面
        node->prev = old_node;//设置新节点的上一个节点为老节点
        node->next = old_node->next;  // 设置新节点的下一个节点为老节点的下一个节点
        if (list->tail == old_node) {  // 如果链表尾节点为老节点，更新尾节点为新节点
            list->tail = node;
        }
    } else {//插入到老节点的前面
        node->next = old_node;//设置新节点的下一个节点为老节点
        node->prev = old_node->prev;//设置新节点的上一个节点为老节点的上一个节点
        if (list->head == old_node) {//如果链表头节点为老节点，更新头节点为新节点
            list->head = node;
        }
    }
    if (node->prev != NULL) {//更新新节点和它上一个节点的关系
        node->prev->next = node;
    }
    if (node->next != NULL) {//更新新节点和它下一个节点的关系
        node->next->prev = node;
    }
    list->len++;//更新链表长度
    return list;
}

/* Remove the specified node from the specified list.
 * It's up to the caller to free the private value of the node.
 *
 * This function can't fail. */
/* 链表制定节点删除 */
void listDelNode(list *list, listNode *node)
{
    if (node->prev)//如果当前节点有上一个节点
        node->prev->next = node->next;//更新当前节点的上一个节点的下一个节点为当前节点的下一个节点
    else//当前节点为头节点
        list->head = node->next;//设置链表的头节点为当前节点的下一个节点
    if (node->next)//当前节点有下一个节点
        node->next->prev = node->prev;//更新当前节点的下一个节点的上一个节点为当前节点的上一个节点
    else//当前节点为尾节点
        list->tail = node->prev;//设置当前节点的上一个节点为尾节点
    if (list->free) list->free(node->value);//调用节点释放方法
    zfree(node);//释放节点内存
    list->len--;//链表长度更新
}

/* Returns a list iterator 'iter'. After the initialization every
 * call to listNext() will return the next element of the list.
 *
 * This function can't fail. */
/* 根据迭代方向，获取链表迭代器 */
listIter *listGetIterator(list *list, int direction)
{
    listIter *iter;

    if ((iter = zmalloc(sizeof(*iter))) == NULL) return NULL;//初始化迭代器内存空间
    if (direction == AL_START_HEAD)//从头开始
        iter->next = list->head;
    else
        iter->next = list->tail;//从尾部开始
    iter->direction = direction;//设置迭代器方向
    return iter;
}

/* Release the iterator memory */
/* 释放迭代器 */
void listReleaseIterator(listIter *iter) {
    zfree(iter);
}

/* Create an iterator in the list private iterator structure */
/* 使迭代器的当前位置回到链表头，正向迭代 */
void listRewind(list *list, listIter *li) {
    li->next = list->head;
    li->direction = AL_START_HEAD;
}

/* 使迭代器的当前位置回到链尾部，逆向迭代 */
void listRewindTail(list *list, listIter *li) {
    li->next = list->tail;
    li->direction = AL_START_TAIL;
}

/* Return the next element of an iterator.
 * It's valid to remove the currently returned element using
 * listDelNode(), but not to remove other elements.
 *
 * The function returns a pointer to the next element of the list,
 * or NULL if there are no more elements, so the classical usage
 * pattern is:
 *
 * iter = listGetIterator(list,<direction>);
 * while ((node = listNext(iter)) != NULL) {
 *     doSomethingWith(listNodeValue(node));
 * }
 *
 * */
/* 获取当前迭代器的下一个节点 */
listNode *listNext(listIter *iter)
{
    listNode *current = iter->next;

    if (current != NULL) {
        if (iter->direction == AL_START_HEAD)
            iter->next = current->next;
        else
            iter->next = current->prev;
    }
    return current;
}

/* Duplicate the whole list. On out of memory NULL is returned.
 * On success a copy of the original list is returned.
 *
 * The 'Dup' method set with listSetDupMethod() function is used
 * to copy the node value. Otherwise the same pointer value of
 * the original node is used as value of the copied node.
 *
 * The original list both on success or error is never modified. */
/*
 *  复制整个链表。内存不足时返回NULL。成功则返回原始链表的拷贝。
 *  节点数据域的’Dup’方法由listSetDupMethod()函数设置，用来拷贝节点数据域。
 *  如果没有设置改函数，拷贝节点的数据域会使用原始节点数据域的指针，这相当于浅拷贝。
 *  原始链表不管在改函数成功还是失败的情况下都不会被修改。 */
list *listDup(list *orig)
{
    list *copy;
    listIter iter;
    listNode *node;

    if ((copy = listCreate()) == NULL)//创建一个copy链表
        return NULL;
    copy->dup = orig->dup;      // 拷贝链表和原始链表的节点复制函数相同
    copy->free = orig->free;    // 拷贝链表和原始链表的节点数据域释放函数相同
    copy->match = orig->match;  // 拷贝链表和原始链表的节点比较函数相同
    listRewind(orig, &iter);    // 使迭代器的当前位置回到链表头，正向迭代
    while((node = listNext(&iter)) != NULL) {
        void *value;

        if (copy->dup) {//设置了节点数据域复制函数
            value = copy->dup(node->value);//复制节点数据域
            if (value == NULL) {//数据域为NULL直接释放拷贝链表并返回NULL
                listRelease(copy);
                return NULL;
            }
        } else  //未设置节点数据域复制函数
            value = node->value;  //直接取原始链表节点的数据域指针赋值
        if (listAddNodeTail(copy, value) == NULL) {  //把复制得到的value添加到拷贝链表的尾部
            listRelease(copy);
            return NULL;
        }
    }
    return copy;
}

/* Search the list for a node matching a given key.
 * The match is performed using the 'match' method
 * set with listSetMatchMethod(). If no 'match' method
 * is set, the 'value' pointer of every node is directly
 * compared with the 'key' pointer.
 *
 * On success the first matching node pointer is returned
 * (search starts from head). If no matching node exists
 * NULL is returned. */
/*
 * 在链表中查找包含指定key的节点。
 * 使用由listSetMatchMethod()函数设置的’match’方法来判断是否匹配。
 * 如果没有设置’match’方法，就使用每个节点的’value’指针直接和’key’指针进行比较。
 * 匹配成功时，返回第一个匹配的节点指针(搜索从链表头开始)。
 * 没有找到匹配的节点就返回NULL。
 * */
listNode *listSearchKey(list *list, void *key)
{
    listIter iter;
    listNode *node;

    listRewind(list, &iter);//使迭代器的当前位置回到链表头，正向迭代
    while((node = listNext(&iter)) != NULL) {
        if (list->match) {//如果当前列表设置了比较函数，则直接使用设置的比较函数进行比较查找
            if (list->match(node->value, key)) {
                return node;
            }
        } else {// 否则直接比较key和node->value
            if (key == node->value) {
                return node;
            }
        }
    }
    return NULL;
}

/* Return the element at the specified zero-based index
 * where 0 is the head, 1 is the element next to head
 * and so on. Negative integers are used in order to count
 * from the tail, -1 is the last element, -2 the penultimate
 * and so on. If the index is out of range NULL is returned. */
/*
 * 把链表当成一个数组，返回指定索引的节点。
 * 负索引值用来从尾巴开始计算，-1表示最后一个元素，
 * -2表示倒数第二个元素，以此类推。
 * 当索引值超出返回返回NULL。
 * */
listNode *listIndex(list *list, long index) {
    listNode *n;

    if (index < 0) {//如果index大雨0，从尾部开始遍历查找
        index = (-index)-1;
        n = list->tail;
        while(index-- && n) n = n->prev;
    } else {//从头开始遍历查找
        n = list->head;
        while(index-- && n) n = n->next;
    }
    return n;
}

/* Rotate the list removing the tail node and inserting it to the head. */
/* 移除链表当前的尾节点，并把它设置为头节点。 */
void listRotateTailToHead(list *list) {
    if (listLength(list) <= 1) return;

    /* Detach current tail */
    listNode *tail = list->tail;
    list->tail = tail->prev; //设置尾节点为当前尾节点的前一个节点
    list->tail->next = NULL; //设置新尾节点后向关系
    /* Move it as head */
    list->head->prev = tail;  //设置当前头节点的前一个节点为原来的尾节点
    tail->prev = NULL;        // 设置新头节点前向关系
    tail->next = list->head;  // 设置新头节点后向关系
    list->head = tail;        // 更新新头节指针
}


/* Rotate the list removing the head node and inserting it to the tail. */
/* 移除链表当前的头节点，并把它设置为尾节点。 */
void listRotateHeadToTail(list *list) {
    if (listLength(list) <= 1) return;

    listNode *head = list->head;
    /* Detach current head */
    list->head = head->next;//设置头节点为当前头节点的下一个节点
    list->head->prev = NULL;//设置头节点的前向关系
    /* Move it as tail */
    list->tail->next = head;//设置尾节点的下一个节点为当前节点
    head->next = NULL;//设置尾节点的后向关系
    head->prev = list->tail;//设置尾节点的前项关系
    list->tail = head;//设置当前节点为尾节点
}

/* Add all the elements of the list 'o' at the end of the
 * list 'l'. The list 'other' remains empty but otherwise valid. */
/* 将链表o拼接在链表了后面 */
void listJoin(list *l, list *o) {
    if (o->head)//如果o有头节点，则该节点的上一个节点只想l节点的尾节点
        o->head->prev = l->tail;

    if (l->tail)//如果l节点有尾节点，则，当前节点的下一个节点指向o节点的头节点
        l->tail->next = o->head;
    else
        l->head = o->head;//否则证明l节点为空节点，l节点的头节点直接指向o的头节点

    if (o->tail) l->tail = o->tail;//如果o的尾节点存在，则l的尾节点直接指向o的尾节点
    l->len += o->len; //更新l链表的长度

    /* Setup other as an empty list. */
    o->head = o->tail = NULL;//重置o节点
    o->len = 0;
}
