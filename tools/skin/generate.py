# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the GNUbase widget skin SVGs and offline review page.

Usage: python tools/skin/generate.py resources/themes
Only the Python standard library is used. Output is deterministic. tests/theme_tests.cpp checks the output.
"""

from __future__ import annotations

import html
import json
import shutil
import sys
from pathlib import Path


THEMES = {
    "gnubase": {
        "colors": {
            "background": "#090b18", "surface": "#101426", "surfaceRaised": "#171c32",
            "border": "#303754", "text": "#f1f4ff", "textMuted": "#aeb7d0",
            "accent": "#10dff2", "accent2": "#e12ad8", "danger": "#ff5270",
            "warning": "#ffc44d", "success": "#39ddb2",
            "meterGradientStops": [
                {"offset": 0, "color": "#10dff2"}, {"offset": 0.68, "color": "#10dff2"},
                {"offset": 0.86, "color": "#9f5cff"}, {"offset": 1, "color": "#f02bd7"},
            ],
            "trackPalette": ["#e12ad8", "#10dff2", "#9b55f5", "#22d8ca", "#ffc44d", "#ff5b72", "#8248ef", "#4f8cff"],
        },
        "radii": {"small": 3, "control": 5, "panel": 6, "round": 999},
        "borderWidths": {"hairline": 1, "control": 1, "focus": 2},
        "fontSizes": {"small": 11, "body": 13, "label": 12, "title": 15, "display": 20},
        "fontFamilies": {"ui": "Inter", "counter": "JetBrains Mono"},
        "glow": True,
    },
    "dark": {
        "colors": {
            "background": "#151617", "surface": "#1e2022", "surfaceRaised": "#292c2f",
            "border": "#484c50", "text": "#f3f4f5", "textMuted": "#b7bbc0",
            "accent": "#5ea7b3", "accent2": "#7896bd", "danger": "#d76a73",
            "warning": "#cda85f", "success": "#67ad8b",
            "meterGradientStops": [
                {"offset": 0, "color": "#5ea7b3"}, {"offset": 0.72, "color": "#72b399"},
                {"offset": 0.9, "color": "#cda85f"}, {"offset": 1, "color": "#d76a73"},
            ],
            "trackPalette": ["#a76f9e", "#5ea7b3", "#8174ad", "#63a99e", "#c0a15e", "#c66f76", "#7865aa", "#6689ae"],
        },
        "radii": {"small": 3, "control": 4, "panel": 5, "round": 999},
        "borderWidths": {"hairline": 1, "control": 1, "focus": 2},
        "fontSizes": {"small": 11, "body": 13, "label": 12, "title": 15, "display": 20},
        "fontFamilies": {"ui": "Inter", "counter": "JetBrains Mono"},
        "glow": False,
    },
    "light": {
        "colors": {
            "background": "#eef1f5", "surface": "#ffffff", "surfaceRaised": "#f7f9fc",
            "border": "#b9c1cc", "text": "#17202d", "textMuted": "#566273",
            "accent": "#0969da", "accent2": "#6554c0", "danger": "#c92d42",
            "warning": "#9a6500", "success": "#157f5b",
            "meterGradientStops": [
                {"offset": 0, "color": "#0b84c6"}, {"offset": 0.72, "color": "#18a078"},
                {"offset": 0.9, "color": "#d58a00"}, {"offset": 1, "color": "#d12f48"},
            ],
            "trackPalette": ["#bb2d8f", "#087fa4", "#6f42c1", "#008f83", "#a86f00", "#c83d52", "#7048c8", "#276fd3"],
        },
        "radii": {"small": 3, "control": 5, "panel": 6, "round": 999},
        "borderWidths": {"hairline": 1, "control": 1, "focus": 2},
        "fontSizes": {"small": 11, "body": 13, "label": 12, "title": 15, "display": 20},
        "fontFamilies": {"ui": "Inter", "counter": "JetBrains Mono"},
        "glow": False,
    },
}

NINE_NAMES = [
    "dialog", "dialog-titlebar", "panel", "panel-header", "toolbar", "display", "tooltip", "focus-ring",
    "button-normal", "button-hover", "button-pressed", "button-disabled", "button-checked",
    "button-primary-normal", "button-primary-hover", "button-primary-pressed",
    "toggle-off", "toggle-hover", "toggle-mute", "toggle-solo", "toggle-record", "toggle-monitor",
    "field-normal", "field-focus", "field-disabled", "dropdown-normal", "dropdown-hover", "dropdown-open",
    "menu", "menu-item-hover", "menu-separator", "list", "list-item-hover", "list-item-selected",
    "tabbar", "tab-normal", "tab-hover", "tab-active",
    "scrollbar-track-vertical", "scrollbar-track-horizontal", "scrollbar-thumb-vertical", "scrollbar-thumb-horizontal",
    "scrollbar-thumb-vertical-hover", "scrollbar-thumb-horizontal-hover", "slider-track", "slider-fill",
    "fader-track", "meter-background",
]

FIXED_NAMES = [
    "dropdown-arrow", "checkbox-off", "checkbox-on", "checkbox-mixed", "radio-off", "radio-on",
    "slider-thumb", "fader-cap", "knob-base", "knob-pointer", "meter-fill", "meter-peak",
    "splitter-grip-vertical", "splitter-grip-horizontal",
]

SIZES = {
    "dialog": (32, 32), "dialog-titlebar": (32, 24), "panel": (24, 24), "panel-header": (24, 20),
    "toolbar": (24, 24), "display": (24, 24), "tooltip": (24, 24), "focus-ring": (20, 20),
    "button": (24, 24), "toggle": (20, 20), "field": (24, 24), "dropdown": (24, 24),
    "menu": (24, 24), "menu-item": (24, 20), "menu-separator": (16, 8), "list": (24, 24),
    "list-item": (24, 20), "tabbar": (24, 24), "tab": (24, 24), "scrollbar-track-vertical": (10, 24),
    "scrollbar-track-horizontal": (24, 10), "scrollbar-thumb-vertical": (10, 24),
    "scrollbar-thumb-horizontal": (24, 10), "slider-track": (24, 8), "slider-fill": (24, 8),
    "fader-track": (12, 32), "meter-background": (12, 32),
    "dropdown-arrow": (12, 8), "checkbox": (18, 18), "radio": (18, 18), "slider-thumb": (16, 16),
    "fader-cap": (22, 12), "knob-base": (40, 40), "knob-pointer": (40, 40), "meter-fill": (12, 96),
    "meter-peak": (12, 4), "splitter-grip-vertical": (8, 24), "splitter-grip-horizontal": (24, 8),
}


def key_for(name: str) -> str:
    for key in sorted(SIZES, key=len, reverse=True):
        if name == key or name.startswith(key + "-"):
            return key
    raise KeyError(name)


def mix(a: str, b: str, amount: float) -> str:
    av = tuple(int(a[i:i + 2], 16) for i in (1, 3, 5))
    bv = tuple(int(b[i:i + 2], 16) for i in (1, 3, 5))
    return "#" + "".join(f"{round(x + (y - x) * amount):02x}" for x, y in zip(av, bv))


def svg_doc(w: int, h: int, body: str) -> str:
    return ("<!-- SPDX-License-Identifier: GPL-3.0-or-later -->\n"
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}">\n'
            f"{body}\n</svg>\n")


def rect(w, h, fill, stroke, radius=4, sw=1, opacity=None, inset=.5):
    op = f' opacity="{opacity}"' if opacity is not None else ""
    return (f'<rect x="{inset}" y="{inset}" width="{w-2*inset}" height="{h-2*inset}" '
            f'rx="{radius}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{op}/>')


def glow_rect(w, h, color, radius=5):
    return (f'<rect x="1.5" y="1.5" width="{w-3}" height="{h-3}" rx="{radius}" fill="none" '
            f'stroke="{color}" stroke-width="3" opacity=".12"/>'
            f'<rect x="2.5" y="2.5" width="{w-5}" height="{h-5}" rx="{max(1,radius-1)}" fill="none" '
            f'stroke="{color}" stroke-width="1" opacity=".32"/>')


def nine_svg(name: str, theme: dict) -> tuple[int, int, str, dict]:
    c = theme["colors"]
    key = key_for(name)
    w, h = SIZES[key]
    r = theme["radii"]["control"]
    bg, border = c["surfaceRaised"], c["border"]
    accent = c["accent"]
    inset = 6 if min(w, h) >= 20 else 3
    extra = ""

    if name == "dialog": bg = c["surface"]
    elif name == "dialog-titlebar":
        bg = mix(c["surfaceRaised"], c["accent2"], .10); r = theme["radii"]["panel"]
        extra = f'<line x1="1" y1="{h-1.5}" x2="{w-1}" y2="{h-1.5}" stroke="{c["accent2"]}" opacity=".5"/>'
    elif name in ("panel", "list", "menu"):
        bg = c["surface"]
    elif name in ("panel-header", "toolbar", "tabbar"):
        bg = mix(c["surface"], c["surfaceRaised"], .55)
    elif name == "display":
        bg = mix(c["background"], "#000000", .20); border = mix(c["border"], c["accent"], .20)
        extra = f'<line x1="5" y1="{h-4.5}" x2="{w-5}" y2="{h-4.5}" stroke="{c["accent"]}" opacity=".35"/>'
    elif name == "tooltip":
        bg = mix(c["surfaceRaised"], c["background"], .25); border = mix(c["border"], c["textMuted"], .25)
    elif name == "focus-ring":
        bg = "none"; border = accent
        extra = glow_rect(w, h, accent, r) if theme["glow"] else ""
    elif name.startswith("button-primary"):
        if name.endswith("normal"): bg = accent
        elif name.endswith("hover"): bg = mix(accent, "#ffffff", .16)
        else: bg = mix(accent, "#000000", .24)
        border = mix(accent, "#ffffff", .22)
        extra = glow_rect(w, h, accent, r) if theme["glow"] and not name.endswith("pressed") else ""
    elif name.startswith("button-"):
        state = name.removeprefix("button-")
        if state == "hover": bg = mix(bg, accent, .13); border = mix(border, accent, .45)
        elif state == "pressed": bg = mix(c["background"], accent, .12); border = accent
        elif state == "disabled": bg = mix(c["surface"], c["background"], .25); border = mix(border, bg, .45)
        elif state == "checked":
            bg = mix(bg, c["accent2"], .22); border = c["accent2"]
            extra = glow_rect(w, h, c["accent2"], r) if theme["glow"] else ""
    elif name.startswith("toggle-"):
        state = name.removeprefix("toggle-"); r = 3
        colors = {"mute": c["warning"], "solo": c["accent"], "record": c["danger"], "monitor": c["success"]}
        if state == "hover": bg = mix(bg, accent, .14); border = mix(border, accent, .5)
        elif state in colors:
            bg = mix(colors[state], c["background"], .10 if theme["glow"] else .24); border = colors[state]
            extra = glow_rect(w, h, colors[state], r) if theme["glow"] else ""
    elif name.startswith("field-") or name.startswith("dropdown-"):
        state = name.split("-")[-1]; bg = mix(c["background"], c["surface"], .28)
        if state in ("focus", "open"): border = accent; extra = glow_rect(w, h, accent, r) if theme["glow"] else ""
        elif state == "hover": border = mix(border, accent, .55)
        elif state == "disabled": bg = mix(c["surface"], c["background"], .35); border = mix(border, bg, .45)
    elif name == "menu-item-hover" or name == "list-item-hover" or name == "tab-hover":
        bg = mix(c["surfaceRaised"], accent, .12); border = "none"; r = 2
    elif name == "menu-separator":
        bg = "none"; border = "none"; r = 0; inset = 2
        extra = f'<line x1="2" y1="{h/2+.5}" x2="{w-2}" y2="{h/2+.5}" stroke="{c["border"]}"/>'
    elif name == "list-item-selected":
        bg = mix(c["surfaceRaised"], accent, .25); border = accent; r = 2
        extra = f'<rect x=".5" y="2" width="2" height="{h-4}" rx="1" fill="{accent}"/>'
        if theme["glow"]: extra += glow_rect(w, h, accent, 2)
    elif name == "tab-active":
        bg = c["surfaceRaised"]; border = c["border"]
        extra = f'<rect x="2" y="{h-3}" width="{w-4}" height="2" rx="1" fill="{accent}"/>'
        if theme["glow"]: extra += f'<rect x="4" y="{h-4}" width="{w-8}" height="3" fill="{accent}" opacity=".16"/>'
    elif name == "tab-normal":
        bg = mix(c["surface"], c["surfaceRaised"], .4); border = "none"
    elif name.startswith("scrollbar-track"):
        bg = mix(c["background"], c["surface"], .35); border = mix(c["border"], bg, .6); r = min(w,h)/2
    elif name.startswith("scrollbar-thumb"):
        hover = name.endswith("hover"); bg = mix(c["border"], accent, .42 if hover else .12); border = "none"; r = min(w,h)/2; inset = 2
    elif name == "slider-track":
        bg = mix(c["background"], c["border"], .30); border = c["border"]; r = 4; inset = 1
    elif name == "slider-fill":
        bg = accent; border = mix(accent, "#ffffff", .12); r = 4; inset = 1
        if theme["glow"]: extra = glow_rect(w, h, accent, 4)
    elif name == "fader-track":
        bg = mix(c["background"], "#000000", .12); border = c["border"]; r = 4; inset = 2
        extra = f'<line x1="{w/2+.5}" y1="4" x2="{w/2+.5}" y2="{h-4}" stroke="{mix(c["border"], c["textMuted"], .3)}"/>'
    elif name == "meter-background":
        bg = mix(c["background"], "#000000", .18); border = c["border"]; r = 3; inset = 1
        extra = "".join(f'<line x1="2" y1="{y+.5}" x2="{w-2}" y2="{y+.5}" stroke="{c["border"]}" opacity=".45"/>' for y in range(7, h-2, 7))

    main = rect(w, h, bg, border, r, 1, None, .5) if border != "none" else f'<rect x="0" y="0" width="{w}" height="{h}" rx="{r}" fill="{bg}"/>'
    body = main + extra
    return w, h, svg_doc(w, h, body), {"left": inset, "top": inset, "right": inset, "bottom": inset}


def fixed_svg(name: str, theme: dict) -> tuple[int, int, str]:
    c = theme["colors"]; key = key_for(name); w, h = SIZES[key]
    accent, bg, border = c["accent"], c["surfaceRaised"], c["border"]
    body = ""
    if name == "dropdown-arrow":
        body = f'<polyline points="2,2 6,6 10,2" fill="none" stroke="{c["textMuted"]}" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>'
    elif name.startswith("checkbox-"):
        state = name.split("-")[-1]
        body = rect(w, h, accent if state != "off" else bg, accent if state != "off" else border, 4)
        if state == "on": body += '<polyline points="4.5,9 7.5,12 13.5,5.5" fill="none" stroke="#ffffff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>'
        elif state == "mixed": body += '<line x1="5" y1="9" x2="13" y2="9" stroke="#ffffff" stroke-width="2" stroke-linecap="round"/>'
    elif name.startswith("radio-"):
        body = f'<circle cx="9" cy="9" r="7.5" fill="{bg}" stroke="{accent if name.endswith("on") else border}"/>'
        if name.endswith("on"): body += f'<circle cx="9" cy="9" r="4" fill="{accent}"/>'
    elif name == "slider-thumb":
        if theme["glow"]: body += f'<circle cx="8" cy="8" r="7" fill="{accent}" opacity=".14"/>'
        body += f'<circle cx="8" cy="8" r="5.5" fill="{accent}" stroke="{mix(accent,"#ffffff",.3)}"/>'
        body += '<circle cx="6.5" cy="6.5" r="1.2" fill="#ffffff" opacity=".6"/>'
    elif name == "fader-cap":
        body = rect(w, h, mix(bg, accent, .10), mix(border, accent, .45), 3)
        body += f'<line x1="5" y1="{h/2+.5}" x2="{w-5}" y2="{h/2+.5}" stroke="{accent}"/>'
    elif name == "knob-base":
        body = f'<circle cx="20" cy="20" r="17" fill="none" stroke="{c["border"]}" stroke-width="3" stroke-dasharray="68 22" transform="rotate(135 20 20)"/>'
        body += f'<circle cx="20" cy="20" r="13.5" fill="{mix(bg,c["background"],.22)}" stroke="{border}"/>'
        body += f'<path d="M8 29 A15 15 0 0 1 11 9" fill="none" stroke="{accent}" stroke-width="2" stroke-linecap="round" opacity=".9"/>'
        body += '<ellipse cx="16" cy="14" rx="5" ry="3" fill="#ffffff" opacity=".06" transform="rotate(-28 16 14)"/>'
    elif name == "knob-pointer":
        body = f'<line x1="20" y1="20" x2="20" y2="9" stroke="{c["text"]}" stroke-width="2" stroke-linecap="round"/>'
        body += f'<circle cx="20" cy="20" r="2" fill="{accent}"/>'
    elif name == "meter-fill":
        stops = "".join(f'<stop offset="{s["offset"]*100:g}%" stop-color="{s["color"]}"/>' for s in c["meterGradientStops"])
        body = f'<defs><linearGradient id="meter" x1="0" y1="1" x2="0" y2="0">{stops}</linearGradient></defs>'
        if theme["glow"]: body += '<rect x="1" y="0" width="10" height="96" rx="3" fill="url(#meter)" opacity=".18"/>'
        body += '<rect x="3" y="0" width="6" height="96" rx="2" fill="url(#meter)"/>'
        body += "".join(f'<line x1="3" y1="{y+.5}" x2="9" y2="{y+.5}" stroke="{c["background"]}" opacity=".35"/>' for y in range(7, 95, 7))
    elif name == "meter-peak":
        body = f'<rect x="1" y="1" width="10" height="2" rx="1" fill="{c["danger"]}"/>'
    elif name.startswith("splitter-grip"):
        if name.endswith("vertical"):
            body = "".join(f'<circle cx="4" cy="{y}" r="1" fill="{c["textMuted"]}" opacity=".7"/>' for y in (7,12,17))
        else:
            body = "".join(f'<circle cx="{x}" cy="4" r="1" fill="{c["textMuted"]}" opacity=".7"/>' for x in (7,12,17))
    return w, h, svg_doc(w, h, body)


def tokens_for(theme: dict) -> dict:
    result = dict(theme["colors"])
    result["cornerRadii"] = theme["radii"]
    result["borderWidths"] = theme["borderWidths"]
    result["fontSizes"] = theme["fontSizes"]
    result["fontFamilies"] = theme["fontFamilies"]
    return result


def luminance(color: str) -> float:
    """Relative luminance of a #rrggbb color, 0 for black to 1 for white."""
    def linear(channel: int) -> float:
        c = channel / 255
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = (int(color[i:i + 2], 16) for i in (1, 3, 5))
    return 0.2126 * linear(r) + 0.7152 * linear(g) + 0.0722 * linear(b)


def theme_rcss(name: str, theme: dict, manifest: dict) -> str:
    """The theme's style sheet: colors, fonts and skins for every widget state, inside the theme's media query.

    resources/styles holds the layout; ui::Themes gives every document both (docs/features/themes.md).
    """
    c = theme["colors"]
    sizes = theme["fontSizes"]
    fonts = theme["fontFamilies"]
    widths = theme["borderWidths"]
    assets = manifest["assets"]
    # Text on an accent-colored fill: dark on a light accent, white on a dark one.
    on_accent = c["background"] if luminance(c["accent"]) > 0.4 else "#ffffff"

    def skin(asset: str) -> str:
        item = assets[asset]
        if "slice" not in item:
            return f"svg({item['file']})"
        s = item["slice"]
        return f"svg-ninepatch({item['file']}, {s['top']} {s['right']} {s['bottom']} {s['left']})"

    rules = [
        ("body", {"background-color": c["background"], "color": c["text"], "font-family": fonts["ui"],
                  "font-size": f"{sizes['body']}dp"}),
        ("h1", {"font-size": f"{sizes['title']}dp", "font-weight": "600"}),
        ("h2", {"font-size": f"{sizes['body']}dp", "font-weight": "600"}),
        (".text-small", {"font-size": f"{sizes['small']}dp", "color": c["textMuted"]}),
        (".text-label", {"font-size": f"{sizes['label']}dp"}),
        (".text-title", {"font-size": f"{sizes['title']}dp", "font-weight": "600"}),
        (".text-display", {"font-size": f"{sizes['display']}dp", "font-weight": "600"}),
        (".text-muted", {"color": c["textMuted"]}),
        (".text-accent", {"color": c["accent"]}),
        (".counter", {"font-family": fonts["counter"]}),
        ("svg.icon", {"image-color": c["text"]}),
        ("svg.icon.accent", {"image-color": c["accent"]}),
        ("svg.icon.muted", {"image-color": c["textMuted"]}),
        ("svg.icon.danger", {"image-color": c["danger"]}),
        # Containers
        (".dialog", {"decorator": skin("dialog")}),
        (".dialog-titlebar", {"decorator": skin("dialog-titlebar"), "font-weight": "600"}),
        (".panel", {"decorator": skin("panel")}),
        (".panel-header", {"decorator": skin("panel-header"), "color": c["textMuted"],
                           "font-size": f"{sizes['label']}dp"}),
        (".toolbar", {"decorator": skin("toolbar")}),
        (".display", {"decorator": skin("display"), "font-family": fonts["counter"], "color": c["text"]}),
        (".tooltip", {"decorator": skin("tooltip"), "font-size": f"{sizes['small']}dp"}),
        (".focus-ring", {"decorator": skin("focus-ring")}),
        # Menus and lists, the drop-down list's too
        (".menu, select selectbox", {"decorator": skin("menu")}),
        (".menu-item:hover, select selectbox option:hover", {"decorator": skin("menu-item-hover")}),
        (".menu-separator", {"decorator": skin("menu-separator")}),
        (".list", {"decorator": skin("list")}),
        (".list-item:hover", {"decorator": skin("list-item-hover")}),
        (".list-item.selected, select selectbox option:checked", {"decorator": skin("list-item-selected")}),
        # Tabs
        ("tabset tabs", {"decorator": skin("tabbar")}),
        ("tabset tab", {"decorator": skin("tab-normal"), "color": c["textMuted"]}),
        ("tabset tab:hover", {"decorator": skin("tab-hover"), "color": c["text"]}),
        ("tabset tab:selected", {"decorator": skin("tab-active"), "color": c["text"]}),
        # Buttons
        ("button", {"decorator": skin("button-normal"), "color": c["text"]}),
        ("button:hover", {"decorator": skin("button-hover")}),
        ("button:active", {"decorator": skin("button-pressed")}),
        ("button.checked", {"decorator": skin("button-checked")}),
        ("button:disabled", {"decorator": skin("button-disabled"), "color": c["textMuted"]}),
        ("button.primary", {"decorator": skin("button-primary-normal"), "color": on_accent}),
        ("button.primary:hover", {"decorator": skin("button-primary-hover")}),
        ("button.primary:active", {"decorator": skin("button-primary-pressed")}),
        ("button:focus-visible", {"decorator": f"{skin('button-normal')}, {skin('focus-ring')}"}),
        ("button.primary:focus-visible", {"decorator": f"{skin('button-primary-normal')}, {skin('focus-ring')}"}),
        ("button svg.icon", {"image-color": c["text"]}),
        ("button:disabled svg.icon", {"image-color": c["textMuted"]}),
        ("button.primary svg.icon", {"image-color": on_accent}),
        # Toggle buttons: mute, solo, record and monitor
        ("button.toggle", {"decorator": skin("toggle-off"), "color": c["textMuted"],
                           "font-size": f"{sizes['small']}dp", "font-weight": "600"}),
        ("button.toggle:hover", {"decorator": skin("toggle-hover"), "color": c["text"]}),
    ]
    for kind in ("mute", "solo", "record", "monitor"):
        rules.append((f"button.toggle.{kind}.checked", {"decorator": skin(f"toggle-{kind}"), "color": c["text"]}))
    rules += [
        # Text fields
        ("input.text, input.password, textarea", {"decorator": skin("field-normal"), "color": c["text"]}),
        ("input.text:focus, input.password:focus, textarea:focus", {"decorator": skin("field-focus")}),
        ("input.text:disabled, input.password:disabled, textarea:disabled",
         {"decorator": skin("field-disabled"), "color": c["textMuted"]}),
        ("input.text selection, input.password selection, textarea selection",
         {"background-color": mix(c["surfaceRaised"], c["accent"], .45), "color": c["text"]}),
        # Drop-down lists: open or focused, hovered, normal
        ("select", {"decorator": skin("dropdown-normal"), "color": c["text"]}),
        ("select:hover", {"decorator": skin("dropdown-hover")}),
        ("select:focus", {"decorator": skin("dropdown-open")}),
        ("select:disabled", {"decorator": skin("field-disabled"), "color": c["textMuted"]}),
        ("select selectarrow, .dropdown-arrow", {"decorator": skin("dropdown-arrow")}),
        # Check boxes and radio buttons
        ("input.checkbox", {"decorator": skin("checkbox-off")}),
        ("input.checkbox:checked", {"decorator": skin("checkbox-on")}),
        ("input.checkbox.mixed", {"decorator": skin("checkbox-mixed")}),
        ("input.radio", {"decorator": skin("radio-off")}),
        ("input.radio:checked", {"decorator": skin("radio-on")}),
        ("input.checkbox:disabled, input.radio:disabled", {"opacity": "0.5"}),
        # Sliders and progress bars
        ("input.range slidertrack, progress", {"decorator": skin("slider-track")}),
        ("input.range sliderprogress, progress fill", {"decorator": skin("slider-fill")}),
        ("input.range sliderbar", {"decorator": skin("slider-thumb")}),
        # Scroll bars
        ("scrollbarvertical slidertrack", {"decorator": skin("scrollbar-track-vertical")}),
        ("scrollbarvertical sliderbar", {"decorator": skin("scrollbar-thumb-vertical")}),
        ("scrollbarvertical sliderbar:hover, scrollbarvertical sliderbar:active",
         {"decorator": skin("scrollbar-thumb-vertical-hover")}),
        ("scrollbarhorizontal slidertrack", {"decorator": skin("scrollbar-track-horizontal")}),
        ("scrollbarhorizontal sliderbar", {"decorator": skin("scrollbar-thumb-horizontal")}),
        ("scrollbarhorizontal sliderbar:hover, scrollbarhorizontal sliderbar:active",
         {"decorator": skin("scrollbar-thumb-horizontal-hover")}),
        ("scrollbarcorner", {"background-color": mix(c["background"], c["surface"], .35)}),
        # Mixer and editor graphics
        (".knob", {"decorator": skin("knob-base")}),
        (".knob-pointer", {"decorator": skin("knob-pointer")}),
        (".fader", {"decorator": skin("fader-track")}),
        (".fader-cap", {"decorator": skin("fader-cap")}),
        (".meter", {"decorator": skin("meter-background")}),
        (".meter-fill", {"decorator": skin("meter-fill")}),
        (".meter-peak", {"decorator": skin("meter-peak")}),
        (".splitter-grip-vertical", {"decorator": skin("splitter-grip-vertical")}),
        (".splitter-grip-horizontal", {"decorator": skin("splitter-grip-horizontal")}),
        # Canvas elements (docs/features/canvas.md): events and notes, waveforms, automation curves and rulers
        ("event-lanes", {"color": mix(c["surfaceRaised"], c["accent"], .35), "stroke-color": c["accent"],
                         "stroke-width": f"{widths['hairline']}dp"}),
        ("waveform", {"color": c["accent"]}),
        ("curve", {"stroke-color": c["accent2"], "stroke-width": f"{widths['focus']}dp"}),
        ("ruler", {"color": c["textMuted"], "stroke-color": c["border"], "stroke-width": f"{widths['hairline']}dp",
                   "font-size": f"{sizes['small']}dp"}),
    ]

    lines = ["/* SPDX-License-Identifier: GPL-3.0-or-later */",
             f"/* The {name} theme: colors, fonts and skins of GNUbase's widgets (docs/features/themes.md). Generated by",
             "   tools/skin/generate.py: change the generator and run it again rather than editing this file. */",
             f"@media (theme: {name})", "{"]
    for selector, declarations in rules:
        lines.append(f"	{selector}")
        lines.append("	{")
        lines.extend(f"		{prop}: {value};" for prop, value in declarations.items())
        lines.append("	}")
    lines.append("}")
    return "\n".join(lines) + "\n"


def write_theme(root: Path, name: str, theme: dict) -> dict:
    dest = root / name
    if dest.exists(): shutil.rmtree(dest)
    dest.mkdir(parents=True)
    assets = {}
    for asset in NINE_NAMES:
        w, h, data, slices = nine_svg(asset, theme)
        filename = asset + ".svg"
        (dest / filename).write_text(data, encoding="utf-8", newline="\n")
        assets[asset] = {"file": filename, "width": w, "height": h, "slice": slices}
    for asset in FIXED_NAMES:
        w, h, data = fixed_svg(asset, theme)
        filename = asset + ".svg"
        (dest / filename).write_text(data, encoding="utf-8", newline="\n")
        assets[asset] = {"file": filename, "width": w, "height": h}
    manifest = {"theme": name, "units": "dp", "assets": assets}
    (dest / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    (dest / "tokens.json").write_text(json.dumps(tokens_for(theme), indent=2) + "\n", encoding="utf-8", newline="\n")
    (dest / "theme.rcss").write_text(theme_rcss(name, theme, manifest), encoding="utf-8", newline="\n")
    return manifest


def asset_gallery(theme_name: str, manifest: dict) -> str:
    cards = []
    for name, item in manifest["assets"].items():
        src = f"{theme_name}/{item['file']}"
        one_w, one_h = item["width"], item["height"]
        stretch = ""
        if "slice" in item:
            s = item["slice"]
            slice_values = f"{s['top']} {s['right']} {s['bottom']} {s['left']}"
            examples = []
            for width, height in ((52, 28), (96, 40), (142, 58)):
                examples.append(f'<span class="slice" style="width:{width}px;height:{height}px;border-width:{s["top"]}px {s["right"]}px {s["bottom"]}px {s["left"]}px;border-image-source:url(\'{src}\');border-image-slice:{slice_values} fill"></span>')
            stretch = '<div class="stretches">' + "".join(examples) + "</div>"
        cards.append(f'<article class="asset"><code>{html.escape(name)}</code><div class="native"><img src="{src}" width="{one_w}" height="{one_h}" alt=""><img src="{src}" width="{one_w*2}" height="{one_h*2}" alt=""></div>{stretch}</article>')
    return "".join(cards)


def scene(theme_name: str, theme: dict) -> str:
    c = theme["colors"]
    vars_ = ";".join(f"--{k}:{v}" for k, v in c.items() if isinstance(v, str))
    return f'''<section class="scene" style="{vars_}">
      <div class="scene-dialog"><div class="scene-title">Channel Settings <span>×</span></div>
        <div class="scene-tabs"><b>General</b><span>Routing</span><span>Inserts</span></div>
        <div class="scene-body"><div class="form"><label>Track name</label><div class="input focus">Lead Synth</div>
          <label>Output</label><div class="input">Stereo Out <i>⌄</i></div>
          <div class="buttons"><button>Cancel</button><button class="primary">Apply</button><button disabled>Disabled</button></div>
          <div class="list"><div>Analog Synth</div><div class="selected">Lead Synth</div><div>Warm Pad</div></div></div>
          <div class="channel"><strong>Lead Synth</strong><div class="knob"><img src="{theme_name}/knob-base.svg"><img src="{theme_name}/knob-pointer.svg"></div>
            <div class="mixbody"><div class="fader"><div></div></div><div class="meter"><img src="{theme_name}/meter-fill.svg"></div></div>
            <div class="toggles"><span>M</span><span>S</span><span class="rec">R</span></div><em>5 &nbsp; Lead Synth</em></div>
        </div></div></section>'''


def write_preview(root: Path, manifests: dict) -> None:
    theme_sections = []
    for name, theme in THEMES.items():
        theme_sections.append(f'<section class="theme"><h2>{name}</h2>{scene(name, theme)}<details><summary>All {len(manifests[name]["assets"])} assets — 1×, 2×, and 9-slice stretch tests</summary><div class="gallery">{asset_gallery(name, manifests[name])}</div></details></section>')
    page = '''<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>GNUbase widget skin preview</title><style>
*{box-sizing:border-box}body{margin:0;background:#25282e;color:#f4f5f7;font:13px/1.4 system-ui,-apple-system,"Segoe UI",sans-serif}header{padding:20px 24px;background:#17191d;border-bottom:1px solid #444}h1{margin:0 0 4px;font-size:22px}header p{margin:0;color:#bfc4cc}.themes{display:grid;grid-template-columns:repeat(3,minmax(350px,1fr));gap:16px;padding:16px}.theme{min-width:0}.theme>h2{margin:0;padding:10px 14px;background:#181a1e;border:1px solid #444;border-radius:7px 7px 0 0;text-transform:capitalize}.scene{background:var(--background);color:var(--text);padding:16px;height:460px;border:1px solid #444}.scene-dialog{height:100%;background:var(--surface);border:1px solid var(--border);border-radius:7px;overflow:hidden;box-shadow:0 10px 22px #0005}.scene-title{height:40px;padding:10px 14px;font-weight:650;background:var(--surfaceRaised);border-bottom:1px solid var(--border)}.scene-title span{float:right;color:var(--textMuted)}.scene-tabs{height:38px;display:flex;gap:4px;padding:5px 9px 0;background:var(--surface);border-bottom:1px solid var(--border)}.scene-tabs>*{padding:7px 12px;color:var(--textMuted)}.scene-tabs b{color:var(--text);border-bottom:2px solid var(--accent)}.scene-body{display:grid;grid-template-columns:1fr 120px;gap:14px;padding:14px}.form label{display:block;color:var(--textMuted);font-size:11px;margin:5px 0}.input{height:31px;padding:6px 9px;background:var(--background);border:1px solid var(--border);border-radius:5px}.input.focus{border-color:var(--accent)}.input i{float:right;color:var(--textMuted)}.buttons{display:flex;gap:6px;margin:13px 0}.buttons button{padding:6px 10px;color:var(--text);background:var(--surfaceRaised);border:1px solid var(--border);border-radius:5px}.buttons .primary{color:#fff;background:var(--accent);border-color:var(--accent)}.buttons button:disabled{opacity:.45}.list{border:1px solid var(--border);border-radius:5px;overflow:hidden}.list div{padding:7px 9px;border-bottom:1px solid var(--border)}.list div:last-child{border:0}.list .selected{background:color-mix(in srgb,var(--accent) 25%,var(--surfaceRaised));border-left:2px solid var(--accent)}.channel{position:relative;padding:9px 9px 34px;text-align:center;background:var(--surfaceRaised);border:1px solid var(--border);border-radius:5px}.channel strong{display:block;color:var(--accent2);font-size:11px;white-space:nowrap;overflow:hidden}.knob{position:relative;width:40px;height:40px;margin:10px auto}.knob img{position:absolute;inset:0}.mixbody{height:165px;display:flex;justify-content:center;gap:13px}.fader{position:relative;width:12px;height:145px;margin-top:8px;background:var(--background);border:1px solid var(--border);border-radius:5px}.fader div{position:absolute;left:-5px;top:55px;width:20px;height:11px;background:var(--surfaceRaised);border:1px solid var(--accent);border-radius:3px}.meter{position:relative;width:12px;height:145px;overflow:hidden;background:var(--background);border:1px solid var(--border);border-radius:3px}.meter img{position:absolute;bottom:0;left:-1px;width:12px;height:96px}.toggles{display:flex;justify-content:center;gap:4px}.toggles span{padding:3px 6px;background:var(--surface);border:1px solid var(--border);border-radius:3px}.toggles .rec{color:#fff;background:var(--danger);border-color:var(--danger)}.channel em{position:absolute;bottom:0;left:0;right:0;padding:5px 2px;color:#fff;background:var(--accent2);font-style:normal;font-size:10px}details{background:#181a1e;border:1px solid #444;border-top:0}summary{cursor:pointer;padding:10px 13px;color:#cbd0d8}.gallery{padding:8px;display:grid;grid-template-columns:1fr;gap:7px}.asset{padding:7px;background:#21242a;border:1px solid #3c414a;border-radius:5px;overflow:hidden}.asset code{display:block;color:#d9deea;font-size:11px;overflow-wrap:anywhere}.native{min-height:48px;display:flex;align-items:center;gap:12px;padding:6px;background:repeating-conic-gradient(#30343b 0 25%,#272a30 0 50%) 0/12px 12px}.native img{object-fit:contain;image-rendering:auto}.stretches{display:flex;flex-wrap:wrap;gap:6px;align-items:center;padding-top:6px}.slice{display:inline-block;border-style:solid;border-image-repeat:stretch}@media(max-width:1150px){.themes{grid-template-columns:1fr}.gallery{grid-template-columns:repeat(2,1fr)}}@media(max-width:650px){.themes{padding:8px}.gallery{grid-template-columns:1fr}.scene-body{grid-template-columns:1fr 110px}}
</style></head><body><header><h1>GNUbase widget skins</h1><p>Offline comparison: composed controls first, then every source asset at 1× and 2× with three stretch tests for each 9-slice.</p></header><main class="themes">''' + "".join(theme_sections) + "</main></body></html>\n"
    (root / "preview.html").write_text(page, encoding="utf-8", newline="\n")


def write_readme(root: Path, count: int) -> None:
    nine = ", ".join(f"`{n}`" for n in NINE_NAMES)
    text = f'''# GNUbase widget skins

Generated by `python tools/skin/generate.py resources/themes`. The generator is the source of truth; edit its theme tokens and regenerate rather than hand-editing SVG output. `tests/theme_tests.cpp` checks every theme against the rules below.

## Layout and naming

Each theme (`gnubase`, `dark`, `light`) contains {count} SVG assets, `manifest.json`, `tokens.json`, and `theme.rcss`. Stateful assets use `<component>-<state>.svg`. Manifest dimensions and slice insets are dp at 1×. Fixed assets omit `slice`.

`theme.rcss` is the theme's style sheet: the colors, fonts, and skins of every widget state, all inside `@media (theme: <name>)`. GNUbase gives every document the shared style sheets in `resources/styles` and every theme's style sheet, and activates the theme the user chose; see `docs/features/themes.md`.

## 9-slice assets

{nine}.

Use the manifest's `left`, `top`, `right`, and `bottom` inset values. Corners are wholly inside those insets; only edges and centers should stretch. In RCSS they are drawn with `decorator: svg-ninepatch(<file>, <top> <right> <bottom> <left>)`, which rasterizes each skin once per UI scale. The offline `preview.html` renders every 9-slice at three sizes using CSS `border-image`.

## Rendering rules

- SVG user units are dp and match each file's `viewBox`, `width`, and `height`.
- Artwork uses the LunaSVG-safe subset: basic shapes and paths, groups, definitions, gradients, clipping/masking, opacity, and transforms. It contains no text, CSS, filters, scripts, raster images, or external references.
- Hairlines are aligned to half pixels where practical. Rounded controls use tokenized 3–6 dp radii.
- Soft default-theme glows are made from stacked translucent vector strokes; there are no blur filters. Dark is restrained and light has no glow.
- UI labels belong in RmlUi, not in the skin SVGs. Fixed layers such as `knob-pointer.svg` are intended to be transformed by application code around their center.
- `meter-fill.svg` is a full-height bottom-to-top gradient. Clip or crop it from the top to show the current level; overlay `meter-peak.svg` at the held peak.
- Colors, radii, border widths, and font sizes are in `tokens.json`. Track palette order is magenta, cyan, purple, teal, amber, coral, violet, blue.
- All generated source and output is GPL-3.0-or-later and carries an SPDX identifier.
'''
    (root / "README.md").write_text(text, encoding="utf-8", newline="\n")


def main() -> int:
    if len(sys.argv) != 2:
        print("Usage: python tools/skin/generate.py resources/themes", file=sys.stderr)
        return 2
    root = Path(sys.argv[1])
    root.mkdir(parents=True, exist_ok=True)
    manifests = {name: write_theme(root, name, theme) for name, theme in THEMES.items()}
    write_preview(root, manifests)
    write_readme(root, len(NINE_NAMES) + len(FIXED_NAMES))
    print(f"Wrote {len(manifests)} themes × {len(NINE_NAMES) + len(FIXED_NAMES)} SVG assets to {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
