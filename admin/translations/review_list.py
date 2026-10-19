#!/usr/bin/env python3
"""Listet die maschinell übersetzten bzw. aus der Geschwistersprache übernommenen Einträge zur Prüfung durch Muttersprachler.

Kennzeichen ist der Übersetzerkommentar im Katalog („maschinell übersetzt, ungeprüft“ oder „aus <Sprache> übernommen,
ungeprüft“). Nach der Prüfung den Kommentar in Qt Linguist („Translator comments“) löschen, dann fällt der Eintrag hier heraus.

Aufruf:
  python3 admin/translations/review_list.py              Übersicht je Sprache
  python3 admin/translations/review_list.py ru > ru.tsv  alle offenen Einträge einer Sprache (Kontext, Quelle, Übersetzung)
"""
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_catalogs import text_of  # noqa: E402

FOLDER = Path(__file__).resolve().parents[2] / 'translations'
MARKERS = ('maschinell übersetzt', 'übernommen, ungeprüft')


def entries(path):
    root = ET.parse(path).getroot()
    for context in root.iter('context'):
        for msg in context.findall('message'):
            comment = msg.findtext('translatorcomment') or ''
            tr = msg.find('translation')
            if tr is None or tr.get('type') in ('vanished', 'obsolete') or not any(m in comment for m in MARKERS):
                continue
            forms = [text_of(f) for f in tr.findall('numerusform')] if msg.get('numerus') == 'yes' else [text_of(tr)]
            yield context.findtext('name'), text_of(msg.find('source')), ' | '.join(forms), comment


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    if len(sys.argv) > 1:
        print('Kontext\tQuelltext\tÜbersetzung\tHerkunft')
        for ctx, source, translation, comment in entries(FOLDER / f'client_{sys.argv[1]}.ts'):
            esc = [s.replace('\t', ' ').replace('\r', '').replace('\n', '\\n') for s in (ctx, source, translation, comment)]
            print('\t'.join(esc))
        return 0
    print(f'{"Sprache":10} {"gesamt":>6} {"zu prüfen":>9}')
    for path in sorted(FOLDER.glob('client_*.ts')):
        root = ET.parse(path).getroot()
        total = sum(1 for m in root.iter('message') if (m.find('translation') is None or
                                                         m.find('translation').get('type') not in ('vanished', 'obsolete')))
        print(f'{path.stem[7:]:10} {total:6} {sum(1 for _ in entries(path)):9}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
