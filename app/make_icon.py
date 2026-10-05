#!/usr/bin/env python3
"""Draws the Show2Cam icons on the same gradient tile as Speak2Mic (a camera instead of speaker -> microphone):
    s2cpanel.ico   the control panel / device: a webcam
    s2csetup.ico   the installer: the webcam with a download badge
Small sizes (16-24 px) use a simplified glyph so they stay readable.
    python3 make_icon.py
"""
import os
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
W = 1024
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def gradient_tile(size, radius):
    """Rounded square with a diagonal violet -> cyan gradient and a soft top highlight (as Speak2Mic)."""
    y, x = np.mgrid[0:size, 0:size].astype(np.float32) / (size - 1)
    t = np.clip(0.65 * y + 0.35 * x, 0, 1)[..., None]
    c0 = np.array([108, 59, 255], np.float32)
    c1 = np.array([0, 194, 255], np.float32)
    rgb = c0 * (1 - t) + c1 * t
    hl = np.clip(1 - y * 2.2, 0, 1)[..., None] * 38
    rgb = np.clip(rgb + hl, 0, 255).astype(np.uint8)
    img = Image.fromarray(np.dstack([rgb, np.full((size, size), 255, np.uint8)]), 'RGBA')
    mask = Image.new('L', (size, size), 0)
    m = int(size * 0.035)
    ImageDraw.Draw(mask).rounded_rectangle((m, m, size - m, size - m), radius=radius, fill=255)
    img.putalpha(mask)
    return img


def draw_camera(d, small):
    """A webcam: rounded body with a big lens, a status light and a stand."""
    white = (255, 255, 255, 255)
    dark = (40, 30, 110, 255)
    if small:
        d.rounded_rectangle((170, 250, 854, 700), radius=150, fill=white)
        d.ellipse((362, 325, 662, 625), fill=dark)
        d.rectangle((452, 700, 572, 820), fill=white)
        d.rounded_rectangle((300, 800, 724, 880), radius=40, fill=white)
        return
    d.rounded_rectangle((200, 230, 824, 680), radius=170, fill=white)
    d.ellipse((357, 300, 667, 610), fill=dark)              # lens
    d.ellipse((422, 365, 602, 545), fill=(0, 150, 220, 255))
    d.ellipse((462, 395, 522, 455), fill=(220, 240, 255, 255))   # reflection
    d.ellipse((700, 300, 750, 350), fill=(80, 220, 120, 255))    # "on" light
    d.rectangle((467, 680, 557, 790), fill=white)          # stand
    d.rounded_rectangle((320, 780, 704, 850), radius=35, fill=white)


def draw_badge(d):
    """Download badge in the bottom right corner (installer)."""
    d.ellipse((610, 610, 960, 960), fill=(30, 200, 90, 255), outline=(255, 255, 255, 255), width=28)
    d.rectangle((755, 680, 815, 820), fill=(255, 255, 255, 255))
    d.polygon([(700, 800), (870, 800), (785, 890)], fill=(255, 255, 255, 255))


def render(size, badge):
    img = gradient_tile(W, 180)
    d = ImageDraw.Draw(img)
    draw_camera(d, size <= 24)
    if badge:
        draw_badge(d)
    return img.resize((size, size), Image.LANCZOS)


def save(name, badge):
    imgs = [render(s, badge) for s in SIZES]
    imgs[-1].save(os.path.join(HERE, name), format='ICO', sizes=[(s, s) for s in SIZES], append_images=imgs[:-1])


if __name__ == '__main__':
    save('s2cpanel.ico', False)
    save('s2csetup.ico', True)
    print('s2cpanel.ico, s2csetup.ico written')
