#!/usr/bin/env python3
"""Selbsttest für make-update-feed.py.

Liest die erzeugten Feeds so, wie der Desktop-Client sie liest
(src/gui/updater/updateinfo.cpp, ocupdater.cpp, appimageupdater.cpp), und
prüft den Versionsvergleich mit der Logik von QVersionNumber. Die Vertragstests
lesen die Feldnamen und Kanalregeln direkt aus dem Client-Code, damit Generator
und Client nicht unbemerkt auseinanderlaufen.

Nur Standardbibliothek:

    python3 admin/release/test_make_update_feed.py -v
"""

import importlib.util
import io
import re
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from xml.dom import minidom

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent


def load_generator():
    spec = importlib.util.spec_from_file_location("make_update_feed", HERE / "make-update-feed.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


feed = load_generator()

RELEASES = "https://github.com/BWTECH-github/client/releases"
WEB = RELEASES + "/tag/v7.11.1"
SETUP = RELEASES + "/download/v7.11.1/owncloud.online-client-7.11.1-win-x64-Setup.exe"
ZSYNC = RELEASES + "/download/v7.11.1/owncloud.online-client-7.11.1-linux-x86_64.AppImage.zsync"
RC_WEB = RELEASES + "/tag/v11.0.0-rc1"
RC_SETUP = RELEASES + "/download/v11.0.0-rc1/owncloud.online-client-11.0.0-rc1-win-x64-Setup.exe"
ALL_PLATFORMS = {"win-x64": SETUP, "linux-x86_64": None, "macos-arm64": None}


# ---- Nachbau der Client-Logik ------------------------------------------------

def dom_text(node):
    """QDomElement::text(): aller Text und CDATA darunter, Kommentare nicht."""
    parts = []
    for child in node.childNodes:
        if child.nodeType in (child.TEXT_NODE, child.CDATA_SECTION_NODE):
            parts.append(child.data)
        elif child.nodeType == child.ELEMENT_NODE:
            parts.append(dom_text(child))
    return "".join(parts)


def parse_like_client(data):
    """UpdateInfo::parseString/parseElement (updateinfo.cpp:54-106).

    Der Client dekodiert die Antwort als UTF-8 (ocupdater.cpp:234), parst ohne
    Namensräume, verlangt das Wurzelelement owncloudclient, übernimmt die vier
    bekannten Kindelemente (ein späteres gewinnt) und übergeht alle anderen.
    Gibt None zurück, wo der Client ok=false meldet.
    """
    text = data.decode("utf-8") if isinstance(data, bytes) else data
    try:
        root = minidom.parseString(text.encode("utf-8")).documentElement
    except Exception:
        return None
    if root.tagName != "owncloudclient":
        return None
    info = {"version": "", "versionstring": "", "web": "", "downloadurl": ""}
    for child in root.childNodes:
        if child.nodeType == child.ELEMENT_NODE and child.tagName in info:
            info[child.tagName] = dom_text(child)
    return info


def split_qversion(text):
    """QVersionNumber::fromString(text, &suffixIndex): Zahlen durch Punkte getrennt und wo der Suffix beginnt."""
    segments = []
    pos = end = 0
    while True:
        match = re.match(r"[0-9]+", text[pos:])
        if not match or int(match.group()) > 2**31 - 1:
            break
        segments.append(int(match.group()))
        pos += match.end()
        end = pos
        if text[pos:pos + 1] != ".":
            break
        pos += 1
    # Tupelvergleich == QVersionNumber::compare, auch "1.2" < "1.2.0"
    return tuple(segments), end


def qversion(text):
    """QVersionNumber::fromString: der Suffix zählt nicht."""
    return split_qversion(text)[0]


def parse_version(text):
    """parseVersion() in ocupdater.cpp: Zahlen und Vorab-Suffix ohne führendes '-'."""
    text = text.strip()
    segments, end = split_qversion(text)
    suffix = text[end:]
    return segments, suffix[1:] if suffix.startswith("-") else suffix


def compare_suffixes(a, b):
    """compareSuffixes() in ocupdater.cpp: Ziffernfolgen als Zahl, der Rest ohne Groß-/Kleinschreibung."""
    tokens_a = re.findall(r"\d+|\D+", a)
    tokens_b = re.findall(r"\d+|\D+", b)
    for token_a, token_b in zip(tokens_a, tokens_b):
        if token_a[0].isdigit() and token_b[0].isdigit():
            key_a, key_b = int(token_a), int(token_b)
        else:
            key_a, key_b = token_a.lower(), token_b.lower()
        if key_a != key_b:
            return -1 if key_a < key_b else 1
    return (len(tokens_a) > len(tokens_b)) - (len(tokens_a) < len(tokens_b))


def compare_versions(a, b):
    """OCUpdater::compareVersions(): Major.Minor.Patch, dann Release vor Vorabversion, dann Suffix, dann alle Zahlen."""
    (numbers_a, suffix_a), (numbers_b, suffix_b) = parse_version(a), parse_version(b)
    core_a = tuple(numbers_a[i] if i < len(numbers_a) else 0 for i in range(3))
    core_b = tuple(numbers_b[i] if i < len(numbers_b) else 0 for i in range(3))
    if core_a != core_b:
        return -1 if core_a < core_b else 1
    if bool(suffix_a) != bool(suffix_b):
        return -1 if suffix_a else 1
    return compare_suffixes(suffix_a, suffix_b) or (numbers_a > numbers_b) - (numbers_a < numbers_b)


def is_newer_version(available, installed, skipped=""):
    """OCUpdater::isNewerVersion(); installiert wie OCUpdater::installedVersion(), z. B. 11.0.0.0-beta1."""
    if not parse_version(available)[0] or compare_versions(available, installed) <= 0:
        return False
    return not parse_version(skipped)[0] or compare_versions(available, skipped) > 0


def windows_updater(info, installed, skipped=""):
    """WindowsUpdater::versionInfoArrived (ocupdater.cpp)."""
    if not info["version"] or not is_newer_version(info["version"], installed, skipped):
        return "up-to-date"
    # ohne downloadurl nur der Hinweis mit Link auf <web> (showNewVersionAvailableWidget)
    return "download" if info["downloadurl"] else "show-web"


def passive_notifier(info, installed):
    """PassiveUpdateNotifier::versionInfoArrived (ocupdater.cpp): Linux ohne AppImage-Updater, macOS ohne Sparkle."""
    if not info["version"] or not is_newer_version(info["version"], installed):
        return "up-to-date"
    return "through-system"


def update_succeeded(installed, target):
    """OCUpdater::updateSucceeded (ocupdater.cpp), beim ersten Start nach dem Update."""
    return not is_newer_version(target, installed)


def client_download_file_name(url):
    """WindowsUpdater: Zieldatei = letztes Pfadstück der Adresse (ocupdater.cpp:354)."""
    return url[url.rfind("/") + 1:]


def build(**overrides):
    arguments = {"tag": "v7.11.1", "web": WEB, "platforms": dict(ALL_PLATFORMS)}
    arguments.update(overrides)
    return feed.build_feeds(**arguments)


# ---- Tests ---------------------------------------------------------------------

class ClientContract(unittest.TestCase):
    """Annahmen des Generators gegen den Client-Code dieses Stands."""

    def read_source(self, relative):
        path = REPO / relative
        if not path.is_file():
            self.skipTest(relative + " fehlt (Generator außerhalb des Repositorys?)")
        return path.read_text(encoding="utf-8")

    def test_feed_elements_are_the_ones_the_client_parses(self):
        source = self.read_source("src/gui/updater/updateinfo.cpp")
        root = re.findall(r'tagName\(\)\s*!=\s*QLatin1String\("(\w+)"\)', source)
        fields = re.findall(r'tagName\(\)\s*==\s*QLatin1String\("(\w+)"\)', source)
        self.assertEqual(root, [feed.ROOT_ELEMENT])
        self.assertEqual(sorted(fields), sorted(feed.FIELDS))

    def test_channel_rule_is_the_one_of_update_channel(self):
        source = self.read_source("src/gui/updater/updater.cpp")
        body = source[source.index("QString updateChannel()"):]
        body = body[:body.index("\n}\n")]
        prefixes = re.findall(r'suffix\.startsWith\(QLatin1String\("(\w+)"\)\)', body)
        self.assertEqual(sorted(prefixes), sorted(feed.BETA_SUFFIX_PREFIXES))
        self.assertIn('QStringLiteral("beta")', body)
        self.assertIn('QStringLiteral("stable")', body)

    def test_client_compares_with_the_prerelease_suffix(self):
        source = self.read_source("src/gui/updater/ocupdater.cpp")
        body = source[source.index("QString OCUpdater::installedVersion()"):]
        body = body[:body.index("\n}\n")]
        self.assertIn("Version::suffix()", body)
        self.assertIn("Version::versionWithBuildNumber()", body)

    def test_package_names_match_publish_release(self):
        publish = self.read_source(".github/workflows/publish-release.yml")
        update = self.read_source(".github/workflows/update-feed.yml")
        for package in ("win-x64-Setup.exe", "linux-x86_64.AppImage", "macos-arm64.dmg"):
            name = "owncloud.online-client-${VER}-" + package
            self.assertIn(name, publish)
            self.assertIn(name, update)


class FeedFiles(unittest.TestCase):

    def test_stable_release_feeds_both_channels_on_every_platform(self):
        names = set(build())
        expected = {
            "owncloud.online-update-%s-%s.xml" % (platform, channel)
            for platform in ("win-x64", "linux-x86_64", "macos-arm64")
            for channel in ("stable", "beta")
        }
        self.assertEqual(names, expected)

    def test_prerelease_feeds_only_the_beta_channel(self):
        feeds = build(tag="v11.0.0-rc1", web=RC_WEB, platforms={"win-x64": RC_SETUP})
        self.assertEqual(list(feeds), ["owncloud.online-update-win-x64-beta.xml"])

    def test_windows_feed_is_exactly_the_expected_document(self):
        expected = (
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            "<owncloudclient>\n"
            "  <version>7.11.1</version>\n"
            "  <versionstring>owncloud.online 7.11.1</versionstring>\n"
            "  <web>" + WEB + "</web>\n"
            "  <downloadurl>" + SETUP + "</downloadurl>\n"
            "</owncloudclient>\n"
        )
        self.assertEqual(build()["owncloud.online-update-win-x64-stable.xml"], expected)

    def test_client_reads_every_field(self):
        info = parse_like_client(build()["owncloud.online-update-win-x64-stable.xml"])
        self.assertEqual(info, {
            "version": "7.11.1",
            "versionstring": "owncloud.online 7.11.1",
            "web": WEB,
            "downloadurl": SETUP,
        })

    def test_platforms_without_download_only_link_the_web_page(self):
        feeds = build()
        for name in ("owncloud.online-update-macos-arm64-stable.xml", "owncloud.online-update-linux-x86_64-stable.xml"):
            self.assertNotIn("<downloadurl>", feeds[name])
            self.assertEqual(parse_like_client(feeds[name])["web"], WEB)

    def test_linux_download_is_the_zsync_file_for_the_appimage_updater(self):
        feeds = build(platforms={"linux-x86_64": ZSYNC})
        info = parse_like_client(feeds["owncloud.online-update-linux-x86_64-stable.xml"])
        self.assertEqual(info["downloadurl"], ZSYNC)

    def test_xml_special_characters_survive(self):
        web = "https://owncloud.online/download?os=win&channel=stable"
        info = parse_like_client(build(web=web)["owncloud.online-update-win-x64-stable.xml"])
        self.assertEqual(info["web"], web)

    def test_same_input_gives_identical_files(self):
        feeds = build()
        self.assertTrue(feeds)
        self.assertEqual(feeds, build())

    def test_downloaded_setup_gets_a_name_unique_to_the_version(self):
        info = parse_like_client(build()["owncloud.online-update-win-x64-stable.xml"])
        self.assertEqual(client_download_file_name(info["downloadurl"]),
                         "owncloud.online-client-7.11.1-win-x64-Setup.exe")

    def test_explicit_version_with_build_number(self):
        info = parse_like_client(build(version="7.11.1.4711")["owncloud.online-update-win-x64-stable.xml"])
        self.assertEqual(info["version"], "7.11.1.4711")

    def test_prerelease_version_keeps_the_suffix_of_the_tag(self):
        feeds = build(tag="v11.0.0-rc1", web=RC_WEB, platforms={"win-x64": RC_SETUP}, version="11.0.0.5")
        info = parse_like_client(feeds["owncloud.online-update-win-x64-beta.xml"])
        self.assertEqual(info["version"], "11.0.0.5-rc1")


class ClientVersionCheck(unittest.TestCase):
    """Was der Client mit dem Feed anfängt (installiert = Version::versionWithBuildNumber(), immer vierstellig)."""

    def windows_info(self, **overrides):
        return parse_like_client(build(**overrides)["owncloud.online-update-win-x64-stable.xml"])

    def test_qversionnumber_semantics(self):
        self.assertLess(qversion("1.2"), qversion("1.2.0"))  # Beispiel aus der Qt-Doku
        self.assertEqual(qversion("7.11.1-rc1"), (7, 11, 1))
        self.assertEqual(qversion(""), ())

    def test_older_client_downloads_the_setup(self):
        self.assertEqual(windows_updater(self.windows_info(), installed="7.11.0.0"), "download")

    def test_same_version_counts_as_up_to_date(self):
        self.assertEqual(windows_updater(self.windows_info(), installed="7.11.1.0"), "up-to-date")
        self.assertEqual(windows_updater(self.windows_info(), installed="7.11.1.4711"), "up-to-date")

    def test_skipped_version_is_not_offered_again(self):
        self.assertEqual(windows_updater(self.windows_info(), installed="7.11.0.0", skipped="7.11.1"), "up-to-date")

    def test_newer_client_never_downgrades(self):
        self.assertEqual(windows_updater(self.windows_info(), installed="11.0.0.0"), "up-to-date")

    def test_jump_to_the_11_line_is_offered(self):
        setup = RELEASES + "/download/v11.0.0/owncloud.online-client-11.0.0-win-x64-Setup.exe"
        info = self.windows_info(tag="v11.0.0", web=RELEASES + "/tag/v11.0.0", platforms={"win-x64": setup})
        self.assertEqual(windows_updater(info, installed="7.11.1.0"), "download")

    def test_successful_update_is_recognised_after_restart(self):
        self.assertTrue(update_succeeded(installed="7.11.1.0", target=self.windows_info()["version"]))

    def test_build_number_above_the_real_build_reports_a_failed_update(self):
        # deshalb --version nur mit der Build-Nummer, die der Bau wirklich trägt
        self.assertFalse(update_succeeded(installed="7.11.1.0", target="7.11.1.5"))

    def test_platforms_without_installer_only_notify(self):
        info = parse_like_client(build()["owncloud.online-update-macos-arm64-stable.xml"])
        self.assertEqual(passive_notifier(info, installed="7.11.0.0"), "through-system")
        self.assertEqual(passive_notifier(info, installed="7.11.1.0"), "up-to-date")

    def test_prerelease_sees_the_final_release(self):
        # Die Baunummer ist heute immer 0, der Suffix der Installation entscheidet
        final = self.windows_info(tag="v11.0.0", web=RELEASES + "/tag/v11.0.0", platforms={
            "win-x64": RELEASES + "/download/v11.0.0/owncloud.online-client-11.0.0-win-x64-Setup.exe"})
        self.assertEqual(windows_updater(final, installed="11.0.0.0-beta1"), "download")
        self.assertEqual(windows_updater(final, installed="11.0.0.0-rc1"), "download")
        self.assertEqual(windows_updater(final, installed="11.0.0.0"), "up-to-date")
        self.assertFalse(update_succeeded(installed="11.0.0.0-beta1", target=final["version"]))
        self.assertTrue(update_succeeded(installed="11.0.0.0", target=final["version"]))

    def test_prerelease_feed_never_offers_itself(self):
        info = parse_like_client(build(tag="v11.0.0-rc1", web=RC_WEB, platforms={"win-x64": RC_SETUP})[
            "owncloud.online-update-win-x64-beta.xml"])
        self.assertEqual(info["version"], "11.0.0-rc1")
        self.assertEqual(windows_updater(info, installed="11.0.0.0-rc1"), "up-to-date")
        self.assertEqual(windows_updater(info, installed="11.0.0.0-beta1"), "download")
        self.assertEqual(windows_updater(info, installed="11.0.0.0"), "up-to-date")

    def test_prerelease_order(self):
        for older, newer in (("11.0.0-beta2", "11.0.0-beta10"), ("11.0.0-beta2", "11.0.0-rc1"),
                             ("11.0.0-rc1", "11.0.0"), ("11.0.0", "11.0.1-beta1")):
            self.assertLess(compare_versions(older, newer), 0, (older, newer))
            self.assertGreater(compare_versions(newer, older), 0, (older, newer))
        self.assertEqual(compare_versions("11.0.0-RC1", "11.0.0-rc01"), 0)


class InputValidation(unittest.TestCase):

    def assertRejected(self, pattern, **overrides):
        with self.assertRaisesRegex(feed.FeedError, pattern):
            build(**overrides)

    def test_plain_http_is_rejected(self):
        self.assertRejected("https", web="http://github.com/BWTECH-github/client/releases/tag/v7.11.1")
        self.assertRejected("https", platforms={"win-x64": SETUP.replace("https:", "http:")})

    def test_query_or_fragment_in_download_is_rejected(self):
        self.assertRejected("Query|Fragment", platforms={"win-x64": SETUP + "?x=1"})
        self.assertRejected("Query|Fragment", platforms={"win-x64": SETUP + "#x"})

    def test_download_name_must_carry_the_version(self):
        self.assertRejected("Version", platforms={"win-x64": RELEASES + "/download/v7.11.1/owncloud.online-Setup.exe"})
        self.assertRejected("Version", platforms={"win-x64": SETUP.replace("7.11.1-win", "7.11.10-win")})

    def test_unknown_platform_is_rejected(self):
        self.assertRejected("Plattform", platforms={"win-arm64": None})

    def test_platform_specific_download_types(self):
        self.assertRejected("macos-arm64", platforms={
            "macos-arm64": RELEASES + "/download/v7.11.1/owncloud.online-client-7.11.1-macos-arm64.dmg"})
        self.assertRejected(r"\.exe", platforms={
            "win-x64": RELEASES + "/download/v7.11.1/owncloud.online-client-7.11.1-win-x64.7z"})
        self.assertRejected(r"\.zsync", platforms={
            "linux-x86_64": RELEASES + "/download/v7.11.1/owncloud.online-client-7.11.1-linux-x86_64.AppImage"})

    def test_version_must_match_the_tag(self):
        self.assertRejected("Tag", version="7.11.2")
        self.assertRejected("Version", version="7.11.1.4.2")
        self.assertRejected("Version", version="7.11")

    def test_stable_channel_needs_a_tag_without_suffix(self):
        self.assertRejected("stable", tag="v11.0.0-rc1", web=RC_WEB, platforms={"win-x64": RC_SETUP}, channels=["stable"])

    def test_unknown_suffix_needs_an_explicit_channel(self):
        setup = RELEASES + "/download/v7.2.0-perf-test/owncloud.online-client-7.2.0-perf-test-win-x64-Setup.exe"
        arguments = dict(tag="v7.2.0-perf-test", web=RELEASES + "/tag/v7.2.0-perf-test", platforms={"win-x64": setup})
        self.assertRejected("Kanal", **arguments)
        self.assertEqual(list(build(channels=["beta"], **arguments)), ["owncloud.online-update-win-x64-beta.xml"])

    def test_at_least_one_platform_is_required(self):
        self.assertRejected("Plattform", platforms={})

    def test_characters_that_break_out_of_the_client_html_are_rejected(self):
        self.assertRejected("Zeichen", web=WEB + "'onmouseover")
        self.assertRejected("Zeichen", version_string="owncloud.online <b>7.11.1</b>")

    def test_malformed_tag_is_rejected(self):
        self.assertRejected("Tag", tag="7.11")
        self.assertRejected("Tag", tag="v7.11.1 ")

    def test_segments_beyond_qversionnumber_range_are_rejected(self):
        self.assertRejected("groß", version="7.11.1.4294967296")


class CommandLine(unittest.TestCase):

    def run_main(self, *arguments):
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            code = feed.main(list(arguments))
        return code, stdout.getvalue(), stderr.getvalue()

    def test_writes_one_lf_file_per_platform_and_channel(self):
        with tempfile.TemporaryDirectory() as out:
            code, stdout, _ = self.run_main(
                "--tag", "v7.11.1", "--web", WEB, "--out", out,
                "--platform", "win-x64=" + SETUP, "--platform", "linux-x86_64", "--platform", "macos-arm64")
            files = sorted(Path(out).iterdir())
            self.assertEqual(code, 0)
            self.assertEqual(len(files), 6)
            for path in files:
                data = path.read_bytes()
                self.assertNotIn(b"\r\n", data)
                self.assertIsNotNone(parse_like_client(data), path.name)
                self.assertIn(path.name, stdout)

    def test_invalid_input_exits_with_code_2_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as out:
            code, _, stderr = self.run_main(
                "--tag", "v7.11.1", "--web", WEB.replace("https:", "http:"), "--out", out,
                "--platform", "win-x64=" + SETUP)
            self.assertEqual(code, 2)
            self.assertIn("Fehler", stderr)
            self.assertEqual(list(Path(out).iterdir()), [])

    def test_platform_given_twice_is_rejected(self):
        with tempfile.TemporaryDirectory() as out:
            code, _, stderr = self.run_main(
                "--tag", "v7.11.1", "--web", WEB, "--out", out,
                "--platform", "win-x64=" + SETUP, "--platform", "win-x64")
            self.assertEqual(code, 2)
            self.assertIn("win-x64", stderr)

    def test_empty_download_after_equals_sign_is_rejected(self):
        with tempfile.TemporaryDirectory() as out:
            code, _, stderr = self.run_main(
                "--tag", "v7.11.1", "--web", WEB, "--out", out, "--platform", "linux-x86_64=")
            self.assertEqual(code, 2)
            self.assertIn("linux-x86_64", stderr)


class FixedLocation(unittest.TestCase):
    """Fester Ablageort (Release "update-feed") und Schutz vor dem Zurücksetzen."""

    def read_source(self, relative):
        path = REPO / relative
        if not path.is_file():
            self.skipTest(relative + " fehlt (Generator außerhalb des Repositorys?)")
        return path.read_text(encoding="utf-8")

    def write_current(self, directory, tag, **overrides):
        arguments = {"tag": tag, "web": RELEASES + "/tag/" + tag, "platforms": {"macos-arm64": None}}
        arguments.update(overrides)
        for name, text in feed.build_feeds(**arguments).items():
            (Path(directory) / name).write_text(text, encoding="utf-8", newline="\n")

    def test_client_default_points_to_the_fixed_release(self):
        cmake = self.read_source("OWNCLOUD.cmake")
        self.assertIn('APPLICATION_UPDATE_URL "%s"' % feed.FEED_BASE_URL, cmake)
        self.assertNotIn("releases/latest", cmake)
        self.assertTrue(feed.FEED_BASE_URL.endswith("/releases/download/%s/" % feed.FEED_RELEASE_TAG))

    def test_workflow_uploads_to_the_fixed_release_as_prerelease(self):
        workflow = self.read_source(".github/workflows/update-feed.yml")
        self.assertIn("FEED_TAG: " + feed.FEED_RELEASE_TAG, workflow)
        self.assertIn("--keep-newer", workflow)
        self.assertIn("--prerelease", workflow)
        self.assertIn("--latest=false", workflow)
        publish = self.read_source(".github/workflows/publish-release.yml")
        self.assertIn("uses: ./.github/workflows/update-feed.yml", publish)

    def test_version_order_matches_the_client(self):
        versions = ["7.11.1", "7.11.1.4711", "11.0.0.0-beta1", "11.0.0-beta1", "11.0.0-beta2",
                    "11.0.0-beta10", "11.0.0-RC1", "11.0.0-rc01", "11.0.0-rc2", "11.0.0", "11.0.0.5",
                    "11.0.1-beta1", "11.0.1", "12.0"]
        for a in versions:
            for b in versions:
                self.assertEqual(feed.compare_versions(a, b), compare_versions(a, b), (a, b))

    def test_feed_version_is_read_back(self):
        text = build()["owncloud.online-update-win-x64-stable.xml"]
        self.assertEqual(feed.read_feed_version(text.encode("utf-8")), "7.11.1")
        self.assertIsNone(feed.read_feed_version(b"<html>404</html>"))
        self.assertIsNone(feed.read_feed_version(b"Not Found"))
        self.assertIsNone(feed.read_feed_version(b"<owncloudclient><version></version></owncloudclient>"))

    def test_rerun_for_an_older_release_keeps_the_newer_feed(self):
        with tempfile.TemporaryDirectory() as current:
            self.write_current(current, "v11.0.1")
            feeds = feed.build_feeds(tag="v11.0.0", web=RELEASES + "/tag/v11.0.0", platforms={"macos-arm64": None})
            kept, skipped = feed.drop_older(feeds, current)
            self.assertEqual(kept, {})
            self.assertEqual(sorted(name for name, _, _ in skipped), sorted(feeds))

    def test_same_or_newer_version_replaces_the_feed(self):
        with tempfile.TemporaryDirectory() as current:
            self.write_current(current, "v11.0.0")
            for tag in ("v11.0.0", "v11.0.1"):
                feeds = feed.build_feeds(tag=tag, web=RELEASES + "/tag/" + tag, platforms={"macos-arm64": None})
                self.assertEqual(feed.drop_older(feeds, current), (feeds, []))

    def test_beta_channel_after_the_final_release(self):
        with tempfile.TemporaryDirectory() as current:
            self.write_current(current, "v11.0.0")
            late_beta = feed.build_feeds(tag="v11.0.0-beta2", web=RELEASES + "/tag/v11.0.0-beta2",
                                         platforms={"macos-arm64": None})
            self.assertEqual(feed.drop_older(late_beta, current)[0], {})
            next_beta = feed.build_feeds(tag="v11.0.1-beta1", web=RELEASES + "/tag/v11.0.1-beta1",
                                         platforms={"macos-arm64": None})
            self.assertEqual(list(feed.drop_older(next_beta, current)[0]),
                             ["owncloud.online-update-macos-arm64-beta.xml"])

    def test_missing_or_broken_current_file_is_replaced(self):
        with tempfile.TemporaryDirectory() as current:
            (Path(current) / "owncloud.online-update-macos-arm64-stable.xml").write_bytes(b"kaputt")
            feeds = feed.build_feeds(tag="v7.11.1", web=WEB, platforms={"macos-arm64": None})
            self.assertEqual(feed.drop_older(feeds, current), (feeds, []))

    def test_command_line_skips_older_files(self):
        with tempfile.TemporaryDirectory() as current, tempfile.TemporaryDirectory() as out:
            self.write_current(current, "v11.0.0", channels=["beta"])
            stdout, stderr = io.StringIO(), io.StringIO()
            with redirect_stdout(stdout), redirect_stderr(stderr):
                code = feed.main(["--tag", "v7.11.1", "--web", WEB, "--out", out, "--platform", "macos-arm64",
                                  "--keep-newer", current])
            self.assertEqual(code, 0)
            self.assertEqual([path.name for path in Path(out).iterdir()],
                             ["owncloud.online-update-macos-arm64-stable.xml"])
            self.assertIn("übersprungen", stderr.getvalue())

    def test_command_line_rejects_a_missing_current_directory(self):
        with tempfile.TemporaryDirectory() as out:
            stderr = io.StringIO()
            with redirect_stdout(io.StringIO()), redirect_stderr(stderr):
                code = feed.main(["--tag", "v7.11.1", "--web", WEB, "--out", out, "--platform", "macos-arm64",
                                  "--keep-newer", str(Path(out) / "fehlt")])
            self.assertEqual(code, 2)
            self.assertEqual(list(Path(out).iterdir()), [])


if __name__ == "__main__":
    unittest.main()
