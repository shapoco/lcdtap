// Copyright (c) 2024 Michael Bell and Pimoroni Ltd
// SPDX-License-Identifier: MIT
//
// Adapted from Pimoroni DVHSTX (drivers/dvhstx/dvi.cpp).
// Added: dvi_timing_1280x720p_reduced_30hz (ported from PicoDVI dvi_timing.c)

#include "lcdtap/pico2/dvi_timing.hpp"

// VGA -- 252 MHz bit clock
const struct dvi_timing dvi_timing_640x480p_60hz = {.h_sync_polarity = false,
                                                    .h_front_porch = 16,
                                                    .h_sync_width = 96,
                                                    .h_back_porch = 48,
                                                    .h_active_pixels = 640,

                                                    .v_sync_polarity = false,
                                                    .v_front_porch = 10,
                                                    .v_sync_width = 2,
                                                    .v_back_porch = 33,
                                                    .v_active_lines = 480,

                                                    .bit_clk_khz = 252000};

// SVGA -- 400 MHz bit clock
const struct dvi_timing dvi_timing_800x600p_60hz = {.h_sync_polarity = false,
                                                    .h_front_porch = 44,
                                                    .h_sync_width = 128,
                                                    .h_back_porch = 88,
                                                    .h_active_pixels = 800,

                                                    .v_sync_polarity = false,
                                                    .v_front_porch = 1,
                                                    .v_sync_width = 4,
                                                    .v_back_porch = 23,
                                                    .v_active_lines = 600,

                                                    .bit_clk_khz = 400000};

// 720x480@60Hz
const struct dvi_timing dvi_timing_720x480p_60hz = {.h_sync_polarity = false,
                                                    .h_front_porch = 16,
                                                    .h_sync_width = 62,
                                                    .h_back_porch = 60,
                                                    .h_active_pixels = 720,

                                                    .v_sync_polarity = false,
                                                    .v_front_porch = 9,
                                                    .v_sync_width = 6,
                                                    .v_back_porch = 30,
                                                    .v_active_lines = 480,

                                                    .bit_clk_khz = 270000};

// 720x576@50Hz - CEA timing
const struct dvi_timing dvi_timing_720x576p_50hz = {.h_sync_polarity = false,
                                                    .h_front_porch = 12,
                                                    .h_sync_width = 64,
                                                    .h_back_porch = 68,
                                                    .h_active_pixels = 720,

                                                    .v_sync_polarity = false,
                                                    .v_front_porch = 5,
                                                    .v_sync_width = 5,
                                                    .v_back_porch = 39,
                                                    .v_active_lines = 576,

                                                    .bit_clk_khz = 270000};

// 720x400@70Hz - "IBM" timing
const struct dvi_timing dvi_timing_720x400p_70hz = {.h_sync_polarity = false,
                                                    .h_front_porch = 18,
                                                    .h_sync_width = 108,
                                                    .h_back_porch = 54,
                                                    .h_active_pixels = 720,

                                                    .v_sync_polarity = true,
                                                    .v_front_porch = 13,
                                                    .v_sync_width = 2,
                                                    .v_back_porch = 34,
                                                    .v_active_lines = 400,

                                                    .bit_clk_khz = 283200};

// 800x480@60Hz (CVT), 295 MHz bit clock
const struct dvi_timing dvi_timing_800x480p_60hz = {.h_sync_polarity = false,
                                                    .h_front_porch = 24,
                                                    .h_sync_width = 72,
                                                    .h_back_porch = 96,
                                                    .h_active_pixels = 800,

                                                    .v_sync_polarity = true,
                                                    .v_front_porch = 3,
                                                    .v_sync_width = 10,
                                                    .v_back_porch = 7,
                                                    .v_active_lines = 480,

                                                    .bit_clk_khz = 295200};

// 800x450@60Hz (CVT, 16:9)
const struct dvi_timing dvi_timing_800x450p_60hz = {.h_sync_polarity = false,
                                                    .h_front_porch = 24,
                                                    .h_sync_width = 72,
                                                    .h_back_porch = 96,
                                                    .h_active_pixels = 800,

                                                    .v_sync_polarity = true,
                                                    .v_front_porch = 3,
                                                    .v_sync_width = 5,
                                                    .v_back_porch = 10,
                                                    .v_active_lines = 450,

                                                    .bit_clk_khz = 278400};

// qHD 960x540@60Hz (CVT)
const struct dvi_timing dvi_timing_960x540p_60hz = {.h_sync_polarity = false,
                                                    .h_front_porch = 32,
                                                    .h_sync_width = 96,
                                                    .h_back_porch = 128,
                                                    .h_active_pixels = 960,

                                                    .v_sync_polarity = true,
                                                    .v_front_porch = 3,
                                                    .v_sync_width = 5,
                                                    .v_back_porch = 14,
                                                    .v_active_lines = 540,

                                                    .bit_clk_khz = 408000};

// qHD 960x540@50Hz (CVT)
const struct dvi_timing dvi_timing_960x540p_50hz = {.h_sync_polarity = false,
                                                    .h_front_porch = 24,
                                                    .h_sync_width = 96,
                                                    .h_back_porch = 120,
                                                    .h_active_pixels = 960,

                                                    .v_sync_polarity = true,
                                                    .v_front_porch = 3,
                                                    .v_sync_width = 5,
                                                    .v_back_porch = 11,
                                                    .v_active_lines = 540,

                                                    .bit_clk_khz = 336000};

// 1024x768 CVT-RB
const struct dvi_timing dvi_timing_1024x768_rb_60hz = {.h_sync_polarity = true,
                                                       .h_front_porch = 48,
                                                       .h_sync_width = 32,
                                                       .h_back_porch = 80,
                                                       .h_active_pixels = 1024,

                                                       .v_sync_polarity = false,
                                                       .v_front_porch = 3,
                                                       .v_sync_width = 4,
                                                       .v_back_porch = 15,
                                                       .v_active_lines = 768,

                                                       .bit_clk_khz = 560000};

// 720p@50Hz CVT-RB
const struct dvi_timing dvi_timing_1280x720p_rb_50hz = {
    .h_sync_polarity = true,
    .h_front_porch = 48,
    .h_sync_width = 32,
    .h_back_porch = 80,
    .h_active_pixels = 1280,

    .v_sync_polarity = false,
    .v_front_porch = 3,
    .v_sync_width = 5,
    .v_back_porch = 9,
    .v_active_lines = 720,

    .bit_clk_khz = 528000};

// 720p@60Hz CVT-RB
const struct dvi_timing dvi_timing_1280x720p_rb_60hz = {
    .h_sync_polarity = true,
    .h_front_porch = 48,
    .h_sync_width = 32,
    .h_back_porch = 80,
    .h_active_pixels = 1280,

    .v_sync_polarity = false,
    .v_front_porch = 3,
    .v_sync_width = 5,
    .v_back_porch = 13,
    .v_active_lines = 720,

    .bit_clk_khz = 640000};

// 1080p@30Hz CVT-RB2
const struct dvi_timing dvi_timing_1920x1080p_rb2_30hz = {
    .h_sync_polarity = true,
    .h_front_porch = 8,
    .h_sync_width = 32,
    .h_back_porch = 40,
    .h_active_pixels = 1920,

    .v_sync_polarity = false,
    .v_front_porch = 7,
    .v_sync_width = 8,
    .v_back_porch = 6,
    .v_active_lines = 1080,

    .bit_clk_khz = 660000};

// 1080p@48Hz YOLO
const struct dvi_timing dvi_timing_1920x1080p_yolo_48hz = {
    .h_sync_polarity = true,
    .h_front_porch = 8,
    .h_sync_width = 16,
    .h_back_porch = 24,
    .h_active_pixels = 1920,

    .v_sync_polarity = false,
    .v_front_porch = 4,
    .v_sync_width = 2,
    .v_back_porch = 6,
    .v_active_lines = 1080,

    .bit_clk_khz = 1032000};

// 1440p@24Hz YOLO
const struct dvi_timing dvi_timing_2560x1440p_yolo_24hz = {
    .h_sync_polarity = true,
    .h_front_porch = 8,
    .h_sync_width = 32,
    .h_back_porch = 20,
    .h_active_pixels = 2560,

    .v_sync_polarity = false,
    .v_front_porch = 2,
    .v_sync_width = 6,
    .v_back_porch = 2,
    .v_active_lines = 1440,

    .bit_clk_khz = 912000};

// 720p@30Hz CVT-RB H-timing, extended V blanking for 30 Hz.
// bit_clk = 480 MHz, pixel_clk = 48 MHz, H-sync = 33.3 kHz, V total = 1111.
const struct dvi_timing dvi_timing_1280x720p_reduced_30hz = {
    .h_sync_polarity = true,
    .h_front_porch = 48,
    .h_sync_width = 32,
    .h_back_porch = 80,
    .h_active_pixels = 1280,

    .v_sync_polarity = false,
    .v_front_porch = 3,
    .v_sync_width = 5,
    .v_back_porch = 383,
    .v_active_lines = 720,

    .bit_clk_khz = 480000};

// 720p@30Hz alt: original CVT-RB at half the 60Hz bit clock (319.2 MHz).
// H-sync = 22.2 kHz. For monitors that reject the standard timing above.
const struct dvi_timing dvi_timing_1280x720p_reduced_30hz_alt = {
    .h_sync_polarity = true,
    .h_front_porch = 48,
    .h_sync_width = 32,
    .h_back_porch = 80,
    .h_active_pixels = 1280,

    .v_sync_polarity = false,
    .v_front_porch = 3,
    .v_sync_width = 5,
    .v_back_porch = 13,
    .v_active_lines = 720,

    .bit_clk_khz = 319200};
