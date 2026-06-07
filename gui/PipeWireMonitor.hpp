/*
 * PipeWireMonitor.hpp — live telemetry for the Monitor tab via `pw-top`.
 *
 * parsePwTop() is PURE (operates on captured output) so it is unit testable.
 *
 * Copyright (C) 2026 PipeASIO contributors
 */
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QTimer;
class QProcess;

struct NodeStats {
    bool found = false;
    QString name;
    int quantum = 0;
    int rate = 0;
    double dspLoad = 0.0;
    long xruns = 0;
    QString state;
};

/* Parse `pw-top -b -n 1` output, returning stats for the first data row whose
 * trailing NAME contains `nodeNameSubstr`. Pure. */
NodeStats parsePwTop(const QByteArray &out, const QString &nodeNameSubstr);

class PipeWireMonitor : public QObject {
    Q_OBJECT
public:
    explicit PipeWireMonitor(QObject *parent = nullptr);
    ~PipeWireMonitor() override;

    void setTarget(const QString &nodeNameSubstr);

public slots:
    void start();
    void stop();

signals:
    void updated(const NodeStats &stats);

private slots:
    void poll();

private:
    QTimer *m_timer;
    QString m_target;
    bool    m_autoDiscover;   /* true => resolve our node via the marker prop */
};
