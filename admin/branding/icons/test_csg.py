# -*- coding: utf-8 -*-
"""Selbsttest für csg.py: gerenderter Pfad muss der Punkt-Zugehörigkeit entsprechen.

Aufruf: python3 test_csg.py   (braucht cairosvg und Pillow)
Jede Form wird als SVG-Pfad gerendert (400 px) und Pixel für Pixel mit
shape.contains() verglichen. Kantenpixel (Deckung zwischen 5 % und 95 %)
zählen nicht; jede andere Abweichung ist ein Fehler.
"""
import io
import math
import sys

import cairosvg
from PIL import Image

from csg import (ArcBand, Circle, Polygon, Rect, RoundRect, Stadium, Transform,
                 path_data)

SIZE = 400
VIEW = 16.0


def check(name, shape):
    d = path_data(shape)
    svg = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %g %g">'
           '<path fill="#000" fill-rule="nonzero" d="%s"/></svg>') % (SIZE, SIZE, VIEW, VIEW, d)
    im = Image.open(io.BytesIO(cairosvg.svg2png(bytestring=svg.encode()))).convert("RGBA")
    alpha = im.getchannel("A").load()
    bad = 0
    k = VIEW / SIZE
    for y in range(SIZE):
        for x in range(SIZE):
            a = alpha[x, y] / 255
            if 0.05 < a < 0.95:
                continue
            inside = shape.contains(((x + 0.5) * k, (y + 0.5) * k))
            if inside != (a >= 0.5):
                bad += 1
    print("%-28s %s  (%d Abweichungen, %d Zeichen Pfad)" % (name, "OK " if bad == 0 else "FEHLER", bad, len(d)))
    return bad == 0


def main():
    cases = {
        "Kreis": Circle(8, 8, 5),
        "Vereinigung Kreise": Circle(5, 8, 3) | Circle(9, 7, 4) | Circle(12, 9, 2.5),
        "Differenz mit Lücke": (Circle(5, 8, 3) | Circle(9, 7, 4)) - Circle(11, 11, 4.5),
        "Ring": Circle(8, 8, 6) - Circle(8, 8, 4),
        "Stadium schräg": Stadium((4, 5), (12, 11), 2),
        "Haken (zwei Stadien)": Stadium((4, 8), (7, 11), 2) | Stadium((7, 11), (12, 5), 2),
        "Bogenband": ArcBand(8, 8, 5, 2, math.radians(200), math.radians(340)),
        "Abgerundetes Rechteck": RoundRect(2, 2, 12, 12, 3),
        "Knopf minus Glyphe": Circle(8, 8, 6) - (Rect(7, 4, 2, 5) | Circle(8, 11, 1.1)),
        "Polygon konkav": Polygon((2, 2), (14, 2), (14, 14), (8, 8), (2, 14)),
        "Skaliert/verschoben": Transform(Circle(5, 8, 3) | Circle(9, 7, 4), 0.8, 2, 1),
        "Schnitt": Circle(6, 8, 4) & Circle(10, 8, 4),
        "Rechtecke kantengleich": Rect(2, 2, 6, 4) | Rect(6, 2, 6, 4) | Rect(2, 6, 4, 6),
        "Rechtecke stoßend": Rect(2, 2, 4, 4) | Rect(6, 2, 4, 4),
        "gleicher Kreis zweimal": (Circle(8, 8, 4) | Circle(8, 8, 4)) - Circle(12, 12, 3),
    }
    ok = all([check(n, s) for n, s in cases.items()])
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
