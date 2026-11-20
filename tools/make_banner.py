#!/usr/bin/env python3
"""Generates the CIA Home Menu banner assets in meta/ (committed, so CI needs
neither PIL nor fonts):

  meta/banner.png  256x128 image shown on the top screen when the title is
                   selected (Midnight theme colours, app icon, name)
  meta/banner.wav  short two-note chime played with it (ffmpeg, < 3 s)

Usage: python3 tools/make_banner.py   (needs Pillow, DejaVu fonts, ffmpeg)
"""
import pathlib
import subprocess

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
META = ROOT / "meta"
FONT_DIR = pathlib.Path("/usr/share/fonts/truetype/dejavu")

BG, SURFACE, TEXT, MUTED, ACCENT = "#0E0E10", "#1B1B1F", "#F4F4F5", "#A1A1AA", "#F2B544"


def banner_png():
    img = Image.new("RGB", (256, 128), BG)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((8, 8, 247, 119), radius=14, fill=SURFACE)
    icon = Image.open(ROOT / "icon.png").convert("RGBA").resize((72, 72), Image.LANCZOS)
    img.paste(icon, (22, 28), icon)
    title = ImageFont.truetype(str(FONT_DIR / "DejaVuSans-Bold.ttf"), 30)
    sub = ImageFont.truetype(str(FONT_DIR / "DejaVuSans.ttf"), 13)
    d.text((108, 36), "Sonnis", font=title, fill=TEXT)
    d.text((109, 74), "Sonos remote", font=sub, fill=MUTED)
    d.rectangle((109, 94, 139, 96), fill=ACCENT)
    img.save(META / "banner.png")


def banner_wav():
    # Two soft notes (E5, B5) with fade-out: a gentle "ready" chime.
    expr = ("0.35*sin(2*PI*659.25*t)*exp(-4*t)*lt(t\\,0.35)"
            "+0.35*sin(2*PI*987.77*t)*exp(-3*(t-0.25))*gte(t\\,0.25)")
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "lavfi",
                    "-i", f"aevalsrc={expr}:s=22050:d=1.2",
                    "-ac", "1", "-sample_fmt", "s16", str(META / "banner.wav")],
                   check=True)


if __name__ == "__main__":
    META.mkdir(exist_ok=True)
    banner_png()
    banner_wav()
    print("wrote meta/banner.png, meta/banner.wav")
