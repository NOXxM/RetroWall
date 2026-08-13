# concepts.py — a few original, retro-themed RetroWall logo concepts (256px each).
import math
from PIL import Image, ImageDraw, ImageFont, ImageFilter

S = 256
def font(sz, bold=True):
    for name in (["impact.ttf"] if bold else []) + ["arialbd.ttf", "arial.ttf"]:
        try:
            return ImageFont.truetype("C:/Windows/Fonts/" + name, sz)
        except OSError:
            continue
    return ImageFont.load_default()

def vgrad(size, top, bot):
    w, h = size
    g = Image.new("RGB", (1, h))
    for y in range(h):
        t = y / (h - 1)
        g.putpixel((0, y), tuple(int(top[i] + (bot[i]-top[i])*t) for i in range(3)))
    return g.resize((w, h))

def rounded_mask(size, rad):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size[0]-1, size[1]-1], rad, fill=255)
    return m

def tile(bg):
    """bg: RGB image sized SxS -> rounded RGBA tile with subtle inner border."""
    t = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    t.paste(bg, (0, 0))
    t.putalpha(rounded_mask((S, S), 52))
    return t

def centre_text(d, text, fnt, fill, cy, stroke=0, stroke_fill=(0,0,0)):
    b = d.textbbox((0, 0), text, font=fnt, stroke_width=stroke)
    w, h = b[2]-b[0], b[3]-b[1]
    d.text(((S-w)/2 - b[0], cy - h/2 - b[1]), text, font=fnt, fill=fill,
           stroke_width=stroke, stroke_fill=stroke_fill)

# ---------------------------------------------------------------- A: synthwave sun
def concept_sun():
    bg = vgrad((S, S), (28, 18, 74), (10, 6, 30)).convert("RGB")
    d = ImageDraw.Draw(bg)
    # sun
    sun = vgrad((160, 160), (255, 214, 92), (247, 30, 122)).convert("RGB")
    sm = Image.new("L", (160, 160), 0)
    ImageDraw.Draw(sm).ellipse([0, 0, 159, 159], fill=255)
    # cut lines on lower half
    smd = ImageDraw.Draw(sm)
    for i, y in enumerate(range(92, 160, 12)):
        smd.rectangle([0, y, 159, y + 5 + i], fill=0)
    bg.paste(sun, (48, 34), sm)
    # neon perspective grid
    horizon = 168
    for gx in range(-6, 7):
        d.line([(S/2 + gx*10, horizon), (S/2 + gx*70, S)], fill=(255, 46, 158), width=2)
    for i, gy in enumerate(range(horizon, S, 12)):
        d.line([(0, gy + i*3), (S, gy + i*3)], fill=(94, 231, 255), width=1)
    return tile(bg)

# ---------------------------------------------------------------- B: pixel RW monogram
def concept_pixel():
    bg = vgrad((S, S), (24, 26, 38), (12, 13, 20)).convert("RGB")
    d = ImageDraw.Draw(bg)
    # scanlines
    for y in range(0, S, 4):
        d.line([(0, y), (S, y)], fill=(255, 255, 255), width=1)
    bg = Image.blend(vgrad((S, S), (24, 26, 38), (12, 13, 20)).convert("RGB"), bg, 0.06)
    d = ImageDraw.Draw(bg)
    f = font(150)
    # magenta drop-shadow + cyan face for a chunky retro monogram
    centre_text(d, "RW", f, (247, 30, 122), S/2 + 8)
    centre_text(d, "RW", f, (94, 231, 255), S/2 - 2)
    return tile(bg)

# ---------------------------------------------------------------- C: sunburst badge
def concept_burst():
    bg = vgrad((S, S), (255, 138, 66), (214, 30, 91)).convert("RGB")
    d = ImageDraw.Draw(bg)
    cx = cy = S/2
    for k in range(24):
        a0 = math.radians(k*15); a1 = math.radians(k*15 + 7.5)
        if k % 2 == 0:
            d.polygon([(cx, cy),
                       (cx+math.cos(a0)*260, cy+math.sin(a0)*260),
                       (cx+math.cos(a1)*260, cy+math.sin(a1)*260)],
                      fill=(255, 205, 92))
    # inner disc
    d.ellipse([44, 44, S-44, S-44], fill=(31, 22, 61))
    f = font(120)
    centre_text(d, "RW", f, (94, 231, 255), S/2)
    return tile(bg)

# ---------------------------------------------------------------- D: neon horizon 'R'
def concept_horizon():
    bg = vgrad((S, S), (44, 10, 66), (12, 6, 26)).convert("RGB")
    d = ImageDraw.Draw(bg)
    # grid floor
    horizon = 150
    for gx in range(-7, 8):
        d.line([(S/2 + gx*8, horizon), (S/2 + gx*80, S)], fill=(180, 40, 200), width=2)
    for i, gy in enumerate(range(horizon, S, 10)):
        d.line([(0, gy + i*4), (S, gy + i*4)], fill=(255, 90, 210), width=1)
    # neon sun ring
    d.ellipse([78, 40, 178, 140], outline=(94, 231, 255), width=6)
    f = font(96)
    centre_text(d, "RW", f, (255, 255, 255), 92, stroke=3, stroke_fill=(20, 200, 255))
    return tile(bg)

concepts = {
    "A_synthwave_sun": concept_sun(),
    "B_pixel_RW":      concept_pixel(),
    "C_sunburst":      concept_burst(),
    "D_neon_horizon":  concept_horizon(),
}
for name, img in concepts.items():
    img.save(f"concept_{name}.png")

# contact sheet
pad, lab = 24, 34
cols = len(concepts)
sheet = Image.new("RGB", (cols*(S+pad)+pad, S+pad*2+lab), (245, 245, 245))
d = ImageDraw.Draw(sheet)
lf = font(22, bold=False)
for i, (name, img) in enumerate(concepts.items()):
    x = pad + i*(S+pad)
    chk = Image.new("RGB", (S, S), (255, 255, 255))
    cd = ImageDraw.Draw(chk)
    for yy in range(0, S, 16):
        for xx in range(0, S, 16):
            if (xx//16 + yy//16) % 2: cd.rectangle([xx, yy, xx+16, yy+16], fill=(222,222,222))
    chk.paste(img, (0, 0), img)
    sheet.paste(chk, (x, pad))
    d.text((x, pad+S+6), name.replace("_", " "), font=lf, fill=(20, 20, 20))
sheet.save("_concepts.png")
print("wrote", ", ".join(concepts))
