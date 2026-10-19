#!/usr/bin/env python3
"""Prüft die Übersetzungskataloge (translations/client_*.ts) auf Lücken und Fehler, die in der Oberfläche sichtbar wären.

Aufruf: python3 admin/translations/check_catalogs.py [Verzeichnis]   (Standard: translations/ im Repository)

Fehler (Exit-Code 1):
- unfertige Einträge; einzige Ausnahme ist der Quellkatalog client_en.ts, der nur die Pluralformen braucht
- Platzhalter (%1 … %99, %n) fehlen oder sind hinzugekommen
- HTML-Tags, die in der Quelle nicht vorkommen, oder ein Link ohne schließendes </a>
- Steuerzeichen als Text ("<byte value=…/>"), fremde URLs, der Name „ownCloud“ ohne Vorbild in der Quelle
- deutscher Text in einem anderen Katalog (mindestens zwei eindeutig deutsche Wörter, die die Quelle nicht enthält)
- Zeichen der falschen chinesischen Schrift: traditionelle in zh_CN/zh-Hans, vereinfachte in zh_TW/zh_HK
  (Daten: zh_script_chars.json, erzeugt mit make_zh_script_chars.py aus den Zeichentabellen von OpenCC)
- maskierte spitze Klammern (&lt; &gt;) anders als in der Quelle oder eine spitze Klammer ohne HTML-Tag: Qt zeigt den
  Text als HTML und verschluckte „<nicht angegeben>“ als unbekanntes Tag
"""
import json
import re
import sys
import xml.etree.ElementTree as ET
from collections import Counter
from functools import lru_cache
from pathlib import Path

PLACEHOLDER = re.compile(r'%(?:L?\d+|n|Ln)')
TAG = re.compile(r'<(/?)([a-zA-Z][a-zA-Z0-9]*)\b[^>]*>')
URL = re.compile(r'https?://[^\s"\'<>)]+')
# „<“, auf das kein Tag folgt (Buchstabe, „/“ oder „!“)
ANGLE_WITHOUT_TAG = re.compile(r'<(?![a-zA-Z/!])')
# erste Pluralform gilt in diesen Sprachen nur für n = 1, dort darf %n fehlen ("včera", "eine Datei")
FIRST_FORM_IS_ONE = {'bg_BG', 'cs', 'de', 'de-informal', 'en', 'en_GB', 'en_US', 'es', 'et', 'it', 'nl', 'pl', 'pt', 'sq', 'sv'}

GERMAN_CATALOGS = {'de', 'de-informal'}
WORD = re.compile(r'[A-Za-zÄÖÜäöüß]+')
# Wörter, die es in keiner anderen Sprache der Kataloge gibt (nicht etwa „die“, „das“, „werden“ oder „Konto“, die im
# Niederländischen, Portugiesischen oder Schwedischen vorkommen); kleingeschrieben verglichen
GERMAN_WORDS = {
    'abgebrochen', 'abzuschließen', 'anwendung', 'aufgrund', 'beendet', 'bitte', 'datei', 'dateien', 'deaktiviert',
    'eine', 'einen', 'einem', 'erfolgreich', 'fehler', 'für', 'gerade', 'ignoriert', 'installiert', 'keine', 'konnte',
    'konnten', 'läuft', 'nicht', 'oder', 'ordner', 'pfad', 'über', 'und', 'verbindung', 'wegen', 'weniger', 'wird',
    'wollen', 'wurde', 'wurden',
}
# die förmliche Anrede nur großgeschrieben
GERMAN_FORMAL = {'Sie', 'Ihr', 'Ihre', 'Ihrem', 'Ihren', 'Ihrer'}
# Chinesisch: Kataloge, deren Schrift aus zh_script_chars.json geprüft wird
ZH_SCRIPT_KEY = {'zh_CN': 'zh_CN', 'zh-Hans': 'zh_CN', 'zh_TW': 'zh_TW', 'zh_HK': 'zh_HK'}


def text_of(el):
    """Text mit <byte value="xd"/>-Einschüben, wie lupdate Steuerzeichen ablegt."""
    if el is None:
        return ''
    parts = [el.text or '']
    for child in el:
        if child.tag == 'byte':
            value = child.get('value', '')
            parts.append(chr(int(value[1:], 16) if value.startswith('x') else int(value)))
        parts.append(child.tail or '')
    return ''.join(parts)


def tag_names(s):
    return Counter((m.group(1) + m.group(2)).lower() for m in TAG.finditer(s))


def german_words(s):
    words = WORD.findall(s)
    return {w.lower() for w in words if w.lower() in GERMAN_WORDS} | {w for w in words if w in GERMAN_FORMAL}


@lru_cache(maxsize=None)
def wrong_script_chars(lang):
    key = ZH_SCRIPT_KEY.get(lang)
    if not key:
        return frozenset()
    data = json.loads((Path(__file__).resolve().parent / 'zh_script_chars.json').read_text(encoding='utf-8'))
    return frozenset(data[key])


def problems(lang, source, form, index, numerus):
    found = []
    src_ph, ph = set(PLACEHOLDER.findall(source)), set(PLACEHOLDER.findall(form))
    if numerus and index == 0 and lang in FIRST_FORM_IS_ONE:
        src_ph.discard('%n')
        ph.discard('%n')
    if ph != src_ph:
        found.append(f'Platzhalter {sorted(ph)} statt {sorted(src_ph)}')
    # "<byte …/>" als Text meldet die eigene Prüfung weiter unten
    extra = set(tag_names(form)) - set(tag_names(source)) - {'byte'}
    if extra:
        found.append(f'HTML-Tags ohne Vorbild in der Quelle: {sorted(extra)}')
    if form.lower().count('<a ') > form.lower().count('</a>'):
        found.append('Link ohne </a>')
    if '<byte' in form:
        found.append('Steuerzeichen als Text')
    if set(URL.findall(form)) - set(URL.findall(source)):
        found.append('URL ohne Vorbild in der Quelle')
    if re.search(r'owncloud(?!\.online)', form, re.I) and not re.search(r'owncloud(?!\.online)', source, re.I):
        found.append('„ownCloud“ im Text')
    if lang not in GERMAN_CATALOGS:
        german = german_words(form) - german_words(source)
        if len(german) >= 2:
            found.append(f'deutscher Text: {sorted(german)}')
    wrong = sorted(set(form) & wrong_script_chars(lang))
    if wrong:
        found.append(f'falsche Schriftvariante: {"".join(wrong)}')
    if any(form.count(entity) != source.count(entity) for entity in ('&lt;', '&gt;')):
        found.append('maskierte spitze Klammern (&lt; &gt;) anders als in der Quelle')
    if ANGLE_WITHOUT_TAG.search(form) and not ANGLE_WITHOUT_TAG.search(source):
        found.append('spitze Klammer ohne HTML-Tag')
    return found


def check_file(path):
    lang = path.stem[len('client_'):]
    errors = []
    root = ET.parse(path).getroot()
    for context in root.iter('context'):
        ctx = context.findtext('name')
        for msg in context.findall('message'):
            tr = msg.find('translation')
            kind = tr.get('type') if tr is not None else 'unfinished'
            if kind in ('vanished', 'obsolete'):
                continue
            source = text_of(msg.find('source'))
            numerus = msg.get('numerus') == 'yes'
            if tr is None:
                forms = []
            elif numerus:
                forms = [text_of(f) for f in tr.findall('numerusform')]
            else:
                forms = [text_of(tr)]
            if kind == 'unfinished' or not forms or any(f == '' for f in forms):
                if lang == 'en' and not numerus:
                    continue
                errors.append((ctx, source, 'unübersetzt'))
                continue
            for i, form in enumerate(forms):
                for p in problems(lang, source, form, i, numerus):
                    errors.append((ctx, source, p))
    return errors


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    folder = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2] / 'translations'
    files = sorted(folder.glob('client_*.ts'))
    if not files:
        print(f'keine Kataloge in {folder}')
        return 1
    total = 0
    for path in files:
        errors = check_file(path)
        total += len(errors)
        for ctx, source, problem in errors:
            short = source.replace('\n', '⏎')[:80]
            print(f'::error file=translations/{path.name}::{problem} – [{ctx}] „{short}“')
    print(f'{len(files)} Kataloge geprüft, {total} Fehler')
    return 1 if total else 0


if __name__ == '__main__':
    sys.exit(main())
