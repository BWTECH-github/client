/*
 * Copyright (C) Lisa Reese <lisa.reese@kiteworks.com>
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

#include "colormanager.h"

#include <QAction>
#include <QGuiApplication>
#include <QPalette>
#include <QWidget>

#include "libsync/theme.h"
#include "resources/iconresources.h"

namespace OCC {

ColorManager::ColorManager(QObject *parent)
    : QObject{parent}
{
    qGuiApp->installEventFilter(this);
}

bool ColorManager::eventFilter(QObject *object, QEvent *event)
{
    if (object == qGuiApp && event->type() == QEvent::ApplicationPaletteChange) {
        IconResources::handleSystemStyleChanged();
        emit appColorsChanged();
    }
    return false;
}

bool ColorManager::isDarkPalette()
{
    // nach dem tatsächlichen Fensterhintergrund entscheiden: manche Plattformen melden
    // Qt::ColorScheme::Unknown zu einer dunklen Palette
    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
}

void ColorManager::applyAccent(QWidget *window)
{
    if (!window)
        return;
    const bool dark = isDarkPalette();
    const QColor accent = Theme::instance()->accentColor(dark);
    const QColor accentText = Theme::instance()->accentTextColor(dark);
    if (!accent.isValid() || !accentText.isValid())
        return;
    // bewusst eine leere Palette: nur die vier Rollen gelten danach als ausdrücklich gesetzt
    QPalette accentPalette;
    accentPalette.setColor(QPalette::Highlight, accent);
    accentPalette.setColor(QPalette::Accent, accent);
    accentPalette.setColor(QPalette::Link, accent);
    accentPalette.setColor(QPalette::HighlightedText, accentText);
    window->setPalette(accentPalette);
}

void ColorManager::keepAccent(QWidget *window)
{
    applyAccent(window);
    connect(this, &ColorManager::appColorsChanged, window, [window] { applyAccent(window); });
}
}
