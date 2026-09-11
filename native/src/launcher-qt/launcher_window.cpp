// SPDX-License-Identifier: AGPL-3.0-only

#include "launcher_window.hpp"
#include "platform_process.hpp"
#include "platform_secrets.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QAction>
#include <QAbstractItemView>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QFont>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <QCloseEvent>
#include <QIODevice>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using cuajone::launcher::AnalyticsMode;
using cuajone::launcher::ComputeMode;
using cuajone::RtspTransport;
using cuajone::VideoAcceleration;
using cuajone::launcher::CameraConnectionProfile;
using cuajone::launcher::LauncherSettings;

QString pathString(const std::filesystem::path& path) {
    return QString::fromStdWString(path.wstring());
}

std::filesystem::path filePath(const QString& value) {
    return std::filesystem::path(value.toStdWString());
}

cuajone::platform::PlatformPathOverrides qtPathOverrides() {
    return {
        filePath(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)),
        filePath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)),
        filePath(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)),
    };
}

QString cleanText(const QString& value) {
    return value.trimmed();
}

std::wstring wide(const QString& value) {
    return value.toStdWString();
}

VideoAcceleration accelerationAt(int index) {
    return index == 1 ? VideoAcceleration::Cpu
        : index == 2 ? VideoAcceleration::Vaapi : VideoAcceleration::Auto;
}

RtspTransport transportAt(int index) {
    return index == 1 ? RtspTransport::Tcp
        : index == 2 ? RtspTransport::Udp : RtspTransport::Default;
}

QString readableError(const std::exception& error) {
    return QString::fromUtf8(error.what());
}

std::map<QString, QString> readEnv(const QString& fileName) {
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        throw std::runtime_error("Could not open the selected .env file");
    }
    const QString content = QString::fromUtf8(file.readAll()).remove(QChar(0xFEFF));
    std::map<QString, QString> result;
    for (QString line : content.split('\n')) {
        line.remove(QChar('\r'));
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        const int separator = line.indexOf('=');
        if (separator <= 0) continue;
        QString key = line.left(separator).trimmed().toUpper();
        QString value = line.mid(separator + 1).trimmed();
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.mid(1, value.size() - 2);
        }
        result[key] = value;
    }
    return result;
}

QString envValue(const std::map<QString, QString>& values, const QString& key) {
    const auto found = values.find(key);
    return found == values.end() ? QString{} : found->second;
}

QStringList profileNames(const QListWidget* list) {
    QStringList result;
    for (const QListWidgetItem* item : list->selectedItems()) result.push_back(item->text());
    return result;
}

void setComboText(QComboBox* combo, const QString& text) {
    const int index = combo->findText(text);
    if (index >= 0) combo->setCurrentIndex(index);
}

CameraConnectionProfile profileDialog(QWidget* parent, CameraConnectionProfile profile, bool& accepted) {
    QDialog dialog(parent);
    dialog.setWindowTitle("Camera profile");
    dialog.setModal(true);
    auto* form = new QFormLayout(&dialog);
    auto* name = new QLineEdit(QString::fromStdWString(profile.name), &dialog);
    auto* host = new QLineEdit(QString::fromStdWString(profile.host), &dialog);
    auto* port = new QSpinBox(&dialog);
    port->setRange(1, 65535);
    port->setValue(profile.port);
    auto* path = new QLineEdit(QString::fromStdWString(profile.path), &dialog);
    auto* user = new QLineEdit(QString::fromStdWString(profile.username), &dialog);
    auto* password = new QLineEdit(QString::fromStdWString(profile.password), &dialog);
    password->setEchoMode(QLineEdit::Password);
    auto* transport = new QComboBox(&dialog);
    transport->addItems({"Default", "TCP", "UDP"});
    transport->setCurrentIndex(profile.transport == RtspTransport::Tcp ? 1
        : profile.transport == RtspTransport::Udp ? 2 : 0);
    auto* acceleration = new QComboBox(&dialog);
    acceleration->addItems({"Auto", "CPU", "VAAPI"});
    acceleration->setCurrentIndex(profile.video_acceleration == VideoAcceleration::Cpu ? 1
        : profile.video_acceleration == VideoAcceleration::Vaapi ? 2 : 0);
    form->addRow("Profile name", name);
    form->addRow("Host / IP", host);
    form->addRow("Port", port);
    form->addRow("RTSP path", path);
    form->addRow("Username", user);
    form->addRow("Password", password);
    form->addRow("Transport", transport);
    form->addRow("Acceleration", acceleration);
    const auto secret_info = cuajone::platform::cameraPasswordStoreInfo();
    auto* security = new QLabel(
        QString("Password storage: %1. Passwords are never written to profile files.")
            .arg(QString::fromUtf8(secret_info.description.data(),
                static_cast<qsizetype>(secret_info.description.size()))),
        &dialog);
    security->setWordWrap(true);
    form->addRow(security);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        accepted = false;
        return profile;
    }
    profile.name = wide(cleanText(name->text()));
    profile.host = wide(cleanText(host->text()));
    profile.port = static_cast<std::uint16_t>(port->value());
    profile.path = wide(cleanText(path->text()));
    profile.username = wide(user->text());
    profile.password = wide(password->text());
    profile.transport = transportAt(transport->currentIndex());
    profile.video_acceleration = accelerationAt(acceleration->currentIndex());
    cuajone::launcher::validateCameraConnectionProfile(profile);
    accepted = true;
    return profile;
}

}  // namespace

LauncherWindow::LauncherWindow(QWidget* parent)
    : QMainWindow(parent),
      platform_paths_(cuajone::platform::resolvePlatformPaths(
          filePath(QCoreApplication::applicationDirPath()), qtPathOverrides())),
      profiles_dir_(platform_paths_.profiles_dir),
      preferences_path_(platform_paths_.config_dir / L"preferences.txt"),
      launcher_settings_path_(platform_paths_.config_dir / L"launcher.ini"),
      managed_model_root_(runtimePath().parent_path() / L"models") {
    std::error_code error;
    std::filesystem::create_directories(platform_paths_.config_dir, error);
    std::filesystem::create_directories(profiles_dir_, error);
    preferences_ = cuajone::launcher::loadOperatorPreferences(preferences_path_);
    telemetry_enabled_ = false;
    setWindowTitle("NexoAI Vision Launcher");
    resize(860, 720);

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    auto* heading = new QLabel("NexoAI Vision", central);
    QFont headingFont = heading->font();
    headingFont.setPointSize(18);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    root->addWidget(heading);

    auto* profileBox = new QGroupBox("Camera profiles", central);
    auto* profileLayout = new QHBoxLayout(profileBox);
    profiles_ = new QListWidget(profileBox);
    profiles_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    profileLayout->addWidget(profiles_, 1);
    auto* profileButtons = new QVBoxLayout;
    new_profile_ = new QPushButton("New", profileBox);
    edit_profile_ = new QPushButton("Edit", profileBox);
    delete_profile_ = new QPushButton("Delete", profileBox);
    select_all_ = new QPushButton("Select all", profileBox);
    for (auto* button : {new_profile_, edit_profile_, delete_profile_, select_all_}) {
        profileButtons->addWidget(button);
    }
    profileButtons->addStretch(1);
    profileLayout->addLayout(profileButtons);
    root->addWidget(profileBox, 1);

    auto* inputForm = new QFormLayout;
    auto* videoRow = new QWidget(central);
    auto* videoLayout = new QHBoxLayout(videoRow);
    videoLayout->setContentsMargins(0, 0, 0, 0);
    video_ = new QLineEdit(videoRow);
    video_browse_ = new QPushButton("Browse...", videoRow);
    videoLayout->addWidget(video_, 1);
    videoLayout->addWidget(video_browse_);
    inputForm->addRow("Video file (optional)", videoRow);
    auto* outputRow = new QWidget(central);
    auto* outputLayout = new QHBoxLayout(outputRow);
    outputLayout->setContentsMargins(0, 0, 0, 0);
    output_ = new QLineEdit(outputRow);
    output_browse_ = new QPushButton("Browse...", outputRow);
    outputLayout->addWidget(output_, 1);
    outputLayout->addWidget(output_browse_);
    inputForm->addRow("Output folder", outputRow);
    root->addLayout(inputForm);

    auto* actions = new QHBoxLayout;
    validate_ = new QPushButton("Validate", central);
    start_ = new QPushButton("Start", central);
    stop_ = new QPushButton("Stop", central);
    stop_->setEnabled(false);
    actions->addWidget(validate_);
    actions->addWidget(start_);
    actions->addWidget(stop_);
    actions->addStretch(1);
    root->addLayout(actions);

    auto* statusForm = new QFormLayout;
    status_ = new QLabel("Ready", central);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    log_path_ = new QLabel("No log created yet", central);
    log_path_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusForm->addRow("Status", status_);
    auto* logRow = new QWidget(central);
    auto* logLayout = new QHBoxLayout(logRow);
    logLayout->setContentsMargins(0, 0, 0, 0);
    logLayout->addWidget(log_path_, 1);
    auto* open = new QPushButton("Open log", logRow);
    logLayout->addWidget(open);
    statusForm->addRow("Log path", logRow);
    root->addLayout(statusForm);

    log_tail_ = new QPlainTextEdit(central);
    log_tail_->setReadOnly(true);
    log_tail_->setPlaceholderText("Runtime output will appear here.");
    root->addWidget(log_tail_, 1);
    setCentralWidget(central);

    connect(new_profile_, &QPushButton::clicked, this, &LauncherWindow::createProfile);
    connect(edit_profile_, &QPushButton::clicked, this, &LauncherWindow::editProfile);
    connect(delete_profile_, &QPushButton::clicked, this, &LauncherWindow::deleteProfiles);
    connect(select_all_, &QPushButton::clicked, this, &LauncherWindow::selectAllProfiles);
    connect(profiles_, &QListWidget::itemDoubleClicked, this, [this] { editProfile(); });
    connect(video_browse_, &QPushButton::clicked, this, &LauncherWindow::browseVideo);
    connect(output_browse_, &QPushButton::clicked, this, &LauncherWindow::browseOutput);
    connect(validate_, &QPushButton::clicked, this, &LauncherWindow::validateConfiguration);
    connect(start_, &QPushButton::clicked, this, &LauncherWindow::startRuntime);
    connect(stop_, &QPushButton::clicked, this, &LauncherWindow::stopRuntime);
    connect(open, &QPushButton::clicked, this, &LauncherWindow::openLog);
    connect(&process_, &QProcess::readyReadStandardOutput, this, &LauncherWindow::appendProcessOutput);
    connect(&process_, &QProcess::readyReadStandardError, this, &LauncherWindow::appendProcessOutput);
    connect(&process_, &QProcess::finished, this, &LauncherWindow::processFinished);
    connect(&process_, &QProcess::errorOccurred, this, &LauncherWindow::processError);

    createMenus();
    loadState();
    reloadProfiles();
    const auto configuredOutput = filePath(output_->text());
    if (output_->text().isEmpty() || !cuajone::platform::ensureWritableDirectory(configuredOutput)) {
        const auto defaultOutput = platform_paths_.data_dir / L"output";
        cuajone::platform::ensureWritableDirectory(defaultOutput);
        output_->setText(pathString(defaultOutput));
    }
    setStatus("Ready");
}

LauncherWindow::~LauncherWindow() {
    if (process_.state() != QProcess::NotRunning) {
        const auto policy = cuajone::platform::processStopPolicy();
        process_.terminate();
        if (!process_.waitForFinished(policy.graceful_timeout_ms)) process_.kill();
        process_.waitForFinished(policy.force_timeout_ms);
    }
    appendProcessOutput();
    log_file_.close();
    saveState();
}

void LauncherWindow::createMenus() {
    QMenu* menu = menuBar()->addMenu("Menu");
    QAction* ppe = menu->addAction("PPE threshold profile");
    QAction* advanced = menu->addAction("Advanced settings");
    QAction* env = menu->addAction("Import .env");
    menu->addSeparator();
    QAction* log = menu->addAction("Open log");
    connect(ppe, &QAction::triggered, this, &LauncherWindow::openPpeThresholds);
    connect(advanced, &QAction::triggered, this, &LauncherWindow::openAdvancedSettings);
    connect(env, &QAction::triggered, this, &LauncherWindow::importEnv);
    connect(log, &QAction::triggered, this, &LauncherWindow::openLog);
}

void LauncherWindow::loadState() {
    QSettings settings(pathString(launcher_settings_path_), QSettings::IniFormat);
    video_->setText(settings.value("video_file").toString());
    output_->setText(settings.value("output_folder").toString());
    compute_mode_ = static_cast<ComputeMode>(settings.value("compute_mode", 0).toInt());
    if (static_cast<int>(compute_mode_) < static_cast<int>(ComputeMode::Auto)
        || static_cast<int>(compute_mode_) > static_cast<int>(ComputeMode::Cpu)) compute_mode_ = ComputeMode::Auto;
    telemetry_enabled_ = settings.value("telemetry_enabled", false).toBool();
    telemetry_interval_seconds_ = settings.value("telemetry_interval_seconds", 5).toInt();
    selected_profiles_ = settings.value("selected_profiles").toStringList();
    if (std::ranges::find(cuajone::launcher::kTelemetryIntervals, telemetry_interval_seconds_)
        == cuajone::launcher::kTelemetryIntervals.end()) telemetry_interval_seconds_ = 5;
}

void LauncherWindow::saveState() const {
    try {
        QSettings settings(pathString(launcher_settings_path_), QSettings::IniFormat);
        settings.setValue("video_file", video_->text());
        settings.setValue("output_folder", output_->text());
        settings.setValue("compute_mode", static_cast<int>(compute_mode_));
        settings.setValue("telemetry_enabled", telemetry_enabled_);
        settings.setValue("telemetry_interval_seconds", telemetry_interval_seconds_);
        settings.setValue("selected_profiles", profileNames(profiles_));
        settings.sync();
        cuajone::launcher::saveOperatorPreferencesAtomic(preferences_path_, preferences_);
    } catch (...) {
        // A read-only configuration directory must not prevent the launcher
        // from closing; the next run will use safe defaults.
    }
}

void LauncherWindow::reloadProfiles(const QString& preferred) {
    const QStringList selected = preferred.isEmpty()
        ? (selected_profiles_.isEmpty() ? profileNames(profiles_) : selected_profiles_)
        : QStringList{preferred};
    profiles_->clear();
    std::vector<QString> names;
    std::error_code error;
    if (std::filesystem::is_directory(profiles_dir_, error) && !error) {
        for (const auto& entry : std::filesystem::directory_iterator(profiles_dir_, error)) {
            if (error) break;
            if (entry.path().extension() == L".profile") names.push_back(QString::fromStdWString(entry.path().stem().wstring()));
        }
    }
    std::sort(names.begin(), names.end());
    for (const QString& name : names) {
        auto* item = new QListWidgetItem(name, profiles_);
        if (selected.contains(name)) item->setSelected(true);
    }
}

std::filesystem::path LauncherWindow::profilePath(const QString& name) const {
    const std::wstring profileName = wide(cleanText(name));
    if (!cuajone::launcher::isValidSavedCameraProfileName(profileName)) {
        throw std::invalid_argument("Profile name is invalid");
    }
    return profiles_dir_ / (profileName + L".profile");
}

CameraConnectionProfile LauncherWindow::readProfile(const QString& name) const {
    QFile file(pathString(profilePath(name)));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) {
        throw std::runtime_error("Could not read camera profile");
    }
    auto profile = cuajone::launcher::parseCameraConnectionProfile(
        file.readAll().toStdString(), wide(name));
    // Do not trust a password field from an existing file, including profiles
    // copied from another platform. Restore it only from the platform secret store.
    profile.password.clear();
    if (const auto password = cuajone::platform::loadCameraPassword(wide(name))) {
        profile.password = *password;
    }
    return profile;
}

void LauncherWindow::writeProfile(const CameraConnectionProfile& profile) {
    cuajone::launcher::validateCameraConnectionProfile(profile);
    auto diskProfile = profile;
    diskProfile.password.clear();
    QSaveFile file(pathString(profilePath(QString::fromStdWString(profile.name))));
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Could not save camera profile");
    const std::string payload = cuajone::launcher::serializeCameraConnectionProfile(diskProfile);
    if (file.write(QByteArray::fromStdString(payload)) != static_cast<qint64>(payload.size())
        || !file.commit()) throw std::runtime_error("Could not atomically save camera profile");
    if (!cuajone::platform::saveCameraPassword(profile.name, profile.password)) {
        throw std::runtime_error("Could not store the camera password securely");
    }
    if (!profile.password.empty()
        && !cuajone::platform::cameraPasswordStoreInfo().persistent
        && !password_warning_shown_) {
        password_warning_shown_ = true;
        QMessageBox::warning(
            this, "Password not persisted",
            "The password is retained only for this launcher session and will be forgotten when it exits.");
    }
}

void LauncherWindow::createProfile() {
    try {
        CameraConnectionProfile profile;
        profile.name = L"CAMERA_01";
        profile.username.clear();
        profile.password.clear();
        for (int index = 1; index < 1000; ++index) {
            profile.name = std::wstring(L"CAMERA_")
                + (index < 10 ? L"0" : L"") + std::to_wstring(index);
            if (!std::filesystem::exists(profilePath(QString::fromStdWString(profile.name)))) break;
        }
        bool accepted = false;
        profile = profileDialog(this, profile, accepted);
        if (!accepted) return;
        if (std::filesystem::exists(profilePath(QString::fromStdWString(profile.name)))) {
            throw std::invalid_argument("A profile with this name already exists");
        }
        writeProfile(profile);
        reloadProfiles(QString::fromStdWString(profile.name));
        setStatus("Profile created");
    } catch (const std::exception& error) {
        QMessageBox::critical(this, "Camera profile", readableError(error));
    }
}

void LauncherWindow::editProfile() {
    try {
        const QStringList selected = profileNames(profiles_);
        if (selected.size() != 1) throw std::invalid_argument("Select exactly one profile to edit");
        const QString oldName = selected.front();
        CameraConnectionProfile profile = readProfile(oldName);
        bool accepted = false;
        profile = profileDialog(this, profile, accepted);
        if (!accepted) return;
        const QString newName = QString::fromStdWString(profile.name);
        if (newName != oldName && std::filesystem::exists(profilePath(newName))) {
            throw std::invalid_argument("A profile with this name already exists");
        }
        writeProfile(profile);
        if (newName != oldName) {
            std::error_code error;
            std::filesystem::remove(profilePath(oldName), error);
            cuajone::platform::deleteCameraPassword(wide(oldName));
        }
        reloadProfiles(newName);
        setStatus("Profile updated");
    } catch (const std::exception& error) {
        QMessageBox::critical(this, "Camera profile", readableError(error));
    }
}

void LauncherWindow::deleteProfiles() {
    const QStringList selected = profileNames(profiles_);
    if (selected.isEmpty()) {
        QMessageBox::information(this, "Camera profiles", "Select one or more profiles first.");
        return;
    }
    if (QMessageBox::question(this, "Delete profiles", "Delete the selected camera profiles?") != QMessageBox::Yes) return;
    try {
        for (const QString& name : selected) {
            std::error_code error;
            std::filesystem::remove(profilePath(name), error);
            cuajone::platform::deleteCameraPassword(wide(name));
        }
        reloadProfiles();
        setStatus("Profiles deleted");
    } catch (const std::exception& error) {
        QMessageBox::critical(this, "Camera profiles", readableError(error));
    }
}

void LauncherWindow::selectAllProfiles() {
    profiles_->selectAll();
    setStatus(QString("Selected %1 profile(s)").arg(profiles_->selectedItems().size()));
}

void LauncherWindow::browseVideo() {
    const QString path = QFileDialog::getOpenFileName(
        this, "Select video file", video_->text(),
        "Video files (*.mp4 *.avi *.mov *.mkv);;All files (*)");
    if (!path.isEmpty()) video_->setText(path);
}

void LauncherWindow::browseOutput() {
    const QString path = QFileDialog::getExistingDirectory(this, "Select output folder", output_->text());
    if (!path.isEmpty()) output_->setText(path);
}

std::filesystem::path LauncherWindow::runtimePath() const {
    auto runtime = filePath(QCoreApplication::applicationDirPath()) / L"NexoAIVision";
#ifdef _WIN32
    runtime += L".exe";
#endif
    return runtime;
}

std::filesystem::path LauncherWindow::nextLogPath() const {
    const auto directory = platform_paths_.logs_dir;
    cuajone::platform::ensureWritableDirectory(directory);
    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss-zzz");
    return directory / (L"nexoai-" + wide(stamp) + L".log");
}

LauncherSettings LauncherWindow::readLauncherSettings() const {
    LauncherSettings settings;
    settings.performance_report = telemetry_enabled_;
    settings.telemetry_interval_seconds = telemetry_interval_seconds_;
    settings.output = filePath(cleanText(output_->text()));
    settings.analytics_mode = AnalyticsMode::PpeFall;
    settings.compute_mode = compute_mode_;
    settings.rtsp_transport = preferences_.rtsp_transport;
    settings.video_acceleration = preferences_.video_acceleration;
    settings.stream_resolution = preferences_.stream_resolution;
    settings.stream_fps = preferences_.stream_fps;
    settings.managed_model_root = managed_model_root_;
    settings.allow_dev_model_fallback = true;
    settings.image_size = preferences_.image_size;
    settings.ppe_class_confidences = preferences_.ppe_class_confidences;
    settings.ppe_enabled = preferences_.ppe_enabled;
    settings.show_window = true;

    const QStringList selected = profileNames(profiles_);
    for (const QString& name : selected) {
        const auto profile = readProfile(name);
        settings.cameras.push_back({
            cuajone::launcher::buildAxisRtspUrl(profile),
            profile.name,
            profile.transport,
            profile.video_acceleration,
        });
    }
    const QString video = cleanText(video_->text());
    if (!video.isEmpty()) {
        if (selected.isEmpty()) settings.source = wide(video);
        else settings.cameras.push_back({wide(video), L"", preferences_.rtsp_transport, preferences_.video_acceleration});
    }
    return settings;
}

void LauncherWindow::launchRuntime(bool preflight) {
    if (process_.state() != QProcess::NotRunning) return;
    const LauncherSettings settings = readLauncherSettings();
    const auto plan = cuajone::launcher::buildLaunchPlan(settings, preflight);
    const auto runtime = runtimePath();
    std::error_code error;
    if (!std::filesystem::is_regular_file(runtime, error) || error) {
        throw std::runtime_error("Sibling NexoAIVision was not found");
    }
    if (!std::filesystem::create_directories(settings.output, error) && error) {
        throw std::runtime_error("Output folder is not writable");
    }
    const auto logPath = nextLogPath();
    std::filesystem::create_directories(logPath.parent_path(), error);
    log_file_.setFileName(pathString(logPath));
    if (!log_file_.open(QIODevice::WriteOnly | QIODevice::Text)) {
        throw std::runtime_error("Could not create runtime log");
    }
    log_path_->setText(pathString(logPath));
    log_tail_->clear();
    log_file_.write("NexoAI Vision runtime log\n");
    log_file_.write(preflight ? "Mode: validation\n" : "Mode: capture\n");
    log_file_.flush();
    QStringList arguments;
    for (const std::wstring& argument : plan.arguments) arguments.push_back(QString::fromStdWString(argument));
    process_.setProgram(pathString(runtime));
    process_.setArguments(arguments);
    process_.setWorkingDirectory(pathString(runtime.parent_path()));
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    preflight_running_ = preflight;
    process_.start();
    if (!process_.waitForStarted(3000)) {
        log_file_.close();
        throw std::runtime_error("Could not start sibling NexoAIVision");
    }
    setRunning(true);
    setStatus(preflight ? "Validating configuration..." : "Runtime is running");
}

void LauncherWindow::validateConfiguration() {
    try {
        launchRuntime(true);
    } catch (const std::exception& error) {
        setStatus("Validation failed");
        QMessageBox::critical(this, "Validation", readableError(error));
    }
}

void LauncherWindow::startRuntime() {
    try {
        launchRuntime(false);
    } catch (const std::exception& error) {
        setStatus("Could not start runtime");
        QMessageBox::critical(this, "Start runtime", readableError(error));
    }
}

void LauncherWindow::stopRuntime() {
    if (process_.state() == QProcess::NotRunning) return;
    const auto policy = cuajone::platform::processStopPolicy();
    setStatus("Stopping runtime...");
    process_.terminate();
    if (!process_.waitForFinished(policy.graceful_timeout_ms)) {
        process_.kill();
        process_.waitForFinished(policy.force_timeout_ms);
    }
    setRunning(false);
}

void LauncherWindow::openLog() {
    const QString path = log_path_->text();
    if (path.isEmpty() || path == "No log created yet" || !QFileInfo::exists(path)) {
        QMessageBox::information(this, "Runtime log", "No runtime log has been created yet.");
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void LauncherWindow::openPpeThresholds() {
    QDialog dialog(this);
    dialog.setWindowTitle("PPE threshold profile");
    auto* form = new QFormLayout(&dialog);
    std::array<QDoubleSpinBox*, cuajone::kPpeOutputLabels.size()> controls{};
    for (std::size_t index = 0; index < controls.size(); ++index) {
        controls[index] = new QDoubleSpinBox(&dialog);
        controls[index]->setRange(0.0, 1.0);
        controls[index]->setSingleStep(0.01);
        controls[index]->setDecimals(2);
        controls[index]->setValue(preferences_.ppe_class_confidences[index]);
        const auto label = cuajone::kPpeOutputLabels[index];
        form->addRow(QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size())), controls[index]);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    for (std::size_t index = 0; index < controls.size(); ++index) {
        preferences_.ppe_class_confidences[index] = static_cast<float>(controls[index]->value());
    }
    try {
        cuajone::launcher::saveOperatorPreferencesAtomic(preferences_path_, preferences_);
        setStatus("PPE thresholds saved");
    } catch (const std::exception& error) {
        QMessageBox::critical(this, "PPE threshold profile", readableError(error));
    }
}

void LauncherWindow::openAdvancedSettings() {
    QDialog dialog(this);
    dialog.setWindowTitle("Advanced settings");
    auto* form = new QFormLayout(&dialog);
    auto* compute = new QComboBox(&dialog);
    compute->addItems({"Auto", "GPU", "CPU"});
    compute->setCurrentIndex(compute_mode_ == ComputeMode::Cuda ? 1 : compute_mode_ == ComputeMode::Cpu ? 2 : 0);
    auto* imgsz = new QComboBox(&dialog);
    for (const int value : cuajone::kAllowedImageSizes) imgsz->addItem(QString::number(value));
    setComboText(imgsz, QString::number(preferences_.image_size));
    auto* resolution = new QComboBox(&dialog);
    for (const auto value : cuajone::launcher::kStreamResolutions) resolution->addItem(QString::fromStdWString(std::wstring(value)));
    setComboText(resolution, QString::fromStdWString(preferences_.stream_resolution));
    auto* fps = new QComboBox(&dialog);
    for (const int value : cuajone::launcher::kStreamFrameRates) fps->addItem(QString::number(value));
    setComboText(fps, QString::number(preferences_.stream_fps));
    auto* telemetry = new QCheckBox("Enable performance telemetry", &dialog);
    telemetry->setChecked(telemetry_enabled_);
    auto* interval = new QComboBox(&dialog);
    for (const int value : cuajone::launcher::kTelemetryIntervals) interval->addItem(QString::number(value) + " seconds", value);
    const int intervalIndex = interval->findData(telemetry_interval_seconds_);
    if (intervalIndex >= 0) interval->setCurrentIndex(intervalIndex);
    auto* acceleration = new QComboBox(&dialog);
    acceleration->addItems({"Auto", "CPU", "VAAPI"});
    acceleration->setCurrentIndex(preferences_.video_acceleration == VideoAcceleration::Cpu ? 1
        : preferences_.video_acceleration == VideoAcceleration::Vaapi ? 2 : 0);
    form->addRow("Compute", compute);
    form->addRow("Inference image size", imgsz);
    form->addRow("Stream resolution", resolution);
    form->addRow("Stream FPS", fps);
    form->addRow(telemetry);
    form->addRow("Telemetry interval", interval);
    form->addRow("Video acceleration", acceleration);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    compute_mode_ = compute->currentIndex() == 1 ? ComputeMode::Cuda
        : compute->currentIndex() == 2 ? ComputeMode::Cpu : ComputeMode::Auto;
    preferences_.image_size = imgsz->currentText().toInt();
    preferences_.stream_resolution = wide(resolution->currentText());
    preferences_.stream_fps = fps->currentText().toInt();
    telemetry_enabled_ = telemetry->isChecked();
    telemetry_interval_seconds_ = interval->currentData().toInt();
    preferences_.video_acceleration = accelerationAt(acceleration->currentIndex());
    try {
        saveState();
        setStatus("Advanced settings saved");
    } catch (const std::exception& error) {
        QMessageBox::critical(this, "Advanced settings", readableError(error));
    }
}

void LauncherWindow::importEnv() {
    const QString fileName = QFileDialog::getOpenFileName(
        this, "Import .env", QString{}, ".env files (*.env *.txt);;All files (*)");
    if (fileName.isEmpty()) return;
    try {
        const auto values = readEnv(fileName);
        const QFileInfo envInfo(fileName);
        const QString transport = envValue(values, "RTSP_TRANSPORT").toLower();
        if (!transport.isEmpty() && transport != "default" && transport != "tcp" && transport != "udp") {
            throw std::invalid_argument("RTSP_TRANSPORT must be tcp, udp, or default");
        }
        if (!transport.isEmpty()) {
            preferences_.rtsp_transport = transport == "default" ? RtspTransport::Default
                : transport == "udp" ? RtspTransport::Udp : RtspTransport::Tcp;
        }
        const QString acceleration = envValue(values, "VIDEO_ACCELERATION").toLower();
        if (!acceleration.isEmpty() && acceleration != "auto" && acceleration != "cpu" && acceleration != "vaapi") {
            throw std::invalid_argument("VIDEO_ACCELERATION must be auto, cpu, or vaapi");
        }
        if (!acceleration.isEmpty()) {
            preferences_.video_acceleration = acceleration == "cpu" ? VideoAcceleration::Cpu
                : acceleration == "vaapi" ? VideoAcceleration::Vaapi : VideoAcceleration::Auto;
        }
        const QString source = envValue(values, "RTSP_URL");
        if (!source.isEmpty()) {
            const QString lower = source.toLower();
            const bool isRtsp = lower.startsWith("rtsp://") || lower.startsWith("rtsps://");
            if (isRtsp) {
                CameraConnectionProfile profile = cuajone::launcher::parseLegacyCameraUrl(
                    wide(source), wide(envValue(values, "CAMERA_ID").isEmpty() ? "CAMERA_IMPORTED" : envValue(values, "CAMERA_ID")));
                if (!transport.isEmpty()) {
                    profile.transport = preferences_.rtsp_transport;
                }
                if (!acceleration.isEmpty()) {
                    profile.video_acceleration = preferences_.video_acceleration;
                }
                const QString resolution = envValue(values, "RTSP_RESOLUTION");
                if (!resolution.isEmpty()) profile.resolution = wide(resolution);
                const QString fps = envValue(values, "RTSP_FPS");
                if (!fps.isEmpty()) profile.fps = fps.toInt();
                writeProfile(profile);
                reloadProfiles(QString::fromStdWString(profile.name));
            } else {
                const QFileInfo candidate(envInfo.dir(), source);
                video_->setText(candidate.isRelative() ? candidate.absoluteFilePath() : source);
            }
        }
        const QString output = envValue(values, "OUTPUT_DIR");
        if (!output.isEmpty()) {
            const QFileInfo candidate(envInfo.dir(), output);
            output_->setText(candidate.isRelative() ? candidate.absoluteFilePath() : output);
        }
        const QString imageSize = envValue(values, "PPE_IMGSZ");
        if (!imageSize.isEmpty() && cuajone::isSupportedImageSize(imageSize.toInt())) preferences_.image_size = imageSize.toInt();
        const QString confidence = envValue(values, "PPE_CONF");
        if (!confidence.isEmpty()) {
            const float value = cuajone::launcher::parsePpeConfidenceThreshold(wide(confidence));
            preferences_.ppe_class_confidences.fill(value);
        }
        const QString resolution = envValue(values, "RTSP_RESOLUTION");
        if (!resolution.isEmpty()) preferences_.stream_resolution = wide(resolution);
        const QString fps = envValue(values, "RTSP_FPS");
        if (!fps.isEmpty()) preferences_.stream_fps = fps.toInt();
        saveState();
        setStatus(".env settings imported");
    } catch (const std::exception& error) {
        QMessageBox::critical(this, "Import .env", readableError(error));
    }
}

void LauncherWindow::appendProcessOutput() {
    const QByteArray output = process_.readAllStandardOutput() + process_.readAllStandardError();
    if (output.isEmpty()) return;
    const std::string redacted = cuajone::launcher::redactRtspCredentials(
        std::string_view(output.constData(), static_cast<std::size_t>(output.size())));
    const QByteArray safe = QByteArray::fromStdString(redacted);
    if (log_file_.isOpen()) {
        log_file_.write(safe);
        log_file_.flush();
    }
    log_tail_->appendPlainText(QString::fromUtf8(safe).trimmed());
}

void LauncherWindow::processFinished(int exit_code, QProcess::ExitStatus status) {
    appendProcessOutput();
    log_file_.close();
    const bool preflight = preflight_running_;
    preflight_running_ = false;
    setRunning(false);
    if (status == QProcess::CrashExit) setStatus("Runtime crashed");
    else if (exit_code == 0) setStatus(preflight ? "Validation passed" : "Runtime completed successfully");
    else setStatus(QString("Runtime stopped (exit code %1)").arg(exit_code));
}

void LauncherWindow::processError(QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
        log_file_.close();
        setRunning(false);
        setStatus("Runtime failed to start");
    }
}

void LauncherWindow::setStatus(const QString& text) {
    status_->setText(text);
}

void LauncherWindow::setRunning(bool running) {
    validate_->setEnabled(!running);
    start_->setEnabled(!running);
    stop_->setEnabled(running);
    profiles_->setEnabled(!running);
    new_profile_->setEnabled(!running);
    edit_profile_->setEnabled(!running);
    delete_profile_->setEnabled(!running);
    select_all_->setEnabled(!running);
    video_->setEnabled(!running);
    video_browse_->setEnabled(!running);
    output_->setEnabled(!running);
    output_browse_->setEnabled(!running);
    menuBar()->setEnabled(!running);
}

void LauncherWindow::closeEvent(QCloseEvent* event) {
    if (process_.state() != QProcess::NotRunning) stopRuntime();
    saveState();
    event->accept();
}
