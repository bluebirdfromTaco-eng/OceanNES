# Builds mac/AppIcon.icns from mac/icon_source.png: the picture fills a standard
# macOS rounded-square icon tile (with the usual margin and soft shadow).
import struct, io, sys
from PIL import Image, ImageDraw, ImageFilter
S = 1024
src = Image.open(sys.argv[1] if len(sys.argv) > 1 else 'mac/icon_source.png').convert('RGBA')
side = min(src.size)
src = src.crop(((src.width - side) // 2, (src.height - side) // 2, (src.width - side) // 2 + side, (src.height - side) // 2 + side))
inset = 100; body = S - 2 * inset
art = src.resize((body, body), Image.BICUBIC)
mask = Image.new('L', (body, body), 0)
ImageDraw.Draw(mask).rounded_rectangle([0, 0, body - 1, body - 1], radius=185, fill=255)
img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
sm = Image.new('L', (S, S), 0); sm.paste(mask, (inset, inset + 14))
shadow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
shadow.putalpha(sm.filter(ImageFilter.GaussianBlur(22)).point(lambda v: v * 0.5))
img = Image.alpha_composite(img, shadow)
layer = Image.new('RGBA', (S, S), (0, 0, 0, 0)); layer.paste(art, (inset, inset), mask)
img = Image.alpha_composite(img, layer)
img.save('/tmp/claude-0/icon1024.png')
def png(sz):
    b = io.BytesIO(); img.resize((sz, sz), Image.LANCZOS).save(b, 'PNG'); return b.getvalue()
chunks = [(b'ic07', 128), (b'ic08', 256), (b'ic09', 512), (b'ic10', 1024), (b'ic11', 32), (b'ic12', 64), (b'ic13', 256), (b'ic14', 512)]
body_bytes = b''.join(t + struct.pack('>I', len(p) + 8) + p for t, p in ((t, png(s)) for t, s in chunks))
open('mac/AppIcon.icns', 'wb').write(b'icns' + struct.pack('>I', len(body_bytes) + 8) + body_bytes)
print('icns bytes', len(body_bytes) + 8)
