#include "list.h"
#include <assert.h>
#include <string.h>

void clh_list_init_(CLH_List *list, struct CLH_ListInitArgs *args)
{
    if (args->data_size == 0) {
        fprintf(stderr, "error: list requires a data size.\n");
        return;
    }
    list->head = NULL;
    list->tail = NULL;
    list->free_nodes = NULL;
    list->len = 0;
    list->data_size = args->data_size;
    list->mutex = clh_mutex_create();
    list->allocator = args->allocator;

    CLH_ListNode *node = NULL;
    for (size_t i = 0; i < args->default_capacity; ++i) {
        node = clh_allocator_allocate(list->allocator, sizeof(*node) + list->data_size);
        node->prev = NULL;
        node->next = list->free_nodes;
        list->free_nodes = node;
    }
}

void clh_list_destroy(CLH_List *list)
{
    CLH_ListNode *cur = NULL, *next = NULL;
    size_t non_free_count = 0;
    for (cur = list->head; cur != NULL; cur = next) {
        ++non_free_count;
        next = cur->next;
        clh_allocator_free(list->allocator, cur);
    }
    if (non_free_count > 0) {
        clh_warning("MEM", "%ld requests where not released.", non_free_count);
    }
    for (cur = list->free_nodes; cur != NULL; cur = next) {
        next = cur->next;
        clh_allocator_free(list->allocator, cur);
    }
    clh_mutex_destroy(&list->mutex);
}

void clh_list_push_data(CLH_List *list, void *data)
{
    CLH_ListNode *node = clh_list_new_node(list);
    memcpy(node->data, data, list->data_size);
    clh_list_push_node(list, node);
}

void clh_list_push_node(CLH_List *list, CLH_ListNode *node)
{
    CLH_LOCK_REGION(list->mutex)
    {
        node->prev = list->tail;
        node->next = NULL;
        if (node->prev != NULL) {
            node->prev->next = node;
        } else {
            assert(list->head == NULL);
            list->head = node;
        }
        list->tail = node;
        list->len += 1;
    }
}

CLH_ListNode *clh_list_pop_node(CLH_List *list)
{
    CLH_ListNode *node = NULL;

    if (list->tail == 0) {
        return node;
    }
    CLH_LOCK_REGION(list->mutex)
    {
        node = list->tail;
        list->tail = node->prev;

        if (node->prev != NULL) {
            node->prev->next = NULL;
            node->prev = NULL;
        } else {
            assert(list->head == node);
            list->head = node->next;
        }
        list->len -= 1;
    }
    return node;
}

void clh_list_remove_node(CLH_List *list, CLH_ListNode *node)
{
    CLH_LOCK_REGION(list->mutex)
    {
        if (node->next != NULL) {
            node->next->prev = node->prev;
        } else {
            assert(list->tail == node);
            list->tail = node->prev;
        }

        if (node->prev != NULL) {
            node->prev->next = node->next;
        } else {
            assert(list->head == node);
            list->head = node->next;
        }
        list->len -= 1;
        node->prev = NULL;
        node->next = list->free_nodes;
        list->free_nodes = node;
    }
}

void clh_list_get_all_nodes(CLH_List *list, CLH_ListNode **begin, CLH_ListNode **end)
{
    CLH_LOCK_REGION(list->mutex)
    {
        *begin = list->head;
        *end = list->tail;
        list->head = NULL;
        list->tail = NULL;
        list->len = 0;
    }
}

void clh_list_release_nodes(CLH_List *list, CLH_ListNode *begin, CLH_ListNode *end)
{
    CLH_LOCK_REGION(list->mutex)
    {
        if (begin == NULL || end == NULL) {
            return;
        }
        end->next = list->free_nodes;
        list->free_nodes = begin;
    }
}

CLH_ListNode *clh_list_new_node(CLH_List *list)
{
    CLH_ListNode *node = NULL;
    CLH_LOCK_REGION(list->mutex)
    {
        if (list->free_nodes == NULL) {
            list->free_nodes
                = clh_allocator_allocate(list->allocator, sizeof(*node) + list->data_size);
            list->free_nodes->prev = NULL;
            list->free_nodes->next = NULL;
        }
        node = list->free_nodes;
        list->free_nodes = node->next;
        node->next = NULL;
        node->prev = NULL;
    }
    return node;
}

void clh_list_release_node(CLH_List *list, CLH_ListNode *node)
{
    CLH_LOCK_REGION(list->mutex)
    {
        node->prev = NULL;
        node->next = list->free_nodes;
        list->free_nodes = node;
    }
}

void clh_list_release_data(CLH_List *list, void *data)
{
    clh_list_release_node(list, (CLH_ListNode *)(data - sizeof(CLH_ListNode)));
}
