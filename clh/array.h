#ifndef CLH_ARRAY
#define CLH_ARRAY

#define Array(ElementType) \
    typedef struct {       \
        ElementType *ptr;  \
        size_t       cap;  \
        size_t       len;  \
    }

#define array_create(arr, capacity)                        \
    do {                                                   \
        (arr).ptr = malloc(capacity * sizeof(*(arr).ptr)); \
        (arr).cap = capacity;                              \
        (arr).len = 0;                                     \
    } while (false);

#define array_destroy(arr) \
    do {                   \
        free((arr).ptr);   \
    } while (false);

#define array_append(arr, elt)                                              \
    do {                                                                    \
        if ((arr).len + 1 >= (arr).cap) {                                   \
            assert((arr).cap > 0);                                          \
            (arr).cap *= 2;                                                 \
            (arr).ptr = realloc((arr).ptr, (arr).cap * sizeof(*(arr).ptr)); \
        }                                                                   \
        (arr).ptr[(arr).len++] = elt;                                       \
    } while (false);

#define array_remove(arr, idx)                       \
    do {                                             \
        if ((arr).len > 0 || idx < (arr).len) {      \
            (arr).ptr[idx] = (arr).ptr[--(arr).len]; \
        }                                            \
    } while (false);

#endif // CLH_ARRAY
