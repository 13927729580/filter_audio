
#include <stdint.h>
#include <stdlib.h>

#include "agc/include/gain_control.h"
#include "ns/include/noise_suppression_x.h"
#include "aec/include/echo_cancellation.h"
#include "other/signal_processing_library.h"

typedef struct {
    NsxHandle *noise_sup_x;
    void *gain_control, *echo_cancellation;
    uint32_t fs;

    WebRtcSpl_State48khzTo16khz state_in, state_in_echo;
    WebRtcSpl_State16khzTo48khz state_out;
    int32_t tmp_mem[496];

    int16_t msInSndCardBuf;

    FilterState hpf;

    int32_t downs_2_state[8];
    int32_t ups_2_state[8];

    int echo_enabled;
    int gain_enabled;
    int noise_enabled;
} Filter_Audio;

#define _FILTER_AUDIO
#include "filter_audio.h"

void kill_filter_audio(Filter_Audio *f_a)
{
    if (!f_a) {
        return;
    }

    WebRtcNsx_Free(f_a->noise_sup_x);
    WebRtcAgc_Free(f_a->gain_control);
    WebRtcAec_Free(f_a->echo_cancellation);
    free(f_a);
}

Filter_Audio *new_filter_audio(uint32_t fs)
{
    if (fs != 8000 && fs != 16000 && fs != 48000) {
        return NULL;
    }

    Filter_Audio *f_a = calloc(sizeof(Filter_Audio), 1);

    if (!f_a) {
        return NULL;
    }

    f_a->fs = fs;

    if (fs == 48000) {
        fs = 16000;
    }

    init_highpass_filter(&f_a->hpf, fs);

    if (WebRtcAgc_Create(&f_a->gain_control) == -1) {
        free(f_a);
        return NULL;
    }

    if (WebRtcNsx_Create(&f_a->noise_sup_x) == -1) {
        WebRtcAgc_Free(f_a->gain_control);
        free(f_a);
        return NULL;
    }

    if (WebRtcAec_Create(&f_a->echo_cancellation) == -1) {
        WebRtcAgc_Free(f_a->gain_control);
        WebRtcNsx_Free(f_a->noise_sup_x);
        free(f_a);
        return NULL;
    }

    WebRtcAgc_config_t gain_config;

    gain_config.targetLevelDbfs = 1;
    gain_config.compressionGaindB = 50;
    gain_config.limiterEnable = kAgcTrue;
 
    if (WebRtcAgc_Init(f_a->gain_control, 0, 255, kAgcModeAdaptiveDigital, fs) == -1 || WebRtcAgc_set_config(f_a->gain_control, gain_config) == -1) {
        kill_filter_audio(f_a);
        return NULL;
    }


    if (WebRtcNsx_Init(f_a->noise_sup_x, fs) == -1 || WebRtcNsx_set_policy(f_a->noise_sup_x, 2) == -1) {
        kill_filter_audio(f_a);
        return NULL;
    }

    AecConfig echo_config;

    echo_config.nlpMode = kAecNlpAggressive;
    echo_config.skewMode = kAecFalse;
    echo_config.metricsMode = kAecFalse;
    echo_config.delay_logging = kAecFalse;

    if (WebRtcAec_Init(f_a->echo_cancellation, fs, f_a->fs) == -1 || WebRtcAec_set_config(f_a->echo_cancellation, echo_config) == -1) {
        kill_filter_audio(f_a);
        return NULL;
    }

    f_a->echo_enabled = 1;
    f_a->gain_enabled = 1;
    f_a->noise_enabled = 1;
    return f_a;
}

int enable_disable_filters(Filter_Audio *f_a, int echo, int noise, int gain)
{
    if (!f_a) {
        return -1;
    }

    f_a->echo_enabled = echo;
    f_a->gain_enabled = gain;
    f_a->noise_enabled = noise;
    return 0;
}

static void downsample_audio_echo_in(Filter_Audio *f_a, int16_t *out, const int16_t *in)
{
    WebRtcSpl_Resample48khzTo16khz(in, out, &f_a->state_in_echo, f_a->tmp_mem);
}

static void downsample_audio(Filter_Audio *f_a, int16_t *out, const int16_t *in)
{
    WebRtcSpl_Resample48khzTo16khz(in, out, &f_a->state_in, f_a->tmp_mem);
}

static void upsample_audio(Filter_Audio *f_a, int16_t *out, const int16_t *in)
{
    WebRtcSpl_Resample16khzTo48khz(in, out, &f_a->state_out, f_a->tmp_mem);
}


int pass_audio_output(Filter_Audio *f_a, const int16_t *data, unsigned int samples)
{
    if (!f_a || !f_a->echo_enabled) {
        return -1;
    }

    unsigned int nsx_samples = f_a->fs / 100;
    if (!samples || (samples % nsx_samples) != 0) {
        return -1;
    }

    _Bool resample = 0;
    unsigned int resampled_samples = 0;
    if (f_a->fs == 48000) {
        samples = (samples / nsx_samples) * 160;
        nsx_samples = 160;
        resample = 1;
    }

    unsigned int temp_samples = samples;

    while (temp_samples) {
        float d_f[nsx_samples];

        if (resample) {
            int16_t d[nsx_samples];
            downsample_audio_echo_in(f_a, d, data + resampled_samples);
            S16ToFloatS16(d, nsx_samples, d_f);
            resampled_samples += 480;
        } else {
            S16ToFloatS16(data + (samples - temp_samples), nsx_samples, d_f);
        }

        if (WebRtcAec_BufferFarend(f_a->echo_cancellation, d_f, nsx_samples) == -1) {
            return -1;
        }

        temp_samples -= nsx_samples;
    }

    return 0;
}

/* Tell the echo canceller how much time in ms it takes for audio to be played and recorded back after. */
int set_echo_delay_ms(Filter_Audio *f_a, int16_t msInSndCardBuf)
{
    if (!f_a) {
        return -1;
    }

    f_a->msInSndCardBuf = msInSndCardBuf;
    return 0;
}

int filter_audio(Filter_Audio *f_a, int16_t *data, unsigned int samples)
{
    if (!f_a) {
        return -1;
    }

    unsigned int nsx_samples = f_a->fs / 100;
    if (!samples || (samples % nsx_samples) != 0) {
        return -1;
    }

    _Bool resample = 0;
    unsigned int resampled_samples = 0;
    if (f_a->fs == 48000) {
        samples = (samples / nsx_samples) * 160;
        nsx_samples = 160;
        resample = 1;
    }

    unsigned int temp_samples = samples;

    while (temp_samples) {
        int16_t d[nsx_samples];
        if (resample) {
            downsample_audio(f_a, d, data + resampled_samples);
        } else {
            memcpy(d, data + (samples - temp_samples), sizeof(d));
        }

        highpass_filter(&f_a->hpf, d, nsx_samples);

        if (f_a->echo_enabled) {
            float d_f[nsx_samples];
            S16ToFloatS16(d, nsx_samples, d_f);
            if (WebRtcAec_Process(f_a->echo_cancellation, d_f, 0, d_f, 0, nsx_samples, f_a->msInSndCardBuf, 0) == -1) {
                return -1;
            }
            FloatS16ToS16(d_f, nsx_samples, d);
        }

        if (f_a->noise_enabled) {
            if (WebRtcNsx_Process(f_a->noise_sup_x, d, 0, d, 0) == -1) {
                return -1;
            }
        }

        if (f_a->gain_enabled) {
            int32_t inMicLevel = 1, outMicLevel;
            uint8_t saturationWarning;

            if (WebRtcAgc_Process(f_a->gain_control, d, 0, nsx_samples, d, 0, inMicLevel, &outMicLevel, 0, &saturationWarning) == -1) {
                return -1;
            }
        }

        if (nsx_samples == 160) {
            int16_t temp[nsx_samples / 2];
            WebRtcSpl_DownsampleBy2(d, nsx_samples, temp, f_a->downs_2_state);
            WebRtcSpl_UpsampleBy2(temp, nsx_samples / 2, d, f_a->ups_2_state);
        }

        if (resample) {
            upsample_audio(f_a, data + resampled_samples, d);
            resampled_samples += 480;
        } else {
            memcpy(data + (samples - temp_samples), d, sizeof(d));
        }

        temp_samples -= nsx_samples;
    }

    return 0;
}

