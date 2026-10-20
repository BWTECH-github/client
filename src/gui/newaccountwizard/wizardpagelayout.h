/*
 * Copyright (C) 2026 by BW-Tech GmbH (owncloud.online)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#pragma once

#include "gui/owncloudguilib.h"

class QLayout;
class QScrollArea;
class QWidget;
class QWizardPage;

namespace OCC {

/**
 * Setzt den Inhalt einer Seite des Kontoassistenten in einen Bildlaufbereich. Der Assistent hat eine feste Größe; reicht
 * sie nicht (lange Meldung, längere Übersetzung, größere Schrift), rollt der Inhalt, statt unter der Knopfzeile
 * abgeschnitten zu werden. Solange alles passt, sieht die Seite aus wie mit einem gewöhnlichen Layout.
 */
OWNCLOUDGUI_EXPORT void setScrollableWizardPageLayout(QWizardPage *page, QLayout *contentLayout);

/**
 * Der Bildlaufbereich von setScrollableWizardPageLayout() allein, für Seiten mit eigener Knopfzeile (die Anmeldedialoge
 * der Kontoansicht): Der Aufrufer setzt ihn über die Knöpfe in sein Layout. Gerollt wird nur senkrecht; nichts im Inhalt
 * wird gestaucht, auch das Logo nicht.
 */
OWNCLOUDGUI_EXPORT QScrollArea *createScrollableContent(QWidget *parent, QLayout *contentLayout);

}
