# make_logo.py — crop the plague-doctor head, clean it up, and emit logo assets.
import numpy as np
from PIL import Image, ImageFilter, ImageEnhance, ImageDraw
from scipy import ndimage

SRC = "valeModel.png"

# 1) Crop tight to the head (hood + mask). Box tuned from eye anchor at (430,543).
im = Image.open(SRC).convert("RGBA")
head = im.crop((284, 372, 600, 696))          # (left, top, right, bottom)

a = np.array(head).astype(np.uint8)
rgb = a[:, :, :3].astype(int)

# 2) Background mask = near-white pixels reachable from the border (flood fill),
#    so white highlights *inside* the mask are preserved.
near_white = (rgb > 232).all(axis=2)
# label connected white regions; keep only those touching the border.
lbl, n = ndimage.label(near_white)
border_ids = set(lbl[0, :]) | set(lbl[-1, :]) | set(lbl[:, 0]) | set(lbl[:, -1])
border_ids.discard(0)
bg = np.isin(lbl, list(border_ids))

alpha = np.where(bg, 0, 255).astype(np.uint8)

# 3) Keep only the largest opaque blob -> removes floating syringe/claw bits.
opaque = alpha > 0
olbl, on = ndimage.label(opaque)
if on > 1:
    sizes = ndimage.sum(np.ones_like(olbl), olbl, index=range(1, on + 1))
    keep = int(np.argmax(sizes)) + 1
    alpha = np.where(olbl == keep, alpha, 0).astype(np.uint8)

# 4) Feather the alpha edge by 1px so the cutout isn't jagged.
alpha_img = Image.fromarray(alpha, "L").filter(ImageFilter.GaussianBlur(0.6))
a[:, :, 3] = np.array(alpha_img)
head = Image.fromarray(a, "RGBA")

# 5) Trim fully-transparent margins, then pad to a square canvas.
bbox = head.getbbox()
head = head.crop(bbox)
side = max(head.size) + 24                      # small breathing room
canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
canvas.paste(head, ((side - head.width) // 2, (side - head.height) // 2), head)

# 6) Touch-up: gentle contrast + saturation so it reads at 16px; brighten eyes.
rgb_only = canvas.convert("RGB")
rgb_only = ImageEnhance.Contrast(rgb_only).enhance(1.08)
rgb_only = ImageEnhance.Color(rgb_only).enhance(1.12)
r, g, b, al = canvas.split()
canvas = Image.merge("RGBA", (*rgb_only.split(), al))

# extra glow on the green eyes
arr = np.array(canvas).astype(int)
rr, gg, bb = arr[:, :, 0], arr[:, :, 1], arr[:, :, 2]
eyes = (gg > 140) & (rr < 150) & (bb < 150) & (gg - rr > 40)
arr[eyes] = np.clip(arr[eyes] * np.array([1.0, 1.15, 1.0, 1.0]), 0, 255)
canvas = Image.fromarray(arr.astype(np.uint8), "RGBA")

canvas.save("logo_head.png")
print("logo_head.png", canvas.size)

# 7) Master 256 logo (transparent) for in-app use.
logo256 = canvas.resize((256, 256), Image.LANCZOS)
logo256.save("logo_256.png")
canvas.resize((64, 64), Image.LANCZOS).save("logo_64.png")   # embedded in the exe

# 8) Multi-resolution .ico for taskbar/title-bar/tray.
ico_sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
logo256.save("app.ico", sizes=ico_sizes)
print("app.ico written with", ico_sizes)
