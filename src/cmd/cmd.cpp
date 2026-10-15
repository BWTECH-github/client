/*
 * Copyright (C) 2026 by BW-Tech GmbH (owncloud.online)
 *
 * Minimal command-line sync client (owncloudcmd) for owncloud.online. Reintroduced
 * after upstream dropped it (DC-46) so the desktop sync engine can be driven
 * headless for testing, CI and headless-server / NAS sync. Signs in with HTTP Basic
 * (username/password) or with an OAuth2 token file, since there is no browser for
 * the interactive OAuth2 sign-in.
 *
 * Usage: owncloudcmd <local_dir> <server_url> (-u <user> -p <password> | --oauth-token-file <file>) [--path <remote>]
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

#include "cmdsession.h"
#include "httpbasiccredentials.h"
#include "httpoauthcredentials.h"
#include "oauthtokenfile.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QHostAddress>
#include <QTimer>

using namespace OCC;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("owncloudcmd"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("owncloud.online command-line sync client\n\n"
                       "Signs in with a user and password (-u/-p) or with an OAuth2 token file (--oauth-token-file or the\n"
                       "environment variable OWNCLOUDCMD_OAUTH_TOKEN_FILE). Refreshed tokens are written back to that file.\n\n"
                       "Exit codes: 0 synced, 1 error, 2 the server no longer accepts the sign-in (create a new token file)."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("local_dir"), QStringLiteral("Local folder to sync"));
    parser.addPositionalArgument(QStringLiteral("server_url"), QStringLiteral("Server root URL, e.g. https://cloud.example.com"));
    QCommandLineOption userOpt({ QStringLiteral("u"), QStringLiteral("user") }, QStringLiteral("User name"), QStringLiteral("user"));
    QCommandLineOption passOpt({ QStringLiteral("p"), QStringLiteral("password") }, QStringLiteral("Password"), QStringLiteral("password"));
    QCommandLineOption tokenFileOpt(QStringLiteral("oauth-token-file"),
        QStringLiteral("OAuth2 token file (JSON with server_url, access_token, refresh_token, expires_at); replaces -u/-p"), QStringLiteral("file"));
    QCommandLineOption pathOpt(QStringLiteral("path"), QStringLiteral("Remote folder (default /)"), QStringLiteral("path"), QStringLiteral("/"));
    parser.addOption(userOpt);
    parser.addOption(passOpt);
    parser.addOption(tokenFileOpt);
    parser.addOption(pathOpt);
    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.size() < 2) {
        qCritical() << "Usage: owncloudcmd <local_dir> <server_url> (-u <user> -p <password> | --oauth-token-file <file>) [--path <remote>]";
        return EXIT_FAILURE;
    }

    const QString localDir = QDir(args.at(0)).absolutePath() + QLatin1Char('/');
    const QUrl serverUrl = QUrl::fromUserInput(args.at(1));
    QString user = parser.value(userOpt);
    const QString password = parser.value(passOpt);
    const QString remotePath = parser.value(pathOpt);

    if (parser.isSet(tokenFileOpt) && parser.isSet(passOpt)) {
        qCritical() << "Use either a password (-p) or an OAuth2 token file (--oauth-token-file), not both";
        return EXIT_FAILURE;
    }
    // Die Umgebungsvariable gilt nur, wenn die Befehlszeile keine andere Anmeldung nennt
    QString tokenFilePath = parser.value(tokenFileOpt);
    if (tokenFilePath.isEmpty() && !parser.isSet(passOpt)) {
        tokenFilePath = qEnvironmentVariable("OWNCLOUDCMD_OAUTH_TOKEN_FILE");
    }

    CmdSession::CredentialsFactory credentialsFactory;
    if (!tokenFilePath.isEmpty()) {
        QString error;
        const std::shared_ptr<OAuthTokenFile> tokens = OAuthTokenFile::load(tokenFilePath, &error);
        if (!tokens) {
            qCritical().noquote() << "Cannot use the OAuth2 token file:" << error;
            return EXIT_FAILURE;
        }
        // Ein Token geht nie an einen anderen Server als den, für den es ausgestellt wurde
        if (!tokens->belongsTo(serverUrl)) {
            qCritical().noquote() << "The OAuth2 token file belongs to" << tokens->serverUrl().toString() << "and not to" << serverUrl.toString();
            return EXIT_FAILURE;
        }
        // im Klartext nur zum eigenen Rechner unbedenklich
        if (serverUrl.scheme() != QLatin1String("https") && serverUrl.host() != QLatin1String("localhost") && !QHostAddress(serverUrl.host()).isLoopback()) {
            qWarning().noquote() << "The OAuth2 tokens are sent unencrypted to" << serverUrl.toString() << "- use https";
        }
        if (user.isEmpty()) {
            user = tokens->userId();
        }
        credentialsFactory = [tokens](Account *account) -> AbstractCredentials * { return new HttpOAuthCredentials(account, tokens); };
    } else {
        if (user.isEmpty() || password.isEmpty()) {
            qCritical() << "A user (-u) and password (-p) are required";
            return EXIT_FAILURE;
        }
        // auch ein für die kanonische Kennung neu angelegtes Konto meldet sich mit dem ursprünglichen Login an
        credentialsFactory = [user, password](Account *account) -> AbstractCredentials * { return new HttpBasicCredentials(account, user, password); };
    }

    CmdSession session(serverUrl, user, localDir, remotePath, credentialsFactory);
    QObject::connect(&session, &CmdSession::finished, &app, [](int exitCode) { QCoreApplication::exit(exitCode); });
    QTimer::singleShot(0, &session, &CmdSession::start);

    return app.exec();
}
