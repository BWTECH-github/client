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

#include "fileproviderdomain.h"
#include "filesystem.h"
#include "syncfileitem.h"

#include <QCryptographicHash>
#include <QLoggingCategory>

using namespace OCC;

Q_LOGGING_CATEGORY(lcVfsMac, "sync.vfs.mac", QtInfoMsg)

VfsMac::VfsMac(QObject *parent)
    : Vfs(parent)
{
}

VfsMac::~VfsMac() = default;

Vfs::Mode VfsMac::mode() const
{
    return Vfs::MacFileProvider;
}

void VfsMac::stop()
{
    // Leave the domain registered: it must survive a client restart the same way
    // the Windows sync root does, otherwise the Finder entry would disappear
    // whenever the client quits.
}

void VfsMac::unregisterFolder()
{
    if (_domainIdentifier.isEmpty()) {
        return;
    }
    Mac::unregisterDomain(_domainIdentifier);
    _domainIdentifier.clear();
}

bool VfsMac::socketApiPinStateActionsShown() const
{
    // The Finder context menu is provided by the File Provider extension.
    return false;
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
    // Dataless files are fully owned by the File Provider layer; from the
    // engine's point of view local files are regular files.
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

void VfsMac::fileStatusChanged(const QString &, SyncFileStatus)
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

    // A domain identifier has to be stable across restarts (it keys the Finder
    // entry and the extension's state) and must not leak the account or path,
    // so derive it from the folder's remote identity.
    const QByteArray seed = (params.baseUrl().toString() + QLatin1Char('|') + params.remotePath).toUtf8();
    _domainIdentifier = QString::fromLatin1(QCryptographicHash::hash(seed, QCryptographicHash::Sha1).toHex());

    const QString displayName = params.providerDisplayName.isEmpty()
        ? QStringLiteral("owncloud.online")
        : params.providerDisplayName;

    Mac::registerDomain(_domainIdentifier, displayName, [this](Mac::DomainResult result, const QString &detail) {
        if (result == Mac::DomainResult::Ok) {
            Q_EMIT started();
            return;
        }
        const QString message = Mac::domainResultMessage(result);
        qCWarning(lcVfsMac) << "Could not register File Provider domain:" << message << detail;
        Q_EMIT error(message);
        // Still report the start as finished so the folder does not hang in a
        // half-configured state; syncing simply behaves like plain sync.
        Q_EMIT started();
    });
}
