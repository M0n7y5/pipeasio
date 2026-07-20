/*
 * control_panel.c - the ASIO ControlPanel dialog.
 *
 * Entry point: pipeasio_control_panel().  First tries to hand off to the
 * native pipeasio-settings panel (flatpak-spawn --host in containers, plain
 * ShellExecute on a host Wine); falls back to a built-in Win32 dialog with
 * Settings (buffer size + latency, fixed buffer size, follow device clock)
 * and About (version, config path + copy, native-panel detection) tabs.
 * The dialog watches config.ini and refreshes on external changes unless
 * the user has unsaved edits.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026 PipeASIO contributors
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "control_panel.h"

#include "pipeasio_config.h"
#include "pipeasio_log.h"
#include "build_info.h" /* PIPEASIO_BUILD_ID, generated per build */

#ifdef PIPEASIO_WOW64_PE
#include "pipeasio_wow64_pe.h"
#else
#include <sys/stat.h>
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winuser.h>
#include <wingdi.h>
#include <shellapi.h>
#include <commctrl.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* --- ASIO ControlPanel ------------------------------------------------------
 * First tries to launch the native pipeasio-settings panel (reachable only
 * when the driver runs on a host Wine that has it in PATH - not inside
 * Flatpak/Bottles).  Falls back to a small built-in Win32 dialog editing
 * buffer_size / fixed_buffer_size / follow_device_clock directly in
 * config.ini; the driver's config watcher applies the change ~1 s later.
 * The dialog also watches the file and refreshes on external changes
 * (settings panel or an editor), unless the user has unsaved edits. */

#define CP_IDC_BUFFER 100
#define CP_IDC_FIXED 101
#define CP_IDC_FOLLOW 102
#define CP_IDC_OPENPANEL 103
#define CP_IDC_OK 104
#define CP_IDC_CANCEL 105
#define CP_IDC_APPLY 106
#define CP_IDC_COPY 107
#define CP_IDC_TAB 108
#define CP_TIMER_WATCH 1

static BOOL cp_open_settings_panel(void);

typedef struct cp_state
{
    struct pipeasio_config cfg; /* values as last loaded from the file */
    uint64_t               fp;  /* config file fingerprint */
    HWND                 combo, fixed_cb, follow_cb;
    HWND                 tab, native_btn, latency, detect_lbl;
    HWND                 page1[8], page2[8]; /* controls per tab page (show/hide) */
    int                  page1n, page2n;
    BOOL                 dirty;         /* user edited something */
    BOOL                 combo_dropped; /* dropdown open: don't refresh under it */
    BOOL                 native_available; /* probe at open: panel launchable */
    char                 cfg_path[1024];
} cp_state;

static uint64_t
cp_config_fingerprint(void)
{
#ifdef PIPEASIO_WOW64_PE
    return pipeasio_wow64_config_fingerprint();
#else
    char        path[1024];
    struct stat st;
    if (!pipeasio_config_path(path, sizeof path) || stat(path, &st) != 0)
        return 0;
    return (uint64_t)st.st_mtime ^ ((uint64_t)st.st_size << 32);
#endif
}

static bool
cp_config_load(struct pipeasio_config *c)
{
#ifdef PIPEASIO_WOW64_PE
    return pipeasio_wow64_load_config(c);
#else
    return pipeasio_config_load(c);
#endif
}

static bool
cp_config_save(const struct pipeasio_config *c)
{
#ifdef PIPEASIO_WOW64_PE
    return pipeasio_wow64_save_config(c);
#else
    return pipeasio_config_save(c);
#endif
}

/* "256 -> 5.3 ms @ 48000 Hz" next to the combo (same math as the Qt panel).
 * Defined after cp_combo_value below. */
static void cp_update_latency(cp_state *st);

static void
cp_fill_controls(cp_state *st)
{
    char label[16];
    int  sel = -1, n = 0;

    SendMessageA(st->combo, CB_RESETCONTENT, 0, 0);
    for (int b = PIPEASIO_MIN_BUFFER_SIZE; b <= PIPEASIO_MAX_BUFFER_SIZE; b <<= 1)
    {
        snprintf(label, sizeof label, "%d", b);
        SendMessageA(st->combo, CB_ADDSTRING, 0, (LPARAM)label);
        if (b == st->cfg.buffer_size)
            sel = n;
        n++;
    }
    if (sel < 0) /* foreign/out-of-range value: show it as an extra item */
    {
        snprintf(label, sizeof label, "%d", st->cfg.buffer_size);
        SendMessageA(st->combo, CB_ADDSTRING, 0, (LPARAM)label);
        sel = n;
    }
    SendMessageA(st->combo, CB_SETCURSEL, sel, 0);
    SendMessageA(st->fixed_cb, BM_SETCHECK, st->cfg.fixed_buffer_size, 0);
    SendMessageA(st->follow_cb, BM_SETCHECK, st->cfg.follow_device_clock, 0);
    cp_update_latency(st);
}

static int
cp_combo_value(HWND combo)
{
    char    buf[32] = "";
    LRESULT sel     = SendMessageA(combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR)
        return 0;
    SendMessageA(combo, CB_GETLBTEXT, sel, (LPARAM)buf);
    return atoi(buf);
}

static void
cp_update_latency(cp_state *st)
{
    char   text[64];
    int    rate = st->cfg.sample_rate > 0 ? st->cfg.sample_rate : 48000;
    double ms   = cp_combo_value(st->combo) * 1000.0 / rate;
    snprintf(text, sizeof text, "%.1f ms @ %d Hz", ms, rate);
    SetWindowTextA(st->latency, text);
}

static void
cp_config_path_str(char *buf, size_t n)
{
#ifdef PIPEASIO_WOW64_PE
    if (!pipeasio_wow64_config_path(buf, n))
        lstrcpynA(buf, "config.ini (unixlib)", n);
#else
    if (!pipeasio_config_path(buf, n))
        lstrcpynA(buf, "$XDG_CONFIG_HOME/pipeasio/config.ini", n);
#endif
}

/* Merge the controls into a fresh copy of the file config and write it.
 * Also refreshes the state so the file watch ignores our own write. */
static BOOL
cp_save_from_controls(cp_state *st)
{
    struct pipeasio_config c;
    cp_config_load(&c); /* fresh base: fields we don't edit are preserved */
    c.buffer_size         = cp_combo_value(st->combo);
    c.fixed_buffer_size   = SendMessageA(st->fixed_cb, BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.follow_device_clock = SendMessageA(st->follow_cb, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!pipeasio_buffer_size_valid(c.buffer_size))
        return FALSE;
    if (!cp_config_save(&c))
        return FALSE;
    st->cfg   = c;
    st->fp    = cp_config_fingerprint();
    st->dirty = FALSE;
    return TRUE;
}

static void
cp_copy_to_clipboard(HWND owner, const char *text)
{
    size_t len = strlen(text) + 1;
    if (!OpenClipboard(owner))
        return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, len);
    if (h)
    {
        void *p = GlobalLock(h);
        if (p)
        {
            memcpy(p, text, len);
            GlobalUnlock(h);
            SetClipboardData(CF_TEXT, h);
        }
        else
            GlobalFree(h);
    }
    CloseClipboard();
}

#define CP_ADD(st, pg, hwnd) ((st)->page##pg[(st)->page##pg##n++] = (hwnd))

static void
cp_show_page(cp_state *st, int idx)
{
    for (int i = 0; i < st->page1n; i++)
        ShowWindow(st->page1[i], idx == 0 ? SW_SHOW : SW_HIDE);
    for (int i = 0; i < st->page2n; i++)
    {
        HWND h = st->page2[i];
        /* the probe's verdict on the Native panel button survives tab switches */
        if (h == st->native_btn && !st->native_available)
        {
            ShowWindow(h, SW_HIDE);
            continue;
        }
        ShowWindow(h, idx == 1 ? SW_SHOW : SW_HIDE);
    }
}

static LRESULT CALLBACK
cp_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    cp_state *st = (cp_state *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    switch (msg)
    {
    case WM_NOTIFY:
        if (wp == CP_IDC_TAB && ((LPNMHDR)lp)->code == TCN_SELCHANGE)
            cp_show_page(st, (int)SendMessageA(st->tab, TCM_GETCURSEL, 0, 0));
        break;
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case CP_IDC_BUFFER:
            if (HIWORD(wp) == CBN_DROPDOWN)
                st->combo_dropped = TRUE;
            else if (HIWORD(wp) == CBN_CLOSEUP)
                st->combo_dropped = FALSE;
            else if (HIWORD(wp) == CBN_SELCHANGE)
            {
                st->dirty = TRUE;
                cp_update_latency(st);
            }
            break;
        case CP_IDC_FIXED:
        case CP_IDC_FOLLOW:
            st->dirty = TRUE;
            break;
        case CP_IDC_OPENPANEL:
            cp_open_settings_panel();
            break;
        case CP_IDC_COPY:
            cp_copy_to_clipboard(hwnd, st->cfg_path);
            break;
        case CP_IDC_OK:
            cp_save_from_controls(st);
            DestroyWindow(hwnd);
            return 0;
        case CP_IDC_APPLY:
            cp_save_from_controls(st);
            return 0;
        case CP_IDC_CANCEL:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == CP_TIMER_WATCH && st && !st->dirty && !st->combo_dropped)
        {
            uint64_t fp = cp_config_fingerprint();
            if (fp && fp != st->fp)
            {
                st->fp = fp;
                cp_config_load(&st->cfg);
                cp_fill_controls(st);
            }
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* True when the native settings panel looks launchable (drives the
 * "Native panel" button visibility; does not launch anything visible). */
static BOOL
cp_native_panel_available(void)
{
    /* Flatpak container: does the portal allow spawning on the host?
     * (needs flatpak override --user <app> --talk-name=org.freedesktop.Flatpak) */
    SHELLEXECUTEINFOA sei = { 0 };
    sei.cbSize            = sizeof sei;
    sei.lpFile            = "flatpak-spawn";
    sei.lpParameters      = "--host true";
    sei.nShow             = SW_HIDE;
    sei.fMask             = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    if (ShellExecuteExA(&sei) && sei.hProcess)
    {
        DWORD code = 1;
        WaitForSingleObject(sei.hProcess, 3000);
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        if (code == 0)
            return TRUE;
    }
    /* Host Wine: the panel binary is directly in PATH. */
    char buf[MAX_PATH];
    return SearchPathA(NULL, "pipeasio-settings", NULL, sizeof buf, buf, NULL) != 0;
}

/* Launch `file args` and verify the process survives the first moments:
 * ShellExecute's return code only means "spawn accepted"; a found-but-broken
 * binary (or a portal rejection) must not suppress the fallback dialog. */
static BOOL
cp_try_launch(const char *file, const char *args)
{
    SHELLEXECUTEINFOA sei = { 0 };
    sei.cbSize            = sizeof sei;
    sei.lpFile            = file;
    sei.lpParameters      = args;
    sei.nShow             = SW_SHOWNORMAL;
    /* FLAG_NO_UI: failure must fall to the next candidate / built-in dialog,
     * not to ShellExecute's own "File not found" message box. */
    sei.fMask             = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    if (!ShellExecuteExA(&sei))
        return FALSE;
    if (sei.hProcess)
    {
        DWORD code = 0;
        WaitForSingleObject(sei.hProcess, 800);
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        if (code != STILL_ACTIVE)
            return FALSE;
    }
    return TRUE;
}

/* Try to launch the native settings panel; true when it actually started. */
static BOOL
cp_open_settings_panel(void)
{
    /* Flatpak container: launch the host panel through the portal (needs
     * `flatpak override --user <app> --talk-name=org.freedesktop.Flatpak`). */
    if (cp_try_launch("flatpak-spawn", "--host pipeasio-settings"))
    {
        TRACE("control panel: handed off via flatpak-spawn --host\n");
        return TRUE;
    }
    /* Host Wine: the panel is directly in PATH. */
    if (cp_try_launch("pipeasio-settings", NULL))
    {
        TRACE("control panel: handed off to pipeasio-settings\n");
        return TRUE;
    }
    TRACE("control panel: no native panel reachable, built-in dialog\n");
    return FALSE;
}

static void
cp_run_dialog(void)
{
    static const char *cls = "PipeASIOControlPanel";
    HINSTANCE          inst = GetModuleHandleA(NULL);
    cp_state           st;

    InitCommonControls();
    memset(&st, 0, sizeof st);
    cp_config_load(&st.cfg);
    st.fp = cp_config_fingerprint();
    cp_config_path_str(st.cfg_path, sizeof st.cfg_path);

    WNDCLASSA wc       = { 0 };
    wc.lpfnWndProc     = cp_wndproc;
    wc.hInstance       = inst;
    wc.lpszClassName   = cls;
    wc.hbrBackground   = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassA(&wc); /* already-registered is fine */

    /* W,H are the CLIENT size: convert to the outer window size (caption +
     * borders) or the bottom button row gets clipped. */
    const int W = 380, H = 230;
    RECT      want = { 0, 0, W, H };
    AdjustWindowRectEx(&want, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
                       WS_EX_DLGMODALFRAME);
    const int winW = want.right - want.left;
    const int winH = want.bottom - want.top;
    /* Own the dialog by the host's active window and center on it: ownerless
     * popup windows can map off-screen (or behind the host) under Wine, which
     * looks exactly like "button pressed, app frozen, nothing opened". */
    HWND owner = GetActiveWindow();
    int  x = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int  y = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;
    if (owner)
    {
        RECT rc;
        if (GetWindowRect(owner, &rc))
        {
            x = rc.left + ((rc.right - rc.left) - winW) / 2;
            y = rc.top + ((rc.bottom - rc.top) - winH) / 2;
        }
    }
    HWND dlg = CreateWindowExA(WS_EX_DLGMODALFRAME, cls, "PipeASIO Settings",
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, x, y,
                               winW, winH, owner, NULL, inst, NULL);
    if (!dlg)
    {
        WARN("control panel: CreateWindowExA failed (%lu)\n", (unsigned long)GetLastError());
        return;
    }
    SetWindowLongPtrA(dlg, GWLP_USERDATA, (LONG_PTR)&st);

    /* Tabs. */
    st.tab = CreateWindowA(WC_TABCONTROL, "", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 8, 6,
                           W - 16, 170, dlg, (HMENU)(INT_PTR)CP_IDC_TAB, inst, NULL);
    TCITEMA tie  = { 0 };
    tie.mask     = TCIF_TEXT;
    tie.pszText  = (char *)"Settings";
    SendMessageA(st.tab, TCM_INSERTITEMA, 0, (LPARAM)&tie);
    tie.pszText  = (char *)"About";
    SendMessageA(st.tab, TCM_INSERTITEMA, 1, (LPARAM)&tie);

    /* Page 1: Settings. */
    CP_ADD(&st, 1, CreateWindowA("STATIC", "Buffer size:", WS_CHILD, 24, 48, 90, 18, dlg, NULL,
                                 inst, NULL));
    st.combo = CreateWindowA("COMBOBOX", "", WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 115, 45,
                             90, 200, dlg, (HMENU)(INT_PTR)CP_IDC_BUFFER, inst, NULL);
    CP_ADD(&st, 1, st.combo);
    st.latency = CreateWindowA("STATIC", "", WS_CHILD, 212, 48, 150, 18, dlg, NULL, inst, NULL);
    CP_ADD(&st, 1, st.latency);
    st.fixed_cb = CreateWindowA("BUTTON", "Fixed buffer size", WS_CHILD | BS_AUTOCHECKBOX, 24,
                                78, 220, 20, dlg, (HMENU)(INT_PTR)CP_IDC_FIXED, inst, NULL);
    CP_ADD(&st, 1, st.fixed_cb);
    st.follow_cb = CreateWindowA("BUTTON", "Follow device clock (Bluetooth)",
                                 WS_CHILD | BS_AUTOCHECKBOX, 24, 103, 260, 20, dlg,
                                 (HMENU)(INT_PTR)CP_IDC_FOLLOW, inst, NULL);
    CP_ADD(&st, 1, st.follow_cb);

    /* Page 2: About. */
    char ver[128];
    snprintf(ver, sizeof ver, "PipeASIO %s (build %s)", PIPEASIO_VERSION, PIPEASIO_BUILD_ID);
    CP_ADD(&st, 2, CreateWindowA("STATIC", ver, WS_CHILD, 24, 44, 340, 18, dlg, NULL, inst,
                                 NULL));
    CP_ADD(&st, 2, CreateWindowA("STATIC", "Config file:", WS_CHILD, 24, 66, 200, 14, dlg,
                                 NULL, inst, NULL));
    HWND path_edit = CreateWindowA("EDIT", st.cfg_path, WS_CHILD | WS_BORDER | ES_READONLY |
                                                           ES_AUTOHSCROLL,
                                   24, 82, 252, 20, dlg, NULL, inst, NULL);
    CP_ADD(&st, 2, path_edit);
    CP_ADD(&st, 2, CreateWindowA("BUTTON", "Copy path", WS_CHILD | BS_PUSHBUTTON, 282, 80, 72,
                                 22, dlg, (HMENU)(INT_PTR)CP_IDC_COPY, inst, NULL));
    CP_ADD(&st, 2, CreateWindowA("STATIC", "Saved to config.ini; the driver applies it ~1 s "
                                          "later.",
                                 WS_CHILD, 24, 110, 340, 16, dlg, NULL, inst, NULL));
    st.detect_lbl = CreateWindowA("STATIC", "", WS_CHILD, 24, 132, 340, 16, dlg, NULL, inst,
                                  NULL);
    CP_ADD(&st, 2, st.detect_lbl);
    CP_ADD(&st, 2, CreateWindowA("STATIC", "Or run on the host:  pipeasio-settings",
                                 WS_CHILD, 24, 150, 340, 16, dlg, NULL, inst, NULL));

    /* Bottom row (always visible). */
    st.native_btn = CreateWindowA("BUTTON", "Native panel", WS_CHILD | BS_PUSHBUTTON, 14,
                                  H - 44, 90, 26, dlg, (HMENU)(INT_PTR)CP_IDC_OPENPANEL, inst,
                                  NULL);
    /* even rhythm: 70 px buttons with 8 px gaps, 14 px right margin */
    CreateWindowA("BUTTON", "Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, W - 240, H - 44,
                  70, 26, dlg, (HMENU)(INT_PTR)CP_IDC_APPLY, inst, NULL);
    CreateWindowA("BUTTON", "OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, W - 162, H - 44,
                  70, 26, dlg, (HMENU)(INT_PTR)CP_IDC_OK, inst, NULL);
    CreateWindowA("BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, W - 84, H - 44,
                  70, 26, dlg, (HMENU)(INT_PTR)CP_IDC_CANCEL, inst, NULL);

    st.native_available = cp_native_panel_available();
    if (!st.native_available)
        ShowWindow(st.native_btn, SW_HIDE);
    SetWindowTextA(st.detect_lbl, st.native_available ? "pipeasio-settings: detected"
                                                      : "pipeasio-settings: not detected");

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    for (HWND c = GetWindow(dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        SendMessageA(c, WM_SETFONT, (WPARAM)font, TRUE);

    cp_fill_controls(&st);
    cp_show_page(&st, 0);
    SetTimer(dlg, CP_TIMER_WATCH, 1000, NULL);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    KillTimer(dlg, CP_TIMER_WATCH);
    UnregisterClassA(cls, inst);
}

/* ASIO entry point: native panel when launchable, built-in dialog else. */
void
pipeasio_control_panel(void)
{
    if (cp_open_settings_panel())
        return;
    cp_run_dialog();
}

