#!/usr/bin/env python3
"""Erzeugt die statischen Update-Feeds des owncloud.online Desktop-Clients.

Je Plattform und Kanal eine XML-Datei in dem Format, das der Client liest
(src/gui/updater/updateinfo.cpp):

    <?xml version="1.0" encoding="UTF-8"?>
    <owncloudclient>
      <version>7.11.1</version>
      <versionstring>owncloud.online 7.11.1</versionstring>
      <web>https://github.com/BWTECH-github/client/releases/tag/v7.11.1</web>
      <downloadurl>https://github.com/…/owncloud.online-client-7.11.1-win-x64-Setup.exe</downloadurl>
    </owncloudclient>

Beispiel:

    python3 admin/release/make-update-feed.py --tag v7.11.1 \\
        --web https://github.com/BWTECH-github/client/releases/tag/v7.11.1 \\
        --platform win-x64=https://github.com/BWTECH-github/client/releases/download/v7.11.1/owncloud.online-client-7.11.1-win-x64-Setup.exe \\
        --platform linux-x86_64 --platform macos-arm64 --out feed

Format, Adressen, Kanäle und was der Client dafür braucht: admin/release/UPDATE-FEED.md.
Nur Standardbibliothek, Python 3.8 oder neuer; Selbsttest: test_make_update_feed.py.
"""

import argparse
import re
import sys
from pathlib import Path
from urllib.parse import urlsplit
from xml.sax.saxutils import escape

FEED_PREFIX = "owncloud.online-update"
ROOT_ELEMENT = "owncloudclient"
FIELDS = ("version", "versionstring", "web", "downloadurl")
CHANNELS = ("stable", "beta")

# Wie updateChannel() in src/gui/updater/updater.cpp: ein Suffix mit diesem
# Anfang macht den Bau zum Beta-Bau, ohne Suffix ist er stable.
BETA_SUFFIX_PREFIXES = ("daily", "nightly", "alpha", "rc", "beta")

# Plattform -> geforderte Endung der Download-Adresse, None = keine erlaubt.
# Windows lädt das Setup herunter und startet es (ocupdater.cpp), der
# AppImage-Updater erwartet eine zsync-Datei (appimageupdater.cpp), macOS meldet
# nur (PassiveUpdateNotifier; Sparkle braucht einen eigenen Appcast).
PLATFORMS = {
    "win-x64": ".exe",
    "linux-x86_64": ".zsync",
    "macos-arm64": None,
}

QT_INT_MAX = 2**31 - 1  # QVersionNumber speichert int-Segmente

TAG_RE = re.compile(r"v?([0-9]+)\.([0-9]+)\.([0-9]+)(?:-([0-9A-Za-z][0-9A-Za-z.-]*))?")
VERSION_RE = re.compile(r"[0-9]+(?:\.[0-9]+){2,3}")
FILE_NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+-]*")
# Der Client setzt <web> ungeschützt in <a href='…'> und <versionstring> teils
# ungeschützt in Rich-Text (ocupdater.cpp:141-160); Steuerzeichen und
# Leerraum gehören in keine der Angaben.
UNSAFE_CHARS_RE = re.compile(r"[\x00-\x20\x7f'\"<>\\`]")
UNSAFE_TEXT_RE = re.compile(r"[\x00-\x1f\x7f'\"<>&]")


class FeedError(ValueError):
    """Ungültige Eingabe; der Text nennt den Grund."""


def feed_file_name(platform, channel):
    return f"{FEED_PREFIX}-{platform}-{channel}.xml"


def _check_segments(segments, text):
    if any(segment > QT_INT_MAX for segment in segments):
        raise FeedError(f"{text}: Zahl zu groß für QVersionNumber (höchstens {QT_INT_MAX})")


def parse_tag(tag):
    """v7.11.1 -> ((7, 11, 1), "", "7.11.1"); v11.0.0-rc1 -> ((11, 0, 0), "rc1", "11.0.0-rc1")."""
    match = TAG_RE.fullmatch(tag)
    if not match:
        raise FeedError(f"Tag {tag!r} hat nicht die Form v<Major>.<Minor>.<Patch>[-<Suffix>]")
    numbers = tuple(int(part) for part in match.groups()[:3])
    _check_segments(numbers, tag)
    return numbers, match.group(4) or "", tag[1:] if tag.startswith("v") else tag


def default_channels(suffix):
    if not suffix:
        # Beta-Installationen bekommen auch jedes stabile Release angeboten.
        return ["stable", "beta"]
    if suffix.startswith(BETA_SUFFIX_PREFIXES):
        return ["beta"]
    raise FeedError(
        f"Suffix {suffix!r} gehört nach updateChannel() zu keinem Vorab-Kanal; "
        "Kanal ausdrücklich angeben (--channel beta)")


def resolve_channels(channels, suffix):
    if channels is None:
        return default_channels(suffix)
    unknown = [channel for channel in channels if channel not in CHANNELS]
    if unknown or not channels:
        raise FeedError(f"Kanal muss einer von {', '.join(CHANNELS)} sein, nicht {unknown or channels!r}")
    if "stable" in channels and suffix:
        raise FeedError(f"Kanal stable nur für Tags ohne Suffix (hier {suffix!r})")
    return [channel for channel in CHANNELS if channel in channels]


def resolve_version(version, numbers, suffix, tag):
    """Zahlen aus dem Tag (oder --version) plus dessen Vorab-Suffix: v11.0.0-rc1 -> 11.0.0-rc1.

    Der Client hält eine Vorabversion für älter als das Release gleicher Nummer
    (OCUpdater::compareVersions); ohne Suffix sähe eine Vorabversion ihren eigenen
    Feed als finales Release und böte sich immer wieder selbst an.
    """
    if version is None:
        segments = numbers
    else:
        if not VERSION_RE.fullmatch(version):
            raise FeedError(f"Version {version!r}: drei oder vier Zahlen mit Punkten erwartet")
        segments = tuple(int(part) for part in version.split("."))
        _check_segments(segments, version)
        if segments[:3] != numbers:
            raise FeedError(f"Version {version} passt nicht zum Tag {tag}")
    text = ".".join(str(segment) for segment in segments)
    return f"{text}-{suffix}" if suffix else text


def check_url(url, what):
    if UNSAFE_CHARS_RE.search(url):
        raise FeedError(f"{what}: Adresse enthält unzulässige Zeichen ({url!r})")
    parts = urlsplit(url)
    if parts.scheme != "https" or not parts.netloc:
        raise FeedError(f"{what}: nur https-Adressen ({url!r})")
    return parts


def check_download(platform, url, display_version):
    required_suffix = PLATFORMS[platform]
    if required_suffix is None:
        raise FeedError(f"{platform}: der Client lädt dort nichts herunter, keine Download-Adresse angeben")
    parts = check_url(url, f"Download {platform}")
    if parts.query or parts.fragment or "?" in url or "#" in url:
        # Der Client nimmt alles nach dem letzten '/' als Dateinamen.
        raise FeedError(f"Download {platform}: Adresse ohne Query und Fragment angeben ({url!r})")
    name = url[url.rfind("/") + 1:]
    if not FILE_NAME_RE.fullmatch(name):
        raise FeedError(f"Download {platform}: Dateiname {name!r} unzulässig")
    if not name.endswith(required_suffix):
        raise FeedError(f"Download {platform}: Datei muss auf {required_suffix} enden ({name!r})")
    # Der Client hält eine Datei gleichen Namens für den fertigen Download
    # (ocupdater.cpp:354-356), daher muss jede Version ihren eigenen Namen haben.
    if not re.search(r"(?<![0-9A-Za-z.])" + re.escape(display_version) + r"(?![0-9A-Za-z])", name):
        raise FeedError(f"Download {platform}: Dateiname {name!r} enthält die Version {display_version} nicht")


def render(fields):
    lines = ['<?xml version="1.0" encoding="UTF-8"?>', f"<{ROOT_ELEMENT}>"]
    for name in FIELDS:
        if fields.get(name):
            lines.append(f"  <{name}>{escape(fields[name])}</{name}>")
    lines.append(f"</{ROOT_ELEMENT}>")
    return "\n".join(lines) + "\n"


def build_feeds(tag, web, platforms, channels=None, version=None, version_string=None):
    """Gibt {Dateiname: XML} zurück; platforms bildet Plattform -> Download-Adresse oder None ab."""
    numbers, suffix, display_version = parse_tag(tag)
    if not platforms:
        raise FeedError("keine Plattform angegeben")
    unknown = sorted(set(platforms) - set(PLATFORMS))
    if unknown:
        raise FeedError(f"unbekannte Plattform {', '.join(unknown)} (bekannt: {', '.join(PLATFORMS)})")
    check_url(web, "Webseite")
    for platform, url in platforms.items():
        if url is not None:
            check_download(platform, url, display_version)
    channels = resolve_channels(channels, suffix)
    version = resolve_version(version, numbers, suffix, tag)
    if version_string is None:
        version_string = f"owncloud.online {display_version}"
    if not version_string or UNSAFE_TEXT_RE.search(version_string):
        raise FeedError(f"Anzeigetext {version_string!r} leer oder mit unzulässigen Zeichen")

    feeds = {}
    for channel in channels:
        for platform in PLATFORMS:
            if platform in platforms:
                feeds[feed_file_name(platform, channel)] = render({
                    "version": version,
                    "versionstring": version_string,
                    "web": web,
                    "downloadurl": platforms[platform],
                })
    return feeds


def parse_platform_arguments(values):
    platforms = {}
    for value in values:
        platform, separator, url = value.partition("=")
        if platform in platforms:
            raise FeedError(f"Plattform {platform} doppelt angegeben")
        if separator and not url:
            raise FeedError(f"Plattform {platform}: leere Download-Adresse nach '='")
        platforms[platform] = url if separator else None
    return platforms


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Erzeugt die Update-Feeds (eine XML-Datei je Plattform und Kanal).",
        epilog="Details: admin/release/UPDATE-FEED.md")
    parser.add_argument("--tag", required=True, help="Release-Tag, z. B. v7.11.1 oder v11.0.0-rc1")
    parser.add_argument("--version", help="Zahlen für <version> (drei oder vier, der Suffix des Tags kommt dazu); Vorgabe: aus dem Tag")
    parser.add_argument("--channel", action="append", choices=CHANNELS,
                        help="Kanal, mehrfach möglich; Vorgabe: ohne Suffix stable und beta, mit Vorab-Suffix beta")
    parser.add_argument("--web", required=True, help="Webseite des Releases (https)")
    parser.add_argument("--platform", action="append", required=True, metavar="PLATTFORM[=URL]",
                        help=f"Plattform ({', '.join(PLATFORMS)}), wahlweise mit Download-Adresse; mehrfach")
    parser.add_argument("--version-string", help="Anzeigetext; Vorgabe: 'owncloud.online <Version aus dem Tag>'")
    parser.add_argument("--out", required=True, type=Path, help="Zielverzeichnis")
    args = parser.parse_args(argv)

    try:
        feeds = build_feeds(
            tag=args.tag,
            web=args.web,
            platforms=parse_platform_arguments(args.platform),
            channels=args.channel,
            version=args.version,
            version_string=args.version_string,
        )
    except FeedError as error:
        print(f"Fehler: {error}", file=sys.stderr)
        return 2

    args.out.mkdir(parents=True, exist_ok=True)
    for name, text in feeds.items():
        path = args.out / name
        path.write_bytes(text.encode("utf-8"))  # immer LF, auch unter Windows
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
