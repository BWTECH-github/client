<!-- Modified by BW-Tech GmbH for owncloud.online desktop client branding. -->

# owncloud.online Desktop Client

Hält Dateien zwischen einem owncloud.online-Server und dem eigenen Rechner
abgeglichen — für Windows, macOS und Linux.

## Was er kann

* **Abgleich in beide Richtungen** für ausgewählte Ordner
* **Selektiver Abgleich** — nur die Ordner holen, die man wirklich braucht
* **Virtuelle Dateien** unter Windows: Der Explorer zeigt alle Dateien, geladen
  wird erst beim Öffnen
* **Große Dateien** werden in Stücken übertragen und lassen sich nach einem
  Abbruch fortsetzen
* **Viele kleine Dateien** gehen gebündelt in einer Anfrage über die Leitung
* **owncloudcmd** für den Abgleich ohne Oberfläche, etwa auf Servern

## Herunterladen

Fertige Pakete: <https://owncloud.online>

## Aus dem Quelltext bauen

Das Projekt nutzt CMake, dazu KDE Craft als übergeordnetes Bausystem.

* CMake: <https://cmake.org/documentation/>
* KDE Craft: <https://community.kde.org/Craft>

Unter Windows werden die Visual-Studio-Bauwerkzeuge (`vcvars`) und Ninja
benötigt. Die Schritte im Einzelnen stehen in der
[Dokumentation](https://docs.owncloud.online/).

## Fehler melden und mitarbeiten

Fehler bitte als
[Issue](https://github.com/BWTECH-github/client/issues) melden, nicht per Mail
an einzelne Personen — sonst geht die Meldung verloren.

Hilfreich ist:

* Version des Clients und des Servers
* Betriebssystem
* Auszug aus dem Abgleich-Protokoll (im Client unter *Allgemein →
  Protokolleinstellungen*), vorher auf Pfade und Namen durchgesehen

Änderungen als [Pull Request](https://github.com/BWTECH-github/client/pulls).

**Sicherheitslücken nicht als Issue melden**, sondern vertraulich an
**security@bw.tech**.

## Herkunft

Dieser Client ist ein Fork des ownCloud Desktop Clients und wird von der
BW-Tech GmbH für owncloud.online gepflegt. Der Dank für die ursprüngliche
Arbeit gehört der ownCloud-Gemeinschaft und den bisherigen Betreuern:

- Markus Goetz
- Olivier Goffart
- Christian Kamm
- Thomas Müller
- Klaas Freitag
- Daniel Molkentin
- Andreas Schneider

## Lizenz

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
