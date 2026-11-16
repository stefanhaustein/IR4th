/* be_6502_c64.c - Commodore 64 (.prg) BASIC SYS stub & KERNAL CHROUT epilogue (Strict C89) */
#include "codebuf.h"
#include "frontend.h"

#define C64_CHROUT 0xFFD2
#define C64_TMP_LO 0xFB
#define C64_TMP_HI 0xFC

static u16 c64_jmp_pc = 0;

static void c64_begin(u16 base) {
    char digits[8];
    int len = 0;
    int i;
    u16 stub_end;
    u16 next_line;

    /* 10 SYS <addr> : 2-byte next_line + 2-byte line_num + 1-byte SYS(0x9E) + digits + NUL + 2-byte end */
    sprintf(digits, "%u", (unsigned)(base + 12));
    while (digits[len]) len++;
    if (len != 4) {
        /* Adjust for 3- or 5-digit addresses */
        sprintf(digits, "%u", (unsigned)(base + 8 + len));
        len = 0;
        while (digits[len]) len++;
    }
    stub_end = (u16)(base + 8 + len);
    next_line = (u16)(base + 6 + len);

    emit16(next_line);
    emit16(10);
    emit8(0x9E); /* BASIC SYS token */
    for (i = 0; i < len; i++) emit8((u8)digits[i]);
    emit8(0x00);
    emit16(0x0000);
    (void)stub_end;

    /* JMP entry_addr (patched by c64_end) */
    emit8(0x4C);
    c64_jmp_pc = cb_pc();
    emit16(0x0000);
}

static int c64_write(FILE *fp, u16 base) {
    u8 hdr[2];
    put_le16(hdr, base);
    if (fwrite(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) return 0;
    return cb_write_raw(fp, 1);
}

static void c64_end(int main_idx) {
    const FuncSig *sig = ir4_func_sig((u8)main_idx);
    u16 entry_addr = cb_cur_addr();
    u16 putchar_call1 = 0;
    u16 putchar_call2 = 0;
    u16 putchar_call3 = 0;
    u16 print_call1 = 0;
    u16 print_call2 = 0;
    u16 print_u16_addr;
    u16 div_loop_pc;

    if (sig->in_count != 0 || sig->out_count > 1) {
        fprintf(stderr, "--c64 requires 'main' with 0 inputs and at most 1 output\n");
        exit(1);
    }

    patch16(c64_jmp_pc, entry_addr);

    /* CLD; LDX #$00 (initialize IR4th data stack without clobbering hardware SP) */
    emit8(0xD8);
    emit8(0xA2); emit8(0x00);

    /* JSR main */
    emit8(0x20); emit16(sig->addr);

    if (sig->out_count == 1) {
        u16 stk_off = be_6502_stack_data_off();
        /* Pop 16-bit signed value from (stack_base,X) into $FB/$FC */
        emit8(0xBD); cb_add_data_reloc(cb_pc(), stk_off); emit16(0x0000);
        emit8(0x85); emit8(C64_TMP_LO);
        emit8(0xBD); cb_add_data_reloc(cb_pc(), (u16)(stk_off + 1)); emit16(0x0000);
        emit8(0x85); emit8(C64_TMP_HI);
        emit8(0xE8); emit8(0xE8); /* INX; INX */

        /* If $FC < 0, print '-' and negate $FB/$FC */
        emit8(0xA5); emit8(C64_TMP_HI);
        emit8(0x10); emit8(18);   /* BPL +18 */

        emit8(0xA9); emit8('-');
        emit8(0x20); putchar_call1 = cb_pc(); emit16(0x0000);
        emit8(0x38);              /* SEC */
        emit8(0xA9); emit8(0x00);
        emit8(0xE5); emit8(C64_TMP_LO);
        emit8(0x85); emit8(C64_TMP_LO);
        emit8(0xA9); emit8(0x00);
        emit8(0xE5); emit8(C64_TMP_HI);
        emit8(0x85); emit8(C64_TMP_HI);

        /* JSR print_u16 */
        emit8(0x20); print_call1 = cb_pc(); emit16(0x0000);

        /* Print PETSCII CR (13) */
        emit8(0xA9); emit8(13);
        emit8(0x20); putchar_call2 = cb_pc(); emit16(0x0000);
    }

    /* Return to BASIC */
    emit8(0x60); /* RTS */

    if (sig->out_count == 1) {
        print_u16_addr = cb_cur_addr();
        patch16(print_call1, print_u16_addr);

        /* div10: $FB/$FC /= 10, A = remainder */
        emit8(0xA9); emit8(0x00);
        emit8(0xA0); emit8(16);
        div_loop_pc = cb_pc();
        emit8(0x06); emit8(C64_TMP_LO);
        emit8(0x26); emit8(C64_TMP_HI);
        emit8(0x2A);
        emit8(0xC9); emit8(10);
        emit8(0x90); emit8(4);
        emit8(0xE9); emit8(10);
        emit8(0xE6); emit8(C64_TMP_LO);
        emit8(0x88);
        emit8(0xD0); emit8((u8)(div_loop_pc - (cb_pc() + 1)));

        emit8(0x48); /* PHA */
        emit8(0xA5); emit8(C64_TMP_LO);
        emit8(0x05); emit8(C64_TMP_HI);
        emit8(0xF0); emit8(3);
        emit8(0x20); print_call2 = cb_pc(); emit16(0x0000);
        patch16(print_call2, print_u16_addr);

        emit8(0x68); /* PLA */
        emit8(0x18); /* CLC */
        emit8(0x69); emit8('0');
        emit8(0x20); putchar_call3 = cb_pc(); emit16(0x0000);
        emit8(0x60); /* RTS */

        patch16(putchar_call1, C64_CHROUT);
        patch16(putchar_call2, C64_CHROUT);
        patch16(putchar_call3, C64_CHROUT);
    }
}

const Platform plat_6502_c64 = {
    "--c64", &be_6502, 0x0801, "c64", 1,
    c64_begin, c64_end, c64_write, NULL
};
