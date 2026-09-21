/* SPDX-License-Identifier: GPL-3.0-or-later */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "../include/pipeasio_guids.h"

#ifdef __i386__
#define CHECK_THISCALL __attribute__((__thiscall__))
#else
#define CHECK_THISCALL CALLBACK
#endif

typedef struct CheckAsio CheckAsio;
typedef struct
{
    HRESULT(CALLBACK *QueryInterface)(CheckAsio *, REFIID, void **);
    ULONG(CALLBACK *AddRef)(CheckAsio *);
    ULONG(CALLBACK *Release)(CheckAsio *);
    LONG(CHECK_THISCALL *Init)(CheckAsio *, void *);
    void(CHECK_THISCALL *GetDriverName)(CheckAsio *, char *);
    LONG(CHECK_THISCALL *GetDriverVersion)(CheckAsio *);
    void(CHECK_THISCALL *GetErrorMessage)(CheckAsio *, char *);
} CheckAsioVtbl;
struct CheckAsio
{
    const CheckAsioVtbl *lpVtbl;
};

static void
escape_json(char *out, const char *text)
{
    while (*text)
    {
        unsigned char c = (unsigned char)*text++;
        if (c == '"' || c == '\\')
        {
            *out++ = '\\';
            *out++ = (char)c;
        }
        else if (c < 32 || c >= 127)
        {
            sprintf(out, "\\u%04x", c);
            out += 6;
        }
        else
            *out++ = (char)c;
    }
    *out = 0;
}

int
main(void)
{
    int          argc       = 0;
    WCHAR      **argv       = CommandLineToArgvW(GetCommandLineW(), &argc);
    HANDLE       result     = INVALID_HANDLE_VALUE;
    CheckAsio   *asio       = NULL;
    int          registered = 0, unixlib = 0, pipewire = 0, com = 0;
    int          registry_key = -1, registry_exists = -1;
    const WCHAR *result_path = NULL;
    LONG         version     = 0;
    char         error[256]  = "", escaped[1536], json[1792];
    WCHAR        value[MAX_PATH];
    DWORD        size = sizeof(value);
    HRESULT      hr;

    if (argv && argc == 3 && !wcscmp(argv[1], L"--result"))
        result_path = argv[2];
    else if (argv && argc == 5 && !wcscmp(argv[1], L"--registry-exists")
             && (!wcscmp(argv[2], L"0") || !wcscmp(argv[2], L"1")) && !wcscmp(argv[3], L"--result"))
    {
        registry_key = argv[2][0] - L'0';
        result_path  = argv[4];
    }
    if (!result_path || wcslen(result_path) < 3 || result_path[1] != L':'
        || result_path[2] != L'\\')
    {
        strcpy(error, "Usage: pipeasio-check.exe --result C:\\absolute\\result.json");
        goto done;
    }
    result = CreateFileW(result_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                         NULL);
    if (result == INVALID_HANDLE_VALUE)
    {
        strcpy(error, "Cannot create the probe result file");
        goto done;
    }
    if (registry_key >= 0)
    {
        const WCHAR *keys[] = { L"Software\\Classes\\CLSID\\{2D3CA9E2-1193-4C5D-B5FD-38798F3DC074}",
                                L"Software\\ASIO\\PipeASIO" };
        HKEY         key;
        LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keys[registry_key], 0, KEY_READ, &key);
        if (status == ERROR_SUCCESS)
        {
            registry_exists = 1;
            RegCloseKey(key);
        }
        else if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
            registry_exists = 0;
        else
            snprintf(error, sizeof(error), "Cannot inspect PipeASIO registration: %ld",
                     (long)status);
        goto done;
    }
    if (GetEnvironmentVariableW(L"PIPEASIO_CHECK_ISOLATED", value, MAX_PATH) != 1
        || value[0] != L'1')
    {
        strcpy(error, "The manager must launch this check with isolated configuration");
        goto done;
    }
    if (!SetEnvironmentVariableW(L"PIPEASIO_CONNECT_TO_HARDWARE", L"off"))
    {
        strcpy(error, "Cannot disable hardware autoconnection");
        goto done;
    }
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\ASIO\\PipeASIO", L"CLSID", RRF_RT_REG_SZ, NULL,
                     value, &size)
                != ERROR_SUCCESS
        || _wcsicmp(value, L"{2D3CA9E2-1193-4C5D-B5FD-38798F3DC074}"))
    {
        strcpy(error, "PipeASIO ASIO registration is missing or points to another class");
        goto done;
    }
    size = sizeof(value);
    if (RegGetValueW(HKEY_CLASSES_ROOT,
                     L"CLSID\\{2D3CA9E2-1193-4C5D-B5FD-38798F3DC074}\\InprocServer32", NULL,
                     RRF_RT_REG_SZ, NULL, value, &size)
        != ERROR_SUCCESS)
    {
        strcpy(error, "PipeASIO COM registration is missing");
        goto done;
    }
    const WCHAR *module = wcsrchr(value, L'\\');
    module              = module ? module + 1 : value;
#ifdef __i386__
    const WCHAR *expected = L"pipeasio32.dll";
#else
    const WCHAR *expected = L"pipeasio64.dll";
#endif
    if (_wcsicmp(module, expected))
    {
        strcpy(error, "PipeASIO COM registration points to an unexpected module");
        goto done;
    }
    registered = 1;
    hr         = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
    {
        snprintf(error, sizeof(error), "COM initialization failed: 0x%08lx", (unsigned long)hr);
        goto done;
    }
    com = 1;
    hr  = CoCreateInstance(&CLSID_PipeASIO, NULL, CLSCTX_INPROC_SERVER, &CLSID_PipeASIO,
                           (void **)&asio);
    if (FAILED(hr) || !asio)
    {
        snprintf(error, sizeof(error), "PipeASIO COM loading failed: 0x%08lx", (unsigned long)hr);
        goto done;
    }
    char name[32] = "";
    asio->lpVtbl->GetDriverName(asio, name);
    if (strcmp(name, "PipeASIO"))
    {
        strcpy(error, "Registered COM class is not the PipeASIO driver");
        goto done;
    }
    version = asio->lpVtbl->GetDriverVersion(asio);
    if (asio->lpVtbl->Init(asio, NULL) != 1)
    {
        char detail[124] = "";
        asio->lpVtbl->GetErrorMessage(asio, detail);
        detail[sizeof(detail) - 1] = 0;
        snprintf(error, sizeof(error), "PipeASIO Init failed: %s", detail);
        goto done;
    }
    /* Init succeeds only after the native unixlib opens its PipeWire client. */
    unixlib  = 1;
    pipewire = 1;

done:
    if (asio)
        asio->lpVtbl->Release(asio);
    if (com)
        CoUninitialize();
    escape_json(escaped, error);
    int length = snprintf(json, sizeof(json),
                          "{\"registered\":%s,\"unixlib\":%s,\"pipewire\":%s,"
                          "\"version\":%ld,\"registry_exists\":%s,\"error\":\"%s\"}\n",
                          registered ? "true" : "false", unixlib ? "true" : "false",
                          pipewire ? "true" : "false", (long)version,
                          registry_exists < 0 ? "null" : (registry_exists ? "true" : "false"),
                          escaped);
    printf("PIPEASIO_CHECK %s", json);
    int written_ok = 0;
    if (result != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        written_ok    = WriteFile(result, json, (DWORD)length, &written, NULL)
                        && written == (DWORD)length && FlushFileBuffers(result);
        CloseHandle(result);
    }
    LocalFree(argv);
    return (registry_key >= 0 ? registry_exists >= 0 : registered && unixlib && pipewire)
                           && written_ok
                   ? 0
                   : 1;
}
