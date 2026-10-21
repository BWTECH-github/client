/*
 * Copyright (C) 2026 by BW-Tech GmbH (owncloud.online)
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

#include "wizardpagelayout.h"

#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWizardPage>

namespace OCC {

void setScrollableWizardPageLayout(QWizardPage *page, QLayout *contentLayout)
{
    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->addWidget(createScrollableContent(page, contentLayout));
}

QScrollArea *createScrollableContent(QWidget *parent, QLayout *contentLayout)
{
    auto *content = new QWidget;
    content->setLayout(contentLayout);
    // so breit wie die Seite, auch wenn ein Text ohne Umbruch breiter wäre: sonst würde der ganze Inhalt breiter und jede
    // Meldung rechts abgeschnitten. Nur senkrecht wird gerollt, Texte brechen um.
    content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    auto *scrollArea = new QScrollArea(parent);
    scrollArea->setObjectName(QStringLiteral("WizardPageScrollArea"));
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setWidgetResizable(true);
    scrollArea->setWidget(content);
    // erst nach setWidget(), das die Füllung einschaltet: Die Seite (Farbe aus dem Theme) scheint durch
    content->setAutoFillBackground(false);
    scrollArea->viewport()->setAutoFillBackground(false);

    // QWizard gibt auf einer neuen Seite dem ersten fokussierbaren Element den Fokus, und das soll das Eingabefeld bleiben: Mit
    // der Tastatur erreichbar ist der Bereich nur, solange es etwas zu rollen gibt
    scrollArea->setFocusPolicy(Qt::NoFocus);
    QObject::connect(scrollArea->verticalScrollBar(), &QScrollBar::rangeChanged, scrollArea,
        [scrollArea](int, int maximum) { scrollArea->setFocusPolicy(maximum > 0 ? Qt::StrongFocus : Qt::NoFocus); });

    return scrollArea;
}

}
