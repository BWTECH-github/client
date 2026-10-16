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

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

namespace OCC {

/**
 * owncloud.online: Umstieg vom offiziellen ownCloud-Desktop-Client (2.x bis 6.x).
 *
 * Liest dessen Konfiguration (owncloud.cfg) nur: Konten (Server, Benutzer) und Sync-Ordner. Am Fremd-Client
 * wird nichts verändert – die Konfiguration wird über eine Kopie gelesen, sein Journal nur kopiert.
 */
namespace LegacyClient {

    struct OWNCLOUDGUI_EXPORT Folder
    {
        /** Lokaler Ordner mit abschließendem / */
        QString localPath;
        /** Serverordner, „/“ für die Wurzel */
        QString targetPath = QStringLiteral("/");
        /** Journal des Fremd-Clients, relativ zum lokalen Ordner */
        QString journalPath;
        /** WebDAV-Adresse (ab 3.0); leer bei 2.x */
        QUrl davUrl;
        /** Space (nur Infinite Scale) */
        QString spaceId;
        QString displayName;
        /** „off“, „wincfapi“, „suffix“ … */
        QString virtualFilesMode;
        bool paused = false;

        bool usesVirtualFiles() const;
        /** Pfad des Journals im lokalen Ordner (Standard des Fremd-Clients, falls nichts eingetragen ist) */
        QString absoluteJournalPath() const;
    };

    struct OWNCLOUDGUI_EXPORT Account
    {
        QUrl url;
        QString davUser;
        QString displayName;
        QList<Folder> folders;

        /** „Benutzer auf Server“ für Dialoge */
        QString displayString() const;
    };

    struct OWNCLOUDGUI_EXPORT Config
    {
        /** gelesene Datei */
        QString path;
        /** Version des Fremd-Clients, soweit eingetragen */
        QString clientVersion;
        QList<Account> accounts;
    };

    /** Orte der Konfiguration des offiziellen Clients auf dieser Plattform, der neuere zuerst. */
    OWNCLOUDGUI_EXPORT QStringList candidateConfigFiles();

    /** Liest die Konfiguration, ohne die Datei zu verändern; leer, wenn sie fehlt oder unlesbar ist. */
    OWNCLOUDGUI_EXPORT std::optional<Config> readConfig(const QString &path);

    /** Die erste vorhandene Konfiguration mit mindestens einem Konto. */
    OWNCLOUDGUI_EXPORT std::optional<Config> findConfig(const QStringList &candidates = candidateConfigFiles());

    /** Prozessname des offiziellen Clients (owncloud bzw. owncloud.exe)? Groß-/Kleinschreibung egal. */
    OWNCLOUDGUI_EXPORT bool isOfficialClientProcessName(const QString &processName);

    /** Läuft der offizielle Client gerade? Dann synchronisierten zwei Programme dieselben Ordner. */
    OWNCLOUDGUI_EXPORT bool isOfficialClientRunning();

    enum class JournalCopy {
        /** Journal kopiert und geprüft */
        Copied,
        /** der Fremd-Client hat (noch) kein Journal */
        NoJournal,
        /** von einer neueren Client-Version geschrieben */
        TooNew,
        Failed,
    };

    /**
     * Kopiert das Journal des Fremd-Clients samt Write-Ahead-Log bzw. Rollback-Journal nach target und prüft die
     * Kopie. Das Original wird nur gelesen. Bei Misserfolg bleibt am Ziel nichts zurück.
     */
    OWNCLOUDGUI_EXPORT JournalCopy copyJournal(const QString &source, const QString &target, QString *error = nullptr);

} // namespace LegacyClient
} // namespace OCC
