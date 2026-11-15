/* ir4c.c - CLI driver & macOS ARM64 JIT runner for IR4th (Strict C89) */
#include "ir4c.h"

#if defined(__APPLE__) && defined(__aarch64__)
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#define HAS_ARM64_JIT 1
#endif

static void print_usage(void) {
    fprintf(stderr, "Usage: ir4c [-t z80|6502|arm64] [-b hex_addr] [-o out.bin] [--run] <input.ir4>\n");
}

#ifdef HAS_ARM64_JIT
/* Tiny ARM64 C-to-IR4th wrapper emitted at the end of code_buf:
 *   STP X29, X30, [SP, #-16]!
 *   BL  <main_addr>
 *   LDR X0, [SP], #16   (or MOV X0, #0 if main returns void)
 *   LDP X29, X30, [SP], #16
 *   RET
 */
static void run_arm64_jit(u8 main_idx) {
    const FuncSig *sig = ir4_func_sig(main_idx);
    u16 main_addr = sig->addr;
    u16 entry_pc = cb_pc();
    i32 diff;
    u32 imm26;
    void *mem;
    long (*jit_fn)(void);
    long result;

    if (sig->in_count != 0 || sig->out_count > 1) {
        fprintf(stderr, "--run requires 'main' with 0 inputs and at most 1 output\n");
        exit(1);
    }

    /* Emit C-ABI trampoline calling IR4th 'main' and popping result into X0 */
    emit16(0x7BFD); emit16(0xA9BF); /* STP X29, X30, [SP, #-16]! */
    diff = (i32)main_addr - (i32)cb_cur_addr();
    imm26 = ((u32)(diff >> 2)) & 0x03FFFFFFUL;
    emit16((u16)((0x94000000UL | imm26) & 0xFFFFU));
    emit16((u16)(((0x94000000UL | imm26) >> 16) & 0xFFFFU));
    if (sig->out_count == 1) {
        emit16(0x07E0); emit16(0xF841); /* LDR X0, [SP], #16 */
    } else {
        emit16(0x0000); emit16(0xD280); /* MOV X0, #0 */
    }
    emit16(0x7BFD); emit16(0xA8C1); /* LDP X29, X30, [SP], #16 */
    emit16(0x03C0); emit16(0xD65F); /* RET */

    mem = mmap(NULL, MAX_CODE, PROT_READ | PROT_WRITE | PROT_EXEC,
               MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap MAP_JIT");
        exit(1);
    }

    pthread_jit_write_protect_np(0);
    memcpy(mem, cb_data(), cb_pc());
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(mem, cb_pc());

    jit_fn = (long (*)(void))((u8 *)mem + entry_pc);
    result = jit_fn();
    printf("JIT main() returned: %ld\n", result);

    munmap(mem, MAX_CODE);
}
#endif

int main(int argc, char **argv) {
    const Backend *be = &be_z80;
    const char *in_path = NULL;
    const char *out_path = "out.bin";
    u16 base = 0x0100;
    int base_set = 0;
    int run_jit = 0;
    int i;
    FILE *in_fp;
    FILE *out_fp;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "z80") == 0) {
                be = &be_z80;
                if (!base_set) base = 0x0100;
            } else if (strcmp(argv[i], "6502") == 0) {
                be = &be_6502;
                if (!base_set) base = 0x0801;
            } else if (strcmp(argv[i], "arm64") == 0) {
                be = &be_arm64;
                if (!base_set) base = 0x0000;
            } else {
                fprintf(stderr, "Unknown target: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            i++;
            base = (u16)strtoul(argv[i], NULL, 0);
            base_set = 1;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            i++;
            out_path = argv[i];
        } else if (strcmp(argv[i], "--run") == 0) {
            run_jit = 1;
        } else if (argv[i][0] == '-') {
            print_usage();
            return 1;
        } else {
            in_path = argv[i];
        }
    }

    if (!in_path) {
        print_usage();
        return 1;
    }

    in_fp = fopen(in_path, "r");
    if (!in_fp) {
        perror("fopen input");
        return 1;
    }

    cb_reset(base);
    ir4_compile_stream(in_fp, be);
    fclose(in_fp);

    out_fp = fopen(out_path, "wb");
    if (!out_fp) {
        perror("fopen output");
        return 1;
    }

    if (cb_pc() > 0) {
        if (fwrite(cb_data(), 1, cb_pc(), out_fp) != (size_t)cb_pc()) {
            perror("fwrite");
            fclose(out_fp);
            return 1;
        }
    }
    fclose(out_fp);

    printf("Compiled %s -> %s (target: %s, base: 0x%04X, size: %u bytes)\n",
           in_path, out_path, be->name, base, cb_pc());

    if (run_jit) {
#ifdef HAS_ARM64_JIT
        int main_idx = ir4_find_func("main");
        if (be != &be_arm64) {
            fprintf(stderr, "--run requires -t arm64\n");
            return 1;
        }
        if (main_idx < 0) {
            fprintf(stderr, "--run requires a 'main' function\n");
            return 1;
        }
        run_arm64_jit((u8)main_idx);
#else
        fprintf(stderr, "--run is only supported on Apple Silicon macOS\n");
        return 1;
#endif
    }

    return 0;
}
