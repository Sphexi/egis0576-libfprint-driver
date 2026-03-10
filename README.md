# libfprint driver for EgisTec EH576 (USB 1c7a:0576)

> **Use at your own risk.** This driver was developed and tested on a single
> Lenovo Yoga laptop. It has not been tested on any other hardware. It may or
> may not work for you, and could behave unexpectedly on other machines or
> kernel versions. No warranty is provided — see [LICENSE](LICENSE).

> **Vibe-coded with Claude.** This entire driver — from USB protocol
> reverse-engineering through NBIS image pipeline debugging to the mirror-padding
> fix — was developed interactively with
> [Claude Sonnet](https://claude.ai) (Anthropic) using Claude Code. The human
> provided the hardware, USB captures, test feedback, and direction; Claude
> wrote and debugged the C code, diagnosed the NBIS minutiae failure, and
> authored the documentation. This is an experiment in AI-assisted low-level
> systems programming.

---

A [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) driver for the
LighTuning/EgisTec EH576 optical fingerprint sensor, found in some Lenovo
Yoga and ThinkPad laptop models.

## Status

Functional on the tested hardware. Enrollment, verification, and PAM
integration all work reliably on one Lenovo Yoga running Kubuntu 24.04.

## Supported hardware

| USB ID | Device | Tested on |
|--------|--------|-----------|
| `1c7a:0576` | LighTuning EgisTec EH576 | Lenovo Yoga (Kubuntu 24.04) |

To check if you have this sensor:

```sh
lsusb | grep 1c7a
```

Expected output:

```
Bus 001 Device 003: ID 1c7a:0576 LighTuning Technology Inc. EgisTec EH576
```

## Quick install

See [INSTALL.md](INSTALL.md) for full build instructions. In brief:

```sh
# Clone libfprint and apply the patch
git clone https://gitlab.freedesktop.org/libfprint/libfprint.git
cd libfprint
git apply /path/to/egis0576-driver/patches/0001-libfprint-Add-driver-for-EgisTec-EH576-fingerprint-s.patch

# Build and install
meson setup builddir
ninja -C builddir
sudo ninja -C builddir install
sudo ldconfig

# Enroll a finger
fprintd-enroll -f right-index-finger "$USER"

# Verify
fprintd-verify -f right-index-finger "$USER"
```

## How it works

The EH576 returns 103×52 byte raw frames over bulk USB. The driver:

1. Plays back a fixed register-write + capture sequence (reverse-engineered
   from Windows driver USB captures)
2. Crops the active 68×52 pixel columns from the 103-byte stride
3. Normalizes the dynamic range to 0–255
4. Applies an unsharp mask for ridge contrast
5. 2× bilinear upscales to 136×104 at 1000 DPI
6. **Mirror-pads by 24 px on all sides** to fix NBIS minutiae extraction
7. Passes the resulting 184×152 image to libfprint's NBIS pipeline

The mirror-padding step is the key insight — see [TECHNICAL.md](TECHNICAL.md)
for a full explanation.

## Files

| File | Description |
|------|-------------|
| `src/egis0576.c` | Driver implementation |
| `src/egis0576.h` | Configuration constants and protocol tables |
| `patches/0001-*.patch` | `git format-patch` ready for upstream submission |
| `tests/test_fingerprint.py` | Interactive acceptance test |
| `INSTALL.md` | Build and installation guide |
| `TECHNICAL.md` | How the image pipeline works and why |

## Acknowledgements

This driver stands on the shoulders of earlier work:

- **[egis0575](https://github.com/Animeshz/linux-scripts) by Animesh Sahu**
  (`Copyright (C) 2021 Animesh Sahu <animeshsahu19@yahoo.com>`) — the direct
  protocol ancestor. The EH576 USB command sequence was adapted from this driver,
  which was itself reverse-engineered from Windows driver USB captures by
  [Animeshz](https://github.com/Animeshz) and Pengu601.

- **[egis0570](https://gitlab.freedesktop.org/libfprint/libfprint/-/blob/master/libfprint/drivers/egis0570.c)
  in libfprint upstream** by Maxim Kolesnikov and Saeed/Ali Rk — used as a
  structural reference for the libfprint FpImageDevice driver framework.

## Development

Developed in a single extended session using [Claude Code](https://claude.ai/claude-code)
(Claude Sonnet 4.6, Anthropic). The session covered:

- USB protocol analysis from Windows driver captures
- Raw image quality analysis (DFT simulation, NCC correlation, ASCII ridge visualisation)
- NBIS pipeline debugging (direction map validity, binarization analysis, minutiae removal tracing)
- Discovery and fix of the NBIS border-block INVALID_DIR problem via mirror padding
- bozorth3 threshold tuning from live score data
- PAM integration and acceptance testing

Co-authored-by: Claude Sonnet 4.6 <noreply@anthropic.com>
