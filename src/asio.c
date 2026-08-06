/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2006 Robert Reif
 * Portions copyright (C) 2007 Ralf Beck
 * Portions copyright (C) 2007 Johnny Petrantoni
 * Portions copyright (C) 2007 Stephane Letz
 * Portions copyright (C) 2008 William Steidtmann
 * Portions copyright (C) 2010 Peter L Jones
 * Portions copyright (C) 2010 Torben Hohn
 * Portions copyright (C) 2010 Nedko Arnaudov
 * Portions copyright (C) 2013 Joakim Hernberg
 * Portions copyright (C) 2026 PipeASIO contributors
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

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>
#ifndef PIPEASIO_WOW64_PE
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#endif
#include <stdatomic.h>

#include <stdlib.h> /* getenv for PIPEASIO_DEBUG */

/* wine/debug.h provides debugstr_guid. pipeasio_log.h overrides TRACE/WARN/ERR. */
#ifndef PIPEASIO_WOW64_PE
#include "wine/debug.h"
#endif
#include "pipeasio_log.h"
#include "pipeasio_guids.h"

#include <objbase.h>
#include <mmsystem.h>
#include <winreg.h>
#include <winuser.h> /* MessageBoxA for the ControlPanel info dialog */
#ifdef WINE_WITH_UNICODE
#include <wine/unicode.h>
#endif

#include "audio.h"
#include "pipeasio_offsets.h"
#include "pipeasio_config.h"
#include "pipeasio_parse.h"
#include "pipeasio_rt.h"
#include "pipeasio_admission_gate.h"
#ifdef PIPEASIO_WOW64_PE
#include "pipeasio_wow64_pe.h"
#endif

#ifdef PIPEASIO_WOW64_PE
/* MinGW build: enough GUID formatting for TRACE diagnostics. */
static inline const char *
wine_dbgstr_guid(const GUID *id)
{
    static char buf[48];
    if (!id)
        return "(null)";
    snprintf(buf, sizeof buf, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
             (unsigned long)id->Data1, id->Data2, id->Data3, id->Data4[0], id->Data4[1],
             id->Data4[2], id->Data4[3], id->Data4[4], id->Data4[5], id->Data4[6], id->Data4[7]);
    return buf;
}
#endif

#if defined(DEBUG) && !defined(PIPEASIO_WOW64_PE)
WINE_DEFAULT_DEBUG_CHANNEL(asio);
#endif

#define MAX_ENVIRONMENT_SIZE 64
#define PIPEASIO_MAX_NAME_LENGTH 32
#define PIPEASIO_ERROR_MESSAGE_SIZE 124

/* i386 ASIO uses MS thiscall. GCC needs a trampoline. */
#if defined(PIPEASIO_WOW64_PE) /* i386 PE / COFF (MinGW) */
#define __ASM_DEFINE_FUNC(name, suffix, code)                                                      \
    asm(".text\n\t.align 4\n\t.globl _" #name suffix "\n_" #name suffix                            \
        ":\n\t.cfi_startproc\n\t" code "\n\t.cfi_endproc");
#define __ASM_GLOBAL_FUNC(name, code) __ASM_DEFINE_FUNC(name, "", code)
#define __ASM_NAME(name) "_" name
#define __ASM_STDCALL(args) "@" #args
#else /* ELF (winegcc) */
#define __ASM_DEFINE_FUNC(name, suffix, code)                                                      \
    asm(".text\n\t.align 4\n\t.globl " #name suffix "\n\t.type " #name suffix                      \
        ",@function\n" #name suffix ":\n\t.cfi_startproc\n\t" code                                 \
        "\n\t.cfi_endproc\n\t.previous");
#define __ASM_GLOBAL_FUNC(name, code) __ASM_DEFINE_FUNC(name, "", code)
#define __ASM_NAME(name) name
#define __ASM_STDCALL(args) ""
#endif

#ifdef __i386__ /* i386 PE/ELF */

#define THISCALL(func) __thiscall_##func
#define THISCALL_NAME(func) __ASM_NAME("__thiscall_" #func)
#undef __thiscall /* MinGW predefines it as the real attribute */
#define __thiscall __stdcall
#define DEFINE_THISCALL_WRAPPER(func, args)                                                        \
    extern void THISCALL(func)(void);                                                              \
    __ASM_GLOBAL_FUNC(__thiscall_##func, "popl %eax\n\t"                                           \
                                         "pushl %ecx\n\t"                                          \
                                         "pushl %eax\n\t"                                          \
                                         "jmp " __ASM_NAME(#func) __ASM_STDCALL(args))

#else /* __i386__ */

#define THISCALL(func) func
#define THISCALL_NAME(func) __ASM_NAME(#func)
#undef __thiscall
#define __thiscall __stdcall
#define DEFINE_THISCALL_WRAPPER(func, args) /* nothing */

#endif /* __i386__ */

/* Hide ELF symbols for COM members (no-op in the PE build: PE has no ELF
 * symbol visibility). */
#ifdef PIPEASIO_WOW64_PE
#define HIDDEN
#else
#define HIDDEN __attribute__((visibility("hidden")))
#endif

#ifdef _WIN64
#define PIPEASIO_CALLBACK CALLBACK
#else
#define PIPEASIO_CALLBACK
#endif

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
typedef struct ASIOChannelInfo
{
    LONG channel;
    LONG isInput;
    LONG isActive;
    LONG channelGroup;
    LONG type;
    char name[32];
} ASIOChannelInfo;

typedef struct Callbacks
{
    void(PIPEASIO_CALLBACK *swapBuffers)(LONG, LONG);
    void(PIPEASIO_CALLBACK *sampleRateChanged)(double);
    LONG(PIPEASIO_CALLBACK *sendNotification)(LONG, LONG, void *, double *);
    void *(PIPEASIO_CALLBACK *swapBuffersWithTimeInfo)(TimeInformation *, LONG, LONG);
} Callbacks;

/*****************************************************************************
 * IPipeASIO interface
 */

#define INTERFACE IPipeASIO
DECLARE_INTERFACE_(IPipeASIO, IUnknown)
{
    STDMETHOD_(HRESULT, QueryInterface)(THIS_ IID riid, void **ppvObject) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD_(LONG, Init)(THIS_ void *sysRef) PURE;
    STDMETHOD_(void, GetDriverName)(THIS_ char *name) PURE;
    STDMETHOD_(LONG, GetDriverVersion)(THIS) PURE;
    STDMETHOD_(void, GetErrorMessage)(THIS_ char *string) PURE;
    STDMETHOD_(LONG, Start)(THIS) PURE;
    STDMETHOD_(LONG, Stop)(THIS) PURE;
    STDMETHOD_(LONG, GetChannels)(THIS_ LONG * numInputChannels, LONG * numOutputChannels) PURE;
    STDMETHOD_(LONG, GetLatencies)(THIS_ LONG * inputLatency, LONG * outputLatency) PURE;
    STDMETHOD_(LONG, GetBufferSize)(THIS_ LONG * minSize, LONG * maxSize, LONG * preferredSize,
                                    LONG * granularity) PURE;
    STDMETHOD_(LONG, CanSampleRate)(THIS_ double sampleRate) PURE;
    STDMETHOD_(LONG, GetSampleRate)(THIS_ double *sampleRate) PURE;
    STDMETHOD_(LONG, SetSampleRate)(THIS_ double sampleRate) PURE;
    STDMETHOD_(LONG, GetClockSources)(THIS_ void *clocks, LONG *numSources) PURE;
    STDMETHOD_(LONG, SetClockSource)(THIS_ LONG index) PURE;
    STDMETHOD_(LONG, GetSamplePosition)(THIS_ w_int64_t * sPos, w_int64_t * tStamp) PURE;
    STDMETHOD_(LONG, GetChannelInfo)(THIS_ void *info) PURE;
    STDMETHOD_(LONG, CreateBuffers)(THIS_ BufferInformation * bufferInfo, LONG numChannels,
                                    LONG bufferSize, Callbacks * callbacks) PURE;
    STDMETHOD_(LONG, DisposeBuffers)(THIS) PURE;
    STDMETHOD_(LONG, ControlPanel)(THIS) PURE;
    STDMETHOD_(LONG, Future)(THIS_ LONG selector, void *opt) PURE;
    STDMETHOD_(LONG, OutputReady)(THIS) PURE;
};
#undef INTERFACE

typedef struct IPipeASIO *LPPIPEASIO;
typedef enum pipeasio_driver_state
{
    Loaded,
    Initializing,
    Initialized,
    Preparing,
    Prepared,
    Starting,
    Running,
    Stopping,
    Disposing,
    Destroying
} pipeasio_driver_state;

typedef struct method_token
{
    struct IPipeASIOImpl *owner;
    bool                  counted;
} method_token;

typedef struct IOChannel
{
    audio_sample_t *audio_buffer;
    char            port_name[PIPEASIO_MAX_NAME_LENGTH];
    audio_port_t   *port;
    bool            active;
} IOChannel;

typedef struct IPipeASIOImpl
{
    /* COM and lifetime state */
    const IPipeASIOVtbl    *lpVtbl;
    _Atomic ULONG           ref;
    pipeasio_admission_gate method_gate;
    pipeasio_admission_gate host_gate;
    HANDLE                  method_idle;
    HANDLE                  host_idle;
    SRWLOCK                 lifecycle_lock;
    HANDLE                  work_event;
    HANDLE                  worker;
    DWORD                   worker_tid;
    HMODULE                 module_pin;
    _Atomic uint32_t        gate_owner_seq;
    _Atomic uint32_t        lifecycle_waiters;
    _Atomic uint32_t        stop_generation;

    /* The app's main window handle on windows, 0 on OS/X */
    HWND sys_ref;

    /* Host stuff */
    LONG host_active_inputs;
    LONG host_active_outputs;
    BOOL host_buffer_index;
    Callbacks *_Atomic host_callbacks;
    /* Live-config watcher: polls config.ini, asks the host to reset on change.
     * Heap ctx shared with the watcher thread. See struct config_watch. */
    struct config_watch   *config_watch;
    CRITICAL_SECTION       config_lock;      /* guards staged_cfg + last_error */
    struct pipeasio_config staged_cfg;       /* watcher -> apply handoff */
    _Atomic bool           config_pending;   /* staged_cfg has a fresh reload */
    _Atomic LONG           follower_quantum; /* last observed device quantum */
    LONG                   host_current_buffersize;
    _Atomic INT            host_driver_state;
    _Atomic uint64_t       host_num_samples;
    _Atomic uint32_t       host_sample_rate;
    /* Last rate the host has observed (GetSampleRate / accepted SetSampleRate /
     * a delivered sampleRateChanged).  Diverges from host_sample_rate when the
     * graph's real rate lands while callbacks are unpublished or the state is
     * not Running; the process callback delivers the missed notification. */
    _Atomic uint32_t host_announced_rate;
    TimeInformation  host_time;
    BOOL             host_time_info_mode;
    _Atomic uint64_t host_time_stamp;
    LONG             host_version;
    /* Rate the host asked for through SetSampleRate, 0 = none.  Applied on the
     * next activation when config.ini does not pin a rate of its own. */
    _Atomic uint32_t host_requested_rate;

    /* PipeASIO configuration options */
    int  pipeasio_number_inputs;
    int  pipeasio_number_outputs;
    BOOL pipeasio_connect_to_hardware;
    BOOL pipeasio_fixed_buffersize;
    BOOL pipeasio_follow_device_clock;
    BOOL pipeasio_realtime;
    LONG pipeasio_preferred_buffersize;
    int  pipeasio_sample_rate; /* 0 = follow graph */
    char pipeasio_output_device[PIPEASIO_DEVICE_NAME_MAX];
    char pipeasio_input_device[PIPEASIO_DEVICE_NAME_MAX];

    /* ASIODriverInfo.errorMessage is exactly 124 bytes in the ASIO SDK. */
    char pipeasio_last_error[PIPEASIO_ERROR_MESSAGE_SIZE];

    /* PipeWire client + discovered device ports */
    audio_client_t *audio_client;
    char            client_name[PIPEASIO_MAX_NAME_LENGTH];
    int             num_phys_input_ports;
    int             num_phys_output_ports;
    const char    **phys_input_ports;
    const char    **phys_output_ports;

    /* host process-callback buffers */
    audio_sample_t *callback_audio_buffer;
    IOChannel      *input_channel;
    IOChannel      *output_channel;
} IPipeASIOImpl;

/****************************************************************************
 *  Interface Methods
 */

HIDDEN HRESULT STDMETHODCALLTYPE QueryInterface(LPPIPEASIO iface, REFIID riid, void **ppvObject);
HIDDEN ULONG STDMETHODCALLTYPE   AddRef(LPPIPEASIO iface);
HIDDEN ULONG STDMETHODCALLTYPE   Release(LPPIPEASIO iface);
HIDDEN LONG STDMETHODCALLTYPE    Init(LPPIPEASIO iface, void *sysRef);
HIDDEN void STDMETHODCALLTYPE    GetDriverName(LPPIPEASIO iface, char *name);
HIDDEN LONG STDMETHODCALLTYPE    GetDriverVersion(LPPIPEASIO iface);
HIDDEN void STDMETHODCALLTYPE    GetErrorMessage(LPPIPEASIO iface, char *string);
HIDDEN LONG STDMETHODCALLTYPE    Start(LPPIPEASIO iface);
HIDDEN LONG STDMETHODCALLTYPE    Stop(LPPIPEASIO iface);
HIDDEN LONG STDMETHODCALLTYPE    GetChannels(LPPIPEASIO iface, LONG *numInputChannels,
                                             LONG *numOutputChannels);
HIDDEN LONG STDMETHODCALLTYPE    GetLatencies(LPPIPEASIO iface, LONG *inputLatency,
                                              LONG *outputLatency);
HIDDEN LONG STDMETHODCALLTYPE    GetBufferSize(LPPIPEASIO iface, LONG *minSize, LONG *maxSize,
                                               LONG *preferredSize, LONG *granularity);
HIDDEN LONG STDMETHODCALLTYPE    CanSampleRate(LPPIPEASIO iface, double sampleRate);
HIDDEN LONG STDMETHODCALLTYPE    GetSampleRate(LPPIPEASIO iface, double *sampleRate);
HIDDEN LONG STDMETHODCALLTYPE    SetSampleRate(LPPIPEASIO iface, double sampleRate);
HIDDEN LONG STDMETHODCALLTYPE    GetClockSources(LPPIPEASIO iface, void *clocks, LONG *numSources);
HIDDEN LONG STDMETHODCALLTYPE    SetClockSource(LPPIPEASIO iface, LONG index);
HIDDEN LONG STDMETHODCALLTYPE    GetSamplePosition(LPPIPEASIO iface, w_int64_t *sPos,
                                                   w_int64_t *tStamp);
HIDDEN LONG STDMETHODCALLTYPE    GetChannelInfo(LPPIPEASIO iface, void *info);
HIDDEN LONG STDMETHODCALLTYPE    CreateBuffers(LPPIPEASIO iface, BufferInformation *bufferInfo,
                                               LONG numChannels, LONG bufferSize,
                                               Callbacks *callbacks);
HIDDEN LONG STDMETHODCALLTYPE    DisposeBuffers(LPPIPEASIO iface);
HIDDEN LONG STDMETHODCALLTYPE    ControlPanel(LPPIPEASIO iface);
HIDDEN LONG STDMETHODCALLTYPE    Future(LPPIPEASIO iface, LONG selector, void *opt);
HIDDEN LONG STDMETHODCALLTYPE    OutputReady(LPPIPEASIO iface);

/*
 * thiscall wrappers for the vtbl (as seen from app side 32bit)
 */

HIDDEN void __thiscall_Init(void);
HIDDEN void __thiscall_GetDriverName(void);
HIDDEN void __thiscall_GetDriverVersion(void);
HIDDEN void __thiscall_GetErrorMessage(void);
HIDDEN void __thiscall_Start(void);
HIDDEN void __thiscall_Stop(void);
HIDDEN void __thiscall_GetChannels(void);
HIDDEN void __thiscall_GetLatencies(void);
HIDDEN void __thiscall_GetBufferSize(void);
HIDDEN void __thiscall_CanSampleRate(void);
HIDDEN void __thiscall_GetSampleRate(void);
HIDDEN void __thiscall_SetSampleRate(void);
HIDDEN void __thiscall_GetClockSources(void);
HIDDEN void __thiscall_SetClockSource(void);
HIDDEN void __thiscall_GetSamplePosition(void);
HIDDEN void __thiscall_GetChannelInfo(void);
HIDDEN void __thiscall_CreateBuffers(void);
HIDDEN void __thiscall_DisposeBuffers(void);
HIDDEN void __thiscall_ControlPanel(void);
HIDDEN void __thiscall_Future(void);
HIDDEN void __thiscall_OutputReady(void);

/*
 *  ASIO process callbacks
 */

static inline int process_callback(audio_nframes_t nframes, void *arg);
static inline int sample_rate_callback(audio_nframes_t nframes, void *arg);

/*
 *  Support functions
 */

HRESULT WINAPI      PipeASIOCreateInstance(REFIID riid, LPVOID *ppobj);
static VOID         configure_driver(IPipeASIOImpl *This);
static DWORD WINAPI lifecycle_worker(void *arg);
static bool         method_begin(IPipeASIOImpl *This, bool identity, method_token *token);
static void         method_end(method_token *token);
static bool         drain_gate(IPipeASIOImpl *This, pipeasio_admission_gate *gate, HANDLE idle,
                               uint32_t target);
static bool         wait_stop_generation(IPipeASIOImpl *This, uint32_t observed);
static LONG         stop_admitted(IPipeASIOImpl *This);
extern void         pipeasio_object_created(void);
extern void         pipeasio_object_destroyed(void);

static const IPipeASIOVtbl PipeASIO_Vtbl = { (void *)QueryInterface,
                                             (void *)AddRef,
                                             (void *)Release,

                                             (void *)THISCALL(Init),
                                             (void *)THISCALL(GetDriverName),
                                             (void *)THISCALL(GetDriverVersion),
                                             (void *)THISCALL(GetErrorMessage),
                                             (void *)THISCALL(Start),
                                             (void *)THISCALL(Stop),
                                             (void *)THISCALL(GetChannels),
                                             (void *)THISCALL(GetLatencies),
                                             (void *)THISCALL(GetBufferSize),
                                             (void *)THISCALL(CanSampleRate),
                                             (void *)THISCALL(GetSampleRate),
                                             (void *)THISCALL(SetSampleRate),
                                             (void *)THISCALL(GetClockSources),
                                             (void *)THISCALL(SetClockSource),
                                             (void *)THISCALL(GetSamplePosition),
                                             (void *)THISCALL(GetChannelInfo),
                                             (void *)THISCALL(CreateBuffers),
                                             (void *)THISCALL(DisposeBuffers),
                                             (void *)THISCALL(ControlPanel),
                                             (void *)THISCALL(Future),
                                             (void *)THISCALL(OutputReady) };
static _Thread_local pipeasio_host_call_token *host_token_top;

static pipeasio_gate_owner
next_gate_owner(IPipeASIOImpl *This)
{
    uint32_t owner = atomic_fetch_add_explicit(&This->gate_owner_seq, 1, memory_order_relaxed) + 1;
    if (!owner || owner > PIPEASIO_GATE_OWNER_MAX)
    {
        uint32_t expected = owner;
        atomic_compare_exchange_strong(&This->gate_owner_seq, &expected, 1);
        owner = 1;
    }
    return owner;
}

static bool
claim_closed_gate(pipeasio_admission_gate *gate, pipeasio_gate_owner owner)
{
    uint64_t            word = atomic_load_explicit(&gate->word, memory_order_acquire);
    pipeasio_gate_owner prior;
    if (!(word & PIPEASIO_GATE_CLOSED_BIT) || (word & PIPEASIO_GATE_PERMANENT_BIT))
        return false;
    prior = pipeasio_gate_word_owner(word);
    if (prior == owner)
        return true;
    if (!prior)
    {
        uint64_t next = (word & (PIPEASIO_GATE_COUNT_MASK | PIPEASIO_GATE_CLOSED_BIT))
                        | pipeasio_gate_owner_bits(owner);
        return atomic_compare_exchange_strong_explicit(&gate->word, &word, next,
                                                       memory_order_acq_rel, memory_order_acquire);
    }
    return pipeasio_gate_handoff(gate, prior, owner);
}

static bool
method_begin(IPipeASIOImpl *This, bool identity, method_token *token)
{
    bool entered   = identity ? pipeasio_gate_try_enter_identity(&This->method_gate)
                              : pipeasio_gate_try_enter(&This->method_gate);
    token->owner   = This;
    token->counted = entered;
    if (!entered)
        return false;
    if (atomic_load_explicit(&This->host_driver_state, memory_order_acquire) == Destroying)
    {
        method_end(token);
        return false;
    }
    return true;
}

#ifdef PIPEASIO_TEST_GATE_BARRIER
static void gate_leave_test_barrier(void);
static void gate_drain_test_signal(void);
#endif

static void
method_end(method_token *token)
{
    IPipeASIOImpl *This;
    if (!token || !token->counted)
        return;
    This           = token->owner;
    token->counted = false;
    if (pipeasio_gate_is_closed(&This->method_gate))
        SetEvent(This->method_idle);
#ifdef PIPEASIO_TEST_GATE_BARRIER
    gate_leave_test_barrier();
#endif
    pipeasio_gate_leave(&This->method_gate);
}

static bool
drain_gate(IPipeASIOImpl *This, pipeasio_admission_gate *gate, HANDLE idle, uint32_t target)
{
    (void)This;
    DWORD deadline = GetTickCount() + 10000;
    for (;;)
    {
        if (pipeasio_gate_count(gate) == target)
            return true;
        ResetEvent(idle);
        if (pipeasio_gate_count(gate) == target)
            return true;
        /* Leavers signal before decrementing so the count still guards this
         * event's lifetime, but a leaver can still miss its signal (it checked
         * the gate open just before we closed).  The event is a latency hint
         * only: bounded slices re-check the count, so a lost signal costs one
         * slice while a genuinely stuck gate still times out. */
        DWORD now = GetTickCount();
        if ((LONG)(now - deadline) >= 0)
            return false;
        DWORD slice = deadline - now;
        if (slice > 100)
            slice = 100;
#ifdef PIPEASIO_TEST_GATE_BARRIER
        /* Tell the interleave test the drain is asleep on the event: only an
         * unsignaled decrement landing here exercises the lost-wakeup path. */
        gate_drain_test_signal();
#endif
        WaitForSingleObject(idle, slice);
    }
}

static bool
wait_stop_generation(IPipeASIOImpl *This, uint32_t observed)
{
    DWORD started = GetTickCount();

    for (;;)
    {
        if (atomic_load_explicit(&This->stop_generation, memory_order_acquire) != observed)
            return true;
        if (atomic_load_explicit(&This->host_driver_state, memory_order_acquire) == Destroying)
            return false;

        DWORD elapsed = GetTickCount() - started;
        if (elapsed >= 10000)
            return false;
        if (!WaitOnAddress((volatile void *)&This->stop_generation, &observed, sizeof observed,
                           10000 - elapsed)
            && GetLastError() != ERROR_TIMEOUT)
            return false;
    }
}

#ifdef PIPEASIO_TEST_STOP_BARRIER
static void
stop_completion_test_barrier(void)
{
    static LONG used;
    char        entered_name[128];
    char        release_name[128];

    if (InterlockedCompareExchange(&used, 1, 0) != 0
        || !GetEnvironmentVariableA("PIPEASIO_TEST_STOP_ENTERED", entered_name, sizeof entered_name)
        || !GetEnvironmentVariableA("PIPEASIO_TEST_STOP_RELEASE", release_name,
                                    sizeof release_name))
        return;

    HANDLE entered = OpenEventA(EVENT_MODIFY_STATE, FALSE, entered_name);
    HANDLE release = OpenEventA(SYNCHRONIZE, FALSE, release_name);
    if (entered && release)
    {
        SetEvent(entered);
        WaitForSingleObject(release, 10000);
    }
    if (entered)
        CloseHandle(entered);
    if (release)
        CloseHandle(release);
}
#endif

#ifdef PIPEASIO_TEST_GATE_BARRIER
/* Signal the test event named by PIPEASIO_TEST_GATE_DRAIN each time a drain
 * is about to sleep.  Lazily opened. Env is armed only for the interleave. */
static void
gate_drain_test_signal(void)
{
    static HANDLE drain_event;
    static LONG   opened;
    char          drain_name[128];

    if (drain_event)
    {
        SetEvent(drain_event);
        return;
    }
    /* Env before the one-shot CAS: earlier drains (e.g. Init's) can run
     * before the probe arms the test, and must not consume it. */
    if (!GetEnvironmentVariableA("PIPEASIO_TEST_GATE_DRAIN", drain_name, sizeof drain_name)
        || InterlockedCompareExchange(&opened, 1, 0) != 0)
        return;
    drain_event = OpenEventA(EVENT_MODIFY_STATE, FALSE, drain_name);
    if (drain_event)
        SetEvent(drain_event);
    else
        InterlockedExchange(&opened, 0); /* let a later slice retry */
}

/* One-shot stall between method_end's is_closed check and its gate_leave.
 * Armed while the gate is open, it forces the lost-wakeup interleaving the
 * sliced drain_gate tolerates: the leaver decrements without ever signaling. */
static void
gate_leave_test_barrier(void)
{
    static LONG used;
    char        entered_name[128];
    char        release_name[128];

    /* Env first: the one-shot must survive method_ends that ran before the
     * probe armed it (the CAS alone would consume it on the first call). */
    if (!GetEnvironmentVariableA("PIPEASIO_TEST_GATE_ENTERED", entered_name, sizeof entered_name)
        || !GetEnvironmentVariableA("PIPEASIO_TEST_GATE_RELEASE", release_name, sizeof release_name)
        || InterlockedCompareExchange(&used, 1, 0) != 0)
        return;

    HANDLE entered = OpenEventA(EVENT_MODIFY_STATE, FALSE, entered_name);
    HANDLE release = OpenEventA(SYNCHRONIZE, FALSE, release_name);
    if (entered && release)
    {
        SetEvent(entered);
        WaitForSingleObject(release, 10000);
    }
    if (entered)
        CloseHandle(entered);
    if (release)
        CloseHandle(release);
}
#endif

bool
pipeasio_host_call_begin(void *owner, pipeasio_host_call_kind kind, pipeasio_host_call_token *token)
{
    IPipeASIOImpl *This = owner;
    Callbacks     *callbacks;
    INT            state;

    memset(token, 0, sizeof *token);
    token->owner      = owner;
    token->gate       = &This->host_gate;
    token->idle_event = This->host_idle;
    token->kind       = kind;
    if (!pipeasio_gate_try_enter(&This->host_gate))
        return false;
    token->counted = true;

    state = atomic_load_explicit(&This->host_driver_state, memory_order_acquire);
    if ((kind == PIPEASIO_HOST_PROCESS && state != Running)
        || (kind == PIPEASIO_HOST_SAMPLE_RATE && state != Running)
        || (kind == PIPEASIO_HOST_CONFIG_RESET && state != Prepared && state != Running)
        || (kind == PIPEASIO_HOST_TIME_INFO && state != Preparing))
        return false;

    callbacks = atomic_load_explicit(&This->host_callbacks, memory_order_acquire);
    if (!callbacks)
        return false;
    token->callbacks = callbacks;
    token->previous  = host_token_top;
    host_token_top   = token;
    token->admitted  = true;
    return true;
}

void
pipeasio_host_call_end(pipeasio_host_call_token *token)
{
    IPipeASIOImpl *This;
    if (!token)
        return;
    This = token->owner;
    if (token->admitted)
    {
        host_token_top  = token->previous;
        token->admitted = false;
    }
    if (!token->counted)
        return;
    token->counted = false;
    if (pipeasio_gate_is_closed(&This->host_gate))
        SetEvent(This->host_idle);
    pipeasio_gate_leave(&This->host_gate);
}

/* Prevent an inlined SysV TLS lookup from clobbering Windows-ABI `This`. */
#if defined(__GNUC__)
__attribute__((noinline))
#endif
bool
pipeasio_host_call_is_reentrant(void *owner)
{
    pipeasio_host_call_token *token;
    for (token = host_token_top; token; token = token->previous)
        if (token->owner == owner)
            return true;
    return false;
}

void
pipeasio_host_call_process(pipeasio_host_call_token *token, int32_t buffer_index,
                           audio_nframes_t add_samples, uint64_t time_nsec)
{
    IPipeASIOImpl *This    = token->owner;
    Callbacks     *cb      = token->callbacks;
    uint64_t       samples = atomic_load_explicit(&This->host_num_samples, memory_order_relaxed);

    atomic_store_explicit(&This->host_time_stamp, time_nsec, memory_order_relaxed);
    if (This->host_time_info_mode)
    {
        This->host_time._2            = 1.0;
        This->host_time.numSamples.lo = (ULONG)(samples & 0xFFFFFFFFu);
        This->host_time.numSamples.hi = (ULONG)(samples >> 32);
        This->host_time.timeStamp.lo  = (ULONG)(time_nsec & 0xFFFFFFFFu);
        This->host_time.timeStamp.hi  = (ULONG)(time_nsec >> 32);
        This->host_time.sampleRate
                = (double)atomic_load_explicit(&This->host_sample_rate, memory_order_acquire);
        This->host_time.flags = 0x7;
        cb->swapBuffersWithTimeInfo(&This->host_time, buffer_index, 1);
    }
    else
    {
        cb->swapBuffers(buffer_index, 1);
    }
    atomic_store_explicit(&This->host_num_samples, samples + add_samples, memory_order_relaxed);
}

int32_t
pipeasio_host_call_notify(pipeasio_host_call_token *token, int32_t selector, int32_t value,
                          void *message, double *opt)
{
    Callbacks *cb = token->callbacks;
    return cb->sendNotification ? cb->sendNotification(selector, value, message, opt) : 0;
}

void
pipeasio_host_call_sample_rate(pipeasio_host_call_token *token, audio_nframes_t sample_rate)
{
    Callbacks *cb = token->callbacks;
    if (cb->sampleRateChanged)
        cb->sampleRateChanged((double)sample_rate);
}

/*****************************************************************************
 * Interface method definitions
 */

HIDDEN HRESULT STDMETHODCALLTYPE
QueryInterface(LPPIPEASIO iface, REFIID riid, void **ppvObject)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    method_token   token;
    ULONG          ref;

    if (!ppvObject)
        return E_POINTER;
    *ppvObject = NULL;
    if (!IsEqualIID(&CLSID_PipeASIO, riid) && !IsEqualIID(&IID_IUnknown, riid))
        return E_NOINTERFACE;
    if (!method_begin(This, true, &token))
        return E_NOINTERFACE;
    ref = AddRef(iface);
    if (ref)
        *ppvObject = This;
    method_end(&token);
    return ref ? S_OK : E_NOINTERFACE;
}

HIDDEN ULONG STDMETHODCALLTYPE
AddRef(LPPIPEASIO iface)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    ULONG          ref  = atomic_load_explicit(&This->ref, memory_order_acquire);

    while (ref)
    {
        if (atomic_load_explicit(&This->host_driver_state, memory_order_acquire) == Destroying)
            return 0;
        if (atomic_compare_exchange_weak_explicit(&This->ref, &ref, ref + 1, memory_order_acq_rel,
                                                  memory_order_acquire))
            return ref + 1;
    }
    return 0;
}

/* Refcounted: one reference for the owner, one for the watcher thread; the
 * last release frees.  `owner` stays valid because the context remains
 * attached to the object until the thread has exited, and the final teardown
 * joins the thread before anything it owns can die (see stop_config_watch). */
struct config_watch
{
    IPipeASIOImpl *owner;
    HANDLE         stop_event;
    HANDLE         thread;
    DWORD          tid;
    volatile LONG  refs;
};

static void
config_watch_release(struct config_watch *w)
{
    if (InterlockedDecrement(&w->refs) == 0)
    {
        if (w->thread)
            CloseHandle(w->thread);
        if (w->stop_event)
            CloseHandle(w->stop_event);
        HeapFree(GetProcessHeap(), 0, w);
    }
}

/* Poll config.ini and request host reset when it changes. */
static DWORD WINAPI
config_watch_proc(LPVOID arg)
{
    struct config_watch *w    = (struct config_watch *)arg;
    IPipeASIOImpl       *This = w->owner;
    char                 path[1024];
#ifndef PIPEASIO_WOW64_PE
    struct stat st;
    time_t      last_sec  = 0;
    long        last_nsec = 0;
    off_t       last_size = 0;
    ino_t       last_ino  = 0;
#else
    uint64_t last_fp = 0;
#endif
    LONG                   last_reset_quantum = 0;
    struct pipeasio_config last_cfg;

#ifndef PIPEASIO_WOW64_PE
    if (!pipeasio_config_path(path, sizeof path))
    {
        WARN("config watcher: cannot resolve config path, live reload disabled\n");
        config_watch_release(w);
        return 0;
    }
    TRACE("config watcher: watching %s\n", path);
#else
    /* PE-side getenv() cannot see $XDG_CONFIG_HOME/$HOME; the unixlib owns the
     * real path and change detection runs through
     * pipeasio_wow64_config_fingerprint(). Keep a label for traces and proceed. */
    lstrcpynA(path, "config.ini (unixlib)", sizeof path);
    TRACE("config watcher: watching config.ini via unixlib fingerprint\n");
#endif

#ifndef PIPEASIO_WOW64_PE
    if (stat(path, &st) == 0)
    {
        last_sec  = st.st_mtim.tv_sec;
        last_nsec = st.st_mtim.tv_nsec;
        last_size = st.st_size;
        last_ino  = st.st_ino;
    }
    pipeasio_config_load(&last_cfg);
#else
    last_fp = pipeasio_wow64_config_fingerprint();
    pipeasio_wow64_load_config(&last_cfg);
#endif

    for (;;)
    {
        DWORD waited = WaitForSingleObject(w->stop_event, 1000);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED)
            break;

        bool reset        = false;
        bool file_changed = false;

        /* config.ini edited in the panel */
#ifndef PIPEASIO_WOW64_PE
        if (stat(path, &st) == 0
            && (st.st_mtim.tv_sec != last_sec || st.st_mtim.tv_nsec != last_nsec
                || st.st_size != last_size || st.st_ino != last_ino))
        {
            last_sec  = st.st_mtim.tv_sec;
            last_nsec = st.st_mtim.tv_nsec;
            last_size = st.st_size;
            last_ino  = st.st_ino;
            TRACE("config watcher: %s changed\n", path);
            file_changed = true;
        }
#else
        {
            uint64_t fp = pipeasio_wow64_config_fingerprint();
            if (fp && fp != last_fp)
            {
                last_fp = fp;
                TRACE("config watcher: %s changed\n", path);
                file_changed = true;
            }
        }
#endif

        /* Reload + diff: stage only reset-worthy field changes so a no-op save
         * (or a channel/node edit that needs a full re-init) does not force a
         * needless graph teardown. */
        if (file_changed)
        {
            struct pipeasio_config newcfg;
#ifdef PIPEASIO_WOW64_PE
            pipeasio_wow64_load_config(&newcfg);
#else
            pipeasio_config_load(&newcfg);
#endif
            bool live_changed   = newcfg.buffer_size != last_cfg.buffer_size
                                  || newcfg.fixed_buffer_size != last_cfg.fixed_buffer_size
                                  || newcfg.sample_rate != last_cfg.sample_rate
                                  || newcfg.follow_device_clock != last_cfg.follow_device_clock
                                  || newcfg.realtime != last_cfg.realtime
                                  || newcfg.auto_connect != last_cfg.auto_connect
                                  || strcmp(newcfg.output_device, last_cfg.output_device) != 0
                                  || strcmp(newcfg.input_device, last_cfg.input_device) != 0;
            bool reinit_changed = newcfg.inputs != last_cfg.inputs
                                  || newcfg.outputs != last_cfg.outputs
                                  || strcmp(newcfg.node_name, last_cfg.node_name) != 0;
            if (reinit_changed)
                WARN("config: channel-count/node-name change needs driver reselect to apply\n");
            if (live_changed)
            {
                EnterCriticalSection(&This->config_lock);
                This->staged_cfg = newcfg;
                LeaveCriticalSection(&This->config_lock);
                atomic_store_explicit(&This->config_pending, true, memory_order_release);
                TRACE("config: staged live reload from %s\n", path);
                reset = true;
            }
            last_cfg = newcfg;
        }

        /* PipeWire default device switched while we are following it */
        if (This->host_driver_state == Running
            && (!This->pipeasio_output_device[0] || !This->pipeasio_input_device[0])
            && audio_default_changed(This->audio_client))
        {
            TRACE("config watcher: default device changed\n");
            reset = true;
        }

        /* Follow-device mode: re-negotiate only when the observed graph quantum
         * differs from the size we are already running.  Without the
         * host_current_buffersize guard the first observation (and every later
         * one) fires a reset even when already converged, thrashing the graph on
         * reset-honoring hosts.  apply_pending_config derives
         * host_current_buffersize from follower_quantum, so the new quantum
         * still applies on the rebuild. */
        if (This->pipeasio_follow_device_clock && This->host_driver_state == Running)
        {
            LONG q = (LONG)audio_observed_quantum(This->audio_client);
            if (q && q != This->host_current_buffersize && q != last_reset_quantum)
            {
                atomic_store(&This->follower_quantum, q);
                last_reset_quantum = q;
                TRACE("config watcher: device quantum %ld, re-negotiating buffer\n", (long)q);
                reset = true;
            }
        }

        if (reset)
        {
            pipeasio_host_call_token token;
            TRACE("config watcher: requesting host reset\n");
            if (pipeasio_host_call_begin(This, PIPEASIO_HOST_CONFIG_RESET, &token))
            {
                if (pipeasio_host_call_notify(&token, 1, 3, NULL, NULL))
                    pipeasio_host_call_notify(&token, 3, 0, NULL, NULL);
            }
            pipeasio_host_call_end(&token);
        }
        else if (This->host_driver_state == Running && audio_latency_changed(This->audio_client))
        {
            /* The device chain moved without needing a rebuild, so the host only
             * has to re-read GetLatencies.  Selector 6 is kAsioLatenciesChanged;
             * the else-if skips it when a reset is already going out, which
             * re-queries anyway. */
            pipeasio_host_call_token token;
            TRACE("config watcher: latency changed, notifying host\n");
            if (pipeasio_host_call_begin(This, PIPEASIO_HOST_CONFIG_RESET, &token))
            {
                if (pipeasio_host_call_notify(&token, 1, 6, NULL, NULL))
                    pipeasio_host_call_notify(&token, 6, 0, NULL, NULL);
            }
            pipeasio_host_call_end(&token);
        }
    }
    config_watch_release(w);
    return 0;
}

/* Signal and join the config watcher.  The context stays attached to the
 * object until the thread has actually exited: a created-but-unscheduled (or
 * notify-blocked) watcher may hold no gate admission, so the gate drains
 * cannot guard w->owner or the DLL mapping for it.  Callers that keep the
 * object alive pass wait_forever=false for best effort: the refcount lets
 * the watcher free the context on exit.  The final teardown passes
 * wait_forever=true so no watcher thread can outlive the object or the
 * module.  Idempotent: returns true once no watcher remains attached. */
static bool
stop_config_watch(IPipeASIOImpl *This, bool wait_forever)
{
    struct config_watch *w = This->config_watch;
    if (!w)
        return true;
    SetEvent(w->stop_event);
    if (w->thread && GetCurrentThreadId() != w->tid)
        WaitForSingleObject(w->thread, wait_forever ? INFINITE : 10000);
    if (w->thread && WaitForSingleObject(w->thread, 0) != WAIT_OBJECT_0)
        return false;
    This->config_watch = NULL;
    config_watch_release(w);
    return true;
}

/* Implies Stop() and DisposeBuffers(). */

HIDDEN ULONG STDMETHODCALLTYPE
Release(LPPIPEASIO iface)
{
    IPipeASIOImpl *This       = (IPipeASIOImpl *)iface;
    HANDLE         completion = NULL;
    bool           reentrant;
    ULONG          ref = atomic_load_explicit(&This->ref, memory_order_acquire);

    while (ref)
    {
        if (ref > 1)
        {
            if (atomic_compare_exchange_weak_explicit(&This->ref, &ref, ref - 1,
                                                      memory_order_acq_rel, memory_order_acquire))
                return ref - 1;
            continue;
        }
        if (!atomic_compare_exchange_weak_explicit(&This->ref, &ref, 0, memory_order_acq_rel,
                                                   memory_order_acquire))
            continue;

        reentrant = pipeasio_host_call_is_reentrant(This);
        if (!reentrant)
            DuplicateHandle(GetCurrentProcess(), This->worker, GetCurrentProcess(), &completion,
                            SYNCHRONIZE, FALSE, 0);

        /* Keep the object alive while publishing the final Destroying wake. */
        atomic_fetch_add_explicit(&This->lifecycle_waiters, 1, memory_order_acq_rel);
        atomic_exchange_explicit(&This->host_driver_state, Destroying, memory_order_acq_rel);
        pipeasio_gate_close_permanently(&This->method_gate);
        pipeasio_gate_close_permanently(&This->host_gate);
        SetEvent(This->method_idle);
        SetEvent(This->host_idle);
        WakeByAddressAll((void *)&This->stop_generation);
        SetEvent(This->work_event);
        atomic_fetch_sub_explicit(&This->lifecycle_waiters, 1, memory_order_release);
        if (completion)
        {
            WaitForSingleObject(completion, 10000);
            CloseHandle(completion);
        }
        return 0;
    }
    return 0;
}
static void
destroy_driver_resources(IPipeASIOImpl *This)
{
    /* Final: no watcher thread may outlive the object or the module. */
    stop_config_watch(This, true);
    if (This->audio_client)
    {
        audio_deactivate(This->audio_client);
        audio_close(This->audio_client);
        This->audio_client = NULL;
    }
    atomic_store_explicit(&This->host_callbacks, NULL, memory_order_release);
    if (This->callback_audio_buffer)
    {
        HeapFree(GetProcessHeap(), 0, This->callback_audio_buffer);
        This->callback_audio_buffer = NULL;
    }
    audio_free_ports(This->phys_input_ports);
    audio_free_ports(This->phys_output_ports);
    This->phys_input_ports  = NULL;
    This->phys_output_ports = NULL;
    if (This->input_channel)
    {
        HeapFree(GetProcessHeap(), 0, This->input_channel);
        This->input_channel  = NULL;
        This->output_channel = NULL;
    }
}

static DWORD WINAPI
lifecycle_worker(void *arg)
{
    IPipeASIOImpl *This = arg;

    for (;;)
    {
        WaitForSingleObject(This->work_event, INFINITE);
        bool completed = false;
        if (atomic_load_explicit(&This->host_driver_state, memory_order_acquire) == Stopping)
            while (!drain_gate(This, &This->host_gate, This->host_idle, 0))
                ;
        AcquireSRWLockExclusive(&This->lifecycle_lock);
        INT expected = Stopping;
        if (atomic_compare_exchange_strong_explicit(&This->host_driver_state, &expected, Prepared,
                                                    memory_order_acq_rel, memory_order_acquire))
        {
            atomic_fetch_add_explicit(&This->stop_generation, 1, memory_order_release);
            completed = true;
        }
        ReleaseSRWLockExclusive(&This->lifecycle_lock);
#ifdef PIPEASIO_TEST_STOP_BARRIER
        if (completed)
            stop_completion_test_barrier();
#endif
        if (completed)
            WakeByAddressAll((void *)&This->stop_generation);
        if (atomic_load_explicit(&This->host_driver_state, memory_order_acquire) != Destroying)
            continue;

        stop_config_watch(This, true);
        while (!drain_gate(This, &This->method_gate, This->method_idle, 0))
            ;
        while (!drain_gate(This, &This->host_gate, This->host_idle, 0))
            ;
        while (atomic_load_explicit(&This->lifecycle_waiters, memory_order_acquire) != 0)
            Sleep(1);

        destroy_driver_resources(This);
        DeleteCriticalSection(&This->config_lock);

        HANDLE  method_idle = This->method_idle;
        HANDLE  host_idle   = This->host_idle;
        HANDLE  work_event  = This->work_event;
        HANDLE  worker      = This->worker;
        HMODULE module      = This->module_pin;

        pipeasio_object_destroyed();
        HeapFree(GetProcessHeap(), 0, This);
        CloseHandle(method_idle);
        CloseHandle(host_idle);
        CloseHandle(work_event);
        if (worker)
            CloseHandle(worker);
        if (module)
            FreeLibraryAndExitThread(module, 0);
        ExitThread(0);
    }
}

static void
clear_last_error(IPipeASIOImpl *This)
{
    EnterCriticalSection(&This->config_lock);
    This->pipeasio_last_error[0] = '\0';
    LeaveCriticalSection(&This->config_lock);
}

static void
set_last_error(IPipeASIOImpl *This, const char *fmt, ...)
{
    char    message[sizeof This->pipeasio_last_error] = { 0 };
    va_list args;

    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);
    message[sizeof message - 1] = '\0';

    EnterCriticalSection(&This->config_lock);
    memcpy(This->pipeasio_last_error, message, sizeof message);
    LeaveCriticalSection(&This->config_lock);
}

/* The rate handed to the backend on the next activation.  config.ini's
 * sample_rate is an explicit user pin and outranks the host; with no pin (0 =
 * follow graph) a rate the host set through SetSampleRate applies instead. */
static audio_nframes_t
effective_forced_rate(IPipeASIOImpl *This)
{
    if (This->pipeasio_sample_rate)
        return (audio_nframes_t)This->pipeasio_sample_rate;
    return (audio_nframes_t)atomic_load_explicit(&This->host_requested_rate, memory_order_acquire);
}

/* sysRef is 0 on OS/X; on Windows it is the application's main window handle.
 * Returns 0 on error, 1 on success. */

DEFINE_THISCALL_WRAPPER(Init, 8)
HIDDEN LONG STDMETHODCALLTYPE
Init(LPPIPEASIO iface, void *sysRef)
{
    IPipeASIOImpl      *This = (IPipeASIOImpl *)iface;
    method_token        token;
    pipeasio_gate_owner owner;
    uint32_t            audio_status = AUDIO_STATUS_OK;
    INT                 expected     = Loaded;
    int                 total;

    if (!method_begin(This, false, &token))
        return 0;
    if (!atomic_compare_exchange_strong_explicit(&This->host_driver_state, &expected, Initializing,
                                                 memory_order_acq_rel, memory_order_acquire))
    {
        method_end(&token);
        return 0;
    }

    /* Open a debug log with the build the host actually loaded: a report then
     * names the version and which half of a WoW64 pair is talking. */
#ifdef PIPEASIO_WOW64_PE
    TRACE("PipeASIO " PIPEASIO_VERSION " (32-bit WoW64 front end)\n");
#else
    TRACE("PipeASIO " PIPEASIO_VERSION " (64-bit)\n");
#endif

    clear_last_error(This);
    owner = next_gate_owner(This);
    if (!pipeasio_gate_close(&This->method_gate, owner)
        || !drain_gate(This, &This->method_gate, This->method_idle, 1)
        || !pipeasio_gate_reopen(&This->method_gate, owner))
    {
        set_last_error(This, "could not acquire the driver method gate");
        goto fail;
    }

    configure_driver(This);
    This->sys_ref = sysRef;
    total         = This->pipeasio_number_inputs + This->pipeasio_number_outputs;
    if (total <= 0 || total > PIPEASIO_MAX_CHANNELS * 2)
    {
        set_last_error(This, "invalid channel configuration: %d inputs + %d outputs",
                       This->pipeasio_number_inputs, This->pipeasio_number_outputs);
        goto fail;
    }

    This->audio_client = audio_open(This->client_name, AUDIO_NULL_OPTION, &audio_status);
    if (!This->audio_client)
    {
        switch (audio_status)
        {
        case AUDIO_STATUS_NO_UNIXLIB:
            set_last_error(This, "the 64-bit unixlib is unavailable; 32-bit hosts need Wine's "
                                 "new WoW64 mode (Wine 11 / Proton 11)");
            break;
        case AUDIO_STATUS_NO_DAEMON:
            set_last_error(This, "cannot connect to the PipeWire daemon (is it running and "
                                 "visible inside the Proton container?)");
            break;
        case AUDIO_STATUS_NO_CONTEXT:
            set_last_error(This, "failed to set up the PipeWire context (broken installation?)");
            break;
        case AUDIO_STATUS_NO_MEMORY:
            set_last_error(This, "out of memory");
            break;
        default:
            set_last_error(This, "failed to initialize the PipeWire client; check the Wine log");
            break;
        }
        goto fail;
    }

    audio_set_forced_rate(This->audio_client, effective_forced_rate(This));
    audio_set_follow_device(This->audio_client, This->pipeasio_follow_device_clock);
    audio_set_realtime(This->audio_client, This->pipeasio_realtime);
    atomic_store_explicit(&This->host_sample_rate, audio_get_sample_rate(This->audio_client),
                          memory_order_release);
    This->host_current_buffersize = This->pipeasio_preferred_buffersize;
    if (This->pipeasio_follow_device_clock)
    {
        LONG hint = atomic_load_explicit(&This->follower_quantum, memory_order_acquire);
        if (hint)
            This->host_current_buffersize = hint;
    }

    This->input_channel = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    (size_t)total * sizeof(*This->input_channel));
    if (!This->input_channel)
    {
        set_last_error(This, "out of memory allocating channel buffers");
        goto fail;
    }
    This->output_channel = This->input_channel + This->pipeasio_number_inputs;

    for (int i = 0; i < This->pipeasio_number_inputs; ++i)
    {
        snprintf(This->input_channel[i].port_name, sizeof This->input_channel[i].port_name, "in_%i",
                 i + 1);
        This->input_channel[i].port
                = audio_port_register(This->audio_client, This->input_channel[i].port_name,
                                      AUDIO_PORT_IS_INPUT, (uint32_t)i);
        if (!This->input_channel[i].port)
        {
            set_last_error(This, "failed to register PipeWire input port %d", i + 1);
            goto fail;
        }
    }
    for (int i = 0; i < This->pipeasio_number_outputs; ++i)
    {
        snprintf(This->output_channel[i].port_name, sizeof This->output_channel[i].port_name,
                 "out_%i", i + 1);
        This->output_channel[i].port
                = audio_port_register(This->audio_client, This->output_channel[i].port_name,
                                      AUDIO_PORT_IS_OUTPUT, (uint32_t)i);
        if (!This->output_channel[i].port)
        {
            set_last_error(This, "failed to register PipeWire output port %d", i + 1);
            goto fail;
        }
    }
    if (!audio_set_process_callback(This->audio_client, process_callback, This)
        || !audio_set_sample_rate_callback(This->audio_client, sample_rate_callback, This))
    {
        set_last_error(This, "failed to install PipeWire callbacks; check the Wine log");
        goto fail;
    }

    expected = Initializing;
    if (!atomic_compare_exchange_strong_explicit(&This->host_driver_state, &expected, Initialized,
                                                 memory_order_release, memory_order_acquire))
    {
        set_last_error(This, "driver state changed during initialization");
        goto fail;
    }
    method_end(&token);
    return 1;

fail:
    if (This->audio_client)
    {
        audio_close(This->audio_client);
        This->audio_client = NULL;
    }
    if (This->input_channel)
    {
        HeapFree(GetProcessHeap(), 0, This->input_channel);
        This->input_channel  = NULL;
        This->output_channel = NULL;
    }
    expected = Initializing;
    atomic_compare_exchange_strong_explicit(&This->host_driver_state, &expected, Loaded,
                                            memory_order_release, memory_order_acquire);
    if (!pipeasio_gate_is_permanent(&This->method_gate)
        && pipeasio_gate_is_closed(&This->method_gate))
        pipeasio_gate_reopen(&This->method_gate, owner);
    method_end(&token);
    return 0;
}

DEFINE_THISCALL_WRAPPER(GetDriverName, 8)
HIDDEN void STDMETHODCALLTYPE
GetDriverName(LPPIPEASIO iface, char *name)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    method_token   token;
    if (!name || !method_begin(This, true, &token))
        return;
    strcpy(name, "PipeASIO");
    method_end(&token);
}

DEFINE_THISCALL_WRAPPER(GetDriverVersion, 4)
HIDDEN LONG STDMETHODCALLTYPE
GetDriverVersion(LPPIPEASIO iface)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    method_token   token;
    LONG           version;
    if (!method_begin(This, true, &token))
        return -1000;
    version = This->host_version;
    method_end(&token);
    return version;
}

DEFINE_THISCALL_WRAPPER(GetErrorMessage, 8)
HIDDEN void STDMETHODCALLTYPE
GetErrorMessage(LPPIPEASIO iface, char *string)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    method_token   token;
    char           message[sizeof This->pipeasio_last_error];

    if (!string || !method_begin(This, true, &token))
        return;
    EnterCriticalSection(&This->config_lock);
    memcpy(message, This->pipeasio_last_error, sizeof message);
    LeaveCriticalSection(&This->config_lock);
    if (message[0])
        lstrcpynA(string, message, PIPEASIO_ERROR_MESSAGE_SIZE);
    else
        lstrcpynA(string, "operation failed before any detail was recorded; check the Wine log",
                  PIPEASIO_ERROR_MESSAGE_SIZE);
    method_end(&token);
}

/* Returns -1000 if IO is missing, -999 if the audio backend fails to start. */

DEFINE_THISCALL_WRAPPER(Start, 4)
HIDDEN LONG STDMETHODCALLTYPE
Start(LPPIPEASIO iface)
{
    IPipeASIOImpl      *This = (IPipeASIOImpl *)iface;
    method_token        token;
    pipeasio_gate_owner owner;
    INT                 expected;
    size_t              samples;

    if (!method_begin(This, false, &token))
        return -1000;
    clear_last_error(This);
    if (pipeasio_host_call_is_reentrant(This))
    {
        set_last_error(This, "Start cannot run from an ASIO host callback");
        method_end(&token);
        return -1000;
    }

    for (;;)
    {
        AcquireSRWLockExclusive(&This->lifecycle_lock);
        expected = atomic_load_explicit(&This->host_driver_state, memory_order_acquire);
        if (expected == Prepared
            && atomic_compare_exchange_strong_explicit(&This->host_driver_state, &expected,
                                                       Starting, memory_order_acq_rel,
                                                       memory_order_acquire))
        {
            ReleaseSRWLockExclusive(&This->lifecycle_lock);
            break;
        }
        if (expected != Stopping)
        {
            ReleaseSRWLockExclusive(&This->lifecycle_lock);
            set_last_error(This, "driver buffers are not prepared");
            method_end(&token);
            return -1000;
        }

        uint32_t observed = atomic_load_explicit(&This->stop_generation, memory_order_acquire);
        atomic_fetch_add_explicit(&This->lifecycle_waiters, 1, memory_order_acq_rel);
        ReleaseSRWLockExclusive(&This->lifecycle_lock);
        method_end(&token);
        bool completed = wait_stop_generation(This, observed);
        if (!method_begin(This, false, &token))
        {
            atomic_fetch_sub_explicit(&This->lifecycle_waiters, 1, memory_order_release);
            return -1000;
        }
        atomic_fetch_sub_explicit(&This->lifecycle_waiters, 1, memory_order_release);
        if (!completed)
        {
            set_last_error(This, "timed out waiting for the previous Stop");
            method_end(&token);
            return -1000;
        }
    }

    owner = next_gate_owner(This);
    if (!pipeasio_gate_close(&This->method_gate, owner)
        || !drain_gate(This, &This->method_gate, This->method_idle, 1)
        || !pipeasio_gate_reopen(&This->method_gate, owner)
        || !claim_closed_gate(&This->host_gate, owner)
        || !drain_gate(This, &This->host_gate, This->host_idle, 0))
    {
        set_last_error(This, "could not acquire the driver callback gates");
        goto fail;
    }

    samples = (size_t)(This->pipeasio_number_inputs + This->pipeasio_number_outputs) * 2
              * (size_t)This->host_current_buffersize;
    memset(This->callback_audio_buffer, 0, samples * sizeof(*This->callback_audio_buffer));
    This->host_buffer_index = 0;
    atomic_store_explicit(&This->host_num_samples, 0, memory_order_relaxed);
    atomic_store_explicit(&This->host_time_stamp, 0, memory_order_relaxed);
#ifdef PIPEASIO_WOW64_PE
    {
        bool in_active[PIPEASIO_MAX_CHANNELS]  = { false };
        bool out_active[PIPEASIO_MAX_CHANNELS] = { false };
        for (int i = 0; i < This->pipeasio_number_inputs; ++i)
            in_active[i] = This->input_channel[i].active;
        for (int i = 0; i < This->pipeasio_number_outputs; ++i)
            out_active[i] = This->output_channel[i].active;
        if (!pipeasio_wow64_bind_rt(This->audio_client, This->callback_audio_buffer,
                                    This->host_current_buffersize, This->pipeasio_number_inputs,
                                    This->pipeasio_number_outputs, in_active, out_active))
        {
            set_last_error(This, "failed to bind the WoW64 callback buffer");
            goto fail;
        }
    }
#endif

    if (!pipeasio_gate_reopen(&This->host_gate, owner))
    {
        set_last_error(This, "could not open the driver callback gate");
        goto fail;
    }
    expected = Starting;
    if (!atomic_compare_exchange_strong_explicit(&This->host_driver_state, &expected, Running,
                                                 memory_order_release, memory_order_acquire))
    {
        pipeasio_gate_close(&This->host_gate, owner);
        set_last_error(This, "driver state changed while starting");
        goto fail;
    }
    method_end(&token);
    return 0;

fail:
    expected = Starting;
    atomic_compare_exchange_strong_explicit(&This->host_driver_state, &expected, Prepared,
                                            memory_order_release, memory_order_acquire);
    if (!pipeasio_gate_is_permanent(&This->method_gate)
        && pipeasio_gate_is_closed(&This->method_gate))
        pipeasio_gate_reopen(&This->method_gate, owner);
    method_end(&token);
    return -999;
}

/* Returns -1000 if IO is missing. swapBuffers() must not be called after this returns. */

static LONG
stop_admitted(IPipeASIOImpl *This)
{
    bool     start_worker = false;
    uint32_t observed;

    AcquireSRWLockExclusive(&This->lifecycle_lock);
    INT state = atomic_load_explicit(&This->host_driver_state, memory_order_acquire);
    observed  = atomic_load_explicit(&This->stop_generation, memory_order_acquire);
    if (state == Running)
    {
        pipeasio_gate_owner owner = next_gate_owner(This);
        if (!atomic_compare_exchange_strong_explicit(&This->host_driver_state, &state, Stopping,
                                                     memory_order_acq_rel, memory_order_acquire)
            || !pipeasio_gate_close(&This->host_gate, owner))
        {
            ReleaseSRWLockExclusive(&This->lifecycle_lock);
            return -1000;
        }
        start_worker = true;
    }
    else if (state != Stopping)
    {
        ReleaseSRWLockExclusive(&This->lifecycle_lock);
        return -1000;
    }
    ReleaseSRWLockExclusive(&This->lifecycle_lock);

    if (start_worker)
        SetEvent(This->work_event);
    if (pipeasio_host_call_is_reentrant(This))
        return 0;
    return wait_stop_generation(This, observed) ? 0 : -1000;
}

DEFINE_THISCALL_WRAPPER(Stop, 4)
HIDDEN LONG STDMETHODCALLTYPE
Stop(LPPIPEASIO iface)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    method_token   token;
    bool           start_worker = false;
    uint32_t       observed;

    if (!method_begin(This, false, &token))
        return -1000;

    AcquireSRWLockExclusive(&This->lifecycle_lock);
    INT state = atomic_load_explicit(&This->host_driver_state, memory_order_acquire);
    observed  = atomic_load_explicit(&This->stop_generation, memory_order_acquire);
    if (state == Running)
    {
        INT                 expected = Running;
        pipeasio_gate_owner owner    = next_gate_owner(This);
        if (!atomic_compare_exchange_strong_explicit(&This->host_driver_state, &expected, Stopping,
                                                     memory_order_acq_rel, memory_order_acquire)
            || !pipeasio_gate_close(&This->host_gate, owner))
        {
            ReleaseSRWLockExclusive(&This->lifecycle_lock);
            method_end(&token);
            return -1000;
        }
        start_worker = true;
    }
    else if (state != Stopping)
    {
        ReleaseSRWLockExclusive(&This->lifecycle_lock);
        method_end(&token);
        return -1000;
    }

    if (pipeasio_host_call_is_reentrant(This))
    {
        ReleaseSRWLockExclusive(&This->lifecycle_lock);
        if (start_worker)
            SetEvent(This->work_event);
        method_end(&token);
        return 0;
    }
    atomic_fetch_add_explicit(&This->lifecycle_waiters, 1, memory_order_acq_rel);
    ReleaseSRWLockExclusive(&This->lifecycle_lock);

    if (start_worker)
        SetEvent(This->work_event);
    method_end(&token);
    LONG result = wait_stop_generation(This, observed) ? 0 : -1000;
    atomic_fetch_sub_explicit(&This->lifecycle_waiters, 1, memory_order_release);
    return result;
}

/* Returns -1000 if no channels are available, otherwise AES_OK. */

DEFINE_THISCALL_WRAPPER(GetChannels, 12)
HIDDEN LONG STDMETHODCALLTYPE
GetChannels(LPPIPEASIO iface, LONG *numInputChannels, LONG *numOutputChannels)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    method_token   token;
    LONG           result;
    if (!numInputChannels || !numOutputChannels)
        return -998;
    if (!method_begin(This, false, &token))
        return -1000;
    INT state = atomic_load_explicit(&This->host_driver_state, memory_order_acquire);
    if (state != Initialized && state != Prepared && state != Running)
        result = -1000;
    else
    {
        *numInputChannels  = This->pipeasio_number_inputs;
        *numOutputChannels = This->pipeasio_number_outputs;
        result             = (*numInputChannels || *numOutputChannels) ? 0 : -1000;
    }
    method_end(&token);
    return result;
}

/* Returns -1000 if no IO is available, otherwise AES_OK. */

DEFINE_THISCALL_WRAPPER(GetLatencies, 12)
HIDDEN LONG STDMETHODCALLTYPE
GetLatencies(LPPIPEASIO iface, LONG *inputLatency, LONG *outputLatency)
{
    IPipeASIOImpl        *This = (IPipeASIOImpl *)iface;
    method_token          token;
    audio_latency_range_t range = { 0, 0 };
    if (!inputLatency || !outputLatency)
        return -998;
    if (!method_begin(This, false, &token))
        return -1000;
    INT state = atomic_load_explicit(&This->host_driver_state, memory_order_acquire);
    if (state != Initialized && state != Prepared && state != Running)
    {
        method_end(&token);
        return -1000;
    }
    *inputLatency  = 0;
    *outputLatency = 0;
    if (This->pipeasio_number_inputs > 0)
    {
        audio_port_get_latency_range(This->input_channel[0].port, AUDIO_CAPTURE_LATENCY, &range);
        *inputLatency = (LONG)range.max;
    }
    if (This->pipeasio_number_outputs > 0)
    {
        audio_port_get_latency_range(This->output_channel[0].port, AUDIO_PLAYBACK_LATENCY, &range);
        *outputLatency = (LONG)range.max;
    }
    method_end(&token);
    return 0;
}

/* Currently reports all sizes the same with granularity 0. Returns -1000 on missing IO. */

DEFINE_THISCALL_WRAPPER(GetBufferSize, 20)
HIDDEN LONG STDMETHODCALLTYPE
GetBufferSize(LPPIPEASIO iface, LONG *minSize, LONG *maxSize, LONG *preferredSize,
              LONG *granularity)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    method_token   token;
    bool           pending;
    BOOL           fixed;
    BOOL           follow;
    LONG           pref;
    if (!minSize || !maxSize || !preferredSize || !granularity)
        return -998;
    if (!method_begin(This, false, &token))
        return -1000;
    INT state = atomic_load_explicit(&This->host_driver_state, memory_order_acquire);
    if (state != Initialized && state != Prepared && state != Running)
    {
        method_end(&token);
        return -1000;
    }
    pending = atomic_load_explicit(&This->config_pending, memory_order_acquire);
    fixed   = This->pipeasio_fixed_buffersize;
    follow  = This->pipeasio_follow_device_clock;
    pref    = This->pipeasio_preferred_buffersize;
    if (pending)
    {
        EnterCriticalSection(&This->config_lock);
        fixed  = This->staged_cfg.fixed_buffer_size ? TRUE : FALSE;
        follow = This->staged_cfg.follow_device_clock ? TRUE : FALSE;
        pref   = This->staged_cfg.buffer_size;
        LeaveCriticalSection(&This->config_lock);
    }
    if (fixed || follow)
    {
        LONG quantum = atomic_load_explicit(&This->follower_quantum, memory_order_acquire);
        *minSize = *maxSize = *preferredSize = (follow && quantum) ? quantum : pref;
        *granularity                         = 0;
    }
    else
    {
        *minSize       = PIPEASIO_MIN_BUFFER_SIZE;
        *maxSize       = PIPEASIO_MAX_BUFFER_SIZE;
        *preferredSize = pref;
        *granularity   = -1;
    }
    method_end(&token);
    return 0;
}

/* A rate the driver can put the graph on: the live one always, plus the standard
 * set when nothing pins us.  config.ini's sample_rate and follow_device_clock
 * are user pins and win over the host, the same way fixed_buffer_size wins over
 * a host-chosen buffer size. */
static bool
rate_is_available(IPipeASIOImpl *This, double sampleRate)
{
    static const uint32_t standard[] = { 44100, 48000, 88200, 96000, 176400, 192000 };

    uint32_t current = atomic_load_explicit(&This->host_sample_rate, memory_order_acquire);
    if (sampleRate == (double)current)
        return true;
    if (This->pipeasio_sample_rate || This->pipeasio_follow_device_clock)
        return false;
    for (size_t i = 0; i < sizeof standard / sizeof standard[0]; i++)
        if (sampleRate == (double)standard[i])
            return true;
    return false;
}

/* Returns -995 if the sample rate isn't available, -1000 on missing IO. */

DEFINE_THISCALL_WRAPPER(CanSampleRate, 12)
HIDDEN LONG STDMETHODCALLTYPE
CanSampleRate(LPPIPEASIO iface, double sampleRate)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    method_token   token;
    LONG           result;
    if (!method_begin(This, false, &token))
        return -1000;
    result = rate_is_available(This, sampleRate) ? 0 : -995;
    method_end(&token);
    return result;
}

/* currentRate holds 0 if unknown.
 * Returns -995 if the sample rate is unknown, -1000 on missing IO. */

DEFINE_THISCALL_WRAPPER(GetSampleRate, 8)
HIDDEN LONG STDMETHODCALLTYPE
GetSampleRate(LPPIPEASIO iface, double *sampleRate)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    method_token   token;
    if (!sampleRate)
        return -998;
    if (!method_begin(This, false, &token))
        return -1000;
    uint32_t current = atomic_load_explicit(&This->host_sample_rate, memory_order_acquire);
    /* The host now knows this value - no pending sampleRateChanged for it. */
    atomic_store_explicit(&This->host_announced_rate, current, memory_order_release);
    *sampleRate = (double)current;
    method_end(&token);
    return current ? 0 : -995;
}

/* SR == 0 enables external sync. Returns -995 on unknown SR, -997 if the
 * current clock is external and SR != 0, -1000 on missing IO. */

DEFINE_THISCALL_WRAPPER(SetSampleRate, 12)
HIDDEN LONG STDMETHODCALLTYPE
SetSampleRate(LPPIPEASIO iface, double sampleRate)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    method_token   token;
    if (!method_begin(This, false, &token))
        return -1000;
    if (!rate_is_available(This, sampleRate))
    {
        method_end(&token);
        return -995;
    }

    uint32_t current   = atomic_load_explicit(&This->host_sample_rate, memory_order_acquire);
    uint32_t requested = (uint32_t)sampleRate;
    if (requested == current)
    {
        atomic_store_explicit(&This->host_announced_rate, current, memory_order_release);
        method_end(&token);
        return 0;
    }

    /* NODE_FORCE_RATE is only read when the filter is built, so the graph moves
     * on the next activation.  Report the requested rate now anyway: the host
     * reads GetSampleRate straight after this and must see what it asked for,
     * and the first process cycle corrects it if the daemon lands elsewhere. */
    atomic_store_explicit(&This->host_requested_rate, requested, memory_order_release);
    atomic_store_explicit(&This->host_sample_rate, requested, memory_order_release);
    atomic_store_explicit(&This->host_announced_rate, requested, memory_order_release);

    INT state = atomic_load_explicit(&This->host_driver_state, memory_order_acquire);
    if (state == Prepared || state == Running)
    {
        pipeasio_host_call_token notify;
        TRACE("SetSampleRate: %u Hz requested, asking the host to reset\n", requested);
        if (pipeasio_host_call_begin(This, PIPEASIO_HOST_CONFIG_RESET, &notify))
        {
            if (pipeasio_host_call_notify(&notify, 1, 3, NULL, NULL))
                pipeasio_host_call_notify(&notify, 3, 0, NULL, NULL);
        }
        pipeasio_host_call_end(&notify);
    }
    else
        audio_set_forced_rate(This->audio_client, (audio_nframes_t)requested);

    method_end(&token);
    return 0;
}

/* numSources: on entry the number of allocated members, on return the number
 * of clock sources (minimum 1, the internal clock). Returns -1000 on missing IO. */

DEFINE_THISCALL_WRAPPER(GetClockSources, 12)
HIDDEN LONG STDMETHODCALLTYPE
GetClockSources(LPPIPEASIO iface, void *clocks, LONG *numSources)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    method_token   token;
    LONG           capacity;
    if (!numSources)
        return -998;
    capacity = *numSources;
    if (capacity < 0 || (capacity > 0 && !clocks))
        return -998;
    if (!method_begin(This, false, &token))
        return -1000;
    *numSources = 1;
    if (capacity > 0)
    {
        ASIOClockSource *clock = clocks;
        memset(clock, 0, sizeof(*clock));
        clock->associatedChannel = -1;
        clock->associatedGroup   = -1;
        clock->isCurrentSource   = 1;
        strcpy(clock->name, "Internal");
    }
    method_end(&token);
    return 0;
}

/* index is one returned by GetClockSources(). Returns -1000 on missing IO;
 * -997 if a clock can't be selected. -995 should not be returned. */

DEFINE_THISCALL_WRAPPER(SetClockSource, 8)
HIDDEN LONG STDMETHODCALLTYPE
SetClockSource(LPPIPEASIO iface, LONG index)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    method_token   token;
    if (!method_begin(This, false, &token))
        return -1000;
    LONG result = index == 0 ? 0 : -1000;
    method_end(&token);
    return result;
}

/* sPos holds the position, reset to 0 on Start(); tStamp holds the system time
 * of sPos. Returns -1000 on missing IO, -996 on missing clock. */

DEFINE_THISCALL_WRAPPER(GetSamplePosition, 12)
HIDDEN LONG STDMETHODCALLTYPE
GetSamplePosition(LPPIPEASIO iface, w_int64_t *sPos, w_int64_t *tStamp)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    method_token   token;
    if (!sPos || !tStamp)
        return -998;
    if (!method_begin(This, false, &token))
        return -1000;
    uint64_t stamp   = atomic_load_explicit(&This->host_time_stamp, memory_order_acquire);
    uint64_t samples = atomic_load_explicit(&This->host_num_samples, memory_order_acquire);
    tStamp->lo       = (ULONG)(stamp & UINT32_MAX);
    tStamp->hi       = (ULONG)(stamp >> 32);
    sPos->lo         = (ULONG)(samples & UINT32_MAX);
    sPos->hi         = (ULONG)(samples >> 32);
    method_end(&token);
    return 0;
}

/* Returns -1000 on missing IO. */

DEFINE_THISCALL_WRAPPER(GetChannelInfo, 8)
HIDDEN LONG STDMETHODCALLTYPE
GetChannelInfo(LPPIPEASIO iface, void *info)
{
    IPipeASIOImpl  *This = (IPipeASIOImpl *)iface;
    method_token    token;
    ASIOChannelInfo request;
    ASIOChannelInfo result = { 0 };
    if (!info)
        return -998;
    memcpy(&request, info, sizeof(request));
    if (!method_begin(This, false, &token))
        return -1000;
    if (request.channel < 0
        || (request.isInput ? request.channel >= This->pipeasio_number_inputs
                            : request.channel >= This->pipeasio_number_outputs))
    {
        method_end(&token);
        return -998;
    }
    IOChannel *channel
            = &(request.isInput ? This->input_channel : This->output_channel)[request.channel];
    result.channel  = request.channel;
    result.isInput  = request.isInput;
    result.isActive = channel->active;
    result.type     = 19;
    lstrcpynA(result.name, channel->port_name, sizeof(result.name));
    memcpy(info, &result, sizeof(result));
    method_end(&token);
    return 0;
}

/* Commit a config staged by the watcher; recompute the forced quantum.
 * MUST run only with the RT data loop stopped (CreateBuffers, between
 * DisposeBuffers and audio_activate): it writes audio_client->follow_device,
 * which audio_on_process reads.  Channel counts and node_name are not applied
 * (re-init only). */
static void
apply_pending_config(IPipeASIOImpl *This)
{
    if (atomic_load_explicit(&This->config_pending, memory_order_acquire))
    {
        EnterCriticalSection(&This->config_lock);
        struct pipeasio_config cfg = This->staged_cfg;
        atomic_store_explicit(&This->config_pending, false, memory_order_release);
        LeaveCriticalSection(&This->config_lock);

        This->pipeasio_connect_to_hardware  = cfg.auto_connect ? TRUE : FALSE;
        This->pipeasio_fixed_buffersize     = cfg.fixed_buffer_size ? TRUE : FALSE;
        This->pipeasio_follow_device_clock  = cfg.follow_device_clock ? TRUE : FALSE;
        This->pipeasio_realtime             = cfg.realtime ? TRUE : FALSE;
        This->pipeasio_preferred_buffersize = cfg.buffer_size; /* loader pow2-validated */
        This->pipeasio_sample_rate          = cfg.sample_rate;
        lstrcpynA(This->pipeasio_output_device, cfg.output_device,
                  sizeof This->pipeasio_output_device);
        lstrcpynA(This->pipeasio_input_device, cfg.input_device,
                  sizeof This->pipeasio_input_device);

        audio_set_forced_rate(This->audio_client, effective_forced_rate(This));
        audio_set_follow_device(This->audio_client, This->pipeasio_follow_device_clock);
        audio_set_realtime(This->audio_client, This->pipeasio_realtime);
        TRACE("config: applied live reload (buffer_size=%d rate=%d follow=%d auto=%d rt=%d)\n",
              (int)This->pipeasio_preferred_buffersize, This->pipeasio_sample_rate,
              (int)This->pipeasio_follow_device_clock, (int)This->pipeasio_connect_to_hardware,
              (int)This->pipeasio_realtime);
    }
    /* Forced quantum: follow-device uses the observed graph quantum, else the
     * configured preferred size.  Mirrors Init().  Runs every call so a
     * follow-device-only reset (config_pending false) still settles.  Host-
     * controlled mode leaves host_current_buffersize to the host. */
    if (This->pipeasio_fixed_buffersize || This->pipeasio_follow_device_clock)
    {
        LONG q = atomic_load_explicit(&This->follower_quantum, memory_order_relaxed);
        This->host_current_buffersize = (This->pipeasio_follow_device_clock && q)
                                                ? q
                                                : This->pipeasio_preferred_buffersize;
    }
}

/* bufferSize must be one returned by GetBufferSize(). Returns -994 if memory
 * can't be allocated, -997 on unsupported bufferSize or invalid bufferInfo,
 * -1000 on missing IO. */

DEFINE_THISCALL_WRAPPER(CreateBuffers, 20)
HIDDEN LONG STDMETHODCALLTYPE
CreateBuffers(LPPIPEASIO iface, BufferInformation *bufferInfo, LONG numChannels, LONG bufferSize,
              Callbacks *callbacks)
{
    IPipeASIOImpl      *This = (IPipeASIOImpl *)iface;
    method_token        token;
    pipeasio_gate_owner owner;
    BufferInformation  *tables                               = NULL;
    BufferInformation  *original                             = NULL;
    BufferInformation  *result                               = NULL;
    audio_sample_t     *audio_buffer                         = NULL;
    const char        **input_endpoints                      = NULL;
    const char        **output_endpoints                     = NULL;
    bool                input_active[PIPEASIO_MAX_CHANNELS]  = { false };
    bool                output_active[PIPEASIO_MAX_CHANNELS] = { false };
    bool                backend_active                       = false;
    bool                caller_written                       = false;
    LONG                active_inputs                        = 0;
    LONG                active_outputs                       = 0;
    LONG                old_buffer_size;
    LONG                error    = -997;
    INT                 expected = Initialized;
    size_t              table_bytes;
    size_t              audio_bytes;
    size_t              input_endpoint_count  = 0;
    size_t              output_endpoint_count = 0;

    if (!method_begin(This, false, &token))
        return -1000;
    if (!atomic_compare_exchange_strong_explicit(&This->host_driver_state, &expected, Preparing,
                                                 memory_order_acq_rel, memory_order_acquire))
    {
        method_end(&token);
        return -1000;
    }
    clear_last_error(This);
    owner = next_gate_owner(This);
    if (!pipeasio_gate_close(&This->method_gate, owner)
        || !drain_gate(This, &This->method_gate, This->method_idle, 1)
        || !pipeasio_gate_reopen(&This->method_gate, owner))
    {
        set_last_error(This, "could not acquire the driver method gate");
        error = -1000;
        goto fail;
    }

    if (!bufferInfo || !callbacks || numChannels < 0
        || numChannels > This->pipeasio_number_inputs + This->pipeasio_number_outputs)
    {
        set_last_error(This, "invalid CreateBuffers arguments");
        goto fail;
    }
    table_bytes = (size_t)numChannels * sizeof(*bufferInfo);
    if (numChannels)
    {
        tables = HeapAlloc(GetProcessHeap(), 0, table_bytes * 2);
        if (!tables)
        {
            set_last_error(This, "out of memory validating channel descriptors");
            error = -994;
            goto fail;
        }
        original = tables;
        result   = tables + numChannels;
        memcpy(original, bufferInfo, table_bytes);
        memcpy(result, original, table_bytes);
    }

    for (LONG i = 0; i < numChannels; ++i)
    {
        LONG channel = original[i].channelNumber;
        if (original[i].isInputType)
        {
            if (channel < 0 || channel >= This->pipeasio_number_inputs || input_active[channel])
            {
                set_last_error(This, "invalid or duplicate input channel %ld", (long)channel);
                goto fail;
            }
            input_active[channel] = true;
            ++active_inputs;
        }
        else
        {
            if (channel < 0 || channel >= This->pipeasio_number_outputs || output_active[channel])
            {
                set_last_error(This, "invalid or duplicate output channel %ld", (long)channel);
                goto fail;
            }
            output_active[channel] = true;
            ++active_outputs;
        }
    }

    apply_pending_config(This);
    old_buffer_size = This->host_current_buffersize;
    if (This->pipeasio_fixed_buffersize || This->pipeasio_follow_device_clock)
    {
        if (bufferSize != This->host_current_buffersize
            || !pipeasio_buffer_size_supported(bufferSize))
        {
            set_last_error(This, "unsupported fixed/follower buffer size %ld", (long)bufferSize);
            goto fail;
        }
    }
    else
    {
        if (!pipeasio_buffer_size_supported(bufferSize) || (bufferSize & (bufferSize - 1)) != 0)
        {
            set_last_error(This, "unsupported buffer size %ld", (long)bufferSize);
            goto fail;
        }
        This->host_current_buffersize = bufferSize;
    }
    if (!audio_set_buffer_size(This->audio_client, (audio_nframes_t)bufferSize))
    {
        set_last_error(This, "the PipeWire graph rejected buffer size %ld", (long)bufferSize);
        error = -999;
        goto fail_restore_size;
    }

    audio_bytes = pipeasio_host_callback_size_bytes(This->pipeasio_number_inputs,
                                                    This->pipeasio_number_outputs, bufferSize,
                                                    sizeof(*audio_buffer));
    if (!audio_bytes
        || !(audio_buffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, audio_bytes)))
    {
        set_last_error(This, "out of memory allocating %ld-sample buffers", (long)bufferSize);
        error = -994;
        goto fail_restore_size;
    }

    for (int i = 0; i < This->pipeasio_number_inputs; ++i)
    {
        This->input_channel[i].audio_buffer
                = audio_buffer + pipeasio_host_input_offset_samples(i, bufferSize);
        This->input_channel[i].active = input_active[i];
    }
    for (int i = 0; i < This->pipeasio_number_outputs; ++i)
    {
        This->output_channel[i].audio_buffer = audio_buffer
                                               + pipeasio_host_output_offset_samples(
                                                       i, This->pipeasio_number_inputs, bufferSize);
        This->output_channel[i].active       = output_active[i];
    }

    for (LONG i = 0; i < numChannels; ++i)
    {
        LONG            channel = original[i].channelNumber;
        audio_sample_t *base = original[i].isInputType ? This->input_channel[channel].audio_buffer
                                                       : This->output_channel[channel].audio_buffer;
        result[i].audioBufferStart = base;
        result[i].audioBufferEnd   = base + bufferSize;
    }

#ifdef PIPEASIO_WOW64_PE
    if (!pipeasio_wow64_bind_rt(This->audio_client, audio_buffer, bufferSize,
                                This->pipeasio_number_inputs, This->pipeasio_number_outputs,
                                input_active, output_active))
    {
        set_last_error(This, "failed to bind the WoW64 callback buffer");
        error = -1000;
        goto fail_internal;
    }
#endif
    if (!audio_activate(This->audio_client))
    {
        set_last_error(This, "could not activate the PipeWire stream (device busy or "
                             "disconnected?)");
        error = -1000;
        goto fail_internal;
    }
    backend_active = true;

    if (This->pipeasio_connect_to_hardware)
    {
        char local_name[PIPEASIO_MAX_NAME_LENGTH];
        input_endpoints = audio_get_device_ports(
                This->audio_client,
                This->pipeasio_input_device[0] ? This->pipeasio_input_device : NULL,
                AUDIO_PORT_IS_OUTPUT);
        output_endpoints = audio_get_device_ports(
                This->audio_client,
                This->pipeasio_output_device[0] ? This->pipeasio_output_device : NULL,
                AUDIO_PORT_IS_INPUT);
        if (!input_endpoints || !output_endpoints)
        {
            set_last_error(This, "could not enumerate audio device ports; check the Wine log");
            error = -1000;
            goto fail_internal;
        }
        while (input_endpoints[input_endpoint_count])
            ++input_endpoint_count;
        while (output_endpoints[output_endpoint_count])
            ++output_endpoint_count;
        for (int i = 0; i < This->pipeasio_number_inputs; ++i)
        {
            if (!input_active[i] || (size_t)i >= input_endpoint_count)
                continue;
            if (!audio_port_get_name(This->input_channel[i].port, local_name, sizeof local_name)
                || !audio_connect(This->audio_client, input_endpoints[i], local_name))
            {
                set_last_error(This, "failed to connect input channel %d to the audio device",
                               i + 1);
                error = -1000;
                goto fail_internal;
            }
        }
        for (int i = 0; i < This->pipeasio_number_outputs; ++i)
        {
            if (!output_active[i] || (size_t)i >= output_endpoint_count)
                continue;
            if (!audio_port_get_name(This->output_channel[i].port, local_name, sizeof local_name)
                || !audio_connect(This->audio_client, local_name, output_endpoints[i]))
            {
                set_last_error(This, "failed to connect output channel %d to the audio device",
                               i + 1);
                error = -1000;
                goto fail_internal;
            }
        }
    }
    audio_free_ports(input_endpoints);
    audio_free_ports(output_endpoints);
    input_endpoints  = NULL;
    output_endpoints = NULL;

    {
        /* A previous best-effort stop may have left a watcher attached.  Join
         * it for real before replacing it: if it were blocked in a host reset
         * notification it holds a host-gate admission, so this wait has the
         * same bound as the gate drains. */
        stop_config_watch(This, true);
        struct config_watch *watch = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*watch));
        if (watch)
        {
            watch->owner      = This;
            watch->refs       = 2; /* stop_config_watch + watcher thread */
            watch->stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
            if (watch->stop_event)
                watch->thread = CreateThread(NULL, 0, config_watch_proc, watch, 0, &watch->tid);
            if (watch->thread)
                This->config_watch = watch;
            else
            {
                if (watch->stop_event)
                    CloseHandle(watch->stop_event);
                HeapFree(GetProcessHeap(), 0, watch);
            }
        }
    }

    This->callback_audio_buffer = audio_buffer;
    This->host_active_inputs    = active_inputs;
    This->host_active_outputs   = active_outputs;
    This->host_time_info_mode   = FALSE;
    atomic_store_explicit(&This->host_callbacks, callbacks, memory_order_release);

    if (!claim_closed_gate(&This->host_gate, owner)
        || !pipeasio_gate_reopen(&This->host_gate, owner))
    {
        set_last_error(This, "could not publish callbacks through the host gate");
        error = -1000;
        goto fail_internal_published;
    }
    {
        pipeasio_host_call_token host_token;
        if (pipeasio_host_call_begin(This, PIPEASIO_HOST_TIME_INFO, &host_token))
            This->host_time_info_mode
                    = pipeasio_host_call_notify(&host_token, 7, 0, NULL, NULL) != 0;
        pipeasio_host_call_end(&host_token);
    }
    if (!pipeasio_gate_close(&This->host_gate, owner)
        || !drain_gate(This, &This->host_gate, This->host_idle, 0)
        || atomic_load_explicit(&This->host_driver_state, memory_order_acquire) != Preparing)
    {
        set_last_error(This, "buffer preparation was interrupted");
        error = -1000;
        goto fail_internal_published;
    }

    if (numChannels)
    {
        memcpy(bufferInfo, result, table_bytes);
        caller_written = true;
    }
    expected = Preparing;
    if (!atomic_compare_exchange_strong_explicit(&This->host_driver_state, &expected, Prepared,
                                                 memory_order_release, memory_order_acquire))
    {
        if (caller_written)
            memcpy(bufferInfo, original, table_bytes);
        set_last_error(This, "driver state changed during buffer preparation");
        error = -1000;
        goto fail_internal_published;
    }
    HeapFree(GetProcessHeap(), 0, tables);
    method_end(&token);
    return 0;

fail_internal_published:
    atomic_store_explicit(&This->host_callbacks, NULL, memory_order_release);
    This->callback_audio_buffer = NULL;
    This->host_active_inputs    = 0;
    This->host_active_outputs   = 0;
    stop_config_watch(This, false);
fail_internal:
    audio_free_ports(input_endpoints);
    audio_free_ports(output_endpoints);
    if (backend_active)
        audio_deactivate(This->audio_client);
    for (int i = 0; i < This->pipeasio_number_inputs; ++i)
    {
        This->input_channel[i].audio_buffer = NULL;
        This->input_channel[i].active       = false;
    }
    for (int i = 0; i < This->pipeasio_number_outputs; ++i)
    {
        This->output_channel[i].audio_buffer = NULL;
        This->output_channel[i].active       = false;
    }
    HeapFree(GetProcessHeap(), 0, audio_buffer);
fail_restore_size:
    This->host_current_buffersize = old_buffer_size;
fail:
    if (caller_written)
        memcpy(bufferInfo, original, table_bytes);
    HeapFree(GetProcessHeap(), 0, tables);
    expected = Preparing;
    atomic_compare_exchange_strong_explicit(&This->host_driver_state, &expected, Initialized,
                                            memory_order_release, memory_order_acquire);
    if (!pipeasio_gate_is_permanent(&This->method_gate)
        && pipeasio_gate_is_closed(&This->method_gate))
        pipeasio_gate_reopen(&This->method_gate, owner);
    method_end(&token);
    return error;
}

/* Implies Stop(). Returns -997 if no buffers were previously allocated, -1000 on missing IO. */

DEFINE_THISCALL_WRAPPER(DisposeBuffers, 4)
HIDDEN LONG STDMETHODCALLTYPE
DisposeBuffers(LPPIPEASIO iface)
{
    IPipeASIOImpl      *This = (IPipeASIOImpl *)iface;
    method_token        token;
    pipeasio_gate_owner owner;
    INT                 state;
    INT                 expected;
    bool                backend_ok  = true;
    bool                method_held = false;

    if (!method_begin(This, false, &token))
        return -1000;
    if (pipeasio_host_call_is_reentrant(This))
    {
        method_end(&token);
        return -1000;
    }

    state = atomic_load_explicit(&This->host_driver_state, memory_order_acquire);
    if (state == Running || state == Stopping)
    {
        if (stop_admitted(This) != 0)
        {
            method_end(&token);
            return -1000;
        }
        state = atomic_load_explicit(&This->host_driver_state, memory_order_acquire);
    }
    expected = Prepared;
    if (state != Prepared
        || !atomic_compare_exchange_strong_explicit(&This->host_driver_state, &expected, Disposing,
                                                    memory_order_acq_rel, memory_order_acquire))
    {
        method_end(&token);
        return -1000;
    }

    owner = next_gate_owner(This);
    if (!pipeasio_gate_close(&This->method_gate, owner))
        goto fail_held;
    method_held = true;
    if (!drain_gate(This, &This->method_gate, This->method_idle, 1))
        goto fail_held;
    if (!pipeasio_gate_reopen(&This->method_gate, owner))
        goto fail_held;
    method_held = false;
    if (!claim_closed_gate(&This->host_gate, owner))
        goto fail_held;
    if (!drain_gate(This, &This->host_gate, This->host_idle, 0))
        goto fail_held;

    stop_config_watch(This, false);
    if (This->audio_client)
        backend_ok = audio_deactivate(This->audio_client);
    atomic_store_explicit(&This->host_callbacks, NULL, memory_order_release);

    for (int i = 0; i < This->pipeasio_number_inputs; ++i)
    {
        This->input_channel[i].audio_buffer = NULL;
        This->input_channel[i].active       = false;
    }
    for (int i = 0; i < This->pipeasio_number_outputs; ++i)
    {
        This->output_channel[i].audio_buffer = NULL;
        This->output_channel[i].active       = false;
    }
    This->host_active_inputs  = 0;
    This->host_active_outputs = 0;
    if (This->callback_audio_buffer)
    {
        HeapFree(GetProcessHeap(), 0, This->callback_audio_buffer);
        This->callback_audio_buffer = NULL;
    }

    expected = Disposing;
    atomic_compare_exchange_strong_explicit(&This->host_driver_state, &expected, Initialized,
                                            memory_order_release, memory_order_acquire);
    method_end(&token);
    return backend_ok ? 0 : -1000;

fail_held:
    /* No teardown ran: restore the state we entered with so a retry sees a
     * coherent object.  host_gate stays closed (possibly under this owner):
     * that is its normal Prepared state, and the next close operation claims
     * it via claim_closed_gate. */
    if (method_held && !pipeasio_gate_is_permanent(&This->method_gate))
        pipeasio_gate_reopen(&This->method_gate, owner);
    expected = Disposing;
    atomic_compare_exchange_strong_explicit(&This->host_driver_state, &expected, Prepared,
                                            memory_order_release, memory_order_acquire);
    method_end(&token);
    return -1000;
}

/* Returns -1000 if no control panel exists, but the return code should be
 * ignored. Call sendNotification if something changed. */

DEFINE_THISCALL_WRAPPER(ControlPanel, 4)
HIDDEN LONG STDMETHODCALLTYPE
ControlPanel(LPPIPEASIO iface)
{
    char           cfg_path[1024];
    char           message[1536];
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    method_token   token;
    if (!method_begin(This, false, &token))
        return -1000;

    TRACE("iface: %p\n", iface);

    /* The settings panel (pipeasio-settings) is a native Linux/Qt app and
     * cannot run inside the Wine/Proton container the host loads us into:
     * the container has no Qt libraries and no clean route back to the host.
     * Rather than fork/exec it and silently fail, tell the user to launch it
     * from a host terminal, and point at the exact INI it edits. */
    if (!pipeasio_config_path(cfg_path, sizeof cfg_path))
        lstrcpynA(cfg_path, "$XDG_CONFIG_HOME/pipeasio/config.ini", sizeof cfg_path);

    snprintf(message, sizeof message,
             "Please launch the PipeASIO settings panel from a terminal on your "
             "Linux host:\n\n"
             "    pipeasio-settings\n\n"
             "It is a native Linux app, so it cannot run inside this Wine/Proton "
             "container.\n\n"
             "Settings are saved to:\n    %s\n\n"
             "Changes are applied automatically about a second after you save, "
             "while PipeASIO is active. If they don't appear, reselect PipeASIO "
             "or restart this application.",
             cfg_path);

    MessageBoxA(NULL, message, "PipeASIO Settings", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    method_end(&token);
    return 0;
}

/* Returns -998 on invalid or unsupported selector, 0x3f4847a0 on success (never 0). */

DEFINE_THISCALL_WRAPPER(Future, 12)
HIDDEN LONG STDMETHODCALLTYPE
Future(LPPIPEASIO iface, LONG selector, void *opt)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    method_token   token;
    LONG           result;
    (void)opt;
    if (!method_begin(This, false, &token))
        return -1000;
    switch (selector)
    {
    case 10:
        result = 0x3f4847a0;
        break;
    case 3:
    case 0x23111961:
    case 0x23111983:
    case 0x23112004:
        result = -1000;
        break;
    default:
        result = -998;
        break;
    }
    method_end(&token);
    return result;
}

/* Returns 0 if supported, -1000 to disable. */

DEFINE_THISCALL_WRAPPER(OutputReady, 4)
HIDDEN LONG STDMETHODCALLTYPE
OutputReady(LPPIPEASIO iface)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    method_token   token;
    if (!method_begin(This, false, &token))
        return -1000;
    method_end(&token);
    return -1000;
}

/****************************************************************************
 *  ASIO process callbacks
 */

static inline int
process_callback(audio_nframes_t nframes, void *arg)
{
    IPipeASIOImpl           *This = (IPipeASIOImpl *)arg;
    pipeasio_host_call_token token;
    bool admitted = pipeasio_host_call_begin(This, PIPEASIO_HOST_PROCESS, &token);
    int  half     = 0;

    if (admitted)
    {
        /* Deliver a sampleRateChanged the SAMPLE_RATE path could not: the
         * backend announces the measured graph rate on the first process
         * cycle after activation (CreateBuffers), when the driver is not yet
         * Running, so only the host_sample_rate store survived (issue #20).
         * Tell the host before it processes the first buffer at that rate. */
        uint32_t rate = atomic_load_explicit(&This->host_sample_rate, memory_order_acquire);
        if (rate != atomic_load_explicit(&This->host_announced_rate, memory_order_acquire))
        {
            atomic_store_explicit(&This->host_announced_rate, rate, memory_order_release);
            pipeasio_host_call_sample_rate(&token, rate);
        }

        /* Read only while admitted: Start writes host_buffer_index after
         * draining the host gate, which rejected callbacks are not part of. */
        half = This->host_buffer_index;
        for (int i = 0; i < This->pipeasio_number_inputs; ++i)
        {
            if (!This->input_channel[i].active)
                continue;
            audio_sample_t *source = audio_port_get_buffer(This->input_channel[i].port, nframes);
            audio_sample_t *destination = &This->input_channel[i].audio_buffer[nframes * half];
            if (source)
                memcpy(destination, source, sizeof(*destination) * nframes);
            else
                memset(destination, 0, sizeof(*destination) * nframes);
        }

        pipeasio_host_call_process(&token, half, nframes, audio_get_time_nsec(This->audio_client));
    }

    for (int i = 0; i < This->pipeasio_number_outputs; ++i)
    {
        const audio_sample_t *source
                = admitted ? &This->output_channel[i].audio_buffer[nframes * half] : NULL;
        audio_port_publish_output(This->output_channel[i].port, source, nframes, admitted,
                                  This->output_channel[i].active);
    }
    if (admitted)
        This->host_buffer_index = half ? 0 : 1;
    pipeasio_host_call_end(&token);
    /* Nonzero tells the backend the host did not consume this cycle, so a
     * deliberate Stop's idle cycles are not billed to us as xruns. */
    return admitted ? 0 : 1;
}

static inline int
sample_rate_callback(audio_nframes_t nframes, void *arg)
{
    IPipeASIOImpl           *This = (IPipeASIOImpl *)arg;
    pipeasio_host_call_token token;
    /* The store must survive pre-publication activation: the filter runs
     * from CreateBuffers, before callbacks are published or the state is
     * Running, and the backend will not repeat a stable rate.  GetSampleRate
     * must report the measured graph rate regardless of delivery. */
    atomic_store_explicit(&This->host_sample_rate, nframes, memory_order_release);
    if (pipeasio_host_call_begin(This, PIPEASIO_HOST_SAMPLE_RATE, &token))
    {
        atomic_store_explicit(&This->host_announced_rate, nframes, memory_order_release);
        pipeasio_host_call_sample_rate(&token, nframes);
    }
    pipeasio_host_call_end(&token);
    return 0;
}

/*****************************************************************************
 *  Support functions
 */

#ifndef WINE_WITH_UNICODE
/* Funtion required as unicode.h no longer in WINE */
static WCHAR *
strrchrW(const WCHAR *str, WCHAR ch)
{
    WCHAR *ret = NULL;
    do
    {
        if (*str == ch)
            ret = (WCHAR *)(ULONG_PTR)str;
    } while (*str++);
    return ret;
}
#endif

static bool
read_environment(const char *name, char *value, DWORD capacity)
{
    DWORD length;
    if (!value || capacity < 2)
        return false;
    value[0] = '\0';
    length   = GetEnvironmentVariableA(name, value, capacity);
    if (!length || length >= capacity)
    {
        value[0] = '\0';
        return false;
    }
    return true;
}

static VOID
configure_driver(IPipeASIOImpl *This)
{
    WCHAR                  application_path[MAX_PATH];
    WCHAR                 *application_name;
    char                   environment_variable[MAX_ENVIRONMENT_SIZE];
    char                   name_environment[PIPEASIO_MAX_NAME_LENGTH];
    char                   device_environment[PIPEASIO_DEVICE_NAME_MAX];
    int                    parsed;
    bool                   flag;
    struct pipeasio_config cfg;
    pipeasio_config_defaults(&cfg);
#ifdef PIPEASIO_WOW64_PE
    bool cfg_found = pipeasio_wow64_load_config(&cfg);
#else
    bool cfg_found = pipeasio_config_load(&cfg);
#endif
    TRACE("config: %s inputs=%d outputs=%d buffer=%d rate=%d\n", cfg_found ? "loaded" : "defaults",
          cfg.inputs, cfg.outputs, cfg.buffer_size, cfg.sample_rate);
    This->pipeasio_number_inputs        = cfg.inputs;
    This->pipeasio_number_outputs       = cfg.outputs;
    This->pipeasio_connect_to_hardware  = cfg.auto_connect ? TRUE : FALSE;
    This->pipeasio_fixed_buffersize     = cfg.fixed_buffer_size ? TRUE : FALSE;
    This->pipeasio_follow_device_clock  = cfg.follow_device_clock ? TRUE : FALSE;
    This->pipeasio_realtime             = cfg.realtime ? TRUE : FALSE;
    This->pipeasio_preferred_buffersize = cfg.buffer_size;
    This->pipeasio_sample_rate          = cfg.sample_rate;
    lstrcpynA(This->pipeasio_output_device, cfg.output_device,
              sizeof(This->pipeasio_output_device));
    lstrcpynA(This->pipeasio_input_device, cfg.input_device, sizeof(This->pipeasio_input_device));
    lstrcpynA(This->client_name, "PipeASIO", sizeof(This->client_name));
    if (cfg.node_name[0])
        lstrcpynA(This->client_name, cfg.node_name, sizeof(This->client_name));
    else
    {
        DWORD length = GetModuleFileNameW(NULL, application_path, MAX_PATH);
        if (length && length < MAX_PATH)
        {
            application_name = strrchrW(application_path, L'.');
            if (application_name)
                *application_name = 0;
            application_name = strrchrW(application_path, L'\\');
            application_name = application_name ? application_name + 1 : application_path;
            if (!WideCharToMultiByte(CP_ACP, WC_SEPCHARS, application_name, -1, This->client_name,
                                     sizeof(This->client_name), NULL, NULL))
                lstrcpynA(This->client_name, "PipeASIO", sizeof(This->client_name));
        }
    }
    if (read_environment("PIPEASIO_NUMBER_INPUTS", environment_variable,
                         sizeof(environment_variable))
        && pipeasio_parse_int(environment_variable, 0, PIPEASIO_MAX_CHANNELS, &parsed))
        This->pipeasio_number_inputs = parsed;
    if (read_environment("PIPEASIO_NUMBER_OUTPUTS", environment_variable,
                         sizeof(environment_variable))
        && pipeasio_parse_int(environment_variable, 0, PIPEASIO_MAX_CHANNELS, &parsed))
        This->pipeasio_number_outputs = parsed;
    if (read_environment("PIPEASIO_CONNECT_TO_HARDWARE", environment_variable,
                         sizeof(environment_variable))
        && pipeasio_parse_bool(environment_variable, &flag))
        This->pipeasio_connect_to_hardware = flag ? TRUE : FALSE;
    if (read_environment("PIPEASIO_FIXED_BUFFERSIZE", environment_variable,
                         sizeof(environment_variable))
        && pipeasio_parse_bool(environment_variable, &flag))
        This->pipeasio_fixed_buffersize = flag ? TRUE : FALSE;
    if (read_environment("PIPEASIO_FOLLOW_DEVICE_CLOCK", environment_variable,
                         sizeof(environment_variable))
        && pipeasio_parse_bool(environment_variable, &flag))
        This->pipeasio_follow_device_clock = flag ? TRUE : FALSE;
    if (read_environment("PIPEASIO_PREFERRED_BUFFERSIZE", environment_variable,
                         sizeof(environment_variable))
        && pipeasio_parse_int(environment_variable, PIPEASIO_MIN_BUFFER_SIZE,
                              PIPEASIO_MAX_BUFFER_SIZE, &parsed)
        && !(parsed & (parsed - 1)))
        This->pipeasio_preferred_buffersize = parsed;
    if (read_environment("PIPEASIO_SAMPLE_RATE", environment_variable, sizeof(environment_variable))
        && pipeasio_parse_int(environment_variable, 0, INT_MAX, &parsed))
        This->pipeasio_sample_rate = parsed;
    if (read_environment("PIPEASIO_OUTPUT_DEVICE", device_environment, sizeof(device_environment)))
        lstrcpynA(This->pipeasio_output_device, device_environment,
                  sizeof(This->pipeasio_output_device));
    if (read_environment("PIPEASIO_INPUT_DEVICE", device_environment, sizeof(device_environment)))
        lstrcpynA(This->pipeasio_input_device, device_environment,
                  sizeof(This->pipeasio_input_device));
    if (read_environment("PIPEASIO_CLIENT_NAME", name_environment, sizeof(name_environment)))
        lstrcpynA(This->client_name, name_environment, sizeof(This->client_name));

    return;
}

/* Allocate the interface pointer and associate it with the vtbl/PipeASIO object */
HRESULT WINAPI
PipeASIOCreateInstance(REFIID riid, LPVOID *ppobj)
{
    IPipeASIOImpl *pobj;
    HMODULE        module = NULL;

    if (!ppobj)
        return E_POINTER;
    *ppobj = NULL;
    if (!IsEqualIID(riid, &IID_IUnknown) && !IsEqualIID(riid, &CLSID_PipeASIO))
        return E_NOINTERFACE;

    pobj = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*pobj));
    if (!pobj)
        return E_OUTOFMEMORY;

    pobj->lpVtbl = &PipeASIO_Vtbl;
    atomic_init(&pobj->ref, 1);
    atomic_init(&pobj->host_driver_state, Loaded);
    atomic_init(&pobj->host_callbacks, NULL);
    atomic_init(&pobj->host_num_samples, 0);
    atomic_init(&pobj->host_sample_rate, 0);
    atomic_init(&pobj->host_announced_rate, 0);
    atomic_init(&pobj->host_time_stamp, 0);
    atomic_init(&pobj->config_pending, false);
    atomic_init(&pobj->follower_quantum, 0);
    atomic_init(&pobj->lifecycle_waiters, 0);
    atomic_init(&pobj->stop_generation, 0);
    atomic_init(&pobj->gate_owner_seq, 1);
    InitializeSRWLock(&pobj->lifecycle_lock);
    pobj->host_version = 92;
    pipeasio_gate_init(&pobj->method_gate, true);
    pipeasio_gate_init(&pobj->host_gate, false);
    atomic_store_explicit(&pobj->host_gate.word,
                          PIPEASIO_GATE_CLOSED_BIT | pipeasio_gate_owner_bits(1),
                          memory_order_relaxed);

    if (!InitializeCriticalSectionAndSpinCount(&pobj->config_lock, 0))
        goto fail_alloc;
    pobj->method_idle = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!pobj->method_idle)
        goto fail_cs;
    pobj->host_idle = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!pobj->host_idle)
        goto fail_method_idle;
    pobj->work_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!pobj->work_event)
        goto fail_host_idle;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            (LPCSTR)(uintptr_t)&PipeASIOCreateInstance, &module))
        goto fail_work_event;
    pobj->module_pin = module;
    pipeasio_object_created();
    pobj->worker = CreateThread(NULL, 0, lifecycle_worker, pobj, 0, &pobj->worker_tid);
    if (!pobj->worker)
        goto fail_object;

    *ppobj = pobj;
    return S_OK;

fail_object:
    pipeasio_object_destroyed();
    FreeLibrary(module);
fail_work_event:
    CloseHandle(pobj->work_event);
fail_host_idle:
    CloseHandle(pobj->host_idle);
fail_method_idle:
    CloseHandle(pobj->method_idle);
fail_cs:
    DeleteCriticalSection(&pobj->config_lock);
fail_alloc:
    HeapFree(GetProcessHeap(), 0, pobj);
    return E_OUTOFMEMORY;
}
