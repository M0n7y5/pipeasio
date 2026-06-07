/*
 * Copyright (C) 2006 Robert Reif
 * Portions copyright (C) 2007 Ralf Beck
 * Portions copyright (C) 2007 Johnny Petrantoni
 * Portions copyright (C) 2007 Stephane Letz
 * Portions copyright (C) 2008 William Steidtmann
 * Portions copyright (C) 2010 Peter L Jones
 * Portions copyright (C) 2010 Torben Hohn
 * Portions copyright (C) 2010 Nedko Arnaudov
 * Portions copyright (C) 2013 Joakim Hernberg
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>

#include <stdlib.h>   /* getenv for PIPEASIO_DEBUG */

/* Include wine/debug.h for helpers like debugstr_guid, then override
 * TRACE/WARN/ERR to raw write(STDERR_FILENO,…): bypasses Wine's WINEDEBUG
 * gating and the stdio buffers discarded on abnormal exit.  Verbose TRACE
 * is opt-in via the PIPEASIO_DEBUG env var; WARN/ERR always emit. */
#include "wine/debug.h"
#undef  TRACE
#undef  WARN
#undef  ERR
static inline int pipeasio_log_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("PIPEASIO_DEBUG") ? 1 : 0;
    return on;
}
#define PIPEASIO_LOG(pfx, fmt, ...) do {                                       \
    char _buf[1024];                                                           \
    int  _n = snprintf(_buf, sizeof _buf, pfx fmt, ##__VA_ARGS__);             \
    if (_n > 0)                                                                \
        (void)write(STDERR_FILENO, _buf,                                       \
                    (size_t)_n < sizeof _buf ? (size_t)_n : sizeof _buf - 1);  \
} while (0)
#define TRACE(fmt, ...) do { if (pipeasio_log_on()) PIPEASIO_LOG("[pipeasio] ", fmt, ##__VA_ARGS__); } while (0)
#define WARN(fmt, ...)  PIPEASIO_LOG("[pipeasio] WARN: ", fmt, ##__VA_ARGS__)
#define ERR(fmt, ...)   PIPEASIO_LOG("[pipeasio] ERR: ",  fmt, ##__VA_ARGS__)

#include <objbase.h>
#include <mmsystem.h>
#include <winreg.h>
#ifdef WINE_WITH_UNICODE
#include <wine/unicode.h>
#endif

#include "audio.h"
#include "pipeasio_offsets.h"

#ifdef DEBUG
WINE_DEFAULT_DEBUG_CHANNEL(asio);
#endif

#define MAX_ENVIRONMENT_SIZE            6
#define PIPEASIO_MAX_NAME_LENGTH        32
#define PIPEASIO_MINIMUM_BUFFERSIZE     16
#define PIPEASIO_MAXIMUM_BUFFERSIZE     8192
#define PIPEASIO_PREFERRED_BUFFERSIZE   1024

/* ASIO drivers (breaking the COM specification) use the Microsoft variety of
 * thiscall calling convention which gcc is unable to produce.  These macros
 * add an extra layer to fixup the registers. Borrowed from config.h and the
 * wine source code.
 */

/* From config.h */
#define __ASM_DEFINE_FUNC(name,suffix,code) asm(".text\n\t.align 4\n\t.globl " #name suffix "\n\t.type " #name suffix ",@function\n" #name suffix ":\n\t.cfi_startproc\n\t" code "\n\t.cfi_endproc\n\t.previous");
#define __ASM_GLOBAL_FUNC(name,code) __ASM_DEFINE_FUNC(name,"",code)
#define __ASM_NAME(name) name
#define __ASM_STDCALL(args) ""

/* From wine source */
#ifdef __i386__  /* thiscall functions are i386-specific */

#define THISCALL(func) __thiscall_ ## func
#define THISCALL_NAME(func) __ASM_NAME("__thiscall_" #func)
#define __thiscall __stdcall
#define DEFINE_THISCALL_WRAPPER(func,args) \
    extern void THISCALL(func)(void); \
    __ASM_GLOBAL_FUNC(__thiscall_ ## func, \
                      "popl %eax\n\t" \
                      "pushl %ecx\n\t" \
                      "pushl %eax\n\t" \
                      "jmp " __ASM_NAME(#func) __ASM_STDCALL(args) )
#else /* __i386__ */

#define THISCALL(func) func
#define THISCALL_NAME(func) __ASM_NAME(#func)
#define __thiscall __stdcall
#define DEFINE_THISCALL_WRAPPER(func,args) /* nothing */

#endif /* __i386__ */

/* Hide ELF symbols for the COM members - No need to to export them */
#define HIDDEN __attribute__ ((visibility("hidden")))

#ifdef _WIN64
#define PIPEASIO_CALLBACK CALLBACK
#else
#define PIPEASIO_CALLBACK
#endif

typedef struct w_int64_t {
    ULONG hi;
    ULONG lo;
} w_int64_t;

typedef struct BufferInformation
{
    LONG isInputType;
    LONG channelNumber;
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
    void (PIPEASIO_CALLBACK *swapBuffers) (LONG, LONG);
    void (PIPEASIO_CALLBACK *sampleRateChanged) (double);
    LONG (PIPEASIO_CALLBACK *sendNotification) (LONG, LONG, void*, double*);
    void* (PIPEASIO_CALLBACK *swapBuffersWithTimeInfo) (TimeInformation*, LONG, LONG);
} Callbacks;

/*****************************************************************************
 * IWineAsio interface
 */

#define INTERFACE IPipeASIO
DECLARE_INTERFACE_(IPipeASIO,IUnknown)
{
    STDMETHOD_(HRESULT, QueryInterface)         (THIS_ IID riid, void** ppvObject) PURE;
    STDMETHOD_(ULONG, AddRef)                   (THIS) PURE;
    STDMETHOD_(ULONG, Release)                  (THIS) PURE;
    STDMETHOD_(LONG, Init)                      (THIS_ void *sysRef) PURE;
    STDMETHOD_(void, GetDriverName)             (THIS_ char *name) PURE;
    STDMETHOD_(LONG, GetDriverVersion)          (THIS) PURE;
    STDMETHOD_(void, GetErrorMessage)           (THIS_ char *string) PURE;
    STDMETHOD_(LONG, Start)                     (THIS) PURE;
    STDMETHOD_(LONG, Stop)                      (THIS) PURE;
    STDMETHOD_(LONG, GetChannels)               (THIS_ LONG *numInputChannels, LONG *numOutputChannels) PURE;
    STDMETHOD_(LONG, GetLatencies)              (THIS_ LONG *inputLatency, LONG *outputLatency) PURE;
    STDMETHOD_(LONG, GetBufferSize)             (THIS_ LONG *minSize, LONG *maxSize, LONG *preferredSize, LONG *granularity) PURE;
    STDMETHOD_(LONG, CanSampleRate)             (THIS_ double sampleRate) PURE;
    STDMETHOD_(LONG, GetSampleRate)             (THIS_ double *sampleRate) PURE;
    STDMETHOD_(LONG, SetSampleRate)             (THIS_ double sampleRate) PURE;
    STDMETHOD_(LONG, GetClockSources)           (THIS_ void *clocks, LONG *numSources) PURE;
    STDMETHOD_(LONG, SetClockSource)            (THIS_ LONG index) PURE;
    STDMETHOD_(LONG, GetSamplePosition)         (THIS_ w_int64_t *sPos, w_int64_t *tStamp) PURE;
    STDMETHOD_(LONG, GetChannelInfo)            (THIS_ void *info) PURE;
    STDMETHOD_(LONG, CreateBuffers)             (THIS_ BufferInformation *bufferInfo, LONG numChannels, LONG bufferSize, Callbacks *callbacks) PURE;
    STDMETHOD_(LONG, DisposeBuffers)            (THIS) PURE;
    STDMETHOD_(LONG, ControlPanel)              (THIS) PURE;
    STDMETHOD_(LONG, Future)                    (THIS_ LONG selector,void *opt) PURE;
    STDMETHOD_(LONG, OutputReady)               (THIS) PURE;
};
#undef INTERFACE

typedef struct IPipeASIO *LPPIPEASIO;

typedef struct IOChannel
{
    audio_sample_t *audio_buffer;
    char                        port_name[PIPEASIO_MAX_NAME_LENGTH];
    audio_port_t                 *port;
    bool                        active;
} IOChannel;

typedef struct IPipeASIOImpl
{
    /* COM stuff */
    const IPipeASIOVtbl         *lpVtbl;
    LONG                        ref;

    /* The app's main window handle on windows, 0 on OS/X */
    HWND                        sys_ref;

    /* Host stuff */
    LONG                        host_active_inputs;
    LONG                        host_active_outputs;
    BOOL                        host_buffer_index;
    Callbacks                  *host_callbacks;
    BOOL                        host_can_time_code;
    LONG                        host_current_buffersize;
    INT                         host_driver_state;
    w_int64_t                   host_num_samples;
    double                      host_sample_rate;
    TimeInformation             host_time;
    BOOL                        host_time_info_mode;
    w_int64_t                   host_time_stamp;
    LONG                        host_version;

    /* PipeASIO configuration options */
    int                         pipeasio_number_inputs;
    int                         pipeasio_number_outputs;
    BOOL                        pipeasio_autostart_server;
    BOOL                        pipeasio_connect_to_hardware;
    BOOL                        pipeasio_fixed_buffersize;
    LONG                        pipeasio_preferred_buffersize;

    /* PipeWire client + discovered device ports */
    audio_client_t               *audio_client;
    char                        client_name[PIPEASIO_MAX_NAME_LENGTH];
    int                         num_phys_input_ports;
    int                         num_phys_output_ports;
    const char                  **phys_input_ports;
    const char                  **phys_output_ports;

    /* host process-callback buffers */
    audio_sample_t *callback_audio_buffer;
    IOChannel                   *input_channel;
    IOChannel                   *output_channel;
} IPipeASIOImpl;

enum { Loaded, Initialized, Prepared, Running };

/****************************************************************************
 *  Interface Methods
 */

/*
 *  as seen from the PipeASIO source
 */

HIDDEN HRESULT STDMETHODCALLTYPE      QueryInterface(LPPIPEASIO iface, REFIID riid, void **ppvObject);
HIDDEN ULONG   STDMETHODCALLTYPE      AddRef(LPPIPEASIO iface);
HIDDEN ULONG   STDMETHODCALLTYPE      Release(LPPIPEASIO iface);
HIDDEN LONG    STDMETHODCALLTYPE      Init(LPPIPEASIO iface, void *sysRef);
HIDDEN void    STDMETHODCALLTYPE      GetDriverName(LPPIPEASIO iface, char *name);
HIDDEN LONG    STDMETHODCALLTYPE      GetDriverVersion(LPPIPEASIO iface);
HIDDEN void    STDMETHODCALLTYPE      GetErrorMessage(LPPIPEASIO iface, char *string);
HIDDEN LONG    STDMETHODCALLTYPE      Start(LPPIPEASIO iface);
HIDDEN LONG    STDMETHODCALLTYPE      Stop(LPPIPEASIO iface);
HIDDEN LONG    STDMETHODCALLTYPE      GetChannels (LPPIPEASIO iface, LONG *numInputChannels, LONG *numOutputChannels);
HIDDEN LONG    STDMETHODCALLTYPE      GetLatencies(LPPIPEASIO iface, LONG *inputLatency, LONG *outputLatency);
HIDDEN LONG    STDMETHODCALLTYPE      GetBufferSize(LPPIPEASIO iface, LONG *minSize, LONG *maxSize, LONG *preferredSize, LONG *granularity);
HIDDEN LONG    STDMETHODCALLTYPE      CanSampleRate(LPPIPEASIO iface, double sampleRate);
HIDDEN LONG    STDMETHODCALLTYPE      GetSampleRate(LPPIPEASIO iface, double *sampleRate);
HIDDEN LONG    STDMETHODCALLTYPE      SetSampleRate(LPPIPEASIO iface, double sampleRate);
HIDDEN LONG    STDMETHODCALLTYPE      GetClockSources(LPPIPEASIO iface, void *clocks, LONG *numSources);
HIDDEN LONG    STDMETHODCALLTYPE      SetClockSource(LPPIPEASIO iface, LONG index);
HIDDEN LONG    STDMETHODCALLTYPE      GetSamplePosition(LPPIPEASIO iface, w_int64_t *sPos, w_int64_t *tStamp);
HIDDEN LONG    STDMETHODCALLTYPE      GetChannelInfo(LPPIPEASIO iface, void *info);
HIDDEN LONG    STDMETHODCALLTYPE      CreateBuffers(LPPIPEASIO iface, BufferInformation *bufferInfo, LONG numChannels, LONG bufferSize, Callbacks *callbacks);
HIDDEN LONG    STDMETHODCALLTYPE      DisposeBuffers(LPPIPEASIO iface);
HIDDEN LONG    STDMETHODCALLTYPE      ControlPanel(LPPIPEASIO iface);
HIDDEN LONG    STDMETHODCALLTYPE      Future(LPPIPEASIO iface, LONG selector, void *opt);
HIDDEN LONG    STDMETHODCALLTYPE      OutputReady(LPPIPEASIO iface);

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

static inline int  buffer_size_callback (audio_nframes_t nframes, void *arg);
static inline void latency_callback(audio_latency_mode_t mode, void *arg);
static inline int  process_callback (audio_nframes_t nframes, void *arg);
static inline int  sample_rate_callback (audio_nframes_t nframes, void *arg);

/*
 *  Support functions
 */

HRESULT WINAPI  PipeASIOCreateInstance(REFIID riid, LPVOID *ppobj);
static  VOID    configure_driver(IPipeASIOImpl *This);

/* {2d3ca9e2-1193-4c5d-b5fd-38798f3dc074} */
static GUID const CLSID_PipeASIO = {
0x2d3ca9e2, 0x1193, 0x4c5d, { 0xb5, 0xfd, 0x38, 0x79, 0x8f, 0x3d, 0xc0, 0x74 } };

static const IPipeASIOVtbl PipeASIO_Vtbl =
{
    (void *) QueryInterface,
    (void *) AddRef,
    (void *) Release,

    (void *) THISCALL(Init),
    (void *) THISCALL(GetDriverName),
    (void *) THISCALL(GetDriverVersion),
    (void *) THISCALL(GetErrorMessage),
    (void *) THISCALL(Start),
    (void *) THISCALL(Stop),
    (void *) THISCALL(GetChannels),
    (void *) THISCALL(GetLatencies),
    (void *) THISCALL(GetBufferSize),
    (void *) THISCALL(CanSampleRate),
    (void *) THISCALL(GetSampleRate),
    (void *) THISCALL(SetSampleRate),
    (void *) THISCALL(GetClockSources),
    (void *) THISCALL(SetClockSource),
    (void *) THISCALL(GetSamplePosition),
    (void *) THISCALL(GetChannelInfo),
    (void *) THISCALL(CreateBuffers),
    (void *) THISCALL(DisposeBuffers),
    (void *) THISCALL(ControlPanel),
    (void *) THISCALL(Future),
    (void *) THISCALL(OutputReady)
};

/*****************************************************************************
 * Interface method definitions
 */


HIDDEN HRESULT STDMETHODCALLTYPE QueryInterface(LPPIPEASIO iface, REFIID riid, void **ppvObject)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl *)iface;

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

/*
 * ULONG STDMETHODCALLTYPE AddRef(LPPIPEASIO iface);
 * Function: Increment the reference count on the object
 * Returns:  Ref count
 */

HIDDEN ULONG STDMETHODCALLTYPE AddRef(LPPIPEASIO iface)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl *)iface;
    ULONG           ref = InterlockedIncrement(&(This->ref));

    TRACE("iface: %p, ref count is %u\n", iface, (unsigned)ref);
    return ref;
}

/*
 * ULONG Release (LPPIPEASIO iface);
 *  Function:   Destroy the interface
 *  Returns:    Ref count
 *  Implies:    Stop() and DisposeBuffers()
 */

HIDDEN ULONG STDMETHODCALLTYPE Release(LPPIPEASIO iface)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl *)iface;
    ULONG            ref = InterlockedDecrement(&This->ref);

    TRACE("iface: %p, ref count is %u\n", iface, (unsigned)ref);

    if (This->host_driver_state == Running)
        Stop(iface);
    if (This->host_driver_state == Prepared)
        DisposeBuffers(iface);

    if (This->host_driver_state == Initialized)
    {
        /* just for good measure we deinitialize IOChannel structures and unregister ports */
        for (int i = 0; i < This->pipeasio_number_inputs; i++)
        {
            audio_port_unregister (This->audio_client, This->input_channel[i].port);
            This->input_channel[i].active = false;
            This->input_channel[i].port = NULL;
        }
        for (int i = 0; i < This->pipeasio_number_outputs; i++)
        {
            audio_port_unregister (This->audio_client, This->output_channel[i].port);
            This->output_channel[i].active = false;
            This->output_channel[i].port = NULL;
        }
        This->host_active_inputs = This->host_active_outputs = 0;
        TRACE("%i IOChannel structures released\n", This->pipeasio_number_inputs + This->pipeasio_number_outputs);

        audio_free (This->phys_output_ports);
        audio_free (This->phys_input_ports);
        audio_close(This->audio_client);
        if (This->input_channel)
            HeapFree(GetProcessHeap(), 0, This->input_channel);
    }
    TRACE("PipeASIO terminated\n\n");
    if (ref == 0)
        HeapFree(GetProcessHeap(), 0, This);
    return ref;
}

/*
 * LONG Init (void *sysRef);
 *  Function:   Initialize the driver
 *  Parameters: Pointer to "This"
 *              sysHanle is 0 on OS/X and on windows it contains the applications main window handle
 *  Returns:    0 on error, and 1 on success
 */

DEFINE_THISCALL_WRAPPER(Init,8)
HIDDEN LONG STDMETHODCALLTYPE Init(LPPIPEASIO iface, void *sysRef)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl *)iface;
    uint32_t   audio_status;
    uint32_t  audio_options = This->pipeasio_autostart_server ? AUDIO_NULL_OPTION : AUDIO_NO_START_SERVER;
    int             i;

    This->sys_ref = sysRef;
    /* Do NOT mlockall(MCL_FUTURE) here.  Under Wine it forces every
     * subsequent mmap to lock its pages under RLIMIT_MEMLOCK; win32u
     * maps a fresh "session" shared-memory block per batch of USER
     * objects, so when the host (FL Studio) registers many plugin
     * window classes during project load, that mmap hits the memlock
     * limit and NtUserRegisterClassExWOW fails with "Failed to get
     * shared session object" — recursing until the GUI thread's 1 MB
     * stack overflows and the host crashes.  The lock is process-global
     * and never reversed, so it persists even after the host switches
     * back to another ASIO driver (only a fresh process recovers).
     * pwasio locks nothing; PipeWire's rt module owns RT paging. */
    configure_driver(This);

    if (!(This->audio_client = audio_open(This->client_name, audio_options, &audio_status)))
    {
        WARN("Unable to open an audio client as: %s\n", This->client_name);
        return 0;
    }
    TRACE("audio client opened as: '%s'\n", audio_get_client_name(This->audio_client));

    This->host_sample_rate = audio_get_sample_rate(This->audio_client);
    This->host_current_buffersize = audio_get_buffer_size(This->audio_client);

    /* Allocate IOChannel structures (zeroed — audio_buffer and active
     * must start NULL/false before CreateBuffers wires them up). */
    This->input_channel = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    (This->pipeasio_number_inputs + This->pipeasio_number_outputs) * sizeof(IOChannel));
    if (!This->input_channel)
    {
        audio_close(This->audio_client);
        ERR("Unable to allocate IOChannel structures for %i channels\n", This->pipeasio_number_inputs);
        return 0;
    }
    This->output_channel = This->input_channel + This->pipeasio_number_inputs;
    TRACE("%i IOChannel structures allocated\n", This->pipeasio_number_inputs + This->pipeasio_number_outputs);

    /* Get and count physical device ports */
    This->phys_input_ports = audio_get_ports(This->audio_client, NULL, NULL, AUDIO_PORT_IS_PHYSICAL | AUDIO_PORT_IS_OUTPUT);
    for (This->num_phys_input_ports = 0; This->phys_input_ports && This->phys_input_ports[This->num_phys_input_ports]; This->num_phys_input_ports++)
        ;
    This->phys_output_ports = audio_get_ports(This->audio_client, NULL, NULL, AUDIO_PORT_IS_PHYSICAL | AUDIO_PORT_IS_INPUT);
    for (This->num_phys_output_ports = 0; This->phys_output_ports && This->phys_output_ports[This->num_phys_output_ports]; This->num_phys_output_ports++)
        ;

    /* Initialize IOChannel structures */
    for (i = 0; i < This->pipeasio_number_inputs; i++)
    {
        This->input_channel[i].active = false;
        This->input_channel[i].port = NULL;
        snprintf(This->input_channel[i].port_name, PIPEASIO_MAX_NAME_LENGTH, "in_%i", i + 1);
        This->input_channel[i].port = audio_port_register(This->audio_client,
            This->input_channel[i].port_name, AUDIO_DEFAULT_TYPE, AUDIO_PORT_IS_INPUT, i);
        /* TRACE("IOChannel structure initialized for input %d: '%s'\n", i, This->input_channel[i].port_name); */
    }
    for (i = 0; i < This->pipeasio_number_outputs; i++)
    {
        This->output_channel[i].active = false;
        This->output_channel[i].port = NULL;
        snprintf(This->output_channel[i].port_name, PIPEASIO_MAX_NAME_LENGTH, "out_%i", i + 1);
        This->output_channel[i].port = audio_port_register(This->audio_client,
            This->output_channel[i].port_name, AUDIO_DEFAULT_TYPE, AUDIO_PORT_IS_OUTPUT, i);
        /* TRACE("IOChannel structure initialized for output %d: '%s'\n", i, This->output_channel[i].port_name); */
    }
    TRACE("%i IOChannel structures initialized\n", This->pipeasio_number_inputs + This->pipeasio_number_outputs);

    if (!audio_set_buffer_size_callback(This->audio_client, buffer_size_callback, This))
    {
        audio_close(This->audio_client);
        HeapFree(GetProcessHeap(), 0, This->input_channel);
        ERR("Unable to register buffer size change callback\n");
        return 0;
    }
    
    if (!audio_set_latency_callback(This->audio_client, latency_callback, This))
    {
        audio_close(This->audio_client);
        HeapFree(GetProcessHeap(), 0, This->input_channel);
        ERR("Unable to register latency callback\n");
        return 0;
    }

    if (!audio_set_process_callback(This->audio_client, process_callback, This))
    {
        audio_close(This->audio_client);
        HeapFree(GetProcessHeap(), 0, This->input_channel);
        ERR("Unable to register process callback\n");
        return 0;
    }

    if (!audio_set_sample_rate_callback (This->audio_client, sample_rate_callback, This))
    {
        audio_close(This->audio_client);
        HeapFree(GetProcessHeap(), 0, This->input_channel);
        ERR("Unable to register sample rate change callback\n");
        return 0;
    }

    This->host_driver_state = Initialized;
    TRACE("PipeASIO 0.%.1f initialized\n",(float) This->host_version / 10);
    return 1;
}

/*
 * void GetDriverName(char *name);
 *  Function:    Returns the driver name in name
 */

DEFINE_THISCALL_WRAPPER(GetDriverName,8)
HIDDEN void STDMETHODCALLTYPE GetDriverName(LPPIPEASIO iface, char *name)
{
    TRACE("iface: %p, name: %p\n", iface, name);
    strcpy(name, "PipeASIO");
    return;
}

/*
 * LONG GetDriverVersion (void);
 *  Function:    Returns the driver version number
 */

DEFINE_THISCALL_WRAPPER(GetDriverVersion,4)
HIDDEN LONG STDMETHODCALLTYPE GetDriverVersion(LPPIPEASIO iface)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl*)iface;

    TRACE("iface: %p\n", iface);
    return This->host_version;
}

/*
 * void GetErrorMessage(char *string);
 *  Function:    Returns an error message for the last occured error in string
 */

DEFINE_THISCALL_WRAPPER(GetErrorMessage,8)
HIDDEN void STDMETHODCALLTYPE GetErrorMessage(LPPIPEASIO iface, char *string)
{
    TRACE("iface: %p, string: %p)\n", iface, string);
    strcpy(string, "PipeASIO does not return error messages\n");
    return;
}

/*
 * LONG Start(void);
 *  Function:    Start IO processing and reset the sample counter to zero
 *  Returns:     -1000 if IO is missing
 *               -999 if the audio backend fails to start
 */

DEFINE_THISCALL_WRAPPER(Start,4)
HIDDEN LONG STDMETHODCALLTYPE Start(LPPIPEASIO iface)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl*)iface;
    int             i;
    DWORD           time;

    TRACE("iface: %p\n", iface);

    if (This->host_driver_state != Prepared)
        return -1000;

    /* Zero the audio buffer */
    for (i = 0; i < (This->pipeasio_number_inputs + This->pipeasio_number_outputs) * 2 * This->host_current_buffersize; i++)
        This->callback_audio_buffer[i] = 0;

    /* prime the callback by preprocessing one outbound host bufffer */
    This->host_buffer_index =  0;
    This->host_num_samples.hi = This->host_num_samples.lo = 0;

    /* GetTickCount, not timeGetTime — see process_callback for the
     * Wine-winmm RT-thread smash details. Same DWORD ms return type so
     * drop-in. (This call site is on the main thread, but keep both
     * sites consistent.) */
    time = GetTickCount();
    This->host_time_stamp.lo = time * 1000000;
    This->host_time_stamp.hi = ((unsigned long long) time * 1000000) >> 32;

    if (This->host_time_info_mode) /* use the newer swapBuffersWithTimeInfo method if supported */
    {
        This->host_time._2 = 1.0;  /* ASIOTime.timeInfo.speed — normal-rate playback */
        This->host_time.numSamples.lo = This->host_time.numSamples.hi = 0;
        This->host_time.timeStamp.lo = This->host_time_stamp.lo;
        This->host_time.timeStamp.hi = This->host_time_stamp.hi;
        This->host_time.sampleRate = This->host_sample_rate;
        This->host_time.flags = 0x7;

        if (This->host_can_time_code) /* addionally use time code if supported */
        {
            This->host_time.speedForTimeCode = 1; /* FIXME */
            This->host_time.timeStampForTimeCode.lo = This->host_time_stamp.lo;
            This->host_time.timeStampForTimeCode.hi = This->host_time_stamp.hi;
            This->host_time.flagsForTimeCode = ~(0x3);
        }
        This->host_callbacks->swapBuffersWithTimeInfo(&This->host_time, This->host_buffer_index, 1);
    }
    else
    { /* use the old swapBuffers method */
        This->host_callbacks->swapBuffers(This->host_buffer_index, 1);
    }

    /* switch host buffer */
    This->host_buffer_index = This->host_buffer_index ? 0 : 1;

    This->host_driver_state = Running;
    TRACE("PipeASIO successfully loaded\n");
    return 0;
}

/*
 * LONG Stop(void);
 *  Function:   Stop IO processing
 *  Returns:    -1000 on missing IO
 *  Note:       swapBuffers() must not called after returning
 */

DEFINE_THISCALL_WRAPPER(Stop,4)
HIDDEN LONG STDMETHODCALLTYPE Stop(LPPIPEASIO iface)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl*)iface;

    TRACE("iface: %p\n", iface);

    if (This->host_driver_state != Running)
        return -1000;

    This->host_driver_state = Prepared;

    return 0;
}

/*
 * LONG GetChannels(LONG *numInputChannels, LONG *numOutputChannels);
 *  Function:   Report number of IO channels
 *  Parameters: numInputChannels and numOutputChannels will hold number of channels on returning
 *  Returns:    -1000 if no channels are available, otherwise AES_OK
 */

DEFINE_THISCALL_WRAPPER(GetChannels,12)
HIDDEN LONG STDMETHODCALLTYPE GetChannels (LPPIPEASIO iface, LONG *numInputChannels, LONG *numOutputChannels)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl*)iface;

    if (!numInputChannels || !numOutputChannels)
        return -998;

    *numInputChannels = This->pipeasio_number_inputs;
    *numOutputChannels = This->pipeasio_number_outputs;
    TRACE("iface: %p, inputs: %i, outputs: %i\n", iface, This->pipeasio_number_inputs, This->pipeasio_number_outputs);
    return 0;
}

/*
 * LONG GetLatencies(LONG *inputLatency, LONG *outputLatency);
 *  Function:   Return latency in frames
 *  Returns:    -1000 if no IO is available, otherwise AES_OK
 */

DEFINE_THISCALL_WRAPPER(GetLatencies,12)
HIDDEN LONG STDMETHODCALLTYPE GetLatencies(LPPIPEASIO iface, LONG *inputLatency, LONG *outputLatency)
{
    IPipeASIOImpl           *This = (IPipeASIOImpl*)iface;
    audio_latency_range_t    range;

    if (!inputLatency || !outputLatency)
        return -998;

    if (This->host_driver_state == Loaded)
        return -1000;

    audio_port_get_latency_range(This->input_channel[0].port, AUDIO_CAPTURE_LATENCY, &range);
    *inputLatency = range.max;
    audio_port_get_latency_range(This->output_channel[0].port, AUDIO_PLAYBACK_LATENCY, &range);
    *outputLatency = range.max;
    TRACE("iface: %p, input latency: %d, output latency: %d\n", iface, (int)*inputLatency, (int)*outputLatency);

    return 0;
}

/*
 * LONG GetBufferSize(LONG *minSize, LONG *maxSize, LONG *preferredSize, LONG *granularity);
 *  Function:    Return minimum, maximum, preferred buffer sizes, and granularity
 *               At the moment return all the same, and granularity 0
 *  Returns:    -1000 on missing IO
 */

DEFINE_THISCALL_WRAPPER(GetBufferSize,20)
HIDDEN LONG STDMETHODCALLTYPE GetBufferSize(LPPIPEASIO iface, LONG *minSize, LONG *maxSize, LONG *preferredSize, LONG *granularity)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl*)iface;

    TRACE("iface: %p, minSize: %p, maxSize: %p, preferredSize: %p, granularity: %p\n", iface, minSize, maxSize, preferredSize, granularity);

    if (!minSize || !maxSize || !preferredSize || !granularity)
        return -998;

    if (This->pipeasio_fixed_buffersize)
    {
        *minSize = *maxSize = *preferredSize = This->host_current_buffersize;
        *granularity = 0;
        TRACE("Buffersize fixed at %d\n", (int)This->host_current_buffersize);
        return 0;
    }

    *minSize = PIPEASIO_MINIMUM_BUFFERSIZE;
    *maxSize = PIPEASIO_MAXIMUM_BUFFERSIZE;
    *preferredSize = This->pipeasio_preferred_buffersize;
    *granularity = -1;
    TRACE("The host can control buffersize\nMinimum: %d, maximum: %d, preferred: %d, granularity: %d, current: %d\n",
          (int)*minSize, (int)*maxSize, (int)*preferredSize, (int)*granularity, (int)This->host_current_buffersize);
    return 0;
}

/*
 * LONG CanSampleRate(double sampleRate);
 *  Function:   Ask if specific SR is available
 *  Returns:    -995 if SR isn't available, -1000 on missing IO
 */

DEFINE_THISCALL_WRAPPER(CanSampleRate,12)
HIDDEN LONG STDMETHODCALLTYPE CanSampleRate(LPPIPEASIO iface, double sampleRate)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl*)iface;

    TRACE("iface: %p, Samplerate = %li, requested samplerate = %li\n", iface, (long) This->host_sample_rate, (long) sampleRate);

    if (sampleRate != This->host_sample_rate)
        return -995;
    return 0;
}

/*
 * LONG GetSampleRate(double *currentRate);
 *  Function:   Return current SR
 *  Parameters: currentRate will hold SR on return, 0 if unknown
 *  Returns:    -995 if SR is unknown, -1000 on missing IO
 */

DEFINE_THISCALL_WRAPPER(GetSampleRate,8)
HIDDEN LONG STDMETHODCALLTYPE GetSampleRate(LPPIPEASIO iface, double *sampleRate)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl*)iface;

    TRACE("iface: %p, Sample rate is %i\n", iface, (int) This->host_sample_rate);

    if (!sampleRate)
        return -998;

    *sampleRate = This->host_sample_rate;
    return 0;
}

/*
 * LONG SetSampleRate(double sampleRate);
 *  Function:   Set requested SR, enable external sync if SR == 0
 *  Returns:    -995 if unknown SR
 *              -997 if current clock is external and SR != 0
 *              -1000 on missing IO
 */

DEFINE_THISCALL_WRAPPER(SetSampleRate,12)
HIDDEN LONG STDMETHODCALLTYPE SetSampleRate(LPPIPEASIO iface, double sampleRate)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl*)iface;

    TRACE("iface: %p, Sample rate %f requested\n", iface, sampleRate);

    if (sampleRate != This->host_sample_rate)
        return -995;
    return 0;
}

/*
 * LONG GetClockSources(void *clocks, LONG *numSources);
 *  Function:   Return available clock sources
 *  Parameters: clocks - a pointer to an array of clock source structures.
 *              numSources - when called: number of allocated members
 *                         - on return: number of clock sources, the minimum is 1 - the internal clock
 *  Returns:    -1000 on missing IO
 */

DEFINE_THISCALL_WRAPPER(GetClockSources,12)
HIDDEN LONG STDMETHODCALLTYPE GetClockSources(LPPIPEASIO iface, void *clocks, LONG *numSources)
{
    LONG *lclocks = (LONG*)clocks;

    TRACE("iface: %p, clocks: %p, numSources: %p\n", iface, clocks, numSources);

    if (!clocks || !numSources)
        return -998;

    *lclocks++ = 0;
    *lclocks++ = -1;
    *lclocks++ = -1;
    *lclocks++ = 1;
    strcpy((char*)lclocks, "Internal");
    *numSources = 1;
    return 0;
}

/*
 * LONG SetClockSource(LONG index);
 *  Function:   Set clock source
 *  Parameters: index returned by GetClockSources()
 *  Returns:    -1000 on missing IO
 *              -997 may be returned if a clock can't be selected
 *              -995 should not be returned
 */

DEFINE_THISCALL_WRAPPER(SetClockSource,8)
HIDDEN LONG STDMETHODCALLTYPE SetClockSource(LPPIPEASIO iface, LONG index)
{
    TRACE("iface: %p, index: %d\n", iface, (int)index);

    if (index != 0)
        return -1000;
    return 0;
}

/*
 * LONG GetSamplePosition (w_int64_t *sPos, w_int64_t *tStamp);
 *  Function:   Return sample position and timestamp
 *  Parameters: sPos holds the position on return, reset to 0 on Start()
 *              tStamp holds the system time of sPos
 *  Return:     -1000 on missing IO
 *              -996 on missing clock
 */

DEFINE_THISCALL_WRAPPER(GetSamplePosition,12)
HIDDEN LONG STDMETHODCALLTYPE GetSamplePosition(LPPIPEASIO iface, w_int64_t *sPos, w_int64_t *tStamp)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl*)iface;

    TRACE("iface: %p, sPos: %p, tStamp: %p\n", iface, sPos, tStamp);

    if (!sPos || !tStamp)
        return -998;

    tStamp->lo = This->host_time_stamp.lo;
    tStamp->hi = This->host_time_stamp.hi;
    sPos->lo = This->host_num_samples.lo;
    sPos->hi = 0; /* FIXME */

    return 0;
}

/*
 * LONG GetChannelInfo (void *info);
 *  Function:   Retrive channel info
 *  Returns:    -1000 on missing IO
 */

DEFINE_THISCALL_WRAPPER(GetChannelInfo,8)
HIDDEN LONG STDMETHODCALLTYPE GetChannelInfo(LPPIPEASIO iface, void *info)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl*)iface;
    LONG *linfo = (LONG*)info;

    const LONG channelNumber = *linfo++;
    const LONG isInputType = *linfo++;

    /* TRACE("(iface: %p, info: %p\n", iface, info); */

    if (channelNumber < 0 || (isInputType ? channelNumber >= This->pipeasio_number_inputs : channelNumber >= This->pipeasio_number_outputs))
        return -998;

    *linfo++ = (isInputType ? This->input_channel : This->output_channel)[channelNumber].active;
    *linfo++ = 0;
    *linfo++ = 19;
    memcpy(linfo, (isInputType ? This->input_channel : This->output_channel)[channelNumber].port_name, PIPEASIO_MAX_NAME_LENGTH);

    return 0;
}

/*
 * LONG CreateBuffers(BufferInformation *bufferInfo, LONG numChannels, LONG bufferSize, Callbacks *callbacks);
 *  Function:   Allocate buffers for IO channels
 *  Parameters: bufferInfo   - pointer to an array of BufferInformation structures
 *              numChannels  - the total number of IO channels to be allocated
 *              bufferSize   - one of the buffer sizes retrieved with GetBufferSize()
 *              callbacks    - pointer to a Callbacks structure
 *  Returns:    -994 if impossible to allocate enough memory
 *              -997 on unsupported bufferSize or invalid bufferInfo data
 *              -1000 on missing IO
 */

DEFINE_THISCALL_WRAPPER(CreateBuffers,20)
HIDDEN LONG STDMETHODCALLTYPE CreateBuffers(LPPIPEASIO iface, BufferInformation *bufferInfo, LONG numChannels, LONG bufferSize, Callbacks *callbacks)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl*)iface;
    BufferInformation  *bufferInfoPerChannel = bufferInfo;
    int             i, j, k;

    TRACE("iface: %p, bufferInfo: %p, numChannels: %d, bufferSize: %d, callbacks: %p\n", iface, bufferInfo, (int)numChannels, (int)bufferSize, callbacks);

    if (This->host_driver_state != Initialized)
        return -1000;

    if (!bufferInfo || !callbacks)
        return -997;

    /* Check for invalid channel numbers.  Both the count-per-direction
     * AND each entry's channelNumber must be in range — without the
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
                     (long)bufferInfoPerChannel->channelNumber,
                     This->pipeasio_number_inputs - 1);
                return -997;
            }
        }
        else
        {
            if (k++  >= This->pipeasio_number_outputs)
            {
                WARN("Invalid output channel requested (too many)\n");
                return -997;
            }
            if (bufferInfoPerChannel->channelNumber < 0
                || bufferInfoPerChannel->channelNumber >= This->pipeasio_number_outputs)
            {
                WARN("Invalid output channelNumber %ld (max %d)\n",
                     (long)bufferInfoPerChannel->channelNumber,
                     This->pipeasio_number_outputs - 1);
                return -997;
            }
        }
    }

    /* set buf_size */
    if (This->pipeasio_fixed_buffersize)
    {
        if (This->host_current_buffersize != bufferSize)
            return -997;
        TRACE("Buffersize fixed at %d\n", (int)This->host_current_buffersize);
    }
    else
    { /* fail if not a power of two and if out of range */
        if (!(bufferSize > 0 && !(bufferSize&(bufferSize-1))
                && bufferSize >= PIPEASIO_MINIMUM_BUFFERSIZE
                && bufferSize <= PIPEASIO_MAXIMUM_BUFFERSIZE))
        {
            WARN("Invalid buffersize %d requested\n", (int)bufferSize);
            return -997;
        }
        else
        {
            if (This->host_current_buffersize == bufferSize)
            {
                TRACE("Buffer size already set to %d\n", (int)This->host_current_buffersize);
            }
            else
            {
                This->host_current_buffersize = bufferSize;
                if (!audio_set_buffer_size(This->audio_client, This->host_current_buffersize))
                {
                    WARN("Unable to set buffersize to %d\n", (int)This->host_current_buffersize);
                    return -999;
                }
                TRACE("Buffer size changed to %d\n", (int)This->host_current_buffersize);
            }
        }
    }

    This->host_callbacks = callbacks;
    This->host_time_info_mode = This->host_can_time_code = FALSE;

    if (This->host_callbacks->sendNotification(7, 0, 0, 0))
    {
        This->host_time_info_mode = TRUE;
        if (This->host_callbacks->sendNotification(8, 0, 0, 0))
            This->host_can_time_code = TRUE;
    }

    /* Allocate audio buffers */

    const size_t cb_bytes = pipeasio_host_callback_size_bytes(
        This->pipeasio_number_inputs, This->pipeasio_number_outputs,
        This->host_current_buffersize, sizeof(audio_sample_t));
    This->callback_audio_buffer = HeapAlloc(GetProcessHeap(), 0, cb_bytes);
    if (!This->callback_audio_buffer)
    {
        ERR("Unable to allocate %i audio buffers\n", This->pipeasio_number_inputs + This->pipeasio_number_outputs);
        return -994;
    }
    TRACE("%i audio buffers allocated (%zu kB) base=%p end=%p\n",
          This->pipeasio_number_inputs + This->pipeasio_number_outputs,
          cb_bytes / 1024,
          This->callback_audio_buffer,
          (void *)((char *)This->callback_audio_buffer + cb_bytes));

    for (i = 0; i < This->pipeasio_number_inputs; i++)
        This->input_channel[i].audio_buffer = This->callback_audio_buffer
            + pipeasio_host_input_offset_samples(i, This->host_current_buffersize);
    for (i = 0; i < This->pipeasio_number_outputs; i++)
        This->output_channel[i].audio_buffer = This->callback_audio_buffer
            + pipeasio_host_output_offset_samples(i, This->pipeasio_number_inputs,
                                                  This->host_current_buffersize);

    /* initialize BufferInformation structures */
    bufferInfoPerChannel = bufferInfo;
    This->host_active_inputs = This->host_active_outputs = 0;

    for (i = 0; i < This->pipeasio_number_inputs; i++) {
        This->input_channel[i].active = false;
    }
    for (i = 0; i < This->pipeasio_number_outputs; i++) {
        This->output_channel[i].active = false;
    }

    for (i = 0; i < numChannels; i++, bufferInfoPerChannel++)
    {
        const LONG ch = bufferInfoPerChannel->channelNumber;
        if (bufferInfoPerChannel->isInputType)
        {
            bufferInfoPerChannel->audioBufferStart = &This->input_channel[ch].audio_buffer[0];
            bufferInfoPerChannel->audioBufferEnd = &This->input_channel[ch].audio_buffer[This->host_current_buffersize];
            This->input_channel[ch].active = true;
            This->host_active_inputs++;
            TRACE("  bufferInfo[%d]: IN  ch=%ld start=%p end=%p (cb_offset=%zu)\n",
                  i, (long)ch,
                  bufferInfoPerChannel->audioBufferStart,
                  bufferInfoPerChannel->audioBufferEnd,
                  (size_t)((char *)bufferInfoPerChannel->audioBufferStart
                           - (char *)This->callback_audio_buffer));
        }
        else
        {
            bufferInfoPerChannel->audioBufferStart = &This->output_channel[ch].audio_buffer[0];
            bufferInfoPerChannel->audioBufferEnd = &This->output_channel[ch].audio_buffer[This->host_current_buffersize];
            This->output_channel[ch].active = true;
            This->host_active_outputs++;
            TRACE("  bufferInfo[%d]: OUT ch=%ld start=%p end=%p (cb_offset=%zu)\n",
                  i, (long)ch,
                  bufferInfoPerChannel->audioBufferStart,
                  bufferInfoPerChannel->audioBufferEnd,
                  (size_t)((char *)bufferInfoPerChannel->audioBufferStart
                           - (char *)This->callback_audio_buffer));
        }
    }
    TRACE("%d audio channels initialized (active_in=%d active_out=%d)\n",
          (int)(This->host_active_inputs + This->host_active_outputs),
          This->host_active_inputs, This->host_active_outputs);

    if (!audio_activate(This->audio_client))
        return -1000;

    /* connect to the hardware io */
    if (This->pipeasio_connect_to_hardware)
    {
        for (i = 0; i < This->num_phys_input_ports && i < This->pipeasio_number_inputs; i++)
        {
            const char *type = audio_port_type(audio_port_by_name(This->audio_client, This->phys_input_ports[i]));
            if (type && strstr(type, "audio"))
                audio_connect(This->audio_client, This->phys_input_ports[i], audio_port_name(This->input_channel[i].port));
        }
        for (i = 0; i < This->num_phys_output_ports && i < This->pipeasio_number_outputs; i++)
        {
            const char *type = audio_port_type(audio_port_by_name(This->audio_client, This->phys_output_ports[i]));
            if (type && strstr(type, "audio"))
                audio_connect(This->audio_client, audio_port_name(This->output_channel[i].port), This->phys_output_ports[i]);
        }
    }

    /* at this point all the connections are made and the process callback is outputting silence */
    This->host_driver_state = Prepared;
    return 0;
}

/*
 * LONG DisposeBuffers(void);
 *  Function:   Release allocated buffers
 *  Returns:    -997 if no buffers were previously allocated
 *              -1000 on missing IO
 *  Implies:    Stop()
 */

DEFINE_THISCALL_WRAPPER(DisposeBuffers,4)
HIDDEN LONG STDMETHODCALLTYPE DisposeBuffers(LPPIPEASIO iface)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl*)iface;
    int             i;

    TRACE("iface: %p\n", iface);

    if (This->host_driver_state == Running)
        Stop (iface);
    if (This->host_driver_state != Prepared)
        return -1000;

    if (!audio_deactivate(This->audio_client))
        return -1000;

    This->host_callbacks = NULL;

    for (i = 0; i < This->pipeasio_number_inputs; i++)
    {
        This->input_channel[i].audio_buffer = NULL;
        This->input_channel[i].active = false;
    }
    for (i = 0; i < This->pipeasio_number_outputs; i++)
    {
        This->output_channel[i].audio_buffer = NULL;
        This->output_channel[i].active = false;
    }
    This->host_active_inputs = This->host_active_outputs = 0;

    if (This->callback_audio_buffer)
        HeapFree(GetProcessHeap(), 0, This->callback_audio_buffer);

    This->host_driver_state = Initialized;
    return 0;
}

/*
 * LONG ControlPanel(void);
 *  Function:   Open a control panel for driver settings
 *  Returns:    -1000 if no control panel exists.  Actually return code should be ignored
 *  Note:       Call sendNotification if something has changed
 */

DEFINE_THISCALL_WRAPPER(ControlPanel,4)
HIDDEN LONG STDMETHODCALLTYPE ControlPanel(LPPIPEASIO iface)
{
    static char arg0[] = "pipeasio-settings\0";
    static char *arg_list[] = { arg0, NULL };

    TRACE("iface: %p\n", iface);

    if (vfork() == 0)
    {
        execvp (arg0, arg_list);
        _exit(1);
    }
    return 0;
}

/*
 * LONG Future(LONG selector, void *opt);
 *  Function:   Various
 *  Returns:    Depends on the selector but in general -998 on invalid selector
 *              -998 if function is unsupported to disable further calls
 *              0x3f4847a0 on success, do not use 0
 */

DEFINE_THISCALL_WRAPPER(Future,12)
HIDDEN LONG STDMETHODCALLTYPE Future(LPPIPEASIO iface, LONG selector, void *opt)
{
    IPipeASIOImpl           *This = (IPipeASIOImpl *) iface;

    TRACE("iface: %p, selector: %d, opt: %p\n", iface, (int)selector, opt);

    switch (selector)
    {
        case 1:
            This->host_can_time_code = TRUE;
            TRACE("The host enabled TimeCode\n");
            return 0x3f4847a0;
        case 2:
            This->host_can_time_code = FALSE;
            TRACE("The host disabled TimeCode\n");
            return 0x3f4847a0;
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
            TRACE("The driver supports TimeCode\n");
            return 0x3f4847a0;
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

/*
 * LONG OutputReady(void);
 *  Function:   Tells the driver that output bufffers are ready
 *  Returns:    0 if supported
 *              -1000 to disable
 */

DEFINE_THISCALL_WRAPPER(OutputReady,4)
HIDDEN LONG STDMETHODCALLTYPE OutputReady(LPPIPEASIO iface)
{
    /* disabled to stop stand alone NI programs from spamming the console
    TRACE("iface: %p\n", iface); */
    return -1000;
}

/****************************************************************************
 *  ASIO process callbacks
 */

static inline int buffer_size_callback(audio_nframes_t nframes, void *arg)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl*)arg;

    if(This->host_driver_state != Running)
        return 0;

    if (This->host_callbacks->sendNotification(1, 3, 0, 0))
        This->host_callbacks->sendNotification(3, 0, 0, 0);
    return 0;
}

static inline void latency_callback(audio_latency_mode_t mode, void *arg)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl*)arg;

    if(This->host_driver_state != Running)
        return;

    if (This->host_callbacks->sendNotification(1, 6, 0, 0))
        This->host_callbacks->sendNotification(6, 0, 0, 0);

    return;
}

static inline int process_callback(audio_nframes_t nframes, void *arg)
{
    IPipeASIOImpl               *This = (IPipeASIOImpl*)arg;

    int                         i;
    audio_transport_state_t      transport_state;
    audio_position_t             transport_position;
    DWORD                       time;

    /* output silence if the host callback isn't running yet */
    if (This->host_driver_state != Running)
    {
        for (i = 0; i < This->host_active_outputs; i++)
        {
            audio_sample_t *dst =
                audio_port_get_buffer(This->output_channel[i].port, nframes);
            if (dst)
                memset(dst, 0, sizeof (audio_sample_t) * nframes);
        }
        return 0;
    }

    /* One-shot dump of the actual memcpy bounds we're about to use,
     * so we can audit FL Studio's idea of the buffer layout vs. ours. */
    static int diag_first_cycle_logged;
    if (!diag_first_cycle_logged)
    {
        diag_first_cycle_logged = 1;
        TRACE("first process_callback: nframes=%u host_buffer_index=%d\n",
              (unsigned)nframes, (int)This->host_buffer_index);
        for (i = 0; i < This->pipeasio_number_inputs; i++)
            if (This->input_channel[i].active)
                TRACE("  in[%d] dst=%p .. %p (%zu bytes), audio_buffer base=%p\n",
                      i,
                      (void *)&This->input_channel[i].audio_buffer[nframes * This->host_buffer_index],
                      (void *)(&This->input_channel[i].audio_buffer[nframes * This->host_buffer_index] + nframes),
                      (size_t)nframes * sizeof(audio_sample_t),
                      (void *)This->input_channel[i].audio_buffer);
        for (i = 0; i < This->pipeasio_number_outputs; i++)
            if (This->output_channel[i].active)
                TRACE("  out[%d] src=%p .. %p (%zu bytes), audio_buffer base=%p\n",
                      i,
                      (void *)&This->output_channel[i].audio_buffer[nframes * This->host_buffer_index],
                      (void *)(&This->output_channel[i].audio_buffer[nframes * This->host_buffer_index] + nframes),
                      (size_t)nframes * sizeof(audio_sample_t),
                      (void *)This->output_channel[i].audio_buffer);
    }

    /* copy device input to host buffers */
    for (i = 0; i < This->pipeasio_number_inputs; i++)
        if (This->input_channel[i].active)
        {
            audio_sample_t *src =
                audio_port_get_buffer(This->input_channel[i].port, nframes);
            audio_sample_t *dst =
                &This->input_channel[i].audio_buffer[nframes * This->host_buffer_index];
            if (src)
                memcpy(dst, src, sizeof (audio_sample_t) * nframes);
            else
                memset(dst, 0, sizeof (audio_sample_t) * nframes);
        }

    if (This->host_num_samples.lo > ULONG_MAX - nframes)
        This->host_num_samples.hi++;
    This->host_num_samples.lo += nframes;

    /* GetTickCount instead of timeGetTime: WINMM's timeGetTime, when
     * called from the PipeWire RT thread (CreateThread'd via our
     * spa_thread_utils override), corrupts the calling process's
     * Wine PE main thread stack — overwriting a saved RIP on main's
     * frame with a small constant.  The corruption is detected ~40
     * audio cycles later when main's Sleep() polls clock_gettime
     * and its FORTIFY canary check trips.
     *
     * GetTickCount (kernel32) doesn't have this issue.  Both return
     * DWORD milliseconds since system start, so it's a drop-in.
     * Bisected via Step 1-2 of the smash-hunt plan; ruled in by
     * gating timeGetTime alone, ruled in further by swapping in
     * GetTickCount (no smash). */
    time = GetTickCount();
    This->host_time_stamp.lo = time * 1000000;
    This->host_time_stamp.hi = ((unsigned long long) time * 1000000) >> 32;

    if (This->host_time_info_mode) /* use the newer swapBuffersWithTimeInfo method if supported */
    {
        This->host_time._2 = 1.0;  /* ASIOTime.timeInfo.speed — normal-rate playback */
        This->host_time.numSamples.lo = This->host_num_samples.lo;
        This->host_time.numSamples.hi = This->host_num_samples.hi;
        This->host_time.timeStamp.lo = This->host_time_stamp.lo;
        This->host_time.timeStamp.hi = This->host_time_stamp.hi;
        This->host_time.sampleRate = This->host_sample_rate;
        This->host_time.flags = 0x7;

        if (This->host_can_time_code) /* FIXME addionally use time code if supported */
        {
            transport_state = audio_transport_query(This->audio_client, &transport_position);
            This->host_time.flagsForTimeCode = 0x1;
            if (transport_state == AUDIO_TRANSPORT_ROLLING)
                This->host_time.flagsForTimeCode |= 0x2;
        }
        This->host_callbacks->swapBuffersWithTimeInfo(&This->host_time, This->host_buffer_index, 1);
    }
    else
    { /* use the old swapBuffers method */
        This->host_callbacks->swapBuffers(This->host_buffer_index, 1);
    }

    /* copy host to device output buffers */
    for (i = 0; i < This->pipeasio_number_outputs; i++)
        if (This->output_channel[i].active)
        {
            audio_sample_t *dst =
                audio_port_get_buffer(This->output_channel[i].port, nframes);
            if (dst)
                memcpy(dst,
                       &This->output_channel[i].audio_buffer[nframes * This->host_buffer_index],
                       sizeof (audio_sample_t) * nframes);
        }

    /* switch host buffer */
    This->host_buffer_index = This->host_buffer_index ? 0 : 1;
    return 0;
}

static inline int sample_rate_callback(audio_nframes_t nframes, void *arg)
{
    IPipeASIOImpl   *This = (IPipeASIOImpl*)arg;

    if(This->host_driver_state != Running)
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
static WCHAR *strrchrW(const WCHAR* str, WCHAR ch)
{
    WCHAR *ret = NULL;
    do { if (*str == ch) ret = (WCHAR *)(ULONG_PTR)str; } while (*str++);
    return ret;
}
#endif

static VOID configure_driver(IPipeASIOImpl *This)
{
    HKEY    hkey;
    LONG    result, value;
    DWORD   type, size;
    WCHAR   application_path [MAX_PATH];
    WCHAR   *application_name;
    char    environment_variable[MAX_ENVIRONMENT_SIZE];

    /* Unicode strings used for the registry */
    static const WCHAR key_software_wine_pipeasio[] =
        { 'S','o','f','t','w','a','r','e','\\',
          'W','i','n','e','\\',
          'P','i','p','e','A','S','I','O',0 };
    static const WCHAR value_pipeasio_number_inputs[] =
        { 'N','u','m','b','e','r',' ','o','f',' ','i','n','p','u','t','s',0 };
    static const WCHAR value_pipeasio_number_outputs[] =
        { 'N','u','m','b','e','r',' ','o','f',' ','o','u','t','p','u','t','s',0 };
    static const WCHAR value_pipeasio_fixed_buffersize[] =
        { 'F','i','x','e','d',' ','b','u','f','f','e','r','s','i','z','e',0 };
    static const WCHAR value_pipeasio_preferred_buffersize[] =
        { 'P','r','e','f','e','r','r','e','d',' ','b','u','f','f','e','r','s','i','z','e',0 };
    static const WCHAR pipeasio_autostart_server[] =
        { 'A','u','t','o','s','t','a','r','t',' ','s','e','r','v','e','r',0 };
    static const WCHAR value_pipeasio_connect_to_hardware[] =
        { 'C','o','n','n','e','c','t',' ','t','o',' ','h','a','r','d','w','a','r','e',0 };

    /* Initialise most member variables,
     * host_num_samples, host_time, & host_time_stamp are initialized in Start()
     * num_phys_input_ports & num_phys_output_ports are initialized in Init() */
    This->host_active_inputs = 0;
    This->host_active_outputs = 0;
    This->host_buffer_index = 0;
    This->host_callbacks = NULL;
    This->host_can_time_code = FALSE;
    This->host_current_buffersize = 0;
    This->host_driver_state = Loaded;
    This->host_sample_rate = 0;
    This->host_time_info_mode = FALSE;
    This->host_version = 92;

    This->pipeasio_number_inputs = 16;
    This->pipeasio_number_outputs = 16;
    This->pipeasio_autostart_server = FALSE;
    This->pipeasio_connect_to_hardware = TRUE;
    This->pipeasio_fixed_buffersize = TRUE;
    This->pipeasio_preferred_buffersize = PIPEASIO_PREFERRED_BUFFERSIZE;

    This->audio_client = NULL;
    This->client_name[0] = 0;
    This->phys_input_ports = NULL;
    This->phys_output_ports = NULL;
    This->callback_audio_buffer = NULL;
    This->input_channel = NULL;
    This->output_channel = NULL;

    /* create registry entries with defaults if not present */
    result = RegCreateKeyExW(HKEY_CURRENT_USER, key_software_wine_pipeasio, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hkey, NULL);

    /* get/set number of pipeasio inputs */
    size = sizeof(DWORD);
    if (RegQueryValueExW(hkey, value_pipeasio_number_inputs, NULL, &type, (LPBYTE) &value, &size) == ERROR_SUCCESS)
    {
        if (type == REG_DWORD)
            This->pipeasio_number_inputs = value;
    }
    else
    {
        type = REG_DWORD;
        size = sizeof(DWORD);
        value = This->pipeasio_number_inputs;
        result = RegSetValueExW(hkey, value_pipeasio_number_inputs, 0, REG_DWORD, (LPBYTE) &value, size);
    }

    /* get/set number of pipeasio outputs */
    size = sizeof(DWORD);
    if (RegQueryValueExW(hkey, value_pipeasio_number_outputs, NULL, &type, (LPBYTE) &value, &size) == ERROR_SUCCESS)
    {
        if (type == REG_DWORD)
            This->pipeasio_number_outputs = value;
    }
    else
    {
        type = REG_DWORD;
        size = sizeof(DWORD);
        value = This->pipeasio_number_outputs;
        result = RegSetValueExW(hkey, value_pipeasio_number_outputs, 0, REG_DWORD, (LPBYTE) &value, size);
    }

    /* allow changing of pipeasio buffer sizes */
    size = sizeof(DWORD);
    if (RegQueryValueExW(hkey, value_pipeasio_fixed_buffersize, NULL, &type, (LPBYTE) &value, &size) == ERROR_SUCCESS)
    {
        if (type == REG_DWORD)
            This->pipeasio_fixed_buffersize = value;
    }
    else
    {
        type = REG_DWORD;
        size = sizeof(DWORD);
        value = This->pipeasio_fixed_buffersize;
        result = RegSetValueExW(hkey, value_pipeasio_fixed_buffersize, 0, REG_DWORD, (LPBYTE) &value, size);
    }

    /* preferred buffer size (if changing buffersize is allowed) */
    size = sizeof(DWORD);
    if (RegQueryValueExW(hkey, value_pipeasio_preferred_buffersize, NULL, &type, (LPBYTE) &value, &size) == ERROR_SUCCESS)
    {
        if (type == REG_DWORD)
            This->pipeasio_preferred_buffersize = value;
    }
    else
    {
        type = REG_DWORD;
        size = sizeof(DWORD);
        value = This->pipeasio_preferred_buffersize;
        result = RegSetValueExW(hkey, value_pipeasio_preferred_buffersize, 0, REG_DWORD, (LPBYTE) &value, size);
    }

    /* get/set autostart */
    size = sizeof(DWORD);
    if (RegQueryValueExW(hkey, pipeasio_autostart_server, NULL, &type, (LPBYTE) &value, &size) == ERROR_SUCCESS)
    {
        if (type == REG_DWORD)
            This->pipeasio_autostart_server = value;
    }
    else
    {
        type = REG_DWORD;
        size = sizeof(DWORD);
        value = This->pipeasio_autostart_server;
        result = RegSetValueExW(hkey, pipeasio_autostart_server, 0, REG_DWORD, (LPBYTE) &value, size);
    }

    /* get/set connect to physical io */
    size = sizeof(DWORD);
    if (RegQueryValueExW(hkey, value_pipeasio_connect_to_hardware, NULL, &type, (LPBYTE) &value, &size) == ERROR_SUCCESS)
    {
        if (type == REG_DWORD)
            This->pipeasio_connect_to_hardware = value;
    }
    else
    {
        type = REG_DWORD;
        size = sizeof(DWORD);
        value = This->pipeasio_connect_to_hardware;
        result = RegSetValueExW(hkey, value_pipeasio_connect_to_hardware, 0, REG_DWORD, (LPBYTE) &value, size);
    }

    /* get client name by stripping path and extension */
    GetModuleFileNameW(0, application_path, MAX_PATH);
    application_name = strrchrW(application_path, L'.');
    *application_name = 0;
    application_name = strrchrW(application_path, L'\\');
    application_name++;
    WideCharToMultiByte(CP_ACP, WC_SEPCHARS, application_name, -1, This->client_name, PIPEASIO_MAX_NAME_LENGTH, NULL, NULL);

    RegCloseKey(hkey);

    /* Look for environment variables to override registry config values */

    if (GetEnvironmentVariableA("PIPEASIO_NUMBER_INPUTS", environment_variable, MAX_ENVIRONMENT_SIZE))
    {
        errno = 0;
        result = strtol(environment_variable, 0, 10);
        if (errno != ERANGE)
            This->pipeasio_number_inputs = result;
    }

    if (GetEnvironmentVariableA("PIPEASIO_NUMBER_OUTPUTS", environment_variable, MAX_ENVIRONMENT_SIZE))
    {
        errno = 0;
        result = strtol(environment_variable, 0, 10);
        if (errno != ERANGE)
            This->pipeasio_number_outputs = result;
    }

    if (GetEnvironmentVariableA("PIPEASIO_AUTOSTART_SERVER", environment_variable, MAX_ENVIRONMENT_SIZE))
    {
        if (!strcasecmp(environment_variable, "on"))
            This->pipeasio_autostart_server = TRUE;
        else if (!strcasecmp(environment_variable, "off"))
            This->pipeasio_autostart_server = FALSE;
    }

    if (GetEnvironmentVariableA("PIPEASIO_CONNECT_TO_HARDWARE", environment_variable, MAX_ENVIRONMENT_SIZE))
    {
        if (!strcasecmp(environment_variable, "on"))
            This->pipeasio_connect_to_hardware = TRUE;
        else if (!strcasecmp(environment_variable, "off"))
            This->pipeasio_connect_to_hardware = FALSE;
    }

    if (GetEnvironmentVariableA("PIPEASIO_FIXED_BUFFERSIZE", environment_variable, MAX_ENVIRONMENT_SIZE))
    {
        if (!strcasecmp(environment_variable, "on"))
            This->pipeasio_fixed_buffersize = TRUE;
        else if (!strcasecmp(environment_variable, "off"))
            This->pipeasio_fixed_buffersize = FALSE;
    }

    if (GetEnvironmentVariableA("PIPEASIO_PREFERRED_BUFFERSIZE", environment_variable, MAX_ENVIRONMENT_SIZE))
    {
        errno = 0;
        result = strtol(environment_variable, 0, 10);
        if (errno != ERANGE)
            This->pipeasio_preferred_buffersize = result;
    }

    /* override the audio client name gotten from the application name */
    size = GetEnvironmentVariableA("PIPEASIO_CLIENT_NAME", environment_variable, PIPEASIO_MAX_NAME_LENGTH);
    if (size > 0 && size < PIPEASIO_MAX_NAME_LENGTH)
        strcpy(This->client_name, environment_variable);

    /* if pipeasio_preferred_buffersize is not a power of two or if out of range, then set to PIPEASIO_PREFERRED_BUFFERSIZE */
    if (!(This->pipeasio_preferred_buffersize > 0 && !(This->pipeasio_preferred_buffersize&(This->pipeasio_preferred_buffersize-1))
            && This->pipeasio_preferred_buffersize >= PIPEASIO_MINIMUM_BUFFERSIZE
            && This->pipeasio_preferred_buffersize <= PIPEASIO_MAXIMUM_BUFFERSIZE))
        This->pipeasio_preferred_buffersize = PIPEASIO_PREFERRED_BUFFERSIZE;

    return;
}

/* Allocate the interface pointer and associate it with the vtbl/PipeASIO object */
HRESULT WINAPI PipeASIOCreateInstance(REFIID riid, LPVOID *ppobj)
{
    IPipeASIOImpl   *pobj;

    /* TRACE("riid: %s, ppobj: %p\n", debugstr_guid(riid), ppobj); */

    /* HEAP_ZERO_MEMORY (0x8) is critical: the struct holds host-facing
     * state like host_time.speed (a double the ASIO host may read even
     * when we don't flag it kSpeedValid).  Allocating with random bytes
     * makes the host see uninitialized doubles — NaN/Inf — and apply
     * them as gain/rate multipliers, which sounds like noise and can
     * trip downstream FPU exceptions deep inside the host's engine. */
    pobj = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*pobj));
    if (pobj == NULL)
    {
        WARN("out of memory\n");
        return E_OUTOFMEMORY;
    }

    pobj->lpVtbl = &PipeASIO_Vtbl;
    pobj->ref = 1;
    TRACE("pobj = %p\n", pobj);
    *ppobj = pobj;
    /* TRACE("return %p\n", *ppobj); */
    return S_OK;
}
