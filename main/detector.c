#include "detector.h"
#include "dsps_fft2r.h"
#include "dsps_wind_hann.h"
#include "esp_err.h"
#include "esp_dsp.h"
#include <stdint.h>
#include <string.h>

#define FFT_SIZE (2 * DETECTOR_BLOCK_SAMPLES)

static const float TONE_MIN_HZ = 4000.0f;
static const float TONE_MAX_HZ = 4120.0f;
static const float REF_TONE_MIN_HZ = 500.0f;
static const float REF_TONE_MAX_HZ = 7500.0f;

static const float MIN_RATIO = 0.3f;
static const float MIN_BAND_ENERGY = 0.001f;

static const float SHORT_MIN_S = 0.4f;
static const float SHORT_MAX_S = 0.7f;

static const float GAP_MIN_S = 0.2f;
static const float GAP_MAX_S = 0.5f;

static const float LONG_MIN_S = 0.8f;
static const float LONG_MAX_S = 1.3f;

static float frame[FFT_SIZE];
static float hann_window[FFT_SIZE];
static float fft_buffer[2 * FFT_SIZE] __attribute__((aligned(16)));

static size_t samples_collected = 0;
static uint32_t sampling_rate;

typedef enum {
    WAIT_FIRST_SHORT,
    WAIT_FIRST_GAP,
    WAIT_SECOND_SHORT,
    WAIT_SECOND_GAP,
    WAIT_LONG,
} sequence_state_t;

static sequence_state_t current_state = WAIT_FIRST_GAP;
static bool previous_tone = false;
static float duration_s = 0;
static float step_s = 0;

esp_err_t detector_init(uint32_t sample_rate){
    sampling_rate = sample_rate;
    
    step_s = (float)DETECTOR_BLOCK_SAMPLES / sample_rate;
    
    esp_err_t err = dsps_fft2r_init_fc32(NULL, FFT_SIZE);
    if (err != ESP_OK){
        return err;
    }

    dsps_wind_hann_f32(hann_window, FFT_SIZE);

    detector_reset();

    return ESP_OK;
}

void detector_reset(){
    memset(frame, 0, sizeof(frame));
    samples_collected = 0;
    current_state = WAIT_FIRST_SHORT;
    previous_tone = false;
    duration_s = 0;
}

static bool detect_tone(){
    for (size_t i = 0; i < FFT_SIZE; i++) {
        fft_buffer[2 * i] = frame[i] * hann_window[i];
        fft_buffer[2 * i + 1] = 0.0f;
    }

    dsps_fft2r_fc32(fft_buffer, FFT_SIZE);
    dsps_bit_rev_fc32(fft_buffer, FFT_SIZE);

    float band_energy = 0.0f;
    float reference_energy = 0.0f;

    for (size_t k = 0; k <= FFT_SIZE/2; k++) {
        float frequency = (float)k * sampling_rate / FFT_SIZE;

        float re = fft_buffer[2 * k];
        float im = fft_buffer[2 * k + 1];
        float power = re * re + im * im;

        if (frequency >= TONE_MIN_HZ && frequency <= TONE_MAX_HZ) {
            band_energy += power;
        }

        if (frequency >= REF_TONE_MIN_HZ && frequency <= REF_TONE_MAX_HZ) {
            reference_energy += power;
        }
    }

    float ratio = 0.0f;

    if (reference_energy > 0.0f) {
        ratio = band_energy / reference_energy;
    }

    return band_energy > MIN_BAND_ENERGY && ratio > MIN_RATIO;
}

static bool update_sequence(bool tone_present){
    bool finished = false;

    if (tone_present == previous_tone) {
        duration_s += step_s;

        if ((current_state == WAIT_FIRST_GAP || current_state == WAIT_SECOND_GAP) && !tone_present && duration_s > GAP_MAX_S) {
            current_state = WAIT_FIRST_SHORT;
        }

        return false;
    }
    
    switch (current_state) {
        case WAIT_FIRST_SHORT:
            if (previous_tone && duration_s >= SHORT_MIN_S && duration_s <= SHORT_MAX_S) {
                current_state = WAIT_FIRST_GAP;
            }
            break;
        case WAIT_FIRST_GAP:
            if (!previous_tone && duration_s >= GAP_MIN_S && duration_s <= GAP_MAX_S) {
                current_state = WAIT_SECOND_SHORT;
            } else {
                current_state = WAIT_FIRST_SHORT;
            }
            break;
        case WAIT_SECOND_SHORT:
            if (previous_tone && duration_s >= SHORT_MIN_S && duration_s <= SHORT_MAX_S) {
                current_state = WAIT_SECOND_GAP;
            } else {
                current_state = WAIT_FIRST_SHORT;
            }
            break;
        case WAIT_SECOND_GAP:
            if (!previous_tone && duration_s >= GAP_MIN_S && duration_s <= GAP_MAX_S) {
                    current_state = WAIT_LONG;
            } else {
                current_state = WAIT_FIRST_SHORT;
            }
            break;
        case WAIT_LONG:
            if (previous_tone && duration_s >= LONG_MIN_S && duration_s <= LONG_MAX_S) {
                finished = true;
            }
            current_state = WAIT_FIRST_SHORT;
            break;
    }

    previous_tone = tone_present;
    duration_s = step_s;

    return finished;
}

bool detector_process(const float *samples){
    memcpy(frame + samples_collected, samples, DETECTOR_BLOCK_SAMPLES * sizeof(frame[0]));

    samples_collected += DETECTOR_BLOCK_SAMPLES;

    if (samples_collected < FFT_SIZE) {
        return false;
    }

    bool tone_present = detect_tone();
    bool finished = update_sequence(tone_present);

    if (finished) {
        detector_reset();
        return true;
    }

    memmove(frame, frame + DETECTOR_BLOCK_SAMPLES, DETECTOR_BLOCK_SAMPLES * sizeof(frame[0]));

    samples_collected = DETECTOR_BLOCK_SAMPLES;

    return false;
}