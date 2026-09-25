/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "InstallationsTab.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <utility>

static QString
displayTargetValue(const QString &value)
{
    static const std::pair<const char *, const char *> labels[]
            = { { "wine", "Wine" },
                { "faugus", "Faugus" },
                { "bottles", "Bottles" },
                { "bottles-flatpak", "Bottles (Flatpak)" },
                { "not-installed", "Not installed" },
                { "installed", "Installed" },
                { "needs-repair", "Needs attention" },
                { "unsupported", "Unavailable" } };
    for (const auto &[key, label] : labels)
        if (value == QLatin1String(key))
            return QCoreApplication::translate("InstallationsTab", label);
    return value;
}

/* Drawn rather than taken from the icon theme: themes name status icons
 * inconsistently, and a bare Fusion session may have no icon theme at all. */
static QIcon
statusIcon(const QString &status, const QPalette &palette, qreal ratio)
{
    QPixmap pixmap(QSize(12, 12) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor muted = palette.color(QPalette::PlaceholderText);
    if (status == QLatin1String("not-installed"))
    {
        painter.setPen(QPen(muted, 1.5));
        painter.drawEllipse(QRectF(2.75, 2.75, 6.5, 6.5));
        return QIcon(pixmap);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(status == QLatin1String("installed")      ? QColor(0x27, 0xae, 0x60)
                     : status == QLatin1String("needs-repair") ? QColor(0xf6, 0x74, 0x00)
                                                               : muted);
    painter.drawEllipse(QRectF(2, 2, 8, 8));
    return QIcon(pixmap);
}

InstallationsTab::InstallationsTab(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    auto *heading = new QVBoxLayout;
    heading->setSpacing(4);
    auto *header    = new QHBoxLayout;
    auto *title     = new QLabel(tr("Wine prefixes"), this);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.25);
    titleFont.setBold(true);
    title->setFont(titleFont);
    header->addWidget(title, 1);
    const QIcon refreshIcon = QIcon::fromTheme(QStringLiteral("view-refresh"),
                                               style()->standardIcon(QStyle::SP_BrowserReload));
    m_refresh               = new QPushButton(refreshIcon, tr("&Refresh targets"), this);
    m_add = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("&Add prefix..."),
                            this);
    header->addWidget(m_refresh);
    header->addWidget(m_add);
    heading->addLayout(header);
    auto *intro = new QLabel(tr("Manage official PipeASIO releases in Faugus, Bottles, or a custom "
                                "Wine prefix. Close the launcher and Windows applications before "
                                "changing an installation. Discovery does not start Wine."),
                             this);
    intro->setWordWrap(true);
    heading->addWidget(intro);
    layout->addLayout(heading);

    m_targets = new QTreeWidget(this);
    m_targets->setObjectName(QStringLiteral("installationTargets"));
    m_targets->setAccessibleName(tr("Wine installation targets"));
    m_targets->setHeaderLabels({ tr("Target"), tr("Launcher"), tr("Status"), tr("Version") });
    m_targets->setRootIsDecorated(false);
    m_targets->setAlternatingRowColors(true);
    m_targets->setSelectionMode(QAbstractItemView::SingleSelection);
    m_targets->setUniformRowHeights(true);
    m_targets->header()->setStretchLastSection(false);
    m_targets->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_targets->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_targets->setMinimumHeight(96);
    layout->addWidget(m_targets, 1);

    m_details    = new QGroupBox(this);
    auto *fields = new QFormLayout(m_details);
    /* Paths have no break opportunities, so a label would pin the window's
     * minimum width to the longest one. A frameless read-only field scrolls. */
    const auto pathField = [this](const QString &name)
    {
        auto *field = new QLineEdit(m_details);
        field->setReadOnly(true);
        field->setFrame(false);
        field->setAccessibleName(name);
        QPalette palette = field->palette();
        palette.setBrush(QPalette::Base, Qt::transparent);
        field->setPalette(palette);
        return field;
    };
    const auto textField = [this](const QString &name)
    {
        auto *field = new QLabel(m_details);
        field->setAccessibleName(name);
        field->setTextFormat(Qt::PlainText);
        /* A wrapped label stays at its narrow size hint in a form unless it
         * expands. setWordWrap() then adds height-for-width to this policy. */
        field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        field->setWordWrap(true);
        field->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        return field;
    };
    m_prefix = pathField(tr("Selected target prefix"));
    fields->addRow(tr("Prefix:"), m_prefix);
    m_runner = pathField(tr("Selected target runner"));
    fields->addRow(tr("Runner:"), m_runner);
    /* Label-less form rows lose height-for-width, so the conditional notes
     * share the status field instead of taking rows of their own. */
    auto *state = new QVBoxLayout;
    m_state     = textField(tr("Selected target status"));
    m_note      = textField(tr("Selected target notes"));
    m_guidance  = textField(tr("Manual prefix launch instructions"));
    m_guidance->setVisible(false);
    for (auto *label : { m_state, m_note, m_guidance })
        state->addWidget(label);
    auto *stateLabel = new QLabel(tr("Status:"), m_details);
    /* Level with the first line of text; the form pads labels for framed fields. */
    stateLabel->setAlignment(Qt::AlignLeading | Qt::AlignTop);
    fields->addRow(stateLabel, state);

    auto *releases     = new QHBoxLayout;
    auto *releaseLabel = new QLabel(tr("&Release:"), m_details);
    m_releases         = new QComboBox(m_details);
    m_releases->setAccessibleName(tr("PipeASIO release"));
    m_releases->addItem(tr("Latest stable release"), QString());
    releaseLabel->setBuddy(m_releases);
    m_refreshReleases = new QToolButton(m_details);
    m_refreshReleases->setIcon(refreshIcon);
    m_refreshReleases->setText(tr("Refresh re&leases"));
    m_refreshReleases->setToolTip(tr("Refresh releases"));
    m_refreshReleases->setToolButtonStyle(Qt::ToolButtonIconOnly);
    releases->addWidget(m_releases, 1);
    releases->addWidget(m_refreshReleases);
    fields->addRow(releaseLabel, releases);

    m_include32 = new QCheckBox(tr("Include e&xperimental 32-bit support"), m_details);
    m_include32->setToolTip(
            tr("Requires Wine's new WoW64 mode. Both frontends are checked, and a failed "
               "check restores the previous installation."));
    fields->addRow(static_cast<QWidget *>(nullptr), m_include32);

    auto *actions = new QHBoxLayout;
    m_remove  = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-delete")), tr("Re&move..."),
                                m_details);
    m_check   = new QPushButton(tr("&Check..."), m_details);
    m_repair  = new QPushButton(tr("Re&pair..."), m_details);
    m_install = new QPushButton(tr("&Install..."), m_details);
    actions->addWidget(m_remove);
    actions->addStretch();
    for (auto *button : { m_check, m_repair, m_install })
        actions->addWidget(button);
    fields->addRow(actions);
    layout->addWidget(m_details);

    auto *statusRow = new QHBoxLayout;
    m_status        = new QLabel(tr("Discovering targets..."), this);
    m_status->setObjectName(QStringLiteral("installationStatus"));
    m_status->setAccessibleName(tr("Manager operation status"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    statusRow->addWidget(m_status, 1);
    m_progress = new QProgressBar(this);
    m_progress->setAccessibleName(tr("Manager operation progress"));
    m_progress->setRange(0, 0);
    m_progress->setTextVisible(false);
    m_progress->setMaximumWidth(160);
    m_progress->setVisible(false);
    statusRow->addWidget(m_progress);
    m_logToggle = new QToolButton(this);
    m_logToggle->setText(tr("&Operation log"));
    m_logToggle->setCheckable(true);
    m_logToggle->setAutoRaise(true);
    m_logToggle->setArrowType(Qt::RightArrow);
    m_logToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    statusRow->addWidget(m_logToggle, 0, Qt::AlignTop);
    layout->addLayout(statusRow);
    m_log = new QPlainTextEdit(this);
    m_log->setAccessibleName(tr("Manager operation log"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    m_log->setMaximumHeight(120);
    m_log->setVisible(false);
    layout->addWidget(m_log);
    connect(m_logToggle, &QToolButton::toggled, this,
            [this](bool shown)
            {
                m_log->setVisible(shown);
                m_logToggle->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
            });
    m_process = new QProcess(this);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &InstallationsTab::readOutput);
    connect(m_process, &QProcess::readyReadStandardError, this, [this]
            { m_log->appendPlainText(QString::fromUtf8(m_process->readAllStandardError())); });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status)
            { finish(code, status == QProcess::CrashExit); });
    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error)
            {
                if (error == QProcess::FailedToStart)
                    fail(tr("Could not start the manager backend: %1. Install the manager package "
                            "or set PIPEASIO_MANAGER_BACKEND to its executable, then refresh "
                            "targets.")
                                 .arg(m_process->errorString()));
            });
    connect(m_refresh, &QPushButton::clicked, this, &InstallationsTab::refreshTargets);
    connect(m_refreshReleases, &QToolButton::clicked, this, &InstallationsTab::refreshReleases);
    connect(m_add, &QPushButton::clicked, this, &InstallationsTab::addPrefix);
    connect(m_install, &QPushButton::clicked, this, [this] { previewInstall(false); });
    connect(m_repair, &QPushButton::clicked, this, [this] { previewInstall(true); });
    connect(m_check, &QPushButton::clicked, this, &InstallationsTab::checkTarget);
    connect(m_remove, &QPushButton::clicked, this, &InstallationsTab::removeTarget);
    connect(m_targets, &QTreeWidget::itemSelectionChanged, this,
            [this]
            {
                const auto metadata = selectedTarget().value(QStringLiteral("metadata")).toObject();
                m_include32->setChecked(metadata.value(QStringLiteral("include_32")).toBool());
                const QString launcher
                        = metadata.value(QStringLiteral("launch_command")).toString();
                m_guidance->setVisible(!launcher.isEmpty());
                m_guidance->setText(
                        launcher.isEmpty()
                                ? QString()
                                : tr("Launch this prefix's Windows applications through:\n%1\n"
                                     "Direct Wine launches do not inherit its PipeASIO library "
                                     "path.")
                                          .arg(launcher));
                updateActions();
            });
    updateActions();
    QTimer::singleShot(0, this, &InstallationsTab::refreshTargets);
}

InstallationsTab::~InstallationsTab()
{
    disconnect(m_targets, nullptr, this, nullptr);
    disconnect(m_process, nullptr, this, nullptr);
    if (m_process->state() != QProcess::NotRunning)
    {
        m_process->terminate();
        if (!m_process->waitForFinished(500))
        {
            m_process->kill();
            m_process->waitForFinished(500);
        }
    }
}

bool
InstallationsTab::busy() const
{
    return m_mutation;
}

QJsonObject
InstallationsTab::selectedTarget() const
{
    const auto *item = m_targets->currentItem();
    return item ? item->data(0, Qt::UserRole).toJsonObject() : QJsonObject();
}

QString
InstallationsTab::targetDescription() const
{
    const auto    target  = selectedTarget();
    const QString version = target.value(QStringLiteral("version")).toString();
    QString       text
            = tr("Target: %1\nPrefix: %2\nRunner: %3\nStatus: %4\nInstalled version: %5")
                      .arg(target.value(QStringLiteral("name")).toString(),
                           target.value(QStringLiteral("prefix")).toString(),
                           target.value(QStringLiteral("runner")).toString(),
                           displayTargetValue(target.value(QStringLiteral("status")).toString()),
                           version.isEmpty() ? tr("Not installed") : version);
    const QString error = target.value(QStringLiteral("error")).toString();
    if (!error.isEmpty())
        text += QStringLiteral("\n") + error;
    const QString note = target.value(QStringLiteral("metadata"))
                                 .toObject()
                                 .value(QStringLiteral("note"))
                                 .toString();
    if (!note.isEmpty())
        text += QStringLiteral("\n") + note;
    return text;
}

void
InstallationsTab::updateActions()
{
    const auto target   = selectedTarget();
    const bool selected = !target.isEmpty();
    const bool supported
            = target.value(QStringLiteral("status")).toString() != QStringLiteral("unsupported");
    const bool installed      = !target.value(QStringLiteral("version")).toString().isEmpty();
    const bool pendingRemoval = target.value(QStringLiteral("metadata"))
                                        .toObject()
                                        .value(QStringLiteral("removal_pending"))
                                        .toBool();
    m_targets->setEnabled(!m_running);
    m_refresh->setEnabled(!m_running);
    m_refreshReleases->setEnabled(!m_running);
    m_add->setEnabled(!m_running);
    m_releases->setEnabled(!m_running);
    m_include32->setEnabled(!m_running);
    m_install->setEnabled(!m_running && selected && supported && !pendingRemoval);
    m_install->setText(installed ? tr("&Update...") : tr("&Install..."));
    m_repair->setEnabled(!m_running && selected && supported && installed && !pendingRemoval);
    m_check->setEnabled(!m_running && selected && supported && installed && !pendingRemoval);
    m_remove->setEnabled(!m_running && selected && supported && installed);
    QString title = target.value(QStringLiteral("name")).toString();
    m_details->setTitle(selected ? title.replace(QLatin1Char('&'), QStringLiteral("&&"))
                                 : tr("No target selected"));
    const auto showPath = [](QLineEdit *field, const QString &path)
    {
        field->setText(path);
        field->setCursorPosition(0);
        field->setToolTip(path);
    };
    showPath(m_prefix, target.value(QStringLiteral("prefix")).toString());
    showPath(m_runner, target.value(QStringLiteral("runner")).toString());
    const QString status  = displayTargetValue(target.value(QStringLiteral("status")).toString());
    const QString version = target.value(QStringLiteral("version")).toString();
    m_state->setText(!selected           ? tr("Select a target to see its prefix and runner.")
                     : version.isEmpty() ? status
                                         : QStringLiteral("%1 \u00b7 %2").arg(status, version));
    QStringList notes = {
        target.value(QStringLiteral("error")).toString(),
        target.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("note")).toString()
    };
    notes.removeAll(QString());
    m_note->setText(notes.join(QLatin1Char('\n')));
    m_note->setVisible(!notes.isEmpty());
}

void
InstallationsTab::run(const QStringList &arguments, bool mutation,
                      std::function<void(const QJsonObject &)> completed)
{
    if (m_running)
        return;
    QString backend = qEnvironmentVariable("PIPEASIO_MANAGER_BACKEND");
    if (backend.isEmpty())
    {
        const QString adjacent = QDir(QCoreApplication::applicationDirPath())
                                         .filePath(QStringLiteral("pipeasio-manage"));
        if (QFileInfo(adjacent).isExecutable())
            backend = adjacent;
        else
            backend = QStandardPaths::findExecutable(QStringLiteral("pipeasio-manage"));
#ifdef PIPEASIO_MANAGER_SOURCE_BACKEND
        if (backend.isEmpty())
            backend = QString::fromUtf8(PIPEASIO_MANAGER_SOURCE_BACKEND);
#endif
    }
    if (backend.isEmpty())
    {
        fail(tr("Manager backend not found. Install the PipeASIO Manager package or set "
                "PIPEASIO_MANAGER_BACKEND to pipeasio-manage, then refresh targets."));
        return;
    }
    m_running        = true;
    m_mutation       = mutation;
    m_receivedResult = false;
    m_result         = {};
    m_output.clear();
    m_protocolError.clear();
    m_completed = std::move(completed);
    m_progress->setVisible(true);
    m_status->setText(tr("Running %1...").arg(arguments.first()));
    m_log->appendPlainText(tr("> %1 %2").arg(backend, arguments.join(QLatin1Char(' '))));
    updateActions();
    m_process->start(backend, QStringList{ QStringLiteral("--json") } + arguments);
}

void
InstallationsTab::readOutput()
{
    m_output += m_process->readAllStandardOutput();
    qsizetype newline;
    while ((newline = m_output.indexOf('\n')) >= 0)
    {
        consumeLine(m_output.left(newline));
        m_output.remove(0, newline + 1);
    }
    if (m_output.size() > 4 * 1024 * 1024)
    {
        m_protocolError = tr("Manager returned an oversized response.");
        m_output.clear();
    }
}

void
InstallationsTab::consumeLine(const QByteArray &line)
{
    if (line.trimmed().isEmpty())
        return;
    QJsonParseError error;
    const auto      document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        m_protocolError = tr(
                "Manager returned invalid JSON. Check that the GUI and backend versions match.");
        return;
    }
    const auto record = document.object();
    const auto event  = record.value(QStringLiteral("event")).toString();
    if (event == QStringLiteral("progress") && !m_receivedResult)
    {
        const auto message = record.value(QStringLiteral("message")).toString();
        m_status->setText(m_mutation ? message
                                               + tr("\nThe manager must stay open until this "
                                                    "operation finishes.")
                                     : message);
        m_log->appendPlainText(message);
    }
    else if (event == QStringLiteral("result") && !m_receivedResult
             && record.value(QStringLiteral("ok")).isBool())
    {
        m_result         = record;
        m_receivedResult = true;
    }
    else
        m_protocolError
                = tr("Manager returned an unexpected protocol record. Check the backend version.");
}

void
InstallationsTab::finish(int exitCode, bool crashed)
{
    if (!m_running)
        return;
    readOutput();
    if (!m_output.trimmed().isEmpty())
        consumeLine(m_output);
    m_output.clear();
    m_log->appendPlainText(QString::fromUtf8(m_process->readAllStandardError()));
    if (!m_protocolError.isEmpty())
        fail(m_protocolError);
    else if (!m_receivedResult)
        fail(tr("Manager exited without a result (exit %1). Review the log and refresh targets "
                "before retrying.")
                     .arg(exitCode));
    else if (!m_result.value(QStringLiteral("ok")).toBool())
        fail(m_result.value(QStringLiteral("error")).toString(tr("Manager operation failed.")));
    else if (crashed || exitCode != 0)
        fail(tr("Manager terminated unexpectedly (exit %1). Refresh targets before retrying.")
                     .arg(exitCode));
    else
    {
        const auto data      = m_result.value(QStringLiteral("data")).toObject();
        auto       completed = std::move(m_completed);
        m_running            = false;
        m_mutation           = false;
        m_progress->setVisible(false);
        const QString message = data.value(QStringLiteral("message")).toString();
        m_status->setText(message.isEmpty() ? tr("Operation completed.") : message);
        if (!message.isEmpty())
            m_log->appendPlainText(message);
        updateActions();
        if (completed)
            completed(data);
    }
}

void
InstallationsTab::fail(const QString &message)
{
    m_running   = false;
    m_mutation  = false;
    m_completed = {};
    m_progress->setVisible(false);
    m_status->setText(tr("%1\nReview the log below. Refresh targets to inspect the current state, "
                         "or refresh releases to retry a network request.")
                              .arg(message));
    m_log->appendPlainText(message);
    m_logToggle->setChecked(true);
    updateActions();
}

void
InstallationsTab::refreshTargets()
{
    const QString selected = selectedTarget().value(QStringLiteral("id")).toString();
    run({ QStringLiteral("list") }, false,
        [this, selected](const QJsonObject &data)
        {
            if (!data.value(QStringLiteral("targets")).isArray())
            {
                fail(tr("Manager response has no target list. Check the backend version."));
                return;
            }
            m_targets->clear();
            for (const auto &value : data.value(QStringLiteral("targets")).toArray())
            {
                const auto target = value.toObject();
                if (target.value(QStringLiteral("id")).toString().isEmpty())
                    continue;
                auto             *item   = new QTreeWidgetItem(m_targets);
                const QStringList fields = { QStringLiteral("name"), QStringLiteral("kind"),
                                             QStringLiteral("status"), QStringLiteral("version") };
                for (int column = 0; column < fields.size(); ++column)
                {
                    const QString value = target.value(fields[column]).toString();
                    item->setText(column,
                                  column == 1 || column == 2 ? displayTargetValue(value) : value);
                    item->setToolTip(column, target.value(fields[column]).toString());
                }
                item->setIcon(2, statusIcon(target.value(QStringLiteral("status")).toString(),
                                            palette(), devicePixelRatioF()));
                item->setData(0, Qt::UserRole, target);
                if (target.value(QStringLiteral("id")).toString() == selected)
                    m_targets->setCurrentItem(item);
            }
            if (!m_targets->currentItem() && m_targets->topLevelItemCount())
                m_targets->setCurrentItem(m_targets->topLevelItem(0));
            m_status->setText(m_targets->topLevelItemCount()
                                      ? tr("Select a target and preview an installation, or check "
                                           "an existing one.")
                                      : tr("No targets found. Add an existing Wine prefix with its "
                                           "owning Wine "
                                           "executable, or create a prefix in Faugus or Bottles "
                                           "and refresh targets."));
            m_log->appendPlainText(
                    tr("Found %1 configured prefixes.").arg(m_targets->topLevelItemCount()));
            updateActions();
        });
}

void
InstallationsTab::refreshReleases()
{
    const QString selected = m_releases->currentData().toString();
    run({ QStringLiteral("releases") }, false,
        [this, selected](const QJsonObject &data)
        {
            if (!data.value(QStringLiteral("releases")).isArray())
            {
                fail(tr("Manager response has no release list. Check the backend version."));
                return;
            }
            m_releases->clear();
            m_releases->addItem(tr("Latest stable release"), QString());
            for (const auto &value : data.value(QStringLiteral("releases")).toArray())
            {
                const auto    release = value.toObject();
                const QString version = release.value(QStringLiteral("version")).toString();
                if (version.isEmpty() || m_releases->findData(version) >= 0)
                    continue;
                QString label = release.value(QStringLiteral("name")).toString(version);
                label += QStringLiteral(" (%1)").arg(version);
                if (release.value(QStringLiteral("prerelease")).toBool())
                    label += tr(" [prerelease]");
                m_releases->addItem(label, version);
            }
            const int index = m_releases->findData(selected);
            m_releases->setCurrentIndex(index >= 0 ? index : 0);
            m_status->setText(m_releases->count() > 1
                                      ? tr("Releases refreshed. Choose a release, then preview the "
                                           "installation.")
                                      : tr("No compatible releases returned. Review the log or "
                                           "refresh releases to retry."));
        });
}

QStringList
InstallationsTab::installArguments(const QString &command) const
{
    QStringList   arguments = { command, QStringLiteral("--target"),
                                selectedTarget().value(QStringLiteral("id")).toString() };
    const QString version   = m_releases->currentData().toString();
    if (!version.isEmpty())
        arguments << QStringLiteral("--release") << version;
    if (m_include32->isChecked())
        arguments << QStringLiteral("--include-32");
    return arguments;
}

void
InstallationsTab::previewInstall(bool repair)
{
    QStringList arguments = installArguments(QStringLiteral("install"));
    if (repair)
    {
        const QString version = selectedTarget().value(QStringLiteral("version")).toString();
        if (version.isEmpty())
        {
            fail(tr("The installed version is unknown. Choose a release and use Install / "
                    "Update."));
            return;
        }
        const int releaseOption = arguments.indexOf(QStringLiteral("--release"));
        if (releaseOption >= 0)
            arguments[releaseOption + 1] = version;
        else
            arguments << QStringLiteral("--release") << version;
    }
    QStringList preview = arguments;
    preview[0]          = QStringLiteral("preview");
    run(preview, false,
        [this, arguments](const QJsonObject &data) mutable
        {
            const QString summary = data.value(QStringLiteral("summary")).toString();
            if (summary.isEmpty() || !data.value(QStringLiteral("permissions")).isArray()
                || !data.value(QStringLiteral("warnings")).isArray())
            {
                fail(tr("Manager returned an incomplete installation preview. No changes were "
                        "made."));
                return;
            }
            if (!arguments.contains(QStringLiteral("--release")))
            {
                const QString version = data.value(QStringLiteral("version")).toString();
                if (version.isEmpty())
                {
                    fail(tr("Manager preview did not identify the release. Refresh releases and "
                            "choose a specific version before retrying."));
                    return;
                }
                arguments << QStringLiteral("--release") << version;
            }
            QString details = summary;
            for (const auto &warning : data.value(QStringLiteral("warnings")).toArray())
                details += tr("\n\nWarning: %1").arg(warning.toString());
            const auto permissions = data.value(QStringLiteral("permissions")).toArray();
            if (!permissions.isEmpty())
            {
                details += tr("\n\nFlatpak permission grants required:");
                for (const auto &permission : permissions)
                    details += QStringLiteral("\n") + permission.toString();
                details += tr("\n\nContinuing explicitly authorizes these grants.");
            }
            else
                details += tr("\n\nNo Flatpak permission grants requested.");
            QMessageBox confirmation(QMessageBox::Question, tr("Confirm PipeASIO installation"),
                                     details, QMessageBox::Cancel, this);
            confirmation.setTextFormat(Qt::PlainText);
            auto *confirm = confirmation.addButton(permissions.isEmpty()
                                                           ? tr("Install / update")
                                                           : tr("Grant permissions and install"),
                                                   QMessageBox::AcceptRole);
            confirmation.setDefaultButton(QMessageBox::Cancel);
            confirmation.exec();
            if (confirmation.clickedButton() != confirm)
                return;
            if (!permissions.isEmpty())
                arguments << QStringLiteral("--allow-permissions");
            run(arguments, true, [this](const QJsonObject &) { refreshTargets(); });
        });
}

void
InstallationsTab::checkTarget()
{
    QMessageBox confirmation(QMessageBox::Question, tr("Check PipeASIO installation"),
                             targetDescription()
                                     + tr("\n\nRun the installation probe through this target's "
                                          "runner? This starts Wine to verify registration and "
                                          "native library loading. It does not play audio."),
                             QMessageBox::Ok | QMessageBox::Cancel, this);
    confirmation.setTextFormat(Qt::PlainText);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    if (confirmation.exec() == QMessageBox::Ok)
        run({ QStringLiteral("check"), QStringLiteral("--target"),
              selectedTarget().value(QStringLiteral("id")).toString() },
            true, [this](const QJsonObject &) { refreshTargets(); });
}

void
InstallationsTab::removeTarget()
{
    QMessageBox confirmation(
            QMessageBox::Warning, tr("Remove PipeASIO installation"),
            targetDescription()
                    + tr("\n\nRemove the managed PipeASIO payload and restore "
                         "the previous files and launcher environment? Unrelated "
                         "files are preserved. No new Flatpak grants will be made."),
            QMessageBox::Cancel, this);
    confirmation.setTextFormat(Qt::PlainText);
    auto *remove = confirmation.addButton(tr("Remove PipeASIO"), QMessageBox::DestructiveRole);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() == remove)
        run({ QStringLiteral("remove"), QStringLiteral("--target"),
              selectedTarget().value(QStringLiteral("id")).toString() },
            true,
            [this](const QJsonObject &)
            {
                m_guidance->clear();
                m_guidance->setVisible(false);
                refreshTargets();
            });
}

void
InstallationsTab::addPrefix()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Add an existing Wine prefix"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *notice
            = new QLabel(tr("Choose the Wine executable that owns this prefix. Do not substitute "
                            "system Wine for a launcher-managed runner. This saves the target "
                            "without installing PipeASIO or starting Wine."),
                         &dialog);
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto *form = new QFormLayout;
    auto *name = new QLineEdit(&dialog);
    name->setAccessibleName(tr("Optional target name"));
    form->addRow(tr("&Name (optional):"), name);
    auto *prefix = new QLineEdit(&dialog);
    prefix->setAccessibleName(tr("Existing Wine prefix path"));
    auto *prefixRow    = new QHBoxLayout;
    auto *browsePrefix = new QPushButton(tr("&Browse prefix..."), &dialog);
    prefixRow->addWidget(prefix);
    prefixRow->addWidget(browsePrefix);
    auto *prefixLabel = new QLabel(tr("&Prefix:"), &dialog);
    prefixLabel->setBuddy(prefix);
    form->addRow(prefixLabel, prefixRow);
    auto *wine = new QLineEdit(&dialog);
    wine->setAccessibleName(tr("Owning Wine executable path"));
    auto *wineRow    = new QHBoxLayout;
    auto *browseWine = new QPushButton(tr("Browse &Wine..."), &dialog);
    wineRow->addWidget(wine);
    wineRow->addWidget(browseWine);
    auto *wineLabel = new QLabel(tr("Wine &executable:"), &dialog);
    wineLabel->setBuddy(wine);
    form->addRow(wineLabel, wineRow);
    layout->addLayout(form);
    auto *buttons
            = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    buttons->button(QDialogButtonBox::Save)->setEnabled(false);
    auto updateSave = [buttons, prefix, wine]
    {
        buttons->button(QDialogButtonBox::Save)
                ->setEnabled(!prefix->text().trimmed().isEmpty()
                             && !wine->text().trimmed().isEmpty());
    };
    connect(prefix, &QLineEdit::textChanged, &dialog, updateSave);
    connect(wine, &QLineEdit::textChanged, &dialog, updateSave);
    connect(browsePrefix, &QPushButton::clicked, &dialog,
            [&dialog, prefix]
            {
                const auto path = QFileDialog::getExistingDirectory(
                        &dialog, tr("Choose Wine prefix"), prefix->text());
                if (!path.isEmpty())
                    prefix->setText(path);
            });
    connect(browseWine, &QPushButton::clicked, &dialog,
            [&dialog, wine]
            {
                const auto path = QFileDialog::getOpenFileName(
                        &dialog, tr("Choose owning Wine executable"), wine->text());
                if (!path.isEmpty())
                    wine->setText(path);
            });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted)
    {
        QStringList arguments = { QStringLiteral("add"), QStringLiteral("--prefix"), prefix->text(),
                                  QStringLiteral("--wine"), wine->text() };
        if (!name->text().trimmed().isEmpty())
            arguments << QStringLiteral("--name") << name->text();
        run(arguments, true, [this](const QJsonObject &) { refreshTargets(); });
    }
}
