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

#include "filesystem.h"
#include "syncfileitem.h"

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
    // TODO(M1): disconnect the NSFileProviderDomain for this folder.
}

void VfsMac::unregisterFolder()
{
    // TODO(M1): remove the NSFileProviderDomain so no stale Finder sidebar
    // entry survives removing/switching the folder.
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
    // TODO(M1): register the NSFileProviderDomain for this folder and only
    // then emit started(). Registration must be idempotent - startImpl() may
    // be called again before a previous start finished (see Vfs docs).
    Q_EMIT started();
}
