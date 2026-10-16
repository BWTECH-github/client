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
#include "legacyclient/legacyclientconfig.h"

#include <QObject>
#include <QPointer>
#include <QStringList>

#include <optional>

namespace OCC {

class AccountState;
class ColorManager;
class FolderMan;
class MainWindow;

namespace LegacyClient {

    struct OWNCLOUDGUI_EXPORT FolderImport
    {
        enum class Result {
            /** an Ort und Stelle übernommen, Journal kopiert: nichts wird erneut heruntergeladen */
            Adopted,
            /** an Ort und Stelle übernommen, ohne Journal: Dateien werden einmal mit dem Server abgeglichen */
            AdoptedWithoutJournal,
            /** Ordner mit virtuellen Dateien neu eingerichtet (nur Platzhalter) */
            NewVirtualFilesFolder,
            Skipped,
        };
        Result result = Result::Skipped;
        /** lokaler Ordner in owncloud.online */
        QString localPath;
        /** übersetzte Begründung bei Skipped */
        QString reason;
    };

    /** Name des eigenen Journals in übernommenen Ordnern; das Journal des Fremd-Clients bleibt daneben unberührt. */
    OWNCLOUDGUI_EXPORT QString adoptedJournalInfix();

    /** Übernimmt einen Ordner des Fremd-Clients für ein angemeldetes Konto. */
    OWNCLOUDGUI_EXPORT FolderImport importFolder(FolderMan *folderMan, AccountState *accountState, const Folder &folder);

} // namespace LegacyClient

/**
 * owncloud.online: Ablauf des Umstiegs vom offiziellen Client – Übersicht und Rückfrage, Warnung bei laufendem
 * Fremd-Client, je Konto Anmeldung im Assistenten (Adresse vorbelegt), Ordner übernehmen, Bericht.
 */
class OWNCLOUDGUI_EXPORT LegacyClientImportController : public QObject
{
    Q_OBJECT
public:
    LegacyClientImportController(const LegacyClient::Config &config, MainWindow *window, ColorManager *colorManager, QObject *parent);

    /**
     * Übersicht der gefundenen Konten und Rückfrage; beim ersten Start mit der Wahl „Neues Konto einrichten“.
     * false: nicht übernehmen (oder nichts mehr zu übernehmen).
     */
    bool confirm(bool firstStart);

    /** Meldet sich je Konto an und übernimmt die Ordner; finished() am Ende. */
    void start();

Q_SIGNALS:
    void finished();

private:
    void importNextAccount();
    void importFolders(AccountState *accountState, bool useVfs);
    void accountFailed(const QString &error);
    bool officialClientStopped();
    void showReport();

    LegacyClient::Config _config;
    QList<LegacyClient::Account> _pending;
    std::optional<LegacyClient::Account> _current;
    QStringList _report;
    QPointer<MainWindow> _window;
    ColorManager *_colorManager = nullptr;
};

} // namespace OCC
