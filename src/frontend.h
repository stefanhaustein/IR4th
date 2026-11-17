/* frontend.h - IR4th parser, type-checker, and compiler driver API (Strict C89)
 *
 * Compiles an .ir4 source stream in two passes (parse/type-check into IR,
 * reachability-based dead-code elimination, and emission via Backend).
 */
#ifndef FRONTEND_H
#define FRONTEND_H

#include "ir4.h"

void ir4_set_exe_path(const char *exe_path);
void ir4_add_lib_root(const char *path);
void ir4_compile_stream(FILE *in, const char *path,
                        const Backend *target_be,
                        const char *platform);
int ir4_find_func(const char *name);
u16 ir4_func_addr(u8 idx);
const FuncSig *ir4_func_sig(u8 idx);

#endif /* FRONTEND_H */
