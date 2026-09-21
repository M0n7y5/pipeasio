/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QStringList>
#include <QWidget>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProcess;
class QProgressBar;
class QPushButton;
class QTreeWidget;

class InstallationsTab : public QWidget
{
    Q_OBJECT
  public:
    explicit InstallationsTab(QWidget *parent = nullptr);
    ~InstallationsTab() override;
    bool busy() const;

  private:
    void        refreshTargets();
    void        refreshReleases();
    void        addPrefix();
    void        previewInstall(bool repair);
    void        checkTarget();
    void        removeTarget();
    void        updateActions();
    void        readOutput();
    void        consumeLine(const QByteArray &line);
    void        finish(int exitCode, bool crashed);
    void        fail(const QString &message);
    void        run(const QStringList &arguments, bool mutation,
                    std::function<void(const QJsonObject &)> completed);
    QJsonObject selectedTarget() const;
    QStringList installArguments(const QString &command) const;
    QString     targetDescription() const;

    QTreeWidget                             *m_targets         = nullptr;
    QLabel                                  *m_guidance        = nullptr;
    QComboBox                               *m_releases        = nullptr;
    QCheckBox                               *m_include32       = nullptr;
    QLabel                                  *m_details         = nullptr;
    QLabel                                  *m_status          = nullptr;
    QPlainTextEdit                          *m_log             = nullptr;
    QProgressBar                            *m_progress        = nullptr;
    QPushButton                             *m_refresh         = nullptr;
    QPushButton                             *m_refreshReleases = nullptr;
    QPushButton                             *m_add             = nullptr;
    QPushButton                             *m_install         = nullptr;
    QPushButton                             *m_repair          = nullptr;
    QPushButton                             *m_check           = nullptr;
    QPushButton                             *m_remove          = nullptr;
    QProcess                                *m_process         = nullptr;
    QByteArray                               m_output;
    QJsonObject                              m_result;
    QString                                  m_protocolError;
    bool                                     m_running        = false;
    bool                                     m_mutation       = false;
    bool                                     m_receivedResult = false;
    std::function<void(const QJsonObject &)> m_completed;
};
