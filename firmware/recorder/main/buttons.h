/*
 * Buttons to GND with internal pull-ups; ISR + debounce task. Pins are in firmware/board/board.h.
 *   ESP32 DevKit V1: one main button (D32, and the board's BOOT button): short press = start /
 *                    pause / resume, hold 1.5 s = stop; optional Stop button on D27.
 *   ESP32-S3:        Start (GPIO1), Stop (GPIO2), Pause/Resume (GPIO7).
 */
#pragma once

void buttons_start(void);
