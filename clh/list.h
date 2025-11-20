#ifndef CLH_QUEUE
#define CLH_QUEUE
#include "clh_defs.h"
#include "mem.h"
#include "thread.h"

typedef struct CLH_ListNode {
    struct CLH_ListNode *prev;
    struct CLH_ListNode *next;
    char                 data[];
} CLH_ListNode;

typedef struct {
    CLH_ListNode *head;
    CLH_ListNode *tail;
    CLH_ListNode *free_nodes;
    size_t        len;
    size_t        data_size;
    CLH_Mutex     mutex;
    CLH_Allocator allocator;
} CLH_List;

struct CLH_ListInitArgs {
    size_t        data_size;
    size_t        default_capacity;
    CLH_Allocator allocator;
};
#define clh_list_init(list, ...) \
    clh_list_init_(list, &(struct CLH_ListInitArgs){.allocator = CLH_SYSTEM_ALLOCATOR, __VA_ARGS__})
void clh_list_init_(CLH_List *list, struct CLH_ListInitArgs *args);
void clh_list_destroy(CLH_List *list);

void clh_list_push_data(CLH_List *list, void *data);

void          clh_list_push_node(CLH_List *list, CLH_ListNode *node);
CLH_ListNode *clh_list_pop_node(CLH_List *list);
void          clh_list_remove_node(CLH_List *list, CLH_ListNode *node);

void clh_list_get_all_nodes(CLH_List *list, CLH_ListNode **begin, CLH_ListNode **end);
void clh_list_release_nodes(CLH_List *list, CLH_ListNode *begin, CLH_ListNode *end);

CLH_ListNode *clh_list_new_node(CLH_List *list);
void          clh_list_release_node(CLH_List *list, CLH_ListNode *node);
void          clh_list_release_data(CLH_List *list, void *data);

#endif
