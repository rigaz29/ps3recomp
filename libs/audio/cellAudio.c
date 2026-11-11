/*
 * ps3recomp - cellAudio HLE implementation
 *
 * Real audio mixing and output. A background mixing thread reads audio data
 * from each active port's buffer in guest memory, mixes them, and outputs
 * to the host audio device.
 *
 * Backend selection:
 *   - Windows default: WASAPI (via mmdeviceapi)
 *   - Everywhere else / if PS3RECOMP_AUDIO_USE_SDL2 defined: SDL2 audio
 *
 * Define PS3RECOMP_AUDIO_USE_SDL2 to force SDL2 backend on Windows.
 *
 * WASAPI, shared mode, as every APO (equalizers, loudness, virtual surround) expects:
 *   PS3_AUDIO_CHANNELS  auto (the default device's own layout), stereo, 5.1 or 7.1: the stream's channels.
 *                       A title's 7.1 port goes to 7.1 and 5.1 as it is (audio_mix.h); Windows folds a
 *                       layout the device lacks down to the one it has.
 *   PS3_AUDIO_RAW=1     raw: past the effects, where the device has a raw path.
 *   PS3_AUDIO_CATEGORY=0  no stream category; by default the stream is AudioCategory_GameEffects.
 *   PS3_AUDIO_REOPEN=0  stay with the device the stream started on; by default a change of the default
 *                       output device, or the loss of it, opens the new default one.
 *   PS3_AUDIO_REOPEN_AT=<s>  open the device again at that second, as a change of device would (a test).
 */

#include "../../runtime/memory/vm.h"   /* vm_commit */
#include "cellAudio.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_base, vm_read64, vm_write32 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "ps3emu/log_first.h"
#include "ps3emu/thread_name.h"
#include "audio_mix.h"

/* Event-queue push/lookup (runtime/syscalls/sys_event.c). Forward-declared
 * rather than including sys_event.h to avoid pulling PPU context/syscall
 * table types into the HLE audio module. */
extern int      sys_event_queue_push_by_id(uint32_t queue_id,
                                            uint64_t source, uint64_t data1,
                                            uint64_t data2,  uint64_t data3);
extern uint32_t sys_event_find_queue_by_key(uint64_t key);
extern uint32_t sys_event_queue_create_direct(uint64_t key, int32_t size);

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_AUDIO_USE_SDL2)
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#elif defined(_WIN32)
  #define AUDIO_BACKEND_SDL2    0
  #define AUDIO_BACKEND_WASAPI  1
#else
  #define AUDIO_BACKEND_SDL2    1
  #define AUDIO_BACKEND_WASAPI  0
#endif

#if AUDIO_BACKEND_WASAPI
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <mmreg.h>
  #include <mmdeviceapi.h>
  #include <audioclient.h>
  #include <process.h>

  /* Define WASAPI COM GUIDs (avoids needing uuid.lib linkage for these) */
  #ifdef __cplusplus
    #define GUID_SECT
  #else
    #define GUID_SECT
  #endif
  #ifndef DEFINE_AUDIO_GUID
    #ifdef INITGUID
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #else
      #define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
              const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
    #endif
  #endif
  DEFINE_AUDIO_GUID(ps3r_CLSID_MMDeviceEnumerator, 0xBCDE0395,0xE52F,0x467C,0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E);
  DEFINE_AUDIO_GUID(ps3r_IID_IMMDeviceEnumerator,  0xA95664D2,0x9614,0x4F35,0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioClient,         0x1CB9AD4C,0xDBFA,0x4c32,0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioRenderClient,   0xF294ACFC,0x3146,0x4483,0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2);
  DEFINE_AUDIO_GUID(ps3r_IID_IAudioClient2,        0x726778CD,0xF60A,0x4EDA,0x82,0xDE,0xE4,0x76,0x10,0xCD,0x78,0xAA);
  DEFINE_AUDIO_GUID(ps3r_IID_IMMNotificationClient,0x7991EEC9,0x7E89,0x4D85,0x83,0x90,0x6C,0x70,0x3C,0xEC,0x60,0xC0);
  DEFINE_AUDIO_GUID(ps3r_IID_IUnknown,             0x00000000,0x0000,0x0000,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46);
  DEFINE_AUDIO_GUID(ps3r_SUBTYPE_IEEE_FLOAT,       0x00000003,0x0000,0x0010,0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71);
  static const PROPERTYKEY ps3r_PKEY_Device_FriendlyName =
      { { 0xA45C254E, 0xDF1C, 0x4EFD, { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } }, 14 };
  static const PROPERTYKEY ps3r_PKEY_RawProcessingSupported =
      { { 0x8943B373, 0x388C, 0x4395, { 0xB5, 0x57, 0xBC, 0x6D, 0xBA, 0xFF, 0xAF, 0xDB } }, 2 };
  /* The speakers of each layout (ksmedia.h's KSAUDIO_SPEAKER_*): the interleaved order is the bits' order. */
  #define PS3R_SPK_STEREO        0x003u   /* FL FR */
  #define PS3R_SPK_5POINT1_BACK  0x03Fu   /* FL FR FC LFE BL BR */
  #define PS3R_SPK_5POINT1_SIDE  0x60Fu   /* FL FR FC LFE SL SR */
  #define PS3R_SPK_7POINT1       0x63Fu   /* FL FR FC LFE BL BR SL SR */
  #ifndef STGM_READ
    #define STGM_READ 0x00000000L
  #endif

  /* Map to the names used in the code */
  #define IID_IMMDeviceEnumerator  ps3r_IID_IMMDeviceEnumerator
  #define CLSID_MMDeviceEnumerator ps3r_CLSID_MMDeviceEnumerator
  #define IID_IAudioClient         ps3r_IID_IAudioClient
  #define IID_IAudioRenderClient   ps3r_IID_IAudioRenderClient
#endif

#if AUDIO_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* Portable threading */
#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE thread_t;
  typedef CRITICAL_SECTION mutex_t;
  #define mutex_init(m)    InitializeCriticalSection(m)
  #define mutex_destroy(m) DeleteCriticalSection(m)
  #define mutex_lock(m)    EnterCriticalSection(m)
  #define mutex_unlock(m)  LeaveCriticalSection(m)
#else
  #include <pthread.h>
  #include <unistd.h>
  #include <time.h>
  typedef pthread_t thread_t;
  typedef pthread_mutex_t mutex_t;
  #define mutex_init(m)    pthread_mutex_init(m, NULL)
  #define mutex_destroy(m) pthread_mutex_destroy(m)
  #define mutex_lock(m)    pthread_mutex_lock(m)
  #define mutex_unlock(m)  pthread_mutex_unlock(m)
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

/* Per-port audio buffer allocated on the host side.
 * In a full emulator these would be in guest VM memory; here we allocate
 * host buffers and expose their addresses through portAddr/readIndexAddr. */
#define AUDIO_PORT_BUF_MAX  (CELL_AUDIO_BLOCK_32 * CELL_AUDIO_BLOCK_SAMPLES * CELL_AUDIO_PORT_8CH)

typedef struct {
    int                  in_use;
    int                  running;
    CellAudioPortParam   param;
    float*               buffer;        /* host audio buffer (float samples) */
    u32                  buf_size;       /* buffer size in bytes */
    u64                  read_index;     /* current read position (block index) */
    u64                  write_index;    /* where the game is writing */
    /* For address reporting to guest */
    u64                  port_addr;      /* guest-visible buffer address */
    u64                  read_idx_addr;  /* guest-visible read index address */
} AudioPortSlot;

static int            s_audio_initialized = 0;
static AudioPortSlot  s_ports[CELL_AUDIO_PORT_MAX];

/* Event queue notification */
typedef struct {
    int  in_use;
    u64  key;
} AudioNotifySlot;
static AudioNotifySlot s_notify_queues[CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES];

/* Timestamp anchor for cellAudioGetPortTimestamp (microseconds, set at init) */
static u64            s_audio_start_us = 0;

/* Mixing thread */
static unsigned long long g_audio_dropped;   /* frames the device had no room for */
static volatile int  s_mix_thread_running = 0;
static thread_t      s_mix_thread;
#ifndef _WIN32
/* Is s_mix_thread a thread that exists and has not been joined? The Win32
 * branch asks that of the HANDLE itself, which it nulls after closing;
 * pthread_t has no such value, so carry the flag. */
static int           s_mix_thread_live = 0;
#endif
static mutex_t       s_audio_mutex;

/* Output mix buffer: one block, interleaved in the output's s_out_ch channels (2, 6 or 8; audio_mix.h's order).
 * Only the mixing thread changes s_out_ch, between blocks, when it opens a device. */
static float s_mix_buffer[CELL_AUDIO_BLOCK_SAMPLES * 8];
static u32   s_out_ch = 2;

/* ---------------------------------------------------------------------------
 * Host audio output backend
 * -----------------------------------------------------------------------*/

#if AUDIO_BACKEND_SDL2

static SDL_AudioDeviceID s_sdl_audio_dev = 0;

static int audio_backend_init(void)
{
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            printf("[cellAudio] SDL audio init failed: %s\n", SDL_GetError());
            return -1;
        }
    }

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq     = CELL_AUDIO_SAMPLE_RATE;
    want.format   = AUDIO_F32SYS;
    want.channels = 2;
    want.samples  = CELL_AUDIO_BLOCK_SAMPLES;
    want.callback = NULL; /* We'll use SDL_QueueAudio */

    s_sdl_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (s_sdl_audio_dev == 0) {
        printf("[cellAudio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_PauseAudioDevice(s_sdl_audio_dev, 0); /* Start playback */
    return 0;
}

static void audio_backend_shutdown(void)
{
    if (s_sdl_audio_dev) {
        SDL_CloseAudioDevice(s_sdl_audio_dev);
        s_sdl_audio_dev = 0;
    }
}

static void audio_backend_submit(const float* stereo_samples, u32 num_samples)
{
    if (s_sdl_audio_dev) {
        SDL_QueueAudio(s_sdl_audio_dev, stereo_samples,
                       num_samples * 2 * sizeof(float));
    }
}

/* SDL queues without bound: keep about four blocks ahead of the device. */
static int audio_backend_room(void)
{
    if (!s_sdl_audio_dev) return -1;
    return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float)) <
           CELL_AUDIO_BLOCK_SAMPLES * 4 ? CELL_AUDIO_BLOCK_SAMPLES : 0;
}

static void audio_backend_wait(void) { SDL_Delay(1); }

static u32 audio_backend_queued_samples(void)
{
    if (s_sdl_audio_dev) {
        return SDL_GetQueuedAudioSize(s_sdl_audio_dev) / (2 * sizeof(float));
    }
    return 0;
}

/* SDL's stream is stereo. */
u32 cellAudio_output_channels(void) { return 2; }

#endif /* AUDIO_BACKEND_SDL2 */

#if AUDIO_BACKEND_WASAPI

static IMMDeviceEnumerator* s_wasapi_enum   = NULL;
static IAudioClient*        s_wasapi_client = NULL;
static IAudioRenderClient*  s_wasapi_render = NULL;
static HANDLE               s_wasapi_event  = NULL;
static UINT32               s_wasapi_buf_frames = 0;
/* Set when the default output device changes (the notification client below), or when the stream is lost
 * (AUDCLNT_E_DEVICE_INVALIDATED: the device unplugged, disabled or given another format): the mixing thread
 * then opens the default device again (PS3_AUDIO_REOPEN, on by default). Without it the stream stayed on a
 * device that was gone and the game went silent until it was restarted. */
static volatile LONG        s_wasapi_reopen = 0;
static int                  s_wasapi_notifying = 0;

/* PS3_AUDIO_CHANNELS: 0 for auto (the device's own layout), else 2, 6 or 8. */
static u32 audio_channels_wanted(void)
{
    const char* e = getenv("PS3_AUDIO_CHANNELS");
    if (!e || !*e || !_stricmp(e, "auto")) return 0;
    if (!_stricmp(e, "stereo") || !strcmp(e, "2")) return 2;
    if (!strcmp(e, "5.1") || !strcmp(e, "6")) return 6;
    if (!strcmp(e, "7.1") || !strcmp(e, "8")) return 8;
    static int said;
    if (!said++) fprintf(stderr, "[cellAudio] PS3_AUDIO_CHANNELS=%s is not auto, stereo, 5.1 or 7.1: auto\n", e);
    return 0;
}

static int audio_env_on(const char* name, int def)
{
    const char* e = getenv(name);
    if (!e || !*e) return def;
    return strcmp(e, "0") != 0 && _stricmp(e, "off") != 0;
}

/* What auto takes for a device of ch channels: 7.1, 5.1 or stereo. */
static u32 audio_channels_for(u32 ch) { return ch >= 8 ? 8 : ch >= 6 ? 6 : 2; }

/* A device's mix format: its channels and their speakers. */
static void wasapi_device_layout(IMMDevice* dev, u32* ch, DWORD* mask)
{
    *ch = 2;
    *mask = PS3R_SPK_STEREO;
    IAudioClient* c = NULL;
    if (FAILED(dev->lpVtbl->Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&c)) || !c) return;
    WAVEFORMATEX* mix = NULL;
    if (SUCCEEDED(c->lpVtbl->GetMixFormat(c, &mix)) && mix) {
        *ch = mix->nChannels;
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22)
            *mask = ((const WAVEFORMATEXTENSIBLE*)mix)->dwChannelMask;
        CoTaskMemFree(mix);
    }
    c->lpVtbl->Release(c);
}

/* A device's name, for the log, and whether it has a raw path (no effects at all). */
static void wasapi_device_info(IMMDevice* dev, char* name, int cap, int* raw)
{
    name[0] = 0;
    *raw = 0;
    IPropertyStore* ps = NULL;
    if (FAILED(dev->lpVtbl->OpenPropertyStore(dev, STGM_READ, &ps)) || !ps) return;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(ps->lpVtbl->GetValue(ps, &ps3r_PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
        WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, name, cap, NULL, NULL);
    PropVariantClear(&v);
    PropVariantInit(&v);
    if (SUCCEEDED(ps->lpVtbl->GetValue(ps, &ps3r_PKEY_RawProcessingSupported, &v)) && v.vt == VT_BOOL)
        *raw = v.boolVal != VARIANT_FALSE;
    PropVariantClear(&v);
    ps->lpVtbl->Release(ps);
}

/* The stream: float at 48 kHz in ch channels. Stereo keeps the plain format it always had. */
static void wasapi_format(WAVEFORMATEXTENSIBLE* f, u32 ch, DWORD mask)
{
    memset(f, 0, sizeof *f);
    f->Format.wFormatTag      = ch == 2 ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_EXTENSIBLE;
    f->Format.nChannels       = (WORD)ch;
    f->Format.nSamplesPerSec  = CELL_AUDIO_SAMPLE_RATE;
    f->Format.wBitsPerSample  = 32;
    f->Format.nBlockAlign     = (WORD)(ch * 4);
    f->Format.nAvgBytesPerSec = CELL_AUDIO_SAMPLE_RATE * ch * 4;
    if (ch != 2) {
        f->Format.cbSize = 22;
        f->Samples.wValidBitsPerSample = 32;
        f->dwChannelMask = mask;
        f->SubFormat = ps3r_SUBTYPE_IEEE_FLOAT;
    }
}

/* Before Initialize: the stream's category, a game's (AudioCategory_GameEffects: its whole mix, not the
 * background music GameMedia is), so Windows and the audio suites that pick a profile by it treat it as a
 * game; and raw, past the effects. Bit 0: the category set, bit 1: raw taken. */
static int wasapi_properties(IAudioClient* c, int category, int raw)
{
    if (!category && !raw) return 0;
    IAudioClient2* c2 = NULL;
    if (FAILED(c->lpVtbl->QueryInterface(c, &ps3r_IID_IAudioClient2, (void**)&c2)) || !c2) return 0;
    AudioClientProperties p;
    memset(&p, 0, sizeof p);
    p.cbSize = sizeof p;
    p.bIsOffload = FALSE;
    p.eCategory = category ? AudioCategory_GameEffects : AudioCategory_Other;
    p.Options = raw ? AUDCLNT_STREAMOPTIONS_RAW : AUDCLNT_STREAMOPTIONS_NONE;
    HRESULT hr = c2->lpVtbl->SetClientProperties(c2, &p);
    if (FAILED(hr) && raw) {
        p.Options = AUDCLNT_STREAMOPTIONS_NONE;
        raw = 0;
        hr = c2->lpVtbl->SetClientProperties(c2, &p);
    }
    c2->lpVtbl->Release(c2);
    return SUCCEEDED(hr) ? (category ? 1 : 0) | (raw ? 2 : 0) : 0;
}

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM      0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

/* One try at a stream on dev, in ch channels; on success it is the backend's. */
static HRESULT wasapi_try(IMMDevice* dev, u32 ch, DWORD mask, int category, int raw, int* props)
{
    IAudioClient* c = NULL;
    HRESULT hr = dev->lpVtbl->Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&c);
    if (FAILED(hr) || !c) return FAILED(hr) ? hr : E_FAIL;
    *props = wasapi_properties(c, category, raw);

    WAVEFORMATEXTENSIBLE f;
    wasapi_format(&f, ch, mask);
    /* Event-driven, 60 ms by default (PS3_AUDIO_BUF_MS). The old 20 ms ran dry whenever the mixing thread
     * lost the CPU for a moment -- with every core busy running SPU/PPU code that was often, and each time is
     * an audible stutter (Bink cutscenes most of all). */
    REFERENCE_TIME dur = 600000;   /* 60 ms in 100 ns units */
    { const char* e = getenv("PS3_AUDIO_BUF_MS"); const int ms = e ? atoi(e) : 0;
      if (ms >= 10 && ms <= 500) dur = (REFERENCE_TIME)ms * 10000; }
    /* Shared mode only takes the device's own mix format unless the stream asks the audio engine to convert:
     * on a device not running 48 kHz stereo float (a 44.1 kHz or 5.1 output) Initialize failed with
     * AUDCLNT_E_UNSUPPORTED_FORMAT and every title ran silent. The conversion also folds a layout the device
     * lacks down to the one it has: 7.1 to a laptop's stereo. */
    const DWORD convert = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = c->lpVtbl->Initialize(c, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | convert,
                               dur, 0, &f.Format, NULL);
    if (FAILED(hr)) {
        /* Fallback: without the event callback */
        hr = c->lpVtbl->Initialize(c, AUDCLNT_SHAREMODE_SHARED, convert, dur, 0, &f.Format, NULL);
        if (ev) CloseHandle(ev);
        ev = NULL;
    } else {
        c->lpVtbl->SetEventHandle(c, ev);
    }
    IAudioRenderClient* rc = NULL;
    if (SUCCEEDED(hr)) hr = c->lpVtbl->GetService(c, &IID_IAudioRenderClient, (void**)&rc);
    if (FAILED(hr)) {
        if (ev) CloseHandle(ev);
        c->lpVtbl->Release(c);
        return hr;
    }
    UINT32 frames = 0;
    c->lpVtbl->GetBufferSize(c, &frames);
    s_wasapi_client = c;
    s_wasapi_render = rc;
    s_wasapi_event = ev;
    s_wasapi_buf_frames = frames;
    s_out_ch = ch;
    c->lpVtbl->Start(c);
    return S_OK;
}

static void wasapi_close(void)
{
    if (s_wasapi_client) s_wasapi_client->lpVtbl->Stop(s_wasapi_client);
    if (s_wasapi_render) { s_wasapi_render->lpVtbl->Release(s_wasapi_render); s_wasapi_render = NULL; }
    if (s_wasapi_client) { s_wasapi_client->lpVtbl->Release(s_wasapi_client); s_wasapi_client = NULL; }
    if (s_wasapi_event) { CloseHandle(s_wasapi_event); s_wasapi_event = NULL; }
    s_wasapi_buf_frames = 0;
}

/* The default output device's stream, in the layout PS3_AUDIO_CHANNELS asks for, else the device's own. A
 * multichannel stream the device refuses falls back to stereo. why: the log's word. 0 on success. */
static int wasapi_open(const char* why)
{
    if (!s_wasapi_enum) return -1;
    IMMDevice* dev = NULL;
    HRESULT hr = s_wasapi_enum->lpVtbl->GetDefaultAudioEndpoint(s_wasapi_enum, eRender, eConsole, &dev);
    if (FAILED(hr) || !dev) {
        printf("[cellAudio] Failed to get default audio endpoint: 0x%08lX\n", hr);
        return -1;
    }
    char name[200];
    int raw_ok = 0;
    wasapi_device_info(dev, name, (int)sizeof name, &raw_ok);
    u32 dev_ch;
    DWORD dev_mask;
    wasapi_device_layout(dev, &dev_ch, &dev_mask);
    const u32 want = audio_channels_wanted();
    u32 ch = want ? want : audio_channels_for(dev_ch);
    DWORD mask = ch == 8 ? PS3R_SPK_7POINT1
               : ch == 6 ? (dev_mask == PS3R_SPK_5POINT1_BACK ? PS3R_SPK_5POINT1_BACK : PS3R_SPK_5POINT1_SIDE)
               : PS3R_SPK_STEREO;
    const int category = audio_env_on("PS3_AUDIO_CATEGORY", 1);
    const int raw = audio_env_on("PS3_AUDIO_RAW", 0);
    int props = 0;
    hr = wasapi_try(dev, ch, mask, category, raw && raw_ok, &props);
    if (FAILED(hr) && ch != 2) {
        fprintf(stderr, "[cellAudio] WASAPI: a %u-channel stream refused (0x%08lX), stereo instead\n", ch,
                (unsigned long)hr);
        ch = 2;
        mask = PS3R_SPK_STEREO;
        hr = wasapi_try(dev, ch, mask, category, raw && raw_ok, &props);
    }
    dev->lpVtbl->Release(dev);
    if (FAILED(hr)) {
        printf("[cellAudio] WASAPI Initialize failed: 0x%08lX\n", hr);
        return -1;
    }
    const char* fx = props & 2 ? "raw, past the effects"
                   : !raw ? "the effects on"
                   : raw_ok ? "raw refused, the effects stay"
                   : "raw asked, but the device has no raw path: the effects stay";
    fprintf(stderr, "[cellAudio] WASAPI %s: \"%s\", %u channels (speakers 0x%lX); the stream %s, 48 kHz float, "
                    "%u frames; category %s; %s\n",
            why, name, dev_ch, (unsigned long)dev_mask, ch == 8 ? "7.1" : ch == 6 ? "5.1" : "stereo",
            s_wasapi_buf_frames, props & 1 ? "game" : "none", fx);
    return 0;
}

/* The notification client: the console role's default render device changed. Windows calls it on a thread
 * of its own; it only sets the flag the mixing thread reads. A static object: AddRef and Release count
 * nothing. */
static HRESULT STDMETHODCALLTYPE nc_query(IMMNotificationClient* self, REFIID riid, void** out)
{
    if (!out) return E_POINTER;
    if (IsEqualGUID(riid, &ps3r_IID_IUnknown) || IsEqualGUID(riid, &ps3r_IID_IMMNotificationClient)) {
        *out = self;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE nc_ref(IMMNotificationClient* self) { (void)self; return 1; }
static HRESULT STDMETHODCALLTYPE nc_state(IMMNotificationClient* self, LPCWSTR id, DWORD state)
{
    (void)self; (void)id; (void)state;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE nc_device(IMMNotificationClient* self, LPCWSTR id)
{
    (void)self; (void)id;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE nc_default(IMMNotificationClient* self, EDataFlow flow, ERole role, LPCWSTR id)
{
    (void)self; (void)id;
    if (flow == eRender && role == eConsole) InterlockedExchange(&s_wasapi_reopen, 1);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE nc_prop(IMMNotificationClient* self, LPCWSTR id, const PROPERTYKEY key)
{
    (void)self; (void)id; (void)key;
    return S_OK;
}
static IMMNotificationClientVtbl s_nc_vtbl = { nc_query, nc_ref, nc_ref, nc_state, nc_device, nc_device,
                                               nc_default, nc_prop };
static IMMNotificationClient s_nc = { &s_nc_vtbl };

static int audio_backend_init(void)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        printf("[cellAudio] CoInitializeEx failed: 0x%08lX\n", hr);
        return -1;
    }
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void**)&s_wasapi_enum);
    if (FAILED(hr) || !s_wasapi_enum) {
        printf("[cellAudio] Failed to create device enumerator: 0x%08lX\n", hr);
        s_wasapi_enum = NULL;
        return -1;
    }
    if (audio_env_on("PS3_AUDIO_REOPEN", 1) &&
        SUCCEEDED(s_wasapi_enum->lpVtbl->RegisterEndpointNotificationCallback(s_wasapi_enum, &s_nc)))
        s_wasapi_notifying = 1;
    if (wasapi_open("opened") != 0) {
        /* No output device yet: the mixing thread tries again every second, as for a lost one. */
        InterlockedExchange(&s_wasapi_reopen, 1);
        return -1;
    }
    return 0;
}

static void audio_backend_shutdown(void)
{
    wasapi_close();
    if (s_wasapi_enum) {
        if (s_wasapi_notifying)
            s_wasapi_enum->lpVtbl->UnregisterEndpointNotificationCallback(s_wasapi_enum, &s_nc);
        s_wasapi_notifying = 0;
        s_wasapi_enum->lpVtbl->Release(s_wasapi_enum);
        s_wasapi_enum = NULL;
    }
}

/* A lost device's errors: the mixing thread opens the default one again. */
static void wasapi_note_error(HRESULT hr)
{
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING)
        InterlockedExchange(&s_wasapi_reopen, 1);
}

static void audio_backend_submit(const float* samples, u32 num_samples)
{
    if (!s_wasapi_render) return;

    UINT32 padding = 0;
    HRESULT hr = s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);
    if (FAILED(hr)) { wasapi_note_error(hr); return; }

    UINT32 available = s_wasapi_buf_frames - padding;
    if (num_samples > available) {
        g_audio_dropped += num_samples - available;
        num_samples = available;
    }
    if (num_samples == 0) return;

    BYTE* buf = NULL;
    hr = s_wasapi_render->lpVtbl->GetBuffer(s_wasapi_render, num_samples, &buf);
    if (SUCCEEDED(hr) && buf) {
        /* PS3_MUTE=1: silence at the device, nothing else changes -- the
         * title still gets a working cellAudio and the device still paces
         * it, unlike PS3_NO_AUDIO, which sends the title down its no-audio
         * path. For unattended runs. */
        static int mute = -1;
        if (mute < 0) mute = getenv("PS3_MUTE") ? 1 : 0;
        memcpy(buf, samples, num_samples * s_out_ch * sizeof(float));
        s_wasapi_render->lpVtbl->ReleaseBuffer(s_wasapi_render, num_samples,
                                               mute ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
    } else if (FAILED(hr)) {
        wasapi_note_error(hr);
    }
}

/* Frames the device can take right now; -1 with no device open. */
static int audio_backend_room(void)
{
    if (!s_wasapi_client) return -1;
    UINT32 padding = 0;
    const HRESULT hr = s_wasapi_client->lpVtbl->GetCurrentPadding(s_wasapi_client, &padding);
    if (FAILED(hr)) {
        /* Device gone (AUDCLNT_E_DEVICE_INVALIDATED on a default-device
         * change, sleep, ...): pace on the clock until it is open again. */
        wasapi_note_error(hr);
        static int _n = 0; if (PS3_LOG_FIRST(_n, 4))
            fprintf(stderr, "[cellAudio] GetCurrentPadding failed 0x%08lX -- clock pacing%c", (unsigned long)hr, 10);
        return -1;
    }
    return (int)(s_wasapi_buf_frames - padding);
}

/* Block until the device has consumed some of its buffer. */
static void audio_backend_wait(void)
{
    if (s_wasapi_event) WaitForSingleObject(s_wasapi_event, 10);
    else Sleep(1);
}

u32 cellAudio_output_channels(void)
{
    if (s_wasapi_client) return s_out_ch;
    /* Before the stream: what it will be. cellAudioOut is asked from the title's own threads, before
     * cellAudioInit; COM for the question alone. */
    static u32 probed = 0;
    if (!probed) {
        const u32 want = audio_channels_wanted();
        u32 ch = 2;
        if (!want) {
            const HRESULT hi = CoInitializeEx(NULL, COINIT_MULTITHREADED);
            IMMDeviceEnumerator* en = NULL;
            if (SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                           &IID_IMMDeviceEnumerator, (void**)&en)) && en) {
                IMMDevice* dev = NULL;
                if (SUCCEEDED(en->lpVtbl->GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)) && dev) {
                    u32 dch;
                    DWORD dmask;
                    wasapi_device_layout(dev, &dch, &dmask);
                    ch = audio_channels_for(dch);
                    dev->lpVtbl->Release(dev);
                }
                en->lpVtbl->Release(en);
            }
            if (SUCCEEDED(hi)) CoUninitialize();
        }
        probed = want ? want : ch;
    }
    return probed;
}

#endif /* AUDIO_BACKEND_WASAPI */

/* ---------------------------------------------------------------------------
 * Mixing
 * -----------------------------------------------------------------------*/

/* Guest PCM is BIG-ENDIAN float32; the port buffer is a raw vm_base view, so
 * every sample must be byte-swapped before use (audio_be_f32, audio_mix.h).
 * Reading it as a host float turned the whole mix into denormal noise --
 * silence after clipping (LBP had a verified end-to-end pipeline with no
 * audible output). */

/* AUDIO_RATE's ring counters, over the running ports: blocks read all zero
 * inside sound (a run of at most AUDIO_GAP_MAX between blocks with data -- the
 * producer was late, not quiet), blocks written after they were read (seen
 * non-zero again one block later), and how many blocks past the one read
 * already held data (0-3, 3 = 3 or more). */
#define AUDIO_GAP_MAX 4
static unsigned s_ar_gaps, s_ar_gap_blocks, s_ar_late, s_ar_lead[4];
/* And the level of each channel: of the 8-channel ports as the title writes them (its order: L R C LFE, the
 * surrounds, the rears), and of the output as it goes to the device (its layout's order), sums of squares. */
static double s_ar_port_sq[8], s_ar_out_sq[8];
static unsigned long long s_ar_port_n, s_ar_out_n;

static int audio_block_has_data(const float* b, u32 nch)
{
    const u32* w = (const u32*)b;
    for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * nch; i++)
        if (w[i]) return 1;
    return 0;
}

static void audio_ring_note(int p, const AudioPortSlot* port, const float* src, u32 nch, u32 nblock)
{
    static u32 zrun[CELL_AUDIO_PORT_MAX];
    static u8 sound[CELL_AUDIO_PORT_MAX];
    const u32 bsz = CELL_AUDIO_BLOCK_SAMPLES * nch;
    const u32 r = (u32)(port->read_index % nblock);
    const u32 prev = (r + nblock - 1) % nblock;
    if (port->read_index > 0 && audio_block_has_data(port->buffer + prev * bsz, nch)) s_ar_late++;
    u32 lead = 0;
    while (lead < 3 && lead + 1 < nblock &&
           audio_block_has_data(port->buffer + ((r + lead + 1) % nblock) * bsz, nch)) lead++;
    s_ar_lead[lead]++;
    if (nch == 8) {
        for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++)
            for (u32 c = 0; c < 8; c++) {
                const double v = audio_be_f32(&src[s * 8 + c]);
                s_ar_port_sq[c] += v * v;
            }
        s_ar_port_n += CELL_AUDIO_BLOCK_SAMPLES;
    }
    if (!audio_block_has_data(src, nch)) { zrun[p]++; return; }
    if (sound[p] && zrun[p] > 0 && zrun[p] <= AUDIO_GAP_MAX) { s_ar_gaps++; s_ar_gap_blocks += zrun[p]; }
    zrun[p] = 0; sound[p] = 1;
}

static int audio_rate_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("AUDIO_RATE") ? 1 : 0;
    return on;
}

/* Mix one block from all active ports into s_mix_buffer, in the output's channels */
static void audio_mix_one_block(void)
{
    const u32 out_ch = s_out_ch;
    memset(s_mix_buffer, 0, sizeof(s_mix_buffer));

    mutex_lock(&s_audio_mutex);

    for (int p = 0; p < CELL_AUDIO_PORT_MAX; p++) {
        AudioPortSlot* port = &s_ports[p];
        /* Diagnostic: a port that HOLDS PCM but was never started is inaudible
         * by design -- flag it once so "no PortStart" vs "no data" is decidable
         * from the log (LBP's Bink movie audio port sat exactly there). */
        if (port->in_use && !port->running && port->buffer) {
            static u8 warned[CELL_AUDIO_PORT_MAX];
            if (!warned[p]) {
                u32 probe = (u32)(port->param.nBlock * CELL_AUDIO_BLOCK_SAMPLES *
                                  port->param.nChannel);
                int nz = 0;
                for (u32 s = 0; s < probe; s += 64) if (port->buffer[s] != 0.0f) { nz = 1; break; }
                if (nz) {
                    warned[p] = 1;
                    fprintf(stderr, "[cellAudio] port %d HAS DATA but never started"
                            " -- game withheld PortStart\n", p);
                }
            }
        }
        if (!port->in_use || !port->running || !port->buffer)
            continue;

        u32 nch    = (u32)port->param.nChannel;
        u32 nblock = (u32)port->param.nBlock;
        float level = port->param.level;
        if (level <= 0.0f) level = 1.0f;
        if (level > 1.0f) level = 1.0f;

        /* Read one block at the current read_index */
        u32 block_idx = (u32)(port->read_index % nblock);
        u32 block_offset = block_idx * CELL_AUDIO_BLOCK_SAMPLES * nch;
        float* src = port->buffer + block_offset;
        if (audio_rate_on()) audio_ring_note(p, port, src, nch, nblock);

        /* AUDIO_PEAK diag: which blocks of this port's WHOLE ring hold data,
         * vs which block the read cursor is on -- distinguishes "writer never
         * writes" from "writer and reader cycle out of phase". */
        { static int s_pk = -1; if (s_pk < 0) s_pk = getenv("AUDIO_PEAK") ? 1 : 0;
          if (s_pk) { static _Thread_local unsigned _c;
            if ((++_c % 400) == 0) {
                char map[36]; u32 b;
                for (b = 0; b < nblock && b < 32; b++) {
                    const float* bp = port->buffer + b * CELL_AUDIO_BLOCK_SAMPLES * nch;
                    int nz = 0;
                    for (u32 s2 = 0; s2 < CELL_AUDIO_BLOCK_SAMPLES * nch; s2 += 16)
                        if (bp[s2] != 0.0f) { nz = 1; break; }
                    map[b] = nz ? '#' : '.';
                }
                map[b] = 0;
                fprintf(stderr, "[audio-ring] port %d ridx=%llu blk=%u ring=[%s]\n",
                        p, (unsigned long long)port->read_index, block_idx, map);
            } } }

        /* Mono to both front channels; a 7.1 port to 7.1 or 5.1 as it is, or to stereo (audio_mix.h). */
        audio_mix_port(s_mix_buffer, out_ch, src, nch, CELL_AUDIO_BLOCK_SAMPLES, level);

        /* Firmware clears a ring-buffer block after consuming it. Besides
         * preventing stale PCM from replaying when a producer stalls, the
         * cleared block is an observable consumption signal for producers. */
        memset(src, 0, CELL_AUDIO_BLOCK_SAMPLES * nch * sizeof(float));

        /* Keep the monotonic counter for tags/timestamps, but publish the
         * ring slot to the guest. SurMixer multiplies this value by the block
         * size directly; an unbounded counter walks beyond the audio buffer. */
        port->read_index++;
        if (port->read_idx_addr)
            vm_write64((u32)port->read_idx_addr, port->read_index % nblock);
    }

    mutex_unlock(&s_audio_mutex);

    /* Clip to [-1.0, 1.0] */
    for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * out_ch; i++) {
        if (s_mix_buffer[i] > 1.0f) s_mix_buffer[i] = 1.0f;
        if (s_mix_buffer[i] < -1.0f) s_mix_buffer[i] = -1.0f;
    }
}

/* ---------------------------------------------------------------------------
 * Mixing thread
 * -----------------------------------------------------------------------*/

static void audio_notify_event_queues(void)
{
    /* Push the audio-period event (data1 = CELL_AUDIO_EVENT_MIX = 0) to each
     * registered notify queue, so the game's audio loop (blocked on
     * sys_event_queue_receive) wakes once per block and writes the next one.
     * Resolve the queue by its ipc_key (set via cellAudioSetNotifyEventQueue). */
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) continue;
        uint32_t qid = sys_event_find_queue_by_key(s_notify_queues[i].key);
        if (qid)
            sys_event_queue_push_by_id(qid, s_notify_queues[i].key,
                                       0 /*CELL_AUDIO_EVENT_MIX*/, 0, 0);
    }
}

#ifdef _WIN32
static unsigned long long g_audio_underruns;

/* Take a block every 5.33 ms by the performance counter, as the hardware
 * does, and keep the device queue near AUDIO_PACE_MS (default 30) as the
 * jitter buffer. Paced on the device's room alone the blocks went in pairs:
 * shared-mode WASAPI frees about 10 ms at a time, so two were read at once,
 * and a guest mixer one block ahead found the second already due. God of War:
 * Ascension's first level: short gaps inside sound 27-35 per 5 s -> 8-9.
 * AUDIO_PACE=0 paces on the device's room again. */
static int audio_pace_ms(void)
{
    static int ms = -1;
    if (ms < 0) {
        const char* e = getenv("AUDIO_PACE");
        const char* t = getenv("AUDIO_PACE_MS");
        ms = (!e || atoi(e) != 0) ? (t ? atoi(t) : 30) : 0;
        if (ms && ms < 15) ms = 15;
    }
    return ms;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* Sleep until the performance counter reaches `due`: to ~0.5 ms with the
 * high-resolution waitable timer (Windows 10 1803 on), else Sleep(1). */
static void audio_sleep_until(long long due, long long freq)
{
    static HANDLE timer = (HANDLE)-1;
    if (timer == (HANDLE)-1)
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const long long left = due - now.QuadPart;
    if (left <= 0) return;
    if (timer) {
        LARGE_INTEGER rel; rel.QuadPart = -(left * 10000000 / freq);
        if (!rel.QuadPart) rel.QuadPart = -1;
        if (SetWaitableTimer(timer, &rel, 0, NULL, NULL, FALSE)) { WaitForSingleObject(timer, 20); return; }
    }
    Sleep(1);
}

static unsigned __stdcall audio_mix_thread_func(void* arg)
{
    (void)arg;
    ps3_thread_name("audio mix");
    printf("[cellAudio] Mixing thread started\n");

    /* The device deadline is every few ms and the guest threads keep every
     * core busy: run this one ahead of them (MMCSS "Pro Audio" when the
     * service is there, the highest normal priority anyway). avrt.dll is
     * loaded by hand so the runtime does not need avrt.lib to link. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    { HMODULE avrt = LoadLibraryW(L"avrt.dll");
      typedef HANDLE (WINAPI *set_mm_fn)(LPCWSTR, LPDWORD);
      set_mm_fn set_mm = avrt ? (set_mm_fn)(void*)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW") : NULL;
      DWORD task = 0;
      if (set_mm && !set_mm(L"Pro Audio", &task))
          fprintf(stderr, "[cellAudio] MMCSS Pro Audio unavailable (%lu)%c", (unsigned long)GetLastError(), 10); }

    /* Pace to the device clock. The PS3 consumes one block every 5.33 ms;
     * the old Sleep(2)/Sleep(5) loop keyed on a queue depth the 1056-frame
     * WASAPI buffer can never reach, so it mixed ~225 blocks/s and the device
     * dropped the surplus: 20% of the audio thrown away, and a guest mixer
     * (GH3's FMOD) that could not keep ahead of the read index played gaps.
     * With no device, pace on the performance counter instead. */
    LARGE_INTEGER qf, q0; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    unsigned long long blocks = 0;
    const int pace_ms = audio_pace_ms();
    const long long period = qf.QuadPart * CELL_AUDIO_BLOCK_SAMPLES / CELL_AUDIO_SAMPLE_RATE;
    long long pace_t0 = 0;              /* when block 0 of the paced clock was due */
    unsigned long long pace_k = 0;      /* blocks taken on it */
    if (pace_ms) fprintf(stderr, "[cellAudio] a block every 5.33 ms, device queue kept near %d ms%c", pace_ms, 10);
#if AUDIO_BACKEND_WASAPI
    /* This thread opens the device again when it changes or is lost: COM for it here. */
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const int reopen_on = audio_env_on("PS3_AUDIO_REOPEN", 1);
    ULONGLONG reopen_retry = 0, reopen_test = 0;
    unsigned reopen_fails = 0;
    { const char* e = getenv("PS3_AUDIO_REOPEN_AT");
      if (e && atoi(e) > 0) reopen_test = GetTickCount64() + (ULONGLONG)atoi(e) * 1000; }
#endif
    while (s_mix_thread_running) {
#if AUDIO_BACKEND_WASAPI
        if (reopen_test && GetTickCount64() >= reopen_test) {
            reopen_test = 0;
            fprintf(stderr, "[cellAudio] PS3_AUDIO_REOPEN_AT: opening the device again, as a change of device would\n");
            InterlockedExchange(&s_wasapi_reopen, 1);
        }
        if (s_wasapi_reopen && reopen_on && GetTickCount64() >= reopen_retry) {
            /* Between blocks: the stream, its channels and the pacing start again on the new device. */
            InterlockedExchange(&s_wasapi_reopen, 0);
            const u32 was = s_out_ch;
            wasapi_close();
            if (wasapi_open("opened again") == 0) {
                reopen_fails = 0;
                pace_t0 = 0;
                pace_k = 0;
                blocks = 0;
                QueryPerformanceCounter(&q0);
                if (s_out_ch != was && getenv("AUDIO_WAV"))
                    fprintf(stderr, "[cellAudio] AUDIO_WAV: %u channels from here on\n", s_out_ch);
            } else {
                InterlockedExchange(&s_wasapi_reopen, 1);
                reopen_retry = GetTickCount64() + 1000;
                if (reopen_fails++ < 3)
                    fprintf(stderr, "[cellAudio] no output device to open: trying again every second\n");
            }
        }
#endif
        const int room = audio_backend_room();
        if (room >= 0 && room < CELL_AUDIO_BLOCK_SAMPLES) { audio_backend_wait(); continue; }
        if (pace_ms && room >= 0) {
            const UINT32 queued = s_wasapi_buf_frames - (UINT32)room;
            const UINT32 target = (UINT32)(CELL_AUDIO_SAMPLE_RATE / 1000) * (UINT32)pace_ms;
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            /* The first blocks are due at once, to fill the queue to the target. */
            if (!pace_t0) pace_t0 = now.QuadPart - (long long)(target / CELL_AUDIO_BLOCK_SAMPLES) * period;
            const long long due = pace_t0 + (long long)pace_k * period;
            if (now.QuadPart - due > 8 * period)     /* a stall: carry on from now, no burst */
                pace_t0 = now.QuadPart - (long long)pace_k * period;
            else if (now.QuadPart < due) { audio_sleep_until(due, qf.QuadPart); continue; }
            pace_k++;
            /* Within two blocks of the target: the device's clock and the
             * counter drift apart, and a stall leaves the queue short. */
            if (queued + 2 * CELL_AUDIO_BLOCK_SAMPLES < target) pace_t0 -= period / 4;
            else if (queued > target + 2 * CELL_AUDIO_BLOCK_SAMPLES) pace_t0 += period / 4;
        }
#if AUDIO_BACKEND_WASAPI
        /* Device queue under half a block: it is about to play silence (an
         * audible click/stutter). AUDIO_RATE reports how often. */
        if (room >= 0 && blocks > 64 &&
            (UINT32)room + CELL_AUDIO_BLOCK_SAMPLES / 2 >= s_wasapi_buf_frames) g_audio_underruns++;
#endif
        if (room < 0) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            const long long due = q0.QuadPart +
                (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
            if (now.QuadPart < due) { Sleep(1); continue; }
            /* More than 50 ms behind (device just went away, a debugger
             * stop): drop the backlog instead of mixing it in a burst. */
            if (now.QuadPart - due > qf.QuadPart / 20)
                q0.QuadPart = now.QuadPart -
                    (long long)(blocks * CELL_AUDIO_BLOCK_SAMPLES * qf.QuadPart / CELL_AUDIO_SAMPLE_RATE);
        }
        blocks++;
        /* AUDIO_RATE: blocks taken within 1.5 ms of the one before (in pairs). */
        static unsigned s_ar_quick;
        if (audio_rate_on()) {
            static long long prev;
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            if (prev && (now.QuadPart - prev) * 2000 < qf.QuadPart * 3) s_ar_quick++;
            prev = now.QuadPart;
        }
        /* Mix and submit one block */
        audio_mix_one_block();
        /* AUDIO_PEAK=1: report the mixed block's peak amplitude periodically so
         * "is any port producing sound" is answerable (LBP Bink movie audio). */
        { static int _ap = -1; if (_ap < 0) _ap = getenv("AUDIO_PEAK") ? 1 : 0;
          if (_ap) { static unsigned _n = 0; float pk = 0.0f;
            for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * s_out_ch; i++) {
                float a = s_mix_buffer[i]; if (a < 0) a = -a; if (a > pk) pk = a; }
            if ((++_n % 200) == 0 || (pk > 0.001f && _n < 40))
                fprintf(stderr, "[audio-peak] block#%u peak=%.4f\n", _n, pk); } }
        /* AUDIO_WAV=<file>: append the final mix as raw f32le 48 kHz in the
         * output's channels, which the log gives (ffmpeg -f f32le -ar 48000
         * -ac <channels> -i <file>). */
        { static FILE* _wf = (FILE*)-1;
          if (_wf == (FILE*)-1) {
              const char* e = getenv("AUDIO_WAV"); _wf = e ? fopen(e, "wb") : NULL;
              if (_wf) fprintf(stderr, "[cellAudio] AUDIO_WAV: f32le, 48 kHz, %u channels\n", s_out_ch); }
          if (_wf) fwrite(s_mix_buffer, sizeof(float), CELL_AUDIO_BLOCK_SAMPLES * s_out_ch, _wf); }
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        /* AUDIO_RATE=1: blocks mixed vs real time, and frames dropped. */
        { static int on = -1; if (on < 0) on = getenv("AUDIO_RATE") ? 1 : 0;
          if (on) { static ULONGLONG t0; static unsigned n, z; static unsigned long long d0, u0;
            ULONGLONG now = GetTickCount64(); if (!t0) t0 = now; n++;
            { int nz = 0; for (u32 i = 0; i < CELL_AUDIO_BLOCK_SAMPLES * s_out_ch; i++) if (s_mix_buffer[i] != 0.0f) { nz = 1; break; }
              if (!nz) z++; }
            for (u32 s = 0; s < CELL_AUDIO_BLOCK_SAMPLES; s++)
                for (u32 c = 0; c < s_out_ch; c++) {
                    const double v = s_mix_buffer[s * s_out_ch + c];
                    s_ar_out_sq[c] += v * v;
                }
            s_ar_out_n += CELL_AUDIO_BLOCK_SAMPLES;
            if (now - t0 >= 5000) {
                fprintf(stderr, "[audio-rate] %.1f blocks/s (real time 187.5), silent %.1f%%, dropped %.0f frames/s, underruns %llu, device buf %u%c",
                        n * 1000.0 / (now - t0), n ? 100.0 * z / n : 0.0, (g_audio_dropped - d0) * 1000.0 / (now - t0),
                        g_audio_underruns - u0,
                #if AUDIO_BACKEND_WASAPI
                        (unsigned)s_wasapi_buf_frames, 10);
#else
                        0u, 10);
#endif
                fprintf(stderr, "[audio-ring] gaps %u (%u blocks), written after read %u, blocks ahead 0:%u 1:%u 2:%u 3+:%u, "
                                "taken right after the one before %.0f%%%c",
                        s_ar_gaps, s_ar_gap_blocks, s_ar_late, s_ar_lead[0], s_ar_lead[1], s_ar_lead[2], s_ar_lead[3],
                        n ? 100.0 * s_ar_quick / n : 0.0, 10);
                /* The level of each channel (rms): what the title's 8-channel ports hold, what goes out. */
                { char pl[160] = "none", ol[160] = "";
                  if (s_ar_port_n) {
                      const double k = 1.0 / (double)s_ar_port_n;
                      snprintf(pl, sizeof pl, "L %.4f R %.4f C %.4f LFE %.4f SL %.4f SR %.4f RL %.4f RR %.4f",
                               sqrt(s_ar_port_sq[0] * k), sqrt(s_ar_port_sq[1] * k), sqrt(s_ar_port_sq[2] * k),
                               sqrt(s_ar_port_sq[3] * k), sqrt(s_ar_port_sq[4] * k), sqrt(s_ar_port_sq[5] * k),
                               sqrt(s_ar_port_sq[6] * k), sqrt(s_ar_port_sq[7] * k)); }
                  for (u32 c = 0, at = 0; c < s_out_ch && s_ar_out_n && at < sizeof ol - 12; c++)
                      at += (u32)snprintf(ol + at, sizeof ol - at, "%s%.4f", c ? " " : "",
                                          sqrt(s_ar_out_sq[c] / (double)s_ar_out_n));
                  fprintf(stderr, "[audio-chan] the 8-channel ports, rms: %s; the output, %u channels: %s\n",
                          pl, s_out_ch, ol);
                  memset(s_ar_port_sq, 0, sizeof s_ar_port_sq);
                  memset(s_ar_out_sq, 0, sizeof s_ar_out_sq);
                  s_ar_port_n = s_ar_out_n = 0; }
                s_ar_gaps = s_ar_gap_blocks = s_ar_late = s_ar_quick = 0;
                memset(s_ar_lead, 0, sizeof s_ar_lead);
                n = z = 0; d0 = g_audio_dropped; u0 = g_audio_underruns; t0 = now; } } }

        /* Notify event queues */
        audio_notify_event_queues();
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return 0;
}
#else
static void* audio_mix_thread_func(void* arg)
{
    (void)arg;
    printf("[cellAudio] Mixing thread started\n");

    while (s_mix_thread_running) {
        audio_mix_one_block();
        audio_backend_submit(s_mix_buffer, CELL_AUDIO_BLOCK_SAMPLES);
        audio_notify_event_queues();

        u32 queued = audio_backend_queued_samples();
        if (queued > CELL_AUDIO_BLOCK_SAMPLES * 4) {
            usleep(5000);
        } else {
            usleep(2000);
        }
    }

    printf("[cellAudio] Mixing thread stopped\n");
    return NULL;
}
#endif

static int audio_start_mix_thread(void)
{
    s_mix_thread_running = 1;

#ifdef _WIN32
    s_mix_thread = (HANDLE)_beginthreadex(NULL, 0, audio_mix_thread_func,
                                           NULL, 0, NULL);
    if (!s_mix_thread) {
        s_mix_thread_running = 0;
        return -1;
    }
#else
    if (pthread_create(&s_mix_thread, NULL, audio_mix_thread_func, NULL) != 0) {
        s_mix_thread_running = 0;
        return -1;
    }
    s_mix_thread_live = 1;
#endif
    return 0;
}

static void audio_stop_mix_thread(void)
{
    s_mix_thread_running = 0;
#ifdef _WIN32
    if (s_mix_thread) {
        WaitForSingleObject(s_mix_thread, 2000);
        CloseHandle(s_mix_thread);
        s_mix_thread = NULL;
    }
#else
    /* Same guard. Init leaves s_audio_initialized set even when the mixing
     * thread failed to start ("continuing silently" -- a title without audio
     * still runs), so Quit reaches here with s_mix_thread never assigned.
     * pthread_t has no null value to test, and joining an unset one, or
     * joining the same one twice, is undefined rather than an error the way
     * WaitForSingleObject on a null HANDLE is. */
    if (s_mix_thread_live) {
        pthread_join(s_mix_thread, NULL);
        s_mix_thread_live = 0;
    }
#endif
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioInit(void)
{
    /* PS3_NO_AUDIO=1: report that the audio library is unavailable. Middleware
     * that drives its mixer through SPU jobs will otherwise block the whole boot
     * waiting on work we cannot yet complete; failing here lets a title take its
     * silent path and keep going. Diagnostic first, workaround second. */
    if (getenv("PS3_NO_AUDIO")) {
        printf("[cellAudio] PS3_NO_AUDIO -- reporting audio unavailable\n");
        return (s32)CELL_AUDIO_ERROR_NOT_INIT;
    }
    printf("[cellAudio] Init()\n");

    if (s_audio_initialized)
        return CELL_AUDIO_ERROR_ALREADY_INIT;

    memset(s_ports, 0, sizeof(s_ports));
    memset(s_notify_queues, 0, sizeof(s_notify_queues));
    mutex_init(&s_audio_mutex);

    if (audio_backend_init() < 0) {
        printf("[cellAudio] WARNING: Audio backend init failed, continuing silently\n");
        /* Don't fail -- games should still run without audio */
    }


#ifdef _WIN32
    { LARGE_INTEGER f, c;
      QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
      s_audio_start_us = (u64)(c.QuadPart / f.QuadPart) * 1000000ULL +
          (u64)(c.QuadPart % f.QuadPart) * 1000000ULL / (u64)f.QuadPart; }
#else
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      s_audio_start_us = (u64)ts.tv_sec * 1000000ULL + (u64)ts.tv_nsec / 1000ULL; }
#endif
    if (audio_start_mix_thread() < 0) {
        printf("[cellAudio] WARNING: Could not start mixing thread\n");
    }

    s_audio_initialized = 1;
    return CELL_OK;
}

s32 cellAudioQuit(void)
{
    printf("[cellAudio] Quit()\n");

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    audio_stop_mix_thread();
    audio_backend_shutdown();

    /* Port buffers live in the guest vm_base arena (bump-allocated, not host
     * malloc) -- just drop the references; never free() them. */
    for (int i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        s_ports[i].buffer = NULL;
        s_ports[i].in_use = 0;
        s_ports[i].running = 0;
    }

    mutex_destroy(&s_audio_mutex);
    s_audio_initialized = 0;
    return CELL_OK;
}

s32 cellAudioPortOpen(const CellAudioPortParam* param, u32* portNum)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    /* param / portNum are GUEST addresses; translate, and read the BE u64 param
     * fields via vm_read64 (a raw param->nChannel faults / is host-endian). */
    uint32_t param_ea   = (uint32_t)(uintptr_t)param;
    uint32_t portNum_ea = (uint32_t)(uintptr_t)portNum;
    if (!param_ea || !portNum_ea)
        return CELL_AUDIO_ERROR_PARAM;

    u64 nch  = vm_read64(param_ea + 0);   /* nChannel */
    u64 nblk = vm_read64(param_ea + 8);   /* nBlock   */
    printf("[cellAudio] PortOpen(nChannel=%llu, nBlock=%llu)\n",
           (unsigned long long)nch, (unsigned long long)nblk);

    if (nch != CELL_AUDIO_PORT_2CH && nch != CELL_AUDIO_PORT_8CH)
        return CELL_AUDIO_ERROR_PARAM;
    if (nblk != CELL_AUDIO_BLOCK_8 && nblk != CELL_AUDIO_BLOCK_16 && nblk != CELL_AUDIO_BLOCK_32)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);

    /* Find a free port slot */
    s32 found = -1;
    for (u32 i = 0; i < CELL_AUDIO_PORT_MAX; i++) {
        if (!s_ports[i].in_use) {
            found = (s32)i;
            break;
        }
    }

    if (found < 0) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_FULL;
    }

    AudioPortSlot* port = &s_ports[found];
    port->in_use  = 1;
    port->running = 0;
    /* Store the decoded (host-endian) param values, not the raw BE guest bytes. */
    memset(&port->param, 0, sizeof(port->param));
    port->param.nChannel = nch;
    port->param.nBlock   = nblk;
    port->read_index  = 0;
    port->write_index = 0;

    /* Allocate the audio buffer in GUEST memory so portAddr/readIndexAddr are
     * real guest addresses (the game's mixer -- FMOD here -- writes PCM there
     * and validates them as 1 MB-aligned guest pointers; a host pointer trips
     * its "invalid parameter" check). Bump from a free main-memory window; the
     * host-side `buffer` is just the vm_base view of the same bytes. */
    u32 buf_samples = (u32)(nblk * CELL_AUDIO_BLOCK_SAMPLES * nch);
    port->buf_size = buf_samples * (u32)sizeof(float);

    /* Each port reuses its own two pages in the runtime-owned audio window. */
    _Static_assert(CELL_AUDIO_PORT_MAX * VM_AUDIO_PORT_SIZE <= VM_AUDIO_SIZE,
                   "Audio port storage exceeds its reserved VM window");
    u32 guest_buf = VM_AUDIO_BASE + (u32)found * VM_AUDIO_PORT_SIZE;
    u32 guest_ridx = guest_buf + 0x100000u;
    if (vm_commit(guest_buf, VM_AUDIO_PORT_SIZE) != CELL_OK) {
        port->in_use = 0;
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_AUDIOSYSTEM;
    }

    port->buffer = (float*)(vm_base + guest_buf);     /* host view of guest buffer */
    memset(port->buffer, 0, port->buf_size);
    vm_write64(guest_ridx, 0);                        /* read index counter (u64, see below) */

    port->port_addr     = guest_buf;
    port->read_idx_addr = guest_ridx;

    vm_write32(portNum_ea, (u32)found);   /* guest out-param */

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortClose(u32 portNum)
{
    printf("[cellAudio] PortClose(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    mutex_lock(&s_audio_mutex);

    if (!s_ports[portNum].in_use) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    }

    /* buffer points INTO the guest vm_base arena (reserved per port in PortOpen),
     * not a host malloc -- do NOT free() it (that corrupts the host heap).
     * The guest window is reclaimed wholesale when vm_base is released. */
    s_ports[portNum].buffer = NULL;

    s_ports[portNum].in_use  = 0;
    s_ports[portNum].running = 0;

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortStart(u32 portNum)
{
    printf("[cellAudio] PortStart(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 1;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioPortStop(u32 portNum)
{
    printf("[cellAudio] PortStop(port=%u)\n", portNum);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!s_ports[portNum].running)
        return CELL_AUDIO_ERROR_PORT_NOT_RUN;

    mutex_lock(&s_audio_mutex);
    s_ports[portNum].running = 0;
    mutex_unlock(&s_audio_mutex);

    return CELL_OK;
}

s32 cellAudioGetPortBlockTag(u32 portNum, u64 blockNo, u64* tag)
{
    uint32_t tag_ea = (uint32_t)(uintptr_t)tag;
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!tag_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];
    u64 nblk = port->param.nBlock ? port->param.nBlock : 1;
    if (blockNo >= nblk) {
        mutex_unlock(&s_audio_mutex);
        return CELL_AUDIO_ERROR_PARAM;
    }
    u64 t = port->read_index + blockNo - (port->read_index % nblk);
    if (blockNo < port->read_index % nblk) t += nblk;
    mutex_unlock(&s_audio_mutex);

    vm_write64(tag_ea, t);
    return CELL_OK;
}

s32 cellAudioGetPortTimestamp(u32 portNum, u64 tag, u64* stamp)
{
    uint32_t stamp_ea = (uint32_t)(uintptr_t)stamp;
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (portNum >= CELL_AUDIO_PORT_MAX)
        return CELL_AUDIO_ERROR_PARAM;
    if (!s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
    if (!stamp_ea)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    u64 global_counter = s_ports[portNum].read_index;
    mutex_unlock(&s_audio_mutex);
    if (tag > global_counter)
        return CELL_AUDIO_ERROR_TAG_NOT_FOUND;

    u64 t = s_audio_start_us + tag * 256000000ULL / 48000ULL;
    vm_write64(stamp_ea, t);
    return CELL_OK;
}

/* cellAudioCreateNotifyEventQueue(sys_event_queue_t* id, u64* key)
 *
 * Creates the queue the game will block on for audio-period events, and
 * registers it for notification in one step -- the game never calls
 * SetNotifyEventQueue for this one.
 *
 * This was missing entirely. YDKJ imports it, got the unimplemented-NID path,
 * and then blocked on an id nothing had created: sys_event_queue_receive
 * answered CELL_ESRCH every time and the title logged 1.7 MILLION
 * "ERROR - LIBAUDIO DROPOUT" lines in a 60 s run.
 *
 * Both params are GUEST addresses and both are big-endian out-values; the id
 * is 32-bit and the key 64-bit, so they cannot be written with a plain store. */
s32 cellAudioCreateNotifyEventQueue(u32* id, u64* key)
{
    uint32_t id_ea  = (uint32_t)(uintptr_t)id;
    uint32_t key_ea = (uint32_t)(uintptr_t)key;

    printf("[cellAudio] CreateNotifyEventQueue(id_ea=0x%08X key_ea=0x%08X)\n",
           id_ea, key_ea);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;
    if (!id_ea || !key_ea)
        return CELL_AUDIO_ERROR_PARAM;

    /* A distinct key per queue: the notify path resolves queues BY KEY, and the
     * game creates its other queues with key 0, so reusing 0 here would make
     * find_queue_by_key ambiguous and post audio events to whichever matched. */
    static uint64_t s_next_key = 0x8000000000000001ull;
    uint64_t k = s_next_key++;

    uint32_t qid = sys_event_queue_create_direct(k, 8);
    if (!qid)
        return CELL_AUDIO_ERROR_EVENT_QUEUE;

    mutex_lock(&s_audio_mutex);
    int slot = -1;
    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) { slot = i; break; }
    }
    if (slot < 0) { mutex_unlock(&s_audio_mutex); return CELL_AUDIO_ERROR_PARAM; }
    s_notify_queues[slot].in_use = 1;
    s_notify_queues[slot].key    = k;
    mutex_unlock(&s_audio_mutex);

    vm_write32(id_ea, qid);
    vm_write64(key_ea, k);
    printf("[cellAudio] CreateNotifyEventQueue -> id=%u key=0x%llX\n",
           qid, (unsigned long long)k);
    return CELL_OK;
}

s32 cellAudioSetNotifyEventQueue(u64 key)
{
    printf("[cellAudio] SetNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (!s_notify_queues[i].in_use) {
            s_notify_queues[i].in_use = 1;
            s_notify_queues[i].key = key;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM; /* no free slots */
}

s32 cellAudioRemoveNotifyEventQueue(u64 key)
{
    printf("[cellAudio] RemoveNotifyEventQueue(key=0x%llX)\n",
           (unsigned long long)key);

    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    mutex_lock(&s_audio_mutex);

    for (int i = 0; i < CELL_AUDIO_MAX_NOTIFY_EVENT_QUEUES; i++) {
        if (s_notify_queues[i].in_use && s_notify_queues[i].key == key) {
            s_notify_queues[i].in_use = 0;
            mutex_unlock(&s_audio_mutex);
            return CELL_OK;
        }
    }

    mutex_unlock(&s_audio_mutex);
    return CELL_AUDIO_ERROR_PARAM;
}

s32 cellAudioGetPortConfig(u32 portNum, CellAudioPortConfig* config)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !s_ports[portNum].in_use)
        return CELL_AUDIO_ERROR_PORT_NOT_OPEN;

    if (!config)
        return CELL_AUDIO_ERROR_PARAM;

    /* `config` is a GUEST address; write the BE struct field-by-field at its
     * guest offsets (8-byte aligned u64s). */
    uint32_t cfg = (uint32_t)(uintptr_t)config;
    if (!cfg)
        return CELL_AUDIO_ERROR_PARAM;

    mutex_lock(&s_audio_mutex);
    AudioPortSlot* port = &s_ports[portNum];

    /* CellAudioPortConfig (cell/audio.h). sys_addr_t is uintptr_t, which on the
     * 32-bit PPU ABI is FOUR bytes -- not eight. Writing the two sys_addr_t
     * fields as u64 shifted every field past it and left the game reading the
     * high (zero) half of readIndexAddr as its pointer:
     *   sys_addr_t readIndexAddr;  u32 @  0
     *   uint32_t   status;         u32 @  4
     *   uint64_t   nChannel;       u64 @  8
     *   uint64_t   nBlock;         u64 @ 16
     *   uint32_t   portSize;       u32 @ 24
     *   sys_addr_t portAddr;       u32 @ 28      -> 32 bytes */
    vm_write32(cfg +  0, (u32)port->read_idx_addr);                          /* readIndexAddr */
    vm_write32(cfg +  4, port->running ? CELL_AUDIO_STATUS_RUN
                                       : CELL_AUDIO_STATUS_READY);           /* status */
    vm_write64(cfg +  8, port->param.nChannel);                             /* nChannel */
    vm_write64(cfg + 16, port->param.nBlock);                               /* nBlock */
    vm_write32(cfg + 24, port->buf_size);                                   /* portSize */
    vm_write32(cfg + 28, (u32)port->port_addr);                             /* portAddr */

    { static int _n = 0; if (PS3_LOG_FIRST(_n, 24))
        fprintf(stderr, "[cellAudio] GetPortConfig(port=%u) status=%s bufEA=0x%08X ridxEA=0x%08X\n",
                portNum, port->running ? "RUN" : "READY",
                (u32)port->port_addr, (u32)port->read_idx_addr); }

    mutex_unlock(&s_audio_mutex);
    return CELL_OK;
}

s32 cellAudioPortGetStatus(u32 portNum, u32* status)
{
    if (!s_audio_initialized)
        return CELL_AUDIO_ERROR_NOT_INIT;

    if (portNum >= CELL_AUDIO_PORT_MAX || !status)
        return CELL_AUDIO_ERROR_PARAM;

    if (!s_ports[portNum].in_use) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_CLOSE);
    } else if (s_ports[portNum].running) {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_RUN);
    } else {
        vm_write32((u32)(uintptr_t)status, CELL_AUDIO_STATUS_READY);
    }

    return CELL_OK;
}

s32 cellAudioSetPersonalDevice(s32 iPersonalStream, s32 iDevice)
{
    (void)iPersonalStream;
    (void)iDevice;
    printf("[cellAudio] SetPersonalDevice(stream=%d, device=%d) - stub\n",
           iPersonalStream, iDevice);
    return CELL_OK;
}

s32 cellAudioUnsetPersonalDevice(s32 iPersonalStream)
{
    (void)iPersonalStream;
    printf("[cellAudio] UnsetPersonalDevice(stream=%d) - stub\n", iPersonalStream);
    return CELL_OK;
}
