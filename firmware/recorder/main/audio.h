/*
 * audio_task: I2S standard mode with DMA (32-bit slots) → 24-bit sample → DC-blocking high-pass →
 * gain → 16-bit, cut into 500 ms chunks, each with a per-session seq, into the RAM ring.
 * With CONFIG_LC_AUDIO_SOURCE_TEST_CLIP it plays the built-in speech clip instead of the mic.
 */
#pragma once

void audio_start(void);
