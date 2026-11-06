#ifndef DETECTOR_H
#define DETECTOR_H

#include "esp_err.h"
#include <stdint.h>

#define DETECTOR_BLOCK_SAMPLES 512

esp_err_t detector_init(uint32_t sample_rate);
void detector_reset();
bool detector_process(const float *samples);

#endif