#ifndef IR4_STRPOOL_H
#define IR4_STRPOOL_H

#include "ir4.h"

/* Growable string scratch buffer. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_reset(StrBuf *sb);
void sb_putc(StrBuf *sb, char c);
void sb_append_n(StrBuf *sb, const char *s, size_t n);
void sb_append(StrBuf *sb, const char *s);
const char *sb_intern(StrBuf *sb);

/* Sorted string interning. */
const char *intern_find(const char *s);
const char *intern_str(const char *s);
const char *intern_str_n(const char *s, size_t len);

#endif
