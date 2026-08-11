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

#include "fileproviderdomain.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QOperatingSystemVersion>
#include <QTimer>

#import <FileProvider/FileProvider.h>
#import <Foundation/Foundation.h>

Q_LOGGING_CATEGORY(lcFileProviderDomain, "sync.vfs.mac.domain", QtInfoMsg)

namespace {

QString fromNSString(NSString *string)
{
    return string ? QString::fromNSString(string) : QString();
}

/// Hop back to the Qt main thread: the File Provider callbacks arrive on
/// arbitrary queues, but our callers expect Qt-thread semantics.
void onMainThread(std::function<void()> fn)
{
    QTimer::singleShot(0, qApp, [fn = std::move(fn)] { fn(); });
}

/// -2001 (providerNotFound) means the extension is not registered at all,
/// -2011 (domainDisabled) means it is registered but switched off by the user.
OCC::Mac::DomainResult classify(NSError *error)
{
    using OCC::Mac::DomainResult;
    if (!error) {
        return DomainResult::Ok;
    }
    if ([error.domain isEqualToString:NSFileProviderErrorDomain]) {
        switch (error.code) {
        case -2011:
            return DomainResult::ExtensionDisabled;
        case -2001:
        case -2014:
            return DomainResult::ExtensionMissing;
        default:
            break;
        }
    }
    return DomainResult::Failed;
}

}

namespace OCC {
namespace Mac {

    bool fileProviderAvailable()
    {
        return QOperatingSystemVersion::current() >= QOperatingSystemVersion::MacOSVentura;
    }

    void registerDomain(const QString &identifier, const QString &displayName,
        std::function<void(DomainResult, const QString &)> callback)
    {
        if (!fileProviderAvailable()) {
            onMainThread([callback] { callback(DomainResult::Unsupported, {}); });
            return;
        }

        NSString *nsIdentifier = identifier.toNSString();
        NSFileProviderDomainIdentifier domainId = nsIdentifier;
        NSFileProviderDomain *domain =
            [[NSFileProviderDomain alloc] initWithIdentifier:domainId displayName:displayName.toNSString()];

        // Drop a previous incarnation first, otherwise a changed display name or
        // a stale domain from an earlier run keeps shadowing the new one.
        [NSFileProviderManager getDomainsWithCompletionHandler:^(NSArray<NSFileProviderDomain *> *domains, NSError *listError) {
            Q_UNUSED(listError)
            dispatch_group_t group = dispatch_group_create();
            for (NSFileProviderDomain *existing in domains) {
                if ([existing.identifier isEqualToString:domainId]) {
                    dispatch_group_enter(group);
                    [NSFileProviderManager removeDomain:existing completionHandler:^(NSError *) {
                        dispatch_group_leave(group);
                    }];
                }
            }
            dispatch_group_notify(group, dispatch_get_main_queue(), ^{
                [NSFileProviderManager addDomain:domain completionHandler:^(NSError *addError) {
                    const auto result = classify(addError);
                    const QString message = fromNSString(addError.localizedDescription);
                    if (addError) {
                        qCWarning(lcFileProviderDomain) << "Failed to register domain" << identifier << message;
                    } else {
                        qCInfo(lcFileProviderDomain) << "Registered File Provider domain" << identifier;
                    }
                    onMainThread([callback, result, message] { callback(result, message); });
                }];
            });
        }];
    }

    void unregisterDomain(const QString &identifier, std::function<void()> callback)
    {
        if (!fileProviderAvailable()) {
            if (callback) {
                onMainThread(callback);
            }
            return;
        }

        NSString *nsIdentifier = identifier.toNSString();
        [NSFileProviderManager getDomainsWithCompletionHandler:^(NSArray<NSFileProviderDomain *> *domains, NSError *) {
            dispatch_group_t group = dispatch_group_create();
            for (NSFileProviderDomain *existing in domains) {
                if ([existing.identifier isEqualToString:nsIdentifier]) {
                    dispatch_group_enter(group);
                    [NSFileProviderManager removeDomain:existing completionHandler:^(NSError *) {
                        dispatch_group_leave(group);
                    }];
                }
            }
            dispatch_group_notify(group, dispatch_get_main_queue(), ^{
                qCInfo(lcFileProviderDomain) << "Unregistered File Provider domain" << identifier;
                if (callback) {
                    onMainThread(callback);
                }
            });
        }];
    }

    QString domainResultMessage(DomainResult result)
    {
        switch (result) {
        case DomainResult::Ok:
            return {};
        case DomainResult::ExtensionDisabled:
            return QCoreApplication::translate("OCC::Mac",
                "Virtual files are switched off for this app. Open System Settings > General > "
                "Login Items & Extensions, select this app under Extensions and enable \"File Providers\".");
        case DomainResult::ExtensionMissing:
            return QCoreApplication::translate("OCC::Mac",
                "This build does not contain the virtual files extension.");
        case DomainResult::Unsupported:
            return QCoreApplication::translate("OCC::Mac",
                "Virtual files require macOS Ventura or newer.");
        case DomainResult::Failed:
            break;
        }
        return QCoreApplication::translate("OCC::Mac", "The virtual files folder could not be registered with macOS.");
    }

} // namespace Mac
} // namespace OCC
