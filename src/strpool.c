#include "strpool.h"
#include "vec.h"

VEC_OF(InternVec, char *);
static InternVec intern_tab;

static int cmp_str_n(const char *a, const char *b, size_t blen) {
    size_t i;
    for (i = 0; i < blen; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb)
            return (ca < cb) ? -1 : 1;
    }
    return (a[blen] == '\0') ? 0 : 1;
}

static size_t intern_bsearch(const char *s, size_t len, int *found) {
    size_t lo = 0, hi = intern_tab.len;
    *found = 0;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp_str_n(intern_tab.data[mid], s, len);
        if (c == 0) {
            *found = 1;
            return mid;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

const char *intern_find(const char *s) {
    int found;
    size_t idx = intern_bsearch(s, strlen(s), &found);
    return found ? intern_tab.data[idx] : NULL;
}

const char *intern_str_n(const char *s, size_t len) {
    int found;
    size_t idx = intern_bsearch(s, len, &found);
    char *copy;
    if (found)
        return intern_tab.data[idx];
    copy = (char *)malloc(len + 1);
    if (!copy)
        ir4_die("out of memory in string intern table");
    if (len > 0)
        memcpy(copy, s, len);
    copy[len] = '\0';
    VEC_INSERT(intern_tab, idx, copy);
    return copy;
}

const char *intern_str(const char *s) { return intern_str_n(s, strlen(s)); }

void sb_reset(StrBuf *sb) {
    sb->len = 0;
    if (sb->data)
        sb->data[0] = '\0';
}

void sb_putc(StrBuf *sb, char c) {
    VEC_ENSURE(*sb, sb->len + 2);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

void sb_append_n(StrBuf *sb, const char *s, size_t n) {
    if (n == 0)
        return;
    VEC_ENSURE(*sb, sb->len + n + 1);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_append(StrBuf *sb, const char *s) { sb_append_n(sb, s, strlen(s)); }

const char *sb_intern(StrBuf *sb) { return intern_str_n(sb->data ? sb->data : "", sb->len); }
