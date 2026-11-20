# Makes the website's screenshots (website/docs/public/img/*.webp) from the trailer's footage, which stays on the
# Mac that recorded it (trailer/clips, trailer/shots: not in git). Each image is one frame or still, cropped to the
# part of rekordbox it shows. rekordbox's playlist sidebar is always cropped away; a personal playlist name that
# can't be cropped is blurred (BLUR). Only track titles and artists, and the playlists Minimal Techno Mix, DnB,
# Liked Songs and dance (local) may show.
#
# The footage shows the full edition. Pictures of what only the full edition has go to private/website/docs/public/img
# (the full edition of the site lays them over docs/public/img), and where a picture shows a full-edition item in
# passing, the public picture leaves that item out the way the public edition draws it (EDIT) and the full edition
# gets the original.
#   python3 -I website/scripts/screens.py [name …]   (needs ffmpeg and Pillow)
import os, subprocess, sys, tempfile
from PIL import Image, ImageDraw, ImageFilter

here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.dirname(os.path.dirname(here))
trailer = os.path.join(repo, 'trailer')
out = os.path.join(here, '..', 'docs', 'public', 'img')
out_full = os.path.join(repo, 'private', 'website', 'docs', 'public', 'img')
new = os.path.join(trailer, 'clips', '2026-11-19')

# name: (source, seconds into a video or None for a still, crop box (l, t, r, b) in source pixels or None,
#        width to scale to, blur boxes in source pixels)
SHOTS = {
    'chip':          (os.path.join(trailer, 'shots', 'main_chip.png'), None, (0, 1708, 700, 1750), 700, []),
    'about':         (os.path.join(new, 'badge.png'), None, None, 520, []),
    'patcher-ready': (os.path.join(trailer, 'shots', 'p3_ready.png'), None, None, 800, []),
    'patcher-done':  (os.path.join(trailer, 'shots', 'p3_done.png'), None, None, 800, []),
    'patcher-win':   (os.path.join(trailer, 'shots', 'p3_win.png'), None, None, 800, []),
    'analysis':      (os.path.join(new, 'analysis.mov'), 31, None, 1600, []),
    'trackmenu':     (os.path.join(new, 'trackmenu_multi_submenu.png'), None, None, 1350, []),
    'genres':        (os.path.join(new, 'selection.mov'), 10, (566, 262, 2614, 1504), 1400, []),
    'multisort':     (os.path.join(new, 'multisort.mov'), 17, (530, 560, 3024, 1160), 1600, []),
    'quickdeck':     (os.path.join(new, 'load_new.mov'), 8.8, (0, 100, 3024, 1045), 1600, []),
    'xfadelock-open':   (os.path.join(new, 'xfadelock.mov'), 1, (1240, 1030, 2040, 1122), 880, []),
    'xfadelock-locked': (os.path.join(new, 'xfadelock.mov'), 10, (1240, 1030, 2040, 1122), 880, []),
    'autocue':       (os.path.join(trailer, 'clips', 'autocue.mov'), 60, None, 1600, [(20, 66, 625, 150)]),
    'flx4':          (os.path.join(trailer, 'clips', 'flx4.mov'), 8, (0, 495, 3024, 1368), 1600, []),
}

# Pictures only the full edition has: private/website/screens_full.py's SHOTS, same form.
FULL_ONLY = set()
full_shots = os.path.join(repo, 'private', 'website', 'screens_full.py')
if os.path.exists(full_shots):
    scope = {'os': os, 'trailer': trailer, 'new': new}
    exec(open(full_shots, encoding='utf-8').read(), scope)
    SHOTS.update(scope['SHOTS'])
    FULL_ONLY = set(scope['SHOTS'])

BG = (28, 28, 28)  # rekordbox's window background, around the analysis window's checkboxes


def analysis_public(im):
    # The public analysis window has no third checkbox: Genre first follows Fast mode, 16 px after it as the
    # checkboxes are spaced (source pixels of analysis.mov at 31 s).
    band = (1430, 1340, 1600, 1412)
    piece = im.crop(band)
    im.paste(BG, (1127, band[1], band[2], band[3]))
    im.paste(piece, (1127, band[1]))
    return im


# The track menu with its platendoos submenu (trackmenu_multi_submenu.png): both menus taken out whole, on a calm
# background instead of the track list they opened over, at the home page cards' 5:3. Source pixels:
MAIN = (1412, 276, 1854, 966)       # the track menu, its 2-pixel border included
SUB = (1854, 636, 2212, 966)        # the submenu: 2 px border + 2 px padding, then items of 50 px from y 640
SUB_ITEMS = ['analysis', 'full', 'autocue', 'stems', 'genres']  # then a separator and one more full-edition item (890-962)
PUBLIC_ITEMS = {'analysis', 'autocue', 'stems', 'genres'}


def submenu(im, public):
    sub = im.crop(SUB)
    if not public:
        return sub
    parts = [sub.crop((0, 0, sub.width, 4))]
    for k, name in enumerate(SUB_ITEMS):
        if name in PUBLIC_ITEMS:
            parts.append(sub.crop((0, 4 + 50 * k, sub.width, 54 + 50 * k)))
    parts.append(sub.crop((0, sub.height - 4, sub.width, sub.height)))
    out = Image.new('RGB', (sub.width, sum(p.height for p in parts)))
    y = 0
    for p in parts:
        out.paste(p, (0, y)); y += p.height
    return out


def menus(im, public):
    main, sub = im.crop(MAIN), submenu(im, public)
    W, H = 1350, 810
    canvas = Image.new('RGB', (W, H))
    top, bottom = (30, 30, 33), (17, 17, 19)
    d = ImageDraw.Draw(canvas)
    for y in range(H):
        t = y / (H - 1)
        d.line([(0, y), (W, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))
    ox = (W - main.width - sub.width) // 2
    oy = (H - main.height) // 2
    sx, sy = ox + main.width, oy + main.height - sub.height  # juce opens it upward: last items line up
    shadow = Image.new('L', (W, H), 0)
    sd = ImageDraw.Draw(shadow)
    for (x, y, w, h) in [(ox, oy, main.width, main.height), (sx, sy, sub.width, sub.height)]:
        sd.rectangle([x, y + 12, x + w, y + h + 12], fill=150)
    shadow = shadow.filter(ImageFilter.GaussianBlur(22))
    canvas.paste((0, 0, 0), (0, 0), shadow)
    canvas.paste(main, (ox, oy))
    canvas.paste(sub, (sx, sy))
    return canvas


# name: (public picture, full edition's picture), each made from the frame
EDIT = {
    'analysis': (analysis_public, lambda im: im),
    'trackmenu': (lambda im: menus(im, True), lambda im: menus(im, False)),
}


def frame(src, t, tmp):
    png = os.path.join(tmp, 'f.png')
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-ss', str(t), '-i', src, '-frames:v', '1', png], check=True)
    return Image.open(png)

def main(names):
    os.makedirs(out, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for name, (src, t, box, width, blur) in SHOTS.items():
            if names and name not in names: continue
            if not os.path.exists(src):
                print(f'{name}: {src} is missing, kept the old image'); continue
            src_im = (frame(src, t, tmp) if t is not None else Image.open(src)).convert('RGB')
            for b in blur:
                src_im.paste(src_im.crop(b).filter(ImageFilter.GaussianBlur(14)), b[:2])
            if name in EDIT:
                variants = [(out, EDIT[name][0]), (out_full, EDIT[name][1])]
            else:
                variants = [(out_full if name in FULL_ONLY else out, lambda im: im)]
            for folder, edit in variants:
                if folder == out_full and not os.path.isdir(os.path.dirname(os.path.dirname(folder))):
                    continue  # a public source tree: no full edition
                im = edit(src_im.copy())
                if box: im = im.crop(box)
                if im.width > width: im = im.resize((width, round(im.height * width / im.width)), Image.LANCZOS)
                os.makedirs(folder, exist_ok=True)
                dest = os.path.join(folder, name + '.webp')
                im.save(dest, 'WEBP', quality=86, method=6)
                print(f'{os.path.relpath(dest, repo)}: {im.width}x{im.height}, {os.path.getsize(dest) // 1024} KB')

if __name__ == '__main__':
    main(sys.argv[1:])
