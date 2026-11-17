/* ir4c.c - CLI driver for IR4th (Strict C89) */
#include "codebuf.h"
#include "frontend.h"

static const Platform *const platforms[] = {
    &plat_z80_cpm,
    &plat_6502_sim,
    &plat_6502_c64,
    &plat_arm64_jit,
    &plat_rv32_virt,
    &plat_rv32_obj,
    NULL
};

static void print_usage(void) {
    fprintf(stderr, "Usage: ir4c [-t z80|6502|arm64|rv32] [-b hex_addr] [-o out.bin] [-I dir] [--platform name] [--zp hex_addr] [--stack-zp hex_addr] [--no-stack-zp] [--run] [--cpm] [--sim65] [--c64] [--virt] [--obj] <input.ir4>\n");
}

int main(int argc, char **argv) {
    const Backend *be = &be_z80;
    const Platform *plat = NULL;
    const char *in_path = NULL;
    const char *out_path = "out.bin";
    const char *platform = NULL;
    u16 base = 0x0100;
    int target_set = 0;
    int base_set = 0;
    int i;
    int j;
    FILE *in_fp;
    FILE *out_fp;

    if (argc > 0 && argv[0]) ir4_set_exe_path(argv[0]);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            i++;
            target_set = 1;
            if (strcmp(argv[i], "z80") == 0) {
                be = &be_z80;
                if (!base_set) base = 0x0100;
            } else if (strcmp(argv[i], "6502") == 0) {
                be = &be_6502;
                if (!base_set) base = 0x0801;
            } else if (strcmp(argv[i], "arm64") == 0) {
                be = &be_arm64;
                if (!base_set) base = 0x0000;
            } else if (strcmp(argv[i], "rv32") == 0) {
                be = &be_rv32;
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
        } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            i++;
            ir4_add_lib_root(argv[i]);
        } else if (strncmp(argv[i], "-I", 2) == 0 && argv[i][2] != '\0') {
            ir4_add_lib_root(argv[i] + 2);
        } else if (strcmp(argv[i], "--platform") == 0 && i + 1 < argc) {
            i++;
            platform = argv[i];
        } else if (strcmp(argv[i], "--zp") == 0 && i + 1 < argc) {
            i++;
            be_6502_set_zp((u8)strtoul(argv[i], NULL, 0));
        } else if (strcmp(argv[i], "--stack-zp") == 0 && i + 1 < argc) {
            const char *arg;
            char *colon;
            u8 zp_base;
            u8 zp_init = 0;
            i++;
            arg = argv[i];
            zp_base = (u8)strtoul(arg, &colon, 0);
            if (colon && *colon == ':') {
                zp_init = (u8)strtoul(colon + 1, NULL, 0);
            }
            be_6502_set_stack_zp(zp_base, zp_init);
        } else if (strcmp(argv[i], "--no-stack-zp") == 0) {
            be_6502_set_stack_zp(0, 0);
        } else if (argv[i][0] == '-') {
            const Platform *matched = NULL;
            for (j = 0; platforms[j]; j++) {
                if (strcmp(argv[i], platforms[j]->flag) == 0) {
                    matched = platforms[j];
                    break;
                }
            }
            if (!matched) {
                print_usage();
                return 1;
            }
            if (plat != NULL) {
                fprintf(stderr, "%s and %s cannot be combined\n", plat->flag, matched->flag);
                return 1;
            }
            plat = matched;
            if (!platform && plat->req_platform) {
                platform = plat->req_platform;
            }
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

    if (plat) {
        if (!target_set) {
            be = plat->be;
        } else if (be != plat->be) {
            fprintf(stderr, "%s requires -t %s\n", plat->flag, plat->be->name);
            fclose(in_fp);
            return 1;
        }
        if (!base_set) {
            base = plat->default_base;
        }
    }

    cb_reset(base);
    be_rv32_set_obj(plat == &plat_rv32_obj);
    if (plat && plat->begin) {
        plat->begin(base);
    }
    ir4_compile_stream(in_fp, in_path, be, platform);
    fclose(in_fp);

    if (plat && plat->end) {
        int main_idx = ir4_find_func("main");
        if (plat->needs_main && main_idx < 0) {
            fprintf(stderr, "%s requires a 'main' function\n", plat->flag);
            return 1;
        }
        plat->end(main_idx);
    }

    /* Place data segment right after code on 16-bit targets */
    if (be != &be_arm64 && be != &be_rv32 && cb_data_size() > 0) {
        cb_patch_data_relocs(cb_cur_addr());
    }

    out_fp = fopen(out_path, "wb");
    if (!out_fp) {
        perror("fopen output");
        return 1;
    }

    if (plat && plat->write) {
        if (!plat->write(out_fp, base)) {
            perror("fwrite");
            fclose(out_fp);
            return 1;
        }
    } else if (!cb_write_raw(out_fp, be != &be_arm64)) {
        perror("fwrite");
        fclose(out_fp);
        return 1;
    }
    fclose(out_fp);

    printf("Compiled %s -> %s (target: %s, base: 0x%04X, size: %u bytes)\n",
           in_path, out_path, be->name, base, cb_pc());

    if (plat && plat->run) {
        int main_idx = ir4_find_func("main");
        if (main_idx < 0) {
            fprintf(stderr, "%s requires a 'main' function\n", plat->flag);
            return 1;
        }
        plat->run((u8)main_idx);
    }

    return 0;
}
