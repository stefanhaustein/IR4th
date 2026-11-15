/* ir4c.c - CLI driver for IR4th (Strict C89) */
#include "ir4c.h"

static void print_usage(void) {
    fprintf(stderr, "Usage: ir4c [-t z80|6502|arm64] [-b hex_addr] [-o out.bin] [--run] [--cpm] <input.ir4>\n");
}

int main(int argc, char **argv) {
    const Backend *be = &be_z80;
    const char *in_path = NULL;
    const char *out_path = "out.bin";
    u16 base = 0x0100;
    int base_set = 0;
    int run_jit = 0;
    int cpm_stub = 0;
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
        } else if (strcmp(argv[i], "--cpm") == 0) {
            cpm_stub = 1;
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

    if (cpm_stub && be != &be_z80) {
        fprintf(stderr, "--cpm requires -t z80\n");
        fclose(in_fp);
        return 1;
    }

    cb_reset(base);
    if (cpm_stub) {
        be_z80_cpm_begin();
    }
    ir4_compile_stream(in_fp, in_path, be);
    fclose(in_fp);

    if (cpm_stub) {
        int main_idx = ir4_find_func("main");
        if (main_idx < 0) {
            fprintf(stderr, "--cpm requires a 'main' function\n");
            return 1;
        }
        be_z80_cpm_end((u8)main_idx);
    }

    /* Place data segment right after code on 16-bit targets */
    if (be != &be_arm64 && cb_data_size() > 0) {
        cb_patch_data_relocs(cb_cur_addr());
    }

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
        int main_idx = ir4_find_func("main");
        if (be != &be_arm64) {
            fprintf(stderr, "--run requires -t arm64\n");
            return 1;
        }
        if (main_idx < 0) {
            fprintf(stderr, "--run requires a 'main' function\n");
            return 1;
        }
        be_arm64_run_jit((u8)main_idx);
    }

    return 0;
}
