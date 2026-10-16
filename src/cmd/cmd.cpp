/*
 * Copyright (C) 2026 by BW-Tech GmbH (owncloud.online)
 *
 * Command-line sync client (owncloudcmd) for owncloud.online. Reintroduced after
 * upstream dropped it (DC-46) so the desktop sync engine can be driven headless for
 * testing, CI and headless-server / NAS sync. Signs in with HTTP Basic
 * (username/password) or with an OAuth2 token file, since there is no browser for
 * the interactive OAuth2 sign-in. Offers the options of the 6.0.3 client.
 *
 * Usage: owncloudcmd [options] <local_dir> <server_url> [remote_folder]
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

#include "account.h"
#include "common/version.h"

#include "cmdoptions.h"
#include "cmdsession.h"
#include "httpbasiccredentials.h"
#include "httpoauthcredentials.h"
#include "oauthtokenfile.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QLoggingCategory>
#include <QNetworkProxyFactory>
#include <QTimer>

#include <iostream>
#include <string>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

using namespace OCC;

namespace {

QtMessageHandler previousMessageHandler = nullptr;

// --silent: nur Fehler ausgeben
void silentMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type != QtCriticalMsg && type != QtFatalMsg) {
        return;
    }
    if (previousMessageHandler) {
        previousMessageHandler(type, context, message);
    } else {
        std::cerr << qFormatLogMessage(type, context, message).toLocal8Bit().constData() << std::endl;
    }
}

// Kennwort ohne Echo von der Konsole bzw. als Zeile von der Standardeingabe
std::optional<QString> readPassword(const QString &user)
{
    std::cerr << "Password for user " << user.toLocal8Bit().constData() << ": " << std::flush;
    std::optional<QString> password;
#ifdef Q_OS_WIN
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(input, &mode)) {
        SetConsoleMode(input, mode & ~ENABLE_ECHO_INPUT);
        wchar_t buffer[1024];
        DWORD read = 0;
        if (ReadConsoleW(input, buffer, 1023, &read, nullptr)) {
            QString line = QString::fromWCharArray(buffer, static_cast<qsizetype>(read));
            while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r'))) {
                line.chop(1);
            }
            password = line;
        }
        SetConsoleMode(input, mode);
        std::cerr << std::endl;
        return password;
    }
#else
    termios previous{};
    const bool terminal = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &previous) == 0;
    if (terminal) {
        termios noEcho = previous;
        noEcho.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &noEcho);
    }
#endif
    std::string line;
    if (std::getline(std::cin, line)) {
        password = QString::fromUtf8(line).remove(QLatin1Char('\r'));
    }
#ifndef Q_OS_WIN
    if (terminal) {
        tcsetattr(STDIN_FILENO, TCSANOW, &previous);
    }
#endif
    std::cerr << std::endl;
    return password;
}

// Rückfrage des Löschschutzes; ohne Antwort (Ende der Eingabe) wird nichts gelöscht
bool confirmRemoveAll(const QString &message)
{
    std::cerr << message.toLocal8Bit().constData() << std::endl << "Delete these files? [y/N] " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) {
        std::cerr << std::endl;
        return false;
    }
    const QString text = QString::fromLocal8Bit(answer.c_str()).trimmed().toLower();
    return text == QLatin1String("y") || text == QLatin1String("yes") || text == QLatin1String("j") || text == QLatin1String("ja");
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("owncloudcmd"));
    QCoreApplication::setApplicationVersion(Version::displayString());

    const CmdOptionsResult parsed = parseCmdOptions(QCoreApplication::arguments());
    switch (parsed.action) {
    case CmdOptionsResult::Action::ShowHelp:
        std::cout << parsed.helpText.toLocal8Bit().constData() << std::flush;
        return EXIT_SUCCESS;
    case CmdOptionsResult::Action::ShowVersion:
        std::cout << "owncloudcmd " << Version::displayString().toLocal8Bit().constData() << std::endl;
        return EXIT_SUCCESS;
    case CmdOptionsResult::Action::Error:
        qCritical().noquote() << parsed.error;
        return EXIT_FAILURE;
    case CmdOptionsResult::Action::Run:
        break;
    }
    const CmdOptions &options = parsed.options;

    if (options.silent) {
        previousMessageHandler = qInstallMessageHandler(silentMessageHandler);
    }
    if (options.logDebug) {
        // dieselben Kategorien wie die Protokollierung der Desktop-App; QT_LOGGING_RULES geht weiter vor
        QLoggingCategory::setFilterRules(QStringLiteral("sync.*.debug=true\ngui.*.debug=true"));
    }
    if (options.proxy) {
        QNetworkProxyFactory::setUseSystemConfiguration(false);
        QNetworkProxy::setApplicationProxy(*options.proxy);
    }

    QString user = options.user;
    CmdSession::CredentialsFactory credentialsFactory;
    if (!options.tokenFile.isEmpty()) {
        QString error;
        const std::shared_ptr<OAuthTokenFile> tokens = OAuthTokenFile::load(options.tokenFile, &error);
        if (!tokens) {
            qCritical().noquote() << "Cannot use the OAuth2 token file:" << error;
            return EXIT_FAILURE;
        }
        // Ein Token geht nie an einen anderen Server als den, für den es ausgestellt wurde
        if (!tokens->belongsTo(options.serverUrl)) {
            qCritical().noquote() << "The OAuth2 token file belongs to" << tokens->serverUrl().toString() << "and not to" << options.serverUrl.toString();
            return EXIT_FAILURE;
        }
        // im Klartext nur zum eigenen Rechner unbedenklich
        if (options.serverUrl.scheme() != QLatin1String("https") && options.serverUrl.host() != QLatin1String("localhost")
            && !QHostAddress(options.serverUrl.host()).isLoopback()) {
            qWarning().noquote() << "The OAuth2 tokens are sent unencrypted to" << options.serverUrl.toString() << "- use https";
        }
        if (user.isEmpty()) {
            user = tokens->userId();
        }
        credentialsFactory = [tokens](Account *account) -> AbstractCredentials * { return new HttpOAuthCredentials(account, tokens); };
    } else {
        if (user.isEmpty()) {
            qCritical() << "A user (-u) and password (-p) are required";
            return EXIT_FAILURE;
        }
        QString password = options.password;
        if (!options.hasPassword) {
            // wie 6.0.3: ohne Kennwort fragen, außer mit --non-interactive
            const auto entered = options.interactive ? readPassword(user) : std::nullopt;
            if (!entered || entered->isEmpty()) {
                qCritical() << "A user (-u) and password (-p) are required";
                return EXIT_FAILURE;
            }
            password = *entered;
        }
        if (password.isEmpty()) {
            qCritical() << "A user (-u) and password (-p) are required";
            return EXIT_FAILURE;
        }
        // auch ein für die kanonische Kennung neu angelegtes Konto meldet sich mit dem ursprünglichen Login an
        credentialsFactory = [user, password](Account *account) -> AbstractCredentials * { return new HttpBasicCredentials(account, user, password); };
    }

    CmdSession::Settings settings;
    settings.ignoreHiddenFiles = options.ignoreHiddenFiles;
    settings.excludeFile = options.excludeFile;
    settings.unsyncedFolders = options.unsyncedFolders;
    settings.maxSyncRetries = options.maxSyncRetries;
    settings.uploadLimit = options.uploadLimit;
    settings.downloadLimit = options.downloadLimit;
    settings.trustSsl = options.trustSsl;
    if (options.interactive) {
        settings.confirmRemoveAll = confirmRemoveAll;
    }

    CmdSession session(options.serverUrl, user, options.localDir, options.remotePath, credentialsFactory, settings);
    QObject::connect(&session, &CmdSession::finished, &app, [](int exitCode) { QCoreApplication::exit(exitCode); });
    QTimer::singleShot(0, &session, &CmdSession::start);

    return app.exec();
}
