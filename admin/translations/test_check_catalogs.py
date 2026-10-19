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

    def test_german_text_in_other_catalog_fails(self):
        german = message('The sync operation is running.&lt;br/&gt;Do you want to stop it?', 'Die sync Operation läuft gerade.&lt;br/&gt;Wollen Sie sie stoppen?')
        self.assertEqual(self.check('es', german), ["deutscher Text: ['Sie', 'gerade', 'läuft', 'wollen']"])
        self.assertEqual(len(self.check('es', message('Aborted due to %1', 'Abgebrochen wegen %1'))), 1)
        # in den deutschen Katalogen selbst und bei ähnlichen Wörtern anderer Sprachen nicht
        self.assertEqual(self.check('de', german), [])
        self.assertEqual(self.check('es', message('Aborted due to %1', 'Cancelado debido a %1')), [])
        self.assertEqual(self.check('nl', message('Until tomorrow', 'Tot morgen')), [])
        self.assertEqual(self.check('sv', message('Account', 'Konto')), [])
        self.assertEqual(self.check('pt', message('Name of the files', 'Nome das pastas die')), [])

    def test_wrong_chinese_script_fails(self):
        self.assertEqual(self.check('zh_TW', message('Failed to rename file', '重命名文件失败')), ['falsche Schriftvariante: 败'])
        self.assertEqual(self.check('zh_HK', message('Use system proxy', '使用系统代理')), ['falsche Schriftvariante: 统'])
        self.assertEqual(self.check('zh-Hans', message('Update Failed', '更新失敗')), ['falsche Schriftvariante: 敗'])
        self.assertEqual(self.check('zh_CN', message('Hide', '隱藏')), ['falsche Schriftvariante: 隱'])
        # richtige Schrift, auch regionale Formen wie 群 in Taiwan und Hongkong
        self.assertEqual(self.check('zh_TW', message('Failed to rename file', '重新命名檔案失敗')), [])
        self.assertEqual(self.check('zh_HK', message('Users and groups', '使用者與群組')), [])
        self.assertEqual(self.check('zh_CN', message('Update Failed', '更新失败')), [])
        # andere Sprachen prüft die Schriftvariante nicht
        self.assertEqual(self.check('ja', message('Hide', '非表示')), [])

    def test_escaped_angle_brackets_must_match_source(self):
        source = '&amp;lt;not specified&amp;gt;'
        self.assertEqual(self.check('lo', message(source, '&lt;ບໍ່ໄດ້ລະບຸ&gt;')),
                         ['maskierte spitze Klammern (&lt; &gt;) anders als in der Quelle', 'spitze Klammer ohne HTML-Tag'])
        self.assertEqual(self.check('ar', message(source, 'غير محدد؛')), ['maskierte spitze Klammern (&lt; &gt;) anders als in der Quelle'])
        self.assertEqual(self.check('lo', message(source, '&amp;lt;ບໍ່ໄດ້ລະບຸ&amp;gt;')), [])


if __name__ == '__main__':
    unittest.main()
