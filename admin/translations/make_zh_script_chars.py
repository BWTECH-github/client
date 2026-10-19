#!/usr/bin/env python3
"""Erzeugt zh_script_chars.json für check_catalogs.py aus den Zeichentabellen von OpenCC (Apache-2.0).

Aufruf: python3 admin/translations/make_zh_script_chars.py <OpenCC/data/dictionary> admin/translations/zh_script_chars.json
(erzeugt mit OpenCC 1.1.9: STCharacters.txt, TSCharacters.txt, TWVariants.txt, HKVariants.txt)

Je Katalog die Zeichen, die dort falsch sind: in zh_CN und zh-Hans die nur traditionell vorkommenden, in zh_TW und zh_HK
die nur vereinfacht vorkommenden. Nicht gemeldet werden regionale Standardformen, die OpenCC für Taiwan bzw. Hongkong
zurückwandelt, und die wenigen in REGIONAL_STANDARD, die dort ebenso üblich sind.
"""
import json
import sys
from pathlib import Path

# in Taiwan und Hongkong üblich, obwohl OpenCC sie ins traditionelle Gegenstück wandelt (群 statt 羣)
REGIONAL_STANDARD = set('群')


def load(path):
    table = {}
    for line in open(path, encoding='utf-8'):
        if not line.strip() or line.startswith('#'):
            continue
        key, values = line.rstrip('\n').split('\t')
        table[key] = values.split()
    return table


def main():
    folder = Path(sys.argv[1])
    st, ts = load(folder / 'STCharacters.txt'), load(folder / 'TSCharacters.txt')
    regions = {'zh_TW': load(folder / 'TWVariants.txt'), 'zh_HK': load(folder / 'HKVariants.txt')}

    def simplified_only(variants):
        found = set()
        for char, traditional in st.items():
            if len(char) != 1 or char in traditional or char in REGIONAL_STANDARD:
                continue
            # die regionale Form eines traditionellen Zeichens kann das vereinfachte selbst sein
            if any(char in variants.get(t, [t]) for t in traditional):
                continue
            found.add(char)
        return found

    traditional_only = {char for char, simplified in ts.items() if len(char) == 1 and char not in simplified}
    per_region = {region: simplified_only(variants) for region, variants in regions.items()}
    # Zeichen, die in beiden Richtungen vorkommen, nie melden
    both = set.union(*per_region.values()) & traditional_only
    data = {'source': 'OpenCC 1.1.9 data/dictionary (STCharacters, TSCharacters, TWVariants, HKVariants), Apache-2.0',
            'zh_CN': ''.join(sorted(traditional_only - both))}
    for region, chars in per_region.items():
        data[region] = ''.join(sorted(chars - both))
    with open(sys.argv[2], 'w', encoding='utf-8', newline='\n') as out:
        json.dump(data, out, ensure_ascii=False, indent=0, sort_keys=True)
        out.write('\n')
    print({key: len(value) for key, value in data.items()})


if __name__ == '__main__':
    main()
