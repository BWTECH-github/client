# Übersetzungen des Desktop-Clients

## Sprachumfang

`translations/` enthält 28 Kataloge: alle Sprachen des letzten Community-Clients 6.0.3 und dazu `pl` und `sv` aus
upstream 7.x.

| Gruppe | Kataloge |
|---|---|
| seit jeher im Fork | de, de-informal, en, en_GB, en_US, es, et, fr, it, ja, lo, nl, pl, pt, pt_BR, sq, sv |
| nach 6.0.3 upstream gestrichen, wieder aufgenommen | ar, bg_BG, cs, ko, ru, tr, ug, zh-Hans, zh_CN, zh_HK, zh_TW |

Die wieder aufgenommenen Kataloge stammen aus dem jeweils jüngsten Community-Stand der upstream-Zweige 6.x und 7.x
und wurden mit `lupdate` an die heutigen Quelltexte angepasst. Der Bau übersetzt jede `client_*.ts` mit `lrelease` und
bettet sie als Ressource ein (`src/resources/CMakeLists.txt`); jedes Paket enthält damit alle Sprachen.

`client_en.ts` ist der Quellkatalog: Er braucht nur die Pluralformen (`numerus`), alle anderen Einträge bleiben offen
und zeigen den englischen Quelltext.

`client_zh-Hans.ts` trägt im Kopf `language="zh_CN"`: Das `lupdate` aus Qt 6.4 (CI-Job „translations“) erkennt
„zh-Hans“ nicht und ließe den Katalog sonst unbearbeitet. Das Attribut bestimmt nur die Pluralregeln, die für beide
gleich sind; welche Sprache der Client anbietet, folgt aus dem Dateinamen.

## Herkunft der Übersetzungen und Kennzeichnung

1. Community-Übersetzungen bleiben, wie sie sind.
2. Lücken wurden zuerst aus allen früheren upstream-Ständen derselben Sprache gefüllt (gleicher Quelltext,
   Platzhalter geprüft).
3. Was dann noch fehlte, ist maschinell übersetzt, mit dem Glossar der jeweiligen Sprache aus den vorhandenen
   Community-Begriffen.

Jede maschinelle Übersetzung trägt im Katalog den Übersetzerkommentar `maschinell übersetzt, ungeprüft`. Übernahmen
aus der Schwestersprache (zh_CN ↔ zh-Hans, zh_TW ↔ zh_HK) tragen `aus <Sprache> übernommen, ungeprüft`. Qt Linguist
zeigt den Kommentar unter „Translator comments“; nach der Prüfung durch Muttersprachler den Kommentar löschen.

```sh
python3 admin/translations/review_list.py        # Übersicht: Einträge je Sprache, davon noch zu prüfen
python3 admin/translations/review_list.py ru     # alle zu prüfenden Einträge einer Sprache als TSV
```

### Ersetzte Community-Übersetzungen

Neu übersetzt (und ebenso gekennzeichnet) wurden nur Einträge, die in der Oberfläche falsch erschienen:

- zerstörte Platzhalter: `% 1`, `٪ 1`, `％1`, `1%`, `%d`, `%2` statt `%n` (u. a. ug 122, zh_TW 15, nl 9, zh_HK 8,
  zh-Hans 8, it 7)
- falsche Sprache: englischer Text in ar (127), nl (6), tr (5) und vereinzelt weiteren, deutscher Text in es (29)
- falsche Schriftvariante: Langzeichen in zh-Hans (162, davon 154 aus zh_CN übernommen) und zh_CN (8), Kurzzeichen in
  zh_TW (44) und zh_HK (113, davon 69 aus zh_TW übernommen)
- verstümmeltes oder fremdes HTML (nl 3, sq 2)

Nach der Gegenprüfung nachgezogen (Einträge je Kontext):

- deutscher Text in es (6), ein englisches „to“ in pt_BR (1)
- Langzeichen in zh-Hans (11, aus zh_CN übernommen), Kurzzeichen in zh_TW (16) und zh_HK (24)
- „PROPFIND-Antwort ist XML“ statt „ist kein XML“ in zh_TW und zh_HK
- „<nicht angegeben>“ ohne Maskierung in lo und ug (Qt verschluckte es als unbekanntes Tag), ohne Klammern in ar

Mechanisch korrigiert, ohne den Wortlaut zu ändern: `<byte value="xd"/>` stand als Text in der Versionsausgabe aller
Sprachen; Leerzeichen am Rand wie in der Quelle (` (backup)` wird an Ordnernamen angehängt); `</ a>` und ein
abschließendes `<p>` statt `</p>`; britische Schreibung in en_US (synchronisation → synchronization u. a.). In nl gilt
für sechs Pluralformen wieder der Community-Wortlaut, nur ein `%2` statt `%n` ist berichtigt.

`check_catalogs.py` erkennt deutschen Text in anderen Katalogen, Zeichen der falschen chinesischen Schrift
(`zh_script_chars.json`, erzeugt mit `make_zh_script_chars.py` aus den Zeichentabellen von OpenCC) und maskierte spitze
Klammern, die von der Quelle abweichen.

## Schreibrichtung (rechts nach links)

Qt spiegelt die Oberfläche, wenn eine geladene Übersetzung den Text `QT_LAYOUT_DIRECTION` im Kontext
`QGuiApplication` mit `RTL` übersetzt. Der Eintrag steht in jedem Katalog (Markierung in `src/gui/translations.cpp`):
`RTL` für ar und ug, sonst `LTR`. Für Uigurisch liefert Qt keinen eigenen Katalog; ohne den Eintrag bliebe die
Oberfläche dort links nach rechts. `TranslationsTest` prüft das.

## Neue oder geänderte Texte

```sh
lupdate src -no-obsolete -locations relative -ts translations/client_*.ts
```

Danach jeden neuen Eintrag in **allen** Katalogen übersetzen: `client_de.ts` in der Sie-Form, `client_de-informal.ts`
in der Du-Form, die übrigen Sprachen mit den Begriffen, die der Katalog schon verwendet. Maschinelle Übersetzungen mit
dem Übersetzerkommentar kennzeichnen. In `client_en.ts` nur Pluralformen ausfüllen. Keine „ownCloud“-Namen und keine
Links, die nicht schon im Quelltext stehen.

```sh
python3 admin/translations/check_catalogs.py     # dieselbe Prüfung wie der CI-Job „translations“
```

Der CI-Job `translations` bricht ab, wenn ein Katalog einen Quelltext nicht kennt, ein Eintrag unübersetzt ist,
Platzhalter fehlen oder hinzukommen, fremdes HTML auftaucht, deutscher Text in einem anderen Katalog steht, ein
chinesischer Katalog die falsche Schrift verwendet oder ein Katalog sich nicht kompilieren lässt.
