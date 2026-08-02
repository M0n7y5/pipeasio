/*
 * Minimal ASIO host used by tests/asio_probe/run*.sh.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ASIO interface mirror. */

typedef struct w_int64_t
{
    ULONG hi;
    ULONG lo;
} w_int64_t;

typedef struct BufferInformation
{
    LONG  isInputType;
    LONG  channelNumber;
    void *audioBufferStart;
    void *audioBufferEnd;
} BufferInformation;

typedef struct TimeInformation
{
    LONG      _1[4];
    double    _2;
    w_int64_t timeStamp;
    w_int64_t numSamples;
    double    sampleRate;
    ULONG     flags;
    char      _3[12];
    double    speedForTimeCode;
    w_int64_t timeStampForTimeCode;
    ULONG     flagsForTimeCode;
    char      _4[64];
} TimeInformation;
typedef struct ASIOClockSource
{
    LONG index;
    LONG associatedChannel;
    LONG associatedGroup;
    LONG isCurrentSource;
    char name[32];
} ASIOClockSource;

/* i386 ASIO methods are thiscall. Host callbacks are cdecl. */
#ifdef __i386__
#define PROBE_THISCALL __attribute__((__thiscall__))
#define PROBE_CB /* cdecl */
#else
#define PROBE_THISCALL CALLBACK
#define PROBE_CB CALLBACK
#endif

typedef struct Callbacks
{
    void(PROBE_CB *swapBuffers)(LONG, LONG);
    void(PROBE_CB *sampleRateChanged)(double);
    LONG(PROBE_CB *sendNotification)(LONG, LONG, void *, double *);
    void *(PROBE_CB *swapBuffersWithTimeInfo)(TimeInformation *, LONG, LONG);
} Callbacks;

/* IPipeASIO COM vtable mirror. */
typedef struct IPipeASIO IPipeASIO;
typedef struct IPipeASIOVtbl
{
    HRESULT(CALLBACK *QueryInterface)(IPipeASIO *, REFIID, void **);
    ULONG(CALLBACK *AddRef)(IPipeASIO *);
    ULONG(CALLBACK *Release)(IPipeASIO *);
    LONG(PROBE_THISCALL *Init)(IPipeASIO *, void *);
    void(PROBE_THISCALL *GetDriverName)(IPipeASIO *, char *);
    LONG(PROBE_THISCALL *GetDriverVersion)(IPipeASIO *);
    void(PROBE_THISCALL *GetErrorMessage)(IPipeASIO *, char *);
    LONG(PROBE_THISCALL *Start)(IPipeASIO *);
    LONG(PROBE_THISCALL *Stop)(IPipeASIO *);
    LONG(PROBE_THISCALL *GetChannels)(IPipeASIO *, LONG *, LONG *);
    LONG(PROBE_THISCALL *GetLatencies)(IPipeASIO *, LONG *, LONG *);
    LONG(PROBE_THISCALL *GetBufferSize)(IPipeASIO *, LONG *, LONG *, LONG *, LONG *);
    LONG(PROBE_THISCALL *CanSampleRate)(IPipeASIO *, double);
    LONG(PROBE_THISCALL *GetSampleRate)(IPipeASIO *, double *);
    LONG(PROBE_THISCALL *SetSampleRate)(IPipeASIO *, double);
    LONG(PROBE_THISCALL *GetClockSources)(IPipeASIO *, void *, LONG *);
    LONG(PROBE_THISCALL *SetClockSource)(IPipeASIO *, LONG);
    LONG(PROBE_THISCALL *GetSamplePosition)(IPipeASIO *, w_int64_t *, w_int64_t *);
    LONG(PROBE_THISCALL *GetChannelInfo)(IPipeASIO *, void *);
    LONG(PROBE_THISCALL *CreateBuffers)(IPipeASIO *, BufferInformation *, LONG, LONG, Callbacks *);
    LONG(PROBE_THISCALL *DisposeBuffers)(IPipeASIO *);
    LONG(PROBE_THISCALL *ControlPanel)(IPipeASIO *);
    LONG(PROBE_THISCALL *Future)(IPipeASIO *, LONG, void *);
    LONG(PROBE_THISCALL *OutputReady)(IPipeASIO *);
} IPipeASIOVtbl;
struct IPipeASIO
{
    const IPipeASIOVtbl *lpVtbl;
};

/* Must match src/asio.c and src/regsvr.c. */
static const GUID CLSID_PipeASIO
        = { 0x2D3CA9E2, 0x1193, 0x4C5D, { 0xB5, 0xFD, 0x38, 0x79, 0x8F, 0x3D, 0xC0, 0x74 } };
static const GUID IID_ProbeIUnknown
        = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

/* Probe state. */

static volatile LONG g_cycles;
static volatile LONG g_callback_tid;
static volatile LONG g_first_index = -1;
static volatile LONG g_block_callback;
static volatile LONG g_stop_ready;
static volatile LONG g_stop_done;
static HANDLE        g_callback_blocked;
static HANDLE        g_callback_release;
static HANDLE        g_stop_completion_entered;
static HANDLE        g_stop_completion_release;

/* PROBE_GATE_BARRIER=1: events for the one-shot method_end stall used to
 * force the method_gate lost-wakeup interleaving.  Names stay in globals so
 * the environment can be armed only moments before the leaver thread runs.
 * arming earlier would let an unrelated method_end consume the one-shot
 * barrier on the main thread. */
static HANDLE g_gate_entered;
static HANDLE g_gate_release;
static HANDLE g_gate_drain;
static char   g_gate_entered_name[128];
static char   g_gate_release_name[128];
static char   g_gate_drain_name[128];

/*
 * Callback -> ordinary Win32 worker scheduling topology (PROBE_RT_WORKER=1).
 *
 * Policies are read from outside: Wine services file opens in the wineserver, so
 * /proc/thread-self inside a PE thread describes the wineserver.  Each side stamps
 * its own Linux comm via SetThreadDescription. run_rt.sh reads
 * /proc/<pid>/task/<tid>/{comm,sched}.
 *
 * The worker is CreateThread'd only after run_concurrent_stop has finished, during
 * the existing restart window.  Creating it earlier interacts with Wine+ASan and
 * makes the concurrent-Stop block wait time out.
 */
static volatile LONG g_rt_worker_mode;
static volatile LONG g_rt_worker_quit;
static volatile LONG g_rt_probe_done;
static volatile LONG g_worker_runs;
static volatile LONG g_handoff_ok;
static volatile LONG g_worker_tid;
static HANDLE        g_work_req;
static HANDLE        g_work_ack;

/* PROBE_XRUN_ARM_FILE: one-shot bufferSwitch stall armed by the runner once
 * its pw-top stream is profiling us (the ERR positive control).  Polled every
 * 32 cycles so the callback pays one wineserver file check per ~0.7s instead
 * of per cycle. Clean legs never set the file and poll nothing. */
static char          g_xrun_arm_path[MAX_PATH];
static LONG          g_xrun_stall_ms;
static volatile LONG g_xrun_stall_done;
static volatile LONG g_xrun_poll_logged;

#define PROBE_CB_THREAD_NAME L"pa-probe-cb"
#define PROBE_WORKER_THREAD_NAME L"pa-probe-wrk"

static HRESULT(WINAPI *g_set_thread_description)(HANDLE, PCWSTR);

static bool
resolve_thread_naming(void)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    void   *fn  = k32 ? (void *)GetProcAddress(k32, "SetThreadDescription") : NULL;
    if (!fn)
        return false;
    *(void **)&g_set_thread_description = fn;
    return true;
}

static void
await_rt_scan(void)
{
    const char *ack = getenv("PROBE_RT_SCAN_FILE");
    if (!ack)
        return;
    for (int i = 0; i < 3000; ++i)
    {
        if (GetFileAttributesA(ack) != INVALID_FILE_ATTRIBUTES)
            return;
        Sleep(10);
    }
    fprintf(stderr, "[probe] rt-worker: scan handshake timed out\n");
}

/* Ordinary CreateThread worker: never given a priority. Named at entry. */
static DWORD WINAPI
rt_worker_thread(void *arg)
{
    (void)arg;
    g_set_thread_description(GetCurrentThread(), PROBE_WORKER_THREAD_NAME);
    InterlockedExchange(&g_worker_tid, (LONG)GetCurrentThreadId());
    for (;;)
    {
        if (WaitForSingleObject(g_work_req, INFINITE) != WAIT_OBJECT_0)
            return 1;
        if (InterlockedCompareExchange(&g_rt_worker_quit, 0, 0))
            return 0;
        InterlockedIncrement(&g_worker_runs);
        SetEvent(g_work_ack);
    }
}

typedef struct
{
    IPipeASIO *asio;
    HANDLE     start;
    LONG       result;
} StopCall;
static void
record_cycle(LONG idx)
{
    InterlockedCompareExchange(&g_callback_tid, (LONG)GetCurrentThreadId(), 0);
    InterlockedCompareExchange(&g_first_index, idx, -1);
    g_cycles++;
    if (g_xrun_arm_path[0] && !g_xrun_stall_done && (g_cycles & 31) == 0)
    {
        DWORD attr = GetFileAttributesA(g_xrun_arm_path);
        if (!InterlockedCompareExchange(&g_xrun_poll_logged, 1, 0))
            fprintf(stderr, "[probe] xrun-stall poll: path=%s attr=0x%lx err=%ld\n",
                    g_xrun_arm_path, (unsigned long)attr, (long)GetLastError());
        if (attr != INVALID_FILE_ATTRIBUTES
            && !InterlockedCompareExchange(&g_xrun_stall_done, 1, 0))
        {
            fprintf(stderr, "[probe] xrun-stall: armed, sleeping %ld ms in bufferSwitch\n",
                    (long)g_xrun_stall_ms);
            Sleep((DWORD)g_xrun_stall_ms);
            fprintf(stderr, "[probe] xrun-stall: done\n");
        }
    }
    if (InterlockedCompareExchange(&g_block_callback, 0, 0))
    {
        SetEvent(g_callback_blocked);
        WaitForSingleObject(g_callback_release, 5000);
    }
    /* One-shot callback -> worker handoff (armed only during the restart leg). */
    if (InterlockedCompareExchange(&g_rt_worker_mode, 0, 0)
        && !InterlockedCompareExchange(&g_rt_probe_done, 1, 0))
    {
        g_set_thread_description(GetCurrentThread(), PROBE_CB_THREAD_NAME);
        SetEvent(g_work_req);
        if (WaitForSingleObject(g_work_ack, 5000) == WAIT_OBJECT_0)
            InterlockedExchange(&g_handoff_ok, 1);
    }
}

static DWORD WINAPI
stop_thread(void *arg)
{
    StopCall *call = arg;
    /* Infinite park: with threads pre-created, a bounded wait can expire while
     * main is still inside a slow CreateThread, and the thread would then
     * call Stop before the callback is even armed.  Cleanup always signals
     * start, so the park cannot outlive the test. */
    if (WaitForSingleObject(call->start, INFINITE) != WAIT_OBJECT_0)
    {
        call->result = -1001;
        InterlockedIncrement(&g_stop_done);
        return 1;
    }
    InterlockedIncrement(&g_stop_ready);
    call->result = call->asio->lpVtbl->Stop(call->asio);
    InterlockedIncrement(&g_stop_done);
    return 0;
}

static bool
run_concurrent_stop(IPipeASIO *asio)
{
    HANDLE   start      = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE   blocked    = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE   release    = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE   threads[2] = { NULL, NULL };
    StopCall calls[2]   = { { asio, start, -1000 }, { asio, start, -1000 } };
    int      created    = 0;
    bool     overlapped = false;
    bool     joined;

    if (!start || !blocked || !release)
        goto done;
    g_callback_blocked = blocked;
    g_callback_release = release;
    InterlockedExchange(&g_stop_ready, 0);
    InterlockedExchange(&g_stop_done, 0);
    /* Create the stop threads first, parked on start: thread creation after
     * earlier joined threads can take seconds under Wine+ASan, and that
     * latency must not eat the blocked callback's 5s budget. */
    for (; created < 2; ++created)
    {
        threads[created] = CreateThread(NULL, 0, stop_thread, &calls[created], 0, NULL);
        if (!threads[created])
            break;
    }
    InterlockedExchange(&g_block_callback, 1);
    if (WaitForSingleObject(blocked, 2000) != WAIT_OBJECT_0)
        goto done;
    SetEvent(start);
    DWORD ready_wait = GetTickCount();
    int   waited     = 0;
    for (; waited < 2000 && InterlockedCompareExchange(&g_stop_ready, 0, 0) < created; ++waited)
        Sleep(1);
    ready_wait = GetTickCount() - ready_wait;
    Sleep(50);
    overlapped = created == 2 && InterlockedCompareExchange(&g_stop_ready, 0, 0) == 2
                 && InterlockedCompareExchange(&g_stop_done, 0, 0) == 0;
    if (!overlapped)
        fprintf(stderr,
                "[probe] concurrent Stop detail: ready=%ld done=%ld created=%d ready_wait=%lums\n",
                (long)InterlockedCompareExchange(&g_stop_ready, 0, 0),
                (long)InterlockedCompareExchange(&g_stop_done, 0, 0), created,
                (unsigned long)ready_wait);
done:
    InterlockedExchange(&g_block_callback, 0);
    if (release)
        SetEvent(release);
    if (start)
        SetEvent(start);
    for (int i = 0; i < created; ++i)
    {
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
    }
    joined = created == 2 && calls[0].result == 0 && calls[1].result == 0;
    if (!created)
        asio->lpVtbl->Stop(asio);
    g_callback_blocked = NULL;
    g_callback_release = NULL;
    if (release)
        CloseHandle(release);
    if (blocked)
        CloseHandle(blocked);
    if (start)
        CloseHandle(start);
    fprintf(stderr, "[probe] concurrent Stop: overlap=%s results=%ld/%ld -> %s\n",
            overlapped ? "yes" : "no", (long)calls[0].result, (long)calls[1].result,
            overlapped && joined ? "ok" : "BAD");
    return overlapped && joined;
}

static bool
run_forced_stop_interleave(IPipeASIO *asio)
{
    HANDLE   stop1_start = CreateEventA(NULL, TRUE, TRUE, NULL);
    HANDLE   stop2_start = CreateEventA(NULL, TRUE, TRUE, NULL);
    HANDLE   blocked     = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE   release     = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE   threads[2]  = { NULL, NULL };
    StopCall calls[2]    = { { asio, stop1_start, -1000 }, { asio, stop2_start, -1000 } };
    bool     ok          = false;

    if (!stop1_start || !stop2_start || !blocked || !release || !g_stop_completion_entered
        || !g_stop_completion_release)
        goto done;

    ResetEvent(g_stop_completion_entered);
    ResetEvent(g_stop_completion_release);
    threads[0] = CreateThread(NULL, 0, stop_thread, &calls[0], 0, NULL);
    if (!threads[0] || WaitForSingleObject(g_stop_completion_entered, 3000) != WAIT_OBJECT_0)
        goto done;

    g_first_index     = -1;
    LONG start_result = asio->lpVtbl->Start(asio);
    if (start_result != 0)
        goto done;

    g_callback_blocked = blocked;
    g_callback_release = release;
    InterlockedExchange(&g_block_callback, 1);
    if (WaitForSingleObject(blocked, 2000) != WAIT_OBJECT_0)
        goto done;

    threads[1] = CreateThread(NULL, 0, stop_thread, &calls[1], 0, NULL);
    if (!threads[1])
        goto done;
    Sleep(50);
    bool second_waiting = WaitForSingleObject(threads[1], 0) == WAIT_TIMEOUT;

    SetEvent(g_stop_completion_release);
    bool first_completed = WaitForSingleObject(threads[0], 2000) == WAIT_OBJECT_0;
    Sleep(50);
    bool second_held = WaitForSingleObject(threads[1], 0) == WAIT_TIMEOUT;

    InterlockedExchange(&g_block_callback, 0);
    SetEvent(release);
    bool second_completed = WaitForSingleObject(threads[1], 3000) == WAIT_OBJECT_0;
    ok = start_result == 0 && second_waiting && first_completed && second_held && second_completed
         && calls[0].result == 0 && calls[1].result == 0 && g_first_index == 0;

    fprintf(stderr,
            "[probe] forced Stop/Start/Stop: waiting=%s first=%ld held=%s second=%ld "
            "first-index=%ld -> %s\n",
            second_waiting ? "yes" : "no", (long)calls[0].result, second_held ? "yes" : "no",
            (long)calls[1].result, (long)g_first_index, ok ? "ok" : "BAD");

done:
    InterlockedExchange(&g_block_callback, 0);
    if (g_stop_completion_release)
        SetEvent(g_stop_completion_release);
    if (release)
        SetEvent(release);
    for (int i = 0; i < 2; ++i)
    {
        if (threads[i])
        {
            WaitForSingleObject(threads[i], 3000);
            CloseHandle(threads[i]);
        }
    }
    g_callback_blocked = NULL;
    g_callback_release = NULL;
    if (release)
        CloseHandle(release);
    if (blocked)
        CloseHandle(blocked);
    if (stop2_start)
        CloseHandle(stop2_start);
    if (stop1_start)
        CloseHandle(stop1_start);
    return ok;
}

static void PROBE_CB
cb_swapBuffers(LONG idx, LONG direct)
{
    (void)direct;
    record_cycle(idx);
}
static void PROBE_CB
cb_sampleRateChanged(double rate)
{
    fprintf(stderr, "[probe] sampleRateChanged(%f)\n", rate);
}
static LONG PROBE_CB
cb_sendNotification(LONG selector, LONG value, void *msg, double *opt)
{
    (void)value;
    (void)msg;
    (void)opt;
    /* Advertise the callback paths this probe implements. */
    if (selector == 1 || selector == 2)
        return 1;
    if (selector == 7 /* kAsioSupportsTimeInfo */)
        return 1;
    return 0;
}
static void *PROBE_CB
cb_swapBuffersWithTimeInfo(TimeInformation *t, LONG idx, LONG direct)
{
    (void)t;
    (void)direct;
    record_cycle(idx);
    return NULL;
}

/* Main probe sequence. */

static int
die(const char *what, LONG err)
{
    fprintf(stderr, "[probe] FAIL: %s -> 0x%lx\n", what, (unsigned long)err);
    return 1;
}

/* winecrt0 hands main() argc=0 under -mno-cygwin on current Wine, so
 * rebuild argv from GetCommandLineA() (argv[0] may be quoted). */
static int
parse_cmdline(char ***out)
{
    static char  buf[1024];
    static char *args[32];
    int          n = 0;
    char        *p = buf;

    lstrcpynA(buf, GetCommandLineA(), sizeof buf);
    while (*p && n < 32)
    {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        if (*p == '"')
        {
            args[n++] = ++p;
            while (*p && *p != '"')
                p++;
        }
        else
        {
            args[n++] = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
        }
        if (*p)
            *p++ = 0;
    }
    *out = args;
    return n;
}

static bool
setup_stop_completion_barrier(void)
{
    if (!getenv("PROBE_STOP_INTERLEAVE"))
        return true;

    char entered_name[128];
    char release_name[128];
    wsprintfA(entered_name, "PipeASIOStopEntered-%lu", (unsigned long)GetCurrentProcessId());
    wsprintfA(release_name, "PipeASIOStopRelease-%lu", (unsigned long)GetCurrentProcessId());
    g_stop_completion_entered = CreateEventA(NULL, TRUE, FALSE, entered_name);
    g_stop_completion_release = CreateEventA(NULL, TRUE, FALSE, release_name);
    return g_stop_completion_entered && g_stop_completion_release
           && SetEnvironmentVariableA("PIPEASIO_TEST_STOP_ENTERED", entered_name)
           && SetEnvironmentVariableA("PIPEASIO_TEST_STOP_RELEASE", release_name);
}

static bool
setup_gate_barrier(void)
{
    if (!getenv("PROBE_GATE_BARRIER"))
        return true;

    wsprintfA(g_gate_entered_name, "PipeASIOGateEntered-%lu", (unsigned long)GetCurrentProcessId());
    wsprintfA(g_gate_release_name, "PipeASIOGateRelease-%lu", (unsigned long)GetCurrentProcessId());
    wsprintfA(g_gate_drain_name, "PipeASIOGateDrain-%lu", (unsigned long)GetCurrentProcessId());
    g_gate_entered = CreateEventA(NULL, TRUE, FALSE, g_gate_entered_name);
    g_gate_release = CreateEventA(NULL, TRUE, FALSE, g_gate_release_name);
    g_gate_drain   = CreateEventA(NULL, TRUE, FALSE, g_gate_drain_name);
    return g_gate_entered && g_gate_release && g_gate_drain;
}

/* Arm the driver's one-shot method_end stall and its drain-wait signal.
 * Must run immediately before the leaver thread so no unrelated method_end
 * can consume the barrier. */
static bool
arm_gate_barrier(void)
{
    return SetEnvironmentVariableA("PIPEASIO_TEST_GATE_ENTERED", g_gate_entered_name)
           && SetEnvironmentVariableA("PIPEASIO_TEST_GATE_RELEASE", g_gate_release_name)
           && SetEnvironmentVariableA("PIPEASIO_TEST_GATE_DRAIN", g_gate_drain_name);
}

typedef struct
{
    IPipeASIO         *asio;
    BufferInformation *bi;
    LONG               nch;
    LONG               prefBs;
    Callbacks         *cbs;
    LONG               rc;
} CreateBuffersCall;

/* Ends its method call parked in the driver's gate-leave test barrier, having
 * already observed the method gate open. */
static DWORD WINAPI
gate_leaver_thread(void *arg)
{
    IPipeASIO *asio = arg;
    double     rate = 0.0;
    asio->lpVtbl->GetSampleRate(asio, &rate);
    return 0;
}

static DWORD WINAPI
create_buffers_thread(void *arg)
{
    CreateBuffersCall *call = arg;
    call->rc = call->asio->lpVtbl->CreateBuffers(call->asio, call->bi, call->nch, call->prefBs,
                                                 call->cbs);
    return 0;
}

int
main(void)
{
    char **argv;
    int    argc = parse_cmdline(&argv);

    /* Make sure every fprintf flushes so we don't lose traces on crash. */
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    int seconds = (argc > 1) ? atoi(argv[1]) : 5;
    if (seconds <= 0)
        seconds = 5;

    fprintf(stderr, "[probe] start, target run = %ds\n", seconds);

    const bool rt_worker = getenv("PROBE_RT_WORKER") != NULL;
    if (rt_worker && !resolve_thread_naming())
    {
        fprintf(stderr, "[probe] rt-worker: SetThreadDescription unavailable\n");
        return 77;
    }

    if (!setup_stop_completion_barrier())
        return die("setup stop completion barrier", GetLastError());
    if (!setup_gate_barrier())
        return die("setup gate barrier", GetLastError());

    const char *xrun_arm = getenv("PROBE_XRUN_ARM_FILE");
    if (xrun_arm)
    {
        lstrcpynA(g_xrun_arm_path, xrun_arm, sizeof g_xrun_arm_path);
        g_xrun_stall_ms = 500;
        const char *ms  = getenv("PROBE_XRUN_STALL_MS");
        if (ms && atoi(ms) > 0)
            g_xrun_stall_ms = atol(ms);
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
        return die("CoInitializeEx", hr);

    IPipeASIO *asio = NULL;
    hr              = CoCreateInstance(&CLSID_PipeASIO, NULL, CLSCTX_INPROC_SERVER, &CLSID_PipeASIO,
                                       (void **)&asio);
    if (FAILED(hr) || !asio)
    {
        CoUninitialize();
        return die("CoCreateInstance(CLSID_PipeASIO)", hr);
    }
    fprintf(stderr, "[probe] got IPipeASIO* = %p\n", asio);
    int   contract_ok = 1;
    void *query       = (void *)(uintptr_t)1;
    GUID  unknown
            = { 0x7c4492ae, 0x7920, 0x41d8, { 0xa2, 0xe6, 0x55, 0x66, 0x12, 0x31, 0x8e, 0x7c } };
    contract_ok &= asio->lpVtbl->QueryInterface(asio, &unknown, &query) == E_NOINTERFACE
                   && query == NULL;
    contract_ok &= asio->lpVtbl->QueryInterface(asio, &IID_ProbeIUnknown, &query) == S_OK
                   && query == asio;
    if (query)
        ((IPipeASIO *)query)->lpVtbl->Release(query);
    query = NULL;
    contract_ok
            &= asio->lpVtbl->QueryInterface(asio, &CLSID_PipeASIO, &query) == S_OK && query == asio;
    if (query)
        ((IPipeASIO *)query)->lpVtbl->Release(query);
    contract_ok &= asio->lpVtbl->QueryInterface(asio, &IID_ProbeIUnknown, NULL) == E_POINTER;

    struct
    {
        ULONG           before;
        ASIOClockSource clock;
        ULONG           after;
    } clock_record   = { 0x13579bdf, { 0 }, 0x2468ace0 };
    LONG clock_count = -1;
    contract_ok &= asio->lpVtbl->GetClockSources(asio, &clock_record.clock, &clock_count) == -998;
    clock_count = 1;
    contract_ok &= asio->lpVtbl->GetClockSources(asio, NULL, &clock_count) == -998;
    clock_count = 0;
    contract_ok &= asio->lpVtbl->GetClockSources(asio, NULL, &clock_count) == 0 && clock_count == 1;
    clock_count = 1;
    contract_ok &= asio->lpVtbl->GetClockSources(asio, &clock_record.clock, &clock_count) == 0
                   && clock_count == 1 && clock_record.clock.index == 0
                   && clock_record.clock.associatedChannel == -1
                   && clock_record.clock.associatedGroup == -1
                   && clock_record.clock.isCurrentSource == 1
                   && !strcmp(clock_record.clock.name, "Internal")
                   && clock_record.before == 0x13579bdf && clock_record.after == 0x2468ace0;
    contract_ok &= asio->lpVtbl->SetClockSource(asio, 0) == 0
                   && asio->lpVtbl->SetClockSource(asio, 1) == -1000;
    fprintf(stderr, "[probe] COM/clock contracts -> %s\n", contract_ok ? "ok" : "BAD");

    LONG rc = asio->lpVtbl->Init(asio, NULL);
    if (rc != 1)
    { /* IASIO::init returns ASIOTrue (=1) on success */
        struct
        {
            char  text[124];
            DWORD guard;
        } errmsg = { { 0 }, 0x50415349u };
        asio->lpVtbl->GetErrorMessage(asio, errmsg.text);
        if (errmsg.guard != 0x50415349u)
        {
            asio->lpVtbl->Release(asio);
            CoUninitialize();
            return die("GetErrorMessage overrun", (LONG)errmsg.guard);
        }
        fprintf(stderr, "[probe] Init failed: %s\n", errmsg.text);
        asio->lpVtbl->Release(asio);
        CoUninitialize();
        return die("Init", rc);
    }

    char name[128] = { 0 };
    asio->lpVtbl->GetDriverName(asio, name);
    LONG ver = asio->lpVtbl->GetDriverVersion(asio);
    fprintf(stderr, "[probe] driver: \"%s\" v%ld\n", name, (long)ver);

    LONG nin = 0, nout = 0;
    asio->lpVtbl->GetChannels(asio, &nin, &nout);
    fprintf(stderr, "[probe] channels: %ld in / %ld out\n", (long)nin, (long)nout);

    LONG minBs = 0, maxBs = 0, prefBs = 0, granBs = 0;
    asio->lpVtbl->GetBufferSize(asio, &minBs, &maxBs, &prefBs, &granBs);
    fprintf(stderr, "[probe] buffer sizes: min=%ld max=%ld pref=%ld gran=%ld\n", (long)minBs,
            (long)maxBs, (long)prefBs, (long)granBs);

    double rate = 0.0;
    asio->lpVtbl->GetSampleRate(asio, &rate);
    fprintf(stderr, "[probe] current sample rate: %.0f\n", rate);

    /* CanSampleRate(48000)? if not, fall through with current rate */
    rc = asio->lpVtbl->CanSampleRate(asio, 48000.0);
    if (rc == 0)
    {
        rc = asio->lpVtbl->SetSampleRate(asio, 48000.0);
        if (rc != 0)
        {
            asio->lpVtbl->Release(asio);
            CoUninitialize();
            return die("SetSampleRate(48000)", rc);
        }
        rate = 48000.0;
    }

    /* Allocate BufferInformation for ALL channels (in+out). */
    LONG               nch = nin + nout;
    BufferInformation *bi  = calloc(nch, sizeof *bi);
    if (!bi)
    {
        asio->lpVtbl->Release(asio);
        CoUninitialize();
        return 1;
    }
    for (LONG i = 0; i < nin; i++)
    {
        bi[i].isInputType   = 1;
        bi[i].channelNumber = i;
    }
    for (LONG i = 0; i < nout; i++)
    {
        bi[nin + i].isInputType   = 0;
        bi[nin + i].channelNumber = i;
    }

    Callbacks cbs = {
        .swapBuffers             = cb_swapBuffers,
        .sampleRateChanged       = cb_sampleRateChanged,
        .sendNotification        = cb_sendNotification,
        .swapBuffersWithTimeInfo = cb_swapBuffersWithTimeInfo,
    };

    if (g_gate_entered)
    {
        /* Force the method_gate lost-wakeup interleaving: the leaver observes
         * the gate open, parks, and only decrements after CreateBuffers has
         * closed the gate and settled into its drain, so it never signals. */
        if (!arm_gate_barrier())
            return die("arm gate barrier", GetLastError());
        HANDLE leaver = CreateThread(NULL, 0, gate_leaver_thread, asio, 0, NULL);
        if (!leaver || WaitForSingleObject(g_gate_entered, 10000) != WAIT_OBJECT_0)
            return die("gate leaver arm", GetLastError());
        CreateBuffersCall call    = { asio, bi, nch, prefBs, &cbs, -1000 };
        HANDLE            creator = CreateThread(NULL, 0, create_buffers_thread, &call, 0, NULL);
        if (!creator)
            return die("gate creator", GetLastError());
        /* Deterministic: release the leaver only once CreateBuffers' drain is
         * asleep on the idle event, so the unsignaled decrement must land in
         * a sleeping drain.  A timeout here means the creator never reached
         * the drain. Fail loudly instead of passing vacuously. */
        if (WaitForSingleObject(g_gate_drain, 15000) != WAIT_OBJECT_0)
            return die("gate drain never waited", GetLastError());
        DWORD released = GetTickCount();
        SetEvent(g_gate_release);
        WaitForSingleObject(creator, 15000);
        WaitForSingleObject(leaver, 5000);
        DWORD elapsed = GetTickCount() - released;
        CloseHandle(creator);
        CloseHandle(leaver);
        rc          = call.rc;
        int gate_ok = rc == 0 && elapsed < 8000;
        fprintf(stderr,
                "[probe] gate-barrier: unsignaled leave during drain;"
                " CreateBuffers rc=%ld in %lums -> %s\n",
                (long)rc, (unsigned long)elapsed, gate_ok ? "ok" : "BAD");
        if (!gate_ok)
        {
            free(bi);
            asio->lpVtbl->Release(asio);
            CoUninitialize();
            return die("CreateBuffers under gate barrier", rc);
        }
    }
    else
    {
        rc = asio->lpVtbl->CreateBuffers(asio, bi, nch, prefBs, &cbs);
        if (rc != 0)
        {
            free(bi);
            asio->lpVtbl->Release(asio);
            CoUninitialize();
            return die("CreateBuffers", rc);
        }
    }
    fprintf(stderr, "[probe] CreateBuffers OK (%ld channels @ %ld frames)\n", (long)nch,
            (long)prefBs);

    LONG inLat = 0, outLat = 0;
    asio->lpVtbl->GetLatencies(asio, &inLat, &outLat);
    fprintf(stderr, "[probe] latencies: in=%ld out=%ld\n", (long)inLat, (long)outLat);

    int    rt_worker_ok     = 1;
    HANDLE rt_worker_handle = NULL;

    g_first_index   = -1;
    DWORD start_tid = GetCurrentThreadId();
    rc              = asio->lpVtbl->Start(asio);
    if (rc != 0)
    {
        asio->lpVtbl->DisposeBuffers(asio);
        free(bi);
        asio->lpVtbl->Release(asio);
        CoUninitialize();
        return die("Start", rc);
    }
    fprintf(stderr, "[probe] Start OK, running for %d s...\n", seconds);
    fflush(stderr);

    /* Timecode was removed in 1.0.0: both selectors must be denied. */
    LONG f_en      = asio->lpVtbl->Future(asio, 1, NULL);  /* kAsioEnableTimeCodeRead */
    LONG f_can     = asio->lpVtbl->Future(asio, 11, NULL); /* kAsioCanTimeCode */
    int  future_ok = (f_en == -998 && f_can == -998);
    fprintf(stderr, "[probe] Future timecode: enable=%ld can=%ld -> %s\n", (long)f_en, (long)f_can,
            future_ok ? "denied (ok)" : "UNEXPECTED");

    /* Count process cycles for N seconds. */
    for (int t = 0; t < seconds; t++)
    {
        LONG before = g_cycles;
        Sleep(1000);
        LONG delta = g_cycles - before;
        fprintf(stderr, "[probe]   t=%d: cycles total=%ld, +%ld this second\n", t + 1,
                (long)g_cycles, (long)delta);
    }
    /* hi stays zero in a short run. lo must advance. */
    w_int64_t spos = { 0, 0 }, stamp = { 0, 0 };
    rc          = asio->lpVtbl->GetSamplePosition(asio, &spos, &stamp);
    int spos_ok = (rc == 0 && spos.hi == 0 && spos.lo > 0);
    fprintf(stderr, "[probe] GetSamplePosition: rc=%ld hi=%lu lo=%lu -> %s\n", (long)rc,
            (unsigned long)spos.hi, (unsigned long)spos.lo, spos_ok ? "ok" : "BAD");

    int  concurrent_stop_ok = getenv("PROBE_STOP_INTERLEAVE") ? run_forced_stop_interleave(asio)
                                                              : run_concurrent_stop(asio);
    LONG stopped_cycles     = g_cycles;
    Sleep(100);
    int  stop_ok             = g_cycles == stopped_cycles;
    int  thread_ok           = g_callback_tid != 0 && (DWORD)g_callback_tid != start_tid;
    LONG initial_first_index = g_first_index;
    int  first_index_ok      = initial_first_index == 0;
    fprintf(stderr, "[probe] callback thread/late callback/first index: %s/%s/%s\n",
            thread_ok ? "ok" : "BAD", stop_ok ? "ok" : "BAD", first_index_ok ? "ok" : "BAD");
    fprintf(stderr, "[probe] Stop complete, total cycles = %ld\n", (long)g_cycles);

    /* Restart leg.  When PROBE_RT_WORKER is set, the ordinary worker is created
     * here, after concurrent Stop, so the handoff cannot interact with that
     * test under Wine+ASan. */
    if (rt_worker)
    {
        InterlockedExchange(&g_rt_worker_quit, 0);
        InterlockedExchange(&g_rt_probe_done, 0);
        InterlockedExchange(&g_handoff_ok, 0);
        InterlockedExchange(&g_worker_runs, 0);
        InterlockedExchange(&g_worker_tid, 0);
        g_work_req = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_work_ack = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (g_work_req && g_work_ack)
            rt_worker_handle = CreateThread(NULL, 0, rt_worker_thread, NULL, 0, NULL);
        if (!rt_worker_handle)
        {
            asio->lpVtbl->DisposeBuffers(asio);
            free(bi);
            asio->lpVtbl->Release(asio);
            CoUninitialize();
            return die("rt-worker setup", GetLastError());
        }
        InterlockedExchange(&g_rt_worker_mode, 1);
    }

    g_first_index = -1;
    InterlockedExchange(&g_callback_tid, 0);
    LONG restart_before = g_cycles;
    rc                  = asio->lpVtbl->Start(asio);
    if (rc == 0)
    {
        if (rt_worker)
        {
            for (int i = 0; i < 500 && !InterlockedCompareExchange(&g_handoff_ok, 0, 0); ++i)
                Sleep(10);
            rt_worker_ok = InterlockedCompareExchange(&g_handoff_ok, 0, 0) == 1
                           && InterlockedCompareExchange(&g_worker_runs, 0, 0) == 1
                           && g_worker_tid != 0 && g_callback_tid != 0
                           && g_callback_tid != g_worker_tid;
            fprintf(stderr, "[probe] rt-worker: cb_tid=%lu worker_tid=%lu handoff=%s\n",
                    (unsigned long)g_callback_tid, (unsigned long)g_worker_tid,
                    rt_worker_ok ? "ok" : "BAD");
            await_rt_scan();
            InterlockedExchange(&g_rt_worker_mode, 0);
            InterlockedExchange(&g_rt_worker_quit, 1);
            SetEvent(g_work_req);
            WaitForSingleObject(rt_worker_handle, 2000);
            CloseHandle(rt_worker_handle);
            rt_worker_handle = NULL;
            if (g_work_ack)
            {
                CloseHandle(g_work_ack);
                g_work_ack = NULL;
            }
            if (g_work_req)
            {
                CloseHandle(g_work_req);
                g_work_req = NULL;
            }
        }
        else
            Sleep(200);
        asio->lpVtbl->Stop(asio);
    }
    LONG restart_stopped = g_cycles;
    Sleep(100);
    int restart_ok = rc == 0 && restart_stopped > restart_before && g_first_index == 0
                     && g_cycles == restart_stopped;
    fprintf(stderr, "[probe] restart: rc=%ld cycles=+%ld first=%ld late=%s -> %s\n", (long)rc,
            (long)(restart_stopped - restart_before), (long)g_first_index,
            g_cycles == restart_stopped ? "no" : "yes", restart_ok ? "ok" : "BAD");

    asio->lpVtbl->DisposeBuffers(asio);
    asio->lpVtbl->Release(asio);
    free(bi);
    if (g_stop_completion_release)
        CloseHandle(g_stop_completion_release);
    if (g_stop_completion_entered)
        CloseHandle(g_stop_completion_entered);
    if (g_gate_release)
        CloseHandle(g_gate_release);
    if (g_gate_entered)
        CloseHandle(g_gate_entered);
    if (g_gate_drain)
        CloseHandle(g_gate_drain);
    CoUninitialize();

    /* Lower-bound pass criterion. run32.sh is stricter in practice. */
    LONG expected = (LONG)((rate / prefBs) * seconds);
    LONG ok = (g_cycles >= expected / 2) && future_ok && spos_ok && contract_ok && thread_ok
              && stop_ok && first_index_ok && concurrent_stop_ok && restart_ok && rt_worker_ok;
    fprintf(stderr, "[probe] expected ~%ld cycles, got %ld -> %s\n", (long)expected, (long)g_cycles,
            ok ? "PASS" : "FAIL");
    return ok ? 0 : 2;
}
