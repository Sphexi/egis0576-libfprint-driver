# Installation guide

## Prerequisites

```sh
# Debian/Ubuntu/Kubuntu
sudo apt install git meson ninja-build libglib2.0-dev libgudev-1.0-dev \
    libusb-1.0-0-dev libgusb-dev libnss3-dev libpixman-1-dev \
    gtk-doc-tools libgirepository1.0-dev gir1.2-gusb-2.0 \
    fprintd libpam-fprintd
```

## Build libfprint with the driver

```sh
# 1. Clone upstream libfprint
git clone https://gitlab.freedesktop.org/libfprint/libfprint.git
cd libfprint

# 2. Apply the driver patch
git apply /path/to/egis0576-driver/patches/0001-libfprint-Add-driver-for-EgisTec-EH576-fingerprint-s.patch

# 3. Build
meson setup builddir
ninja -C builddir

# 4. Install
sudo ninja -C builddir install
sudo ldconfig
```

## udev rules

The patch adds the device to `data/autosuspend.hwdb`. To apply:

```sh
sudo systemd-hwdb update
sudo udevadm trigger
```

If you installed from the patch, `sudo ninja -C builddir install` handles
this automatically. Otherwise, add a rule manually:

```sh
echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="1c7a", ATTRS{idProduct}=="0576", \
    MODE="0664", GROUP="plugdev"' | sudo tee /etc/udev/rules.d/60-libfprint-egis0576.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
sudo usermod -aG plugdev "$USER"
# Log out and back in for group membership to take effect
```

## Enroll a fingerprint

```sh
fprintd-enroll -f right-index-finger "$USER"
```

Follow the prompts — 7 scans are required. The extra stages improve matching
reliability given the sensor's small active area (~3.5 × 2.6 mm).

## Test verification

```sh
fprintd-verify -f right-index-finger "$USER"
```

Or use the interactive acceptance test script:

```sh
python3 tests/test_fingerprint.py
```

## PAM integration (sudo, lock screen)

```sh
sudo pam-auth-update --enable fprintd
```

This adds fingerprint authentication to `/etc/pam.d/common-auth`. The default
configuration tries the fingerprint once, then falls back to password.

To check the resulting config:

```sh
grep fprintd /etc/pam.d/common-auth
```

Expected:
```
auth    [success=2 default=ignore]    pam_fprintd.so max-tries=1 timeout=10
```

## Troubleshooting

**Enroll fails immediately**
- Check `lsusb | grep 1c7a` — device must be visible
- Check udev group: `groups "$USER"` must include `plugdev`
- Check fprintd is running: `systemctl status fprintd`

**Verify always returns no-match**
- Re-enroll: placement consistency matters on this small sensor
- Check `journalctl -u fprintd | grep score` — scores below 10 indicate
  poor image quality or significant finger position change between enroll
  and verify

**journalctl logs**
```sh
journalctl -u fprintd -f
```

Debug output (when libfprint built with debug enabled):
```sh
G_MESSAGES_DEBUG=all fprintd --no-daemon
```
