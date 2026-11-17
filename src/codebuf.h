/* codebuf.h - Shared code & data emission buffer API (Strict C89)
 *
 * Manages the target machine-code buffer, initialized/BSS data segment,
 * data-offset relocations (patched once data placement is known), and
 * named external symbol relocations (resolved by JIT or ELF .o output).
 */
#ifndef CODEBUF_H
#define CODEBUF_H

#include "ir4.h"

void cb_reset(u16 base);
u16 cb_pc(void);
u16 cb_cur_addr(void);
const u8 *cb_data(void);

void emit8(u8 b);
void emit16(u16 w);
void emit32(u32 w);
void emit8_8(u8 a, u8 b);
void emit8_16(u8 a, u16 w);
void emit8_8_8(u8 a, u8 b, u8 c);
void cb_rewind(u16 n);

void patch8(u16 patch_offset, u8 val);
void patch16(u16 patch_offset, u16 target_addr);
void patch32(u16 patch_offset, u32 val);
void put_le16(u8 *p, u16 v);
void put_le32(u8 *p, u32 v);
int cb_write_raw(FILE *fp, int include_data);

u16 cb_alloc_data(u16 size, u8 align);
u16 cb_data_size(void);
void cb_emit_data_u8(u8 b);
void cb_emit_data_u16(u16 w);
void cb_emit_data_word(i32 val, u8 word_bytes);
const u8 *cb_init_data(void);
u16 cb_init_data_size(void);

void cb_add_data_reloc(u16 patch_offset, u16 data_offset);
void cb_add_data_reloc_split(u16 lo_offset, u16 hi_offset, u16 data_offset);
void cb_patch_data_relocs(u16 data_base);

void cb_add_data_init_reloc(u16 patch_data_off, u16 target_val, u8 addr_bytes, u8 is_code);
u16  cb_data_init_reloc_count(void);
void cb_get_data_init_reloc(u16 idx, u16 *patch_data_off, u16 *target_val, u8 *addr_bytes, u8 *is_code);
void cb_patch_data_init_reloc(u16 idx, u16 code_addr);
void cb_patch_data_init_relocs_base(u32 code_base, u32 data_base);

void cb_add_extern_reloc(u16 patch_offset, const char *name);
u16 cb_extern_reloc_count(void);
const char *cb_extern_reloc_name(u16 idx, u16 *patch_offset);

#endif /* CODEBUF_H */
