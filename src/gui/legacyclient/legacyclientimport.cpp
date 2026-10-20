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

#include "legacyclient/legacyclientimport.h"

#include "accessmanager.h"
#include "account.h"
#include "accountmanager.h"
#include "accountstate.h"
#include "common/syncjournaldb.h"
#include "common/utility.h"
#include "common/vfs.h"
#include "folder.h"
#include "folderman.h"
#include "mainwindow/colormanager.h"
#include "mainwindow/mainwindow.h"
#include "networkadapters/serverdiagnostics.h"
#include "newaccountwizard/newaccountbuilder.h"
#include "newaccountwizard/newaccountmodel.h"
#include "newaccountwizard/newaccountwizard.h"
#include "newaccountwizard/newaccountwizardcontroller.h"
#include "theme.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTimer>

#include <algorithm>
#include <utility>

using namespace std::chrono_literals;

namespace OCC {

Q_LOGGING_CATEGORY(lcLegacyClientImport, "gui.legacyclient.import", QtInfoMsg)

namespace LegacyClient {

    namespace {

        FolderImport skipped(const Folder &folder, const QString &reason)
        {
            qCInfo(lcLegacyClientImport) << "Not taking over" << folder.localPath << ":" << reason;
            return {FolderImport::Result::Skipped, folder.localPath, reason};
        }

        bool sameDavUrl(const QUrl &a, const QUrl &b)
        {
            const auto normalized = [](const QUrl &url) {
                return url.adjusted(QUrl::StripTrailingSlash | QUrl::NormalizePathSegments | QUrl::RemoveUserInfo).toString().toLower();
            };
            return normalized(a) == normalized(b);
        }

        // „//“ im Pfad zu „/“: Utility::concatUrlPath() verlangt Pfade ohne „//“ (Q_ASSERT, in Beta-Builds ein Absturz). Nicht
        // QDir::cleanPath(), das unter Windows ein führendes „//“ (UNC) stehen ließe.
        QString withoutDoubledSlashes(QString path)
        {
            static const QRegularExpression doubledSlashes(QStringLiteral("/{2,}"));
            return path.replace(doubledSlashes, QStringLiteral("/"));
        }

        // owncloud.online synchronisiert den Ordner (oder einen darin bzw. darüber) schon?
        bool alreadySynchronized(FolderMan *folderMan, const QString &localPath)
        {
            const auto folders = folderMan->folders();
            return std::any_of(folders.cbegin(), folders.cend(), [&](const OCC::Folder *f) { return pathsOverlap(f->path(), localPath); });
        }

    } // namespace

    QString adoptedJournalInfix()
    {
        return QStringLiteral("online");
    }

    bool isSameAccount(const Account &legacy, const OCC::Account *account)
    {
        return account && account->davUser().compare(legacy.davUser, Qt::CaseInsensitive) == 0 && sameServer(account->url(), legacy.url);
    }

    QList<SignInSupport> checkSignInSupport(QNetworkAccessManager *nam, const QList<Account> &accounts, std::chrono::milliseconds timeout)
    {
        QList<SignInSupport> results(accounts.size(), SignInSupport::Available);
        qsizetype running = 0;
        QEventLoop loop;
        const auto done = [&running, &loop] {
            if (--running == 0) {
                loop.quit();
            }
        };
        const auto makeRequest = [timeout](const QUrl &url) {
            QNetworkRequest request(url);
            request.setTransferTimeout(static_cast<int>(timeout.count()));
            return request;
        };

        for (qsizetype i = 0; i < accounts.size(); ++i) {
            QUrl server = accounts.at(i).url.adjusted(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment);
            server.setPath(withoutDoubledSlashes(server.path(QUrl::FullyEncoded)), QUrl::TolerantMode);
            if (!server.isValid() || server.host().isEmpty()) {
                continue;
            }
            ++running;
            // dieselben Proben wie DetermineAuthTypeAdapter: Wer OpenID Connect anbietet, braucht die App „OAuth2“ nicht
            QNetworkReply *openIdReply = nam->get(makeRequest(Utility::concatUrlPath(server, QStringLiteral("/.well-known/openid-configuration"))));
            QObject::connect(openIdReply, &QNetworkReply::finished, &loop, [&, i, server, openIdReply] {
                openIdReply->deleteLater();
                const bool openIdConnect = openIdReply->error() == QNetworkReply::NoError && ServerDiagnostics::isOpenIdConfiguration(openIdReply->readAll());
                // ohne HTTP-Antwort (Netz, Zertifikat, Zeitüberschreitung) ist über den Server nichts zu sagen: Das klärt der Assistent
                if (openIdConnect || openIdReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 0) {
                    done();
                    return;
                }
                // ohne grant_type antwortet die App mit einem JSON-Fehler; ohne die App leitet der Server zur Anmeldeseite um
                QNetworkRequest tokenRequest = makeRequest(Utility::concatUrlPath(server, QStringLiteral("/index.php/apps/oauth2/api/v1/token")));
                tokenRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
                tokenRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
                QNetworkReply *tokenReply = nam->post(tokenRequest, QByteArray());
                QObject::connect(tokenReply, &QNetworkReply::finished, &loop, [&, i, server, tokenReply] {
                    tokenReply->deleteLater();
                    const auto issue = ServerDiagnostics::classifyOAuth2TokenReply(tokenReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
                        tokenReply->readAll(), QUrl::fromEncoded(tokenReply->rawHeader(QByteArrayLiteral("Location"))));
                    if (issue == ServerDiagnostics::Issue::OAuth2AppMissing) {
                        qCInfo(lcLegacyClientImport) << "The OAuth2 app is missing on" << server.toDisplayString() << "- the account cannot be taken over";
                        results[i] = SignInSupport::OAuth2AppMissing;
                    }
                    done();
                });
            });
        }
        // Beendet sich das Programm währenddessen, kehrt exec() vorzeitig zurück; nicht geprüfte Server gelten dann als verfügbar
        if (running > 0) {
            loop.exec();
        }
        return results;
    }

    FolderImport importFolder(FolderMan *folderMan, AccountState *accountState, const Folder &folder, const QStringList &officialFolders)
    {
        OCC::Account *account = accountState->account();
        if (!folder.spaceId.isEmpty() || account->capabilities().spacesSupport().enabled) {
            return skipped(folder, QCoreApplication::translate("OCC::LegacyClient", "Spaces are not taken over; add them after signing in."));
        }
        if (folder.davUrl.isValid() && !sameDavUrl(folder.davUrl, account->davUrl())) {
            return skipped(folder, QCoreApplication::translate("OCC::LegacyClient", "The folder belongs to a different user or server."));
        }
        // mehrere Ordnerpaare je Konto, aber jeden Serverordner (samt Ober- und Unterordnern) nur einmal, wie im
        // Ordner-Assistenten; geprüft vor dem Kopieren des Journals
        const QString remotePathError = folderMan->checkRemotePathValidity(account->uuid(), folder.targetPath);
        if (!remotePathError.isEmpty()) {
            return skipped(folder, remotePathError);
        }
        const QString displayName = folder.displayName.isEmpty() ? QDir(folder.localPath).dirName() : folder.displayName;

        if (folder.usesVirtualFiles()) {
            // Platzhalter gehören zur Synchronisationswurzel des Fremd-Clients: an anderer Stelle neu einrichten,
            // es entstehen nur Platzhalter, heruntergeladen wird nichts
            if (!VfsPluginManager::instance().isVirtualFilesAvailable()) {
                return skipped(folder, QCoreApplication::translate("OCC::LegacyClient", "The folder uses virtual files, which are not available on this computer."));
            }
            const QString syncRoot = account->hasDefaultSyncRoot() ? account->defaultSyncRoot()
                                                                   : FolderMan::suggestSyncFolder(FolderMan::NewFolderType::SpacesSyncRoot, account->uuid());
            const bool isRoot = folder.targetPath == QLatin1String("/") || folder.targetPath.isEmpty();
            const QString localPath = isRoot ? syncRoot
                                             : folderMan->findGoodPathForNewSyncFolder(syncRoot, displayName, FolderMan::NewFolderType::SpacesFolder, account->uuid());
            // nie in einem Ordner des Fremd-Clients oder um ihn herum: dessen Platzhalter und Synchronisationswurzel bleiben,
            // wie sie sind, und kein Ordner wird von beiden Programmen synchronisiert
            for (const auto &officialFolder : officialFolders) {
                if (pathsOverlap(localPath, officialFolder)) {
                    return skipped(folder,
                        QCoreApplication::translate("OCC::LegacyClient", "The new folder %1 would overlap the folder %2 of the official client.")
                            .arg(QDir::toNativeSeparators(localPath), QDir::toNativeSeparators(officialFolder)));
                }
            }
            if (alreadySynchronized(folderMan, localPath)) {
                return skipped(folder, QCoreApplication::translate("OCC::LegacyClient", "The folder is already synchronized."));
            }
            FolderMan::SyncConnectionDescription description;
            description.davUrl = account->davUrl();
            description.localPath = localPath;
            description.remotePath = folder.targetPath;
            description.displayName = isRoot ? Theme::instance()->appNameGUI() : displayName;
            description.useVirtualFiles = true;
            description.sortPriority = 0;
            const QString setupError = folderMan->addFolderFromGui(accountState, description);
            // das Konto kann schon weitere Ordnerpaare haben: den neuen Ordner über seinen Pfad finden
            const OCC::Folder *created = setupError.isEmpty() ? folderMan->folderForPath(localPath) : nullptr;
            if (!created) {
                return skipped(folder, setupError.isEmpty() ? QCoreApplication::translate("OCC::LegacyClient", "The folder could not be set up.") : setupError);
            }
            return {FolderImport::Result::NewVirtualFilesFolder, created->path(), {}};
        }

        // klassisch synchronisierter Ordner: an Ort und Stelle übernehmen, das Journal als Kopie
        if (!QFileInfo(folder.localPath).isDir()) {
            return skipped(folder, QCoreApplication::translate("OCC::LegacyClient", "The local folder does not exist."));
        }
        if (alreadySynchronized(folderMan, folder.localPath)) {
            return skipped(folder, QCoreApplication::translate("OCC::LegacyClient", "The folder is already synchronized."));
        }
        const QString journalName = SyncJournalDb::makeDbName(folder.localPath, adoptedJournalInfix());
        const QString journal = QDir(folder.localPath).filePath(journalName);
        const auto copy = copyJournal(folder.absoluteJournalPath(), journal);

        FolderDefinition definition(account->davUrl(), QString(), displayName);
        definition.setLocalPath(folder.localPath);
        definition.setTargetPath(folder.targetPath);
        definition.setJournalPath(journalName);
        definition.setPaused(folder.paused);
        const OCC::Folder *adopted = folderMan->adoptExistingFolder(accountState, definition);
        if (!adopted) {
            for (const auto &suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm"), QStringLiteral("-journal")}) {
                QFile::remove(journal + suffix);
            }
            return skipped(folder, QCoreApplication::translate("OCC::LegacyClient", "The folder could not be set up."));
        }
        return {copy == JournalCopy::Copied ? FolderImport::Result::Adopted : FolderImport::Result::AdoptedWithoutJournal, adopted->path(), {}};
    }

} // namespace LegacyClient

LegacyClientImportController::LegacyClientImportController(const LegacyClient::Config &config, MainWindow *window, ColorManager *colorManager, QObject *parent)
    : QObject(parent)
    , _config(config)
    , _window(window)
    , _colorManager(colorManager)
{
}

bool LegacyClientImportController::confirm(bool firstStart)
{
    // schon eingerichtete Konten nicht noch einmal
    _pending.clear();
    QList<LegacyClient::Account> candidates;
    for (const auto &account : std::as_const(_config.accounts)) {
        if (AccountManager::instance()->accountForLoginExists(account.url, account.davUser)) {
            _report.append(tr("%1: already set up.").arg(account.displayString()));
            continue;
        }
        candidates.append(account);
    }
    if (candidates.isEmpty()) {
        if (!firstStart && _window) {
            QMessageBox::information(_window, tr("Import from the official client"), tr("All accounts of the official client are already set up."));
        }
        return false;
    }

    // vor der Rückfrage: Ohne die App „OAuth2“ auf dem Server meldet sich owncloud.online nicht an, das Konto ließe sich also
    // nicht übernehmen. Die Anmeldung im Assistenten scheiterte sonst erst nach dem Beenden des offiziellen Clients.
    QList<LegacyClient::SignInSupport> support;
    {
        AccessManager accessManager(nullptr);
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
        const auto restoreCursor = qScopeGuard([] { QGuiApplication::restoreOverrideCursor(); });
        support = LegacyClient::checkSignInSupport(&accessManager, candidates, 10s);
    }
    QStringList lines;
    QStringList blockedLines;
    for (qsizetype i = 0; i < candidates.size(); ++i) {
        const auto &account = candidates.at(i);
        const QString line = tr("• %1 – %n sync folder(s)", "", account.folders.size()).arg(account.displayString());
        if (support.value(i) == LegacyClient::SignInSupport::OAuth2AppMissing) {
            blockedLines.append(line);
            _report.append(tr("%1: not taken over, the \"OAuth2\" app is not installed or not enabled on the server.").arg(account.displayString()));
        } else {
            _pending.append(account);
            lines.append(line);
        }
    }

    QString text;
    if (_pending.isEmpty()) {
        text = tr("The official desktop client is set up on this computer.");
    } else {
        text = tr("The official desktop client is set up on this computer. Do you want to take over its accounts and sync folders?") + QStringLiteral("\n\n")
            + lines.join(QLatin1Char('\n'));
    }
    if (!blockedLines.isEmpty()) {
        text += QStringLiteral("\n\n")
            + tr("%n account(s) cannot be taken over because the \"OAuth2\" app, which is needed to sign in, is not installed or not enabled on the server:",
                "", blockedLines.size())
            + QStringLiteral("\n\n") + blockedLines.join(QLatin1Char('\n')) + QStringLiteral("\n\n")
            + tr("Please ask your administrator to install the \"OAuth2\" app via \"Market\" and enable it for all users. Afterwards, choose \"Import "
                 "from official client...\" in the menu.");
    }
    QMessageBox box(
        _pending.isEmpty() ? QMessageBox::Information : QMessageBox::Question, tr("Import from the official client"), text, QMessageBox::NoButton, _window);
    QPushButton *importButton = nullptr;
    if (!_pending.isEmpty()) {
        box.setInformativeText(tr("You sign in once per account in the browser. Folders that are already synchronized are recognized and not "
                                  "downloaded again. Nothing is changed in the official client."));
        importButton = box.addButton(tr("Import"), QMessageBox::AcceptRole);
        box.setDefaultButton(importButton);
    }
    if (firstStart) {
        QPushButton *newAccountButton = box.addButton(tr("Set up a new account"), QMessageBox::RejectRole);
        if (!importButton) {
            box.setDefaultButton(newAccountButton);
        }
    } else if (importButton) {
        box.addButton(tr("Cancel"), QMessageBox::RejectRole);
    } else {
        box.addButton(QMessageBox::Ok);
    }
    if (_window) {
        _window->ensureVisible();
    }
    box.exec();
    return importButton && box.clickedButton() == importButton && officialClientStopped();
}

bool LegacyClientImportController::officialClientStopped()
{
    // zwei Programme auf denselben Ordnern synchronisieren gegeneinander
    while (LegacyClient::isOfficialClientRunning()) {
        QMessageBox box(QMessageBox::Warning, tr("Import from the official client"),
            tr("The official client is still running. Quit it and turn off its autostart before importing, otherwise both programs "
               "synchronize the same folders."),
            QMessageBox::NoButton, _window);
        QPushButton *retryButton = box.addButton(tr("Check again"), QMessageBox::AcceptRole);
        QPushButton *anywayButton = box.addButton(tr("Import anyway"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(retryButton);
        box.exec();
        if (box.clickedButton() == anywayButton) {
            qCWarning(lcLegacyClientImport) << "Importing although the official client is running";
            return true;
        }
        if (box.clickedButton() != retryButton) {
            return false;
        }
    }
    return true;
}

void LegacyClientImportController::start()
{
    qCInfo(lcLegacyClientImport) << "Importing" << _pending.size() << "accounts from" << _config.path;
    QTimer::singleShot(0, this, &LegacyClientImportController::importNextAccount);
}

void LegacyClientImportController::importNextAccount()
{
    if (_pending.isEmpty() || !_window) {
        showReport();
        Q_EMIT finished();
        return;
    }
    _current = _pending.takeFirst();

    NewAccountWizard wizard(_window);
    _colorManager->keepAccent(&wizard);
    NewAccountModel model(nullptr);
    NewAccountWizardController wizardController(&model, &wizard, _colorManager, nullptr);
    wizardController.presetServerUrl(_current->url);
    wizard.setWindowTitle(tr("Sign in: %1").arg(_current->displayString()));
    _window->ensureVisible();
    if (wizard.exec() != QDialog::Accepted || !model.isComplete()) {
        _report.append(tr("%1: sign-in cancelled, not taken over.").arg(_current->displayString()));
        _current.reset();
        QTimer::singleShot(0, this, &LegacyClientImportController::importNextAccount);
        return;
    }

    // wie AccountsGuiController::runAccountWizard, statt der Standard-Ordner aber die des Fremd-Clients
    auto *builder = new NewAccountBuilder(model, this);
    connect(builder, &NewAccountBuilder::requestSetUpSyncFoldersForAccount, this,
        [this](AccountState *accountState, bool useVfs) { importFolders(accountState, useVfs ? SyncChoice::VirtualFiles : SyncChoice::SyncAll); });
    // „Ordner selbst wählen“: den Ordner-Assistenten öffnet importFolders() nur, wenn nichts übernommen wurde
    connect(builder, &NewAccountBuilder::requestLoadSpacesOnly, this, [this](AccountState *accountState) { importFolders(accountState, SyncChoice::Selective); });
    connect(builder, &NewAccountBuilder::unableToCompleteAccountCreation, this, &LegacyClientImportController::accountFailed);
    builder->buildAccount();
}

void LegacyClientImportController::setUpLikeNewAccount(AccountState *accountState, SyncChoice choice)
{
    FolderMan *folderMan = FolderMan::instance();
    if (choice == SyncChoice::Selective) {
        // nichts ungefragt synchronisieren: Spaces laden, die Ordner wählt der Nutzer im Ordner-Assistenten
        folderMan->setUpInitialSpaces(accountState);
        _folderWizardAccounts.append(accountState->account()->uuid());
    } else {
        folderMan->setUpInitialSyncFolders(accountState, choice == SyncChoice::VirtualFiles);
    }
}

void LegacyClientImportController::importFolders(AccountState *accountState, SyncChoice choice)
{
    if (!_current || !accountState || !accountState->account()) {
        return;
    }
    const LegacyClient::Account legacy = *std::exchange(_current, std::nullopt);
    Account *account = accountState->account();
    FolderMan *folderMan = FolderMan::instance();

    if (!LegacyClient::isSameAccount(legacy, account)) {
        _report.append(tr("%1: signed in as %2, the folders of the official client were not taken over.")
                           .arg(legacy.displayString(), account->displayNameWithHost()));
        setUpLikeNewAccount(accountState, choice);
    } else {
        // zuerst klassisch synchronisierte Ordner: ihre Dateien liegen lokal und sollen nicht erneut herunterkommen
        QList<LegacyClient::Folder> folders = legacy.folders;
        std::stable_partition(folders.begin(), folders.end(), [](const LegacyClient::Folder &folder) { return !folder.usesVirtualFiles(); });
        const QStringList officialFolders = LegacyClient::localFolders(_config);
        int imported = 0;
        for (const auto &folder : std::as_const(folders)) {
            const auto result = LegacyClient::importFolder(folderMan, accountState, folder, officialFolders);
            const QString source = QDir::toNativeSeparators(folder.localPath);
            switch (result.result) {
            case LegacyClient::FolderImport::Result::Adopted:
                _report.append(tr("%1: taken over, the files are recognized without downloading them again.").arg(source));
                break;
            case LegacyClient::FolderImport::Result::AdoptedWithoutJournal:
                _report.append(tr("%1: taken over; the synchronization state could not be read, the files are compared with the server once.").arg(source));
                break;
            case LegacyClient::FolderImport::Result::NewVirtualFilesFolder:
                _report.append(tr("%1: set up again with virtual files at %2; only placeholders are created.")
                                   .arg(source, QDir::toNativeSeparators(result.localPath)));
                break;
            case LegacyClient::FolderImport::Result::Skipped:
                _report.append(tr("%1: not taken over. %2").arg(source, result.reason));
                break;
            }
            if (result.result != LegacyClient::FolderImport::Result::Skipped) {
                ++imported;
            }
        }
        if (imported == 0) {
            // wie ein neu eingerichtetes Konto, damit es nicht ohne Ordner bleibt, und mit der Wahl aus dem Assistenten
            setUpLikeNewAccount(accountState, choice);
        }
        _takenOverFolders += imported;
    }
    QTimer::singleShot(0, this, &LegacyClientImportController::importNextAccount);
}

void LegacyClientImportController::accountFailed(const QString &error)
{
    if (_current) {
        _report.append(tr("%1: the account could not be created: %2").arg(_current->displayString(), error));
        _current.reset();
    }
    QTimer::singleShot(0, this, &LegacyClientImportController::importNextAccount);
}

void LegacyClientImportController::showReport()
{
    if (!_window || _report.isEmpty()) {
        return;
    }
    QMessageBox box(QMessageBox::Information, tr("Import finished"), _report.join(QLatin1Char('\n')), QMessageBox::Ok, _window);
    // doppelt synchronisiert würden nur übernommene Ordner. Hat der offizielle Client noch Konten, die hier fehlen (ohne die App
    // „OAuth2“, Anmeldung abgebrochen), braucht der Nutzer ihn dafür weiter: dann nicht zum Deinstallieren raten.
    if (_takenOverFolders > 0) {
        const bool everyAccountSetUp = std::all_of(_config.accounts.cbegin(), _config.accounts.cend(),
            [](const LegacyClient::Account &account) { return AccountManager::instance()->accountForLoginExists(account.url, account.davUser); });
        box.setInformativeText(everyAccountSetUp
                ? tr("Quit the official client for good (turn off its autostart or uninstall it), so that the folders are not synchronized twice.")
                : tr("The accounts that were not taken over are only set up in the official client. Before you start it again, remove the "
                     "accounts taken over from it, so that both programs do not synchronize the same folders."));
    }
    _window->ensureVisible();
    box.exec();
}

} // namespace OCC
