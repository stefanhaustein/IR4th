#ifndef IR4_VEC_H
#define IR4_VEC_H

#include "ir4.h"

#define VEC_OF(Name, T) \
    typedef struct {    \
        T *data;        \
        size_t len;     \
        size_t cap;     \
    } Name

void *vec_grow(void *data, size_t *cap, size_t elem_size, size_t need);
void *vec_calloc(size_t n, size_t elem_size);

#define NEW(T) ((T *)vec_calloc(1, sizeof(T)))

#define VEC_ENSURE(v, need) \
    (((need) > (v).cap) ? ((v).data = vec_grow((v).data, &(v).cap, sizeof(*(v).data), (need))) \
                        : (v).data)

#define VEC_ADD(v) (VEC_ENSURE((v), (v).len + 1), &(v).data[(v).len++])

#define VEC_PUSH(v, val) (*VEC_ADD(v) = (val))

#define VEC_INSERT(v, idx, val)                                                                   \
    do {                                                                                          \
        size_t _i = (idx);                                                                        \
        VEC_ENSURE((v), (v).len + 1);                                                             \
        if (_i < (v).len) {                                                                       \
            memmove(&(v).data[_i + 1], &(v).data[_i], ((v).len - _i) * sizeof(*(v).data));        \
        }                                                                                         \
        (v).data[_i] = (val);                                                                     \
        (v).len++;                                                                                \
    } while (0)

#endif
