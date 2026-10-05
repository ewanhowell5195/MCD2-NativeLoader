#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"

#define UOBJECT_CLASS 0x10
#define UOBJECT_NAME 0x18
#define UOBJECT_OUTER 0x20
#define UFUNCTION_FUNC 0xD8

#define GOBJ_OBJECTS 0x00
#define GOBJ_MAX_ELEMS 0x10
#define GOBJ_NUM_ELEMS 0x14
#define GOBJ_MAX_CHUNKS 0x18
#define GOBJ_NUM_CHUNKS 0x1C
#define ITEM_STRIDE 24
#define CHUNK_ELEMS 65536

#define GNATIVES_COUNT 255

void **g_gnatives;

static uint8_t *g_base;
static size_t g_image_size;
static uint64_t g_text_lo, g_text_hi;
static uint64_t g_blocks, g_gobj, g_objects_arr;
static uint64_t g_cache_lo, g_cache_hi;

static int region_ok(const MEMORY_BASIC_INFORMATION *mbi) {
  if (mbi->State != MEM_COMMIT) return 0;
  DWORD prot = mbi->Protect & 0xff;
  if (mbi->Protect & PAGE_GUARD) return 0;
  return prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
}

static int readable(const void *p, size_t n) {
  uint64_t a = (uint64_t)p, end = a + n;
  if (end < a) return 0;
  if (a >= g_cache_lo && end <= g_cache_hi && g_cache_hi) return 1;
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery((void *)a, &mbi, sizeof(mbi)) || !region_ok(&mbi)) return 0;
  uint64_t rlo = (uint64_t)mbi.BaseAddress, rhi = rlo + mbi.RegionSize;
  if (end <= rhi) {
    g_cache_lo = rlo;
    g_cache_hi = rhi;
    return 1;
  }
  uint64_t cur = rhi;
  while (cur < end) {
    if (!VirtualQuery((void *)cur, &mbi, sizeof(mbi)) || !region_ok(&mbi)) return 0;
    cur = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
  }
  return 1;
}

static uint64_t rd64(uint64_t a) { return readable((void *)a, 8) ? *(uint64_t *)a : 0; }
static uint32_t rd32(uint64_t a) { return readable((void *)a, 4) ? *(uint32_t *)a : 0; }
static int in_image(uint64_t p) { return p >= (uint64_t)g_base && p < (uint64_t)g_base + g_image_size; }
static int in_text(uint64_t p) { return p >= g_text_lo && p < g_text_hi; }
static int plausible_ptr(uint64_t q) { return q >= 0x10000 && q < 0x7fffffffffffULL && (q & 7) == 0; }

static int section_range(const char *name, uint64_t *lo, uint64_t *hi) {
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
  IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
  IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
  for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
    if (memcmp(sec[i].Name, name, strlen(name)) == 0) {
      *lo = (uint64_t)g_base + sec[i].VirtualAddress;
      *hi = *lo + sec[i].Misc.VirtualSize;
      return 1;
    }
  }
  return 0;
}

void engine_init(void) {
  g_base = (uint8_t *)GetModuleHandleW(NULL);
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
  IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
  g_image_size = nt->OptionalHeader.SizeOfImage;
  section_range(".text", &g_text_lo, &g_text_hi);
}

static int resolve_name(uint32_t id, char *out, int cb) {
  out[0] = 0;
  uint64_t blk = rd64(g_blocks + (uint64_t)(id >> 16) * 8);
  if (!blk) return 0;
  uint64_t entry = blk + (uint64_t)(id & 0xffff) * 2;
  if (!readable((void *)entry, 2)) return 0;
  uint16_t h = *(uint16_t *)entry;
  int wide = h & 1, len = h >> 6;
  if (len <= 0 || len > 1024) return 0;
  if (wide) {
    if (!readable((void *)(entry + 2), (size_t)len * 2)) return 0;
    const wchar_t *w = (const wchar_t *)(entry + 2);
    int nn = len < cb - 1 ? len : cb - 1;
    for (int i = 0; i < nn; i++) out[i] = (char)w[i];
    out[nn] = 0;
  } else {
    if (!readable((void *)(entry + 2), (size_t)len) || len >= cb) return 0;
    memcpy(out, (void *)(entry + 2), len);
    out[len] = 0;
  }
  return 1;
}

static int obj_name(uint64_t obj, char *out, int cb) {
  if (!obj || !readable((void *)(obj + UOBJECT_NAME), 4)) {
    out[0] = 0;
    return 0;
  }
  return resolve_name(*(uint32_t *)(obj + UOBJECT_NAME), out, cb);
}

static int valid_gobjects(uint64_t a) {
  uint64_t objects = rd64(a + GOBJ_OBJECTS);
  if (!plausible_ptr(objects)) return 0;
  int32_t max_elems = (int32_t)rd32(a + GOBJ_MAX_ELEMS), num_elems = (int32_t)rd32(a + GOBJ_NUM_ELEMS);
  int32_t max_chunks = (int32_t)rd32(a + GOBJ_MAX_CHUNKS), num_chunks = (int32_t)rd32(a + GOBJ_NUM_CHUNKS);
  if (max_elems < 1024 || max_elems > 0x4000000) return 0;
  if (num_elems < 1 || num_elems > max_elems) return 0;
  if (max_chunks != (max_elems + CHUNK_ELEMS - 1) / CHUNK_ELEMS) return 0;
  int32_t need = (num_elems + CHUNK_ELEMS - 1) / CHUNK_ELEMS;
  if (num_chunks < need || num_chunks > max_chunks) return 0;
  uint64_t chunk0 = rd64(objects);
  if (!chunk0 || !readable((void *)chunk0, ITEM_STRIDE)) return 0;
  uint64_t obj0 = rd64(chunk0);
  if (!obj0 || !readable((void *)obj0, 0x30)) return 0;
  return in_image(rd64(obj0));
}

static int is_none_block(uint64_t p) {
  if (!readable((void *)p, 8)) return 0;
  uint16_t h = *(uint16_t *)p;
  if ((h & 1) || (h >> 6) != 4) return 0;
  if (memcmp((void *)(p + 2), "None", 4) != 0) return 0;
  uint64_t next = p + 6;
  if (!readable((void *)next, 2)) return 0;
  uint16_t h2 = *(uint16_t *)next;
  int len2 = h2 >> 6;
  if ((h2 & 1) || len2 < 1 || len2 > 64) return 0;
  if (!readable((void *)(next + 2), 1)) return 0;
  char c = *(char *)(next + 2);
  return c >= 'A' && c <= 'z';
}

static int is_gnatives(uint64_t p) {
  if (!readable((void *)p, GNATIVES_COUNT * 8)) return 0;
  uint64_t *v = (uint64_t *)p;
  int adj = 0;
  for (int i = 0; i < GNATIVES_COUNT; i++) {
    if (!in_text(v[i])) return 0;
    if (i > 0 && v[i] == v[i - 1]) adj++;
  }
  return adj >= 10;
}

static uint64_t obj_at(int32_t i) {
  uint64_t chunk = rd64(g_objects_arr + (uint64_t)(i / CHUNK_ELEMS) * 8);
  return chunk ? rd64(chunk + (uint64_t)(i % CHUNK_ELEMS) * ITEM_STRIDE) : 0;
}

int engine_find_globals(void) {
  uint64_t ds, de;
  if (!section_range(".data", &ds, &de)) return 0;

  uint64_t gobj = 0, blocks = 0, gnat = 0;
  MEMORY_BASIC_INFORMATION mbi;
  __try {
    for (uint64_t a = ds; a < de;) {
      if (!VirtualQuery((void *)a, &mbi, sizeof(mbi))) break;
      uint64_t rend = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
      if (rend > de) rend = de;
      DWORD prot = mbi.Protect & 0xff;
      if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && (prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE)) {
        for (uint64_t p = (a > (uint64_t)mbi.BaseAddress ? a : (uint64_t)mbi.BaseAddress); p + 0x20 <= rend; p += 8) {
          if (!gobj && valid_gobjects(p)) gobj = p;
          uint64_t q = *(uint64_t *)p;
          if (!blocks && plausible_ptr(q) && is_none_block(q)) blocks = p;
          if (!gnat && in_text(q) && is_gnatives(p)) gnat = p;
          if (gobj && blocks && gnat) break;
        }
      }
      a = rend > a ? rend : a + mbi.RegionSize;
      if (gobj && blocks && gnat) break;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }

  if (!gobj || !blocks || !gnat) return 0;
  g_blocks = blocks;
  g_gobj = gobj;
  g_objects_arr = rd64(gobj + GOBJ_OBJECTS);
  g_gnatives = (void **)gnat;
  return 1;
}

uint64_t engine_find_function(const char *outer, const char *name) {
  char cls[128], nm[128], ou[128];
  int32_t count = (int32_t)rd32(g_gobj + GOBJ_NUM_ELEMS);
  for (int32_t i = 0; i < count; i++) {
    uint64_t o = obj_at(i);
    if (!o) continue;
    uint64_t c = rd64(o + UOBJECT_CLASS);
    if (!obj_name(c, cls, sizeof(cls)) || strcmp(cls, "Function") != 0) continue;
    if (!obj_name(o, nm, sizeof(nm)) || strcmp(nm, name) != 0) continue;
    uint64_t pu = rd64(o + UOBJECT_OUTER);
    if (!obj_name(pu, ou, sizeof(ou)) || strcmp(ou, outer) != 0) continue;
    return o;
  }
  return 0;
}

int engine_swap_func(uint64_t uf, NativeFn thunk, NativeFn *orig_out) {
  void **slot = (void **)(uf + UFUNCTION_FUNC);
  if (!in_image((uint64_t)*slot)) return 0;
  *orig_out = (NativeFn)*slot;
  *slot = (void *)thunk;
  return 1;
}

static void assign_fstring(const wchar_t *text, FStr *out) {
  uint8_t fr[0x40];
  memset(fr, 0, sizeof(fr));
  *(uint64_t *)(fr + FRAME_CODE) = (uint64_t)text;
  ((NativeFn)g_gnatives[EX_UNICODESTRINGCONST])(NULL, fr, out);
}

void engine_make_fstring(const char *utf8, FStr *out) {
  out->data = NULL;
  out->num = 0;
  out->max = 0;
  int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
  wchar_t *tmp = malloc((size_t)wlen * 2);
  MultiByteToWideChar(CP_UTF8, 0, utf8, -1, tmp, wlen);
  assign_fstring(tmp, out);
  free(tmp);
}

void engine_free_fstring(FStr *s) {
  if (s->data) assign_fstring(L"", s);
}
