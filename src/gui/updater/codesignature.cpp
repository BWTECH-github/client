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

#include "updater/codesignature.h"

#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <qt_windows.h>
// Reihenfolge wichtig: wintrust/softpub nach windows.h
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <string>
#endif

namespace OCC::CodeSignature {

#ifdef Q_OS_WIN
namespace {
// CN des Antragstellers (flags 0) oder des Ausstellers (CERT_NAME_ISSUER_FLAG), so wie er im Zertifikat steht
QString commonName(PCCERT_CONTEXT cert, DWORD flags)
{
    void *oid = const_cast<char *>(szOID_COMMON_NAME);
    const DWORD size = CertGetNameStringW(cert, CERT_NAME_ATTR_TYPE, flags, oid, nullptr, 0);
    if (size <= 1) {
        return {};
    }
    std::wstring name(size, L'\0');
    CertGetNameStringW(cert, CERT_NAME_ATTR_TYPE, flags, oid, name.data(), size);
    name.resize(size - 1);
    return QString::fromStdWString(name);
}
}
#endif

QString requiredSigner()
{
    // Antragsteller-CN unseres DigiCert-Code-Signing-Zertifikats (vgl. admin/win/codesign.ps1)
    return QStringLiteral("BW-TECH GmbH, IT Service f\u00fcr Systemintegration & Datensicherheit");
}

QString requiredIssuer()
{
    return QStringLiteral("DigiCert");
}

bool signerMatches(const QString &signer, const QString &issuer)
{
    // Genau unser CN: ein Teilstück wie in "XYZ BW-TECH GmbH Holding" reicht nicht. NFC, damit ein
    // zerlegt kodiertes "ü" (u + Trema) im Zertifikat als dasselbe Zeichen gilt.
    return signer.normalized(QString::NormalizationForm_C) == requiredSigner() && issuer.contains(requiredIssuer());
}

Result verifyFile(const QString &path)
{
    Result result;
    if (!QFileInfo(path).isFile()) {
        result.error = QStringLiteral("file not found");
        return result;
    }
#ifdef Q_OS_WIN
    const std::wstring nativePath = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).toStdWString();

    WINTRUST_FILE_INFO fileInfo = {};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = nativePath.c_str();

    WINTRUST_DATA data = {};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &fileInfo;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const HWND noUi = static_cast<HWND>(INVALID_HANDLE_VALUE);
    const LONG status = WinVerifyTrust(noUi, &action, &data);
    if (status == ERROR_SUCCESS) {
        // Signierer aus der geprüften Kette lesen
        if (CRYPT_PROVIDER_DATA *provData = WTHelperProvDataFromStateData(data.hWVTStateData)) {
            if (CRYPT_PROVIDER_SGNR *signer = WTHelperGetProvSignerFromChain(provData, 0, FALSE, 0)) {
                if (CRYPT_PROVIDER_CERT *cert = WTHelperGetProvCertFromChain(signer, 0)) {
                    result.signer = commonName(cert->pCert, 0);
                    result.issuer = commonName(cert->pCert, CERT_NAME_ISSUER_FLAG);
                }
            }
        }
        result.valid = !result.signer.isEmpty();
        if (!result.valid) {
            result.error = QStringLiteral("signer certificate not readable");
        }
    } else {
        result.error = QStringLiteral("WinVerifyTrust failed: 0x%1").arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'));
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(noUi, &action, &data);
#else
    result.error = QStringLiteral("Authenticode verification is only available on Windows");
#endif
    return result;
}

bool verifyUpdatePackage(const QString &path, QString *error)
{
    const Result r = verifyFile(path);
    QString reason;
    if (!r.valid) {
        reason = r.error;
    } else if (!signerMatches(r.signer, r.issuer)) {
        reason = QStringLiteral("unexpected signer: %1 (issuer: %2)").arg(r.signer, r.issuer);
    }
    if (error) {
        *error = reason;
    }
    return reason.isEmpty();
}

} // namespace OCC::CodeSignature
