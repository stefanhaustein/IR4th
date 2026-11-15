/* codebuf.c - Shared binary code & data emission buffer for IR4th (Strict C89) */
#include "ir4c.h"

typedef struct {
    u8  split;
    u16 off1;
    u16 off2;
    u16 data_offset;
} DataReloc;

static u8        code_buf[MAX_CODE];
static u16       code_pc   = 0;
static u16       base_addr = 0x0100; /* Default: CP/M .COM load address */
static u16       data_size = 0;
static DataReloc relocs[MAX_RELOCS];
static u16       reloc_cnt = 0;

void cb_reset(u16 base) {
    code_pc   = 0;
    base_addr = base;
    data_size = 0;
    reloc_cnt = 0;
}

u16 cb_pc(void) {
    return code_pc;
}

u16 cb_cur_addr(void) {
    return (u16)(base_addr + code_pc);
}

const u8 *cb_data(void) {
    return code_buf;
}

void emit8(u8 b) {
    if (code_pc >= MAX_CODE) {
        ir4_die("code buffer overflow");
    }
    code_buf[code_pc++] = b;
}

void emit16(u16 w) {
    emit8((u8)(w & 0xFF));         /* Little-endian low byte */
    emit8((u8)((w >> 8) & 0xFF));  /* Little-endian high byte */
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

void patch16(u16 patch_offset, u16 target_addr) {
    if ((u32)patch_offset + 1 >= MAX_CODE) {
        ir4_die("patch offset out of bounds");
    }
    code_buf[patch_offset]     = (u8)(target_addr & 0xFF);
    code_buf[patch_offset + 1] = (u8)((target_addr >> 8) & 0xFF);
}

u16 cb_alloc_data(u16 size, u8 align) {
    u16 aligned = data_size;
    u16 mask = (u16)(align > 1 ? align - 1 : 0);
    if (mask > 0) {
        aligned = (u16)((aligned + mask) & ~mask);
    }
    if ((u32)aligned + size > MAX_DATA) {
        ir4_die("data segment overflow");
    }
    data_size = (u16)(aligned + size);
    return aligned;
}

u16 cb_data_size(void) {
    return data_size;
}

void cb_add_data_reloc(u16 patch_offset, u16 data_offset) {
    if (reloc_cnt >= MAX_RELOCS) {
        ir4_die("too many data relocations");
    }
    relocs[reloc_cnt].split       = 0;
    relocs[reloc_cnt].off1        = patch_offset;
    relocs[reloc_cnt].off2        = 0;
    relocs[reloc_cnt].data_offset = data_offset;
    reloc_cnt++;
}

void cb_add_data_reloc_split(u16 lo_offset, u16 hi_offset, u16 data_offset) {
    if (reloc_cnt >= MAX_RELOCS) {
        ir4_die("too many data relocations");
    }
    relocs[reloc_cnt].split       = 1;
    relocs[reloc_cnt].off1        = lo_offset;
    relocs[reloc_cnt].off2        = hi_offset;
    relocs[reloc_cnt].data_offset = data_offset;
    reloc_cnt++;
}

void cb_patch_data_relocs(u16 data_base) {
    u16 i;
    for (i = 0; i < reloc_cnt; i++) {
        u16 addr = (u16)(data_base + relocs[i].data_offset);
        if (relocs[i].split) {
            code_buf[relocs[i].off1] = (u8)(addr & 0xFF);
            code_buf[relocs[i].off2] = (u8)((addr >> 8) & 0xFF);
        } else {
            patch16(relocs[i].off1, addr);
        }
    }
}
