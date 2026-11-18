/* rv32_fp.c - Minimal IEEE-754 binary32 wrapper for RV32IM soft-float.
 * Compiled with -march=rv32im -mabi=ilp32 -Os -nostdlib -lgcc into src/be_rv32_fp.h.
 */
__asm__(
    ".section .text.entry, \"ax\", @progbits\n"
    ".globl _fp_table\n"
    "_fp_table:\n"
    "    j fp_add\n"
    "    j fp_sub\n"
    "    j fp_mul\n"
    "    j fp_div\n"
    "    j fp_neg\n"
    "    j fp_cmp\n"
    "    j fp_itof\n"
    "    j fp_ftoi\n"
);

float fp_add(float a, float b) { return a + b; }
float fp_sub(float a, float b) { return a - b; }
float fp_mul(float a, float b) { return a * b; }
float fp_div(float a, float b) { return a / b; }
float fp_neg(float a)          { return -a; }

int fp_cmp(float a, float b) {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

float fp_itof(int w) { return (float)w; }
int   fp_ftoi(float a) { return (int)a; }
