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

#include <QNetworkProxy>
#include <QProcessEnvironment>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

namespace OCC {

/** Befehlszeile von owncloudcmd: die Optionen des 6.0.3-Clients, dazu die OAuth2-Token-Datei. */
struct CmdOptions
{
    /** Lokaler Ordner, absolut und mit abschließendem / */
    QString localDir;
    QUrl serverUrl;
    QString remotePath = QStringLiteral("/");

    QString user;
    /** -p bzw. OWNCLOUDCMD_PASSWORD; leer, wenn keins angegeben ist */
    QString password;
    bool hasPassword = false;
    /** --oauth-token-file bzw. OWNCLOUDCMD_OAUTH_TOKEN_FILE */
    QString tokenFile;

    bool silent = false;
    bool logDebug = false;
    bool trustSsl = false;
    bool interactive = true;
    bool ignoreHiddenFiles = true;
    std::optional<QNetworkProxy> proxy;
    QString excludeFile;
    /** --unsyncedfolders: Serverordner, die nicht synchronisiert werden („Ordner/“) */
    std::optional<QSet<QString>> unsyncedFolders;
    int maxSyncRetries = 3;
    /** Byte/s, 0 = unbegrenzt */
    qint64 uploadLimit = 0;
    qint64 downloadLimit = 0;
};

struct CmdOptionsResult
{
    enum class Action { Run, ShowHelp, ShowVersion, Error };
    Action action = Action::Run;
    CmdOptions options;
    QString error;
    QString helpText;
};

/** Liest die Befehlszeile (mit Programmname vorn). Umgebungsvariablen: OWNCLOUDCMD_PASSWORD, OWNCLOUDCMD_OAUTH_TOKEN_FILE. */
CmdOptionsResult parseCmdOptions(const QStringList &arguments, const QProcessEnvironment &environment = QProcessEnvironment::systemEnvironment());

/** --httpproxy: http://host:port (auch nur host:port) oder socks5://host:port */
std::optional<QNetworkProxy> parseCmdProxy(const QString &value, QString *error);

/** --unsyncedfolders: ein Serverordner je Zeile, # leitet einen Kommentar ein */
std::optional<QSet<QString>> readUnsyncedFolders(const QString &path, QString *error);

} // namespace OCC
