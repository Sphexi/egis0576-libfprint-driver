# libfprint driver for EgisTec EH576 (USB 1c7a:0576)

A [libfprint](https://gitlab.freedesktop.org/libfprint/libfprint) driver for the
LighTuning/EgisTec EH576 optical fingerprint sensor, found in several Lenovo
ThinkPad laptop models (e.g. ThinkPad E14 Gen 2 AMD).

## Status

Functional. Enrollment, verification, and PAM integration all work reliably.

## Supported hardware

| USB ID | Device | Notes |
|--------|--------|-------|
| `1c7a:0576` | LighTuning EgisTec EH576 | Confirmed working |

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
git apply /path/to/patches/0001-libfprint-Add-driver-for-EgisTec-EH576-fingerprint-s.patch

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
