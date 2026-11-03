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

// built with -fobjc-arc (see CMakeLists.txt)

#include "fileproviderdomain.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMap>
#include <QOperatingSystemVersion>
#include <QSaveFile>

#import <FileProvider/FileProvider.h>
#import <Foundation/Foundation.h>
#import <Security/Security.h>

Q_LOGGING_CATEGORY(lcFileProviderDomain, "sync.vfs.mac.domain", QtInfoMsg)

namespace {

using OCC::Mac::DomainResult;

QString fromNSString(NSString *string)
{
    return string ? QString::fromNSString(string) : QString();
}

/// Hop back to the Qt main thread: the File Provider callbacks arrive on
/// arbitrary queues, but our callers expect Qt-thread semantics.
void onMainThread(std::function<void()> fn)
{
    QMetaObject::invokeMethod(qApp, [fn = std::move(fn)] { fn(); }, Qt::QueuedConnection);
}

NSString *extensionPath()
{
    return [NSBundle.mainBundle.builtInPlugInsPath stringByAppendingPathComponent:@"FileProviderExt.appex"];
}

/// The app group the extension declares (NSExtensionFileProviderDocumentGroup in
/// its Info.plist). Read from the embedded extension, so both sides always agree
/// on the container and the keychain service, whatever team signed the build.
QString appGroup()
{
    static const QString group = [] {
        NSBundle *extension = [NSBundle bundleWithPath:extensionPath()];
        NSDictionary *info = extension.infoDictionary[@"NSExtension"];
        if (![info isKindOfClass:NSDictionary.class]) {
            return QString();
        }
        NSString *value = info[@"NSExtensionFileProviderDocumentGroup"];
        return [value isKindOfClass:NSString.class] ? fromNSString(value) : QString();
    }();
    return group;
}

/// Same names in FileProviderExt/WebDavBackend.swift (AppGroup)
NSString *keychainService()
{
    return [appGroup().toNSString() stringByAppendingString:@".fileprovider"];
}

QString credentialsRequestName()
{
    return appGroup() + QStringLiteral(".fileprovider.credentials");
}

QString containerFile(const QString &name)
{
    if (appGroup().isEmpty()) {
        return {};
    }
    NSURL *container = [NSFileManager.defaultManager containerURLForSecurityApplicationGroupIdentifier:appGroup().toNSString()];
    if (!container) {
        return {};
    }
    QDir().mkpath(fromNSString(container.path));
    return QDir(fromNSString(container.path)).filePath(name);
}

QString settingsFile(const QString &identifier)
{
    return containerFile(QStringLiteral("FileProviderConfig-%1.json").arg(identifier));
}

DomainResult classify(NSError *error)
{
    if (!error) {
        return DomainResult::Ok;
    }
    if ([error.domain isEqualToString:NSFileProviderErrorDomain]) {
        switch (error.code) {
        case NSFileProviderErrorDomainDisabled:
            return DomainResult::ExtensionDisabled;
        case NSFileProviderErrorProviderNotFound:
        case NSFileProviderErrorProviderTranslocated:
            return DomainResult::ExtensionMissing;
        default:
            // -2014 (no launchable extension) is only named from macOS 14.1 on
            if (error.code == -2014) {
                return DomainResult::ExtensionMissing;
            }
            break;
        }
    }
    return DomainResult::Failed;
}

/// Look the registered domain up by its identifier; nil if macOS has none.
void withDomain(const QString &identifier, void (^body)(NSFileProviderDomain *domain, NSError *error))
{
    NSString *wanted = identifier.toNSString();
    [NSFileProviderManager getDomainsWithCompletionHandler:^(NSArray<NSFileProviderDomain *> *domains, NSError *error) {
        NSFileProviderDomain *found = nil;
        for (NSFileProviderDomain *domain in domains) {
            if ([domain.identifier isEqualToString:wanted]) {
                found = domain;
                break;
            }
        }
        body(found, error);
    }];
}

void logError(const char *what, const QString &identifier, NSError *error)
{
    if (error) {
        qCWarning(lcFileProviderDomain) << what << identifier << error.code << fromNSString(error.localizedDescription);
    }
}

QMap<int, std::function<void()>> &credentialRequestHandlers()
{
    static QMap<int, std::function<void()>> handlers;
    return handlers;
}

void credentialRequestPosted(CFNotificationCenterRef, void *, CFNotificationName, const void *, CFDictionaryRef)
{
    onMainThread([] {
        qCInfo(lcFileProviderDomain) << "The File Provider extension asks for fresh credentials";
        const auto handlers = credentialRequestHandlers();
        for (const auto &handler : handlers) {
            handler();
        }
    });
}

}

namespace OCC {
namespace Mac {

    bool fileProviderAvailable()
    {
        if (QOperatingSystemVersion::current() < QOperatingSystemVersion::MacOSVentura) {
            return false;
        }
        // a build without the extension (BUILD_MAC_FILE_PROVIDER off) must not offer virtual files
        return [NSFileManager.defaultManager fileExistsAtPath:extensionPath()] && !appGroup().isEmpty();
    }

    bool writeDomainSettings(const QString &identifier, const DomainSettings &settings)
    {
        const QString path = settingsFile(identifier);
        if (path.isEmpty()) {
            qCWarning(lcFileProviderDomain) << "No app group container for the File Provider settings";
            return false;
        }
        const QJsonObject json{
            {QStringLiteral("webDavUrl"), settings.rootUrl.toString(QUrl::FullyEncoded)},
            {QStringLiteral("metaUrl"), settings.metaUrl.toString(QUrl::FullyEncoded)},
            {QStringLiteral("remoteRoot"), settings.remoteRoot},
            {QStringLiteral("username"), settings.user},
            {QStringLiteral("userAgent"), settings.userAgent},
        };
        const QByteArray data = QJsonDocument(json).toJson(QJsonDocument::Compact);
        QFile current(path);
        if (current.open(QIODevice::ReadOnly) && current.readAll() == data) {
            return true;
        }
        current.close();
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            qCWarning(lcFileProviderDomain) << "Could not write" << path << file.errorString();
            return false;
        }
        return true;
    }

    void removeDomainSettings(const QString &identifier)
    {
        for (const QString &name : {QStringLiteral("FileProviderConfig-%1.json"), QStringLiteral("FileProviderStore-%1.json")}) {
            const QString path = containerFile(name.arg(identifier));
            if (!path.isEmpty()) {
                QFile::remove(path);
            }
        }
    }

#pragma clang diagnostic push
// SecAccess/SecTrustedApplication are the only way to grant a second program access to a
// login keychain item without a provisioning profile (Developer ID builds have none; the
// data protection keychain refuses app groups that no profile authorizes, -34018).
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    bool storeAuthorization(const QString &identifier, const QByteArray &authorization)
    {
        if (appGroup().isEmpty() || authorization.isEmpty()) {
            return false;
        }
        NSDictionary *query = @{
            (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
            (__bridge id)kSecAttrService : keychainService(),
            (__bridge id)kSecAttrAccount : identifier.toNSString(),
        };
        // a fresh item each time, so its access list always names the extension that is installed now
        SecItemDelete((__bridge CFDictionaryRef)query);

        NSMutableArray *trusted = [NSMutableArray array];
        SecTrustedApplicationRef client = nullptr;
        if (SecTrustedApplicationCreateFromPath(nullptr, &client) == errSecSuccess && client) {
            [trusted addObject:(__bridge_transfer id)client];
        }
        SecTrustedApplicationRef extension = nullptr;
        const OSStatus extensionStatus = SecTrustedApplicationCreateFromPath(extensionPath().fileSystemRepresentation, &extension);
        if (extensionStatus != errSecSuccess || !extension) {
            qCWarning(lcFileProviderDomain) << "Could not reference the File Provider extension for the keychain:" << extensionStatus;
            return false;
        }
        [trusted addObject:(__bridge_transfer id)extension];

        NSString *appName = NSBundle.mainBundle.infoDictionary[@"CFBundleName"];
        NSString *label = [NSString stringWithFormat:@"%@ (File Provider)", appName.length ? appName : @"owncloud.online"];
        SecAccessRef access = nullptr;
        const OSStatus accessStatus = SecAccessCreate((__bridge CFStringRef)label, (__bridge CFArrayRef)trusted, &access);
        if (accessStatus != errSecSuccess || !access) {
            qCWarning(lcFileProviderDomain) << "Could not create the keychain access list:" << accessStatus;
            return false;
        }

        NSMutableDictionary *attributes = [query mutableCopy];
        attributes[(__bridge id)kSecValueData] = authorization.toNSData();
        attributes[(__bridge id)kSecAttrAccess] = (__bridge_transfer id)access;
        attributes[(__bridge id)kSecAttrLabel] = label;
        const OSStatus status = SecItemAdd((__bridge CFDictionaryRef)attributes, nullptr);
        if (status != errSecSuccess) {
            qCWarning(lcFileProviderDomain) << "Could not store the File Provider credentials in the keychain:" << status;
            return false;
        }
        return true;
    }
#pragma clang diagnostic pop

    void removeAuthorization(const QString &identifier)
    {
        if (appGroup().isEmpty()) {
            return;
        }
        NSDictionary *query = @{
            (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
            (__bridge id)kSecAttrService : keychainService(),
            (__bridge id)kSecAttrAccount : identifier.toNSString(),
        };
        SecItemDelete((__bridge CFDictionaryRef)query);
    }

    void registerDomain(const QString &identifierRef, const QString &displayNameRef,
        std::function<void(DomainResult, const QString &)> callback)
    {
        // copies: blocks capture C++ references as references, and macOS answers long after the caller returned
        const QString identifier = identifierRef;
        const QString displayName = displayNameRef;
        if (!fileProviderAvailable()) {
            onMainThread([callback] { callback(DomainResult::Unsupported, {}); });
            return;
        }

        NSString *name = displayName.toNSString();
        withDomain(identifier, ^(NSFileProviderDomain *existing, NSError *) {
            // Keep a registered domain as it is: removing and re-adding it would discard what
            // macOS keeps for it, including local changes the extension has not uploaded yet.
            if (existing && [existing.displayName isEqualToString:name]) {
                qCInfo(lcFileProviderDomain) << "File Provider domain already registered" << identifier;
                onMainThread([callback] { callback(DomainResult::Ok, {}); });
                return;
            }
            // new, or only the display name changed (addDomain updates it in place)
            NSFileProviderDomain *domain = [[NSFileProviderDomain alloc] initWithIdentifier:identifier.toNSString() displayName:name];
            [NSFileProviderManager addDomain:domain
                           completionHandler:^(NSError *addError) {
                               const auto result = classify(addError);
                               const QString message = fromNSString(addError.localizedDescription);
                               if (addError) {
                                   logError("Failed to register domain", identifier, addError);
                               } else {
                                   qCInfo(lcFileProviderDomain) << "Registered File Provider domain" << identifier << displayName;
                               }
                               onMainThread([callback, result, message] { callback(result, message); });
                           }];
        });
    }

    void queryDomain(const QString &identifierRef, std::function<void(DomainResult, const QString &)> callback)
    {
        const QString identifier = identifierRef; // a copy for the blocks
        if (!fileProviderAvailable()) {
            onMainThread([callback] { callback(DomainResult::Unsupported, {}); });
            return;
        }
        withDomain(identifier, ^(NSFileProviderDomain *domain, NSError *listError) {
            if (!domain) {
                logError("Could not list File Provider domains", identifier, listError);
                onMainThread([callback] { callback(DomainResult::Failed, {}); });
                return;
            }
            const bool userEnabled = domain.userEnabled;
            NSFileProviderManager *manager = [NSFileProviderManager managerForDomain:domain];
            [manager getUserVisibleURLForItemIdentifier:NSFileProviderRootContainerItemIdentifier
                                      completionHandler:^(NSURL *url, NSError *error) {
                                          auto result = classify(error);
                                          if (result == DomainResult::Ok && !userEnabled) {
                                              result = DomainResult::ExtensionDisabled;
                                          }
                                          if (error) {
                                              logError("Domain not usable", identifier, error);
                                          }
                                          const QString path = fromNSString(url.path);
                                          onMainThread([callback, result, path] { callback(result, path); });
                                      }];
        });
    }

    void unregisterDomain(const QString &identifierRef, std::function<void()> callback)
    {
        const QString identifier = identifierRef; // a copy for the blocks
        if (!fileProviderAvailable()) {
            if (callback) {
                onMainThread(callback);
            }
            return;
        }

        withDomain(identifier, ^(NSFileProviderDomain *existing, NSError *) {
            if (!existing) {
                if (callback) {
                    onMainThread(callback);
                }
                return;
            }
            // macOS keeps files whose changes never reached the server and tells us where
            [NSFileProviderManager removeDomain:existing
                                           mode:NSFileProviderDomainRemovalModePreserveDirtyUserData
                              completionHandler:^(NSURL *preservedLocation, NSError *error) {
                                  logError("Failed to remove domain", identifier, error);
                                  if (preservedLocation) {
                                      qCWarning(lcFileProviderDomain)
                                          << "Unsynced files of the removed domain are kept in" << fromNSString(preservedLocation.path);
                                  }
                                  qCInfo(lcFileProviderDomain) << "Unregistered File Provider domain" << identifier;
                                  if (callback) {
                                      onMainThread(callback);
                                  }
                              }];
        });
    }

    void removeStaleDomains()
    {
        if (!fileProviderAvailable()) {
            return;
        }
        [NSFileProviderManager getDomainsWithCompletionHandler:^(NSArray<NSFileProviderDomain *> *domains, NSError *error) {
            logError("Could not list File Provider domains", {}, error);
            for (NSFileProviderDomain *domain in domains) {
                const QString identifier = fromNSString(domain.identifier);
                const QString settings = settingsFile(identifier);
                if (settings.isEmpty() || QFileInfo::exists(settings)) {
                    continue;
                }
                qCInfo(lcFileProviderDomain) << "Removing File Provider domain without a folder" << identifier << fromNSString(domain.displayName);
                [NSFileProviderManager removeDomain:domain
                                               mode:NSFileProviderDomainRemovalModePreserveDirtyUserData
                                  completionHandler:^(NSURL *preservedLocation, NSError *removeError) {
                                      logError("Failed to remove domain", identifier, removeError);
                                      if (preservedLocation) {
                                          qCWarning(lcFileProviderDomain)
                                              << "Unsynced files of the removed domain are kept in" << fromNSString(preservedLocation.path);
                                      }
                                  }];
                removeDomainSettings(identifier);
                removeAuthorization(identifier);
            }
        }];
    }

    void setDomainSuspended(const QString &identifierRef, const QString &reason, bool temporary)
    {
        const QString identifier = identifierRef; // a copy for the blocks
        if (!fileProviderAvailable()) {
            return;
        }
        NSString *localizedReason = reason.toNSString();
        withDomain(identifier, ^(NSFileProviderDomain *domain, NSError *) {
            if (!domain) {
                return;
            }
            NSFileProviderManager *manager = [NSFileProviderManager managerForDomain:domain];
            if (localizedReason.length == 0) {
                if (!domain.disconnected) {
                    return;
                }
                [manager reconnectWithCompletionHandler:^(NSError *error) {
                    logError("Could not reconnect domain", identifier, error);
                    if (!error) {
                        qCInfo(lcFileProviderDomain) << "Reconnected File Provider domain" << identifier;
                    }
                }];
                return;
            }
            const NSFileProviderManagerDisconnectionOptions options = temporary ? NSFileProviderManagerDisconnectionOptionsTemporary : 0;
            [manager disconnectWithReason:localizedReason
                                  options:options
                        completionHandler:^(NSError *error) {
                            logError("Could not disconnect domain", identifier, error);
                            if (!error) {
                                qCInfo(lcFileProviderDomain) << "Disconnected File Provider domain" << identifier << fromNSString(localizedReason);
                            }
                        }];
        });
    }

    void signalChanges(const QString &identifierRef)
    {
        const QString identifier = identifierRef; // a copy for the blocks
        if (!fileProviderAvailable()) {
            return;
        }
        withDomain(identifier, ^(NSFileProviderDomain *domain, NSError *) {
            if (!domain) {
                return;
            }
            [[NSFileProviderManager managerForDomain:domain] signalEnumeratorForContainerItemIdentifier:NSFileProviderWorkingSetContainerItemIdentifier
                                                                                      completionHandler:^(NSError *error) {
                                                                                          logError("Could not signal changes", identifier, error);
                                                                                      }];
        });
    }

    void signalErrorsResolved(const QString &identifierRef)
    {
        const QString identifier = identifierRef; // a copy for the blocks
        if (!fileProviderAvailable()) {
            return;
        }
        withDomain(identifier, ^(NSFileProviderDomain *domain, NSError *) {
            if (!domain) {
                return;
            }
            NSFileProviderManager *manager = [NSFileProviderManager managerForDomain:domain];
            for (NSNumber *code in @[ @(NSFileProviderErrorNotAuthenticated), @(NSFileProviderErrorServerUnreachable) ]) {
                NSError *resolved = [NSError errorWithDomain:NSFileProviderErrorDomain code:code.integerValue userInfo:nil];
                [manager signalErrorResolved:resolved
                           completionHandler:^(NSError *error) {
                               logError("Could not signal a resolved error", identifier, error);
                           }];
            }
            [manager signalEnumeratorForContainerItemIdentifier:NSFileProviderWorkingSetContainerItemIdentifier
                                              completionHandler:^(NSError *error) {
                                                  logError("Could not signal changes", identifier, error);
                                              }];
        });
    }

    int observeCredentialRequests(std::function<void()> handler)
    {
        static int nextToken = 0;
        static bool observing = false;
        if (!observing && !appGroup().isEmpty()) {
            observing = true;
            CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(), nullptr, credentialRequestPosted,
                (__bridge CFStringRef)credentialsRequestName().toNSString(), nullptr, CFNotificationSuspensionBehaviorDeliverImmediately);
        }
        const int token = ++nextToken;
        credentialRequestHandlers().insert(token, std::move(handler));
        return token;
    }

    void stopObservingCredentialRequests(int token)
    {
        credentialRequestHandlers().remove(token);
    }

} // namespace Mac
} // namespace OCC
