/* codebuf.c - Shared binary code & data emission buffer for IR4th (Strict C89) */
#include "codebuf.h"
#include "strpool.h"
#include "vec.h"

typedef struct {
    u8 split;
    u16 off1;
    u16 off2;
    u16 data_offset;
} DataReloc;

typedef struct {
    u16 patch_offset;
    const char *name;
} ExternReloc;

typedef struct {
    u16 patch_data_off;
    u16 target_val;
    u8 addr_bytes;
    u8 is_code;
} DataInitReloc;

VEC_OF(ByteVec, u8);
VEC_OF(DataRelocVec, DataReloc);
VEC_OF(DataInitRelocVec, DataInitReloc);
VEC_OF(ExternRelocVec, ExternReloc);

static ByteVec code_buf;
static u16 base_addr = 0x0100; /* Default: CP/M .COM load address */
static ByteVec data_buf;
static u16 data_init_size = 0;
static DataRelocVec relocs;
static DataInitRelocVec data_init_relocs;
static ExternRelocVec ext_relocs;

void cb_reset(u16 base) {
    code_buf.len = 0;
    base_addr = base;
    data_buf.len = 0;
    data_init_size = 0;
    relocs.len = 0;
    data_init_relocs.len = 0;
    ext_relocs.len = 0;
}

u16 cb_pc(void) { return (u16)code_buf.len; }

u16 cb_cur_addr(void) { return (u16)(base_addr + code_buf.len); }

const u8 *cb_data(void) { return code_buf.data; }

void emit8(u8 b) { VEC_PUSH(code_buf, b); }

void emit16(u16 w) {
    emit8((u8)(w & 0xFF));        /* Little-endian low byte */
    emit8((u8)((w >> 8) & 0xFF)); /* Little-endian high byte */
}

void emit32(u32 w) {
    emit16((u16)(w & 0xFFFFU));
    emit16((u16)((w >> 16) & 0xFFFFU));
}

void emit8_8(u8 a, u8 b) {
    emit8(a);
    emit8(b);
}

void emit8_16(u8 a, u16 w) {
    emit8(a);
    emit16(w);
}

void emit8_8_8(u8 a, u8 b, u8 c) {
    emit8(a);
    emit8(b);
    emit8(c);
}

void cb_rewind(u16 n) {
    if (n > code_buf.len) {
        ir4_die("code buffer underflow");
    }
    code_buf.len -= n;
}

void patch8(u16 patch_offset, u8 val) {
    if (patch_offset >= code_buf.len) {
        ir4_die("patch offset out of bounds");
    }
    code_buf.data[patch_offset] = val;
}

void patch16(u16 patch_offset, u16 target_addr) {
    if ((size_t)patch_offset + 1 >= code_buf.len) {
        ir4_die("patch offset out of bounds");
    }
    code_buf.data[patch_offset] = (u8)(target_addr & 0xFF);
    code_buf.data[patch_offset + 1] = (u8)((target_addr >> 8) & 0xFF);
}

void patch32(u16 patch_offset, u32 val) {
    patch16(patch_offset, (u16)(val & 0xFFFFU));
    patch16((u16)(patch_offset + 2), (u16)((val >> 16) & 0xFFFFU));
}

void put_le16(u8 *p, u16 v) {
    p[0] = (u8)(v & 0xFFU);
    p[1] = (u8)((v >> 8) & 0xFFU);
}

void put_le32(u8 *p, u32 v) {
    p[0] = (u8)(v & 0xFFUL);
    p[1] = (u8)((v >> 8) & 0xFFUL);
    p[2] = (u8)((v >> 16) & 0xFFUL);
    p[3] = (u8)((v >> 24) & 0xFFUL);
}

int cb_write_raw(FILE *fp, int include_data) {
    if (code_buf.len > 0 && fwrite(code_buf.data, 1, code_buf.len, fp) != code_buf.len) {
        return 0;
    }
    if (include_data && data_init_size > 0 &&
        fwrite(data_buf.data, 1, data_init_size, fp) != (size_t)data_init_size) {
        return 0;
    }
    return 1;
}

u16 cb_alloc_data(u16 size, u8 align) {
    size_t aligned = data_buf.len;
    size_t mask = (size_t)(align > 1 ? align - 1 : 0);
    size_t new_len;
    if (mask > 0) {
        aligned = (aligned + mask) & ~mask;
    }
    new_len = aligned + size;
    if (new_len > data_buf.len) {
        VEC_ENSURE(data_buf, new_len);
        memset(data_buf.data + data_buf.len, 0, new_len - data_buf.len);
        data_buf.len = new_len;
    }
    return (u16)aligned;
}

u16 cb_data_size(void) { return (u16)data_buf.len; }

void cb_emit_data_u8(u8 b) {
    VEC_PUSH(data_buf, b);
    data_init_size = (u16)data_buf.len;
}

void cb_emit_data_u16(u16 w) {
    cb_emit_data_u8((u8)(w & 0xFF));
    cb_emit_data_u8((u8)((w >> 8) & 0xFF));
}

void cb_emit_data_word(i32 val, u8 word_bytes) {
    u32 lo = (u32)val;
    u8 fill = (val < 0) ? 0xFF : 0x00;
    u8 i;
    if (word_bytes == 2) {
        cb_emit_data_u16((u16)lo);
        return;
    }
    for (i = 0; i < 4; i++) {
        cb_emit_data_u8((u8)(lo & 0xFF));
        lo >>= 8;
    }
    for (i = 4; i < word_bytes; i++) {
        cb_emit_data_u8(fill);
    }
}

const u8 *cb_init_data(void) { return data_buf.data; }

u16 cb_init_data_size(void) { return data_init_size; }

void cb_add_data_reloc(u16 patch_offset, u16 data_offset) {
    DataReloc *r = VEC_ADD(relocs);
    r->split = 0;
    r->off1 = patch_offset;
    r->off2 = 0;
    r->data_offset = data_offset;
}

void cb_add_data_reloc_split(u16 lo_offset, u16 hi_offset, u16 data_offset) {
    DataReloc *r = VEC_ADD(relocs);
    r->split = 1;
    r->off1 = lo_offset;
    r->off2 = hi_offset;
    r->data_offset = data_offset;
}

void cb_patch_data_init_relocs_base(u32 code_base, u32 data_base) {
    size_t i;
    for (i = 0; i < data_init_relocs.len; i++) {
        u32 val = data_init_relocs.data[i].is_code
                      ? (code_base + (u32)data_init_relocs.data[i].target_val)
                      : (data_base + (u32)data_init_relocs.data[i].target_val);
        u16 off = data_init_relocs.data[i].patch_data_off;
        u8 b;
        for (b = 0; b < data_init_relocs.data[i].addr_bytes; b++) {
            data_buf.data[off + b] = (u8)(val & 0xFF);
            val >>= 8;
        }
    }
}

void cb_patch_data_relocs(u16 data_base) {
    size_t i;
    for (i = 0; i < relocs.len; i++) {
        u16 addr = (u16)(data_base + relocs.data[i].data_offset);
        if (relocs.data[i].split) {
            code_buf.data[relocs.data[i].off1] = (u8)(addr & 0xFF);
            code_buf.data[relocs.data[i].off2] = (u8)((addr >> 8) & 0xFF);
        } else {
            patch16(relocs.data[i].off1, addr);
        }
    }
    cb_patch_data_init_relocs_base(0, (u32)data_base);
}

void cb_add_data_init_reloc(u16 patch_data_off, u16 target_val, u8 addr_bytes, u8 is_code) {
    DataInitReloc *r = VEC_ADD(data_init_relocs);
    r->patch_data_off = patch_data_off;
    r->target_val = target_val;
    r->addr_bytes = addr_bytes;
    r->is_code = is_code;
}

u16 cb_data_init_reloc_count(void) { return (u16)data_init_relocs.len; }

void cb_get_data_init_reloc(u16 idx, u16 *patch_data_off, u16 *target_val, u8 *addr_bytes,
                            u8 *is_code) {
    *patch_data_off = data_init_relocs.data[idx].patch_data_off;
    *target_val = data_init_relocs.data[idx].target_val;
    *addr_bytes = data_init_relocs.data[idx].addr_bytes;
    *is_code = data_init_relocs.data[idx].is_code;
}

void cb_patch_data_init_reloc(u16 idx, u16 code_addr) {
    data_init_relocs.data[idx].target_val = code_addr;
}

void cb_add_extern_reloc(u16 patch_offset, const char *name) {
    ExternReloc *r = VEC_ADD(ext_relocs);
    r->patch_offset = patch_offset;
    r->name = intern_str(name);
}

u16 cb_extern_reloc_count(void) { return (u16)ext_relocs.len; }

const char *cb_extern_reloc_name(u16 idx, u16 *patch_offset) {
    *patch_offset = ext_relocs.data[idx].patch_offset;
    return ext_relocs.data[idx].name;
}
