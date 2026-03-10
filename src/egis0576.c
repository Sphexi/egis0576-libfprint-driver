/*
 * Egis Technology Inc. (aka. LighTuning) EH576 driver for libfprint
 * Copyright (C) 2024 Ben (adapted from egis0575 by Animesh Sahu)
 *
 * Original egis0575 driver:
 * Copyright (C) 2021 Animesh Sahu <animeshsahu19@yahoo.com>
 *
 * Protocol reverse-engineered from Windows driver USB captures of both the
 * EH575 (1c7a:0575) and EH576 (1c7a:0576) sensors by Animeshz and Pengu601.
 *
 * The EH576 is a swipe-style optical fingerprint sensor that returns raw
 * 8-bit grayscale frames (103x52 pixels).  The driver works by playing back
 * a fixed sequence of register-write and capture commands over bulk USB, then
 * assembling multiple frames into a composite image for matching.
 *
 * Protocol summary:
 *   - All commands start with the magic header "EGIS" (0x45 0x47 0x49 0x53)
 *   - All responses start with "SIGE" (0x53 0x49 0x47 0x45)
 *   - EP 0x01 OUT (bulk): host -> device commands
 *   - EP 0x82 IN  (bulk): device -> host responses and image data
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

#define FP_COMPONENT "egis0576"

#include "egis0576.h"
#include "drivers_api.h"

/* ========================================================================= *
 *  Device state structure
 * ========================================================================= */

struct _FpDeviceEgis0576
{
  FpImageDevice parent;

  gboolean      running;   /* SSM loop is active */
  gboolean      stop;      /* deactivation requested */

  GSList       *strips;      /* collected image strips (fpi_frame*) */
  gsize         strips_len;  /* number of strips collected */

  /* Current packet array being walked */
  const Packet *pkt_array;
  int           pkt_array_len;
  int           current_index;
};

enum sm_states {
  SM_INIT,        /* initialise driver state, select packet array */
  SM_START,       /* check stop flag; loop head */
  SM_REQ,         /* send next command packet to device */
  SM_RESP,        /* read device response */
  SM_PROCESS_IMG, /* assemble and report captured image */
  SM_DONE,        /* one frame done; jump back to SM_START */
  SM_STATES_NUM
};

G_DECLARE_FINAL_TYPE (FpDeviceEgis0576, fpi_device_egis0576, FPI, DEVICE_EGIS0576, FpImageDevice);
G_DEFINE_TYPE (FpDeviceEgis0576, fpi_device_egis0576, FP_TYPE_IMAGE_DEVICE);

/* ========================================================================= *
 *  Frame assembler helpers
 * ========================================================================= */

static unsigned char
egis_get_pixel (struct fpi_frame_asmbl_ctx *ctx,
                struct fpi_frame           *frame,
                unsigned int                x,
                unsigned int                y)
{
  if (x >= (unsigned int) ctx->frame_width)
    return 0;
  return frame->data[x + y * ctx->frame_width];
}

/* EGIS0576_IMGWIDTH is 68, which is already a multiple of 4, so no padding
 * column is required and image_width == frame_width. */
#define EGIS0576_ASMBL_WIDTH EGIS0576_IMGWIDTH  /* 68, 4-byte aligned */

static struct fpi_frame_asmbl_ctx assembling_ctx = {
  .frame_width  = EGIS0576_IMGWIDTH,
  .frame_height = EGIS0576_RFMGHEIGHT,
  .image_width  = EGIS0576_ASMBL_WIDTH,
  .get_pixel    = egis_get_pixel,
};

/* ========================================================================= *
 *  Image data validation
 * ========================================================================= */

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

/*
 * valid_data: returns TRUE if the first 100 bytes of a transfer are not all
 * zero.  All-zero responses indicate the sensor hasn't been calibrated or no
 * finger was placed in time.
 */
static gboolean
valid_data (FpiUsbTransfer *transfer)
{
  int sum = 0;

  for (size_t i = 0; i < MIN (100, transfer->actual_length); i++)
    sum |= transfer->buffer[i];
  return sum != 0;
}

/*
 * finger_present: returns TRUE when the mean pixel value in the frame
 * exceeds EGIS0576_MIN_MEAN.
 *
 * After PRE_INIT calibration the sensor returns near-zero data when no
 * finger is present (background-subtracted baseline).  A real fingerprint
 * push raises the mean to ≈53 (measured by USB capture with a finger).
 * Mean-based detection is simpler and more reliable than variance for this
 * sensor; it matches the approach used by the egis0570 driver.
 */
static gboolean
finger_present (FpiUsbTransfer *transfer)
{
  unsigned char *buf    = transfer->buffer;
  int            length = transfer->actual_length;
  double         mean   = 0.0;

  for (int i = 0; i < length; i++)
    mean += buf[i];
  mean /= length;

  return mean > (double) EGIS0576_MIN_MEAN;
}

/*
 * good_quality: returns TRUE when the frame has sufficient ridge contrast.
 *
 * Low-contrast "uniform press" frames (no visible ridge structure) pass the
 * finger_present() mean check but produce only spurious edge minutiae in NBIS,
 * corrupting the enrolled template.  We detect them via the ratio mean/max of
 * the active pixel columns: good frames have clear ridges (some pixels much
 * brighter than others) so mean/max is low (~0.60–0.72); uniform-press frames
 * have nearly flat intensity so mean/max approaches 1.0 (~0.88–0.90).
 *
 * Reject when: mean/max > NUMER/DENOM
 * i.e. when:   sum * DENOM > NUMER * npix * max_val   (integer arithmetic)
 */
static gboolean
good_quality (FpiUsbTransfer *transfer)
{
  guint    max_val = 0;
  guint32  sum     = 0;
  guint    npix    = EGIS0576_IMGWIDTH * EGIS0576_RFMGHEIGHT;

  for (int row = 0; row < EGIS0576_RFMGHEIGHT; row++)
    for (int col = 0; col < EGIS0576_IMGWIDTH; col++)
      {
        guint8 v = transfer->buffer[(EGIS0576_RFMDIS + row) * EGIS0576_STRIDE + col];
        if (v > max_val)
          max_val = v;
        sum += v;
      }

  if (max_val == 0)
    return FALSE;

  /* mean/max <= NUMER/DENOM  ⟺  sum * DENOM <= NUMER * npix * max_val */
  return (sum * (guint32) EGIS0576_CONTRAST_DENOM <=
          (guint32) EGIS0576_CONTRAST_NUMER * npix * max_val);
}

/* ========================================================================= *
 *  Image enhancement
 * ========================================================================= */

/*
 * apply_unsharp_mask: enhance local ridge/valley contrast using a 5-tap
 * separable box-blur unsharp mask.
 *
 * Algorithm: blur = box_blur5(data);  out = 2.5*data - 1.5*blur
 * Integer form: out = clamp((5*data - 3*blur) / 2, 0, 255)
 */
static void
apply_unsharp_mask (guint8 *data, guint w, guint h)
{
  guint   npix = w * h;
  guint8 *tmp  = g_malloc (npix);
  guint8 *blur = g_malloc (npix);

  /* Horizontal 5-tap box blur */
  for (guint row = 0; row < h; row++)
    for (guint col = 0; col < w; col++)
      {
        guint sum = 0;
        for (int k = -2; k <= 2; k++)
          {
            int c2 = (int) col + k;
            if (c2 < 0)
              c2 = 0;
            else if (c2 >= (int) w)
              c2 = (int) w - 1;
            sum += data[row * w + (guint) c2];
          }
        tmp[row * w + col] = (guint8) (sum / 5);
      }

  /* Vertical 5-tap box blur */
  for (guint col = 0; col < w; col++)
    for (guint row = 0; row < h; row++)
      {
        guint sum = 0;
        for (int k = -2; k <= 2; k++)
          {
            int r2 = (int) row + k;
            if (r2 < 0)
              r2 = 0;
            else if (r2 >= (int) h)
              r2 = (int) h - 1;
            sum += tmp[(guint) r2 * w + col];
          }
        blur[row * w + col] = (guint8) (sum / 5);
      }

  g_free (tmp);

  /* Unsharp combine: out = 2.5×orig − 1.5×blur = (5×orig − 3×blur) / 2 */
  for (guint i = 0; i < npix; i++)
    {
      int v = (5 * (int) data[i] - 3 * (int) blur[i]) / 2;
      data[i] = (guint8) (v < 0 ? 0 : v > 255 ? 255 : v);
    }

  g_free (blur);
}

/*
 * mirror_pad_image: surround an FpImage with a border of mirror-reflected
 * content and return the new (larger) FpImage.
 *
 * NBIS marks direction-map blocks at the image border INVALID because its
 * 24-pixel DFT window extends into the uniform constant-value padding it
 * adds internally.  Invalid border blocks cause every ridge endpoint that
 * touches the border to be removed by remove_near_invblock_V2(), leaving
 * only the rare interior minutia.
 *
 * Reflecting PAD pixels of real fingerprint content on all four sides gives
 * the NBIS DFT window real ridge structure even at the edge blocks, so those
 * blocks get valid directions and the ridge endpoints in the original image
 * area survive.  Bozorth3 matching uses relative minutia geometry, so the
 * uniform PAD-pixel offset between enroll and verify cancels out.
 *
 * PAD = 3 × MAP_BLOCKSIZE_V2 (3 × 8 = 24 px) is sufficient to cover the
 * 24-pixel DFT window and ensure three full valid blocks between the padded
 * edge and the original image interior.
 */
static FpImage *
mirror_pad_image (FpImage *src)
{
  const guint  sw = src->width;
  const guint  sh = src->height;
  const guint  pw = sw + 2 * EGIS0576_MIRPAD;
  const guint  ph = sh + 2 * EGIS0576_MIRPAD;
  const guint8 *s = src->data;

  FpImage *dst = fp_image_new ((gint) pw, (gint) ph);

  dst->ppmm  = src->ppmm;
  dst->flags = src->flags;

  guint8 *d = dst->data;

  for (guint dy = 0; dy < ph; dy++)
    {
      /* Map destination row to source row via mirror reflection. */
      guint sy;
      if (dy < EGIS0576_MIRPAD)
        sy = EGIS0576_MIRPAD - dy - 1;           /* top mirror  */
      else if (dy >= EGIS0576_MIRPAD + sh)
        sy = 2 * sh - (dy - EGIS0576_MIRPAD) - 1; /* bottom mirror */
      else
        sy = dy - EGIS0576_MIRPAD;               /* original    */
      if (sy >= sh)
        sy = sh - 1;                             /* safety clamp */

      for (guint dx = 0; dx < pw; dx++)
        {
          guint sx;
          if (dx < EGIS0576_MIRPAD)
            sx = EGIS0576_MIRPAD - dx - 1;           /* left mirror  */
          else if (dx >= EGIS0576_MIRPAD + sw)
            sx = 2 * sw - (dx - EGIS0576_MIRPAD) - 1; /* right mirror */
          else
            sx = dx - EGIS0576_MIRPAD;               /* original     */
          if (sx >= sw)
            sx = sw - 1;                             /* safety clamp */

          d[dy * pw + dx] = s[sy * sw + sx];
        }
    }

  return dst;
}

/* ========================================================================= *
 *  Image frame accumulation and assembly
 * ========================================================================= */

/*
 * save_img: called after each image-returning packet (opcode 0x64).
 *
 * Validates the raw frame, optionally appends it to the strip list, and
 * decides whether to request more frames or to assemble and report.
 */
static void
save_img (FpiUsbTransfer *transfer, FpDevice *dev)
{
  FpDeviceEgis0576 *self = FPI_DEVICE_EGIS0576 (dev);

  if (!valid_data (transfer))
    {
      /* Sensor returned all-zero data — not calibrated yet or no finger.
       * Loop back and try the next capture rather than failing the session. */
      fpi_ssm_jump_to_state (transfer->ssm, SM_REQ);
      return;
    }

  if (self->stop)
    {
      g_slist_free_full (self->strips, g_free);
      self->strips_len = 0;
      self->strips     = NULL;
      fpi_ssm_jump_to_state (transfer->ssm, SM_DONE);
      return;
    }

  if (!finger_present (transfer))
    {
      /* Finger lifted mid-swipe: if we have enough strips, assemble now */
      if (self->strips_len > 0)
        goto start_processing;
    }
  else if (!good_quality (transfer))
    {
      /* Low-contrast "uniform press" frame — no visible ridge structure.
       * Discard silently and request another capture. */
      fp_dbg ("Frame rejected: insufficient ridge contrast (low mean/max ratio)");
      fpi_ssm_jump_to_state (transfer->ssm, SM_REQ);
      return;
    }
  else
    {
      /* Copy the active pixel columns (EGIS0576_IMGWIDTH = 80) from each of
       * the RFMGHEIGHT rows.  The raw USB buffer has EGIS0576_STRIDE = 103
       * bytes per row; we copy only the leftmost IMGWIDTH bytes per row,
       * discarding the inactive zero-padding columns on the right. */
      struct fpi_frame *stripe =
        g_malloc (EGIS0576_IMGWIDTH * EGIS0576_RFMGHEIGHT + sizeof (struct fpi_frame));
      stripe->delta_x = 0;
      stripe->delta_y = 0;
      for (int row = 0; row < EGIS0576_RFMGHEIGHT; row++)
        memcpy (stripe->data + row * EGIS0576_IMGWIDTH,
                transfer->buffer + (EGIS0576_RFMDIS + row) * EGIS0576_STRIDE,
                EGIS0576_IMGWIDTH);
      self->strips = g_slist_prepend (self->strips, stripe);
      self->strips_len += 1;
    }

  if (self->strips_len < EGIS0576_CONSECUTIVE_CAPTURES)
    {
      /* Need more frames: loop back to send next command sequence */
      fpi_ssm_jump_to_state (transfer->ssm, SM_REQ);
      return;
    }

start_processing:
  fpi_ssm_next_state (transfer->ssm);  /* -> SM_PROCESS_IMG */
}

/*
 * process_imgs: assemble collected strips into a composite image and hand it
 * off to libfprint's image-device layer.
 */
static void
process_imgs (FpiSsm *ssm, FpDevice *dev)
{
  FpImageDevice        *img_self = FP_IMAGE_DEVICE (dev);
  FpDeviceEgis0576     *self     = FPI_DEVICE_EGIS0576 (dev);
  FpiImageDeviceState   state;

  fpi_image_device_report_finger_status (img_self, TRUE);

  g_object_get (dev, "fpi-image-device-state", &state, NULL);
  if (state == FPI_IMAGE_DEVICE_STATE_CAPTURE)
    {
      if (!self->stop)
        {
          g_autoptr(FpImage) img = NULL;

          self->strips = g_slist_reverse (self->strips);
          fpi_do_movement_estimation (&assembling_ctx, self->strips);
          img = fpi_assemble_frames (&assembling_ctx, self->strips);

          /* Normalize image to the full 0-255 dynamic range.
           *
           * The sensor returns background-subtracted values; with a typical
           * press the max pixel value is only ~130 out of 255.  After
           * FPI_IMAGE_COLORS_INVERTED the ridges end up at ~125 (light gray)
           * instead of 0 (black), which severely limits NBIS minutiae
           * detection.  Stretching to full range maps ridges to 255 in raw
           * (→ 0 = black after inversion) and gives NBIS the contrast it needs.
           *
           * The no-finger region (raw = 0) is not affected by this scaling
           * because we scale by max, not by (max-min): zeros stay at zero and
           * invert to 255 (white background) as expected.
           */
          {
            guint8 *data    = img->data;
            guint   npix    = img->width * img->height;
            guint8  max_val = 0;

            for (guint i = 0; i < npix; i++)
              if (data[i] > max_val)
                max_val = data[i];

            if (max_val > 0 && max_val < 255)
              for (guint i = 0; i < npix; i++)
                data[i] = (guint8) ((data[i] * 255u) / max_val);
          }

          /* Enhance local ridge/valley contrast before passing to NBIS. */
          apply_unsharp_mask (img->data, img->width, img->height);

          /* FPI_IMAGE_COLORS_INVERTED: after normalization, ridges are bright
           * (255) and valleys/background are dark (0 or low).  Inversion maps
           * ridges → 0 (black) and background → 255 (white), the convention
           * NBIS/mindtct expects for minutiae extraction.
           *
           * Do NOT set FPI_IMAGE_PARTIAL: that flag causes NBIS to call
           * remove_perimeter_pts=TRUE, which REMOVES minutiae within 10 px of
           * the fingerprint data boundary.  For our small sensor those perimeter
           * minutiae are valid and essential for a successful match.  The default
           * (no PARTIAL) is remove_perimeter_pts=FALSE — keep all minutiae.
           */
          img->flags |= FPI_IMAGE_COLORS_INVERTED;

          /* 2× upscale (EGIS0576_RESIZE=2): ridges widen from ~1–2 px to ~2–4 px,
           * giving NBIS a more stable ridge skeleton after binarisation+thinning.
           * Without upscale the 1-pixel-wide ridges at 68×52 produce a fragmented
           * skeleton with almost no detectable minutiae.
           * ppmm = (500 × RESIZE)/25.4 keeps bozorth3 distances in physical mm. */
          FpImage *resized = fpi_image_resize (img, EGIS0576_RESIZE, EGIS0576_RESIZE);
          resized->ppmm = (500.0 * EGIS0576_RESIZE) / 25.4;

          /* Mirror-pad the resized image so NBIS border blocks see real ridge
           * structure and are marked VALID by the DFT direction map.  Without
           * this, all ridges terminating at the image border are removed as
           * "near-invalid-block" artifacts, leaving only 1–2 interior minutiae.
           * See mirror_pad_image() for a detailed explanation. */
          FpImage *padded = mirror_pad_image (resized);
          g_object_unref (resized);

          fpi_image_device_image_captured (img_self, padded);
        }

      g_slist_free_full (self->strips, g_free);
      self->strips     = NULL;
      self->strips_len = 0;

      fpi_image_device_report_finger_status (img_self, FALSE);
      fpi_ssm_next_state (ssm);  /* -> SM_DONE */
    }
}

/* ========================================================================= *
 *  USB I/O helpers
 * ========================================================================= */

/*
 * resp_cb: completion callback for all device responses.
 *
 * Drives the packet-sequence state machine:
 *  - On error: log which array/index failed, propagate error.
 *  - At end of POST_INIT or REPEAT: image is embedded in the last response;
 *    call save_img() to process it, then switch to REPEAT array.
 *  - If POST_INIT[1].response[5] == 0x01: sensor needs calibration; switch
 *    to PRE_INIT array.
 *  - Otherwise: advance index and loop back to SM_REQ.
 */
static void
resp_cb (FpiUsbTransfer *transfer,
         FpDevice       *dev,
         gpointer        user_data,
         GError         *error)
{
  FpDeviceEgis0576 *self = FPI_DEVICE_EGIS0576 (dev);

  if (error)
    {
      const char *array_name = "pre-init";
      if (self->pkt_array == EGIS0576_POST_INIT_PACKETS)
        array_name = "post-init";
      else if (self->pkt_array == EGIS0576_REPEAT_PACKETS)
        array_name = "repeat";

      fp_dbg ("Error at index %d of %s array: %s",
              self->current_index, array_name, error->message);
      fpi_ssm_mark_failed (transfer->ssm, error);

      g_slist_free_full (self->strips, g_free);
      self->strips_len = 0;
      self->strips     = NULL;
      return;
    }

  if (self->current_index == self->pkt_array_len - 1)
    {
      /*
       * Last packet in POST_INIT or REPEAT carries the image data.
       * Switch unconditionally to REPEAT for subsequent captures.
       */
      if (self->pkt_array == EGIS0576_REPEAT_PACKETS ||
          self->pkt_array == EGIS0576_POST_INIT_PACKETS)
        {
          self->pkt_array     = EGIS0576_REPEAT_PACKETS;
          self->pkt_array_len = EGIS0576_REPEAT_PACKETS_LENGTH;
          self->current_index = 0;
          save_img (transfer, dev);
          return;
        }
      else
        {
          /* PRE_INIT complete: move to POST_INIT */
          self->pkt_array     = EGIS0576_POST_INIT_PACKETS;
          self->pkt_array_len = EGIS0576_POST_INIT_PACKETS_LENGTH;
          self->current_index = 0;
        }
    }
  else if (self->pkt_array == EGIS0576_POST_INIT_PACKETS &&
           self->current_index == 1 &&
           transfer->actual_length >= 6 &&
           transfer->buffer[5] == 0x01)
    {
      /*
       * POST_INIT command 2 response[5] == 0x01 means the sensor has not
       * been calibrated since power-on.  Full PRE_INIT calibration required.
       */
      fp_dbg ("Sensor needs calibration — switching to PRE_INIT sequence");
      self->pkt_array     = EGIS0576_PRE_INIT_PACKETS;
      self->pkt_array_len = EGIS0576_PRE_INIT_PACKETS_LENGTH;
      self->current_index = 0;
    }
  else
    {
      self->current_index += 1;
    }

  fpi_ssm_jump_to_state (transfer->ssm, SM_REQ);
}

static void
recv_resp (FpiSsm *ssm, FpDevice *dev, int response_length)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk (transfer, EGIS0576_EPIN, response_length);
  transfer->ssm = ssm;
  fpi_usb_transfer_submit (transfer, EGIS0576_TIMEOUT, NULL, resp_cb, NULL);
}

static void
send_req (FpiSsm *ssm, FpDevice *dev, const Packet *pkt)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk_full (transfer, EGIS0576_EPOUT,
                                   pkt->sequence, pkt->length, NULL);
  transfer->ssm          = ssm;
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_submit (transfer, EGIS0576_TIMEOUT, NULL,
                            fpi_ssm_usb_transfer_cb, NULL);
}

/* ========================================================================= *
 *  SSM state machine
 * ========================================================================= */

static void
ssm_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpDeviceEgis0576 *self    = FPI_DEVICE_EGIS0576 (dev);
  FpImageDevice    *img_dev = FP_IMAGE_DEVICE (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SM_INIT:
      fp_dbg ("Starting capture sequence");
      self->pkt_array     = EGIS0576_POST_INIT_PACKETS;
      self->pkt_array_len = EGIS0576_POST_INIT_PACKETS_LENGTH;
      self->current_index = 0;
      self->strips_len    = 0;
      self->strips        = NULL;
      fpi_ssm_next_state (ssm);
      break;

    case SM_START:
      if (self->stop)
        {
          fp_dbg ("Deactivating — capture loop complete");
          fpi_ssm_mark_completed (ssm);
          fpi_image_device_deactivate_complete (img_dev, NULL);
        }
      else
        {
          fpi_ssm_next_state (ssm);
        }
      break;

    case SM_REQ:
      send_req (ssm, dev, &self->pkt_array[self->current_index]);
      break;

    case SM_RESP:
      recv_resp (ssm, dev, self->pkt_array[self->current_index].response_length);
      break;

    case SM_PROCESS_IMG:
      process_imgs (ssm, dev);
      break;

    case SM_DONE:
      fpi_ssm_jump_to_state (ssm, SM_START);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
loop_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpImageDevice    *img_dev = FP_IMAGE_DEVICE (dev);
  FpDeviceEgis0576 *self    = FPI_DEVICE_EGIS0576 (dev);

  self->running = FALSE;

  if (error)
    fpi_image_device_session_error (img_dev, error);
}

/* ========================================================================= *
 *  FpImageDevice vfunc implementations
 * ========================================================================= */

static void
dev_init (FpImageDevice *dev)
{
  GError *error = NULL;

  g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                 EGIS0576_INTERFACE, 0, &error);
  fpi_image_device_open_complete (dev, error);
}

static void
dev_deinit (FpImageDevice *dev)
{
  GError *error = NULL;

  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                   EGIS0576_INTERFACE, 0, &error);
  fpi_image_device_close_complete (dev, error);
}

static void
dev_start (FpImageDevice *dev)
{
  FpDeviceEgis0576 *self = FPI_DEVICE_EGIS0576 (dev);
  FpiSsm           *ssm  = fpi_ssm_new (FP_DEVICE (dev), ssm_run_state, SM_STATES_NUM);

  self->stop = FALSE;
  fpi_ssm_start (ssm, loop_complete);
  self->running = TRUE;

  fpi_image_device_activate_complete (dev, NULL);
}

static void
dev_stop (FpImageDevice *dev)
{
  FpDeviceEgis0576 *self = FPI_DEVICE_EGIS0576 (dev);

  if (self->running)
    self->stop = TRUE;
  else
    fpi_image_device_deactivate_complete (dev, NULL);
}

/* ========================================================================= *
 *  Driver registration
 * ========================================================================= */

static const FpIdEntry id_table[] = {
  { .vid = 0x1c7a, .pid = 0x0576 },
  { .vid = 0,      .pid = 0      },
};

static void
fpi_device_egis0576_init (FpDeviceEgis0576 *self)
{
}

static void
fpi_device_egis0576_class_init (FpDeviceEgis0576Class *klass)
{
  FpDeviceClass      *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id        = "egis0576";
  dev_class->full_name = "LighTuning Technology Inc. EgisTec EH576";
  dev_class->type      = FP_DEVICE_TYPE_USB;
  dev_class->id_table  = id_table;
  /* The sensor physically covers a thin strip of the finger (swipe-style
   * hardware) but we capture one frame per press and assemble no strips,
   * so the UX is press-style.  PRESS tells fprintd to say "press finger"
   * rather than "swipe finger", which matches actual usage. */
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  img_class->img_open   = dev_init;
  img_class->img_close  = dev_deinit;
  img_class->activate   = dev_start;
  img_class->deactivate = dev_stop;

  /* The image passed to NBIS is 2× upscaled then mirror-padded by
   * EGIS0576_MIRPAD on each side; report the final padded size. */
  img_class->img_width  = EGIS0576_IMGWIDTH  * EGIS0576_RESIZE + 2 * EGIS0576_MIRPAD;
  img_class->img_height = EGIS0576_IMGHEIGHT * EGIS0576_RESIZE + 2 * EGIS0576_MIRPAD;

  /* Extra enrollment stages for small-sensor coverage.
   *
   * The EH576 active area is only ~3.5×2.6 mm.  A 2-3 mm lateral shift in
   * finger placement between enroll and verify captures a completely
   * non-overlapping region of the fingerprint, giving bozorth3 zero common
   * minutiae.  Seven enrollment stages give libfprint seven independent
   * templates spread across slightly different press positions, greatly
   * improving the probability that at least one overlaps the verify capture.
   *
   * The elanspi driver uses the same workaround for the same reason. */
  dev_class->nr_enroll_stages = EGIS0576_ENROLL_STAGES;

  /* Lowered bozorth3 threshold for small sensor (68×52 active area, 2× upscale).
   * See EGIS0576_RESIZE and libfprint issue #271 for context. */
  img_class->bz3_threshold = EGIS0576_BZ3_THRESHOLD;
}
