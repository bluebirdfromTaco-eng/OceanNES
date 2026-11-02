"""Builds mac/AppIcon.icns from mac/icon/source.png, shaped like a standard
macOS app icon (rounded square on the 1024 grid with a soft shadow)."""
import io, struct
from PIL import Image, ImageDraw, ImageFilter

src = Image.open('mac/icon/source.png').convert('RGB')
w, h = src.size
s = min(w, h)
src = src.crop(((w - s) // 2, (h - s) // 2, (w - s) // 2 + s, (h - s) // 2 + s))

def master():
    body, radius, off = 824, 185, 100
    art = src.resize((body, body), Image.LANCZOS)
    mask = Image.new('L', (body * 4, body * 4), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, body * 4 - 1, body * 4 - 1), radius * 4, fill=255)
    mask = mask.resize((body, body), Image.LANCZOS)
    out = Image.new('RGBA', (1024, 1024), (0, 0, 0, 0))
    shadow = Image.new('RGBA', (1024, 1024), (0, 0, 0, 0))
    sm = Image.new('L', (1024, 1024), 0)
    sm.paste(mask, (off, off + 10))
    shadow.putalpha(sm.point(lambda v: v * 90 // 255).filter(ImageFilter.GaussianBlur(14)))
    out = Image.alpha_composite(out, shadow)
    layer = Image.new('RGBA', (1024, 1024), (0, 0, 0, 0))
    layer.paste(art.convert('RGBA'), (off, off), mask)
    return Image.alpha_composite(out, layer)

M = master()
M.save('mac/icon/AppIcon-1024.png')
def png(sz):
    b = io.BytesIO(); M.resize((sz, sz), Image.LANCZOS).save(b, 'PNG'); return b.getvalue()
chunks = [(b'ic07', 128), (b'ic08', 256), (b'ic09', 512), (b'ic10', 1024), (b'ic11', 32), (b'ic12', 64), (b'ic13', 256), (b'ic14', 512)]
body = b''.join(t + struct.pack('>I', len(d) + 8) + d for t, d in ((t, png(s)) for t, s in chunks))
open('mac/AppIcon.icns', 'wb').write(b'icns' + struct.pack('>I', len(body) + 8) + body)
print('icns bytes', len(body) + 8)
