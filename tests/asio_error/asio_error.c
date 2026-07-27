/*
 * asio_error.c - verifies that a failed ASIOInit() reports a real error
 * message via GetErrorMessage() instead of the "does not return error
 * messages" stub.
 *
 * run.sh sets XDG_RUNTIME_DIR / PIPEWIRE_RUNTIME_DIR to a non-existent
 * path before starting this probe, which forces audio_open() to fail.
 * The probe then checks that GetErrorMessage() returns a non-empty,
 * non-stub string mentioning the failure reason.
 *
 * Exit: 0 pass, 1 fail, 77 skip (Init unexpectedly succeeded).
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>

#include <stdio.h>
#include <string.h>

typedef struct w_int64_t
{
    ULONG hi;
    ULONG lo;
} w_int64_t;

typedef struct IPipeASIO IPipeASIO;

/* Vtable layout must match src/asio.c; we only call Release/Init/GetErrorMessage. */
typedef struct IPipeASIOVtbl
{
    void *QueryInterface;
    void *AddRef;
    ULONG(CALLBACK *Release)(IPipeASIO *);
    LONG(CALLBACK *Init)(IPipeASIO *, void *);
    void *GetDriverName;
    void *GetDriverVersion;
    void(CALLBACK *GetErrorMessage)(IPipeASIO *, char *);
    void *Start;
    void *Stop;
    void *GetChannels;
    void *GetLatencies;
    void *GetBufferSize;
    void *CanSampleRate;
    void *GetSampleRate;
    void *SetSampleRate;
    void *GetClockSources;
    void *SetClockSource;
    void *GetSamplePosition;
    void *GetChannelInfo;
    void *CreateBuffers;
    void *DisposeBuffers;
    void *ControlPanel;
    void *Future;
    void *OutputReady;
} IPipeASIOVtbl;

struct IPipeASIO
{
    const IPipeASIOVtbl *lpVtbl;
};

static const GUID CLSID_PipeASIO
        = { 0x2D3CA9E2, 0x1193, 0x4C5D, { 0xB5, 0xFD, 0x38, 0x79, 0x8F, 0x3D, 0xC0, 0x74 } };

int
main(void)
{
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
    {
        fprintf(stderr, "[err] CoInitializeEx -> 0x%lx\n", (unsigned long)hr);
        return 1;
    }

    IPipeASIO *asio = NULL;
    hr              = CoCreateInstance(&CLSID_PipeASIO, NULL, CLSCTX_INPROC_SERVER, &CLSID_PipeASIO,
                                       (void **)&asio);
    if (FAILED(hr) || !asio)
    {
        fprintf(stderr, "[err] CoCreateInstance(CLSID_PipeASIO) -> 0x%lx\n", (unsigned long)hr);
        CoUninitialize();
        return 1;
    }

    LONG rc = asio->lpVtbl->Init(asio, NULL);
    if (rc == 1)
    {
        /* No failure happened, so there is nothing to assert. */
        fprintf(stderr, "[err] SKIP: Init succeeded; cannot exercise the failure path\n");
        asio->lpVtbl->Release(asio);
        CoUninitialize();
        return 77;
    }

    char msg[512] = { 0 };
    asio->lpVtbl->GetErrorMessage(asio, msg);
    fprintf(stderr, "[err] Init failed as expected; GetErrorMessage: \"%s\"\n", msg);

    int ok = msg[0] != '\0' && strcmp(msg, "PipeASIO does not return error messages\n") != 0
             && strstr(msg, "Unable") != NULL;
    if (!ok)
    {
        fprintf(stderr, "[err] FAIL: expected a concrete init failure, got stub/empty message\n");
        asio->lpVtbl->Release(asio);
        CoUninitialize();
        return 1;
    }

    asio->lpVtbl->Release(asio);
    CoUninitialize();
    return 0;
}
