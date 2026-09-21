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
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
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

InstallationsTab::InstallationsTab(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(tr("Manage official PipeASIO releases in Faugus, Bottles, or a custom "
                                "Wine prefix. Close the launcher and Windows applications before "
                                "changing an installation. Discovery does not start Wine."),
                             this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *discovery = new QHBoxLayout;
    m_refresh       = new QPushButton(tr("&Refresh targets"), this);
    m_add           = new QPushButton(tr("&Add prefix..."), this);
    discovery->addWidget(m_refresh);
    discovery->addWidget(m_add);
    discovery->addStretch();
    layout->addLayout(discovery);

    m_targets = new QTreeWidget(this);
    m_targets->setObjectName(QStringLiteral("installationTargets"));
    m_targets->setAccessibleName(tr("Wine installation targets"));
    m_targets->setHeaderLabels(
            { tr("Target"), tr("Launcher"), tr("Runner"), tr("Status"), tr("Version") });
    m_targets->setRootIsDecorated(false);
    m_targets->setSelectionMode(QAbstractItemView::SingleSelection);
    m_targets->setUniformRowHeights(true);
    m_targets->header()->setSectionResizeMode(QHeaderView::Interactive);
    m_targets->header()->setStretchLastSection(true);
    m_targets->setMinimumHeight(140);
    layout->addWidget(m_targets, 1);

    m_details = new QLabel(tr("Select a target to see its prefix and runner."), this);
    m_details->setTextFormat(Qt::PlainText);
    m_details->setWordWrap(true);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_details->setAccessibleName(tr("Selected target details"));
    layout->addWidget(m_details);

    auto *releases     = new QHBoxLayout;
    auto *releaseLabel = new QLabel(tr("&Release:"), this);
    m_releases         = new QComboBox(this);
    m_releases->setAccessibleName(tr("PipeASIO release"));
    m_releases->addItem(tr("Latest stable release"), QString());
    releaseLabel->setBuddy(m_releases);
    m_refreshReleases = new QPushButton(tr("Refresh re&leases"), this);
    releases->addWidget(releaseLabel);
    releases->addWidget(m_releases, 1);
    releases->addWidget(m_refreshReleases);
    layout->addLayout(releases);

    m_include32 = new QCheckBox(tr("Include e&xperimental 32-bit support"), this);
    m_include32->setToolTip(
            tr("Requires Wine's new WoW64 mode. Both frontends are checked, and a failed "
               "check restores the previous installation."));
    layout->addWidget(m_include32);

    auto *actions = new QHBoxLayout;
    m_install     = new QPushButton(tr("&Install..."), this);
    m_repair      = new QPushButton(tr("Re&pair..."), this);
    m_check       = new QPushButton(tr("&Check..."), this);
    m_remove      = new QPushButton(tr("Re&move..."), this);
    for (auto *button : { m_install, m_repair, m_check, m_remove })
        actions->addWidget(button);
    actions->addStretch();
    layout->addLayout(actions);

    m_status = new QLabel(tr("Discovering targets..."), this);
    m_status->setObjectName(QStringLiteral("installationStatus"));
    m_status->setAccessibleName(tr("Manager operation status"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(m_status);
    m_guidance = new QLabel(this);
    m_guidance->setAccessibleName(tr("Manual prefix launch instructions"));
    m_guidance->setTextFormat(Qt::PlainText);
    m_guidance->setWordWrap(true);
    m_guidance->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_guidance->setVisible(false);
    layout->addWidget(m_guidance);
    m_progress = new QProgressBar(this);
    m_progress->setAccessibleName(tr("Manager operation progress"));
    m_progress->setRange(0, 0);
    m_progress->setVisible(false);
    layout->addWidget(m_progress);
    m_log = new QPlainTextEdit(this);
    m_log->setAccessibleName(tr("Manager operation log"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);
    m_log->setMaximumHeight(160);
    layout->addWidget(m_log);

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
    connect(m_refreshReleases, &QPushButton::clicked, this, &InstallationsTab::refreshReleases);
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
    m_details->setText(selected ? targetDescription()
                                : tr("Select a target to see its prefix and runner."));
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
                                             QStringLiteral("runner"), QStringLiteral("status"),
                                             QStringLiteral("version") };
                for (int column = 0; column < fields.size(); ++column)
                {
                    const QString value = target.value(fields[column]).toString();
                    item->setText(column,
                                  column == 1 || column == 3 ? displayTargetValue(value) : value);
                    item->setToolTip(column, target.value(fields[column]).toString());
                }
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
