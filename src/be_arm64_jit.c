/* be_arm64_jit.c - Apple Silicon macOS MAP_JIT Runner for IR4th (Strict C89) */
#include "ir4c.h"

#if defined(__APPLE__) && defined(__aarch64__)
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>

/* Tiny ARM64 C-to-IR4th wrapper emitted at the end of code_buf:
 *   STP X29, X30, [SP, #-16]!
 *   STR X28, [SP, #-16]!
 *   MOV X28, X0         (data_base passed as arg0)
 *   BL  <main_addr>
 *   LDR X0, [SP], #16   (or MOV X0, #0 if main returns void)
 *   LDR X28, [SP], #16
 *   LDP X29, X30, [SP], #16
 *   RET
 */
void be_arm64_run_jit(u8 main_idx) {
    const FuncSig *sig = ir4_func_sig(main_idx);
    u16 main_addr = sig->addr;
    u16 entry_pc = cb_pc();
    i32 diff;
    u32 imm26;
    void *mem;
    void *data_mem = NULL;
    long (*jit_fn)(void *);
    long result;

    if (sig->in_count != 0 || sig->out_count > 1) {
        fprintf(stderr, "--run requires 'main' with 0 inputs and at most 1 output\n");
        exit(1);
    }

    /* Emit C-ABI trampoline calling IR4th 'main' and popping result into X0 */
    emit16(0x7BFD); emit16(0xA9BF); /* STP X29, X30, [SP, #-16]! */
    emit16(0x0FFC); emit16(0xF81F); /* STR X28, [SP, #-16]! */
    emit16(0x03FC); emit16(0xAA00); /* MOV X28, X0 */
    diff = (i32)main_addr - (i32)cb_cur_addr();
    imm26 = ((u32)(diff >> 2)) & 0x03FFFFFFUL;
    emit16((u16)((0x94000000UL | imm26) & 0xFFFFU));
    emit16((u16)(((0x94000000UL | imm26) >> 16) & 0xFFFFU));
    if (sig->out_count == 1) {
        emit16(0x07E0); emit16(0xF841); /* LDR X0, [SP], #16 */
    } else {
        emit16(0x0000); emit16(0xD280); /* MOV X0, #0 */
    }
    emit16(0x07FC); emit16(0xF841); /* LDR X28, [SP], #16 */
    emit16(0x7BFD); emit16(0xA8C1); /* LDP X29, X30, [SP], #16 */
    emit16(0x03C0); emit16(0xD65F); /* RET */

    mem = mmap(NULL, MAX_CODE, PROT_READ | PROT_WRITE | PROT_EXEC,
               MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap MAP_JIT");
        exit(1);
    }

    if (cb_data_size() > 0) {
        data_mem = calloc(1, cb_data_size());
        if (!data_mem) {
            perror("calloc data");
            munmap(mem, MAX_CODE);
            exit(1);
        }
    }

    pthread_jit_write_protect_np(0);
    memcpy(mem, cb_data(), cb_pc());
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(mem, cb_pc());

    jit_fn = (long (*)(void *))((u8 *)mem + entry_pc);
    result = jit_fn(data_mem);
    printf("JIT main() returned: %ld\n", result);

    free(data_mem);
    munmap(mem, MAX_CODE);
}
#else
void be_arm64_run_jit(u8 main_idx) {
    (void)main_idx;
    fprintf(stderr, "--run is only supported on Apple Silicon macOS\n");
    exit(1);
}
#endif
