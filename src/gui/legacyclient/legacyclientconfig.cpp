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

#include "legacyclient/legacyclientconfig.h"

#include "common/ownsql.h"
#include "common/syncjournaldb.h"
#include "common/utility.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLoggingCategory>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <tlhelp32.h>
#endif

namespace OCC {

Q_LOGGING_CATEGORY(lcLegacyClient, "gui.legacyclient", QtInfoMsg)

namespace LegacyClient {

    namespace {

        // Ordner des offiziellen Clients und seine Konfigurationsdatei
        const QString legacyDirC = QStringLiteral("ownCloud");
        const QString legacyFileC = QStringLiteral("owncloud.cfg");
        // Standardname seines Journals (seit 2.6; ältere Clients tragen ihren Namen ein)
        const QString legacyJournalC = QStringLiteral(".sync_journal.db");

        // 2.x führte Ordner mit virtuellen Dateien in eigenen Gruppen
        const QStringList folderGroupsC = {QStringLiteral("Folders"), QStringLiteral("FoldersWithPlaceholders"), QStringLiteral("Multifolders")};

        QString withTrailingSlash(QString path)
        {
            if (!path.isEmpty() && !path.endsWith(QLatin1Char('/'))) {
                path.append(QLatin1Char('/'));
            }
            return path;
        }

        Folder readFolder(QSettings &settings, const QString &group)
        {
            Folder folder;
            folder.localPath = withTrailingSlash(QDir::fromNativeSeparators(settings.value(QStringLiteral("localPath")).toString()));
            folder.journalPath = settings.value(QStringLiteral("journalPath")).toString();
            const QString target = settings.value(QStringLiteral("targetPath")).toString();
            folder.targetPath = target.isEmpty() ? QStringLiteral("/") : target;
            folder.davUrl = settings.value(QStringLiteral("davUrl")).toUrl();
            folder.spaceId = settings.value(QStringLiteral("spaceId")).toString();
            folder.displayName = settings.value(QStringLiteral("displayString"), settings.value(QStringLiteral("displayName"))).toString();
            folder.paused = settings.value(QStringLiteral("paused")).toBool();
            folder.virtualFilesMode = settings.value(QStringLiteral("virtualFilesMode")).toString().toLower();
            if (folder.virtualFilesMode.isEmpty()) {
                // 2.5/2.6: usePlaceholders bzw. eigene Gruppe für Platzhalter-Ordner
                const bool placeholders = settings.value(QStringLiteral("usePlaceholders")).toBool() || group == QLatin1String("FoldersWithPlaceholders");
                folder.virtualFilesMode = placeholders ? QStringLiteral("suffix") : QStringLiteral("off");
            }
            return folder;
        }

        void removeDatabaseFiles(const QString &path)
        {
            for (const auto &suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm"), QStringLiteral("-journal")}) {
                QFile::remove(path + suffix);
            }
        }

        int defaultPort(const QUrl &url)
        {
            return url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0 ? 443 : 80;
        }

        // absoluter Ordnerpfad mit „/“ am Ende, Verknüpfungen aufgelöst, soweit der Ordner existiert
        QString normalizedFolder(const QString &path)
        {
            const QFileInfo info(QDir::fromNativeSeparators(path));
            const QString canonical = info.exists() ? info.canonicalFilePath() : QString();
            return withTrailingSlash(canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical);
        }

        // Ersatz für Tests (setOfficialClientForTesting)
        std::function<bool()> &runningProbeForTesting()
        {
            static std::function<bool()> probe;
            return probe;
        }

        QStringList &configCandidatesForTesting()
        {
            static QStringList candidates;
            return candidates;
        }

        bool officialClientRunningCached()
        {
            if (runningProbeForTesting()) {
                return runningProbeForTesting()();
            }
            // jeder Ordner fragt vor seinem Lauf; die Prozessliste genügt alle paar Sekunden (macOS startet dafür pgrep)
            static QElapsedTimer checked;
            static bool running = false;
            if (!checked.isValid() || checked.hasExpired(5000)) {
                running = isOfficialClientRunning();
                checked.start();
            }
            return running;
        }

        struct ConfigFolders
        {
            QDateTime modified;
            QStringList folders;
        };

        // lokale Ordner einer Konfiguration, neu gelesen nur, wenn die Datei sich geändert hat
        QStringList cachedLocalFolders(const QString &path)
        {
            static QHash<QString, ConfigFolders> cache;
            const QFileInfo info(path);
            if (!info.isFile()) {
                cache.remove(path);
                return {};
            }
            auto it = cache.find(path);
            if (it == cache.end() || it->modified != info.lastModified()) {
                const auto config = readConfig(path);
                it = cache.insert(path, {info.lastModified(), config ? localFolders(*config) : QStringList()});
            }
            return it->folders;
        }

    } // namespace

    bool Folder::usesVirtualFiles() const
    {
        return !virtualFilesMode.isEmpty() && virtualFilesMode != QLatin1String("off");
    }

    QString Folder::absoluteJournalPath() const
    {
        return QDir(localPath).filePath(journalPath.isEmpty() ? legacyJournalC : journalPath);
    }

    QString Account::displayString() const
    {
        const QString host = url.host() + (url.port() > 0 ? QStringLiteral(":%1").arg(url.port()) : QString());
        return QStringLiteral("%1@%2").arg(displayName.isEmpty() ? davUser : displayName, host);
    }

    QStringList candidateConfigFiles()
    {
        QStringList candidates;
#ifdef Q_OS_WIN
        // ab 3.0 im servergebundenen Profil (Roaming), 2.x lokal
        for (const auto &variable : {"APPDATA", "LOCALAPPDATA"}) {
            const QString base = qEnvironmentVariable(variable);
            if (!base.isEmpty()) {
                candidates << QDir(QDir::fromNativeSeparators(base)).filePath(legacyDirC + QLatin1Char('/') + legacyFileC);
            }
        }
#else
        // Linux: $XDG_CONFIG_HOME bzw. ~/.config, macOS: ~/Library/Preferences
        const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
        if (!base.isEmpty()) {
            candidates << QDir(base).filePath(legacyDirC + QLatin1Char('/') + legacyFileC);
        }
#endif
        return candidates;
    }

    std::optional<Config> readConfig(const QString &path)
    {
        if (!QFileInfo(path).isFile()) {
            return {};
        }
        // über eine Kopie lesen: QSettings soll an der Datei des Fremd-Clients nichts anfassen, auch keine Sperrdatei
        QTemporaryDir copyDir;
        const QString copy = copyDir.filePath(legacyFileC);
        if (!copyDir.isValid() || !QFile::copy(path, copy)) {
            qCWarning(lcLegacyClient) << "Could not read the configuration of the official client" << path;
            return {};
        }

        Config config;
        config.path = path;
        {
            QSettings settings(copy, QSettings::IniFormat);
            if (settings.status() != QSettings::NoError) {
                qCWarning(lcLegacyClient) << "Could not parse the configuration of the official client" << path << settings.status();
                return {};
            }
            config.clientVersion = settings.value(QStringLiteral("clientVersion")).toString();
            settings.beginGroup(QStringLiteral("Accounts"));
            const auto accountGroups = settings.childGroups();
            for (const auto &accountGroup : accountGroups) {
                settings.beginGroup(accountGroup);
                Account account;
                account.url = settings.value(QStringLiteral("url")).toUrl();
                for (const auto &key : {QStringLiteral("dav_user"), QStringLiteral("http_user"), QStringLiteral("user")}) {
                    account.davUser = settings.value(key).toString();
                    if (!account.davUser.isEmpty()) {
                        break;
                    }
                }
                // der Anmeldename (6.0.3: http_user, 2.x: user) für die Kennwortseite des Assistenten
                for (const auto &key : {QStringLiteral("http_user"), QStringLiteral("user"), QStringLiteral("dav_user")}) {
                    account.loginUser = settings.value(key).toString();
                    if (!account.loginUser.isEmpty()) {
                        break;
                    }
                }
                account.displayName = settings.value(QStringLiteral("display-name")).toString();
                QStringList seenPaths;
                for (const auto &group : folderGroupsC) {
                    settings.beginGroup(group);
                    const auto folderGroups = settings.childGroups();
                    for (const auto &folderGroup : folderGroups) {
                        settings.beginGroup(folderGroup);
                        const Folder folder = readFolder(settings, group);
                        settings.endGroup();
                        if (!folder.localPath.isEmpty() && !seenPaths.contains(folder.localPath)) {
                            seenPaths.append(folder.localPath);
                            account.folders.append(folder);
                        }
                    }
                    settings.endGroup();
                }
                settings.endGroup();
                if (account.url.isValid() && !account.url.host().isEmpty() && !account.davUser.isEmpty()) {
                    config.accounts.append(account);
                } else {
                    qCWarning(lcLegacyClient) << "Skipping incomplete account" << accountGroup << "of the official client";
                }
            }
        }
        return config;
    }

    std::optional<Config> findConfig(const QStringList &candidates)
    {
        for (const auto &candidate : candidates) {
            auto config = readConfig(candidate);
            if (config && !config->accounts.isEmpty()) {
                qCInfo(lcLegacyClient) << "Found the configuration of the official client" << candidate << "version" << config->clientVersion << "with"
                                       << config->accounts.size() << "accounts";
                return config;
            }
        }
        return {};
    }

    bool isOfficialClientProcessName(const QString &processName)
    {
        return processName.compare(QLatin1String("owncloud"), Qt::CaseInsensitive) == 0
            || processName.compare(QLatin1String("owncloud.exe"), Qt::CaseInsensitive) == 0;
    }

    bool isOfficialClientRunning()
    {
#if defined(Q_OS_WIN)
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return false;
        }
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool running = false;
        for (bool more = Process32FirstW(snapshot, &entry); more && !running; more = Process32NextW(snapshot, &entry)) {
            running = isOfficialClientProcessName(QString::fromWCharArray(entry.szExeFile));
        }
        CloseHandle(snapshot);
        return running;
#elif defined(Q_OS_LINUX)
        // /proc/<pid>/comm trägt den Programmnamen (höchstens 15 Zeichen)
        QDirIterator processes(QStringLiteral("/proc"), QDir::Dirs | QDir::NoDotAndDotDot);
        while (processes.hasNext()) {
            QFile comm(processes.next() + QStringLiteral("/comm"));
            if (comm.open(QIODevice::ReadOnly) && isOfficialClientProcessName(QString::fromUtf8(comm.readAll()).trimmed())) {
                return true;
            }
        }
        return false;
#else
        QProcess pgrep;
        pgrep.start(QStringLiteral("pgrep"), {QStringLiteral("-x"), QStringLiteral("owncloud")});
        return pgrep.waitForFinished(3000) && pgrep.exitStatus() == QProcess::NormalExit && pgrep.exitCode() == 0;
#endif
    }

    bool sameServer(const QUrl &a, const QUrl &b)
    {
        const auto port = [](const QUrl &url) {
            const int port = url.port(defaultPort(url));
            return port == defaultPort(url) ? -1 : port;
        };
        const auto path = [](const QUrl &url) {
            QString path = QDir::cleanPath(url.path());
            while (path.endsWith(QLatin1Char('/'))) {
                path.chop(1);
            }
            return path;
        };
        return !a.host().isEmpty() && a.host().compare(b.host(), Qt::CaseInsensitive) == 0 && port(a) == port(b) && path(a) == path(b);
    }

    bool pathsOverlap(const QString &a, const QString &b)
    {
        if (a.isEmpty() || b.isEmpty()) {
            return false;
        }
        const QString first = normalizedFolder(a);
        const QString second = normalizedFolder(b);
        const auto sensitivity = Utility::fsCaseSensitivity();
        return first.startsWith(second, sensitivity) || second.startsWith(first, sensitivity);
    }

    QStringList localFolders(const Config &config)
    {
        QStringList folders;
        for (const auto &account : config.accounts) {
            for (const auto &folder : account.folders) {
                folders.append(folder.localPath);
            }
        }
        return folders;
    }

    QString doubleSyncReason(const QString &localPath)
    {
        if (!officialClientRunningCached()) {
            return {};
        }
        // alle Orte: nach einem Umstieg von 2.x auf 3.x+ liegt die alte Konfiguration noch daneben
        const QStringList candidates = configCandidatesForTesting().isEmpty() ? candidateConfigFiles() : configCandidatesForTesting();
        for (const auto &candidate : candidates) {
            for (const auto &folder : cachedLocalFolders(candidate)) {
                if (pathsOverlap(localPath, folder)) {
                    qCWarning(lcLegacyClient) << "Not syncing" << localPath << "while the official client is running, it also syncs" << folder;
                    return QCoreApplication::translate("OCC::LegacyClient",
                        "The official client is running and also synchronizes the folder %1. Quit it, so that the folder is not "
                        "synchronized by two programs at the same time.")
                        .arg(QDir::toNativeSeparators(folder));
                }
            }
        }
        return {};
    }

    void setOfficialClientForTesting(const std::function<bool()> &isRunning, const QStringList &configCandidates)
    {
        runningProbeForTesting() = isRunning;
        configCandidatesForTesting() = configCandidates;
    }

    JournalCopy copyJournal(const QString &source, const QString &target, QString *error)
    {
        auto fail = [&target, error](JournalCopy result, const QString &message) {
            removeDatabaseFiles(target);
            if (error) {
                *error = message;
            }
            qCWarning(lcLegacyClient) << "Journal not taken over:" << message;
            return result;
        };

        if (!QFileInfo(source).isFile()) {
            return JournalCopy::NoJournal;
        }
        // Reste eines früheren Versuchs
        removeDatabaseFiles(target);
        // Datenbank mit ihrem Write-Ahead-Log bzw. Rollback-Journal, damit die Kopie vollständig ist; -shm baut SQLite neu auf
        for (const auto &suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-journal")}) {
            if ((suffix.isEmpty() || QFileInfo::exists(source + suffix)) && !QFile::copy(source + suffix, target + suffix)) {
                return fail(JournalCopy::Failed, QStringLiteral("could not copy %1").arg(source + suffix));
            }
        }
        if (SyncJournalDb::dbIsTooNewForClient(target)) {
            return fail(JournalCopy::TooNew, QStringLiteral("%1 was written by a newer client").arg(source));
        }
        {
            // die Kopie muss sich öffnen lassen und Einträge haben; eine defekte Kopie ersetzt SQLite durch eine leere
            SqlDatabase db;
            if (!db.openOrCreateReadWrite(target)) {
                return fail(JournalCopy::Failed, QStringLiteral("could not open the copy of %1").arg(source));
            }
            bool ok = false;
            {
                SqlQuery query(db);
                ok = query.prepare("SELECT COUNT(*) FROM metadata", true) == 0 && query.exec() && query.next().hasData;
            }
            db.close();
            if (!ok) {
                return fail(JournalCopy::Failed, QStringLiteral("the copy of %1 is not a sync journal").arg(source));
            }
        }
        return JournalCopy::Copied;
    }

} // namespace LegacyClient
} // namespace OCC
