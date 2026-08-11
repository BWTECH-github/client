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
#pragma once

#include <QString>

#include <functional>

namespace OCC {
namespace Mac {

    /** Result of registering a File Provider domain. */
    enum class DomainResult {
        Ok,
        /** The extension exists but the user has not switched it on in
         *  System Settings > Login Items & Extensions > (app) > File Providers.
         *  Without that the domain stays dormant, so we surface it to the user. */
        ExtensionDisabled,
        /** No File Provider extension is registered for this app at all,
         *  typically a build without BUILD_MAC_FILE_PROVIDER. */
        ExtensionMissing,
        /** macOS is too old (File Provider virtual files need Ventura). */
        Unsupported,
        Failed,
    };

    /** Whether this macOS version can host our File Provider extension. */
    bool fileProviderAvailable();

    /** Register (or refresh) the domain for a synced folder.
     *
     * Asynchronous: the callback runs on the main thread once macOS has
     * answered. Calling it again for the same identifier is safe - an existing
     * domain is removed first so a restart cannot leave duplicates behind.
     */
    void registerDomain(const QString &identifier, const QString &displayName,
        std::function<void(DomainResult, const QString &errorMessage)> callback);

    /** Remove the domain again, e.g. when vfs is switched off or the folder goes away. */
    void unregisterDomain(const QString &identifier, std::function<void()> callback = {});

    /** Human readable hint for a non-Ok result, ready to show in the GUI. */
    QString domainResultMessage(DomainResult result);

} // namespace Mac
} // namespace OCC
