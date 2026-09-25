/*
 * LoadHistogram.cpp - implementation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Copyright (C) 2026 PipeASIO contributors
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "LoadHistogram.hpp"

#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPolygonF>
#include <QString>

#include <algorithm>
#include <cmath>

namespace
{

/* Levels: comfortable below 60 %, warming up below 85 %, at risk above. One
 * vertical gradient paints both the band and the line, so a colour always
 * marks the level it is drawn at. */
QLinearGradient
levelGradient(const QRectF &plot, int alpha)
{
    const QColor    green(0x3f, 0xb9, 0x50, alpha);
    const QColor    amber(0xd2, 0x99, 0x22, alpha);
    const QColor    red(0xf8, 0x51, 0x49, alpha);
    QLinearGradient gradient(plot.bottomLeft(), plot.topLeft());
    gradient.setColorAt(0.0, green);
    gradient.setColorAt(0.5999, green);
    gradient.setColorAt(0.60, amber);
    gradient.setColorAt(0.8499, amber);
    gradient.setColorAt(0.85, red);
    gradient.setColorAt(1.0, red);
    return gradient;
}

struct Slice
{
    qreal x;
    float low;
    float mean;
    float high;
};

} // namespace

LoadHistogram::LoadHistogram(QWidget *parent) : QWidget(parent)
{
    m_samples.reserve(kKeptSamples);
}

void
LoadHistogram::pushSample(double load)
{
    const float v = static_cast<float>(std::clamp(load, 0.0, 1.0));
    m_samples.append(v);
    ++m_count;
    if (m_samples.size() > kKeptSamples)
        m_samples.remove(0, m_samples.size() - kKeptSamples);
    m_current = v;
    m_active  = true;
    update();
}

void
LoadHistogram::setWaiting()
{
    if (!m_active)
        return; /* already idle - avoid needless repaints */
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
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF area = rect();
    p.setPen(Qt::NoPen);
    p.setBrush(palette().color(QPalette::Base));
    p.drawRoundedRect(area, 4, 4);

    /* Inset so the line stays whole at 0 % and 100 %. */
    const QRectF plot = area.adjusted(0, 2, 0, -2);
    const auto   yFor = [&plot](float load) { return plot.bottom() - load * plot.height(); };

    QColor grid = palette().color(QPalette::Text);
    grid.setAlpha(28);
    p.setPen(QPen(grid, 1));
    for (const float level : { 0.25f, 0.5f, 0.75f })
    {
        const qreal y = std::floor(yFor(level)) + 0.5;
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }

    /* Whole seconds only, aligned to the absolute sample count, so every
     * slice keeps its shape while it scrolls: from the second before the one
     * straddling the left edge, so the line enters from off screen, to the
     * last one completed. The second still filling in is not drawn. */
    const qreal  width      = area.width();
    const qint64 first      = m_count - m_samples.size();
    const qint64 edge       = m_count - kMaxSamples; /* sample index at x = 0 */
    const qint64 firstWhole = first + (kSliceSamples - first % kSliceSamples) % kSliceSamples;
    const qint64 from       = std::max(edge - edge % kSliceSamples - kSliceSamples, firstWhole);
    const auto   xFor       = [this, width](qreal index)
    { return width - (m_count - index) * width / kMaxSamples; };
    QVector<Slice> slices;
    slices.reserve(kKeptSamples / kSliceSamples + 2);
    for (qint64 start = from; start + kSliceSamples <= m_count; start += kSliceSamples)
    {
        Slice slice{ xFor(start + kSliceSamples / 2.0), 1.0f, 0.0f, 0.0f };
        for (qint64 i = start; i < start + kSliceSamples; ++i)
        {
            const float v = m_samples.at(static_cast<int>(i - first));
            slice.low     = std::min(slice.low, v);
            slice.high    = std::max(slice.high, v);
            slice.mean += v;
        }
        slice.mean /= kSliceSamples;
        slices.append(slice);
    }
    if (!slices.isEmpty())
    {
        /* Run the ends out to where the history starts and to the right edge. */
        Slice oldest = slices.first();
        oldest.x     = xFor(from);
        slices.prepend(oldest);
        Slice newest = slices.last();
        newest.x     = width;
        slices.append(newest);

        QPolygonF band;
        QPolygonF line;
        for (const Slice &s : slices)
        {
            band << QPointF(s.x, yFor(s.high));
            line << QPointF(s.x, yFor(s.mean));
        }
        for (auto s = slices.crbegin(); s != slices.crend(); ++s)
            band << QPointF(s->x, yFor(s->low));

        /* When idle the frozen history is dimmed so the graph reads as stopped. */
        p.setOpacity(m_active ? 1.0 : 0.35);
        p.setPen(Qt::NoPen);
        p.setBrush(levelGradient(plot, 90));
        p.drawPolygon(band);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QBrush(levelGradient(plot, 255)), 1.5, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
        p.drawPolyline(line);
        p.setOpacity(1.0);
    }

    const QRectF text = area.adjusted(6, 4, -6, -4);
    if (m_active)
    {
        QFont font = p.font();
        font.setBold(true);
        p.setFont(font);
        p.setPen(palette().color(QPalette::Text));
        p.drawText(text, Qt::AlignLeft | Qt::AlignTop,
                   QString::number(qRound(m_current * 100.0)) + QStringLiteral("%"));
    }
    else
    {
        p.setPen(palette().color(QPalette::Text));
        p.drawText(text, Qt::AlignCenter, QStringLiteral("waiting for audio..."));
    }
}
