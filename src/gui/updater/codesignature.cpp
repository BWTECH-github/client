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

#include <QFileInfo>

#ifdef Q_OS_WIN
#include <qt_windows.h>
// Reihenfolge wichtig: wintrust/softpub nach windows.h
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#endif

namespace OCC::CodeSignature {

QString requiredSigner()
{
    // Antragsteller unseres Code-Signing-Zertifikats (vgl. admin/win/codesign.ps1)
    return QStringLiteral("BW-TECH GmbH");
}

bool signerMatches(const QString &signer, const QString &required)
{
    // signtool /n und Get-AuthenticodeSignature -like vergleichen ohne
    // Groß-/Kleinschreibung; leere Angaben passen nie
    return !required.trimmed().isEmpty() && signer.contains(required.trimmed(), Qt::CaseInsensitive);
}

Result verifyFile(const QString &path)
{
    Result result;
    if (!QFileInfo(path).isFile()) {
        result.error = QStringLiteral("file not found");
        return result;
    }
#ifdef Q_OS_WIN
    const std::wstring nativePath = QFileInfo(path).absoluteFilePath().replace(QLatin1Char('/'), QLatin1Char('\')).toStdWString();

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
                    wchar_t name[512] = {};
                    if (CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 512) > 1) {
                        result.signer = QString::fromWCharArray(name);
                    }
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
    } else if (!signerMatches(r.signer, requiredSigner())) {
        reason = QStringLiteral("unexpected signer: %1").arg(r.signer);
    }
    if (error) {
        *error = reason;
    }
    return reason.isEmpty();
}

} // namespace OCC::CodeSignature
