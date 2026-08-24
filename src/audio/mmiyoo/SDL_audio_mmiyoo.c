/*
  Customized version for Miyoo-Mini handheld.
  Only tested under Miyoo-Mini stock OS (original firmware) with Parasyte compatible layer.

  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>
  Copyright (C) 2022-2022 Steward Fu <steward.fu@gmail.com>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_AUDIO_DRIVER_MMIYOO

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "SDL_timer.h"
#include "SDL_audio.h"
#include "SDL_audio_mmiyoo.h"
#include "../SDL_audio_c.h"
#include "../SDL_audiodev_c.h"

#if defined(MMIYOO)
#include "mi_sys.h"
#include "mi_common_datatype.h"
#include "mi_ao.h"
#include "mi_ao_datatype.h"

static MI_AO_CHN g_AoChn = 0;
static MI_AUDIO_DEV g_AoDevId = 0;

#define MMIYOO_AUDIO_DEFAULT_FREQ            44100
#define MMIYOO_AUDIO_SAMPLE_ALIGN            128    /* MI_AO requires u32PtNumPerFrm aligned to this */
#define MMIYOO_AUDIO_FRAME_COUNT             6      /* MI_AUDIO_Attr_t.u32FrmNum: internal AO frame buffer count */
#define MMIYOO_AUDIO_PORT_DEPTH_MIN          6      /* MI_SYS_SetChnOutputPortDepth: min queued output frames */
#define MMIYOO_AUDIO_PORT_DEPTH_MAX          7      /* MI_SYS_SetChnOutputPortDepth: max queued output frames */
#define MMIYOO_AUDIO_TARGET_BACKLOG_FRAMES   2      /* WaitDevice throttle target, revisit if too tight/loose */
#define MMIYOO_AUDIO_WAIT_DELAY_MIN_MS       5      /* WaitDevice poll floor: avoid a tight QueryChnStat spin */
#define MMIYOO_AUDIO_RETRY_DELAY_MS          1      /* PlayDevice retry spacing on MI_AO_ERR_NOBUF */
/* MI_AO_SendFrame's s32MilliSec: -1 blocks until space is free (the pattern
 * used by SigmaStar's own MI_AO_SendFrame example, incl. the NOBUF retry
 * loop below), 0 is non-blocking, >0 is a bounded timeout in ms. */
#define MMIYOO_AUDIO_SENDFRAME_BLOCK_FOREVER (-1)
/* Safety net only, not expected to trigger in normal operation: caps the
 * NOBUF retry loop so a stuck AO channel can't hang the audio thread
 * forever if this->enabled never clears. */
#define MMIYOO_AUDIO_SENDFRAME_MAX_RETRIES   5000

static MI_AUDIO_SampleRate_e
MMIYOO_SelectSampleRate(int *freq)
{
    static const int supported_freqs[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    static const MI_AUDIO_SampleRate_e supported_rates[] = {
        E_MI_AUDIO_SAMPLE_RATE_8000,
        E_MI_AUDIO_SAMPLE_RATE_11025,
        E_MI_AUDIO_SAMPLE_RATE_12000,
        E_MI_AUDIO_SAMPLE_RATE_16000,
        E_MI_AUDIO_SAMPLE_RATE_22050,
        E_MI_AUDIO_SAMPLE_RATE_24000,
        E_MI_AUDIO_SAMPLE_RATE_32000,
        E_MI_AUDIO_SAMPLE_RATE_44100,
        E_MI_AUDIO_SAMPLE_RATE_48000
    };
    int target;
    int best;
    int idx;
    int diff;

    target = *freq;
    if (target <= 0) {
        target = MMIYOO_AUDIO_DEFAULT_FREQ;
    }

    best = 0;
    diff = SDL_abs(supported_freqs[0] - target);
    for (idx = 1; idx < (int)SDL_arraysize(supported_freqs); ++idx) {
        const int curdiff = SDL_abs(supported_freqs[idx] - target);
        if (curdiff < diff) {
            diff = curdiff;
            best = idx;
        }
    }

    *freq = supported_freqs[best];
    return supported_rates[best];
}
#endif

static void MMIYOO_CloseDevice(_THIS)
{
#if defined(MMIYOO)
    if (this->hidden && this->hidden->ao_active) {
        MI_AO_ClearChnBuf(g_AoDevId, g_AoChn);
        MI_AO_DisableChn(g_AoDevId, g_AoChn);
        MI_AO_Disable(g_AoDevId);
        MI_AO_ClrPubAttr(g_AoDevId);
        this->hidden->ao_active = SDL_FALSE;
    }
#endif
    if (this->hidden) {
        SDL_free(this->hidden->mixbuf);
        SDL_free(this->hidden);
        this->hidden = NULL;
    }
}

static int MMIYOO_OpenDevice(_THIS, void *handle, const char *devname, int iscapture)
{
    Uint32 bitsize;
    Uint32 sample_bytes;
    Uint32 frame_bytes;
    Uint32 mixlen;
#if defined(MMIYOO)
    MI_AUDIO_SampleRate_e samplerate;
    MI_AUDIO_Attr_t attr;
    MI_SYS_ChnPort_t port;
    MI_S32 ret;
#endif

    if (iscapture) {
        return SDL_SetError("MMIYOO: capture is not supported");
    }

    this->spec.format = AUDIO_S16SYS;
    if (this->spec.channels <= 0) {
        this->spec.channels = 1;
    } else if (this->spec.channels > 2) {
        this->spec.channels = 2;
    }

    if (this->spec.samples < MMIYOO_AUDIO_SAMPLE_ALIGN) {
        this->spec.samples = MMIYOO_AUDIO_SAMPLE_ALIGN;
    }
    this->spec.samples = (Uint16)((this->spec.samples + (MMIYOO_AUDIO_SAMPLE_ALIGN - 1)) & ~(MMIYOO_AUDIO_SAMPLE_ALIGN - 1));

#if defined(MMIYOO)
    samplerate = MMIYOO_SelectSampleRate(&this->spec.freq);
#endif

    SDL_CalculateAudioSpec(&this->spec);

    bitsize = SDL_AUDIO_BITSIZE(this->spec.format);
    if (bitsize == 0) {
        bitsize = 16;
    }
    sample_bytes = bitsize / 8;
    frame_bytes = sample_bytes * (Uint32)this->spec.channels;
    mixlen = this->spec.size;

    this->hidden = (struct SDL_PrivateAudioData *)SDL_malloc(sizeof(*this->hidden));
    if (this->hidden == NULL) {
        return SDL_OutOfMemory();
    }
    SDL_zerop(this->hidden);

    this->hidden->frame_bytes = frame_bytes;
    this->hidden->mixlen = mixlen;
    this->hidden->mixbuf = (Uint8 *)SDL_malloc(mixlen);
    if (this->hidden->mixbuf == NULL) {
        SDL_free(this->hidden);
        this->hidden = NULL;
        return SDL_OutOfMemory();
    }

#if defined(MMIYOO)
    SDL_zero(attr);
    attr.eSamplerate = samplerate;
    attr.eBitwidth = E_MI_AUDIO_BIT_WIDTH_16;
    attr.eWorkmode = E_MI_AUDIO_MODE_I2S_MASTER;
    attr.eSoundmode = (this->spec.channels > 1) ? E_MI_AUDIO_SOUND_MODE_STEREO : E_MI_AUDIO_SOUND_MODE_MONO;
    attr.u32FrmNum = MMIYOO_AUDIO_FRAME_COUNT;
    attr.u32PtNumPerFrm = this->spec.samples;
    attr.u32CodecChnCnt = attr.eSoundmode == E_MI_AUDIO_SOUND_MODE_STEREO ? 2 : 1;
    attr.u32ChnCnt = (MI_U32)this->spec.channels;

    ret = MI_AO_SetPubAttr(g_AoDevId, &attr);
    if (ret != MI_SUCCESS) {
        SDL_SetError("MMIYOO: MI_AO_SetPubAttr failed (0x%x)", ret);
        goto fail_pubattr;
    }

    ret = MI_AO_Enable(g_AoDevId);
    if (ret != MI_SUCCESS) {
        SDL_SetError("MMIYOO: MI_AO_Enable failed (0x%x)", ret);
        goto fail_enable;
    }

    ret = MI_AO_EnableChn(g_AoDevId, g_AoChn);
    if (ret != MI_SUCCESS) {
        SDL_SetError("MMIYOO: MI_AO_EnableChn failed (0x%x)", ret);
        goto fail_chn;
    }

    SDL_zero(port);
    port.eModId = E_MI_MODULE_ID_AO;
    port.u32DevId = g_AoDevId;
    port.u32ChnId = g_AoChn;
    port.u32PortId = 0;
    ret = MI_SYS_SetChnOutputPortDepth(&port, MMIYOO_AUDIO_PORT_DEPTH_MIN, MMIYOO_AUDIO_PORT_DEPTH_MAX);
    if (ret != MI_SUCCESS) {
        SDL_Log("MMIYOO: MI_SYS_SetChnOutputPortDepth failed (0x%x), using driver default depth", ret);
    }
    MI_AO_ClearChnBuf(g_AoDevId, g_AoChn);

    this->hidden->ao_active = SDL_TRUE;
    SDL_zero(this->hidden->frame);
    this->hidden->frame.eBitwidth = attr.eBitwidth;
    this->hidden->frame.eSoundmode = attr.eSoundmode;
    this->hidden->frame.apVirAddr[0] = this->hidden->mixbuf;
    this->hidden->frame.apVirAddr[1] = NULL;
    this->hidden->frame.u32Len = this->hidden->mixlen;
#endif

    return 0;

#if defined(MMIYOO)
fail_chn:
    MI_AO_Disable(g_AoDevId);
fail_enable:
    MI_AO_ClrPubAttr(g_AoDevId);
fail_pubattr:
    SDL_free(this->hidden->mixbuf);
    SDL_free(this->hidden);
    this->hidden = NULL;
    return -1;
#endif
}

static void MMIYOO_WaitDevice(_THIS)
{
#if defined(MMIYOO)
    MI_AO_ChnState_t aoState;
    MI_U32 target_backlog_bytes;
    MI_U32 bytes_per_second;
    MI_S32 ret;

    if (!this->hidden || !this->hidden->ao_active) {
        return;
    }

    if (this->hidden->mixlen == 0U) {
        return;
    }
    target_backlog_bytes = MMIYOO_AUDIO_TARGET_BACKLOG_FRAMES * this->hidden->mixlen;
    bytes_per_second = (this->hidden->frame_bytes && this->spec.freq > 0) ? (this->spec.freq * this->hidden->frame_bytes) : 0;

    /* Throttle on queued backlog, not free space, to keep steady-state latency low. */
    while (SDL_AtomicGet(&this->enabled)) {
        ret = MI_AO_QueryChnStat(g_AoDevId, g_AoChn, &aoState);
        if (ret != MI_SUCCESS) {
            SDL_Delay(MMIYOO_AUDIO_WAIT_DELAY_MIN_MS);
            continue;
        }

        if (aoState.u32ChnBusyNum <= target_backlog_bytes) {
            break;
        }

        if (bytes_per_second == 0) {
            SDL_Delay(MMIYOO_AUDIO_WAIT_DELAY_MIN_MS);
        } else {
            Uint32 deficit;
            Uint32 delay_ms;

            deficit = aoState.u32ChnBusyNum - target_backlog_bytes;
            delay_ms = (deficit * 1000U) / bytes_per_second;
            if (delay_ms < MMIYOO_AUDIO_WAIT_DELAY_MIN_MS) {
                delay_ms = MMIYOO_AUDIO_WAIT_DELAY_MIN_MS;
            }
            SDL_Delay(delay_ms);
        }
    }
#else
    SDL_Delay(MMIYOO_AUDIO_WAIT_DELAY_MIN_MS);
#endif
}

static void MMIYOO_PlayDevice(_THIS)
{
#if defined(MMIYOO)
    MI_AUDIO_Frame_t *frame;
    MI_S32 ret;
    int retries;

    if (!this->hidden || !this->hidden->ao_active) {
        return;
    }

    frame = &this->hidden->frame;
    frame->u32Len = this->hidden->mixlen;
    frame->apVirAddr[0] = this->hidden->mixbuf;
    frame->u32Seq++;

    retries = 0;
    do {
        ret = MI_AO_SendFrame(g_AoDevId, g_AoChn, frame, MMIYOO_AUDIO_SENDFRAME_BLOCK_FOREVER);
        if (ret == MI_AO_ERR_NOBUF) {
            SDL_Delay(MMIYOO_AUDIO_RETRY_DELAY_MS);
            retries++;
        }
    } while ((ret == MI_AO_ERR_NOBUF) && SDL_AtomicGet(&this->enabled) && retries < MMIYOO_AUDIO_SENDFRAME_MAX_RETRIES);

    if (ret == MI_AO_ERR_NOBUF) {
        SDL_Log("MMIYOO: MI_AO_SendFrame stayed NOBUF after %d retries, dropping frame", retries);
    }
#else
    SDL_Delay(5);
#endif
}

static Uint8 *MMIYOO_GetDeviceBuf(_THIS)
{
    if (!this->hidden) {
        return NULL;
    }
    return (this->hidden->mixbuf);
}

static int MMIYOO_Init(SDL_AudioDriverImpl *impl)
{
    impl->OpenDevice = MMIYOO_OpenDevice;
    impl->PlayDevice = MMIYOO_PlayDevice;
    impl->WaitDevice = MMIYOO_WaitDevice;
    impl->GetDeviceBuf = MMIYOO_GetDeviceBuf;
    impl->CloseDevice = MMIYOO_CloseDevice;
    impl->OnlyHasDefaultOutputDevice = 1;
    return 1;
}

AudioBootStrap MMIYOOAUDIO_bootstrap = {"MMIYOO", "MMIYOO AUDIO DRIVER", MMIYOO_Init, 0};

#endif
