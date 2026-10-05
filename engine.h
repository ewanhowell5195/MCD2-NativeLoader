#pragma once
#include <stddef.h>
#include <stdint.h>

#define FRAME_OBJECT 0x18
#define FRAME_CODE 0x20
#define EX_ENDFUNCTIONPARMS 0x16
#define EX_UNICODESTRINGCONST 0x34

typedef void(__cdecl *NativeFn)(void *ctx, void *frame, void *result);
typedef struct { wchar_t *data; int32_t num; int32_t max; } FStr;

extern void **g_gnatives;

void engine_init(void);
int engine_find_globals(void);
uint64_t engine_find_function(const char *outer, const char *name);
int engine_swap_func(uint64_t uf, NativeFn thunk, NativeFn *orig_out);
void engine_make_fstring(const char *utf8, FStr *out);
void engine_free_fstring(FStr *s);
