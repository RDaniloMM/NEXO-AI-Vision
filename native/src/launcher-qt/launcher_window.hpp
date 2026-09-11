// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "cuajone/launcher_support.hpp"
#include "platform_paths.hpp"

#include <QFile>
#include <QMainWindow>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <array>
#include <filesystem>

class QCloseEvent;
class QComboBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QLabel;

class LauncherWindow final : public QMainWindow {
public:
    explicit LauncherWindow(QWidget* parent = nullptr);
    ~LauncherWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void createMenus();
    void loadState();
    void saveState() const;
    void reloadProfiles(const QString& preferred = {});
    void createProfile();
    void editProfile();
    void deleteProfiles();
    void selectAllProfiles();
    void browseVideo();
    void browseOutput();
    void validateConfiguration();
    void startRuntime();
    void stopRuntime();
    void openLog();
    void openPpeThresholds();
    void openAdvancedSettings();
    void importEnv();
    void appendProcessOutput();
    void processFinished(int exit_code, QProcess::ExitStatus status);
    void processError(QProcess::ProcessError error);
    void setStatus(const QString& text);
    void launchRuntime(bool preflight);

    cuajone::launcher::LauncherSettings readLauncherSettings() const;
    cuajone::launcher::CameraConnectionProfile readProfile(const QString& name) const;
    void writeProfile(const cuajone::launcher::CameraConnectionProfile& profile);
    std::filesystem::path profilePath(const QString& name) const;
    std::filesystem::path runtimePath() const;
    std::filesystem::path nextLogPath() const;
    void setRunning(bool running);

    QListWidget* profiles_{};
    QLineEdit* video_{};
    QLineEdit* output_{};
    QLabel* status_{};
    QLabel* log_path_{};
    QPlainTextEdit* log_tail_{};
    QPushButton* validate_{};
    QPushButton* start_{};
    QPushButton* stop_{};
    QPushButton* new_profile_{};
    QPushButton* edit_profile_{};
    QPushButton* delete_profile_{};
    QPushButton* select_all_{};
    QPushButton* video_browse_{};
    QPushButton* output_browse_{};

    QProcess process_;
    QFile log_file_;
    cuajone::launcher::OperatorPreferences preferences_;
    cuajone::launcher::ComputeMode compute_mode_{cuajone::launcher::ComputeMode::Auto};
    bool telemetry_enabled_{};
    int telemetry_interval_seconds_{5};
    bool preflight_running_{};
    bool password_warning_shown_{};
    cuajone::platform::PlatformPaths platform_paths_;
    std::filesystem::path profiles_dir_;
    std::filesystem::path preferences_path_;
    std::filesystem::path launcher_settings_path_;
    std::filesystem::path managed_model_root_;
    QStringList selected_profiles_;
};
