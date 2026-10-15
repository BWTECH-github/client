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

#include "gui/owncloudguilib.h"

#include <QString>

namespace OCC::CodeSignature {

// Ergebnis der Authenticode-Prüfung einer Datei
struct Result
{
    bool valid = false; // Signatur und Zertifikatskette gültig
    QString signer; // CN des Antragstellers im Signaturzertifikat
    QString issuer; // CN des Ausstellers des Signaturzertifikats
    QString error; // Grund bei Fehlschlag
};

// CN, den das Signaturzertifikat eines Updates genau tragen muss
OWNCLOUDGUI_EXPORT QString requiredSigner();

// Text, den der CN des Ausstellers enthalten muss
OWNCLOUDGUI_EXPORT QString requiredIssuer();

// Prüft die eingebettete Authenticode-Signatur (nur Windows, sonst immer ungültig)
OWNCLOUDGUI_EXPORT Result verifyFile(const QString &path);

// true, wenn signer genau requiredSigner() ist und issuer requiredIssuer() enthält
OWNCLOUDGUI_EXPORT bool signerMatches(const QString &signer, const QString &issuer);

// Gültige Signatur UND signerMatches(); sonst false mit Grund in error
OWNCLOUDGUI_EXPORT bool verifyUpdatePackage(const QString &path, QString *error = nullptr);

} // namespace OCC::CodeSignature
