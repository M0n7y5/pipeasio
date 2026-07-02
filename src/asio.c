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
#include <errno.h>
#include <limits.h>
#ifndef PIPEASIO_WOW64_PE
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#endif
#include <stdatomic.h>

#include <stdlib.h> /* getenv for PIPEASIO_DEBUG */

/* wine/debug.h provides debugstr_guid; pipeasio_log.h overrides TRACE/WARN/ERR. */
#ifndef PIPEASIO_WOW64_PE
#include "wine/debug.h"
#endif
#include "pipeasio_log.h"

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
#include "pipeasio_rt.h"
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
#define PIPEASIO_MINIMUM_BUFFERSIZE 16
#define PIPEASIO_MAXIMUM_BUFFERSIZE 8192
#define PIPEASIO_PREFERRED_BUFFERSIZE 1024

/* i386 ASIO uses MS thiscall; GCC needs a trampoline. */
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
#define __thiscall __stdcall
#define DEFINE_THISCALL_WRAPPER(func, args) /* nothing */

#endif /* __i386__ */

/* Hide ELF symbols for COM members. */
#define HIDDEN __attribute__((visibility("hidden")))

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

typedef struct IOChannel
{
    audio_sample_t *audio_buffer;
    char            port_name[PIPEASIO_MAX_NAME_LENGTH];
    audio_port_t   *port;
    bool            active;
} IOChannel;

typedef struct IPipeASIOImpl
{
    /* COM stuff */
    const IPipeASIOVtbl *lpVtbl;
    LONG                 ref;

    /* The app's main window handle on windows, 0 on OS/X */
    HWND sys_ref;

    /* Host stuff */
    LONG host_active_inputs;
    LONG host_active_outputs;
    BOOL host_buffer_index;
    Callbacks *_Atomic host_callbacks;
    /* Live-config watcher: polls config.ini, asks the host to reset on change */
    HANDLE                 config_watch_thread;
    HANDLE                 config_watch_stop;
    DWORD                  config_watch_tid;
    CRITICAL_SECTION       config_lock;      /* guards staged_cfg */
    struct pipeasio_config staged_cfg;       /* watcher -> apply handoff */
    _Atomic bool           config_pending;   /* staged_cfg has a fresh reload */
    _Atomic LONG           follower_quantum; /* last observed device quantum */
    LONG                   host_current_buffersize;
    _Atomic INT            host_driver_state;
    _Atomic uint64_t       host_num_samples;
    double                 host_sample_rate;
    TimeInformation        host_time;
    BOOL                   host_time_info_mode;
    _Atomic uint64_t       host_time_stamp;
    LONG                   host_version;

    /* PipeASIO configuration options */
    int  pipeasio_number_inputs;
    int  pipeasio_number_outputs;
    BOOL pipeasio_connect_to_hardware;
    BOOL pipeasio_fixed_buffersize;
    BOOL pipeasio_follow_device_clock;
    LONG pipeasio_preferred_buffersize;
    int  pipeasio_sample_rate; /* 0 = follow graph */
    char pipeasio_output_device[PIPEASIO_DEVICE_NAME_MAX];
    char pipeasio_input_device[PIPEASIO_DEVICE_NAME_MAX];

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

enum
{
    Loaded,
    Initialized,
    Prepared,
    Running
};

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

static inline int  buffer_size_callback(audio_nframes_t nframes, void *arg);
static inline void latency_callback(audio_latency_mode_t mode, void *arg);
static inline int  process_callback(audio_nframes_t nframes, void *arg);
static inline int  sample_rate_callback(audio_nframes_t nframes, void *arg);

/*
 *  Support functions
 */

HRESULT WINAPI PipeASIOCreateInstance(REFIID riid, LPVOID *ppobj);
static VOID    configure_driver(IPipeASIOImpl *This);

/* {2d3ca9e2-1193-4c5d-b5fd-38798f3dc074} */
static GUID const CLSID_PipeASIO
        = { 0x2d3ca9e2, 0x1193, 0x4c5d, { 0xb5, 0xfd, 0x38, 0x79, 0x8f, 0x3d, 0xc0, 0x74 } };

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

/*****************************************************************************
 * Interface method definitions
 */

HIDDEN HRESULT STDMETHODCALLTYPE
QueryInterface(LPPIPEASIO iface, REFIID riid, void **ppvObject)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;

    TRACE("iface: %p, riid: %s, ppvObject: %p)\n", iface, wine_dbgstr_guid(riid), ppvObject);

    if (ppvObject == NULL)
        return E_INVALIDARG;

    if (IsEqualIID(&CLSID_PipeASIO, riid))
    {
        AddRef(iface);
        *ppvObject = This;
        return S_OK;
    }

    return E_NOINTERFACE;
}

HIDDEN ULONG STDMETHODCALLTYPE
AddRef(LPPIPEASIO iface)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    ULONG          ref  = InterlockedIncrement(&(This->ref));

    TRACE("iface: %p, ref count is %u\n", iface, (unsigned)ref);
    return ref;
}

/* Poll config.ini and request host reset when it changes. */
static DWORD WINAPI
config_watch_proc(LPVOID arg)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)arg;
    char           path[1024];
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
        DWORD waited = WaitForSingleObject(This->config_watch_stop, 1000);
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

        if (reset && This->host_driver_state == Running && This->host_callbacks)
        {
            TRACE("config watcher: requesting host reset\n");
            if (This->host_callbacks->sendNotification(1, 3, 0, 0))
                This->host_callbacks->sendNotification(3, 0, 0, 0);
        }
    }
    return 0;
}

/* Signal, join, and dispose the config watcher.  Idempotent: safe to call when
 * the watcher was never started or already stopped. */
static void
stop_config_watch(IPipeASIOImpl *This)
{
    if (This->config_watch_thread && GetCurrentThreadId() == This->config_watch_tid)
    {
        /* Reentrant teardown: a host serviced kAsioResetRequest synchronously on
         * the watcher thread.  Joining ourselves would deadlock.  Signal stop and
         * return; the watcher exits at its next loop check, and the COM-thread
         * DisposeBuffers()/Release() path reaps the handles. */
        if (This->config_watch_stop)
            SetEvent(This->config_watch_stop);
        return;
    }
    if (This->config_watch_thread)
    {
        if (This->config_watch_stop)
            SetEvent(This->config_watch_stop);
        WaitForSingleObject(This->config_watch_thread, INFINITE);
        CloseHandle(This->config_watch_thread);
        This->config_watch_thread = NULL;
    }
    if (This->config_watch_stop)
    {
        CloseHandle(This->config_watch_stop);
        This->config_watch_stop = NULL;
    }
}

/* Implies Stop() and DisposeBuffers(). */

HIDDEN ULONG STDMETHODCALLTYPE
Release(LPPIPEASIO iface)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    ULONG          ref  = InterlockedDecrement(&This->ref);

    TRACE("iface: %p, ref count is %u\n", iface, (unsigned)ref);

    if (ref != 0)
        return ref;

    if (This->host_driver_state == Running)
        Stop(iface);
    if (This->host_driver_state == Prepared)
        DisposeBuffers(iface);

    if (This->host_driver_state == Initialized)
    {
        /* just for good measure we deinitialize IOChannel structures and unregister ports */
        for (int i = 0; i < This->pipeasio_number_inputs; i++)
        {
            audio_port_unregister(This->audio_client, This->input_channel[i].port);
            This->input_channel[i].active = false;
            This->input_channel[i].port   = NULL;
        }
        for (int i = 0; i < This->pipeasio_number_outputs; i++)
        {
            audio_port_unregister(This->audio_client, This->output_channel[i].port);
            This->output_channel[i].active = false;
            This->output_channel[i].port   = NULL;
        }
        This->host_active_inputs = This->host_active_outputs = 0;
        TRACE("%i IOChannel structures released\n",
              This->pipeasio_number_inputs + This->pipeasio_number_outputs);

        audio_free_ports(This->phys_output_ports);
        audio_free_ports(This->phys_input_ports);
        stop_config_watch(This);
        audio_close(This->audio_client);
        if (This->input_channel)
            HeapFree(GetProcessHeap(), 0, This->input_channel);
    }
    TRACE("PipeASIO terminated\n\n");
    DeleteCriticalSection(&This->config_lock);
    HeapFree(GetProcessHeap(), 0, This);
    return ref;
}

/* sysRef is 0 on OS/X; on Windows it is the application's main window handle.
 * Returns 0 on error, 1 on success. */

DEFINE_THISCALL_WRAPPER(Init, 8)
HIDDEN LONG STDMETHODCALLTYPE
Init(LPPIPEASIO iface, void *sysRef)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    uint32_t       audio_status;
    uint32_t       audio_options = AUDIO_NULL_OPTION;
    int            i;

    This->sys_ref = sysRef;
    /* Do not call mlockall(MCL_FUTURE).  Wine's win32u maps USER shared memory
     * after driver init; locking those pages can exhaust RLIMIT_MEMLOCK and
     * crash plugin-heavy hosts.  PipeWire's RT module owns paging. */
    configure_driver(This);

    if (!(This->audio_client = audio_open(This->client_name, audio_options, &audio_status)))
    {
        WARN("Unable to open an audio client as: %s\n", This->client_name);
        return 0;
    }
    TRACE("audio client opened as: '%s'\n", audio_get_client_name(This->audio_client));

    audio_set_forced_rate(This->audio_client, (audio_nframes_t)This->pipeasio_sample_rate);
    audio_set_follow_device(This->audio_client, This->pipeasio_follow_device_clock);

    This->host_sample_rate = audio_get_sample_rate(This->audio_client);
    /* Before CreateBuffers, report the configured preferred size. */
    This->host_current_buffersize = This->pipeasio_preferred_buffersize;
    if (This->pipeasio_follow_device_clock)
    {
        /* First guess until the watcher observes the device quantum. */
        LONG hint = atomic_load(&This->follower_quantum);
        if (hint)
            This->host_current_buffersize = hint;
    }

    /* Zeroed: CreateBuffers initializes audio_buffer and active. */
    This->input_channel = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    (This->pipeasio_number_inputs + This->pipeasio_number_outputs)
                                            * sizeof(IOChannel));
    if (!This->input_channel)
    {
        audio_close(This->audio_client);
        ERR("Unable to allocate IOChannel structures for %i channels\n",
            This->pipeasio_number_inputs);
        return 0;
    }
    This->output_channel = This->input_channel + This->pipeasio_number_inputs;
    TRACE("%i IOChannel structures allocated\n",
          This->pipeasio_number_inputs + This->pipeasio_number_outputs);

    This->phys_input_ports = audio_get_ports(This->audio_client, NULL, NULL,
                                             AUDIO_PORT_IS_PHYSICAL | AUDIO_PORT_IS_OUTPUT);
    for (This->num_phys_input_ports = 0;
         This->phys_input_ports && This->phys_input_ports[This->num_phys_input_ports];
         This->num_phys_input_ports++)
        ;
    This->phys_output_ports = audio_get_ports(This->audio_client, NULL, NULL,
                                              AUDIO_PORT_IS_PHYSICAL | AUDIO_PORT_IS_INPUT);
    for (This->num_phys_output_ports = 0;
         This->phys_output_ports && This->phys_output_ports[This->num_phys_output_ports];
         This->num_phys_output_ports++)
        ;

    for (i = 0; i < This->pipeasio_number_inputs; i++)
    {
        This->input_channel[i].active = false;
        This->input_channel[i].port   = NULL;
        snprintf(This->input_channel[i].port_name, PIPEASIO_MAX_NAME_LENGTH, "in_%i", i + 1);
        This->input_channel[i].port
                = audio_port_register(This->audio_client, This->input_channel[i].port_name,
                                      AUDIO_DEFAULT_TYPE, AUDIO_PORT_IS_INPUT, i);
    }
    for (i = 0; i < This->pipeasio_number_outputs; i++)
    {
        This->output_channel[i].active = false;
        This->output_channel[i].port   = NULL;
        snprintf(This->output_channel[i].port_name, PIPEASIO_MAX_NAME_LENGTH, "out_%i", i + 1);
        This->output_channel[i].port
                = audio_port_register(This->audio_client, This->output_channel[i].port_name,
                                      AUDIO_DEFAULT_TYPE, AUDIO_PORT_IS_OUTPUT, i);
    }
    TRACE("%i IOChannel structures initialized\n",
          This->pipeasio_number_inputs + This->pipeasio_number_outputs);

    if (!audio_set_buffer_size_callback(This->audio_client, buffer_size_callback, This))
    {
        audio_close(This->audio_client);
        HeapFree(GetProcessHeap(), 0, This->input_channel);
        ERR("Unable to register buffer size change callback\n");
        audio_free_ports(This->phys_input_ports);
        audio_free_ports(This->phys_output_ports);
        return 0;
    }

    if (!audio_set_latency_callback(This->audio_client, latency_callback, This))
    {
        audio_close(This->audio_client);
        HeapFree(GetProcessHeap(), 0, This->input_channel);
        ERR("Unable to register latency callback\n");
        audio_free_ports(This->phys_input_ports);
        audio_free_ports(This->phys_output_ports);
        return 0;
    }

    if (!audio_set_process_callback(This->audio_client, process_callback, This))
    {
        audio_close(This->audio_client);
        HeapFree(GetProcessHeap(), 0, This->input_channel);
        ERR("Unable to register process callback\n");
        audio_free_ports(This->phys_input_ports);
        audio_free_ports(This->phys_output_ports);
        return 0;
    }

    if (!audio_set_sample_rate_callback(This->audio_client, sample_rate_callback, This))
    {
        audio_close(This->audio_client);
        HeapFree(GetProcessHeap(), 0, This->input_channel);
        ERR("Unable to register sample rate change callback\n");
        audio_free_ports(This->phys_input_ports);
        audio_free_ports(This->phys_output_ports);
        return 0;
    }

    This->host_driver_state = Initialized;
    TRACE("PipeASIO " PIPEASIO_VERSION " initialized\n");
    return 1;
}

DEFINE_THISCALL_WRAPPER(GetDriverName, 8)
HIDDEN void STDMETHODCALLTYPE
GetDriverName(LPPIPEASIO iface, char *name)
{
    TRACE("iface: %p, name: %p\n", iface, name);
    strcpy(name, "PipeASIO");
    return;
}

DEFINE_THISCALL_WRAPPER(GetDriverVersion, 4)
HIDDEN LONG STDMETHODCALLTYPE
GetDriverVersion(LPPIPEASIO iface)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;

    TRACE("iface: %p\n", iface);
    return This->host_version;
}

DEFINE_THISCALL_WRAPPER(GetErrorMessage, 8)
HIDDEN void STDMETHODCALLTYPE
GetErrorMessage(LPPIPEASIO iface, char *string)
{
    TRACE("iface: %p, string: %p)\n", iface, string);
    strcpy(string, "PipeASIO does not return error messages\n");
    return;
}

/* Returns -1000 if IO is missing, -999 if the audio backend fails to start. */

DEFINE_THISCALL_WRAPPER(Start, 4)
HIDDEN LONG STDMETHODCALLTYPE
Start(LPPIPEASIO iface)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    int            i;

    TRACE("iface: %p\n", iface);

    if (This->host_driver_state != Prepared)
        return -1000;

    for (i = 0; i < (This->pipeasio_number_inputs + This->pipeasio_number_outputs) * 2
                            * This->host_current_buffersize;
         i++)
        This->callback_audio_buffer[i] = 0;

    /* prime the callback by preprocessing one outbound host bufffer */
    This->host_buffer_index = 0;
    atomic_store_explicit(&This->host_num_samples, 0, memory_order_relaxed);
    atomic_store_explicit(&This->host_time_stamp, 0, memory_order_relaxed);

    /* systemTime from the PipeWire graph clock - 0 until the first process
     * cycle runs, which is fine for the one-shot prime. */
    pipeasio_host_buffer_switch(This, This->host_buffer_index, 0,
                                audio_get_time_nsec(This->audio_client));

    This->host_buffer_index = This->host_buffer_index ? 0 : 1;

    This->host_driver_state = Running;
    TRACE("PipeASIO successfully loaded\n");
    return 0;
}

/* Returns -1000 if IO is missing. swapBuffers() must not be called after this returns. */

DEFINE_THISCALL_WRAPPER(Stop, 4)
HIDDEN LONG STDMETHODCALLTYPE
Stop(LPPIPEASIO iface)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;

    TRACE("iface: %p\n", iface);

    if (This->host_driver_state != Running)
        return -1000;

    This->host_driver_state = Prepared;

    return 0;
}

/* Returns -1000 if no channels are available, otherwise AES_OK. */

DEFINE_THISCALL_WRAPPER(GetChannels, 12)
HIDDEN LONG STDMETHODCALLTYPE
GetChannels(LPPIPEASIO iface, LONG *numInputChannels, LONG *numOutputChannels)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;

    if (!numInputChannels || !numOutputChannels)
        return -998;

    *numInputChannels  = This->pipeasio_number_inputs;
    *numOutputChannels = This->pipeasio_number_outputs;
    TRACE("iface: %p, inputs: %i, outputs: %i\n", iface, This->pipeasio_number_inputs,
          This->pipeasio_number_outputs);
    return 0;
}

/* Returns -1000 if no IO is available, otherwise AES_OK. */

DEFINE_THISCALL_WRAPPER(GetLatencies, 12)
HIDDEN LONG STDMETHODCALLTYPE
GetLatencies(LPPIPEASIO iface, LONG *inputLatency, LONG *outputLatency)
{
    IPipeASIOImpl        *This = (IPipeASIOImpl *)iface;
    audio_latency_range_t range;

    if (!inputLatency || !outputLatency)
        return -998;

    if (This->host_driver_state == Loaded)
        return -1000;

    audio_port_get_latency_range(This->input_channel[0].port, AUDIO_CAPTURE_LATENCY, &range);
    *inputLatency = range.max;
    audio_port_get_latency_range(This->output_channel[0].port, AUDIO_PLAYBACK_LATENCY, &range);
    *outputLatency = range.max;
    TRACE("iface: %p, input latency: %d, output latency: %d\n", iface, (int)*inputLatency,
          (int)*outputLatency);

    return 0;
}

/* Currently reports all sizes the same with granularity 0. Returns -1000 on missing IO. */

DEFINE_THISCALL_WRAPPER(GetBufferSize, 20)
HIDDEN LONG STDMETHODCALLTYPE
GetBufferSize(LPPIPEASIO iface, LONG *minSize, LONG *maxSize, LONG *preferredSize,
              LONG *granularity)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;

    TRACE("iface: %p, minSize: %p, maxSize: %p, preferredSize: %p, granularity: %p\n", iface,
          minSize, maxSize, preferredSize, granularity);

    if (!minSize || !maxSize || !preferredSize || !granularity)
        return -998;

    bool pending = atomic_load_explicit(&This->config_pending, memory_order_acquire);
    BOOL fixed   = This->pipeasio_fixed_buffersize;
    BOOL follow  = This->pipeasio_follow_device_clock;
    LONG pref    = This->pipeasio_preferred_buffersize;
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
        LONG q   = atomic_load_explicit(&This->follower_quantum, memory_order_relaxed);
        *minSize = *maxSize = *preferredSize = (follow && q) ? q : pref;
        *granularity                         = 0;
        TRACE("Buffersize fixed at %d (pending=%d)\n", (int)*preferredSize, (int)pending);
        return 0;
    }

    *minSize       = PIPEASIO_MINIMUM_BUFFERSIZE;
    *maxSize       = PIPEASIO_MAXIMUM_BUFFERSIZE;
    *preferredSize = pref;
    *granularity   = -1;
    TRACE("The host can control buffersize (min=%d max=%d preferred=%d)\n", (int)*minSize,
          (int)*maxSize, (int)*preferredSize);
    return 0;
}

/* Returns -995 if the sample rate isn't available, -1000 on missing IO. */

DEFINE_THISCALL_WRAPPER(CanSampleRate, 12)
HIDDEN LONG STDMETHODCALLTYPE
CanSampleRate(LPPIPEASIO iface, double sampleRate)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;

    TRACE("iface: %p, Samplerate = %li, requested samplerate = %li\n", iface,
          (long)This->host_sample_rate, (long)sampleRate);

    if (sampleRate != This->host_sample_rate)
        return -995;
    return 0;
}

/* currentRate holds 0 if unknown.
 * Returns -995 if the sample rate is unknown, -1000 on missing IO. */

DEFINE_THISCALL_WRAPPER(GetSampleRate, 8)
HIDDEN LONG STDMETHODCALLTYPE
GetSampleRate(LPPIPEASIO iface, double *sampleRate)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;

    TRACE("iface: %p, Sample rate is %i\n", iface, (int)This->host_sample_rate);

    if (!sampleRate)
        return -998;

    *sampleRate = This->host_sample_rate;
    return 0;
}

/* SR == 0 enables external sync. Returns -995 on unknown SR, -997 if the
 * current clock is external and SR != 0, -1000 on missing IO. */

DEFINE_THISCALL_WRAPPER(SetSampleRate, 12)
HIDDEN LONG STDMETHODCALLTYPE
SetSampleRate(LPPIPEASIO iface, double sampleRate)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;

    TRACE("iface: %p, Sample rate %f requested\n", iface, sampleRate);

    if (sampleRate != This->host_sample_rate)
        return -995;
    return 0;
}

/* numSources: on entry the number of allocated members, on return the number
 * of clock sources (minimum 1, the internal clock). Returns -1000 on missing IO. */

DEFINE_THISCALL_WRAPPER(GetClockSources, 12)
HIDDEN LONG STDMETHODCALLTYPE
GetClockSources(LPPIPEASIO iface, void *clocks, LONG *numSources)
{
    LONG *lclocks = (LONG *)clocks;

    TRACE("iface: %p, clocks: %p, numSources: %p\n", iface, clocks, numSources);

    if (!clocks || !numSources)
        return -998;

    *lclocks++ = 0;
    *lclocks++ = -1;
    *lclocks++ = -1;
    *lclocks++ = 1;
    strcpy((char *)lclocks, "Internal");
    *numSources = 1;
    return 0;
}

/* index is one returned by GetClockSources(). Returns -1000 on missing IO;
 * -997 if a clock can't be selected; -995 should not be returned. */

DEFINE_THISCALL_WRAPPER(SetClockSource, 8)
HIDDEN LONG STDMETHODCALLTYPE
SetClockSource(LPPIPEASIO iface, LONG index)
{
    TRACE("iface: %p, index: %d\n", iface, (int)index);

    if (index != 0)
        return -1000;
    return 0;
}

/* sPos holds the position, reset to 0 on Start(); tStamp holds the system time
 * of sPos. Returns -1000 on missing IO, -996 on missing clock. */

DEFINE_THISCALL_WRAPPER(GetSamplePosition, 12)
HIDDEN LONG STDMETHODCALLTYPE
GetSamplePosition(LPPIPEASIO iface, w_int64_t *sPos, w_int64_t *tStamp)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;

    TRACE("iface: %p, sPos: %p, tStamp: %p\n", iface, sPos, tStamp);

    if (!sPos || !tStamp)
        return -998;

    uint64_t stamp   = atomic_load_explicit(&This->host_time_stamp, memory_order_relaxed);
    uint64_t samples = atomic_load_explicit(&This->host_num_samples, memory_order_relaxed);

    tStamp->lo = (ULONG)(stamp & 0xFFFFFFFFu);
    tStamp->hi = (ULONG)(stamp >> 32);
    sPos->lo   = (ULONG)(samples & 0xFFFFFFFFu);
    sPos->hi   = (ULONG)(samples >> 32);

    return 0;
}

/* Returns -1000 on missing IO. */

DEFINE_THISCALL_WRAPPER(GetChannelInfo, 8)
HIDDEN LONG STDMETHODCALLTYPE
GetChannelInfo(LPPIPEASIO iface, void *info)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    if (!info)
        return -998;
    LONG *linfo = (LONG *)info;

    const LONG channelNumber = *linfo++;
    const LONG isInputType   = *linfo++;

    if (channelNumber < 0
        || (isInputType ? channelNumber >= This->pipeasio_number_inputs
                        : channelNumber >= This->pipeasio_number_outputs))
        return -998;

    *linfo++ = (isInputType ? This->input_channel : This->output_channel)[channelNumber].active;
    *linfo++ = 0;
    *linfo++ = 19;
    memcpy(linfo,
           (isInputType ? This->input_channel : This->output_channel)[channelNumber].port_name,
           PIPEASIO_MAX_NAME_LENGTH);

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
        This->pipeasio_preferred_buffersize = cfg.buffer_size; /* loader pow2-validated */
        This->pipeasio_sample_rate          = cfg.sample_rate;
        lstrcpynA(This->pipeasio_output_device, cfg.output_device,
                  sizeof This->pipeasio_output_device);
        lstrcpynA(This->pipeasio_input_device, cfg.input_device,
                  sizeof This->pipeasio_input_device);

        audio_set_forced_rate(This->audio_client, (audio_nframes_t)This->pipeasio_sample_rate);
        audio_set_follow_device(This->audio_client, This->pipeasio_follow_device_clock);
        TRACE("config: applied live reload (buffer_size=%d rate=%d follow=%d auto=%d)\n",
              (int)This->pipeasio_preferred_buffersize, This->pipeasio_sample_rate,
              (int)This->pipeasio_follow_device_clock, (int)This->pipeasio_connect_to_hardware);
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
    IPipeASIOImpl     *This                 = (IPipeASIOImpl *)iface;
    BufferInformation *bufferInfoPerChannel = bufferInfo;
    int                i, j, k;

    TRACE("iface: %p, bufferInfo: %p, numChannels: %d, bufferSize: %d, callbacks: %p\n", iface,
          bufferInfo, (int)numChannels, (int)bufferSize, callbacks);

    if (This->host_driver_state != Initialized)
        return -1000;

    if (!bufferInfo || !callbacks)
        return -997;

    /* RT data loop is stopped here (post-DisposeBuffers); safe to commit a
     * config the watcher staged while Running. */
    apply_pending_config(This);

    /* Check for invalid channel numbers.  Both the count-per-direction
     * AND each entry's channelNumber must be in range - without the
     * latter, a host that passes channelNumber=99 walks straight off
     * input_channel[]/output_channel[] into adjacent heap. */
    for (i = j = k = 0; i < numChannels; i++, bufferInfoPerChannel++)
    {
        if (bufferInfoPerChannel->isInputType)
        {
            if (j++ >= This->pipeasio_number_inputs)
            {
                WARN("Invalid input channel requested (too many)\n");
                return -997;
            }
            if (bufferInfoPerChannel->channelNumber < 0
                || bufferInfoPerChannel->channelNumber >= This->pipeasio_number_inputs)
            {
                WARN("Invalid input channelNumber %ld (max %d)\n",
                     (long)bufferInfoPerChannel->channelNumber, This->pipeasio_number_inputs - 1);
                return -997;
            }
        }
        else
        {
            if (k++ >= This->pipeasio_number_outputs)
            {
                WARN("Invalid output channel requested (too many)\n");
                return -997;
            }
            if (bufferInfoPerChannel->channelNumber < 0
                || bufferInfoPerChannel->channelNumber >= This->pipeasio_number_outputs)
            {
                WARN("Invalid output channelNumber %ld (max %d)\n",
                     (long)bufferInfoPerChannel->channelNumber, This->pipeasio_number_outputs - 1);
                return -997;
            }
        }
    }

    /* set buf_size */
    if (This->pipeasio_fixed_buffersize || This->pipeasio_follow_device_clock)
    {
        if (This->host_current_buffersize != bufferSize)
            return -997;
        /* Sync the backend buffer size so audio_activate forces the matching
         * PipeWire quantum.  Without this the graph keeps its default quantum
         * while the host fills only bufferSize frames per cycle - the daemon
         * then plays buffer-worth of real audio stretched across a larger
         * quantum, i.e. slow, pitched-down output. */
        if (!audio_set_buffer_size(This->audio_client, bufferSize))
        {
            WARN("Unable to set buffersize to %d\n", (int)bufferSize);
            return -999;
        }
        TRACE("Buffersize fixed at %d\n", (int)This->host_current_buffersize);
    }
    else
    { /* fail if not a power of two and if out of range */
        if (!(bufferSize > 0 && !(bufferSize & (bufferSize - 1))
              && bufferSize >= PIPEASIO_MINIMUM_BUFFERSIZE
              && bufferSize <= PIPEASIO_MAXIMUM_BUFFERSIZE))
        {
            WARN("Invalid buffersize %d requested\n", (int)bufferSize);
            return -997;
        }
        else
        {
            /* Always push the host size to the backend before audio_activate. */
            This->host_current_buffersize = bufferSize;
            if (!audio_set_buffer_size(This->audio_client, bufferSize))
            {
                WARN("Unable to set buffersize to %d\n", (int)bufferSize);
                return -999;
            }
            TRACE("Buffer size set to %d\n", (int)bufferSize);
        }
    }

    This->host_callbacks      = callbacks;
    This->host_time_info_mode = FALSE;

    if (This->host_callbacks->sendNotification(7, 0, 0, 0))
        This->host_time_info_mode = TRUE;

    /* Allocate audio buffers */

    const size_t cb_bytes = pipeasio_host_callback_size_bytes(
            This->pipeasio_number_inputs, This->pipeasio_number_outputs,
            This->host_current_buffersize, sizeof(audio_sample_t));
    This->callback_audio_buffer = HeapAlloc(GetProcessHeap(), 0, cb_bytes);
    if (!This->callback_audio_buffer)
    {
        ERR("Unable to allocate %i audio buffers\n",
            This->pipeasio_number_inputs + This->pipeasio_number_outputs);
        return -994;
    }
    TRACE("%i audio buffers allocated (%zu kB) base=%p end=%p\n",
          This->pipeasio_number_inputs + This->pipeasio_number_outputs, cb_bytes / 1024,
          This->callback_audio_buffer, (void *)((char *)This->callback_audio_buffer + cb_bytes));

    for (i = 0; i < This->pipeasio_number_inputs; i++)
        This->input_channel[i].audio_buffer
                = This->callback_audio_buffer
                  + pipeasio_host_input_offset_samples(i, This->host_current_buffersize);
    for (i = 0; i < This->pipeasio_number_outputs; i++)
        This->output_channel[i].audio_buffer
                = This->callback_audio_buffer
                  + pipeasio_host_output_offset_samples(i, This->pipeasio_number_inputs,
                                                        This->host_current_buffersize);

    /* initialize BufferInformation structures */
    bufferInfoPerChannel     = bufferInfo;
    This->host_active_inputs = This->host_active_outputs = 0;

    for (i = 0; i < This->pipeasio_number_inputs; i++)
    {
        This->input_channel[i].active = false;
    }
    for (i = 0; i < This->pipeasio_number_outputs; i++)
    {
        This->output_channel[i].active = false;
    }

    for (i = 0; i < numChannels; i++, bufferInfoPerChannel++)
    {
        const LONG ch = bufferInfoPerChannel->channelNumber;
        if (bufferInfoPerChannel->isInputType)
        {
            bufferInfoPerChannel->audioBufferStart = &This->input_channel[ch].audio_buffer[0];
            bufferInfoPerChannel->audioBufferEnd
                    = &This->input_channel[ch].audio_buffer[This->host_current_buffersize];
            This->input_channel[ch].active = true;
            This->host_active_inputs++;
            TRACE("  bufferInfo[%d]: IN  ch=%ld start=%p end=%p (cb_offset=%zu)\n", i, (long)ch,
                  bufferInfoPerChannel->audioBufferStart, bufferInfoPerChannel->audioBufferEnd,
                  (size_t)((char *)bufferInfoPerChannel->audioBufferStart
                           - (char *)This->callback_audio_buffer));
        }
        else
        {
            bufferInfoPerChannel->audioBufferStart = &This->output_channel[ch].audio_buffer[0];
            bufferInfoPerChannel->audioBufferEnd
                    = &This->output_channel[ch].audio_buffer[This->host_current_buffersize];
            This->output_channel[ch].active = true;
            This->host_active_outputs++;
            TRACE("  bufferInfo[%d]: OUT ch=%ld start=%p end=%p (cb_offset=%zu)\n", i, (long)ch,
                  bufferInfoPerChannel->audioBufferStart, bufferInfoPerChannel->audioBufferEnd,
                  (size_t)((char *)bufferInfoPerChannel->audioBufferStart
                           - (char *)This->callback_audio_buffer));
        }
    }
    TRACE("%d audio channels initialized (active_in=%d active_out=%d)\n",
          (int)(This->host_active_inputs + This->host_active_outputs), This->host_active_inputs,
          This->host_active_outputs);

#ifdef PIPEASIO_WOW64_PE
    /* Hand the shared callback buffer + channel activity to the unix RT loop;
     * the gather/scatter runs unix-side (see src/wow64/audio_unix.c). */
    {
        bool in_active[PIPEASIO_MAX_CHANNELS];
        bool out_active[PIPEASIO_MAX_CHANNELS];
        for (i = 0; i < This->pipeasio_number_inputs; i++)
            in_active[i] = This->input_channel[i].active;
        for (i = 0; i < This->pipeasio_number_outputs; i++)
            out_active[i] = This->output_channel[i].active;
        pipeasio_wow64_bind_rt(This->audio_client, This->callback_audio_buffer,
                               This->host_current_buffersize, This->pipeasio_number_inputs,
                               This->pipeasio_number_outputs, in_active, out_active);
    }
#endif

    if (!audio_activate(This->audio_client))
    {
        for (i = 0; i < This->pipeasio_number_inputs; i++)
        {
            This->input_channel[i].audio_buffer = NULL;
            This->input_channel[i].active       = false;
        }
        for (i = 0; i < This->pipeasio_number_outputs; i++)
        {
            This->output_channel[i].audio_buffer = NULL;
            This->output_channel[i].active       = false;
        }
        HeapFree(GetProcessHeap(), 0, This->callback_audio_buffer);
        This->callback_audio_buffer = NULL;
        This->host_callbacks        = NULL;
        This->host_active_inputs = This->host_active_outputs = 0;
        return -1000;
    }

    /* Connect to hardware: a chosen device (by node.name) or the first
     * available one ("" => default).  Our inputs read FROM a source's output
     * ports; our outputs write TO a sink's input ports. */
    if (This->pipeasio_connect_to_hardware)
    {
        const char **in_src
                = This->pipeasio_input_device[0]
                          ? audio_get_device_ports(This->audio_client, This->pipeasio_input_device,
                                                   AUDIO_PORT_IS_OUTPUT)
                          : NULL;
        const char **out_dst
                = This->pipeasio_output_device[0]
                          ? audio_get_device_ports(This->audio_client, This->pipeasio_output_device,
                                                   AUDIO_PORT_IS_INPUT)
                          : NULL;
        const char **use_in  = in_src ? in_src : This->phys_input_ports;
        const char **use_out = out_dst ? out_dst : This->phys_output_ports;

        for (i = 0; use_in && use_in[i] && i < This->pipeasio_number_inputs; i++)
        {
            const char *type = audio_port_type(audio_port_by_name(This->audio_client, use_in[i]));
            if (type && strstr(type, "audio"))
                audio_connect(This->audio_client, use_in[i],
                              audio_port_name(This->input_channel[i].port));
        }
        for (i = 0; use_out && use_out[i] && i < This->pipeasio_number_outputs; i++)
        {
            const char *type = audio_port_type(audio_port_by_name(This->audio_client, use_out[i]));
            if (type && strstr(type, "audio"))
                audio_connect(This->audio_client, audio_port_name(This->output_channel[i].port),
                              use_out[i]);
        }

        if (in_src)
            audio_free_ports(in_src);
        if (out_dst)
            audio_free_ports(out_dst);
    }

    /* at this point all the connections are made and the process callback is outputting silence */
    This->host_driver_state = Prepared;

    /* Watch config.ini for panel edits and reset on change (live reload).
     * Reap any prior watcher (e.g. left by a reentrant self-stop) before
     * starting a fresh one; no-op on the normal path. */
    stop_config_watch(This);
    This->config_watch_stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (This->config_watch_stop)
    {
        This->config_watch_thread
                = CreateThread(NULL, 0, config_watch_proc, This, 0, &This->config_watch_tid);
        if (!This->config_watch_thread)
        {
            CloseHandle(This->config_watch_stop);
            This->config_watch_stop = NULL;
            WARN("config watcher: CreateThread failed, live reload disabled\n");
        }
    }
    else
        WARN("config watcher: CreateEvent failed, live reload disabled\n");
    return 0;
}

/* Implies Stop(). Returns -997 if no buffers were previously allocated, -1000 on missing IO. */

DEFINE_THISCALL_WRAPPER(DisposeBuffers, 4)
HIDDEN LONG STDMETHODCALLTYPE
DisposeBuffers(LPPIPEASIO iface)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)iface;
    int            i;

    TRACE("iface: %p\n", iface);

    /* Stop the live-config watcher before any teardown so no in-flight reset
     * request races with Stop()/host_callbacks going away. */
    stop_config_watch(This);

    if (This->host_driver_state == Running)
        Stop(iface);
    if (This->host_driver_state != Prepared)
        return -1000;

    if (!audio_deactivate(This->audio_client))
        return -1000;

    This->host_callbacks = NULL;

    for (i = 0; i < This->pipeasio_number_inputs; i++)
    {
        This->input_channel[i].audio_buffer = NULL;
        This->input_channel[i].active       = false;
    }
    for (i = 0; i < This->pipeasio_number_outputs; i++)
    {
        This->output_channel[i].audio_buffer = NULL;
        This->output_channel[i].active       = false;
    }
    This->host_active_inputs = This->host_active_outputs = 0;

    if (This->callback_audio_buffer)
        HeapFree(GetProcessHeap(), 0, This->callback_audio_buffer);
    This->callback_audio_buffer = NULL;

    This->host_driver_state = Initialized;
    return 0;
}

/* Returns -1000 if no control panel exists, but the return code should be
 * ignored. Call sendNotification if something changed. */

DEFINE_THISCALL_WRAPPER(ControlPanel, 4)
HIDDEN LONG STDMETHODCALLTYPE
ControlPanel(LPPIPEASIO iface)
{
    char cfg_path[1024];
    char message[1536];

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
    return 0;
}

/* Returns -998 on invalid or unsupported selector, 0x3f4847a0 on success (never 0). */

DEFINE_THISCALL_WRAPPER(Future, 12)
HIDDEN LONG STDMETHODCALLTYPE
Future(LPPIPEASIO iface, LONG selector, void *opt)
{
    TRACE("iface: %p, selector: %d, opt: %p\n", iface, (int)selector, opt);

    switch (selector)
    {
    case 1:
    case 2:
        TRACE("The driver does not support TimeCode\n");
        return -998;
    case 3:
        TRACE("The driver denied request to set input monitor\n");
        return -1000;
    case 4:
        TRACE("The driver denied request for Transport control\n");
        return -998;
    case 5:
        TRACE("The driver denied request to set input gain\n");
        return -998;
    case 6:
        TRACE("The driver denied request to get input meter \n");
        return -998;
    case 7:
        TRACE("The driver denied request to set output gain\n");
        return -998;
    case 8:
        TRACE("The driver denied request to get output meter\n");
        return -998;
    case 9:
        TRACE("The driver does not support input monitor\n");
        return -998;
    case 10:
        TRACE("The driver supports TimeInfo\n");
        return 0x3f4847a0;
    case 11:
        TRACE("The driver does not support TimeCode\n");
        return -998;
    case 12:
        TRACE("The driver denied request for Transport\n");
        return -998;
    case 13:
        TRACE("The driver does not support input gain\n");
        return -998;
    case 14:
        TRACE("The driver does not support input meter\n");
        return -998;
    case 15:
        TRACE("The driver does not support output gain\n");
        return -998;
    case 16:
        TRACE("The driver does not support output meter\n");
        return -998;
    case 0x23111961:
        TRACE("The driver denied request to set DSD IO format\n");
        return -1000;
    case 0x23111983:
        TRACE("The driver denied request to get DSD IO format\n");
        return -1000;
    case 0x23112004:
        TRACE("The driver does not support DSD IO format\n");
        return -1000;
    default:
        TRACE("ASIOFuture() called with undocumented selector\n");
        return -998;
    }
}

/* Returns 0 if supported, -1000 to disable. */

DEFINE_THISCALL_WRAPPER(OutputReady, 4)
HIDDEN LONG STDMETHODCALLTYPE
OutputReady(LPPIPEASIO iface)
{
    return -1000;
}

/****************************************************************************
 *  ASIO process callbacks
 */

static inline int
buffer_size_callback(audio_nframes_t nframes, void *arg)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)arg;

    if (This->host_driver_state != Running)
        return 0;

    if (This->host_callbacks->sendNotification(1, 3, 0, 0))
        This->host_callbacks->sendNotification(3, 0, 0, 0);
    return 0;
}

static inline void
latency_callback(audio_latency_mode_t mode, void *arg)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)arg;

    if (This->host_driver_state != Running)
        return;

    if (This->host_callbacks->sendNotification(1, 6, 0, 0))
        This->host_callbacks->sendNotification(6, 0, 0, 0);

    return;
}

/* Host-callback bridge. */

void
pipeasio_host_buffer_switch(void *This, int32_t buffer_index, audio_nframes_t add_samples,
                            uint64_t time_nsec)
{
    IPipeASIOImpl *impl = (IPipeASIOImpl *)This;
    Callbacks     *cb   = atomic_load_explicit(&impl->host_callbacks, memory_order_relaxed);

    /* Native 64-bit counters, split into the ASIO hi/lo wire format only at
     * the edges.  Single RT writer; relaxed atomics keep GetSamplePosition's
     * COM-thread reads untorn. */
    uint64_t samples = atomic_load_explicit(&impl->host_num_samples, memory_order_relaxed)
                       + add_samples;
    atomic_store_explicit(&impl->host_num_samples, samples, memory_order_relaxed);
    atomic_store_explicit(&impl->host_time_stamp, time_nsec, memory_order_relaxed);

    if (impl->host_time_info_mode)
    {
        impl->host_time._2            = 1.0;
        impl->host_time.numSamples.lo = (ULONG)(samples & 0xFFFFFFFFu);
        impl->host_time.numSamples.hi = (ULONG)(samples >> 32);
        impl->host_time.timeStamp.lo  = (ULONG)(time_nsec & 0xFFFFFFFFu);
        impl->host_time.timeStamp.hi  = (ULONG)(time_nsec >> 32);
        impl->host_time.sampleRate    = impl->host_sample_rate;
        impl->host_time.flags         = 0x7;

        cb->swapBuffersWithTimeInfo(&impl->host_time, buffer_index, 1);
    }
    else
    {
        cb->swapBuffers(buffer_index, 1);
    }
}

int
pipeasio_host_is_running(void *This)
{
    return ((IPipeASIOImpl *)This)->host_driver_state == Running;
}

static inline int
process_callback(audio_nframes_t nframes, void *arg)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)arg;

    int i;

    /* output silence if the host callback isn't running yet */
    if (atomic_load_explicit(&This->host_driver_state, memory_order_relaxed) != Running)
    {
        for (i = 0; i < This->host_active_outputs; i++)
        {
            audio_sample_t *dst = audio_port_get_buffer(This->output_channel[i].port, nframes);
            audio_nframes_t avail = audio_port_buffer_avail_frames(This->output_channel[i].port);
            audio_nframes_t n     = (dst && avail < nframes) ? avail : (dst ? nframes : 0);
            if (n)
                memset(dst, 0, sizeof(audio_sample_t) * n);
        }
        return 0;
    }

    /* copy device input to host buffers */
    for (i = 0; i < This->pipeasio_number_inputs; i++)
        if (This->input_channel[i].active)
        {
            audio_sample_t *src = audio_port_get_buffer(This->input_channel[i].port, nframes);
            audio_sample_t *dst
                    = &This->input_channel[i].audio_buffer[nframes * This->host_buffer_index];
            audio_nframes_t avail = audio_port_buffer_avail_frames(This->input_channel[i].port);
            audio_nframes_t n     = (src && avail < nframes) ? avail : (src ? nframes : 0);
            if (n)
                memcpy(dst, src, sizeof(audio_sample_t) * n);
            if (n < nframes)
                memset(dst + n, 0, sizeof(audio_sample_t) * (nframes - n));
        }

    pipeasio_host_buffer_switch(This, This->host_buffer_index, nframes,
                                audio_get_time_nsec(This->audio_client));

    /* copy host to device output buffers */
    for (i = 0; i < This->pipeasio_number_outputs; i++)
        if (This->output_channel[i].active)
        {
            audio_sample_t *dst = audio_port_get_buffer(This->output_channel[i].port, nframes);
            audio_nframes_t avail = audio_port_buffer_avail_frames(This->output_channel[i].port);
            audio_nframes_t n     = (dst && avail < nframes) ? avail : (dst ? nframes : 0);
            if (n)
                memcpy(dst,
                       &This->output_channel[i].audio_buffer[nframes * This->host_buffer_index],
                       sizeof(audio_sample_t) * n);
        }

    This->host_buffer_index = This->host_buffer_index ? 0 : 1;
    return 0;
}

static inline int
sample_rate_callback(audio_nframes_t nframes, void *arg)
{
    IPipeASIOImpl *This = (IPipeASIOImpl *)arg;

    if (This->host_driver_state != Running)
        return 0;

    This->host_sample_rate = nframes;
    This->host_callbacks->sampleRateChanged(nframes);
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

static VOID
configure_driver(IPipeASIOImpl *This)
{
    WCHAR                  application_path[MAX_PATH];
    WCHAR                 *application_name;
    char                   environment_variable[MAX_ENVIRONMENT_SIZE];
    char                   name_env[PIPEASIO_MAX_NAME_LENGTH];
    char                   dev_env[PIPEASIO_DEVICE_NAME_MAX];
    LONG                   result;
    DWORD                  n;
    struct pipeasio_config cfg;

    /* Initialise most member variables.
     * host_num_samples, host_time, & host_time_stamp are initialized in Start()
     * num_phys_input_ports & num_phys_output_ports are initialized in Init() */
    This->host_active_inputs      = 0;
    This->host_active_outputs     = 0;
    This->host_buffer_index       = 0;
    This->host_callbacks          = NULL;
    This->host_current_buffersize = 0;
    This->host_driver_state       = Loaded;
    This->host_sample_rate        = 0;
    This->host_time_info_mode     = FALSE;
    This->host_version            = 92; /* ASIO API version, not PIPEASIO_VERSION */

    /* Load settings from the flat INI the pipeasio-settings panel writes
     * ($XDG_CONFIG_HOME/pipeasio/config.ini).  A missing file yields defaults. */
    char cfg_path[1024] = "";
    pipeasio_config_path(cfg_path, sizeof cfg_path);
#ifdef PIPEASIO_WOW64_PE
    bool cfg_found = pipeasio_wow64_load_config(&cfg);
#else
    bool cfg_found = pipeasio_config_load(&cfg);
#endif
    TRACE("config: %s  path=%s  buffer_size=%d inputs=%d outputs=%d fixed=%d "
          "rate=%d auto=%d out='%s' in='%s'\n",
          cfg_found ? "loaded" : "MISSING -> defaults", cfg_path, cfg.buffer_size, cfg.inputs,
          cfg.outputs, cfg.fixed_buffer_size, cfg.sample_rate, cfg.auto_connect, cfg.output_device,
          cfg.input_device);
    This->pipeasio_number_inputs        = cfg.inputs;
    This->pipeasio_number_outputs       = cfg.outputs;
    This->pipeasio_connect_to_hardware  = cfg.auto_connect ? TRUE : FALSE;
    This->pipeasio_fixed_buffersize     = cfg.fixed_buffer_size ? TRUE : FALSE;
    This->pipeasio_follow_device_clock  = cfg.follow_device_clock ? TRUE : FALSE;
    This->pipeasio_preferred_buffersize = cfg.buffer_size;
    This->pipeasio_sample_rate          = cfg.sample_rate;
    lstrcpynA(This->pipeasio_output_device, cfg.output_device, sizeof This->pipeasio_output_device);
    lstrcpynA(This->pipeasio_input_device, cfg.input_device, sizeof This->pipeasio_input_device);

    This->audio_client          = NULL;
    This->client_name[0]        = 0;
    This->phys_input_ports      = NULL;
    This->phys_output_ports     = NULL;
    This->callback_audio_buffer = NULL;
    This->input_channel         = NULL;
    This->output_channel        = NULL;
    This->config_watch_thread   = NULL;
    This->config_watch_stop     = NULL;

    /* Client (PipeWire node) name: the INI may pin one, otherwise derive it
     * from the host application's executable name. */
    if (cfg.node_name[0])
    {
        lstrcpynA(This->client_name, cfg.node_name, PIPEASIO_MAX_NAME_LENGTH);
    }
    else
    {
        GetModuleFileNameW(0, application_path, MAX_PATH);
        application_name = strrchrW(application_path, L'.');
        if (application_name)
            *application_name = 0;
        application_name = strrchrW(application_path, L'\\');
        application_name = application_name ? application_name + 1 : application_path;
        WideCharToMultiByte(CP_ACP, WC_SEPCHARS, application_name, -1, This->client_name,
                            PIPEASIO_MAX_NAME_LENGTH, NULL, NULL);
    }

    /* Environment variables override INI values. */
    if (GetEnvironmentVariableA("PIPEASIO_NUMBER_INPUTS", environment_variable,
                                MAX_ENVIRONMENT_SIZE))
    {
        errno  = 0;
        result = strtol(environment_variable, 0, 10);
        if (errno != ERANGE)
            This->pipeasio_number_inputs = result;
    }
    if (GetEnvironmentVariableA("PIPEASIO_NUMBER_OUTPUTS", environment_variable,
                                MAX_ENVIRONMENT_SIZE))
    {
        errno  = 0;
        result = strtol(environment_variable, 0, 10);
        if (errno != ERANGE)
            This->pipeasio_number_outputs = result;
    }
    if (This->pipeasio_number_inputs < 0)
        This->pipeasio_number_inputs = 0;
    if (This->pipeasio_number_inputs > PIPEASIO_MAX_CHANNELS)
        This->pipeasio_number_inputs = PIPEASIO_MAX_CHANNELS;
    if (This->pipeasio_number_outputs < 0)
        This->pipeasio_number_outputs = 0;
    if (This->pipeasio_number_outputs > PIPEASIO_MAX_CHANNELS)
        This->pipeasio_number_outputs = PIPEASIO_MAX_CHANNELS;
    if (GetEnvironmentVariableA("PIPEASIO_CONNECT_TO_HARDWARE", environment_variable,
                                MAX_ENVIRONMENT_SIZE))
    {
        if (!strcasecmp(environment_variable, "on"))
            This->pipeasio_connect_to_hardware = TRUE;
        else if (!strcasecmp(environment_variable, "off"))
            This->pipeasio_connect_to_hardware = FALSE;
    }
    if (GetEnvironmentVariableA("PIPEASIO_FIXED_BUFFERSIZE", environment_variable,
                                MAX_ENVIRONMENT_SIZE))
    {
        if (!strcasecmp(environment_variable, "on"))
            This->pipeasio_fixed_buffersize = TRUE;
        else if (!strcasecmp(environment_variable, "off"))
            This->pipeasio_fixed_buffersize = FALSE;
    }
    if (GetEnvironmentVariableA("PIPEASIO_FOLLOW_DEVICE_CLOCK", environment_variable,
                                MAX_ENVIRONMENT_SIZE))
    {
        if (!strcasecmp(environment_variable, "on"))
            This->pipeasio_follow_device_clock = TRUE;
        else if (!strcasecmp(environment_variable, "off"))
            This->pipeasio_follow_device_clock = FALSE;
    }
    if (GetEnvironmentVariableA("PIPEASIO_PREFERRED_BUFFERSIZE", environment_variable,
                                MAX_ENVIRONMENT_SIZE))
    {
        errno  = 0;
        result = strtol(environment_variable, 0, 10);
        if (errno != ERANGE)
            This->pipeasio_preferred_buffersize = result;
    }
    if (GetEnvironmentVariableA("PIPEASIO_SAMPLE_RATE", environment_variable, MAX_ENVIRONMENT_SIZE))
    {
        errno  = 0;
        result = strtol(environment_variable, 0, 10);
        if (errno != ERANGE)
            This->pipeasio_sample_rate = result;
    }
    n = GetEnvironmentVariableA("PIPEASIO_OUTPUT_DEVICE", dev_env, sizeof dev_env);
    if (n > 0 && n < sizeof dev_env)
        lstrcpynA(This->pipeasio_output_device, dev_env, sizeof This->pipeasio_output_device);
    n = GetEnvironmentVariableA("PIPEASIO_INPUT_DEVICE", dev_env, sizeof dev_env);
    if (n > 0 && n < sizeof dev_env)
        lstrcpynA(This->pipeasio_input_device, dev_env, sizeof This->pipeasio_input_device);

    /* override the audio client name */
    n = GetEnvironmentVariableA("PIPEASIO_CLIENT_NAME", name_env, sizeof name_env);
    if (n > 0 && n < sizeof name_env)
        lstrcpynA(This->client_name, name_env, PIPEASIO_MAX_NAME_LENGTH);

    /* if pipeasio_preferred_buffersize is not a power of two or out of range,
     * fall back to PIPEASIO_PREFERRED_BUFFERSIZE */
    if (!(This->pipeasio_preferred_buffersize > 0
          && !(This->pipeasio_preferred_buffersize & (This->pipeasio_preferred_buffersize - 1))
          && This->pipeasio_preferred_buffersize >= PIPEASIO_MINIMUM_BUFFERSIZE
          && This->pipeasio_preferred_buffersize <= PIPEASIO_MAXIMUM_BUFFERSIZE))
        This->pipeasio_preferred_buffersize = PIPEASIO_PREFERRED_BUFFERSIZE;

    return;
}

/* Allocate the interface pointer and associate it with the vtbl/PipeASIO object */
HRESULT WINAPI
PipeASIOCreateInstance(REFIID riid, LPVOID *ppobj)
{
    IPipeASIOImpl *pobj;

    /* Host-facing doubles must start at zero, not indeterminate bytes. */
    pobj = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*pobj));
    if (pobj == NULL)
    {
        WARN("out of memory\n");
        return E_OUTOFMEMORY;
    }

    pobj->lpVtbl = &PipeASIO_Vtbl;
    InitializeCriticalSection(&pobj->config_lock);
    pobj->ref = 1;
    TRACE("pobj = %p\n", pobj);
    *ppobj = pobj;
    return S_OK;
}
