"""Builds docs/social-preview.png (1280x640), the image GitHub shows when the repository is shared.

Run after updating docs/screenshots/hero.png:  python tools/make_social_preview.py
Then upload it in GitHub: Settings > General > Social preview.
"""
import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 1280, 640
FONTS = r"C:\Windows\Fonts"


def font(name, size):
    for candidate in (name, "segoeui.ttf"):
        path = os.path.join(FONTS, candidate)
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def main():
    img = Image.new("RGB", (W, H))
    top, bottom = (36, 40, 92), (18, 20, 46)
    d = ImageDraw.Draw(img)
    for y in range(H):
        t = y / (H - 1)
        d.line([(0, y), (W, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    # Soft glow behind the logo.
    glow = Image.new("L", (W, H), 0)
    ImageDraw.Draw(glow).ellipse([-120, 40, 520, 560], fill=90)
    glow = glow.filter(ImageFilter.GaussianBlur(120))
    img.paste(Image.new("RGB", (W, H), (99, 125, 255)), (0, 0), glow)

    # Screenshot on the right, with a shadow and rounded corners.
    shot = Image.open(os.path.join(ROOT, "docs", "screenshots", "hero.png")).convert("RGB")
    sw = 760
    shot = shot.resize((sw, int(shot.height * sw / shot.width)), Image.LANCZOS)
    x0, y0 = 560, 110
    shadow = Image.new("L", (W, H), 0)
    ImageDraw.Draw(shadow).rounded_rectangle([x0 + 10, y0 + 22, x0 + sw + 10, y0 + shot.height + 22], 18, fill=150)
    shadow = shadow.filter(ImageFilter.GaussianBlur(24))
    img.paste(Image.new("RGB", (W, H), (0, 0, 0)), (0, 0), shadow)
    mask = Image.new("L", shot.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, shot.width - 1, shot.height - 1], 14, fill=255)
    img.paste(shot, (x0, y0), mask)

    # Logo, name and tagline on the left.
    logo = Image.open(os.path.join(ROOT, "docs", "logo.png")).convert("RGBA").resize((120, 120), Image.LANCZOS)
    img.paste(logo, (72, 150), logo)
    d = ImageDraw.Draw(img)
    d.text((72, 292), "Featherweight", font=font("seguisb.ttf", 58), fill=(255, 255, 255))
    d.text((74, 372), "A light, fast PDF reader", font=font("segoeui.ttf", 30), fill=(214, 219, 255))
    d.text((74, 410), "for Windows", font=font("segoeui.ttf", 30), fill=(214, 219, 255))
    d.text((74, 480), "Forms \u00b7 Highlights & notes \u00b7 Search \u00b7 Print", font=font("segoeui.ttf", 21),
           fill=(160, 168, 220))
    d.text((74, 512), "No bloat \u00b7 no ads \u00b7 no telemetry", font=font("segoeui.ttf", 21), fill=(160, 168, 220))

    out = os.path.join(ROOT, "docs", "social-preview.png")
    img.save(out)
    print("wrote", out)


if __name__ == "__main__":
    main()
