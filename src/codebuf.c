/* codebuf.c - Shared binary code emission buffer for IR4th (Strict C89) */
#include "ir4c.h"

static u8  code_buf[MAX_CODE];
static u16 code_pc   = 0;
static u16 base_addr = 0x0100; /* Default: CP/M .COM load address */

void cb_reset(u16 base) {
    code_pc   = 0;
    base_addr = base;
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
