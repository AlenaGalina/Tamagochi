import sys
from PIL import Image
pal = {'.': (255,255,255), 'o': (40,30,30), 'f': (235,170,90), 'w': (255,250,240),
       'p': (240,150,160), 'e': (40,30,30), 'E': (40,30,30), 'n': (40,30,30), 'C': (245,160,170),
       'd': (180,110,60), 'M': (255,250,240), 'e': (40,30,30)}
paths = sys.argv[2:]
rows = [open(p).read().split() for p in paths]
w = max(len(r[0]) for r in rows); h = max(len(r) for r in rows)
S = 12
img = Image.new('RGB', ((w + 2) * S * len(rows), (h + 2) * S), (255, 255, 255))
for i, r in enumerate(rows):
    for y, line in enumerate(r):
        for x, c in enumerate(line):
            col = pal.get(c, (255, 0, 255))
            for dy in range(S):
                for dx in range(S):
                    img.putpixel(((x + 1 + i * (w + 2)) * S + dx, (y + 1) * S + dy), col)
img.save(sys.argv[1])
