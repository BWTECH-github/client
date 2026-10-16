#!/usr/bin/env python3
"""Selbsttest für check_catalogs.py: jede Fehlerart wird erkannt, erlaubte Abweichungen nicht."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_catalogs  # noqa: E402

HEAD = '<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE TS>\n<TS version="2.1" language="{lang}">\n<context>\n<name>Ctx</name>\n'
TAIL = '</context>\n</TS>\n'


def message(source, translation, kind=None, numerus=False):
    attr = f' type="{kind}"' if kind else ''
    if numerus:
        forms = ''.join(f'<numerusform>{f}</numerusform>' for f in translation)
        return f'<message numerus="yes"><source>{source}</source><translation{attr}>{forms}</translation></message>\n'
    return f'<message><source>{source}</source><translation{attr}>{translation}</translation></message>\n'


class CheckCatalogsTest(unittest.TestCase):
    def check(self, lang, *messages):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / f'client_{lang}.ts'
            path.write_text(HEAD.format(lang=lang) + ''.join(messages) + TAIL, encoding='utf-8')
            return [problem for _, _, problem in check_catalogs.check_file(path)]

    def test_complete_catalog_passes(self):
        self.assertEqual(self.check('fr', message('Open %1', 'Ouvrir %1'), message('<b>Note:</b> %1', '<b>Remarque :</b> %1')), [])

    def test_unfinished_entry_fails(self):
        self.assertEqual(self.check('fr', message('Open', '', 'unfinished')), ['unübersetzt'])

    def test_english_source_catalog_needs_only_plurals(self):
        self.assertEqual(self.check('en', message('Open', '', 'unfinished')), [])
        self.assertEqual(self.check('en', message('%n file(s)', ['', ''], 'unfinished', numerus=True)), ['unübersetzt'])

    def test_broken_placeholders_fail(self):
        self.assertEqual(len(self.check('ru', message('Open in %1', 'Открыто в% 1'))), 1)
        self.assertEqual(len(self.check('it', message('%1 and %n file(s)', ['%1 e %2 file', '%1 e %2 file'], numerus=True))), 2)

    def test_plural_without_number_only_where_first_form_means_one(self):
        self.assertEqual(self.check('cs', message('%n day(s) ago', ['včera', 'před %n dny', 'před %n dny'], numerus=True)), [])
        self.assertEqual(len(self.check('ru', message('%n day(s)', ['день', '%n дня', '%n дней'], numerus=True))), 1)

    def test_foreign_markup_fails(self):
        self.assertTrue(self.check('nl', message('Cannot connect to %1', 'Kan niet verbinden met &lt;i&gt;%1&lt;/i&gt;')))
        self.assertTrue(self.check('pt_BR', message("Click &lt;a href='%1'&gt;here&lt;/a&gt;", "Clique &lt;a href='%1'&gt;aqui&lt;/ a&gt;")))

    def test_control_character_as_text_fails(self):
        self.assertEqual(self.check('de', message('%1 version %2', '%1 Version %2&lt;byte value="xd"/&gt;')), ['Steuerzeichen als Text'])

    def test_brand_and_links_fail(self):
        self.assertEqual(self.check('sv', message('Quit %1', 'Avsluta ownCloud')), ['Platzhalter [] statt [\'%1\']', '„ownCloud“ im Text'])
        self.assertEqual(self.check('fr', message('Help', 'Aide https://example.com')), ['URL ohne Vorbild in der Quelle'])
        self.assertEqual(len(self.check('fr', message('Help', 'Aide https://owncloud.com'))), 2)


if __name__ == '__main__':
    unittest.main()
