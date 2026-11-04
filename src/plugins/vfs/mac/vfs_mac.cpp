/*
 * Copyright (C) by BW-Tech GmbH
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

#include "vfs_mac.h"

#include "account.h"
#include "common/utility.h"
#include "configfile.h"
#include "creds/abstractcredentials.h"
#include "filesystem.h"
#include "networkjobs.h"
#include "syncfileitem.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QPointer>

using namespace OCC;
using namespace std::chrono_literals;

Q_LOGGING_CATEGORY(lcVfsMac, "sync.vfs.mac", QtInfoMsg)

namespace {
// so many polls in a row without usable credentials (the token is being refreshed, or it
// could not be) and the extension loses its copy as well
constexpr int pollsUntilCredentialsDropped = 2;

/// the folder's remote path without slashes: "" for the whole account, else "Projekte/2026"
QString remoteRoot(const VfsSetupParams &params)
{
    QString root = params.remotePath;
    while (root.startsWith(QLatin1Char('/'))) {
        root.remove(0, 1);
    }
    while (root.endsWith(QLatin1Char('/'))) {
        root.chop(1);
    }
    return root;
}
}

// here and not in fileproviderdomain.mm: lupdate reads no Objective-C++
QString OCC::Mac::domainResultMessage(Mac::DomainResult result)
{
    switch (result) {
    case DomainResult::Ok:
        return {};
    case DomainResult::ExtensionDisabled:
        return VfsMac::tr("The files are not shown in the Finder yet: macOS needs your permission. Open System Settings > General > "
                          "Login Items & Extensions, click the info button next to %1 under \"Extensions\" and switch on \"File Providers\".")
            .arg(QCoreApplication::applicationName());
    case DomainResult::ExtensionMissing:
        return VfsMac::tr("macOS does not find the virtual files extension of this app. Move the app to the Applications folder and start it again.");
    case DomainResult::Unsupported:
        return VfsMac::tr("Virtual files require macOS Ventura or newer.");
    case DomainResult::Failed:
        break;
    }
    return VfsMac::tr("The virtual files folder could not be registered with macOS.");
}

VfsMac::VfsMac(QObject *parent)
    : Vfs(parent)
{
    _pollTimer.setInterval(ConfigFile().remotePollInterval(30s));
    connect(&_pollTimer, &QTimer::timeout, this, &VfsMac::poll);
}

VfsMac::~VfsMac()
{
    if (_credentialRequestToken) {
        Mac::stopObservingCredentialRequests(_credentialRequestToken);
    }
}

Vfs::Mode VfsMac::mode() const
{
    return Vfs::MacFileProvider;
}

void VfsMac::stop()
{
    // Leave the domain registered: it must survive a client restart the same way
    // the Windows sync root does, otherwise the Finder entry would disappear
    // whenever the client quits. The extension keeps its credentials until they expire.
    _pollTimer.stop();
}

void VfsMac::unregisterFolder()
{
    _pollTimer.stop();
    if (_domainIdentifier.isEmpty()) {
        return;
    }
    qCInfo(lcVfsMac) << "Removing File Provider domain" << _domainIdentifier;
    dropAuthorization();
    Mac::removeDomainSettings(_domainIdentifier);
    Mac::unregisterDomain(_domainIdentifier);
    _domainIdentifier.clear();
}

bool VfsMac::socketApiPinStateActionsShown() const
{
    // "Download Now" / "Remove Download" come from the Finder for File Provider items.
    return false;
}

void VfsMac::fileProviderRefresh()
{
    if (_domainIdentifier.isEmpty()) {
        return;
    }
    if (_registered && _suspendReason.isEmpty()) {
        Mac::signalChanges(_domainIdentifier);
    }
    poll();
}

void VfsMac::fileProviderSetSuspended(const QString &reason, bool temporary)
{
    if (_suspendApplied && reason == _suspendReason && temporary == _suspendTemporary) {
        return;
    }
    _suspendReason = reason;
    _suspendTemporary = temporary;
    _suspendApplied = false;
    if (_registered) {
        applySuspension();
    }
}

void VfsMac::applySuspension()
{
    if (_domainIdentifier.isEmpty()) {
        return;
    }
    _suspendApplied = true;
    qCInfo(lcVfsMac) << "File Provider domain" << _domainIdentifier << (_suspendReason.isEmpty() ? QStringLiteral("connected") : _suspendReason);
    Mac::setDomainSuspended(_domainIdentifier, _suspendReason, _suspendTemporary);
    if (_suspendReason.isEmpty()) {
        poll();
    } else if (!_suspendTemporary) {
        // signed out: the extension must not go on with the old credentials
        dropAuthorization();
    }
}

QString VfsMac::fileProviderProblem(bool *needsSystemSettings) const
{
    if (needsSystemSettings) {
        *needsSystemSettings = _state == Mac::DomainResult::ExtensionDisabled;
    }
    return Mac::domainResultMessage(_state);
}

QString VfsMac::fileProviderPath() const
{
    return _userVisiblePath;
}

Result<void, QString> VfsMac::createPlaceholder(const SyncFileItem &)
{
    // Placeholders are materialized by the extension, not by the sync engine.
    return {};
}

bool VfsMac::needsMetadataUpdate(const SyncFileItem &)
{
    return false;
}

bool VfsMac::isDehydratedPlaceholder(const QString &)
{
    // Dataless files are fully owned by the File Provider layer.
    return false;
}

bool VfsMac::statTypeVirtualFile(csync_file_stat_t *, void *)
{
    return false;
}

bool VfsMac::setPinState(const QString &folderPath, PinState state)
{
    qCDebug(lcVfsMac) << "setPinState" << folderPath << state;
    return setPinStateInDb(folderPath, state);
}

Optional<PinState> VfsMac::pinState(const QString &folderPath)
{
    return pinStateInDb(folderPath);
}

Vfs::AvailabilityResult VfsMac::availability(const QString &folderPath)
{
    return availabilityInDb(folderPath);
}

void VfsMac::onFileStatusChanged(const QString &, SyncFileStatus)
{
}

Result<Vfs::ConvertToPlaceholderResult, QString> VfsMac::updateMetadata(const SyncFileItem &item, const QString &filePath, const QString &replacesFile)
{
    Q_UNUSED(replacesFile)

    if (!item.isDirectory()) {
        const bool isReadOnly = !item._remotePerm.isNull() && !item._remotePerm.hasPermission(RemotePermissions::CanWrite);
        FileSystem::setFileReadOnlyWeak(filePath, isReadOnly);
    }
    return { ConvertToPlaceholderResult::Ok };
}

void VfsMac::startImpl(const VfsSetupParams &params)
{
    qCInfo(lcVfsMac) << "Starting macOS File Provider vfs for" << params.filesystemPath;

    // The identifier keys the Finder entry and the extension's state, so it must stay the same
    // across restarts and server moves, differ per folder and not show the account or path:
    // the account's uuid and the folder's local path (unique per folder), hashed.
    const QByteArray seed = params.account->uuid().toByteArray(QUuid::WithoutBraces) + '|' + params.filesystemPath.toUtf8();
    _domainIdentifier = QString::fromLatin1(QCryptographicHash::hash(seed, QCryptographicHash::Sha1).toHex());

    const QString root = remoteRoot(params);
    // Der Finder stellt den Namen der App selbst voran („owncloud.online - …“). Der Domain-Name nennt
    // deshalb nur das Konto, wie der offizielle Client (Benutzer@Server), und bei einem Unterordner oder
    // Space zusätzlich dessen Namen. Mit dem App-Namen hieß der Eintrag „owncloud.online - owncloud.online“.
    QString displayName = params.account->displayNameWithHost();
    if (!root.isEmpty()) {
        displayName = tr("%1 - %2").arg(displayName, root.section(QLatin1Char('/'), -1));
    }

    Mac::DomainSettings settings;
    settings.rootUrl = Utility::concatUrlPath(params.baseUrl(), params.remotePath);
    settings.metaUrl = Utility::concatUrlPath(params.account->url(), QStringLiteral("remote.php/dav/meta"));
    settings.remoteRoot = root;
    settings.user = params.account->davUser();
    settings.userAgent = QString::fromUtf8(Utility::userAgentString());
    if (!Mac::writeDomainSettings(_domainIdentifier, settings)) {
        qCWarning(lcVfsMac) << "The File Provider extension gets no connection settings for" << params.filesystemPath;
    } else {
        static bool staleDomainsRemoved = false;
        if (!staleDomainsRemoved) {
            staleDomainsRemoved = true;
            Mac::removeStaleDomains();
        }
    }

    if (!_credentialRequestToken) {
        // an extension got a 401: refresh (the account's credentials do that on the request) and hand
        // over what works now
        QPointer<VfsMac> guarded(this);
        _credentialRequestToken = Mac::observeCredentialRequests([guarded] {
            if (guarded) {
                guarded->poll();
            }
        });
    }

    QPointer<VfsMac> self(this);
    Mac::registerDomain(_domainIdentifier, displayName, [self](Mac::DomainResult result, const QString &detail) {
        if (!self) {
            return;
        }
        self->_registered = true;
        self->_state = result;
        if (result != Mac::DomainResult::Ok) {
            qCWarning(lcVfsMac) << "Could not register File Provider domain:" << Mac::domainResultMessage(result) << detail;
        }
        // The folder must not hang half configured: it is started either way and shows the
        // problem (fileProviderProblem()) until macOS lets the domain work.
        Q_EMIT self->started();
        self->applySuspension();
        self->_pollTimer.start();
    });
}

void VfsMac::poll()
{
    if (_domainIdentifier.isEmpty() || !_registered) {
        return;
    }
    checkDomain();
    if (!_suspendReason.isEmpty()) {
        // signed out or paused: the domain is disconnected, nothing to hand over
        return;
    }

    Account *account = params().account;
    AbstractCredentials *credentials = account ? account->credentials() : nullptr;
    if (!credentials || !credentials->ready()) {
        // the token is being refreshed, or could not be: the extension must not go on with the old one for long
        if (!_mirroredAuthorization.isEmpty() && ++_pollsWithoutCredentials >= pollsUntilCredentialsDropped) {
            qCInfo(lcVfsMac) << "No usable credentials, the File Provider extension loses its copy";
            dropAuthorization();
        }
        return;
    }
    _pollsWithoutCredentials = 0;

    if (_pollRunning) {
        return;
    }
    _pollRunning = true;
    auto *job = new RequestEtagJob(account, params().baseUrl(), params().remotePath, this);
    connect(job, &QObject::destroyed, this, [this] { _pollRunning = false; });
    connect(job, &RequestEtagJob::finishedWithError, this, [this] { _lastPollFailed = true; });
    connect(job, &RequestEtagJob::finishedWithoutError, this, [this, job] {
        // The request went through, so the header it carried is what the extension needs.
        if (job->hasReply()) {
            mirrorAuthorization(job->reply()->request().rawHeader(QByteArrayLiteral("Authorization")));
        }
        if (_lastPollFailed) {
            // the server is reachable again: what failed meanwhile is retried now
            _lastPollFailed = false;
            Mac::signalErrorsResolved(_domainIdentifier);
        }
        const QString etag = job->etag();
        if (!etag.isEmpty() && etag != _lastEtag) {
            // the first answer after the start, too: the extension may have missed changes meanwhile
            qCDebug(lcVfsMac) << "Server changes for File Provider domain" << _domainIdentifier;
            _lastEtag = etag;
            Mac::signalChanges(_domainIdentifier);
        }
    });
    job->start();
}

void VfsMac::checkDomain()
{
    QPointer<VfsMac> self(this);
    const QString identifier = _domainIdentifier;
    Mac::queryDomain(identifier, [self, identifier](Mac::DomainResult result, const QString &path) {
        if (!self || self->_domainIdentifier != identifier) {
            return;
        }
        if (!path.isEmpty()) {
            self->_userVisiblePath = path;
        }
        if (result == self->_state) {
            return;
        }
        qCInfo(lcVfsMac) << "File Provider domain" << identifier << "is now"
                         << (result == Mac::DomainResult::Ok ? QStringLiteral("usable") : Mac::domainResultMessage(result));
        self->_state = result;
        if (result == Mac::DomainResult::Ok) {
            Mac::signalErrorsResolved(identifier);
        }
        // the folder shows the new state
        Q_EMIT self->needSync();
    });
}

void VfsMac::mirrorAuthorization(const QByteArray &authorization)
{
    if (authorization.isEmpty() || authorization == _mirroredAuthorization) {
        return;
    }
    if (Mac::storeAuthorization(_domainIdentifier, authorization)) {
        qCInfo(lcVfsMac) << "Credentials for File Provider domain" << _domainIdentifier << "updated";
        _mirroredAuthorization = authorization;
        // operations that failed with "not authenticated" are retried now
        Mac::signalErrorsResolved(_domainIdentifier);
    }
}

void VfsMac::dropAuthorization()
{
    if (_domainIdentifier.isEmpty()) {
        return;
    }
    Mac::removeAuthorization(_domainIdentifier);
    _mirroredAuthorization.clear();
}
