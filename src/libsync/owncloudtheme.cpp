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

#include "iconresources.h"

#include <QCoreApplication>
#include <QIcon>
#include <QString>
#include <QVariant>


namespace OCC {

ownCloudTheme::ownCloudTheme()
    : Theme()
{
}

QColor ownCloudTheme::wizardHeaderBackgroundColor() const
{
    return QColor(QStringLiteral("#00E4BD"));
}

QColor ownCloudTheme::wizardHeaderTitleColor() const
{
    return QColor(QStringLiteral("#262B45"));
}

// owncloud.online-Mint: #00E4BD ist auf hellem Grund zu schwach (1,64:1 auf Weiß), deshalb im hellen
// Schema das dunkle Mint #007A66 (5,3:1 auf Weiß, weiße Schrift darauf 5,3:1). Im dunklen Schema
// #00E4BD (10:1 auf #202020) mit Navy-Schrift #041E42 (10,1:1). Geprüft in test/testtheme.cpp.
QColor ownCloudTheme::accentColor(bool darkScheme) const
{
    return darkScheme ? QColor(0x00, 0xE4, 0xBD) : QColor(0x00, 0x7A, 0x66);
}

QColor ownCloudTheme::accentTextColor(bool darkScheme) const
{
    return darkScheme ? QColor(0x04, 0x1E, 0x42) : QColor(0xFF, 0xFF, 0xFF);
}

QColor ownCloudTheme::errorColor(bool darkScheme) const
{
    // hell 7,3:1 auf Weiß, dunkel 5,9:1 auf #202020
    return darkScheme ? QColor(0xFF, 0x6B, 0x6B) : QColor(0xB0, 0x00, 0x20);
}

QColor ownCloudTheme::selectionColor() const
{
    // 25 % Mint über dem Zeilenhintergrund; die Zeilenschrift bleibt in der normalen Textfarbe
    return QColor(0x00, 0xE4, 0xBD, 64);
}

QString ownCloudTheme::wizardHeaderLogoName() const
{
    return "wizard_logo";
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
