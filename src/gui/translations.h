/*
 * Copyright (C) by Fabian Müller <fmueller@owncloud.com>
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

#include <QSet>
#include <QString>
#include <QStringList>

class QTranslator;

namespace OCC {

namespace Translations {

    /**
    * @return translation files' filename prefix
    */
    OWNCLOUDGUI_EXPORT const QString translationsFilePrefix();

    /**
    * @returntranslation files' filename suffix
    */
    OWNCLOUDGUI_EXPORT const QString translationsFileSuffix();

    /**
     * @return path to translation files
     */
    OWNCLOUDGUI_EXPORT QString translationsDirectoryPath();

    /**
     * @return list of locales for which translations are available
     */
    OWNCLOUDGUI_EXPORT QSet<QString> listAvailableTranslations();

    /**
     * Verzeichnisse, in denen die Qt-eigenen Kataloge (qt_*.qm, qtbase_*.qm, qtkeychain_*.qm) gesucht werden.
     *
     * QLibraryInfo allein reicht in den Paketen nicht: liegt Qt6Core.dll ohne Import-Bibliothek neben der exe, hält Qt
     * das Programm für windeployqt-verteilt und meldet <Programmordner>/bin/translations, der Installer legt die Kataloge
     * aber unter <Programmordner>/translations ab. Dazu kommen die Orte im AppImage und im macOS-Bundle.
     *
     * @param qtTranslationsPath QLibraryInfo::path(QLibraryInfo::TranslationsPath)
     * @param applicationDirPath QCoreApplication::applicationDirPath()
     * @return die Verzeichnisse in Suchreihenfolge, ohne Duplikate
     */
    OWNCLOUDGUI_EXPORT QStringList qtTranslationsSearchPaths(const QString &qtTranslationsPath, const QString &applicationDirPath);

    /**
     * Lädt für @p language den ersten Katalog aus @p catalogPrefixes (z. B. "qt_", "qtbase_"), der in einem der
     * Verzeichnisse liegt und sich laden lässt. Varianten der Client-Übersetzung wie "de-informal" werden auf die
     * Sprache ("de") abgebildet, Qt liefert nur diese; "zh-Hans" nutzt den Katalog zh_CN, "zh_HK" den Katalog zh_TW.
     *
     * @return true, wenn ein Katalog geladen wurde
     */
    OWNCLOUDGUI_EXPORT bool loadQtTranslation(
        QTranslator *translator, const QStringList &catalogPrefixes, const QString &language, const QStringList &searchPaths);

} // namespace Translations

} // namespace OCC
