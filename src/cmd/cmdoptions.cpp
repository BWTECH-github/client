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

#include "cmdoptions.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <limits>
#include <utility>

namespace OCC {

namespace {

const QString passwordEnvC = QStringLiteral("OWNCLOUDCMD_PASSWORD");
const QString tokenFileEnvC = QStringLiteral("OWNCLOUDCMD_OAUTH_TOKEN_FILE");

// nicht negative ganze Zahl einer Option
std::optional<qint64> nonNegativeNumber(const QString &value)
{
    bool ok = false;
    const qint64 number = value.trimmed().toLongLong(&ok);
    if (!ok || number < 0) {
        return {};
    }
    return number;
}

}

std::optional<QNetworkProxy> parseCmdProxy(const QString &value, QString *error)
{
    const QString text = value.trimmed();
    // ohne Schema wie 6.0.3 als HTTP-Proxy lesen
    const QUrl url(text.contains(QLatin1String("://")) ? text : QStringLiteral("http://") + text);
    const QString scheme = url.scheme().toLower();
    QNetworkProxy::ProxyType type = QNetworkProxy::HttpProxy;
    if (scheme == QLatin1String("socks5")) {
        type = QNetworkProxy::Socks5Proxy;
    } else if (scheme != QLatin1String("http")) {
        *error = QStringLiteral("Unsupported proxy type '%1', use http://host:port or socks5://host:port").arg(url.scheme());
        return {};
    }
    if (!url.isValid() || url.host().isEmpty() || url.port() <= 0 || url.port() > std::numeric_limits<quint16>::max()) {
        *error = QStringLiteral("Could not read the proxy '%1', it should have the format http://host:port").arg(value);
        return {};
    }
    QNetworkProxy proxy(type, url.host(), static_cast<quint16>(url.port()));
    // Zugangsdaten in der Adresse (http://user:pass@host:port) gelten für den Proxy
    if (!url.userName().isEmpty()) {
        proxy.setUser(url.userName());
        proxy.setPassword(url.password());
    }
    return proxy;
}

std::optional<QSet<QString>> readUnsyncedFolders(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Cannot read the list of unsynced folders '%1': %2").arg(path, file.errorString());
        return {};
    }
    QSet<QString> folders;
    const auto lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    for (auto line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            continue;
        }
        // wie die Auswahl-Sync-Liste des Journals: relativ, mit abschließendem /;
        // Serverpfade kennen keinen Rückstrich, er gilt deshalb überall als Trenner
        line.replace(QLatin1Char('\\'), QLatin1Char('/'));
        while (line.startsWith(QLatin1Char('/'))) {
            line.remove(0, 1);
        }
        if (line.isEmpty()) {
            continue;
        }
        if (!line.endsWith(QLatin1Char('/'))) {
            line.append(QLatin1Char('/'));
        }
        folders.insert(line);
    }
    return folders;
}

CmdOptionsResult parseCmdOptions(const QStringList &arguments, const QProcessEnvironment &environment)
{
    CmdOptionsResult result;
    auto fail = [&result](const QString &error) {
        result.action = CmdOptionsResult::Action::Error;
        result.error = error;
        return result;
    };

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("owncloud.online command-line sync client\n\n"
                       "Signs in with a user and password (-u/-p, or the password from the environment variable %1) or with an\n"
                       "OAuth2 token file (--oauth-token-file or the environment variable %2). Refreshed tokens are written back\n"
                       "to that file. Without a password and with a terminal, owncloudcmd asks for it.\n\n"
                       "If a sync would delete all or very many files (for example because the local folder is empty or a drive\n"
                       "is missing), owncloudcmd asks before deleting anything; with --non-interactive it stops instead.\n\n"
                       "Exit codes: 0 synced, 1 error, 2 the server no longer accepts the sign-in (create a new token file),\n"
                       "3 stopped because the sync would delete all or very many files.")
            .arg(passwordEnvC, tokenFileEnvC));
    const QCommandLineOption helpOpt = parser.addHelpOption();
    const QCommandLineOption versionOpt = parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("local_dir"), QStringLiteral("Local folder to sync"));
    parser.addPositionalArgument(QStringLiteral("server_url"), QStringLiteral("Server root URL, e.g. https://cloud.example.com"));
    parser.addPositionalArgument(QStringLiteral("remote_folder"), QStringLiteral("Remote folder (default /), same as --path"), QStringLiteral("[remote_folder]"));

    const QCommandLineOption userOpt({QStringLiteral("u"), QStringLiteral("user")}, QStringLiteral("User name"), QStringLiteral("user"));
    const QCommandLineOption passOpt({QStringLiteral("p"), QStringLiteral("password")},
        QStringLiteral("Password (visible in the process list, prefer %1)").arg(passwordEnvC), QStringLiteral("password"));
    const QCommandLineOption tokenFileOpt(QStringLiteral("oauth-token-file"),
        QStringLiteral("OAuth2 token file (JSON with server_url, access_token, refresh_token, expires_at); replaces -u/-p"), QStringLiteral("file"));
    const QCommandLineOption pathOpt(QStringLiteral("path"), QStringLiteral("Remote folder (default /)"), QStringLiteral("path"));
    const QCommandLineOption silentOpt({QStringLiteral("s"), QStringLiteral("silent")}, QStringLiteral("Only print errors"));
    const QCommandLineOption proxyOpt(QStringLiteral("httpproxy"), QStringLiteral("Use this proxy"), QStringLiteral("http://server:port"));
    const QCommandLineOption trustOpt(QStringLiteral("trust"), QStringLiteral("Trust the TLS certificate of the server even if it is not valid"));
    const QCommandLineOption excludeOpt(QStringLiteral("exclude"), QStringLiteral("Additional exclude list"), QStringLiteral("file"));
    const QCommandLineOption unsyncedOpt(
        QStringLiteral("unsyncedfolders"), QStringLiteral("File with the remote folders not to sync, one per line (selective sync)"), QStringLiteral("file"));
    const QCommandLineOption nonInteractiveOpt(
        QStringLiteral("non-interactive"), QStringLiteral("Never ask: no password prompt, stop instead of asking before deleting all files"));
    const QCommandLineOption retriesOpt(
        QStringLiteral("max-sync-retries"), QStringLiteral("Sync again at most n times if the server or the folder changed meanwhile (default 3)"),
        QStringLiteral("n"));
    const QCommandLineOption upOpt(QStringLiteral("uplimit"), QStringLiteral("Limit the upload speed to n KB/s"), QStringLiteral("n"));
    const QCommandLineOption downOpt(QStringLiteral("downlimit"), QStringLiteral("Limit the download speed to n KB/s"), QStringLiteral("n"));
    const QCommandLineOption hiddenOpt(QStringLiteral("sync-hidden-files"), QStringLiteral("Also sync hidden files"));
    const QCommandLineOption debugOpt(QStringLiteral("logdebug"), QStringLiteral("More verbose logging"));
    for (const auto &option : {userOpt, passOpt, tokenFileOpt, pathOpt, silentOpt, proxyOpt, trustOpt, excludeOpt, unsyncedOpt, nonInteractiveOpt,
             retriesOpt, upOpt, downOpt, hiddenOpt, debugOpt}) {
        parser.addOption(option);
    }

    result.helpText = parser.helpText();
    if (!parser.parse(arguments)) {
        return fail(parser.errorText());
    }
    if (parser.isSet(helpOpt)) {
        result.action = CmdOptionsResult::Action::ShowHelp;
        return result;
    }
    if (parser.isSet(versionOpt)) {
        result.action = CmdOptionsResult::Action::ShowVersion;
        return result;
    }

    const QStringList args = parser.positionalArguments();
    if (args.size() < 2 || args.size() > 3) {
        return fail(QStringLiteral("Usage: owncloudcmd [options] <local_dir> <server_url> [remote_folder]"));
    }
    auto &options = result.options;

    const QFileInfo localDir(args.at(0));
    if (!localDir.isDir()) {
        return fail(QStringLiteral("The local folder %1 does not exist").arg(args.at(0)));
    }
    options.localDir = QDir(localDir.absoluteFilePath()).absolutePath();
    if (!options.localDir.endsWith(QLatin1Char('/'))) {
        options.localDir.append(QLatin1Char('/'));
    }
    options.serverUrl = QUrl::fromUserInput(args.at(1));
    if (!options.serverUrl.isValid() || options.serverUrl.host().isEmpty()) {
        return fail(QStringLiteral("Invalid server URL %1").arg(args.at(1)));
    }

    if (args.size() == 3 && parser.isSet(pathOpt)) {
        return fail(QStringLiteral("Use either --path or the remote_folder argument, not both"));
    }
    QString remotePath = args.size() == 3 ? args.at(2) : parser.isSet(pathOpt) ? parser.value(pathOpt) : QStringLiteral("/");
    if (!remotePath.startsWith(QLatin1Char('/'))) {
        remotePath.prepend(QLatin1Char('/'));
    }
    options.remotePath = remotePath;

    // Anmeldung: -p schließt die Token-Datei aus; Umgebungsvariablen gelten nur ohne Angabe auf der Befehlszeile
    options.user = parser.value(userOpt);
    if (parser.isSet(tokenFileOpt) && parser.isSet(passOpt)) {
        return fail(QStringLiteral("Use either a password (-p) or an OAuth2 token file (--oauth-token-file), not both"));
    }
    if (parser.isSet(passOpt)) {
        options.password = parser.value(passOpt);
        options.hasPassword = true;
    } else {
        options.tokenFile = parser.isSet(tokenFileOpt) ? parser.value(tokenFileOpt) : environment.value(tokenFileEnvC);
        if (options.tokenFile.isEmpty() && environment.contains(passwordEnvC)) {
            options.password = environment.value(passwordEnvC);
            options.hasPassword = true;
        }
    }

    options.silent = parser.isSet(silentOpt);
    options.logDebug = parser.isSet(debugOpt);
    options.trustSsl = parser.isSet(trustOpt);
    options.interactive = !parser.isSet(nonInteractiveOpt);
    options.ignoreHiddenFiles = !parser.isSet(hiddenOpt);

    if (parser.isSet(proxyOpt)) {
        QString error;
        options.proxy = parseCmdProxy(parser.value(proxyOpt), &error);
        if (!options.proxy) {
            return fail(error);
        }
    }
    if (parser.isSet(excludeOpt)) {
        options.excludeFile = parser.value(excludeOpt);
        const QFileInfo exclude(options.excludeFile);
        if (!exclude.isFile() || !exclude.isReadable()) {
            return fail(QStringLiteral("Cannot read the exclude list %1").arg(options.excludeFile));
        }
        options.excludeFile = exclude.absoluteFilePath();
    }
    if (parser.isSet(unsyncedOpt)) {
        QString error;
        options.unsyncedFolders = readUnsyncedFolders(parser.value(unsyncedOpt), &error);
        if (!options.unsyncedFolders) {
            return fail(error);
        }
    }
    if (parser.isSet(retriesOpt)) {
        const auto retries = nonNegativeNumber(parser.value(retriesOpt));
        if (!retries || *retries > 1000) {
            return fail(QStringLiteral("--max-sync-retries needs a number from 0 to 1000"));
        }
        options.maxSyncRetries = static_cast<int>(*retries);
    }
    // KB/s wie in den Netzwerk-Einstellungen der Desktop-App (1 KB = 1024 Byte)
    for (const auto &[option, limit] : {std::pair{upOpt, &options.uploadLimit}, std::pair{downOpt, &options.downloadLimit}}) {
        if (parser.isSet(option)) {
            const auto kbytes = nonNegativeNumber(parser.value(option));
            if (!kbytes || *kbytes > std::numeric_limits<qint64>::max() / 1024) {
                return fail(QStringLiteral("--%1 needs a number of KB/s, 0 for no limit").arg(option.names().constFirst()));
            }
            *limit = *kbytes * 1024;
        }
    }
    return result;
}

} // namespace OCC
