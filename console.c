#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <commdlg.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "loader.h"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#define COL_BG RGB(30, 30, 30)
#define COL_BAR RGB(37, 37, 38)
#define COL_TEXT RGB(212, 212, 212)
#define COL_DIM RGB(150, 150, 150)
#define COL_BTN RGB(58, 58, 58)
#define COL_BTN_DOWN RGB(82, 82, 82)
#define COL_BORDER RGB(70, 70, 70)

#define BAR_H 34
#define BTN_W 80
#define BTN_H 24
#define BTN_GAP 6

#define WM_APP_FLUSH (WM_APP + 1)
#define ID_EDIT 100
#define ID_COPY 101
#define ID_SAVE 102
#define TIMER_TICK 1
#define STATUS_MS 5000
#define RAISE_WATCH_TICKS 300
#define READY_TIMEOUT_MS 5000

enum { CONSOLE_NONE, CONSOLE_OPEN, CONSOLE_CLOSED };

static SRWLOCK g_lock = SRWLOCK_INIT;
static int g_state = CONSOLE_NONE;
static HWND g_wnd = NULL;
static wchar_t *g_pending = NULL;
static size_t g_pending_len = 0;
static size_t g_pending_cap = 0;
static int g_flush_posted = 0;
static wchar_t g_title[MAX_PATH] = L"";

static HWND g_edit = NULL;
static HWND g_copy_btn = NULL;
static HWND g_save_btn = NULL;
static WNDPROC g_edit_proc = NULL;
static HFONT g_mono_font = NULL;
static HFONT g_ui_font = NULL;
static HBRUSH g_bg_brush = NULL;
static HBRUSH g_bar_brush = NULL;
static UINT g_dpi = 96;
static wchar_t g_status[MAX_PATH * 2] = L"";
static ULONGLONG g_status_until = 0;

static int g_watch_ticks = 0;
static int g_stable = 0;
static HWND g_last_fg = NULL;
static RECT g_last_rect = { 0 };
static int g_game_seen = 0;
static int g_game_gone = 0;

static void drop_pending(void) {
  free(g_pending);
  g_pending = NULL;
  g_pending_len = 0;
  g_pending_cap = 0;
}

void console_write(const wchar_t *prefix, const char *msg) {
  size_t len = strlen(msg);
  while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r')) len--;
  int wlen = len > 0 ? MultiByteToWideChar(CP_UTF8, 0, msg, (int)len, NULL, 0) : 0;
  size_t plen = prefix ? wcslen(prefix) : 0;
  size_t need = (prefix ? plen + 3 : 0) + (size_t)wlen + 2;

  AcquireSRWLockExclusive(&g_lock);
  if (g_state == CONSOLE_OPEN) {
    if (g_pending_len + need + 1 > g_pending_cap) {
      size_t cap = (g_pending_len + need + 1) * 2;
      wchar_t *grown = realloc(g_pending, cap * sizeof(wchar_t));
      if (!grown) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
      }
      g_pending = grown;
      g_pending_cap = cap;
    }
    wchar_t *p = g_pending + g_pending_len;
    size_t n = 0;
    if (prefix) {
      p[n++] = L'[';
      wmemcpy(p + n, prefix, plen);
      n += plen;
      p[n++] = L']';
      p[n++] = L' ';
    }
    if (wlen > 0) n += MultiByteToWideChar(CP_UTF8, 0, msg, (int)len, p + n, wlen);
    p[n++] = L'\r';
    p[n++] = L'\n';
    g_pending_len += n;
    g_pending[g_pending_len] = L'\0';
    if (g_wnd && !g_flush_posted && PostMessageW(g_wnd, WM_APP_FLUSH, 0, 0)) g_flush_posted = 1;
  }
  ReleaseSRWLockExclusive(&g_lock);
}

void nl_log(const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
  va_end(ap);
  console_write(L"NativeLoader", msg);
}

static int px(int v) {
  return MulDiv(v, (int)g_dpi, 96);
}

static void create_fonts(void) {
  if (g_mono_font) DeleteObject(g_mono_font);
  if (g_ui_font) DeleteObject(g_ui_font);
  g_mono_font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
  g_ui_font = CreateFontW(-px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

static void layout(HWND hwnd) {
  RECT rc;
  GetClientRect(hwnd, &rc);
  int bar = px(BAR_H);
  int bw = px(BTN_W);
  int bh = px(BTN_H);
  int gap = px(BTN_GAP);
  int y = (bar - bh) / 2;
  SetWindowPos(g_save_btn, NULL, rc.right - gap - bw, y, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
  SetWindowPos(g_copy_btn, NULL, rc.right - 2 * gap - 2 * bw, y, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
  SetWindowPos(g_edit, NULL, 0, bar + 1, rc.right, rc.bottom - bar - 1 > 0 ? rc.bottom - bar - 1 : 0, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void apply_fonts(void) {
  SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_mono_font, TRUE);
  SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(px(6), px(6)));
  SendMessageW(g_copy_btn, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
  SendMessageW(g_save_btn, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
}

static void invalidate_bar(HWND hwnd) {
  RECT rc;
  GetClientRect(hwnd, &rc);
  rc.bottom = px(BAR_H) + 1;
  InvalidateRect(hwnd, &rc, FALSE);
}

static void set_status(HWND hwnd, const wchar_t *text) {
  wcscpy_s(g_status, sizeof(g_status) / sizeof(g_status[0]), text);
  g_status_until = GetTickCount64() + STATUS_MS;
  invalidate_bar(hwnd);
}

static void format_elapsed(wchar_t *out, size_t cch) {
  ULONGLONG s = (GetTickCount64() - g_start_tick) / 1000;
  if (s >= 3600) swprintf_s(out, cch, L"Elapsed %llu:%02llu:%02llu", s / 3600, s / 60 % 60, s % 60);
  else swprintf_s(out, cch, L"Elapsed %llu:%02llu", s / 60, s % 60);
}

static int view_at_bottom(void) {
  SCROLLINFO si;
  si.cbSize = sizeof(si);
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
  if (!GetScrollInfo(g_edit, SB_VERT, &si) || si.nPage == 0) return 1;
  return si.nPos + (int)si.nPage >= si.nMax;
}

static void append_to_edit(const wchar_t *text) {
  int len = GetWindowTextLengthW(g_edit);
  DWORD sel_start = 0;
  DWORD sel_end = 0;
  SendMessageW(g_edit, EM_GETSEL, (WPARAM)&sel_start, (LPARAM)&sel_end);
  int first = (int)SendMessageW(g_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
  int follow = view_at_bottom();

  SendMessageW(g_edit, WM_SETREDRAW, FALSE, 0);
  SendMessageW(g_edit, EM_SETSEL, len, len);
  SendMessageW(g_edit, EM_REPLACESEL, FALSE, (LPARAM)text);
  SendMessageW(g_edit, EM_SETSEL, sel_start, sel_end);
  if (!follow) {
    int now = (int)SendMessageW(g_edit, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageW(g_edit, EM_LINESCROLL, 0, first - now);
  }
  SendMessageW(g_edit, WM_SETREDRAW, TRUE, 0);
  if (follow) SendMessageW(g_edit, WM_VSCROLL, SB_BOTTOM, 0);
  InvalidateRect(g_edit, NULL, TRUE);
}

static void flush_pending(void) {
  AcquireSRWLockExclusive(&g_lock);
  wchar_t *text = g_pending;
  g_pending = NULL;
  g_pending_len = 0;
  g_pending_cap = 0;
  g_flush_posted = 0;
  ReleaseSRWLockExclusive(&g_lock);
  if (text) {
    append_to_edit(text);
    free(text);
  }
}

static wchar_t *edit_text(int *out_len) {
  int len = GetWindowTextLengthW(g_edit);
  wchar_t *text = malloc((len + 1) * sizeof(wchar_t));
  if (!text) return NULL;
  text[GetWindowTextW(g_edit, text, len + 1)] = L'\0';
  *out_len = (int)wcslen(text);
  return text;
}

static void copy_all(HWND hwnd) {
  int len = 0;
  wchar_t *text = edit_text(&len);
  if (!text) return;
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(wchar_t));
  int ok = 0;
  if (mem) {
    wchar_t *p = GlobalLock(mem);
    if (p) {
      wmemcpy(p, text, len + 1);
      GlobalUnlock(mem);
      if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        ok = SetClipboardData(CF_UNICODETEXT, mem) != NULL;
        CloseClipboard();
      }
    }
    if (!ok) GlobalFree(mem);
  }
  free(text);
  set_status(hwnd, ok ? L"Copied to the clipboard" : L"Copy FAILED");
}

static void save_output(HWND hwnd) {
  wchar_t dir[MAX_PATH] = L"";
  wchar_t path[MAX_PATH];
  SYSTEMTIME st;
  GetLocalTime(&st);
  swprintf_s(path, MAX_PATH, L"NativeLoader-console-%04u-%02u-%02u_%02u-%02u-%02u.txt", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  exe_dir(dir, MAX_PATH);

  OPENFILENAMEW ofn = { 0 };
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd;
  ofn.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
  ofn.lpstrFile = path;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrInitialDir = dir[0] ? dir : NULL;
  ofn.lpstrDefExt = L"txt";
  ofn.lpstrTitle = L"Save console output";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (!GetSaveFileNameW(&ofn)) {
    DWORD err = CommDlgExtendedError();
    if (err) {
      wchar_t status[64];
      swprintf_s(status, 64, L"Save FAILED, dialog error 0x%lx", err);
      set_status(hwnd, status);
    } else {
      set_status(hwnd, L"Save cancelled");
    }
    return;
  }
  const wchar_t *name = ofn.nFileOffset < wcslen(path) ? path + ofn.nFileOffset : path;

  int len = 0;
  wchar_t *text = edit_text(&len);
  if (!text) return;
  int cb = len > 0 ? WideCharToMultiByte(CP_UTF8, 0, text, len, NULL, 0, NULL, NULL) : 0;
  char *u8 = malloc(cb > 0 ? cb : 1);
  int ok = 0;
  if (u8) {
    if (cb > 0) WideCharToMultiByte(CP_UTF8, 0, text, len, u8, cb, NULL, NULL);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      ok = WriteFile(f, u8, (DWORD)cb, &written, NULL) && written == (DWORD)cb;
      CloseHandle(f);
    }
    free(u8);
  }
  free(text);

  wchar_t status[MAX_PATH];
  if (ok) swprintf_s(status, MAX_PATH, L"Saved %ls", name);
  else swprintf_s(status, MAX_PATH, L"Save FAILED, err=%lu", GetLastError());
  set_status(hwnd, status);
}

static int our_game_window(void) {
  HWND w = NULL;
  while ((w = FindWindowExW(NULL, w, L"UnrealWindow", NULL)) != NULL) {
    DWORD pid = 0;
    if (GetWindowThreadProcessId(w, &pid) && pid == GetCurrentProcessId()) return 1;
  }
  return 0;
}

static void check_game_alive(HWND hwnd) {
  if (our_game_window()) {
    g_game_seen = 1;
    g_game_gone = 0;
    return;
  }
  if (g_game_seen && ++g_game_gone >= 2) DestroyWindow(hwnd);
}

static void raise_above_game(HWND hwnd) {
  if (g_watch_ticks < 0) return;
  if (++g_watch_ticks > RAISE_WATCH_TICKS) {
    g_watch_ticks = -1;
    return;
  }
  HWND fg = GetForegroundWindow();
  DWORD pid = 0;
  wchar_t cls[32];
  RECT rect;
  if (!fg || !GetWindowThreadProcessId(fg, &pid) || pid != GetCurrentProcessId() || !GetClassNameW(fg, cls, 32) || wcscmp(cls, L"UnrealWindow") != 0 || !GetWindowRect(fg, &rect)) {
    g_stable = 0;
    return;
  }
  g_stable = fg == g_last_fg && EqualRect(&rect, &g_last_rect) ? g_stable + 1 : 0;
  g_last_fg = fg;
  g_last_rect = rect;
  if (g_stable >= 2) {
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    g_watch_ticks = -1;
  }
}

static void paint_bar(HWND hwnd, HDC dc) {
  RECT rc;
  GetClientRect(hwnd, &rc);
  RECT bar = rc;
  bar.bottom = px(BAR_H);
  FillRect(dc, &bar, g_bar_brush);
  RECT line = rc;
  line.top = bar.bottom;
  line.bottom = bar.bottom + 1;
  HBRUSH border = CreateSolidBrush(COL_BORDER);
  FillRect(dc, &line, border);
  DeleteObject(border);

  HFONT old = SelectObject(dc, g_ui_font);
  SetBkMode(dc, TRANSPARENT);
  wchar_t elapsed[64];
  format_elapsed(elapsed, 64);
  RECT text = bar;
  text.left += px(10);
  SetTextColor(dc, COL_TEXT);
  DrawTextW(dc, elapsed, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
  if (g_status[0] && GetTickCount64() < g_status_until) {
    text.left += px(130);
    text.right = rc.right - 2 * px(BTN_W) - 3 * px(BTN_GAP);
    SetTextColor(dc, COL_DIM);
    DrawTextW(dc, g_status, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
  }
  SelectObject(dc, old);
}

static void draw_button(const DRAWITEMSTRUCT *d) {
  HBRUSH fill = CreateSolidBrush((d->itemState & ODS_SELECTED) ? COL_BTN_DOWN : COL_BTN);
  FillRect(d->hDC, &d->rcItem, fill);
  DeleteObject(fill);
  HBRUSH border = CreateSolidBrush(COL_BORDER);
  FrameRect(d->hDC, &d->rcItem, border);
  DeleteObject(border);
  wchar_t label[32];
  GetWindowTextW(d->hwndItem, label, 32);
  HFONT old = SelectObject(d->hDC, g_ui_font);
  SetBkMode(d->hDC, TRANSPARENT);
  SetTextColor(d->hDC, COL_TEXT);
  RECT r = d->rcItem;
  DrawTextW(d->hDC, label, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
  SelectObject(d->hDC, old);
}

static LRESULT CALLBACK edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_KEYDOWN && wp == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
    SendMessageW(hwnd, EM_SETSEL, 0, -1);
    return 0;
  }
  if (msg == WM_CHAR && wp == 1) return 0;
  return CallWindowProcW(g_edit_proc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK console_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_CREATE: {
    g_dpi = GetDpiForWindow(hwnd);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    create_fonts();
    g_bg_brush = CreateSolidBrush(COL_BG);
    g_bar_brush = CreateSolidBrush(COL_BAR);
    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL, 0, 0, 0, 0, hwnd, (HMENU)ID_EDIT, g_hinst, NULL);
    g_copy_btn = CreateWindowExW(0, L"BUTTON", L"Copy all", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)ID_COPY, g_hinst, NULL);
    g_save_btn = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 0, 0, hwnd, (HMENU)ID_SAVE, g_hinst, NULL);
    SendMessageW(g_edit, EM_SETLIMITTEXT, 0, 0);
    SetWindowTheme(g_edit, L"DarkMode_Explorer", NULL);
    g_edit_proc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
    apply_fonts();
    SetWindowPos(hwnd, NULL, 0, 0, px(900), px(520), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetTimer(hwnd, TIMER_TICK, 1000, NULL);
    return 0;
  }
  case WM_SIZE:
    layout(hwnd);
    return 0;
  case WM_GETMINMAXINFO: {
    MINMAXINFO *mm = (MINMAXINFO *)lp;
    mm->ptMinTrackSize.x = px(360);
    mm->ptMinTrackSize.y = px(200);
    return 0;
  }
  case WM_DPICHANGED: {
    g_dpi = HIWORD(wp);
    create_fonts();
    apply_fonts();
    RECT *r = (RECT *)lp;
    SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
    layout(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    paint_bar(hwnd, dc);
    EndPaint(hwnd, &ps);
    return 0;
  }
  case WM_CTLCOLORSTATIC:
  case WM_CTLCOLOREDIT:
    if ((HWND)lp == g_edit) {
      SetTextColor((HDC)wp, COL_TEXT);
      SetBkColor((HDC)wp, COL_BG);
      return (LRESULT)g_bg_brush;
    }
    break;
  case WM_DRAWITEM:
    if (wp == ID_COPY || wp == ID_SAVE) {
      draw_button((const DRAWITEMSTRUCT *)lp);
      return TRUE;
    }
    break;
  case WM_COMMAND:
    if (LOWORD(wp) == ID_COPY) {
      copy_all(hwnd);
      return 0;
    }
    if (LOWORD(wp) == ID_SAVE) {
      save_output(hwnd);
      return 0;
    }
    break;
  case WM_TIMER:
    if (wp == TIMER_TICK) {
      invalidate_bar(hwnd);
      raise_above_game(hwnd);
      check_game_alive(hwnd);
      return 0;
    }
    break;
  case WM_APP_FLUSH:
    flush_pending();
    return 0;
  case WM_DESTROY:
    KillTimer(hwnd, TIMER_TICK);
    AcquireSRWLockExclusive(&g_lock);
    g_state = CONSOLE_CLOSED;
    g_wnd = NULL;
    drop_pending();
    ReleaseSRWLockExclusive(&g_lock);
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static BOOL CALLBACK use_game_icon(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR param) {
  (void)type;
  WNDCLASSEXW *wc = (WNDCLASSEXW *)param;
  wc->hIcon = LoadImageW(module, name, IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED);
  wc->hIconSm = LoadImageW(module, name, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
  return FALSE;
}

static DWORD WINAPI console_thread(LPVOID ready) {
  ACTCTXW ctx = { 0 };
  ctx.cbSize = sizeof(ctx);
  ctx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
  ctx.hModule = g_hinst;
  ctx.lpResourceName = ISOLATIONAWARE_MANIFEST_RESOURCE_ID;
  HANDLE actctx = CreateActCtxW(&ctx);
  ULONG_PTR cookie = 0;
  if (actctx != INVALID_HANDLE_VALUE && !ActivateActCtx(actctx, &cookie)) cookie = 0;
  HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  WNDCLASSEXW wc = { 0 };
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = console_proc;
  wc.hInstance = g_hinst;
  wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
  wc.lpszClassName = L"NativeLoaderConsole";
  EnumResourceNamesW(GetModuleHandleW(NULL), RT_GROUP_ICON, use_game_icon, (LONG_PTR)&wc);
  HWND hwnd = NULL;
  if (RegisterClassExW(&wc)) hwnd = CreateWindowExW(0, L"NativeLoaderConsole", g_title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 900, 520, NULL, NULL, g_hinst, NULL);

  AcquireSRWLockExclusive(&g_lock);
  if (hwnd) {
    g_wnd = hwnd;
    if (g_pending_len > 0 && !g_flush_posted && PostMessageW(hwnd, WM_APP_FLUSH, 0, 0)) g_flush_posted = 1;
  } else {
    g_state = CONSOLE_CLOSED;
    drop_pending();
  }
  ReleaseSRWLockExclusive(&g_lock);

  if (hwnd) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  SetEvent((HANDLE)ready);

  if (hwnd) {
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  if (g_mono_font) DeleteObject(g_mono_font);
  if (g_ui_font) DeleteObject(g_ui_font);
  if (g_bg_brush) DeleteObject(g_bg_brush);
  if (g_bar_brush) DeleteObject(g_bar_brush);
  if (SUCCEEDED(com)) CoUninitialize();
  if (cookie) DeactivateActCtx(0, cookie);
  if (actctx != INVALID_HANDLE_VALUE) ReleaseActCtx(actctx);
  return 0;
}

void console_open(const wchar_t *title) {
  wcscpy_s(g_title, MAX_PATH, title);
  HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
  AcquireSRWLockExclusive(&g_lock);
  g_state = CONSOLE_OPEN;
  ReleaseSRWLockExclusive(&g_lock);

  HANDLE t = CreateThread(NULL, 0, console_thread, ready, 0, NULL);
  if (!t) {
    AcquireSRWLockExclusive(&g_lock);
    g_state = CONSOLE_CLOSED;
    ReleaseSRWLockExclusive(&g_lock);
    CloseHandle(ready);
    return;
  }
  CloseHandle(t);
  if (WaitForSingleObject(ready, READY_TIMEOUT_MS) == WAIT_OBJECT_0) CloseHandle(ready);
}
