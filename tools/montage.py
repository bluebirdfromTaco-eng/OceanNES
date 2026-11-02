import sys, subprocess, os
from PIL import Image, ImageDraw
frames = int(sys.argv[1]); out = sys.argv[2]; roms = sys.argv[3:]
os.makedirs('/tmp/claude-0/shots', exist_ok=True)
ims = []
for r in roms:
    p = '/tmp/claude-0/shots/' + os.path.basename(r) + '.ppm'
    if os.path.exists(p): os.remove(p)
    subprocess.run(['/home/claude/nes/runtest', r, str(frames), p], capture_output=True)
    im = Image.open(p).convert('RGB') if os.path.exists(p) else Image.new('RGB', (256, 240), (60, 0, 0))
    d = ImageDraw.Draw(im); d.rectangle([0, 0, 255, 9], fill=(80, 0, 0)); d.text((2, 0), os.path.basename(r)[:40], fill=(255, 255, 0))
    ims.append(im)
cols = 4; rows = (len(ims) + cols - 1) // cols
M = Image.new('RGB', (256 * cols, 240 * rows))
for i, im in enumerate(ims): M.paste(im, ((i % cols) * 256, (i // cols) * 240))
M.save(out)
