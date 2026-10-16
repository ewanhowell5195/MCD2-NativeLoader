#include <windows.h>
#include <string.h>
#include "loader.h"

typedef VOID (WINAPI *ExitProcessFn)(UINT);

static ExitProcessFn g_exit_process;

static VOID WINAPI final_exit(UINT code) {
  HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
  BYTE *debug_output = kernelbase ? (BYTE *)GetProcAddress(kernelbase, "OutputDebugStringA") : NULL;
  DWORD old;
  if (debug_output && VirtualProtect(debug_output, 1, PAGE_EXECUTE_READWRITE, &old)) {
    *debug_output = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), debug_output, 1);
    VirtualProtect(debug_output, 1, old, &old);
  }
  g_exit_process(code);
}

void exit_fix_install(void) {
  BYTE *ucrt = (BYTE *)GetModuleHandleW(L"ucrtbase.dll");
  if (!ucrt) return;
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(ucrt + ((IMAGE_DOS_HEADER *)ucrt)->e_lfanew);
  IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return;
  for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(ucrt + dir.VirtualAddress); d->Name; d++) {
    if (!d->OriginalFirstThunk) continue;
    IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(ucrt + d->OriginalFirstThunk);
    IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(ucrt + d->FirstThunk);
    for (; names->u1.AddressOfData; names++, slots++) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      IMAGE_IMPORT_BY_NAME *name = (IMAGE_IMPORT_BY_NAME *)(ucrt + names->u1.AddressOfData);
      if (strcmp((const char *)name->Name, "ExitProcess") != 0) continue;
      DWORD old;
      if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old)) return;
      g_exit_process = (ExitProcessFn)slots->u1.Function;
      InterlockedExchangePointer((PVOID *)&slots->u1.Function, (PVOID)final_exit);
      VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
      return;
    }
  }
}
