/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
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

#include "owncloudtheme.h"

#include "config.h"
#include "iconresources.h"

#include <QCoreApplication>
#include <QIcon>
#include <QString>
#include <QVariant>


namespace OCC {

namespace {
    // Einzige Quelle der Markenfarben; alle Kontraste prüft test/testtheme.cpp.
    // Mint #00E4BD: Markenfarbe, nur auf dunklem Grund für Text geeignet (1,64:1 auf Weiß).
    constexpr QRgb brandMint = 0x00E4BD;
    // dunkles Mint #007A66 für Akzente im hellen Schema (5,3:1 auf Weiß)
    constexpr QRgb brandMintDark = 0x007A66;
    // Navy #041E42 als Schrift auf Mint (10,1:1)
    constexpr QRgb brandNavy = 0x041E42;
    constexpr QRgb errorLight = 0xB00020; // 7,3:1 auf Weiß
    constexpr QRgb errorDark = 0xFF6B6B; // 5,9:1 auf #202020
}

ownCloudTheme::ownCloudTheme()
    : Theme()
{
}

QColor ownCloudTheme::wizardHeaderBackgroundColor() const
{
    return QColor(brandMint);
}

QColor ownCloudTheme::wizardHeaderTitleColor() const
{
    return QColor(brandNavy);
}

// Im hellen Schema das dunkle Mint (weiße Schrift darauf 5,3:1), im dunklen Schema Mint (10:1 auf #202020)
// mit Navy-Schrift.
QColor ownCloudTheme::accentColor(bool darkScheme) const
{
    return QColor(darkScheme ? brandMint : brandMintDark);
}

QColor ownCloudTheme::accentTextColor(bool darkScheme) const
{
    return darkScheme ? QColor(brandNavy) : QColor(Qt::white);
}

QColor ownCloudTheme::errorColor(bool darkScheme) const
{
    return QColor(darkScheme ? errorDark : errorLight);
}

QColor ownCloudTheme::selectionColor() const
{
    // 25 % Mint über dem Zeilenhintergrund; die Zeilenschrift bleibt in der normalen Textfarbe
    QColor selection(brandMint);
    selection.setAlpha(64);
    return selection;
}

QString ownCloudTheme::wizardHeaderLogoName() const
{
    return "wizard_logo";
}

QString ownCloudTheme::applicationIconName() const
{
    // Upstream leitet den Namen aus APPLICATION_SHORTNAME ab ("owncloud.online-icon"),
    // die Branding-Ressource heißt aber nach APPLICATION_ICON_NAME (owncloud-online-icon.svg).
    // Ohne diese Überschreibung findet getBrandingIcon() nichts, und Fenster-, Tray- und
    // Update-Symbole fallen auf ein Platzhaltersymbol zurück.
    return QStringLiteral(APPLICATION_ICON_NAME "-icon");
}

QString ownCloudTheme::aboutIconName() const
{
    return "oc-image-about";
}

bool ownCloudTheme::moveToTrashDefaultValue() const
{
    // for the vanilla ownCloud client move-to-trash option is enabled by default
    return true;
}

bool ownCloudTheme::allowSystemConfigOverrides() const
{
    return true;
}
}
