/*
 * asio_loopback.c - digital loopback analyzer for PipeASIO.
 *
 * A Wine ASIO host (like asio_probe) that plays a per-channel frame
 * counter out of the driver's output ports and verifies it on the input
 * ports after an external PipeWire loopback (run.sh links the driver's
 * out_* ports through a null sink's monitors back into in_*).
 *
 * Because PipeASIO is float32 end-to-end with no converter in the path,
 * the loopback must be BIT-EXACT.  One test signal yields everything:
 *
 *   - true round-trip latency in samples (counter offset), compared
 *     against GetLatencies()
 *   - sample continuity: every dropped, duplicated, or reordered buffer
 *     breaks the counter sequence
 *   - bit-exactness: any value that is not the exactly-representable
 *     expected float is corruption
 *   - channel mapping: ch0 carries +counter, ch1 carries -counter, so a
 *     swap flips the sign
 *
 * Each buffer size given on the command line runs as a phase in one
 * process: CreateBuffers -> Start -> lock -> measure -> Stop ->
 * DisposeBuffers, exercising the renegotiation path hosts hit when the
 * user changes the buffer size.
 *
 * Usage: wine asio_loopback [seconds] [bufsize ...]
 *        seconds  measurement time per phase after signal lock (def. 6)
 *        bufsize  per-phase ASIO buffer size; 0 = driver's preferred
 *                 (default: one phase at the preferred size)
 *
 * Exit: 0 all phases pass, 2 any phase failed.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <objbase.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- ASIO interface (mirrors src/asio.c) ------------------------- */

typedef struct w_int64_t { ULONG hi; ULONG lo; } w_int64_t;

typedef struct BufferInformation {
    LONG  isInputType;
    LONG  channelNumber;
    void *audioBufferStart;       /* half 0 */
    void *audioBufferEnd;         /* half 1 */
} BufferInformation;

typedef struct ChannelInformation {
    LONG channelNumber;
    LONG isInputType;
    LONG isActive;
    LONG channelGroup;
    LONG sampleType;              /* 19 = ASIOSTFloat32LSB */
    char name[32];
} ChannelInformation;

typedef struct TimeInformation {
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

typedef struct Callbacks {
    void (CALLBACK *swapBuffers)               (LONG, LONG);
    void (CALLBACK *sampleRateChanged)         (double);
    LONG (CALLBACK *sendNotification)          (LONG, LONG, void *, double *);
    void *(CALLBACK *swapBuffersWithTimeInfo)  (TimeInformation *, LONG, LONG);
} Callbacks;

typedef struct IPipeASIO IPipeASIO;
typedef struct IPipeASIOVtbl {
    HRESULT (CALLBACK *QueryInterface)     (IPipeASIO *, REFIID, void **);
    ULONG   (CALLBACK *AddRef)             (IPipeASIO *);
    ULONG   (CALLBACK *Release)            (IPipeASIO *);
    LONG    (CALLBACK *Init)               (IPipeASIO *, void *);
    void    (CALLBACK *GetDriverName)      (IPipeASIO *, char *);
    LONG    (CALLBACK *GetDriverVersion)   (IPipeASIO *);
    void    (CALLBACK *GetErrorMessage)    (IPipeASIO *, char *);
    LONG    (CALLBACK *Start)              (IPipeASIO *);
    LONG    (CALLBACK *Stop)               (IPipeASIO *);
    LONG    (CALLBACK *GetChannels)        (IPipeASIO *, LONG *, LONG *);
    LONG    (CALLBACK *GetLatencies)       (IPipeASIO *, LONG *, LONG *);
    LONG    (CALLBACK *GetBufferSize)      (IPipeASIO *, LONG *, LONG *, LONG *, LONG *);
    LONG    (CALLBACK *CanSampleRate)      (IPipeASIO *, double);
    LONG    (CALLBACK *GetSampleRate)      (IPipeASIO *, double *);
    LONG    (CALLBACK *SetSampleRate)      (IPipeASIO *, double);
    LONG    (CALLBACK *GetClockSources)    (IPipeASIO *, void *, LONG *);
    LONG    (CALLBACK *SetClockSource)     (IPipeASIO *, LONG);
    LONG    (CALLBACK *GetSamplePosition)  (IPipeASIO *, w_int64_t *, w_int64_t *);
    LONG    (CALLBACK *GetChannelInfo)     (IPipeASIO *, ChannelInformation *);
    LONG    (CALLBACK *CreateBuffers)      (IPipeASIO *, BufferInformation *, LONG, LONG, Callbacks *);
    LONG    (CALLBACK *DisposeBuffers)     (IPipeASIO *);
    LONG    (CALLBACK *ControlPanel)       (IPipeASIO *);
    LONG    (CALLBACK *Future)             (IPipeASIO *, LONG, void *);
    LONG    (CALLBACK *OutputReady)        (IPipeASIO *);
} IPipeASIOVtbl;
struct IPipeASIO { const IPipeASIOVtbl *lpVtbl; };

static const GUID CLSID_PipeASIO = {
    0x2D3CA9E2, 0x1193, 0x4C5D,
    { 0xB5, 0xFD, 0x38, 0x79, 0x8F, 0x3D, 0xC0, 0x74 }
};

/* ---------- signal codec ------------------------------------------------- *
 * Counter v in [1, 2^24) maps to f = v / 2^24: both v and f are exactly
 * representable in float32, so the loopback round-trip must reproduce the
 * bits.  ch0 carries +f, ch1 carries -f (channel-swap detector).  The
 * counter resets every phase; phases must stay under 2^24 samples
 * (~349 s @ 48 kHz) so it never wraps. */

#define SCALE   16777216.0f      /* 2^24 */
#define NCH     2
#define LOCK_RUN 64              /* consecutive in-sequence frames = lock */

static float enc(UINT32 v, int ch)
{
    float f = (float)v * (1.0f / SCALE);
    return ch ? -f : f;
}

/* ---------- per-phase callback state ------------------------------------ */

typedef struct chstat {
    int       locked;
    UINT32    expect;            /* next counter value expected */
    long long latency;           /* input index - output index of locked value */
    LONG      discont;           /* post-lock sequence breaks */
    LONG      bit_err;           /* post-lock undecodable values */
    LONG      sign_err;          /* post-lock wrong-channel values */
    int       run;               /* lock-acquisition run length */
    UINT32    run_expect;
} chstat;

static volatile LONG g_cycles;
static LONG    g_bs;                       /* current phase buffer size */
static float  *g_in_buf[NCH][2];           /* [channel][half] */
static float  *g_out_buf[NCH][2];
static UINT32  g_out_counter;              /* next value to write (starts 1) */
static UINT64  g_in_index;                 /* input frames consumed this phase */
static chstat  g_ch[NCH];

static void phase_reset(LONG bs)
{
    g_cycles      = 0;
    g_bs          = bs;
    g_out_counter = 1;
    g_in_index    = 0;
    memset((void *)g_ch, 0, sizeof g_ch);
}

/* Decode one input sample; returns the counter value or 0 if not a clean
 * encoded value for channel `ch` (sign_ok reports the sign check). */
static UINT32 dec(float x, int ch, int *sign_ok)
{
    float a  = fabsf(x);
    *sign_ok = ch ? (x < 0.0f) : (x > 0.0f);
    UINT32 v = (UINT32)(a * SCALE + 0.5f);
    if (v < 1 || v >= (UINT32)SCALE)
        return 0;
    if ((float)v * (1.0f / SCALE) != a)   /* bit-exactness */
        return 0;
    return v;
}

static void process_half(LONG idx)
{
    int ch;
    LONG f;

    /* verify inputs */
    for (ch = 0; ch < NCH; ch++) {
        const float *in = g_in_buf[ch][idx];
        chstat *s = &g_ch[ch];
        for (f = 0; f < g_bs; f++) {
            int    sign_ok;
            UINT32 v = dec(in[f], ch, &sign_ok);

            if (!s->locked) {
                /* acquire: LOCK_RUN consecutive incrementing frames */
                if (v && sign_ok && (s->run == 0 || v == s->run_expect)) {
                    s->run++;
                    s->run_expect = v + 1;
                    if (s->run >= LOCK_RUN) {
                        UINT64 gi0 = g_in_index + f - (LOCK_RUN - 1);
                        UINT32 v0  = v - (LOCK_RUN - 1);
                        s->locked  = 1;
                        s->expect  = v + 1;
                        s->latency = (long long)gi0 - ((long long)v0 - 1);
                    }
                } else if (v && sign_ok) {
                    s->run        = 1;
                    s->run_expect = v + 1;
                } else {
                    s->run = 0;
                }
                continue;
            }

            if (v && sign_ok && v == s->expect) {
                s->expect = v + 1;
            } else if (v && sign_ok) {
                s->discont++;             /* jump within a valid stream */
                s->expect = v + 1;
            } else if (v && !sign_ok) {
                s->sign_err++;            /* channel swap / crosstalk */
                s->expect++;
            } else {
                s->bit_err++;             /* corrupted / silent sample */
                s->expect++;
            }
        }
    }

    /* generate outputs */
    for (ch = 0; ch < NCH; ch++) {
        float *out = g_out_buf[ch][idx];
        for (f = 0; f < g_bs; f++)
            out[f] = enc(g_out_counter + (UINT32)f, ch);
    }
    g_out_counter += (UINT32)g_bs;
    g_in_index    += (UINT64)g_bs;
    g_cycles++;
}

static void CALLBACK cb_swapBuffers(LONG idx, LONG direct)
{
    (void)direct;
    process_half(idx);
}
static void CALLBACK cb_sampleRateChanged(double rate)
{
    fprintf(stderr, "[loop] sampleRateChanged(%f)\n", rate);
}
static LONG CALLBACK cb_sendNotification(LONG selector, LONG value,
                                         void *msg, double *opt)
{
    (void)value; (void)msg; (void)opt;
    if (selector == 1 || selector == 2) return 1;
    if (selector == 7 /* kAsioSupportsTimeInfo */) return 1;
    return 0;
}
static void *CALLBACK cb_swapBuffersWithTimeInfo(TimeInformation *t,
                                                 LONG idx, LONG direct)
{
    (void)t; (void)direct;
    process_half(idx);
    return NULL;
}

/* ---------- phase driver ------------------------------------------------- */

static int die(const char *what, LONG err)
{
    fprintf(stderr, "[loop] FAIL: %s -> %ld\n", what, (long)err);
    return 1;
}

/* Runs one CreateBuffers/Start/measure/Stop/Dispose phase.
 * Returns 0 pass, 1 fail. */
static int run_phase(IPipeASIO *asio, LONG bs, int seconds)
{
    BufferInformation bi[2 * NCH];
    Callbacks cbs = {
        cb_swapBuffers, cb_sampleRateChanged,
        cb_sendNotification, cb_swapBuffersWithTimeInfo
    };
    LONG rc;
    int  ch;

    double rate = 0.0;
    asio->lpVtbl->GetSampleRate(asio, &rate);

    for (ch = 0; ch < NCH; ch++) {
        bi[ch].isInputType            = 1;
        bi[ch].channelNumber          = ch;
        bi[NCH + ch].isInputType      = 0;
        bi[NCH + ch].channelNumber    = ch;
    }

    rc = asio->lpVtbl->CreateBuffers(asio, bi, 2 * NCH, bs, &cbs);
    if (rc != 0)
        return die("CreateBuffers", rc);

    for (ch = 0; ch < NCH; ch++) {
        g_in_buf[ch][0]  = (float *)bi[ch].audioBufferStart;
        g_in_buf[ch][1]  = (float *)bi[ch].audioBufferEnd;
        g_out_buf[ch][0] = (float *)bi[NCH + ch].audioBufferStart;
        g_out_buf[ch][1] = (float *)bi[NCH + ch].audioBufferEnd;
    }

    LONG inLat = 0, outLat = 0;
    asio->lpVtbl->GetLatencies(asio, &inLat, &outLat);

    fprintf(stderr, "[loop] phase: bs=%ld rate=%.0f latencies in=%ld out=%ld\n",
            (long)bs, rate, (long)inLat, (long)outLat);

    phase_reset(bs);

    rc = asio->lpVtbl->Start(asio);
    if (rc != 0) {
        asio->lpVtbl->DisposeBuffers(asio);
        return die("Start", rc);
    }

    /* wait for signal lock on both channels (run.sh links the loopback
     * asynchronously, so the first second(s) are silence) */
    int waited;
    for (waited = 0; waited < 100; waited++) {       /* 10 s */
        if (g_ch[0].locked && g_ch[1].locked)
            break;
        Sleep(100);
    }
    if (!(g_ch[0].locked && g_ch[1].locked)) {
        fprintf(stderr, "[loop] FAIL: no signal lock after 10 s "
                "(ch0=%d ch1=%d, cycles=%ld) - loopback links missing?\n",
                g_ch[0].locked, g_ch[1].locked, (long)g_cycles);
        asio->lpVtbl->Stop(asio);
        asio->lpVtbl->DisposeBuffers(asio);
        return 1;
    }
    fprintf(stderr, "[loop] locked after %d ms (latency ch0=%lld ch1=%lld)\n",
            waited * 100, g_ch[0].latency, g_ch[1].latency);

    /* measurement window */
    for (int t = 0; t < seconds; t++) {
        LONG before = g_cycles;
        Sleep(1000);
        fprintf(stderr, "[loop]   t=%d: +%ld cycles, discont=%ld/%ld "
                "bits=%ld/%ld sign=%ld/%ld\n",
                t + 1, (long)(g_cycles - before),
                (long)g_ch[0].discont, (long)g_ch[1].discont,
                (long)g_ch[0].bit_err, (long)g_ch[1].bit_err,
                (long)g_ch[0].sign_err, (long)g_ch[1].sign_err);
    }

    asio->lpVtbl->Stop(asio);
    asio->lpVtbl->DisposeBuffers(asio);

    /* verdict */
    int fail = 0;
    for (ch = 0; ch < NCH; ch++) {
        chstat *s = &g_ch[ch];
        if (s->discont || s->bit_err || s->sign_err) {
            fprintf(stderr, "[loop] FAIL ch%d: discont=%ld bit_err=%ld sign_err=%ld\n",
                    ch, (long)s->discont, (long)s->bit_err, (long)s->sign_err);
            fail = 1;
        }
        if (s->latency <= 0) {
            fprintf(stderr, "[loop] FAIL ch%d: non-positive latency %lld\n",
                    ch, s->latency);
            fail = 1;
        }
    }
    if (g_ch[0].latency != g_ch[1].latency) {
        fprintf(stderr, "[loop] FAIL: channel latencies differ (%lld vs %lld)\n",
                g_ch[0].latency, g_ch[1].latency);
        fail = 1;
    }

    long long rtl      = g_ch[0].latency;
    long long reported = (long long)inLat + (long long)outLat;
    if (rtl % bs)
        fprintf(stderr, "[loop] WARN: RTL %lld not a multiple of bs %ld\n",
                rtl, (long)bs);
    if (llabs(rtl - reported) > (long long)bs) {
        fprintf(stderr, "[loop] FAIL: measured RTL %lld vs reported %lld "
                "(in %ld + out %ld) differs by more than one buffer\n",
                rtl, reported, (long)inLat, (long)outLat);
        fail = 1;
    }

    fprintf(stderr, "[loop] phase bs=%ld: RTL=%lld samples (%.2f cycles, "
            "%.2f ms) reported=%lld -> %s\n",
            (long)bs, rtl, (double)rtl / (double)bs,
            rate > 0 ? 1000.0 * (double)rtl / rate : 0.0,
            reported, fail ? "FAIL" : "PASS");
    return fail;
}

/* ---------- main --------------------------------------------------------- */

/* winecrt0 hands main() argc=0 under -mno-cygwin on current Wine, so
 * rebuild argv from GetCommandLineA() (argv[0] may be quoted). */
static int parse_cmdline(char ***out)
{
    static char  buf[1024];
    static char *args[32];
    int          n = 0;
    char        *p = buf;

    lstrcpynA(buf, GetCommandLineA(), sizeof buf);
    while (*p && n < 32) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            args[n++] = ++p;
            while (*p && *p != '"') p++;
        } else {
            args[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (*p) *p++ = 0;
    }
    *out = args;
    return n;
}

int main(void)
{
    char **argv;
    int    argc = parse_cmdline(&argv);

    setvbuf(stderr, NULL, _IONBF, 0);

    int seconds = (argc > 1) ? atoi(argv[1]) : 6;
    if (seconds < 1) seconds = 1;

    HRESULT hr = CoInitialize(NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[loop] CoInitialize -> 0x%lx\n", (unsigned long)hr);
        return 1;
    }

    IPipeASIO *asio = NULL;
    hr = CoCreateInstance(&CLSID_PipeASIO, NULL, CLSCTX_INPROC_SERVER,
                          &CLSID_PipeASIO, (void **)&asio);
    if (FAILED(hr) || !asio) {
        fprintf(stderr, "[loop] CoCreateInstance -> 0x%lx\n", (unsigned long)hr);
        CoUninitialize();
        return 1;
    }

    LONG rc = asio->lpVtbl->Init(asio, NULL);
    if (rc == 0) {  /* ASIOTrue = 1, ASIOFalse = 0 on this path */
        asio->lpVtbl->Release(asio);
        CoUninitialize();
        return die("Init", rc);
    }

    LONG nin = 0, nout = 0;
    asio->lpVtbl->GetChannels(asio, &nin, &nout);
    if (nin < NCH || nout < NCH) {
        fprintf(stderr, "[loop] need %d in / %d out, driver has %ld/%ld\n",
                NCH, NCH, (long)nin, (long)nout);
        asio->lpVtbl->Release(asio);
        CoUninitialize();
        return 1;
    }

    LONG mn = 0, mx = 0, pref = 0, gran = 0;
    asio->lpVtbl->GetBufferSize(asio, &mn, &mx, &pref, &gran);
    fprintf(stderr, "[loop] driver: buffers min=%ld max=%ld pref=%ld\n",
            (long)mn, (long)mx, (long)pref);

    /* All channels must be float32 (type 19) - the codec depends on it. */
    for (int ch = 0; ch < NCH; ch++) {
        for (int isin = 0; isin <= 1; isin++) {
            ChannelInformation ci;
            memset(&ci, 0, sizeof ci);
            ci.channelNumber = ch;
            ci.isInputType   = isin;
            rc = asio->lpVtbl->GetChannelInfo(asio, &ci);
            if (rc != 0 || ci.sampleType != 19) {
                fprintf(stderr, "[loop] FAIL: ch%d %s sampleType=%ld (want 19)\n",
                        ch, isin ? "in" : "out", (long)ci.sampleType);
                asio->lpVtbl->Release(asio);
                CoUninitialize();
                return 1;
            }
        }
    }

    /* phase list: argv[2..] buffer sizes, 0 or none = preferred */
    int failures = 0, phases = 0;
    if (argc > 2) {
        for (int a = 2; a < argc; a++) {
            LONG bs = (LONG)atol(argv[a]);
            if (bs == 0) bs = pref;
            if (bs < mn || bs > mx) {
                fprintf(stderr, "[loop] skip bs=%ld (outside %ld..%ld; "
                        "set PIPEASIO_FIXED_BUFFERSIZE=off?)\n",
                        (long)bs, (long)mn, (long)mx);
                continue;
            }
            phases++;
            failures += run_phase(asio, bs, seconds);
        }
    } else {
        phases++;
        failures += run_phase(asio, pref, seconds);
    }

    asio->lpVtbl->Release(asio);
    CoUninitialize();

    if (!phases) {
        fprintf(stderr, "[loop] no runnable phases\n");
        return 1;
    }
    fprintf(stderr, "[loop] %d/%d phases passed -> %s\n",
            phases - failures, phases, failures ? "FAIL" : "PASS");
    return failures ? 2 : 0;
}
