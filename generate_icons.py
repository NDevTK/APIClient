"""Generate extension icons for APIClient Chrome extension."""
import math
from PIL import Image, ImageDraw, ImageFont

SIZES = [16, 48, 128]

def draw_icon(size):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    pad = max(1, size // 16)

    # Background: rounded rectangle, dark blue-gray
    bg_color = (30, 41, 59)  # slate-800
    r = size // 5
    draw.rounded_rectangle([pad, pad, size - pad - 1, size - pad - 1], radius=r, fill=bg_color)

    # Draw "API" braces + dot pattern representing network/data flow
    accent = (56, 189, 248)    # sky-400
    highlight = (250, 204, 21) # yellow-400

    cx, cy = size / 2, size / 2

    if size <= 16:
        # Simple design for 16px: two angle brackets < > with a dot
        lw = max(1, size // 8)
        # Left bracket <
        pts_l = [(cx - 1, cy - size * 0.25), (cx - size * 0.28, cy), (cx - 1, cy + size * 0.25)]
        draw.line(pts_l, fill=accent, width=lw)
        # Right bracket >
        pts_r = [(cx + 1, cy - size * 0.25), (cx + size * 0.28, cy), (cx + 1, cy + size * 0.25)]
        draw.line(pts_r, fill=accent, width=lw)
        # Center dot
        dr = max(1, size // 10)
        draw.ellipse([cx - dr, cy - dr, cx + dr, cy + dr], fill=highlight)
    else:
        lw = max(2, size // 20)

        # Left curly brace {
        brace_w = size * 0.18
        brace_h = size * 0.55
        bx = cx - size * 0.18
        top = cy - brace_h / 2
        bot = cy + brace_h / 2
        mid_indent = bx - brace_w * 0.5

        # Top curve
        draw.arc([mid_indent, top, bx + brace_w * 0.3, top + brace_h * 0.45],
                 start=-90, end=0, fill=accent, width=lw)
        # Bottom curve
        draw.arc([mid_indent, bot - brace_h * 0.45, bx + brace_w * 0.3, bot],
                 start=0, end=90, fill=accent, width=lw)
        # Middle notch
        draw.ellipse([mid_indent - lw, cy - lw, mid_indent + lw, cy + lw], fill=accent)

        # Right curly brace }
        rx = cx + size * 0.18
        mid_indent_r = rx + brace_w * 0.5

        draw.arc([rx - brace_w * 0.3, top, mid_indent_r, top + brace_h * 0.45],
                 start=180, end=270, fill=accent, width=lw)
        draw.arc([rx - brace_w * 0.3, bot - brace_h * 0.45, mid_indent_r, bot],
                 start=90, end=180, fill=accent, width=lw)
        draw.ellipse([mid_indent_r - lw, cy - lw, mid_indent_r + lw, cy + lw], fill=accent)

        # Three data-flow dots in the center (vertical)
        dot_r = max(2, size // 18)
        spacing = size * 0.12
        for i in range(-1, 2):
            dy = cy + i * spacing
            color = highlight if i == 0 else accent
            draw.ellipse([cx - dot_r, dy - dot_r, cx + dot_r, dy + dot_r], fill=color)

        # Small signal arcs (right side, suggesting network/broadcast)
        if size >= 48:
            arc_cx = cx + size * 0.35
            arc_cy = cy
            for j, radius in enumerate([size * 0.08, size * 0.14]):
                alpha = 200 - j * 60
                arc_color = (*accent[:3], alpha)
                arc_lw = max(1, lw - 1)
                draw.arc(
                    [arc_cx - radius, arc_cy - radius, arc_cx + radius, arc_cy + radius],
                    start=-45, end=45, fill=arc_color, width=arc_lw
                )

    return img

for size in SIZES:
    icon = draw_icon(size)
    icon.save(f"icons/icon{size}.png")
    print(f"Generated icons/icon{size}.png ({size}x{size})")
