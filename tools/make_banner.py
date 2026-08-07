#!/usr/bin/env python3
"""Draw the 720x320 Pebble appstore banner.

    python3 tools/make_banner.py

Writes developer-portal/banner_720x320.png -- the header image the store listing
shows above the description. Store artwork only; nothing here ships in the .pbw.

The watch screens are DRAWN rather than screenshotted, for the same two reasons
the sibling projects give. A 144x168 screen has to be scaled and letterboxed to
fill a 720x320 banner, which turns crisp 1px rules into mush. And the emulator
does not report the design's colours faithfully -- the accent is #FFAA00 on the
watch but comes back from `pebble screenshot` as #F1AA86.

Everything below is drawn from the same tokens the app uses (see
src/c/status_colour.h), so the banner and the watch cannot drift apart.
"""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "developer-portal/banner_720x320.png"
ICON = ROOT / "developer-portal/app_icon/app_icon_144.png"

W, H = 720, 320

# The design palette, as in src/c/status_colour.h.
PAPER = (255, 255, 255)
INK = (0, 0, 0)
ACCENT = (255, 170, 0)   # #FFAA00 -- arriving today, selection
DIM = (85, 85, 85)       # #555555
RULE = (170, 170, 170)   # #AAAAAA
GREEN = (0, 170, 85)     # #00AA55 -- delivered

# Banner furniture, matching the other listings in this workspace.
CANVAS = (242, 241, 238)
PANEL_X = 500            # brand panel starts here
TITLE = (27, 27, 25)
BODY = (74, 72, 68)

FONT_DIR = "/System/Library/Fonts/Supplemental/"
BOLD = FONT_DIR + "Arial Bold.ttf"
REG = FONT_DIR + "Arial.ttf"
NARROW_BOLD = FONT_DIR + "Arial Narrow Bold.ttf"
NARROW = FONT_DIR + "Arial Narrow.ttf"


def font(path, size):
    return ImageFont.truetype(path, size)


def rounded(size, radius, fill):
    """A rounded-rect image with an alpha channel, for bezels and the icon."""
    img = Image.new("RGBA", size, (0, 0, 0, 0))
    ImageDraw.Draw(img).rounded_rectangle([0, 0, size[0] - 1, size[1] - 1],
                                          radius=radius, fill=fill)
    return img


def shadow(size, radius, offset, blur=6):
    """A soft drop shadow the same shape as the card it sits under."""
    from PIL import ImageFilter
    pad = blur * 3
    img = Image.new("RGBA", (size[0] + pad * 2, size[1] + pad * 2), (0, 0, 0, 0))
    ImageDraw.Draw(img).rounded_rectangle(
        [pad, pad, pad + size[0], pad + size[1]], radius=radius,
        fill=(0, 0, 0, 70))
    return img.filter(ImageFilter.GaussianBlur(blur)), pad, offset


# ---------------------------------------------------------------------------
# The watch screens, drawn at their true 144x168. Two of them at this size fill
# the brand panel the way the sibling listings do, with the front one bleeding
# off the bottom edge.
# ---------------------------------------------------------------------------

S = 1.0
SW, SH = int(144 * S), int(168 * S)


def px(v):
    return int(round(v * S))


def list_screen():
    """The parcel list: glance bar, four rows, today stripe on row two."""
    img = Image.new("RGB", (SW, SH), PAPER)
    d = ImageDraw.Draw(img)
    head_h = px(20)
    d.rectangle([0, 0, SW, head_h], fill=INK)
    d.text((px(8), head_h // 2), "1 ARRIVING TODAY", font=font(NARROW_BOLD, px(11)),
           fill=PAPER, anchor="lm")

    rows = [("Running shoes", "Out for delivery", True, False),
            ("Camera lens", "Delivered 1:32 PM", False, True),
            ("Coffee beans", "In transit", False, False),
            ("Desk lamp", "Label created", False, False)]
    y = head_h
    row_h = px(43)
    for name, status, selected, today in rows:
        if selected:
            d.rectangle([0, y, SW, y + row_h], fill=ACCENT)
        elif today:
            d.rectangle([0, y, px(4), y + row_h], fill=ACCENT)
        d.line([(0, y + row_h), (SW, y + row_h)], fill=RULE)
        d.text((px(12), y + px(11)), name, font=font(NARROW_BOLD, px(15)),
               fill=INK, anchor="lm")
        d.text((px(12), y + px(29)), status, font=font(NARROW, px(12)),
               fill=INK if selected else DIM, anchor="lm")
        y += row_h
        if y > SH:
            break
    return img


def detail_screen():
    """One parcel: courier header, status block, the facts, the timeline."""
    img = Image.new("RGB", (SW, SH), PAPER)
    d = ImageDraw.Draw(img)
    head_h = px(20)
    d.rectangle([0, 0, SW, head_h], fill=INK)
    d.text((px(8), head_h // 2), "YUN EXPRESS", font=font(NARROW_BOLD, px(11)),
           fill=PAPER, anchor="lm")
    d.text((SW - px(8), head_h // 2), "2/4", font=font(NARROW, px(11)),
           fill=RULE, anchor="rm")

    hero_h = px(38)
    d.rectangle([0, head_h, SW, head_h + hero_h], fill=GREEN)
    d.text((px(9), head_h + px(13)), "Delivered", font=font(BOLD, px(17)),
           fill=PAPER, anchor="lm")
    d.text((px(9), head_h + px(29)), "Today 1:32 PM", font=font(NARROW, px(11)),
           fill=PAPER, anchor="lm")

    y = head_h + hero_h + px(7)
    for caption, value in (("NOW AT", "Singapore"), ("EXPECTED", "Arrived today")):
        d.text((px(9), y), caption, font=font(NARROW, px(10)), fill=DIM, anchor="lm")
        d.text((px(9), y + px(14)), value, font=font(NARROW_BOLD, px(14)),
               fill=INK, anchor="lm")
        y += px(30)

    d.line([(0, y - px(4)), (SW, y - px(4))], fill=RULE)
    y += px(4)
    d.text((px(18), y), "TODAY", font=font(NARROW, px(10)), fill=DIM, anchor="lm")
    y += px(15)
    for i, (label, when) in enumerate((("Delivered", "1:32 PM"),
                                       ("Out for delivery", "10:29 AM"),
                                       ("With local courier", "4:14 AM"))):
        dot = ACCENT if i == 0 else DIM
        d.rectangle([px(6), y - px(2), px(6) + px(5), y + px(3)], fill=dot)
        if i < 2:
            d.line([(px(8), y + px(4)), (px(8), y + px(22))], fill=RULE)
        d.text((px(18), y), label, font=font(NARROW, px(13)), fill=INK, anchor="lm")
        d.text((px(18), y + px(12)), when, font=font(NARROW, px(10)), fill=DIM,
               anchor="lm")
        y += px(24)
    return img


def bezel(screen, radius=18, frame=9):
    """Drop a screen into a black watch body."""
    body = rounded((screen.width + frame * 2, screen.height + frame * 2), radius,
                   (24, 24, 22, 255))
    body.paste(screen, (frame, frame))
    return body


def main():
    img = Image.new("RGB", (W, H), CANVAS)
    d = ImageDraw.Draw(img)

    # Brand panel, with a darker band at the edge like the sibling listings.
    d.rectangle([PANEL_X, 0, W, H], fill=ACCENT)
    d.rectangle([W - 26, 0, W, H], fill=(232, 155, 0))

    # An oversized, low-contrast copy of the mark, clipped to the panel so it
    # reads as a watermark rather than spilling onto the light side.
    mark = Image.open(ICON).convert("RGBA").resize((260, 260), Image.LANCZOS)
    mark.putalpha(40)
    panel = img.crop((PANEL_X, 0, W, H))
    panel.paste(mark, (-70, 40), mark)
    img.paste(panel, (PANEL_X, 0))

    # Icon, title, subtitle, tagline.
    icon = Image.open(ICON).convert("RGBA").resize((104, 104), Image.LANCZOS)
    mask = rounded((104, 104), 22, (255, 255, 255, 255)).split()[3]
    img.paste(icon, (48, 58), mask)

    d.text((180, 64), "Parcel Tracking", font=font(BOLD, 40), fill=TITLE)
    d.text((180, 122), "Every parcel you're waiting for,", font=font(REG, 21),
           fill=BODY)
    d.text((180, 151), "right on your wrist", font=font(REG, 21), fill=BODY)
    d.text((180, 208), "Glance. Check. Done.", font=font(BOLD, 28),
           fill=(201, 124, 0))

    # Two screens: the detail behind, the list in front and bleeding off the
    # bottom edge, as the other listings in this workspace do.
    back = bezel(detail_screen())
    front = bezel(list_screen())
    # The front card's top edge has to clear the detail's EXPECTED value, or the
    # banner shows a label with its answer hidden behind the other watch.
    for card, pos in ((back, (548, 16)), (front, (492, 150))):
        sh, pad, _ = shadow((card.width, card.height), 18, 0)
        img.paste((0, 0, 0), (pos[0] - pad + 4, pos[1] - pad + 8), sh)
        img.paste(card, pos, card)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    img.save(OUT)
    print("wrote", OUT.relative_to(ROOT), img.size)


if __name__ == "__main__":
    main()
