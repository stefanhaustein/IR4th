#include "vec.h"
#include "ir4.h"

void *vec_grow(void *data, size_t *cap, size_t elem_size, size_t need) {
    size_t new_cap = *cap ? *cap * 2 : 16;
    void *p;
    while (new_cap < need)
        new_cap *= 2;
    p = realloc(data, new_cap * elem_size);
    if (!p)
        ir4_die("out of memory");
    *cap = new_cap;
    return p;
}

void *vec_calloc(size_t n, size_t elem_size) {
    void *p = calloc(n, elem_size);
    if (!p)
        ir4_die("out of memory");
    return p;
}
