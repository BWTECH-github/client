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

#pragma once

#include <QColor>
#include <QHash>
#include <QObject>
#include <QStyleHints>

class QAction;
class QWidget;

namespace OCC {

class ColorManager : public QObject
{
    Q_OBJECT

public:
    explicit ColorManager(QObject *parent);

    /**
     * Setzt den Markenakzent des Themes (Theme::accentColor/accentTextColor) auf die Palette des Fensters:
     * Highlight, Accent, Link und HighlightedText. Nur diese Rollen werden ausdrücklich gesetzt, alle anderen folgen
     * weiter der Anwendungspalette. Anders als QApplication::setPalette() lässt das den automatischen Wechsel
     * zwischen hellem und dunklem Farbschema (DC-348) unangetastet. Eigenständige Fenster (Dialoge) erben die
     * Palette ihres Elternfensters nicht und brauchen einen eigenen Aufruf.
     */
    static void applyAccent(QWidget *window);

    /**
     * Schriftfarbe für Fehlermeldungen passend zum aktuellen Farbschema: Theme::errorColor(), ohne Theme-Farbe
     * ein Rot mit mindestens 4,5:1 Kontrast (hell #B00020 auf Weiß, dunkel #FF6B6B auf #202020).
     */
    static QColor errorTextColor();

    /// Setzt errorTextColor() als Text- und Fensterschrift (QLabel nutzt WindowText, Eingabefelder Text).
    static void applyErrorColor(QWidget *widget);

    /// wie applyAccent(), setzt den Akzent zusätzlich nach jedem Wechsel des Farbschemas neu
    void keepAccent(QWidget *window);

    /// true, wenn der Fensterhintergrund der Anwendungspalette dunkel ist
    static bool isDarkPalette();

signals:
    void appColorsChanged();

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
};
}
