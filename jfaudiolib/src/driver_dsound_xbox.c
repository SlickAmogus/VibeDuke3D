/*
 * Xbox DirectSound audio driver for jfaudiolib (MultiVoc backend)
 *
 * Replaces the SDL audio path with RXDK's DirectSound library, which
 * programs the APU's full VP→GP→EP→AC97 pipeline including:
 *   - Voice Processor hardware mixing
 *   - GP FIFO DMA (not DSP MOVE — the key difference from our manual code)
 *   - EP Dolby AC3 encoding for optical 5.1 output
 *
 * Architecture:
 *   MultiVoc generates mixed PCM into ring buffers via MixCallBack.
 *   We create a DirectSound looping buffer and periodically copy PCM
 *   into it via Lock/Unlock in a pump function called from the game loop.
 *   DirectSoundDoWork() drives the hardware frame processing.
 */

#define NOD3D 1      /* Don't include D3D from xtl.h */
#define NODSOUND 1   /* We include dsound.h manually below */
#include "xbox_defs.h"

/* Include RXDK DirectSound header */
#undef NODSOUND
#include <dsound.h>

#include "driver_dsound_xbox.h"
#include "multivoc.h"
#include "asssys.h"
#include <string.h>

#ifdef _XBOX
extern void xbox_log(const char *fmt, ...);
#else
#define xbox_log(...) ((void)0)
#endif

/* ---- Error codes -------------------------------------------------------- */

enum {
    XDS_Err_Warning = -2,
    XDS_Err_Error   = -1,
    XDS_Err_Ok      = 0,
    XDS_Err_Uninitialised,
    XDS_Err_CreateDS,
    XDS_Err_CreateBuffer,
    XDS_Err_Lock,
    XDS_Err_Play
};

/* ---- State -------------------------------------------------------------- */

static int ErrorCode = XDS_Err_Ok;
static int Initialised = 0;
static int Playing = 0;

static IDirectSound *pDS = NULL;
static IDirectSoundBuffer *pDSBuf = NULL;         /* Front (FL/FR) - default mixbins */
static IDirectSoundBuffer *pDSBufCenter = NULL;   /* Center/LFE - custom mixbins */
static IDirectSoundBuffer *pDSBufSurround = NULL; /* Surround (SL/SR) - custom mixbins */
static LPAC97MEDIAOBJECT pAc97Digital = NULL;     /* AC97 digital output (keeps AC3 mode alive) */

/* Ring buffer from MultiVoc */
static char *MixBuffer = NULL;
static int MixBufferSize = 0;       /* Size of ONE division */
static int MixBufferCount = 0;      /* Number of divisions */
static int MixBufferCurrent = 0;    /* Current division index */
static int MixBufferUsed = 0;       /* Bytes consumed from current div */
static void (*MixCallBack)(void) = NULL;

/* DirectSound buffer state */
#define DS_BUFFER_SIZE  (32768)      /* 32KB ring buffer (~185ms at 44100/16/2) */
static DWORD DSWriteCursor = 0;     /* Our write position in DS buffer */
static CRITICAL_SECTION DSLock;

static int PumpCallCount = 0;

/* Volume amplification — Xbox APU pipeline with AC3 encoding outputs quieter
 * than expected.  Boost PCM samples before writing to the DS buffer. */
#define VOLUME_BOOST 4  /* 4x = ~12 dB gain */
#define SURROUND_EXTRA_NUM 6  /* 6/5 = 1.2x = +20% extra for surround/center */
#define SURROUND_EXTRA_DEN 5

static void AmplifyBuffer(void *buf, DWORD bytes)
{
    short *samples = (short *)buf;
    DWORD count = bytes / 2;  /* 16-bit samples */
    DWORD i;

    for (i = 0; i < count; i++) {
        int val = (int)samples[i] * VOLUME_BOOST;
        if (val > 32767) val = 32767;
        else if (val < -32768) val = -32768;
        samples[i] = (short)val;
    }
}

static void AmplifyBufferSurround(void *buf, DWORD bytes)
{
    short *samples = (short *)buf;
    DWORD count = bytes / 2;
    DWORD i;

    for (i = 0; i < count; i++) {
        int val = (int)samples[i] * VOLUME_BOOST * SURROUND_EXTRA_NUM / SURROUND_EXTRA_DEN;
        if (val > 32767) val = 32767;
        else if (val < -32768) val = -32768;
        samples[i] = (short)val;
    }
}

/* Bass boost — simple one-pole low-pass filter applied additively.
 * Extracts low-frequency content and mixes it back in at BASS_GAIN ratio.
 * Cutoff ~150 Hz at 22050 Hz sample rate (alpha ≈ 0.04), ~150 Hz at 11025 Hz
 * if mixrate is lower.  Stereo: L/R processed independently. */
#define BASS_ALPHA_NUM  1    /* alpha = 1/24 ≈ 0.042 (~150 Hz @ 22050) */
#define BASS_ALPHA_DEN  24
#define BASS_GAIN_NUM   3    /* add 3/4 of low-passed signal back = ~+6 dB shelf */
#define BASS_GAIN_DEN   4

static int bass_state_l = 0;  /* fixed-point Q16 filter state */
static int bass_state_r = 0;

static void BassBoostBuffer(void *buf, DWORD bytes)
{
    short *samples = (short *)buf;
    DWORD count = bytes / 2;  /* total samples (L+R interleaved) */
    DWORD i;

    for (i = 0; i + 1 < count; i += 2) {
        int l = samples[i];
        int r = samples[i + 1];

        /* One-pole LPF: state += alpha * (input - state)  (Q16 fixed-point) */
        bass_state_l += (l * 65536 - bass_state_l) * BASS_ALPHA_NUM / BASS_ALPHA_DEN;
        bass_state_r += (r * 65536 - bass_state_r) * BASS_ALPHA_NUM / BASS_ALPHA_DEN;

        /* Add filtered bass back to original */
        l += (bass_state_l / 65536) * BASS_GAIN_NUM / BASS_GAIN_DEN;
        r += (bass_state_r / 65536) * BASS_GAIN_NUM / BASS_GAIN_DEN;

        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        samples[i] = (short)l;
        samples[i + 1] = (short)r;
    }
}

/* Stereo-to-surround upmix (Hafler principle).
 * Extracts the stereo difference signal (L-R) from the front buffer and
 * adds it to the surround buffer.  Center-panned content (lead melody,
 * bass, dialog) cancels in L-R so stays in front only.  Stereo-spread
 * content (reverb, panned instruments) creates ambient surround presence.
 * Applied before amplification so the surround gain treats upmixed and
 * direct surround content identically. */
#define UPMIX_NUM 5   /* 5/10 = 50% of difference → surround */
#define UPMIX_DEN 10

static void UpmixStereoToSurround(void *front_buf, void *surround_buf, DWORD bytes)
{
    short *front = (short *)front_buf;
    short *surround = (short *)surround_buf;
    DWORD count = bytes / 2;   /* 16-bit samples */
    DWORD i;

    for (i = 0; i + 1 < count; i += 2) {
        int fl = (int)front[i];        /* front left */
        int fr = (int)front[i + 1];    /* front right */
        int diff = fl - fr;            /* stereo difference */

        /* SL gets +diff, SR gets -diff (decorrelated surround image) */
        int sl = surround[i]     + diff * UPMIX_NUM / UPMIX_DEN;
        int sr = surround[i + 1] - diff * UPMIX_NUM / UPMIX_DEN;

        if (sl > 32767) sl = 32767; else if (sl < -32768) sl = -32768;
        if (sr > 32767) sr = 32767; else if (sr < -32768) sr = -32768;
        surround[i]     = (short)sl;
        surround[i + 1] = (short)sr;
    }
}

/* Feed low-frequency content from the front buffer into the LFE channel.
 * This gives explosions and music a subwoofer presence. */
#define LFE_FEED_NUM  1   /* 1/3 of low-passed front signal → LFE */
#define LFE_FEED_DEN  3

static int lfe_state_l = 0;
static int lfe_state_r = 0;

static void FeedLFE(void *front_buf, void *center_buf, DWORD bytes)
{
    short *front = (short *)front_buf;
    short *center = (short *)center_buf;  /* LFE is right channel of center pair */
    DWORD count = bytes / 2;
    DWORD i;

    for (i = 0; i + 1 < count; i += 2) {
        int mono = ((int)front[i] + (int)front[i + 1]) / 2;

        /* Low-pass for subwoofer content (~80 Hz) */
        int state = (lfe_state_l + lfe_state_r) / 2;
        state += (mono * 65536 - state) / 48;  /* alpha ≈ 1/48 → ~73 Hz @ 22050 */
        lfe_state_l = state;
        lfe_state_r = state;

        /* Mix into LFE (right channel of center buffer) */
        int lfe = (int)center[i + 1] + (state / 65536) * LFE_FEED_NUM / LFE_FEED_DEN;
        if (lfe > 32767) lfe = 32767; else if (lfe < -32768) lfe = -32768;
        center[i + 1] = (short)lfe;
    }
}

/* ---- API ---------------------------------------------------------------- */

int XboxDSDrv_GetError(void)
{
    return ErrorCode;
}

const char *XboxDSDrv_ErrorString(int ErrorNumber)
{
    switch (ErrorNumber) {
        case XDS_Err_Warning:
        case XDS_Err_Error:
            return XboxDSDrv_ErrorString(ErrorCode);
        case XDS_Err_Ok:
            return "Xbox DirectSound ok.";
        case XDS_Err_Uninitialised:
            return "Xbox DirectSound uninitialised.";
        case XDS_Err_CreateDS:
            return "Xbox DirectSound: DirectSoundCreate failed.";
        case XDS_Err_CreateBuffer:
            return "Xbox DirectSound: CreateSoundBuffer failed.";
        case XDS_Err_Lock:
            return "Xbox DirectSound: Lock failed.";
        case XDS_Err_Play:
            return "Xbox DirectSound: Play failed.";
        default:
            return "Unknown Xbox DirectSound error.";
    }
}

int XboxDSDrv_PCM_Init(int *mixrate, int *numchannels, int *samplebits, void *initdata)
{
    HRESULT hr;
    DSBUFFERDESC dsbd;
    WAVEFORMATEX wfx;

    (void)initdata;

    if (Initialised) {
        XboxDSDrv_PCM_Shutdown();
    }

    xbox_log("XboxDS: PCM_Init rate=%d ch=%d bits=%d\n",
        *mixrate, *numchannels, *samplebits);

    /* Force stereo 16-bit (DirectSound on Xbox requires 16-bit PCM) */
    *numchannels = 2;
    *samplebits = 16;

    /* Initialize critical section */
    RtlInitializeCriticalSection(&DSLock);

    /* Reset APU to clean state before DirectSoundCreate.
     * The Xbox kernel leaves APU registers in a partially-configured state
     * from boot. RXDK's DirectSound expects to own the APU from scratch.
     * Disable APU interrupts and reset the front-end/setup-engine. */
    {
        volatile unsigned long *apu = (volatile unsigned long *)0xFE800000u;

        /* Disable all APU interrupts */
        apu[0x1004 / 4] = 0;  /* NV_PAPU_IEN = 0 */

        /* Stop setup engine processing */
        apu[0x2000 / 4] = 0;  /* NV_PAPU_SECTL = 0 (disable XCNTMODE) */

        /* Halt front-end */
        apu[0x1100 / 4] = 0;  /* NV_PAPU_FECTL = 0 */

        /* Clear any pending interrupts */
        apu[0x1000 / 4] = 0xFFFFFFFF;  /* NV_PAPU_ISTS = write-1-to-clear all */

        /* Brief stall to let hardware settle */
        KeStallExecutionProcessor(100);

        xbox_log("XboxDS: APU reset done\n");
    }

    /* Initialize DirectSound's global critical section.
     * dsound.lib expects this to be initialized by DLL startup, but since
     * we link the .obj files directly, no DLL init runs.
     * g_DirectSoundCriticalSection is defined in globals.obj. */
    {
        extern CRITICAL_SECTION g_DirectSoundCriticalSection;
        RtlInitializeCriticalSection(&g_DirectSoundCriticalSection);
        xbox_log("XboxDS: initialized g_DirectSoundCriticalSection\n");
    }

    /* Set speaker config for 5.1 AC3 output.
     * RXDK globals (from globals.obj) control what DirectSoundCreate does:
     *   g_dwDirectSoundOverrideSpeakerConfig — checked first by DirectSoundCreate
     *   g_dwDirectSoundSpeakerConfig — actual config used by the APU
     * DirectSoundOverrideSpeakerConfig() strips the AC3 flag (0x10000) — the
     * GetSpeakerConfig returns 0x2 not 0x10002.  So we also set the globals
     * directly to ensure the AC3 flag reaches SetupEncodeProcessor, which
     * calls AC3SetDigitalOutput to program the EP for Dolby Digital. */
    {
        extern DWORD g_dwDirectSoundOverrideSpeakerConfig;
        extern DWORD g_dwDirectSoundSpeakerConfig;
        DWORD desired = DSSPEAKER_ENABLE_AC3 | DSSPEAKER_SURROUND;  /* 0x10002 */

        /* Call the API first (sets basic layout) */
        DirectSoundOverrideSpeakerConfig(desired);

        /* Force the AC3 flag into both globals directly */
        g_dwDirectSoundOverrideSpeakerConfig = desired;
        g_dwDirectSoundSpeakerConfig = desired;

        xbox_log("XboxDS: Speaker globals set: override=0x%08X config=0x%08X\n",
            (unsigned)g_dwDirectSoundOverrideSpeakerConfig,
            (unsigned)g_dwDirectSoundSpeakerConfig);
    }

    /* Create DirectSound — reads speaker config to load GP/EP programs */
    xbox_log("XboxDS: calling DirectSoundCreate...\n");
    hr = DirectSoundCreate(NULL, &pDS, NULL);
    if (FAILED(hr)) {
        xbox_log("XboxDS: DirectSoundCreate FAILED hr=0x%08X\n", (unsigned)hr);
        ErrorCode = XDS_Err_CreateDS;
        return XDS_Err_Error;
    }
    xbox_log("XboxDS: DirectSoundCreate OK pDS=%p\n", (void *)pDS);

    /* Check what DirectSoundCreate did to the globals — did AC3 flag survive? */
    {
        extern DWORD g_dwDirectSoundOverrideSpeakerConfig;
        extern DWORD g_dwDirectSoundSpeakerConfig;
        DWORD speakerConfig = 0;
        hr = IDirectSound_GetSpeakerConfig(pDS, &speakerConfig);
        xbox_log("XboxDS: Post-create: GetSpeaker=0x%08X override=0x%08X config=0x%08X\n",
            (unsigned)speakerConfig,
            (unsigned)g_dwDirectSoundOverrideSpeakerConfig,
            (unsigned)g_dwDirectSoundSpeakerConfig);
    }

    /* Set up wave format */
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (unsigned short)*numchannels;
    wfx.nSamplesPerSec = *mixrate;
    wfx.wBitsPerSample = (unsigned short)*samplebits;
    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    /* Create sound buffer — explicitly route to FL/FR only to prevent
     * any default RXDK routing that might include surround channels.
     * NOTE: frontPairs/frontBins must stay in scope through CreateSoundBuffer
     * since dsbd.lpMixBins holds a pointer to them. */
    {
        DSMIXBINVOLUMEPAIR frontPairs[2];
        DSMIXBINS frontBins;

        frontPairs[0].dwMixBin = DSMIXBIN_FRONT_LEFT;
        frontPairs[0].lVolume  = 0;
        frontPairs[1].dwMixBin = DSMIXBIN_FRONT_RIGHT;
        frontPairs[1].lVolume  = 0;
        frontBins.dwMixBinCount = 2;
        frontBins.lpMixBinVolumePairs = frontPairs;

        memset(&dsbd, 0, sizeof(dsbd));
        dsbd.dwSize = sizeof(dsbd);
        dsbd.dwFlags = 0; /* 2D buffer */
        dsbd.dwBufferBytes = DS_BUFFER_SIZE;
        dsbd.lpwfxFormat = &wfx;
        dsbd.lpMixBins = &frontBins;
        dsbd.dwInputMixBin = 0;

        hr = IDirectSound_CreateSoundBuffer(pDS, &dsbd, &pDSBuf, NULL);
        if (FAILED(hr)) {
            xbox_log("XboxDS: CreateSoundBuffer FAILED hr=0x%08X\n", (unsigned)hr);
            IDirectSound_Release(pDS);
            pDS = NULL;
            ErrorCode = XDS_Err_CreateBuffer;
            return XDS_Err_Error;
        }
        xbox_log("XboxDS: CreateSoundBuffer OK pBuf=%p size=%d\n",
            (void *)pDSBuf, DS_BUFFER_SIZE);
    }

    /* Create Center/LFE and Surround buffers with custom mixbin routing */
    {
        DSMIXBINVOLUMEPAIR centerPairs[2];
        DSMIXBINS centerBins;
        DSMIXBINVOLUMEPAIR surroundPairs[2];
        DSMIXBINS surroundBins;

        /* Center buffer: ch0→Center, ch1→LFE */
        centerPairs[0].dwMixBin = DSMIXBIN_FRONT_CENTER;
        centerPairs[0].lVolume  = 0; /* DSBVOLUME_MAX */
        centerPairs[1].dwMixBin = DSMIXBIN_LOW_FREQUENCY;
        centerPairs[1].lVolume  = 0;
        centerBins.dwMixBinCount = 2;
        centerBins.lpMixBinVolumePairs = centerPairs;

        dsbd.lpMixBins = &centerBins;
        hr = IDirectSound_CreateSoundBuffer(pDS, &dsbd, &pDSBufCenter, NULL);
        if (FAILED(hr)) {
            xbox_log("XboxDS: CreateSoundBuffer CENTER FAILED hr=0x%08X\n", (unsigned)hr);
            /* Non-fatal: fall back to stereo-only */
            pDSBufCenter = NULL;
        } else {
            xbox_log("XboxDS: Center/LFE buffer created OK\n");
        }

        /* Surround buffer: ch0→BackLeft, ch1→BackRight */
        surroundPairs[0].dwMixBin = DSMIXBIN_BACK_LEFT;
        surroundPairs[0].lVolume  = 0;
        surroundPairs[1].dwMixBin = DSMIXBIN_BACK_RIGHT;
        surroundPairs[1].lVolume  = 0;
        surroundBins.dwMixBinCount = 2;
        surroundBins.lpMixBinVolumePairs = surroundPairs;

        dsbd.lpMixBins = &surroundBins;
        hr = IDirectSound_CreateSoundBuffer(pDS, &dsbd, &pDSBufSurround, NULL);
        if (FAILED(hr)) {
            xbox_log("XboxDS: CreateSoundBuffer SURROUND FAILED hr=0x%08X\n", (unsigned)hr);
            pDSBufSurround = NULL;
        } else {
            xbox_log("XboxDS: Surround buffer created OK\n");
        }
    }

    /* Enable 5.1 surround if center and surround buffers were created */
    if (pDSBufCenter && pDSBufSurround) {
        MV_SetSurroundMode(1);
        xbox_log("XboxDS: 5.1 surround mode ENABLED\n");
    }

    /* Clear all buffers */
    {
        LPVOID ptr1;
        DWORD bytes1;
        hr = IDirectSoundBuffer_Lock(pDSBuf, 0, DS_BUFFER_SIZE,
            &ptr1, &bytes1, NULL, NULL, DSBLOCK_ENTIREBUFFER);
        if (SUCCEEDED(hr)) {
            memset(ptr1, 0, bytes1);
            IDirectSoundBuffer_Unlock(pDSBuf, ptr1, bytes1, NULL, 0);
        }
        if (pDSBufCenter) {
            hr = IDirectSoundBuffer_Lock(pDSBufCenter, 0, DS_BUFFER_SIZE,
                &ptr1, &bytes1, NULL, NULL, DSBLOCK_ENTIREBUFFER);
            if (SUCCEEDED(hr)) {
                memset(ptr1, 0, bytes1);
                IDirectSoundBuffer_Unlock(pDSBufCenter, ptr1, bytes1, NULL, 0);
            }
        }
        if (pDSBufSurround) {
            hr = IDirectSoundBuffer_Lock(pDSBufSurround, 0, DS_BUFFER_SIZE,
                &ptr1, &bytes1, NULL, NULL, DSBLOCK_ENTIREBUFFER);
            if (SUCCEEDED(hr)) {
                memset(ptr1, 0, bytes1);
                IDirectSoundBuffer_Unlock(pDSBufSurround, ptr1, bytes1, NULL, 0);
            }
        }
    }

    DSWriteCursor = 0;
    PumpCallCount = 0;
    Initialised = 1;

    xbox_log("XboxDS: PCM_Init OK\n");
    return XDS_Err_Ok;
}

void XboxDSDrv_PCM_Shutdown(void)
{
    if (!Initialised) return;

    xbox_log("XboxDS: PCM_Shutdown\n");

    if (Playing) {
        XboxDSDrv_PCM_StopPlayback();
    }

    if (pAc97Digital) {
        XAc97MediaObject_Release(pAc97Digital);
        pAc97Digital = NULL;
    }
    if (pDSBufSurround) {
        IDirectSoundBuffer_Release(pDSBufSurround);
        pDSBufSurround = NULL;
    }
    if (pDSBufCenter) {
        IDirectSoundBuffer_Release(pDSBufCenter);
        pDSBufCenter = NULL;
    }
    if (pDSBuf) {
        IDirectSoundBuffer_Release(pDSBuf);
        pDSBuf = NULL;
    }

    if (pDS) {
        IDirectSound_Release(pDS);
        pDS = NULL;
    }

    Initialised = 0;
}

/* Fill a DS buffer region from the MV ring buffer (front) and optionally
 * from center/surround static buffers.  MixCallBack is called when a
 * division boundary is crossed, which generates all 3 outputs at once. */
static void FillRegion(char *fdst, DWORD bytes,
                       char *cdst, char *sdst, int surround)
{
    DWORD remaining = bytes;

    while (remaining > 0) {
        char *src;
        DWORD avail, chunk;

        if (MixBufferUsed >= MixBufferSize) {
            MixCallBack();
            MixBufferUsed = 0;
            MixBufferCurrent++;
            if (MixBufferCurrent >= MixBufferCount)
                MixBufferCurrent = 0;
        }

        src = MixBuffer + (MixBufferCurrent * MixBufferSize) + MixBufferUsed;
        avail = MixBufferSize - MixBufferUsed;
        chunk = (remaining < avail) ? remaining : avail;

        memcpy(fdst, src, chunk);
        fdst += chunk;

        if (surround && cdst) {
            memcpy(cdst, (char *)MV_CenterMixBuf + MixBufferUsed, chunk);
            cdst += chunk;
        }
        if (surround && sdst) {
            memcpy(sdst, (char *)MV_SurroundMixBuf + MixBufferUsed, chunk);
            sdst += chunk;
        }

        MixBufferUsed += chunk;
        remaining -= chunk;
    }
}

/* Pump PCM data from MultiVoc ring buffer into DirectSound buffer(s).
 * Called from the game loop (via DirectSoundDoWork integration).
 * In surround mode, fills front/center/surround buffers in lockstep. */
static void PumpAudio(void)
{
    DWORD playCursor, writeCursor;
    DWORD writeBytes;
    HRESULT hr;
    LPVOID fptr1 = NULL, fptr2 = NULL;
    DWORD fbytes1 = 0, fbytes2 = 0;
    LPVOID cptr1 = NULL, cptr2 = NULL;
    DWORD cbytes1 = 0, cbytes2 = 0;
    LPVOID sptr1 = NULL, sptr2 = NULL;
    DWORD sbytes1 = 0, sbytes2 = 0;
    int surround = (pDSBufCenter && pDSBufSurround && MV_GetSurroundMode());

    if (!Playing || !pDSBuf || !MixCallBack) return;

    PumpCallCount++;

    /* Get current play position from front buffer (master clock) */
    hr = IDirectSoundBuffer_GetCurrentPosition(pDSBuf, &playCursor, &writeCursor);
    if (FAILED(hr)) return;

    /* Calculate how much space is available to write */
    if (DSWriteCursor <= playCursor) {
        writeBytes = playCursor - DSWriteCursor;
    } else {
        writeBytes = DS_BUFFER_SIZE - DSWriteCursor + playCursor;
    }

    /* Underrun recovery: if we need to fill more than 75% of the buffer,
     * the play cursor has likely lapped our write cursor during a long
     * stall (e.g. tile preloading, texture eviction, level load).  The
     * circular-distance formula can't distinguish "100 bytes ahead" from
     * "one full lap + 100 bytes ahead," so reset to the DirectSound write
     * cursor and fill a modest amount to resume cleanly. */
    if (writeBytes > DS_BUFFER_SIZE * 3 / 4) {
        DSWriteCursor = writeCursor;
        writeBytes = DS_BUFFER_SIZE / 4;
    }

    if (writeBytes < 1024) return;
    writeBytes -= 512;

    if (PumpCallCount <= 5) {
        xbox_log("XboxDS: Pump#%d play=%u ours=%u avail=%u surr=%d\n",
            PumpCallCount, (unsigned)playCursor,
            (unsigned)DSWriteCursor, (unsigned)writeBytes, surround);
    }

    /* Lock front buffer */
    hr = IDirectSoundBuffer_Lock(pDSBuf, DSWriteCursor, writeBytes,
        &fptr1, &fbytes1, &fptr2, &fbytes2, 0);
    if (FAILED(hr)) return;

    /* Lock center and surround buffers at the same position */
    if (surround) {
        IDirectSoundBuffer_Lock(pDSBufCenter, DSWriteCursor, writeBytes,
            &cptr1, &cbytes1, &cptr2, &cbytes2, 0);
        IDirectSoundBuffer_Lock(pDSBufSurround, DSWriteCursor, writeBytes,
            &sptr1, &sbytes1, &sptr2, &sbytes2, 0);
    }

    /* Fill region 1 (all buffers in lockstep) */
    FillRegion((char *)fptr1, fbytes1,
               surround ? (char *)cptr1 : NULL,
               surround ? (char *)sptr1 : NULL, surround);

    /* Fill wrap-around region 2 */
    if (fptr2 && fbytes2 > 0) {
        FillRegion((char *)fptr2, fbytes2,
                   surround ? (char *)cptr2 : NULL,
                   surround ? (char *)sptr2 : NULL, surround);
    }

    /* Bass boost on front buffer (before amplification, operates on raw mix) */
    if (fptr1 && fbytes1) BassBoostBuffer(fptr1, fbytes1);
    if (fptr2 && fbytes2) BassBoostBuffer(fptr2, fbytes2);

    /* NOTE: Hafler upmix removed — it was extracting L-R differences
     * from the entire front buffer (SFX+music), causing positional SFX
     * (e.g. gunfire panned front-left) to leak into rear speakers.
     * The engine's native surround mixer (MV_SurroundMixBuf) already
     * handles rear-channel placement for positional SFX correctly. */

    /* Boost volume on all buffers */
    if (fptr1 && fbytes1) AmplifyBuffer(fptr1, fbytes1);
    if (fptr2 && fbytes2) AmplifyBuffer(fptr2, fbytes2);
    if (surround) {
        if (cptr1 && cbytes1) AmplifyBufferSurround(cptr1, cbytes1);
        if (cptr2 && cbytes2) AmplifyBufferSurround(cptr2, cbytes2);
        if (sptr1 && sbytes1) AmplifyBufferSurround(sptr1, sbytes1);
        if (sptr2 && sbytes2) AmplifyBufferSurround(sptr2, sbytes2);

        /* Feed low-frequency from front into LFE (subwoofer) */
        if (fptr1 && cptr1 && fbytes1) FeedLFE(fptr1, cptr1, fbytes1);
        if (fptr2 && cptr2 && fbytes2) FeedLFE(fptr2, cptr2, fbytes2);
    }

    IDirectSoundBuffer_Unlock(pDSBuf, fptr1, fbytes1, fptr2, fbytes2);

    if (surround) {
        IDirectSoundBuffer_Unlock(pDSBufCenter, cptr1, cbytes1, cptr2, cbytes2);
        IDirectSoundBuffer_Unlock(pDSBufSurround, sptr1, sbytes1, sptr2, sbytes2);
    }

    DSWriteCursor = (DSWriteCursor + fbytes1 + fbytes2) % DS_BUFFER_SIZE;
}

int XboxDSDrv_PCM_BeginPlayback(char *BufferStart, int BufferSize,
                                 int NumDivisions, void (*CallBackFunc)(void))
{
    HRESULT hr;

    xbox_log("XboxDS: BeginPlayback buf=%p size=%d divs=%d\n",
        (void *)BufferStart, BufferSize, NumDivisions);

    if (!Initialised) {
        ErrorCode = XDS_Err_Uninitialised;
        return XDS_Err_Error;
    }

    if (Playing) {
        XboxDSDrv_PCM_StopPlayback();
    }

    MixBuffer = BufferStart;
    MixBufferSize = BufferSize;
    MixBufferCount = NumDivisions;
    MixBufferCurrent = 0;
    MixBufferUsed = 0;
    MixCallBack = CallBackFunc;
    DSWriteCursor = 0;
    PumpCallCount = 0;

    /* Prime the mix buffer */
    MixCallBack();

    /* Pre-fill the DS buffer with initial audio data */
    {
        LPVOID ptr1;
        DWORD bytes1;
        hr = IDirectSoundBuffer_Lock(pDSBuf, 0, DS_BUFFER_SIZE,
            &ptr1, &bytes1, NULL, NULL, DSBLOCK_ENTIREBUFFER);
        if (SUCCEEDED(hr)) {
            char *dst = (char *)ptr1;
            DWORD remaining = bytes1;
            while (remaining > 0) {
                if (MixBufferUsed >= MixBufferSize) {
                    MixCallBack();
                    MixBufferUsed = 0;
                    MixBufferCurrent++;
                    if (MixBufferCurrent >= MixBufferCount)
                        MixBufferCurrent = 0;
                }
                char *src = MixBuffer + (MixBufferCurrent * MixBufferSize) + MixBufferUsed;
                DWORD avail = MixBufferSize - MixBufferUsed;
                DWORD chunk = (remaining < avail) ? remaining : avail;
                memcpy(dst, src, chunk);
                dst += chunk;
                MixBufferUsed += chunk;
                remaining -= chunk;
            }
            AmplifyBuffer(ptr1, bytes1);
            IDirectSoundBuffer_Unlock(pDSBuf, ptr1, bytes1, NULL, 0);
            DSWriteCursor = bytes1 % DS_BUFFER_SIZE;
            xbox_log("XboxDS: Pre-filled %u bytes\n", (unsigned)bytes1);
        }
    }

    /* Start looping playback on all buffers */
    hr = IDirectSoundBuffer_Play(pDSBuf, 0, 0, DSBPLAY_LOOPING);
    if (FAILED(hr)) {
        xbox_log("XboxDS: Play FAILED hr=0x%08X\n", (unsigned)hr);
        ErrorCode = XDS_Err_Play;
        return XDS_Err_Error;
    }

    if (pDSBufCenter) {
        IDirectSoundBuffer_Play(pDSBufCenter, 0, 0, DSBPLAY_LOOPING);
    }
    if (pDSBufSurround) {
        IDirectSoundBuffer_Play(pDSBufSurround, 0, 0, DSBPLAY_LOOPING);
    }

    Playing = 1;
    xbox_log("XboxDS: Play started, looping (surround=%d)\n",
        (pDSBufCenter && pDSBufSurround) ? 1 : 0);
    return XDS_Err_Ok;
}

/* Flush a single DS buffer with silence */
static void FlushDSBuffer(IDirectSoundBuffer *buf)
{
    LPVOID ptr1;
    DWORD bytes1;
    HRESULT hr;
    if (!buf) return;
    hr = IDirectSoundBuffer_Lock(buf, 0, DS_BUFFER_SIZE,
        &ptr1, &bytes1, NULL, NULL, DSBLOCK_ENTIREBUFFER);
    if (SUCCEEDED(hr)) {
        memset(ptr1, 0, bytes1);
        IDirectSoundBuffer_Unlock(buf, ptr1, bytes1, NULL, 0);
    }
}

void XboxDSDrv_PCM_StopPlayback(void)
{
    if (!Initialised || !Playing) return;

    xbox_log("XboxDS: StopPlayback\n");

    if (pDSBuf) {
        IDirectSoundBuffer_Stop(pDSBuf);
        FlushDSBuffer(pDSBuf);
        IDirectSoundBuffer_SetCurrentPosition(pDSBuf, 0);
    }
    if (pDSBufCenter) {
        IDirectSoundBuffer_Stop(pDSBufCenter);
        FlushDSBuffer(pDSBufCenter);
        IDirectSoundBuffer_SetCurrentPosition(pDSBufCenter, 0);
    }
    if (pDSBufSurround) {
        IDirectSoundBuffer_Stop(pDSBufSurround);
        FlushDSBuffer(pDSBufSurround);
        IDirectSoundBuffer_SetCurrentPosition(pDSBufSurround, 0);
    }

    DSWriteCursor = 0;
    Playing = 0;
}

void XboxDSDrv_PCM_Lock(void)
{
    RtlEnterCriticalSection(&DSLock);
}

void XboxDSDrv_PCM_Unlock(void)
{
    RtlLeaveCriticalSection(&DSLock);
}

/* Flush the DS buffer with silence.  Called when FX_StopAllSounds() runs
 * during scene transitions so stale PCM doesn't replay. */
void XboxDS_FlushBuffer(void)
{
    if (!Initialised || !pDSBuf) return;

    FlushDSBuffer(pDSBuf);
    FlushDSBuffer(pDSBufCenter);
    FlushDSBuffer(pDSBufSurround);

    /* Sync our write cursor to the current play position so PumpAudio
     * doesn't bulk-fill the buffer with silence before new sounds trigger. */
    {
        DWORD playPos;
        if (SUCCEEDED(IDirectSoundBuffer_GetCurrentPosition(pDSBuf, &playPos, NULL))) {
            DSWriteCursor = playPos;
        }
    }
}

/* ---- Public pump function (called from game loop) ----------------------- */

void XboxDS_Pump(void)
{
    if (!Initialised || !Playing) return;

    /* Pump our audio data */
    PumpAudio();

    /* Drive DirectSound's hardware frame processing.
     * This is REQUIRED on Xbox — it triggers the VP→GP→EP pipeline. */
    DirectSoundDoWork();
}
