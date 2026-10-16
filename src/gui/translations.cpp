/*
 * Copyright (C) by Duncan Mac-Vicar P. <duncan@kde.org>
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 * Copyright (C) by Daniel Molkentin <danimo@owncloud.com>
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

#include "translations.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QTranslator>

namespace OCC {

namespace Translations {

    Q_LOGGING_CATEGORY(lcTranslations, "gui.translations", QtInfoMsg)

    const QString translationsFilePrefix()
    {
        return QStringLiteral("client_");
    }

    const QString translationsFileSuffix()
    {
        return QStringLiteral(".qm");
    }

    QString translationsDirectoryPath()
    {
        return QStringLiteral(":/client/translations/");
    }

    QSet<QString> listAvailableTranslations()
    {
        QSet<QString> availableTranslations;

        // calculate a glob pattern which can be used in the iterator below to match only translations files
        const QString pattern = translationsFilePrefix() + QLatin1Char('*') + translationsFileSuffix();

        QDirIterator it(Translations::translationsDirectoryPath(), QStringList() << pattern);

        while (it.hasNext()) {
            // extract locale part from filename
            // of course, this method relies on the filenames to be accurate
            const auto fileName = QFileInfo(it.next()).fileName();
            const auto localePartLength = fileName.length() - translationsFileSuffix().length() - translationsFilePrefix().length();
            QString localeName = fileName.mid(translationsFilePrefix().length(), localePartLength);

            availableTranslations.insert(localeName);
        }

        return availableTranslations;
    }

    QStringList qtTranslationsSearchPaths(const QString &qtTranslationsPath, const QString &applicationDirPath)
    {
        QStringList candidates = {qtTranslationsPath};
        if (!applicationDirPath.isEmpty()) {
            candidates += {
                // windeployqt: neben der exe
                applicationDirPath + QStringLiteral("/translations"),
                // Windows-Installer (<Programmordner>/bin/..) und AppImage (usr/bin/..)
                applicationDirPath + QStringLiteral("/../translations"),
                // macOS-Bundle: Contents/MacOS/../Resources
                applicationDirPath + QStringLiteral("/../Resources/translations"),
                // Linux-Pakete nach FHS
                applicationDirPath + QStringLiteral("/../share/qt6/translations"),
            };
        }

        QStringList paths;
        for (const auto &candidate : std::as_const(candidates)) {
            if (candidate.isEmpty()) {
                continue;
            }
            const QString path = QDir::cleanPath(candidate);
            if (!paths.contains(path)) {
                paths.append(path);
            }
        }
        return paths;
    }

    bool loadQtTranslation(QTranslator *translator, const QStringList &catalogPrefixes, const QString &language, const QStringList &searchPaths)
    {
        // "de-informal" -> "de"; Gebietsangaben wie in "de_DE" kürzt QTranslator::load() selbst
        const QString qtLanguage = language.section(QLatin1Char('-'), 0, 0);
        for (const auto &dir : searchPaths) {
            for (const auto &prefix : catalogPrefixes) {
                if (translator->load(prefix + qtLanguage, dir)) {
                    qCInfo(lcTranslations) << "Loaded Qt translation" << prefix + qtLanguage << "from" << dir;
                    return true;
                }
            }
        }
        return false;
    }

} // namespace Translations

} // namespace OCC
