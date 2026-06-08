/*
 * LoadHistogram.cpp — implementation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2026 PipeASIO contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.  See COPYING.GUI for the full license text.
 */
#include "LoadHistogram.hpp"

#include <QColor>
#include <QPainter>
#include <QPaintEvent>
#include <QRect>
#include <QString>

#include <algorithm>

namespace
{

/* Colour a bar by its load level: comfortable / warming up / at risk. */
QColor
colourFor(float load)
{
    if (load < 0.60f)
        return QColor(0x3f, 0xb9, 0x50); /* green */
    if (load < 0.85f)
        return QColor(0xd2, 0x99, 0x22); /* amber */
    return QColor(0xf8, 0x51, 0x49);     /* red */
}

} // namespace

LoadHistogram::LoadHistogram(QWidget *parent) : QWidget(parent)
{
    m_samples.reserve(kMaxSamples);
}

void
LoadHistogram::pushSample(double load)
{
    const float v = static_cast<float>(std::clamp(load, 0.0, 1.0));
    m_samples.append(v);
    if (m_samples.size() > kMaxSamples)
        m_samples.remove(0, m_samples.size() - kMaxSamples);
    m_current = v;
    m_active  = true;
    update();
}

void
LoadHistogram::setWaiting()
{
    if (!m_active)
        return; /* already idle — avoid needless repaints */
    m_active = false;
    update();
}

QSize
LoadHistogram::sizeHint() const
{
    return QSize(220, 64);
}

QSize
LoadHistogram::minimumSizeHint() const
{
    return QSize(120, 40);
}

void
LoadHistogram::paintEvent(QPaintEvent *)
{
    QPainter    p(this);
    const QRect r = rect();
    const int   w = r.width();
    const int   h = r.height();

    /* Panel background. */
    p.fillRect(r, QColor(0x1e, 0x22, 0x2a));

    /* Reference gridlines at 25 / 50 / 75 / 100 %. */
    p.setPen(QColor(0xff, 0xff, 0xff, 28));
    for (int pct = 25; pct <= 100; pct += 25)
    {
        const int y = h - 1 - (h - 1) * pct / 100;
        p.drawLine(0, y, w, y);
    }

    /* Bars: newest sample at the right edge, one column per sample.  When idle
     * the frozen history is dimmed so the live graph reads as "stopped". */
    const int n = std::min(static_cast<int>(m_samples.size()), w);
    if (n > 0)
    {
        p.setOpacity(m_active ? 1.0 : 0.35);
        for (int i = 0; i < n; ++i)
        {
            const float load = m_samples.at(m_samples.size() - n + i);
            const int   x    = w - n + i;
            const int   bh   = static_cast<int>(load * (h - 1));
            p.setPen(colourFor(load));
            p.drawLine(x, h - 1, x, h - 1 - bh);
        }
        p.setOpacity(1.0);
    }

    if (m_active)
    {
        p.setPen(QColor(0xc9, 0xd1, 0xd9));
        p.drawText(QRect(4, 2, w - 8, 16), Qt::AlignLeft | Qt::AlignTop,
                   QString::number(qRound(m_current * 100.0)) + QStringLiteral("%"));
    }
    else
    {
        p.setPen(QColor(0xff, 0xff, 0xff, 120));
        p.drawText(r, Qt::AlignCenter, QStringLiteral("waiting for audio..."));
    }
}
