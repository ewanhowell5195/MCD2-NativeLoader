#include <windows.h>
#include <wchar.h>
#include "winmm_exports.h"
#include "loader.h"

#define WINMM_NAME(name, ordinal) #name,
#define WINMM_EXPORT(name, ordinal) __pragma(comment(linker, "/EXPORT:" #name ",@" #ordinal))

static const char *const WINMM_NAMES[] = { WINMM_EXPORTS(WINMM_NAME) };
WINMM_EXPORTS(WINMM_EXPORT)

void *g_winmm[ARRAYSIZE(WINMM_NAMES)];

int load_genuine_winmm(void) {
  wchar_t path[MAX_PATH];
  GetSystemDirectoryW(path, MAX_PATH);
  wcscat_s(path, MAX_PATH, L"\\winmm.dll");
  HMODULE h = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!h) return 0;
  for (int i = 0; i < ARRAYSIZE(WINMM_NAMES); i++) g_winmm[i] = (void *)GetProcAddress(h, WINMM_NAMES[i]);
  return 1;
}
