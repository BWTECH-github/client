# -*- coding: utf-8 -*-
"""Kleine 2D-CSG-Geometrie für die Symbol-Entwürfe.

Grundformen (Kreise, Polygone, Stadien, Bogenbänder, abgerundete Rechtecke)
lassen sich vereinigen, schneiden und abziehen. Das Ergebnis wird als reiner
SVG-Pfad (M/L/A/Z) ausgegeben – ohne mask, clipPath, Filter oder CSS, damit
jeder SVG-Renderer (QtSvg, librsvg, cairosvg, Browser) dasselbe Bild zeichnet.

Koordinaten wie in SVG: x nach rechts, y nach unten. Ein Winkel θ beschreibt
den Punkt c + r·(cos θ, sin θ); wachsendes θ läuft auf dem Bildschirm im
Uhrzeigersinn (entspricht sweep-flag = 1).

Verfahren: alle Kanten aller Grundformen sammeln, an allen Schnittpunkten
teilen, jedes Teilstück über einen Punkttest links/rechts der Kante als
Randstück erkennen und so ausrichten, dass die Fläche immer auf derselben
Seite liegt; danach die Stücke zu geschlossenen Schleifen verketten. Mit
fill-rule „nonzero“ ergibt das genau die Fläche des Ausdrucks.

Grenzen: Berührungen (Tangenten) und deckungsgleiche Kanten verschiedener
Grundformen vermeiden – dort ist die Seitenprüfung nicht eindeutig.
"""
import math

TAU = 2 * math.pi
_EPS_SIDE = 1e-4   # Abstand des Punkttests neben der Kante (Entwurfseinheiten)
_EPS_T = 1e-9


# ---------------------------------------------------------------- Kanten

class Line:
    def __init__(self, p0, p1):
        self.p0, self.p1 = p0, p1

    def point(self, t):
        return (self.p0[0] + (self.p1[0] - self.p0[0]) * t,
                self.p0[1] + (self.p1[1] - self.p0[1]) * t)

    def tangent(self, t):
        dx, dy = self.p1[0] - self.p0[0], self.p1[1] - self.p0[1]
        n = math.hypot(dx, dy)
        return (dx / n, dy / n)

    def sub(self, t0, t1):
        return Line(self.point(t0), self.point(t1))

    def reversed(self):
        return Line(self.p1, self.p0)

    @property
    def start(self):
        return self.p0

    @property
    def end(self):
        return self.p1


class Arc:
    """Kreisbogen um c mit Radius r, ab Winkel a0 um span (vorzeichenbehaftet)."""

    def __init__(self, c, r, a0, span):
        self.c, self.r, self.a0, self.span = c, r, a0, span

    def angle(self, t):
        return self.a0 + self.span * t

    def point(self, t):
        a = self.angle(t)
        return (self.c[0] + self.r * math.cos(a), self.c[1] + self.r * math.sin(a))

    def tangent(self, t):
        a = self.angle(t)
        s = 1 if self.span > 0 else -1
        return (-math.sin(a) * s, math.cos(a) * s)

    def param(self, theta):
        """Parameter t ∈ [0,1] für den Winkel theta oder None, wenn außerhalb."""
        if self.span > 0:
            d = (theta - self.a0) % TAU
            span = self.span
        else:
            d = (self.a0 - theta) % TAU
            span = -self.span
        if d <= span * (1 + 1e-12):
            return min(d / span, 1.0)
        if TAU - d < 1e-9:      # knapp vor dem Startpunkt (Rundung)
            return 0.0
        return None

    def sub(self, t0, t1):
        return Arc(self.c, self.r, self.angle(t0), self.span * (t1 - t0))

    def reversed(self):
        return Arc(self.c, self.r, self.a0 + self.span, -self.span)

    @property
    def start(self):
        return self.point(0)

    @property
    def end(self):
        return self.point(1)


# ---------------------------------------------------------- Schnittpunkte

def _line_param(l, p):
    dx, dy = l.p1[0] - l.p0[0], l.p1[1] - l.p0[1]
    return ((p[0] - l.p0[0]) * dx + (p[1] - l.p0[1]) * dy) / (dx * dx + dy * dy)


def _inter_line_line(a, b):
    (x1, y1), (x2, y2) = a.p0, a.p1
    (x3, y3), (x4, y4) = b.p0, b.p1
    den = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4)
    if abs(den) < 1e-14:
        # parallel; bei Deckungsgleichheit an den Endpunkten der jeweils anderen Strecke teilen
        dx, dy = x2 - x1, y2 - y1
        if abs((x3 - x1) * dy - (y3 - y1) * dx) > 1e-9 * math.hypot(dx, dy):
            return []
        out = []
        for u, p in ((0.0, b.p0), (1.0, b.p1)):
            t = _line_param(a, p)
            if -_EPS_T <= t <= 1 + _EPS_T:
                out.append((t, u))
        for t, p in ((0.0, a.p0), (1.0, a.p1)):
            u = _line_param(b, p)
            if -_EPS_T <= u <= 1 + _EPS_T:
                out.append((t, u))
        return out
    t = ((x1 - x3) * (y3 - y4) - (y1 - y3) * (x3 - x4)) / den
    u = -((x1 - x2) * (y1 - y3) - (y1 - y2) * (x1 - x3)) / den
    if -_EPS_T <= t <= 1 + _EPS_T and -_EPS_T <= u <= 1 + _EPS_T:
        return [(t, u)]
    return []


def _inter_line_arc(l, a):
    dx, dy = l.p1[0] - l.p0[0], l.p1[1] - l.p0[1]
    fx, fy = l.p0[0] - a.c[0], l.p0[1] - a.c[1]
    A = dx * dx + dy * dy
    B = 2 * (fx * dx + fy * dy)
    C = fx * fx + fy * fy - a.r * a.r
    disc = B * B - 4 * A * C
    if disc < -1e-12:
        return []
    disc = math.sqrt(max(disc, 0.0))
    out = []
    for t in {(-B - disc) / (2 * A), (-B + disc) / (2 * A)}:
        if -_EPS_T <= t <= 1 + _EPS_T:
            px, py = l.p0[0] + dx * t, l.p0[1] + dy * t
            u = a.param(math.atan2(py - a.c[1], px - a.c[0]))
            if u is not None:
                out.append((t, u))
    return out


def _inter_arc_arc(a, b):
    (x0, y0), r0 = a.c, a.r
    (x1, y1), r1 = b.c, b.r
    d = math.hypot(x1 - x0, y1 - y0)
    if d < 1e-12 and abs(r0 - r1) < 1e-12:
        # derselbe Kreis: an den Endpunkten des jeweils anderen Bogens teilen
        out = []
        for u in (0.0, 1.0):
            px, py = b.point(u)
            t = a.param(math.atan2(py - y0, px - x0))
            if t is not None:
                out.append((t, u))
        for t in (0.0, 1.0):
            px, py = a.point(t)
            u = b.param(math.atan2(py - y1, px - x1))
            if u is not None:
                out.append((t, u))
        return out
    if d < 1e-12 or d > r0 + r1 + 1e-12 or d < abs(r0 - r1) - 1e-12:
        return []
    l = (r0 * r0 - r1 * r1 + d * d) / (2 * d)
    h = math.sqrt(max(r0 * r0 - l * l, 0.0))
    xm, ym = x0 + l * (x1 - x0) / d, y0 + l * (y1 - y0) / d
    pts = {(xm + h * (y1 - y0) / d, ym - h * (x1 - x0) / d),
           (xm - h * (y1 - y0) / d, ym + h * (x1 - x0) / d)}
    out = []
    for px, py in pts:
        t = a.param(math.atan2(py - y0, px - x0))
        u = b.param(math.atan2(py - y1, px - x1))
        if t is not None and u is not None:
            out.append((t, u))
    return out


def _intersect(a, b):
    if isinstance(a, Line) and isinstance(b, Line):
        return _inter_line_line(a, b)
    if isinstance(a, Line) and isinstance(b, Arc):
        return _inter_line_arc(a, b)
    if isinstance(a, Arc) and isinstance(b, Line):
        return [(u, t) for t, u in _inter_line_arc(b, a)]
    return _inter_arc_arc(a, b)


# ------------------------------------------------------------- Grundformen

class Shape:
    def __or__(self, other):
        return Union(self, other)

    def __sub__(self, other):
        return Diff(self, other)

    def __and__(self, other):
        return Inter(self, other)


class Circle(Shape):
    def __init__(self, cx, cy, r):
        self.c, self.r = (cx, cy), r

    def contains(self, p):
        return (p[0] - self.c[0]) ** 2 + (p[1] - self.c[1]) ** 2 < self.r ** 2

    def edges(self):
        return [Arc(self.c, self.r, 0.0, math.pi), Arc(self.c, self.r, math.pi, math.pi)]


class Polygon(Shape):
    def __init__(self, *pts):
        self.pts = [tuple(p) for p in pts]

    def contains(self, p):
        x, y = p
        inside = False
        n = len(self.pts)
        for i in range(n):
            (x1, y1), (x2, y2) = self.pts[i], self.pts[(i + 1) % n]
            if (y1 > y) != (y2 > y):
                xi = x1 + (y - y1) * (x2 - x1) / (y2 - y1)
                if xi > x:
                    inside = not inside
        return inside

    def edges(self):
        n = len(self.pts)
        return [Line(self.pts[i], self.pts[(i + 1) % n]) for i in range(n)]


def Rect(x, y, w, h):
    return Polygon((x, y), (x + w, y), (x + w, y + h), (x, y + h))


class Stadium(Shape):
    """Strich von p0 nach p1 mit Breite w und runden Enden."""

    def __init__(self, p0, p1, w):
        self.p0, self.p1, self.h = p0, p1, w / 2

    def contains(self, p):
        (x0, y0), (x1, y1) = self.p0, self.p1
        dx, dy = x1 - x0, y1 - y0
        t = ((p[0] - x0) * dx + (p[1] - y0) * dy) / (dx * dx + dy * dy)
        t = max(0.0, min(1.0, t))
        return math.hypot(p[0] - (x0 + dx * t), p[1] - (y0 + dy * t)) < self.h

    def edges(self):
        (x0, y0), (x1, y1) = self.p0, self.p1
        a = math.atan2(y1 - y0, x1 - x0)
        nx, ny = -math.sin(a) * self.h, math.cos(a) * self.h
        return [Line((x0 + nx, y0 + ny), (x1 + nx, y1 + ny)),
                Arc((x1, y1), self.h, a + math.pi / 2, -math.pi),
                Line((x1 - nx, y1 - ny), (x0 - nx, y0 - ny)),
                Arc((x0, y0), self.h, a - math.pi / 2, -math.pi)]


class ArcBand(Shape):
    """Bogenstrich um c (Mittelradius R, Breite w) von Winkel a0 bis a1 (a1 > a0), gerade Enden."""

    def __init__(self, cx, cy, R, w, a0, a1):
        self.c, self.R, self.h, self.a0, self.a1 = (cx, cy), R, w / 2, a0, a1

    def contains(self, p):
        d = math.hypot(p[0] - self.c[0], p[1] - self.c[1])
        if abs(d - self.R) >= self.h:
            return False
        th = (math.atan2(p[1] - self.c[1], p[0] - self.c[0]) - self.a0) % TAU
        return th < self.a1 - self.a0

    def edges(self):
        c, ro, ri, a0, a1 = self.c, self.R + self.h, self.R - self.h, self.a0, self.a1
        pt = lambda r, a: (c[0] + r * math.cos(a), c[1] + r * math.sin(a))
        return [Arc(c, ro, a0, a1 - a0), Line(pt(ro, a1), pt(ri, a1)),
                Arc(c, ri, a1, a0 - a1), Line(pt(ri, a0), pt(ro, a0))]


class RoundRect(Shape):
    def __init__(self, x, y, w, h, r):
        self.x, self.y, self.w, self.h, self.r = x, y, w, h, r

    def contains(self, p):
        x, y = p
        if not (self.x < x < self.x + self.w and self.y < y < self.y + self.h):
            return False
        cx = min(max(x, self.x + self.r), self.x + self.w - self.r)
        cy = min(max(y, self.y + self.r), self.y + self.h - self.r)
        return (x - cx) ** 2 + (y - cy) ** 2 < self.r ** 2

    def edges(self):
        x, y, w, h, r = self.x, self.y, self.w, self.h, self.r
        hp = math.pi / 2
        return [Line((x + r, y), (x + w - r, y)), Arc((x + w - r, y + r), r, -hp, hp),
                Line((x + w, y + r), (x + w, y + h - r)), Arc((x + w - r, y + h - r), r, 0, hp),
                Line((x + w - r, y + h), (x + r, y + h)), Arc((x + r, y + h - r), r, hp, hp),
                Line((x, y + h - r), (x, y + r)), Arc((x + r, y + r), r, math.pi, hp)]


# ------------------------------------------------------------- Verknüpfung

class Union(Shape):
    def __init__(self, *parts):
        self.parts = parts

    def contains(self, p):
        return any(s.contains(p) for s in self.parts)

    def edges(self):
        return [e for s in self.parts for e in s.edges()]


class Diff(Shape):
    def __init__(self, a, *b):
        self.a, self.b = a, b

    def contains(self, p):
        return self.a.contains(p) and not any(s.contains(p) for s in self.b)

    def edges(self):
        return self.a.edges() + [e for s in self.b for e in s.edges()]


class Inter(Shape):
    def __init__(self, *parts):
        self.parts = parts

    def contains(self, p):
        return all(s.contains(p) for s in self.parts)

    def edges(self):
        return [e for s in self.parts for e in s.edges()]


class Transform(Shape):
    """Verschieben und gleichmäßig skalieren: p' = p·k + (dx, dy)."""

    def __init__(self, shape, k=1.0, dx=0.0, dy=0.0):
        self.s, self.k, self.dx, self.dy = shape, k, dx, dy

    def contains(self, p):
        return self.s.contains(((p[0] - self.dx) / self.k, (p[1] - self.dy) / self.k))

    def edges(self):
        k, dx, dy = self.k, self.dx, self.dy
        out = []
        for e in self.s.edges():
            if isinstance(e, Line):
                out.append(Line((e.p0[0] * k + dx, e.p0[1] * k + dy), (e.p1[0] * k + dx, e.p1[1] * k + dy)))
            else:
                out.append(Arc((e.c[0] * k + dx, e.c[1] * k + dy), e.r * k, e.a0, e.span))
        return out


# --------------------------------------------------------------- Randlinie

def _key(p):
    return (round(p[0], 6), round(p[1], 6))


def boundary(shape):
    """Randstücke von shape als Liste geschlossener, gleichsinnig ausgerichteter Schleifen."""
    edges = shape.edges()
    cuts = [set() for _ in edges]
    for i in range(len(edges)):
        for j in range(i + 1, len(edges)):
            for t, u in _intersect(edges[i], edges[j]):
                cuts[i].add(min(max(t, 0.0), 1.0))
                cuts[j].add(min(max(u, 0.0), 1.0))
    pieces = []
    for e, ts in zip(edges, cuts):
        ts = sorted({0.0, 1.0} | {t for t in ts if 1e-7 < t < 1 - 1e-7})
        for t0, t1 in zip(ts, ts[1:]):
            if t1 - t0 < 1e-7:
                continue
            piece = e.sub(t0, t1)
            m, (tx, ty) = piece.point(0.5), piece.tangent(0.5)
            n = (-ty, tx)
            left = shape.contains((m[0] + n[0] * _EPS_SIDE, m[1] + n[1] * _EPS_SIDE))
            right = shape.contains((m[0] - n[0] * _EPS_SIDE, m[1] - n[1] * _EPS_SIDE))
            if left == right:
                continue
            pieces.append(piece if left else piece.reversed())
    # doppelte Stücke (gleiche Kante mehrfach in der Formel) entfernen
    uniq, seen = [], set()
    for p in pieces:
        k = (_key(p.start), _key(p.end), _key(p.point(0.5)))
        if k not in seen:
            seen.add(k)
            uniq.append(p)
    # zu Schleifen verketten
    by_start = {}
    for idx, p in enumerate(uniq):
        by_start.setdefault(_key(p.start), []).append(idx)
    used = [False] * len(uniq)
    loops = []
    for i in range(len(uniq)):
        if used[i]:
            continue
        loop, cur = [], i
        while cur is not None and not used[cur]:
            used[cur] = True
            loop.append(uniq[cur])
            nxt = [j for j in by_start.get(_key(uniq[cur].end), []) if not used[j]]
            cur = nxt[0] if nxt else None
        if _key(loop[-1].end) != _key(loop[0].start):
            raise ValueError("offene Randschleife – Geometrie prüfen (Berührung/Tangente?)")
        loops.append(loop)
    return loops


def _fmt(v):
    s = ("%.3f" % v).rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def _merge(loop):
    """Benachbarte Stücke desselben Kreises bzw. derselben Geraden zusammenfassen."""
    merged = []
    for p in loop:
        q = merged[-1] if merged else None
        if isinstance(p, Arc) and isinstance(q, Arc) and _key(p.c) == _key(q.c) \
                and abs(p.r - q.r) < 1e-9 and (p.span > 0) == (q.span > 0) \
                and abs(q.span + p.span) < math.pi * 1.99:
            merged[-1] = Arc(q.c, q.r, q.a0, q.span + p.span)
        elif isinstance(p, Line) and isinstance(q, Line):
            d0, d1 = q.tangent(0), p.tangent(0)
            if abs(d0[0] * d1[1] - d0[1] * d1[0]) < 1e-9 and d0[0] * d1[0] + d0[1] * d1[1] > 0:
                merged[-1] = Line(q.p0, p.p1)
            else:
                merged.append(p)
        else:
            merged.append(p)
    return merged


def path_data(shape):
    """SVG-Pfaddaten (für fill-rule nonzero) der Fläche shape."""
    out = []
    for loop in boundary(shape):
        merged = _merge(loop)
        x, y = merged[0].start
        cmd = ["M%s %s" % (_fmt(x), _fmt(y))]
        for p in merged:
            if isinstance(p, Line):
                ex, ey = p.end
                cmd.append("L%s %s" % (_fmt(ex), _fmt(ey)))
                continue
            # Bögen in Stücke ≤ 90° teilen: manche Renderer nähern große Bögen
            # mit nur einer Bézierkurve an (Halbkreis ≈ 2 % Radiusfehler)
            n = max(1, math.ceil(abs(p.span) / (math.pi / 2) - 1e-9))
            sweep = 1 if p.span > 0 else 0
            for i in range(n):
                ex, ey = p.point((i + 1) / n)
                cmd.append("A%s %s 0 0 %d %s %s" % (_fmt(p.r), _fmt(p.r), sweep, _fmt(ex), _fmt(ey)))
        cmd.append("Z")
        out.append("".join(cmd))
    return "".join(out)


def area(shape, bbox, step=0.02):
    """Fläche per Rasterzählung (nur für Tests/Plausibilität)."""
    x0, y0, x1, y1 = bbox
    n = 0
    y = y0 + step / 2
    while y < y1:
        x = x0 + step / 2
        while x < x1:
            n += shape.contains((x, y))
            x += step
        y += step
    return n * step * step
