/*
 * Egis Technology Inc. (aka. LighTuning) EH576 driver for libfprint
 * Copyright (C) 2024 Ben (adapted from egis0575 by Animesh Sahu)
 *
 * Original egis0575 driver:
 * Copyright (C) 2021 Animesh Sahu <animeshsahu19@yahoo.com>
 *
 * Protocol reverse-engineered from Windows driver USB captures.
 * The EH576 (USB ID 1c7a:0576) uses a protocol nearly identical to the
 * EH575 (1c7a:0575): raw 8-bit grayscale frames over bulk USB, with an
 * EGIS/SIGE command framing layer.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

/*
 * Device configuration
 */

#define EGIS0576_CONFIGURATION 1
#define EGIS0576_INTERFACE     0

/*
 * USB endpoints
 *
 * The EH576 exposes four endpoints:
 *   EP 0x01 OUT - Bulk 512B  (commands sent to device)
 *   EP 0x82 IN  - Bulk 512B  (responses and image data from device)
 *   EP 0x83 IN  - Interrupt 16B (status events)
 *   EP 0x84 IN  - Interrupt 16B (status events)
 *
 * Only the bulk endpoints are used by this driver.
 */

#define EGIS0576_EPOUT 0x01  /* EP 1 OUT Bulk */
#define EGIS0576_EPIN  0x82  /* EP 2 IN  Bulk */

/*
 * Command/response framing
 *
 * Every command sent to the device starts with the ASCII string "EGIS"
 * (bytes 0x45 0x47 0x49 0x53), followed by an opcode and parameters.
 *
 * Every response from the device starts with "SIGE" (bytes 0x53 0x49
 * 0x47 0x45), followed by echoed parameters and status.
 *
 * Known opcodes:
 *   0x60 - Write register (short form): [EGIS, 0x60, reg, val] -> 7B response
 *   0x61 - Write register (alt form):   [EGIS, 0x61, reg, val] -> 7B response
 *   0x62 - Read register:               [EGIS, 0x62, reg, ?]   -> 10B response
 *   0x63 - Write config block:          [EGIS, 0x63, len, ...]  -> variable response
 *   0x64 - Capture image (finger):      [EGIS, 0x64, 0x14, 0xec] -> 5356B image
 *   0x73 - Capture image (background):  [EGIS, 0x73, 0x14, 0xec] -> 5356B image
 *
 * Packet sequences below were extracted from Wireshark captures of the
 * official Windows driver initializing the EH576 sensor.
 */

typedef struct Packet
{
  int            length;
  unsigned char *sequence;
  int            response_length;
} Packet;

/*
 * PRE_INIT_PACKETS: Full sensor calibration sequence (29 commands).
 *
 * This sequence is run when the device has not been calibrated since power-on
 * (detected by response[5] == 0x01 during POST_INIT command 2).  It performs
 * a background baseline capture (command 16, opcode 0x73) which the sensor
 * subtracts from subsequent finger images to produce clean prints.
 *
 * Without calibration the image capture returns mostly zero bytes.
 */
#define EGIS0576_PRE_INIT_PACKETS_LENGTH 29
static const Packet EGIS0576_PRE_INIT_PACKETS[] = {
  /* 1  */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x00,0x00}, .response_length = 7},
  /* 2  */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x01,0x00}, .response_length = 7},
  /* 3  */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x0a,0xfd}, .response_length = 7},
  /* 4  */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x35,0x02}, .response_length = 7},
  /* 5  */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x80,0x00}, .response_length = 7},
  /* 6  */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x80,0x00}, .response_length = 7},
  /* 7  */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x0a,0xfc}, .response_length = 7},
  /* 8  */ {.length = 9,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x63,0x01,0x02,0x0f,0x03}, .response_length = 9},
  /* 9  */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x0c,0x22}, .response_length = 7},
  /* 10 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x09,0x83}, .response_length = 7},
  /* 11 */ {.length = 13, .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x63,0x26,0x06,0x06,0x60,0x06,0x05,0x2f,0x06}, .response_length = 13},
  /* 12 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x0a,0xf4}, .response_length = 7},
  /* 13 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x0c,0x44}, .response_length = 7},
  /* 14 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x50,0x03}, .response_length = 7},
  /* 15 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x50,0x03}, .response_length = 7},
  /* 16 - Background calibration capture (5356B, may also return 7B if no data) */
           {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x73,0x14,0xec}, .response_length = 5356},
  /* 17 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x40,0xec}, .response_length = 7},
  /* 18 */ {.length = 18, .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x63,0x09,0x0b,0x83,0x24,0x00,0x44,0x0f,0x08,0x20,0x20,0x01,0x05,0x12}, .response_length = 18},
  /* 19 */ {.length = 13, .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x63,0x26,0x06,0x06,0x60,0x06,0x05,0x2f,0x06}, .response_length = 13},
  /* 20 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x23,0x00}, .response_length = 7},
  /* 21 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x24,0x33}, .response_length = 7},
  /* 22 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x20,0x00}, .response_length = 7},
  /* 23 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x21,0x66}, .response_length = 7},
  /* 24 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x00,0x66}, .response_length = 7},
  /* 25 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x01,0x66}, .response_length = 7},
  /* 26 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x40,0x66}, .response_length = 7},
  /* 27 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x0c,0x22}, .response_length = 7},
  /* 28 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x0b,0x03}, .response_length = 7},
  /* 29 - Final command; transitions to POST_INIT_PACKETS */
           {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x0a,0xfc}, .response_length = 7},
};

/*
 * POST_INIT_PACKETS: Normal startup sequence (18 commands).
 *
 * Executed every time the driver activates.  If command 2's response has
 * byte[5] == 0x01 the sensor needs full calibration; the driver switches to
 * PRE_INIT_PACKETS in that case.  The final command (18) triggers the first
 * finger-image capture and transitions to REPEAT_PACKETS.
 */
#define EGIS0576_POST_INIT_PACKETS_LENGTH 18
static const Packet EGIS0576_POST_INIT_PACKETS[] = {
  /* 1  */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x00,0xfc}, .response_length = 7},
  /* 2  - If response[5] == 0x01, switch to PRE_INIT */
           {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x01,0xfc}, .response_length = 7},
  /* 3  */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x40,0xfc}, .response_length = 7},
  /* 4  */ {.length = 18, .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x63,0x09,0x0b,0x83,0x24,0x00,0x44,0x0f,0x08,0x20,0x20,0x01,0x05,0x12}, .response_length = 18},
  /* 5  */ {.length = 13, .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x63,0x26,0x06,0x06,0x60,0x06,0x05,0x2f,0x06}, .response_length = 13},
  /* 6  */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x23,0x00}, .response_length = 7},
  /* 7  */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x24,0x33}, .response_length = 7},
  /* 8  */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x20,0x00}, .response_length = 7},
  /* 9  */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x21,0x66}, .response_length = 7},
  /* 10 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x00,0x66}, .response_length = 7},
  /* 11 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x01,0x66}, .response_length = 7},
  /* 12 */ {.length = 9,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x63,0x2c,0x02,0x00,0x57}, .response_length = 9},
  /* 13 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x2d,0x02}, .response_length = 7},
  /* 14 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x62,0x67,0x03}, .response_length = 10},
  /* 15 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x0f,0x03}, .response_length = 7},
  /* 16 */ {.length = 9,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x63,0x2c,0x02,0x00,0x13}, .response_length = 9},
  /* 17 */ {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x00,0x02}, .response_length = 7},
  /* 18 - First finger image capture; transitions to REPEAT_PACKETS */
           {.length = 7,  .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x64,0x14,0xec}, .response_length = 5356},
};

/*
 * REPEAT_PACKETS: Per-frame capture loop (9 commands).
 *
 * Executed in a loop for each subsequent finger image.  Command 9 triggers
 * the actual capture and returns the 5356-byte (103x52) grayscale frame.
 */
#define EGIS0576_REPEAT_PACKETS_LENGTH 9
static const Packet EGIS0576_REPEAT_PACKETS[] = {
  /* 1 */ {.length = 7, .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x61,0x2d,0x20}, .response_length = 7},
  /* 2 */ {.length = 7, .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x00,0x20}, .response_length = 7},
  /* 3 */ {.length = 7, .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x01,0x20}, .response_length = 7},
  /* 4 */ {.length = 9, .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x63,0x2c,0x02,0x00,0x57}, .response_length = 9},
  /* 5 */ {.length = 7, .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x2d,0x02}, .response_length = 7},
  /* 6 */ {.length = 7, .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x62,0x67,0x03}, .response_length = 10},
  /* 7 */ {.length = 9, .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x63,0x2c,0x02,0x00,0x13}, .response_length = 9},
  /* 8 */ {.length = 7, .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x60,0x00,0x02}, .response_length = 7},
  /* 9 - Finger image capture (5356B raw frame) */
          {.length = 7, .sequence = (unsigned char[]){0x45,0x47,0x49,0x53, 0x64,0x14,0xec}, .response_length = 5356},
};

/*
 * Image geometry
 *
 * The sensor USB response is 5356 bytes = 103 bytes/row × 52 rows.
 * However, USB capture analysis shows that only the leftmost ~69–76 columns
 * contain real fingerprint pixel data.  The remaining ~24–34 columns on the
 * right side are always zero (inactive sensor columns that return background
 * after PRE_INIT calibration subtracts the baseline).
 *
 * Using the full 103-column width creates an artificial "boundary" at x≈70
 * where fingerprint ridges abruptly end and transition to a zero region that
 * inverts to white (255).  This boundary produces position-sensitive false
 * minutiae that vary between finger presses, severely hurting bozorth3 matching.
 *
 * The fix is to crop each row to EGIS0576_IMGWIDTH active columns (80,
 * a multiple of 4 that safely includes all observed real pixel data) while
 * using EGIS0576_STRIDE = 103 as the row pitch in the raw USB buffer.
 *
 * This is still small for NBIS/bozorth3; we use a 2× upscale from 68×52 →
 * 136×104 pixels to improve minutiae count.  See libfprint issue #271.
 *
 * RFMGHEIGHT: number of rows extracted from each frame for assembly.
 * RFMDIS: number of rows skipped from the top before extracting.
 * For the EH576 all rows are valid, so RFMDIS=0 and RFMGHEIGHT=IMGHEIGHT.
 */
#define EGIS0576_STRIDE    103  /* bytes per row in the 5356-byte USB response */
#define EGIS0576_IMGWIDTH  68   /* active pixel columns to crop to (4-byte aligned, < 70 active cols) */
#define EGIS0576_IMGHEIGHT 52
#define EGIS0576_IMGSIZE   (EGIS0576_STRIDE * EGIS0576_IMGHEIGHT)  /* 5356 total USB bytes */

/* All 52 rows contain valid data; no rows need to be skipped. */
#define EGIS0576_RFMGHEIGHT EGIS0576_IMGHEIGHT  /* use all 52 rows */
#define EGIS0576_RFMDIS     0

/* One capture per press.  This is a press-mode sensor; each press yields a
 * single 103×52 frame.  Collecting multiple frames from the same static press
 * does not improve image quality and fpi_do_movement_estimation produces
 * noise-driven delta_y on identical frames. */
#define EGIS0576_CONSECUTIVE_CAPTURES 1

/* Minimum mean pixel value to accept a frame as containing a finger.
 * After SOFTWARE background subtraction, an empty sensor returns mean ≈ 1;
 * a light touch produces mean ≈ 11-20; a firm press gives mean ≈ 30-50.
 * Threshold of 12 accepts any reasonable touch while rejecting empty frames. */
#define EGIS0576_MIN_MEAN 12

/* Threshold for finger-OFF detection (hysteresis).
 * Must be well below MIN_MEAN to prevent frame-to-frame noise from
 * falsely triggering finger-off while the finger is still down.
 * No-finger frames have mean ≈ 1; even the lightest graze gives ≈ 11. */
#define EGIS0576_FINGER_OFF_MEAN 5

/* Ridge-contrast quality threshold, expressed as a fraction (NUM/DEN).
 *
 * For frames where the active pixel region is nearly uniform (all pixels
 * similar in value), the ratio mean/max approaches 1.  Such frames arise
 * when the finger barely grazes the sensor or presses so hard that ridges
 * and valleys cannot be distinguished.  Feeding them to NBIS produces only
 * spurious edge minutiae that corrupt the enrolled template and prevent
 * bozorth3 matching.
 *
 * Empirical data from USB captures and debug logging:
 *   Good frames (clear ridges):  active mean/max ≈ 0.55–0.65
 *   Over-pressed frames:         active mean/max ≈ 0.79     ← NBIS finds only 1 minutia
 *   Bad frames  (flat/no touch): active mean/max ≈ 0.88–0.90
 *
 * Reject any frame where mean/max > NUM/DEN = 3/4 = 0.75, which sits between
 * the good-frame range (0.55–0.65) and the over-pressed range (0.79+). */
#define EGIS0576_CONTRAST_NUMER 3
#define EGIS0576_CONTRAST_DENOM 4

/* 2× upscale.  The 68×52 native image is too small for NBIS V2 to reliably
 * compute the direction map (only ~9×7 map blocks, leaving ~6×4 valid inner
 * blocks after border removal — insufficient for LOFARM orientation
 * estimation).  Upscaling to 136×104 gives 17×13 = 221 blocks with 14×9 = 126
 * valid inner blocks, which is sufficient for NBIS to find 5–15 minutiae.
 *
 * Ridge period doubles from ~7 px (500 DPI) to ~14 px (1000 DPI).  The four
 * DFT waves (wavelengths 24, 12, 8, 6 px) still capture 14 px ridges through
 * the 12 px wave (1.7 ridges/window — borderline but workable).
 *
 * ppmm is set to (500 × RESIZE)/25.4 = 39.37 (1000 DPI) so that bozorth3
 * distances are scaled in physical mm, not pixels. */
#define EGIS0576_RESIZE  2

/* Mirror-padding added around the 2× upscaled image before NBIS processing.
 * NBIS border direction-map blocks (8 px each) are always INVALID because the
 * 24-px DFT window extends into uniform constant padding.  Mirror-reflecting
 * EGIS0576_MIRPAD pixels of real fingerprint content on all four sides gives
 * those border blocks valid DFT ridge directions, so ridge endpoints at the
 * original-image boundary are no longer removed as "near-invalid-block"
 * artifacts.  24 px = 3 × MAP_BLOCKSIZE_V2 ensures three full valid blocks
 * of reflected content between the padded edge and the original image area. */
#define EGIS0576_MIRPAD 24

/* bozorth3 matching threshold.  Default is 40; small values are needed for
 * this small sensor.  In testing, the enrolled finger scores 15–29, adjacent
 * fingers on the same hand score 3–8, and unrelated fingers score 0.
 * A threshold of 10 sits cleanly in the gap between adjacent-finger false
 * positives (≤8) and genuine matches (≥15). */
#define EGIS0576_BZ3_THRESHOLD 10

/* Number of enrollment stages.  The EH576 active area is only ~3.5×2.6 mm;
 * a 1-2 mm shift in finger placement between captures results in mostly
 * non-overlapping image regions.  Ten stages give libfprint ten
 * independent XYT templates covering different positions across the
 * fingertip, balancing coverage against enrollment convenience. */
#define EGIS0576_ENROLL_STAGES 10

#define EGIS0576_TIMEOUT 10000  /* ms */
