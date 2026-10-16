#!/usr/bin/env python3
"""
Builds the graphical desktop's assets into C: anti-aliased fonts, app icons, the wallpaper,
the mouse cursor and the window shadow.

    python3 tools/gen_ui_assets.py OUT_DIR

Everything comes from third_party/ui/ (Inter and JetBrains Mono under the SIL Open Font
License, Lucide icons under the ISC license) or is drawn here; nothing is fetched at build
time. The pixels go into one raw-DEFLATE blob that the kernel inflates once at start-up with
the inflater it already has for PNG (png.c), so the kernel image grows by the compressed size.
"""
import json, math, os, sys, zlib
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
UI = os.path.join(HERE, '..', 'third_party', 'ui')
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'kernel', 'display')

blob = bytearray()
def add(data: bytes) -> int:
    while len(blob) % 4: blob.append(0)
    off = len(blob)
    blob.extend(data)
    return off

# ---------------------------------------------------------------- proportional fonts
UI_CODEPOINTS = list(range(32, 127)) + list(range(160, 256)) + \
                [0x11E, 0x11F, 0x130, 0x131, 0x15E, 0x15F, 0x2013, 0x2014, 0x2022, 0x2026, 0x2190, 0x2192, 0x2318]

fonts_c = []
def make_font(cname, path, size):
    f = ImageFont.truetype(path, size)
    asc, desc = f.getmetrics()
    glyphs = []
    for cp in UI_CODEPOINTS:
        ch = chr(cp)
        try:
            mask, off = f.getmask2(ch, mode='L')
        except Exception:
            continue
        w, h = mask.size
        adv = round(f.getlength(ch))
        data = bytes(mask) if w and h else b''
        o = add(data)
        # off is the top-left of the bitmap relative to the text origin (top of ascent)
        glyphs.append((cp, w, h, off[0], off[1], adv, o))
    rows = ',\n'.join('    {0x%04X, %d, %d, %d, %d, %d, %du}' % g for g in glyphs)
    fonts_c.append(f'static const ui_glyph_t {cname}_glyphs[] = {{\n{rows}\n}};\n'
                   f'const ui_font_t {cname} = {{ {size}, {asc}, {desc}, {asc + desc + max(2, size // 6)}, '
                   f'{len(glyphs)}, {cname}_glyphs }};\n')

make_font('ui_font_regular', os.path.join(UI, 'Inter-Regular.ttf'), 14)
make_font('ui_font_medium',  os.path.join(UI, 'Inter-SemiBold.ttf'), 14)
make_font('ui_font_small',   os.path.join(UI, 'Inter-Regular.ttf'), 12)
make_font('ui_font_title',   os.path.join(UI, 'Inter-Bold.ttf'), 22)
make_font('ui_font_huge',    os.path.join(UI, 'Inter-Bold.ttf'), 48)

# ---------------------------------------------------------------- the terminal cell font
# 256 cells in code page 437 order, because that is what the apps' character surfaces hold.
CELL_W, CELL_H = 9, 18
CP437_LOW = "\u0000☺☻♥♦♣♠•◘○◙♂♀♪♫☼►◄↕‼¶§▬↨↑↓→←∟↔▲▼"
def cp437_char(b):
    if b < 32: return CP437_LOW[b]
    if b == 0x7F: return '⌂'
    return bytes([b]).decode('cp437')

mono = ImageFont.truetype(os.path.join(UI, 'JetBrainsMono-Regular.ttf'), 15)
fallback = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf', 14) \
    if os.path.exists('/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf') else mono
m_asc, m_desc = mono.getmetrics()
base_y = (CELL_H - (m_asc + m_desc)) // 2 + m_asc

_notdef = {}
def has_glyph(font, ch):
    """A font draws its "missing" box for characters it lacks; compare against that."""
    key = id(font)
    if key not in _notdef: _notdef[key] = bytes(font.getmask('\U0010FFFD'))
    try:
        return bytes(font.getmask(ch)) != _notdef[key]
    except Exception:
        return False

def box_cell(ch):
    """Box drawing, blocks and shades drawn to the exact cell so lines join between cells."""
    o = ord(ch)
    im = Image.new('L', (CELL_W, CELL_H), 0)
    d = ImageDraw.Draw(im)
    cx, cy = CELL_W // 2, CELL_H // 2
    if 0x2591 <= o <= 0x2593:                    # shades
        level = {0x2591: 64, 0x2592: 128, 0x2593: 192}[o]
        for y in range(CELL_H):
            for x in range(CELL_W):
                if (x + y) % 2 == 0 or level > 160: im.putpixel((x, y), level)
        return im
    blocks = {0x2588: (0, 0, CELL_W, CELL_H), 0x2584: (0, CELL_H // 2, CELL_W, CELL_H),
              0x2580: (0, 0, CELL_W, CELL_H // 2), 0x258C: (0, 0, CELL_W // 2 + 1, CELL_H),
              0x2590: (CELL_W // 2, 0, CELL_W, CELL_H), 0x25A0: (2, 5, CELL_W - 2, CELL_H - 5)}
    if o in blocks:
        x0, y0, x1, y1 = blocks[o]
        d.rectangle([x0, y0, x1 - 1, y1 - 1], fill=255)
        return im
    import unicodedata
    name = unicodedata.name(ch, '')
    if not name.startswith('BOX DRAWINGS'):
        return None
    words = name.replace('BOX DRAWINGS ', '').split()
    # which arms, and single/double
    arms = {}
    def arm(dirn, style): arms[dirn] = style
    n = ' '.join(words)
    style_of = lambda w: 2 if w == 'DOUBLE' else 1
    if 'HORIZONTAL' in words and 'VERTICAL' in words:
        hs = 2 if 'DOUBLE HORIZONTAL' in n or n.startswith('DOUBLE VERTICAL AND HORIZONTAL') else 1
        vs = 2 if 'DOUBLE VERTICAL' in n or n.startswith('DOUBLE VERTICAL AND HORIZONTAL') else 1
        if n.startswith('LIGHT') or n.startswith('HEAVY'): hs = vs = 1
        for a in 'LRUD': arm(a, hs if a in 'LR' else vs)
    else:
        parts = n.split(' AND ')
        for p in parts:
            ws = p.split()
            st = 2 if 'DOUBLE' in ws else 1
            if 'HORIZONTAL' in ws: arm('L', st); arm('R', st)
            if 'VERTICAL' in ws: arm('U', st); arm('D', st)
            if 'DOWN' in ws: arm('D', st)
            if 'UP' in ws: arm('U', st)
            if 'LEFT' in ws: arm('L', st)
            if 'RIGHT' in ws: arm('R', st)
        # "DOUBLE DOWN AND RIGHT" style names: the style word applies to both
        if n.startswith('DOUBLE') and ' AND ' in n and 'SINGLE' not in n:
            for k in arms: arms[k] = 2
        if 'DOWN SINGLE AND' in n or 'UP SINGLE AND' in n:
            for k in 'UD':
                if k in arms: arms[k] = 1
            for k in 'LR':
                if k in arms: arms[k] = 2
        if 'DOWN DOUBLE AND' in n or 'UP DOUBLE AND' in n or 'VERTICAL DOUBLE AND' in n:
            for k in 'UD':
                if k in arms: arms[k] = 2
            for k in 'LR':
                if k in arms: arms[k] = 1
        if 'VERTICAL SINGLE AND' in n:
            for k in 'UD':
                if k in arms: arms[k] = 1
            for k in 'LR':
                if k in arms: arms[k] = 2
    for a, st in arms.items():
        offs = [0] if st == 1 else [-2, 2]
        for of in offs:
            if a == 'L': d.line([(0, cy + of), (cx, cy + of)], fill=255)
            if a == 'R': d.line([(cx, cy + of), (CELL_W - 1, cy + of)], fill=255)
            if a == 'U': d.line([(cx + of, 0), (cx + of, cy)], fill=255)
            if a == 'D': d.line([(cx + of, cy), (cx + of, CELL_H - 1)], fill=255)
    return im

cells = bytearray()
for b in range(256):
    ch = cp437_char(b)
    im = box_cell(ch) if b >= 0xB0 and b <= 0xDF or b in (0xFE,) else None
    if im is None:
        im = Image.new('L', (CELL_W, CELL_H), 0)
        if b not in (0, 32, 255):
            d = ImageDraw.Draw(im)
            f = mono if has_glyph(mono, ch) else fallback
            w = f.getlength(ch)
            d.text(((CELL_W - w) / 2, base_y), ch, font=f, fill=255, anchor='ls')
    cells.extend(im.tobytes())
mono_off = add(bytes(cells))

# ---------------------------------------------------------------- images
images = []
def add_image(name, im):
    im = im.convert('RGBA')
    images.append((name, im.width, im.height, add(im.tobytes())))

def squircle_mask(size, scale=4):
    S = size * scale
    m = Image.new('L', (S, S), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * 0.23), fill=255)
    return m.resize((size, size), Image.LANCZOS)

codes = json.load(open(os.path.join(UI, 'lucide-codes.json')))
lucide_path = os.path.join(UI, 'lucide.ttf')

def app_icon(size, glyph, c1, c2, fg=(255, 255, 255)):
    S = size * 4
    grad = Image.new('RGBA', (S, S))
    for y in range(S):
        t = y / (S - 1)
        col = tuple(int(c1[i] * (1 - t) + c2[i] * t) for i in range(3)) + (255,)
        ImageDraw.Draw(grad).line([(0, y), (S, y)], fill=col)
    # a soft top highlight, the way glossy icons catch light
    hl = Image.new('L', (S, S), 0)
    ImageDraw.Draw(hl).ellipse([-S * 0.3, -S * 0.9, S * 1.3, S * 0.45], fill=46)
    grad = Image.composite(Image.new('RGBA', (S, S), (255, 255, 255, 255)), grad, hl)
    f = ImageFont.truetype(lucide_path, int(S * 0.56))
    g = Image.new('L', (S, S), 0)
    ImageDraw.Draw(g).text((S / 2, S / 2), chr(codes[glyph]), font=f, fill=255, anchor='mm')
    sh = g.filter(ImageFilter.GaussianBlur(S * 0.02))
    grad = Image.composite(Image.new('RGBA', (S, S), (0, 0, 0, 255)), grad, sh.point(lambda v: int(v * 0.25)))
    grad = Image.composite(Image.new('RGBA', (S, S), fg + (255,)), grad, g)
    grad = grad.resize((size, size), Image.LANCZOS)
    grad.putalpha(squircle_mask(size))
    return grad

APPS = [  # name, glyph, top colour, bottom colour
    ('terminal',   'square-terminal', (70, 74, 86),   (28, 30, 36)),
    ('notepad',    'notebook-pen',    (255, 214, 92), (244, 160, 36)),
    ('clock',      'clock',           (90, 98, 255),  (50, 40, 190)),
    ('about',      'info',            (110, 190, 255),(30, 110, 230)),
    ('diskmgr',    'hard-drive',      (160, 170, 185),(90, 100, 118)),
    ('calculator', 'calculator',      (255, 160, 70), (230, 90, 30)),
    ('files',      'folder',          (90, 190, 255), (20, 120, 240)),
    ('paint',      'palette',         (255, 120, 170),(200, 60, 200)),
    ('viewer',     'image',           (120, 220, 150),(30, 160, 100)),
    ('taskmgr',    'activity',        (90, 230, 160), (10, 150, 110)),
    ('media',      'music',           (255, 95, 120), (215, 30, 80)),
    ('netmon',     'globe',           (80, 210, 230), (20, 120, 200)),
    ('snake',      'gamepad-2',       (150, 230, 90), (60, 160, 40)),
    ('2048',       'grid-2x2',        (250, 200, 120),(230, 140, 60)),
    ('pdf',        'file-text',       (255, 110, 90), (210, 40, 40)),
    ('sticky',     'sticky-note',     (255, 240, 130),(240, 200, 40)),
    ('doom',       'skull',           (120, 40, 40),  (40, 10, 10)),
    ('settings',   'settings',        (150, 156, 168),(80, 86, 100)),
    ('launcher',   'layout-grid',     (255, 150, 90), (180, 80, 230)),
    ('trash',      'trash-2',         (200, 205, 215),(130, 136, 150)),
]
for name, glyph, c1, c2 in APPS:
    add_image('icon48_' + name, app_icon(48, glyph, c1, c2))
    add_image('icon20_' + name, app_icon(20, glyph, c1, c2))

# small monochrome glyphs for the top bar and window buttons (white on transparent)
for g in ['ethernet-port', 'wifi', 'volume-2', 'battery-full', 'search', 'power', 'bell', 'x', 'minus', 'maximize-2', 'house', 'monitor', 'cpu', 'chevron-right']:
    S = 64
    f = ImageFont.truetype(lucide_path, 52)
    im = Image.new('L', (S, S), 0)
    ImageDraw.Draw(im).text((S / 2, S / 2), chr(codes[g]), font=f, fill=255, anchor='mm')
    im = im.resize((16, 16), Image.LANCZOS)
    rgba = Image.new('RGBA', (16, 16), (255, 255, 255, 0))
    rgba.putalpha(im)
    add_image('glyph_' + g, rgba)

# the tOS mark for the top-left menu
S = 64
mark = Image.new('L', (S, S), 0)
d = ImageDraw.Draw(mark)
d.rounded_rectangle([6, 6, 58, 58], radius=16, outline=255, width=6)
fnt = ImageFont.truetype(os.path.join(UI, 'Inter-Bold.ttf'), 34)
d.text((S / 2, S / 2 + 1), 't', font=fnt, fill=255, anchor='mm')
mark = mark.resize((18, 18), Image.LANCZOS)
rgba = Image.new('RGBA', (18, 18), (255, 255, 255, 0)); rgba.putalpha(mark)
add_image('logo', rgba)

# the cursor: an arrow drawn at 4x and reduced, white with a dark rim and a soft shadow
S = 4
pts = [(1, 1), (1, 17), (5, 13), (8, 20), (10.5, 19), (7.6, 12.3), (13, 12.3)]
big = Image.new('RGBA', (24 * S, 26 * S), (0, 0, 0, 0))
shadow = Image.new('L', big.size, 0)
ImageDraw.Draw(shadow).polygon([((x + 0.8) * S, (y + 1.2) * S) for x, y in pts], fill=110)
shadow = shadow.filter(ImageFilter.GaussianBlur(S * 1.1))
big = Image.composite(Image.new('RGBA', big.size, (0, 0, 0, 255)), big, shadow)
dd = ImageDraw.Draw(big)
dd.polygon([(x * S, y * S) for x, y in pts], fill=(20, 20, 24, 255))
inner = [(1 + 1.1, 1 + 2.6), (2.1, 14.6), (5.3, 11.4), (8.6, 18.6), (9.4, 18.2), (6.2, 11.2), (10.6, 11.2)]
dd.polygon([(x * S, y * S) for x, y in inner], fill=(255, 255, 255, 255))
add_image('cursor', big.resize((24, 26), Image.LANCZOS))

# window shadow: a blurred rounded rectangle, drawn as nine slices at run time
SH = 120
pad = 40
sh = Image.new('L', (SH, SH), 0)
ImageDraw.Draw(sh).rounded_rectangle([pad, pad + 6, SH - pad, SH - pad + 6], radius=12, fill=150)
sh = sh.filter(ImageFilter.GaussianBlur(14))
rgba = Image.new('RGBA', (SH, SH), (0, 0, 0, 0)); rgba.putalpha(sh)
add_image('shadow', rgba)

# the wallpaper: an evening gradient with soft light shapes, at 1280x720
W, H = 1280, 720
wp = Image.new('RGB', (W, H))
top, mid, bot = (24, 28, 72), (88, 52, 140), (236, 120, 110)
px = wp.load()
for y in range(H):
    t = y / (H - 1)
    if t < 0.55:
        u = t / 0.55; a, b = top, mid
    else:
        u = (t - 0.55) / 0.45; a, b = mid, bot
    col = tuple(int(a[i] * (1 - u) + b[i] * u) for i in range(3))
    for x in range(W):
        px[x, y] = col
glow = Image.new('RGB', (W, H), (0, 0, 0))
gd = ImageDraw.Draw(glow)
for (cx, cy, r, col) in [(260, 640, 420, (255, 150, 90)), (1050, 180, 360, (90, 120, 255)),
                         (760, 560, 300, (255, 90, 160)), (420, 120, 260, (120, 80, 220))]:
    gd.ellipse([cx - r, cy - r * 0.6, cx + r, cy + r * 0.6], fill=col)
glow = glow.filter(ImageFilter.GaussianBlur(120))
wp = Image.blend(wp, glow, 0.35)
# a few soft waves along the bottom, like hills at dusk
waves = Image.new('L', (W, H), 0)
wd = ImageDraw.Draw(waves)
for i, (amp, base, alpha) in enumerate([(26, 560, 60), (34, 610, 90), (22, 660, 130)]):
    pts = [(x, base + amp * math.sin(x / (170 + 40 * i) + i * 1.7)) for x in range(0, W + 8, 8)]
    wd.polygon(pts + [(W, H), (0, H)], fill=alpha)
waves = waves.filter(ImageFilter.GaussianBlur(3))
dark = Image.new('RGB', (W, H), (30, 14, 50))
wp = Image.composite(dark, wp, waves)
images.append(('wallpaper', W, H, add(wp.tobytes())))   # RGB, 3 bytes per pixel

# ---------------------------------------------------------------- write it out
comp = zlib.compressobj(9, zlib.DEFLATED, -15)
packed = comp.compress(bytes(blob)) + comp.flush()

with open(os.path.join(OUT, 'ui_assets.c'), 'w') as f:
    f.write('/* Generated by tools/gen_ui_assets.py -- do not edit. Fonts: Inter, JetBrains Mono (SIL OFL);\n'
            ' * icons: Lucide (ISC). Licences in third_party/ui/. */\n#include "ui_assets.h"\n\n')
    f.write(f'const uint32_t ui_blob_raw_len = {len(blob)}u;\nconst uint32_t ui_blob_len = {len(packed)}u;\n')
    f.write('const uint8_t ui_blob[] = {\n')
    for i in range(0, len(packed), 24):
        f.write('  ' + ','.join(str(b) for b in packed[i:i + 24]) + ',\n')
    f.write('};\n\n')
    for fc in fonts_c: f.write(fc + '\n')
    f.write(f'const uint32_t ui_mono_off = {mono_off}u;\n\n')
    f.write('const ui_image_t ui_images[] = {\n')
    for name, w, h, o in images:
        f.write(f'    {{"{name}", {w}, {h}, {o}u}},\n')
    f.write('};\n')
    f.write(f'const int ui_image_count = {len(images)};\n')

print(f'ui assets: {len(blob)} bytes raw, {len(packed)} compressed, {len(images)} images')

if os.environ.get('UI_PREVIEW'):
    # a contact sheet of everything, for looking at while working on the desktop
    sheet = Image.new('RGBA', (1280, 720 + 260), (40, 40, 48, 255))
    raw = bytes(blob)
    for name, w, h, o in images:
        if name == 'wallpaper':
            sheet.paste(Image.frombytes('RGB', (w, h), raw[o:o + w * h * 3]), (0, 260))
    x, y = 8, 8
    for name, w, h, o in images:
        if name == 'wallpaper': continue
        im = Image.frombytes('RGBA', (w, h), raw[o:o + w * h * 4])
        if x + w > 1270: x = 8; y += 64
        sheet.alpha_composite(im, (x, y)); x += w + 10
    cellsheet = Image.frombytes('L', (CELL_W, CELL_H * 256), raw[mono_off:mono_off + CELL_W * CELL_H * 256])
    for b in range(256):
        c = cellsheet.crop((0, b * CELL_H, CELL_W, (b + 1) * CELL_H))
        sheet.paste(Image.new('RGBA', (CELL_W, CELL_H), (230, 230, 230, 255)), (8 + (b % 64) * 10, 150 + (b // 64) * 20), c)
    sheet.save(os.environ['UI_PREVIEW'])
