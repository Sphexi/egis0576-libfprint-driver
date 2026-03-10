# Technical notes: EgisTec EH576 driver

## Hardware overview

The EgisTec EH576 (USB 1c7a:0576) is a press-style optical fingerprint sensor.

| Property | Value |
|----------|-------|
| Active area | ~3.5 × 2.6 mm |
| Raw frame | 103 × 52 bytes (103-byte stride, 68 active columns) |
| Native resolution | ~500 DPI |
| USB interface | Bulk EP 0x01 OUT / EP 0x82 IN |
| Protocol framing | `EGIS` (host→device) / `SIGE` (device→host) 4-byte magic |

## Protocol

The driver plays back a fixed sequence of register-write and capture commands.
The sequence was reverse-engineered from Windows driver USB captures of both
the EH575 (1c7a:0575) and EH576 (1c7a:0576) — the protocol is nearly identical.

Each command is a bulk write starting with `45 47 49 53` ("EGIS"). Responses
start with `53 49 47 45` ("SIGE"). Image data follows the response header
directly in the capture response packet.

## Image pipeline

### 1. Frame assembly (68×52)

The raw 103-byte USB rows are cropped to the leftmost 68 active pixel columns.
A single frame is captured per scan (no stitching).

### 2. Dynamic range normalization

The sensor returns background-subtracted values. With a typical finger press
the maximum pixel value is only ~130/255. Without normalization, ridges end up
at ~125 gray after NBIS color inversion instead of 0 (black), severely limiting
minutiae detection.

The driver stretches to the full 0–255 range by scaling by `255/max_value`,
leaving zero pixels (no-finger background) unchanged.

### 3. Unsharp mask

A 5-tap separable box-blur unsharp mask (`strength = 1.5×`) sharpens the
ridge/valley boundary before passing to NBIS:

```
blur = box_blur_5tap(image)
output = clamp((5 × image − 3 × blur) / 2, 0, 255)
```

### 4. 2× bilinear upscale → 136×104 px

The 68×52 native image has ridges only ~2–3 px wide. NBIS's directional
binarization grid (7×7 px) cannot reliably threshold single-pixel ridges.
A 2× bilinear upscale widens ridges to ~4–6 px, making them legible to NBIS.
`ppmm` is set to `(500 × 2) / 25.4 ≈ 39.4` to keep bozorth3 distances in
physical millimetres.

### 5. Mirror padding → 184×152 px  *(the key fix)*

**Problem:** NBIS (mindtct V2) computes a ridge-direction map by dividing the
image into 8×8 pixel blocks and running a 24×24 DFT window over each block.
At the image boundary, the DFT window extends outside the image into NBIS's
internal uniform constant-value padding. Blocks with uniform padding have no
measurable DFT power and are marked `INVALID_DIR`.

`remove_near_invblock_V2()` then removes any minutia that is adjacent to an
`INVALID_DIR` block (unless the invalid block itself has ≥7 valid neighbours —
which border blocks do not). Because the EH576's 136×104 image is small enough
that every ridge enters and exits through these invalid border blocks, *all*
ridge endpoints are adjacent to invalid blocks and get removed. Only 1–2 rare
interior minutiae survive — far too few for bozorth3.

**Fix:** Before passing the image to NBIS, the driver mirror-reflects
`EGIS0576_MIRPAD = 24` pixels (3 × `MAP_BLOCKSIZE_V2`) of real fingerprint
content on all four sides. The reflected content gives the border DFT blocks
a valid ridge direction, so `remove_near_invblock_V2()` no longer removes
ridge endpoints at the original image boundary. Typical result: 12–25 minutiae
per capture vs 1–2 without the padding.

```
Before:   136×104  →  ~1–2 minutiae  →  bozorth3 score 0  →  verify-no-match
After:    184×152  →  12–25 minutiae →  bozorth3 score 15–29 → verify-match
```

bozorth3 uses relative minutia geometry, so the uniform 24-px positional
offset between enroll and verify captures cancels out automatically.

## NBIS parameter context

NBIS was designed for full rolled/slap fingerprint images (typically 500×500 px
or larger). Several thresholds become relevant for this small sensor:

| Parameter | Value | Effect at small scale |
|-----------|-------|----------------------|
| `MAP_BLOCKSIZE_V2` | 8 px | One block ≈ one ridge period at RESIZE=2 |
| `MAP_WINDOWSIZE_V2` | 24 px | DFT window covers ~1.7 ridge periods |
| `MAX_HOOK_LEN_V2` | 30 px | Removes hooks < ~2 ridge periods |
| `SMALL_LOOP_LEN` | 15 px | Removes loops < ~1 ridge period |
| `INV_BLOCK_MARGIN_V2` | 4 px | Every pixel is in a block "margin" |

The mirror padding fixes the border-block validity issue without requiring any
changes to these NBIS constants, keeping the patch minimally invasive.

## bozorth3 threshold

| Condition | Typical score |
|-----------|--------------|
| Enrolled finger, good placement | 15–29 |
| Adjacent finger (e.g. middle next to enrolled index) | 3–8 |
| Unrelated finger | 0 |

`EGIS0576_BZ3_THRESHOLD = 10` sits cleanly in the gap between adjacent-finger
false positives (≤8) and genuine matches (≥15).

## Enrollment stages

`EGIS0576_ENROLL_STAGES = 7` — more than the default of 3. The sensor's
~3.5×2.6 mm active area means even a 1–2 mm shift in finger placement between
enroll and verify captures a partially non-overlapping image region. Seven
enrollment templates spread across slightly different press positions
substantially improve the probability that at least one overlaps any given
verify capture. This is the same approach used by the `elanspi` driver.
