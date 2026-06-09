/*
 * LoadHistogram.hpp — a rolling DSP-load history graph for the Monitor tab.
 *
 * Each pushed sample (load in [0, 1]) adds a column at the right edge; older
 * samples scroll left.  Bars are colour-coded green/amber/red by level, the
 * current value is shown as text, and when no audio is active the history
 * freezes (dimmed) behind a "waiting for audio..." overlay.
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
#pragma once

#include <QVector>
#include <QWidget>

class LoadHistogram : public QWidget
{
    Q_OBJECT
  public:
    explicit LoadHistogram(QWidget *parent = nullptr);

    /* Record one DSP-load sample (clamped to [0, 1]) and mark the graph active. */
    void pushSample(double load);
    /* Audio stopped: keep the history but stop scrolling and dim it. */
    void setWaiting();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    static constexpr int kMaxSamples = 600;

    QVector<float> m_samples;
    float          m_current = 0.0f;
    bool           m_active  = false;
};
