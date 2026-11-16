/* frontend.h - IR4th frontend entry points (Strict C89) */
#ifndef FRONTEND_H
#define FRONTEND_H

#include "ir4c.h"

void           ir4_compile_stream(FILE *in, const char *path,
                                  const Backend *target_be,
                                  const char *platform);
int            ir4_find_func(const char *name);
u16            ir4_func_addr(u8 idx);
const FuncSig *ir4_func_sig(u8 idx);

#endif /* FRONTEND_H */
