# pixel_logo.py — authentic pixel-art "RW" icon for RetroWall.
# Designed on a 32x32 logical grid, then upscaled nearest-neighbor to stay crisp.
from PIL import Image
import numpy as np

G = 32                       # logical grid
SCALE = 8                    # 32 * 8 = 256 px

# Palette (retro synthwave)
BG_TOP   = (26, 22, 54)      # deep indigo
BG_BOT   = (12, 10, 28)
BORDER   = (94, 231, 255)    # neon cyan frame
CYAN     = (94, 231, 255)    # letter face
CYAN_DK  = (40, 150, 190)    # letter shade
MAGENTA  = (247, 40, 140)    # chromatic drop shadow

# 8x11 "R"  and  11x11 "W"  (1 = ink)
R = [
    "11111100",
    "10000110",
    "10000010",
    "10000010",
    "10000110",
    "11111100",
    "10011000",
    "10001100",
    "10000110",
    "10000011",
    "10000011",
]
W = [
    "10000000001",
    "10000000001",
    "10000000001",
    "10000000001",
    "10000000001",
    "10000100001",
    "10001110001",
    "10111011101",
    "11100000111",
    "11000000011",
    "10000000001",
]

def stamp(grid, bmp, ox, oy, val):
    for y, row in enumerate(bmp):
        for x, c in enumerate(row):
            if c == "1":
                gx, gy = ox + x, oy + y
                if 0 <= gx < G and 0 <= gy < G:
                    grid[gy, gx] = val

# 1 = letter face, 2 = letter shade, 3 = magenta ghost
layer = np.zeros((G, G), dtype=np.uint8)

# positions: R at x=4, W at x=14, both y=10
rx, wx, ly = 4, 14, 10
# magenta chromatic ghost, offset +1,+1
stamp(layer, R, rx + 1, ly + 1, 3); stamp(layer, W, wx + 1, ly + 1, 3)
# cyan shade underlayer, offset +1 down
stamp(layer, R, rx, ly + 1, 2);     stamp(layer, W, wx, ly + 1, 2)
# cyan face
stamp(layer, R, rx, ly, 1);         stamp(layer, W, wx, ly, 1)

# ---- compose RGBA at logical resolution (fully transparent background) ----
img = np.zeros((G, G, 4), dtype=np.uint8)   # everything transparent; only RW is drawn

# paint letters
col = {1: CYAN, 2: CYAN_DK, 3: MAGENTA}
for v, c in col.items():
    ys, xs = np.where(layer == v)
    img[ys, xs, :3] = c
    img[ys, xs, 3] = 255

logical = Image.fromarray(img, "RGBA")
logical.save("pixel_logical_32.png")

# upscale NEAREST -> crisp pixels
big = logical.resize((G*SCALE, G*SCALE), Image.NEAREST)
big.save("logo_256.png")
big.resize((64, 64), Image.NEAREST).save("logo_64.png")

# .ico (nearest for the small sizes to keep them crunchy)
icons = []
for s in [16, 24, 32, 48, 64, 128, 256]:
    icons.append(logical.resize((s, s), Image.NEAREST))
big.save("app.ico", sizes=[(s, s) for s in [16,24,32,48,64,128,256]])
print("saved logo_256.png, logo_64.png, app.ico")

# preview on checkerboard
def checker(sz, step=16):
    c = Image.new("RGB", (sz, sz), (235,235,235)); px = c.load()
    for y in range(sz):
        for x in range(sz):
            if (x//step + y//step) % 2: px[x,y]=(205,205,205)
    return c
prev = checker(256); prev.paste(big, (0,0), big)
# small-size strip
strip = checker(256)
x = 8
for s in [16,24,32,48,64]:
    im = logical.resize((s,s), Image.NEAREST)
    strip.paste(im, (x, 256-s-8), im); x += s + 8
prev.save("_pixprev.png"); strip.save("_pixstrip.png")
