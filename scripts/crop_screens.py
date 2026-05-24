"""
crop_screens.py  — M5Stack screen extractor
Detects the device body and crops to just the LCD area.

Speed trick: all detection runs on a 400px-wide thumbnail; crop coords
are scaled back up to the original resolution before cropping.
"""

from PIL import Image
import numpy as np
from scipy import ndimage
import os, sys

SCREENSHOTS_DIR = '/Users/Mike/Projects/M5/screenshots'
OUTPUT_DIR      = '/Users/Mike/Projects/M5/screenshots/cropped'

DETECT_WIDTH     = 400    # thumbnail width for detection (fast)
DARK_THRESHOLD   = 0.18   # pixels darker than this = device body
ERODE_PX         = 8      # erosion on thumbnail — removes thin label tape
MIN_DEVICE_PX    = 400    # min dark pixels in eroded thumbnail
BUTTON_FRACTION  = 0.28   # cut from right (button strip)
BEZEL_FRACTION   = 0.055  # coarse inward trim
LCD_THRESHOLD    = 0.045  # LCD black > plastic black (backlight glow)
LCD_MARGIN       = 12     # pixels around detected LCD content


def trim_to_lcd(img):
    """Remove pure-black plastic bezel by finding the backlit LCD area."""
    arr = np.array(img, dtype=np.float32) / 255.0
    brightness = arr.mean(axis=2)
    lit = brightness > LCD_THRESHOLD
    rows = np.where(np.any(lit, axis=1))[0]
    cols = np.where(np.any(lit, axis=0))[0]
    if len(rows) < 20 or len(cols) < 20:
        return img
    rmin = max(0, int(rows[0])  - LCD_MARGIN)
    rmax = min(img.height, int(rows[-1]) + LCD_MARGIN)
    cmin = max(0, int(cols[0])  - LCD_MARGIN)
    cmax = min(img.width,  int(cols[-1]) + LCD_MARGIN)
    return img.crop((cmin, rmin, cmax, rmax))


def find_and_crop(img_path, output_path, debug=False):
    img = Image.open(img_path).convert('RGB')
    ow, oh = img.size

    # --- Detection on thumbnail ---
    scale = DETECT_WIDTH / ow
    th = int(oh * scale)
    thumb = img.resize((DETECT_WIDTH, th), Image.LANCZOS)
    arr = np.array(thumb, dtype=np.float32) / 255.0
    brightness = arr.mean(axis=2)

    dark = brightness < DARK_THRESHOLD

    # Erode to remove thin label-tape strips; device body survives
    kern = np.ones((ERODE_PX * 2 + 1, ERODE_PX * 2 + 1), bool)
    dark_eroded = ndimage.binary_erosion(dark, kern)

    labeled, n = ndimage.label(dark_eroded)
    if n == 0:
        print(f"  SKIP (no device found): {os.path.basename(img_path)}")
        return False

    sizes = ndimage.sum(dark_eroded, labeled, range(1, n + 1))
    largest = int(np.argmax(sizes)) + 1
    if sizes[largest - 1] < MIN_DEVICE_PX:
        print(f"  SKIP (device region too small): {os.path.basename(img_path)}")
        return False

    device_seed = labeled == largest
    device_mask = ndimage.binary_dilation(device_seed, kern)
    device_mask = ndimage.binary_fill_holes(device_mask)

    rows_t = np.where(np.any(device_mask, axis=1))[0]
    cols_t = np.where(np.any(device_mask, axis=0))[0]
    rmin_t, rmax_t = int(rows_t[0]), int(rows_t[-1])
    cmin_t, cmax_t = int(cols_t[0]), int(cols_t[-1])

    if debug:
        dh_t, dw_t = rmax_t - rmin_t, cmax_t - cmin_t
        print(f"  thumb device bbox: ({cmin_t},{rmin_t})→({cmax_t},{rmax_t})  {dw_t}×{dh_t}")

    # --- Scale bounds back to original resolution ---
    inv = 1.0 / scale
    rmin = int(rmin_t * inv);  rmax = int(rmax_t * inv)
    cmin = int(cmin_t * inv);  cmax = int(cmax_t * inv)
    dh = rmax - rmin;          dw = cmax - cmin

    bezel_v = int(dh * BEZEL_FRACTION)
    bezel_h = int(dw * BEZEL_FRACTION)

    # Pass 1: coarse crop to device body
    crop = img.crop((
        cmin + bezel_h,
        rmin + bezel_v,
        cmax - int(dw * BUTTON_FRACTION),
        rmax - bezel_v,
    ))

    # Pass 2: trim remaining pure-black bezel using LCD backlight threshold
    crop = trim_to_lcd(crop)

    crop.save(output_path, quality=95)
    return True


def main(test_mode=False):
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    files = sorted([
        f for f in os.listdir(SCREENSHOTS_DIR)
        if f.lower().endswith(('.jpg', '.jpeg', '.png'))
        and not f.startswith('.')
    ])

    if test_mode:
        files = files[:5]
        print(f"TEST MODE — {len(files)} files")
    else:
        print(f"Processing {len(files)} files...")

    ok = skip = 0
    for fname in files:
        src = os.path.join(SCREENSHOTS_DIR, fname)
        dst = os.path.join(OUTPUT_DIR, fname)
        print(f"  {fname}", end=' ', flush=True)
        if find_and_crop(src, dst, debug=True):
            print("✓")
            ok += 1
        else:
            skip += 1

    print(f"\nDone: {ok} cropped, {skip} skipped → {OUTPUT_DIR}")


if __name__ == '__main__':
    main('--test' in sys.argv)
