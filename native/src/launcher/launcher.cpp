// SPDX-License-Identifier: AGPL-3.0-only

#include "cuajone/launcher_support.hpp"
#include "cuajone/launcher_resources.h"
#include "cuajone/launcher_version.hpp"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincred.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace cuajone::launcher;
using cuajone::kAllowedImageSizes;
using cuajone::kPpeOutputLabels;
using cuajone::RtspTransport;
using cuajone::VideoAcceleration;

constexpr wchar_t kWindowClass[] = L"NexoAIVisionLauncherWindow";
constexpr wchar_t kProductName[] = L"NexoAI Vision";
// Runtime executable produced by the CMake target; keep in sync with CUAJONE_PRODUCT_EXE.
constexpr wchar_t kRuntimeExecutable[] = L"NexoAIVision.exe";

std::string runtimeExecutableName() {
    // The runtime executable name is ASCII-only; no locale conversion needed.
    std::string name;
    for (wchar_t ch : kRuntimeExecutable) {
        name.push_back(static_cast<char>(ch));
    }
    return name;
}
constexpr UINT kProcessFinished = WM_APP + 1;
constexpr ULONGLONG kGracefulStopMilliseconds = 30000;

struct LauncherWindow;
std::filesystem::path siblingRuntime();
void persistPreferences(LauncherWindow& state);
void showError(LauncherWindow& state, const std::exception& error);
void openPpeProfileDialog(LauncherWindow& state);
void openAdvancedSettingsDialog(LauncherWindow& state);
void loadEnv(LauncherWindow& state);
LRESULT CALLBACK thresholdWheelProcedure(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR reference);

enum ControlId : int {
    SourceEdit = 100,
    SourceLabelEdit,
    LanguageButton,
    ThemeButton,
    OpenLogButton,
    OutputEdit,
    OutputBrowse,
    SourceBrowse,
    AnalyticsCombo,
    ComputeCombo,
    ImageSizeCombo,
    RtspTransportCombo,
    VideoAccelerationCombo,
    StreamResolutionCombo,
    StreamFpsCombo,
    PpeThresholdBase = 200,
    PpeEnabledBase = 220,
    ShowCheck = 300,
    ValidateButton,
    StartButton,
    StopButton,
    StatusText,
    LogPathEdit,
    SavedCameraCombo,
    SaveCameraButton,
    LoadCameraButton,
    DeleteCameraButton,
    AddCameraButton,
    EditCameraButton,
    SelectAllCameraButton,
    MenuButton,
    PerformanceMenu = 400,
    AboutMenu,
    IntervalMenuBase = 410,
};

struct LocalizedText {
    HWND control{};
    const wchar_t* english{};
    const wchar_t* spanish{};
};

struct LauncherWindow {
    HWND window{};
    HWND menu_button{};
    bool performance_report{};
    int telemetry_interval_seconds{5};
    HWND source{};
    HWND source_label{};
    HWND saved_camera{};
    HWND save_camera{};
    HWND load_camera{};
    HWND delete_camera{};
    HWND add_camera{};
    HWND edit_camera{};
    HWND select_all_camera{};
    HWND language{};
    HWND theme_button{};
    HWND output{};
    HWND analytics{};
    HWND compute{};
    HWND image_size{};
    HWND rtsp_transport{};
    HWND video_acceleration{};
    HWND stream_resolution{};
    HWND stream_fps{};
    std::array<HWND, kPpeOutputLabels.size()> ppe_thresholds{};
    std::array<HWND, cuajone::kPpeItemCount> ppe_enabled{};
    HWND show{};
    HWND validate{};
    HWND start{};
    HWND stop{};
    HWND status{};
    HWND log_path{};
    std::vector<LocalizedText> localized_text;
    std::vector<std::pair<std::wstring, std::wstring>> runtime_options;
    HFONT font{};
    HFONT heading_font{};
    HFONT button_font{};
    HBRUSH window_brush{};
    HBRUSH input_brush{};
    HBRUSH status_brush{};
    std::filesystem::path program_data;
    std::filesystem::path managed_model_root;
    std::filesystem::path preferences_path;
    OperatorPreferences preferences;
    HANDLE process{};
    HANDLE job{};
    DWORD process_id{};
    std::thread waiter;
    std::thread output_pump;
    std::atomic_bool stop_requested{};
    std::atomic<ULONGLONG> stop_deadline{};
    bool close_requested{};
    bool spanish{};
    bool dark{};
    AnalyticsMode analytics_mode{AnalyticsMode::PpeFall};
    ComputeMode compute_mode{ComputeMode::Auto};
};

struct Palette {
    COLORREF window;
    COLORREF input;
    COLORREF text;
    COLORREF muted;
    COLORREF primary;
    COLORREF primary_pressed;
    COLORREF status;
    COLORREF border;
    COLORREF disabled;
};

Palette palette(const LauncherWindow& state) {
    if (state.dark) {
        return {
            RGB(17, 24, 39), RGB(31, 41, 55), RGB(243, 244, 246), RGB(209, 213, 219),
            RGB(59, 130, 246), RGB(37, 99, 235), RGB(30, 58, 96), RGB(75, 85, 99),
            RGB(55, 65, 81),
        };
    }
    return {
        RGB(246, 248, 252), RGB(255, 255, 255), RGB(31, 41, 55), RGB(75, 85, 99),
        RGB(22, 91, 170), RGB(17, 72, 136), RGB(232, 240, 254), RGB(203, 213, 225),
        RGB(229, 231, 235),
    };
}

std::wstring editText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, result.data(), length + 1);
    result.resize(static_cast<std::size_t>(length));
    return result;
}

void setText(HWND control, const std::filesystem::path& value) {
    SetWindowTextW(control, value.c_str());
}

void setStatus(LauncherWindow& state, std::wstring_view text) {
    SetWindowTextW(state.status, std::wstring(text).c_str());
}

void addLocalizedText(
    LauncherWindow& state,
    HWND control,
    const wchar_t* english,
    const wchar_t* spanish) {
    state.localized_text.push_back({control, english, spanish});
}

void updateAnalyticsOptions(LauncherWindow& state) {
    const LRESULT selection = SendMessageW(state.analytics, CB_GETCURSEL, 0, 0);
    SendMessageW(state.analytics, CB_RESETCONTENT, 0, 0);
    SendMessageW(
        state.analytics, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(state.spanish ? L"Solo EPP" : L"PPE only"));
    SendMessageW(
        state.analytics, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(state.spanish ? L"EPP + caídas" : L"PPE + fall"));
    SendMessageW(state.analytics, CB_SETCURSEL, selection == CB_ERR ? 1 : selection, 0);
}

void refreshLanguage(LauncherWindow& state) {
    for (const auto& text : state.localized_text) {
        SetWindowTextW(text.control, state.spanish ? text.spanish : text.english);
    }
    updateAnalyticsOptions(state);
    const std::wstring current_status = editText(state.status);
    if (current_status == L"Ready" || current_status == L"Listo") {
        setStatus(state, state.spanish ? L"Listo" : L"Ready");
    }
    SetWindowTextW(
        state.language,
        state.spanish ? L"Cambiar idioma a inglés" : L"Switch language to Spanish");
    SetWindowTextW(
        state.theme_button,
        state.dark
            ? (state.spanish ? L"Cambiar a tema claro" : L"Switch to light theme")
            : (state.spanish ? L"Cambiar a tema oscuro" : L"Switch to dark theme"));
}

void showLauncherMenu(LauncherWindow& state) {
    HMENU menu = CreatePopupMenu();
    HMENU intervals = CreatePopupMenu();
    if (!menu || !intervals) {
        if (menu) DestroyMenu(menu);
        if (intervals) DestroyMenu(intervals);
        throw std::runtime_error("Could not create launcher menu");
    }
    const UINT editable = state.process ? MF_GRAYED : MF_ENABLED;
    AppendMenuW(menu, MF_STRING | editable | (state.performance_report ? MF_CHECKED : MF_UNCHECKED),
        PerformanceMenu, state.spanish ? L"&Depuración de rendimiento" : L"&Performance debugging");
    for (std::size_t index = 0; index < kTelemetryIntervals.size(); ++index) {
        const auto seconds = kTelemetryIntervals[index];
        const std::wstring label = std::to_wstring(seconds) + (state.spanish ? L" segundos" : L" seconds");
        AppendMenuW(intervals, MF_STRING, IntervalMenuBase + index, label.c_str());
        if (seconds == state.telemetry_interval_seconds) {
            CheckMenuRadioItem(intervals, IntervalMenuBase,
                IntervalMenuBase + static_cast<UINT>(kTelemetryIntervals.size()) - 1,
                IntervalMenuBase + static_cast<UINT>(index), MF_BYCOMMAND);
        }
    }
    AppendMenuW(menu, MF_POPUP | editable, reinterpret_cast<UINT_PTR>(intervals),
        state.spanish ? L"&Intervalo de telemetría" : L"Telemetry &interval");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | editable, IDM_PPE_PROFILE,
        state.spanish ? L"Perfil de umbrales EPP" : L"PPE threshold profile");
    AppendMenuW(menu, MF_STRING | editable, IDM_ADVANCED_SETTINGS,
        state.spanish ? L"Configuración avanzada" : L"Advanced settings");
    AppendMenuW(menu, MF_STRING | editable, IDM_LOAD_ENV,
        state.spanish ? L"Importar .env" : L"Import .env");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, AboutMenu,
        state.spanish ? L"&Acerca de Nexo AI Vision" : L"&About Nexo AI Vision");
    RECT anchor{};
    GetWindowRect(state.menu_button, &anchor);
    const UINT selected = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        anchor.left, anchor.bottom, state.window, nullptr);
    DestroyMenu(menu);
    // These dialogs are reachable only through the Menu popup (see WM_COMMAND).
    if (selected == IDM_PPE_PROFILE) {
        openPpeProfileDialog(state);
        return;
    }
    if (selected == IDM_ADVANCED_SETTINGS) {
        openAdvancedSettingsDialog(state);
        return;
    }
    if (selected == IDM_LOAD_ENV) {
        loadEnv(state);
        return;
    }
    if (selected == AboutMenu) {
        const auto version = runningLauncherVersion();
        const std::wstring text = (state.spanish
            ? L"Versión del lanzador/producto: " : L"Launcher/product version: ")
            + version.value_or(state.spanish ? L"No disponible (sin recurso de versión legible)"
                : L"Unavailable (no readable version resource)");
        MessageBoxW(state.window, text.c_str(),
            state.spanish ? L"Acerca de Nexo AI Vision" : L"About Nexo AI Vision", MB_OK | MB_ICONINFORMATION);
    } else if (!state.process) {
        if (selected == PerformanceMenu) state.performance_report = !state.performance_report;
        else if (selected >= IntervalMenuBase && selected < IntervalMenuBase + kTelemetryIntervals.size()) {
            state.telemetry_interval_seconds = kTelemetryIntervals[selected - IntervalMenuBase];
        }
        if (selected != 0) {
            setStatus(state, state.spanish
                ? L"Opciones para esta sesión. Iniciar captura; Validar solo comprueba la configuración."
                : L"Session options. Start runs capture; Validate only checks configuration.");
        }
    }
}

std::wstring trim(std::wstring value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t character) {
        return std::iswspace(character) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t character) {
        return std::iswspace(character) != 0;
    }).base();
    return first >= last ? std::wstring{} : std::wstring(first, last);
}

std::wstring upper(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towupper(character));
    });
    return value;
}

std::string utf8FromWide(std::wstring_view value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return result;
}

// Decode an exception message for display: error messages are UTF-8, but fall
// back to the system ANSI codepage so legacy text never shows as mojibake.
std::wstring errorMessageWide(const std::exception& error) {
    const std::string narrow(error.what());
    if (narrow.empty()) return {};
    int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, narrow.data(), static_cast<int>(narrow.size()), nullptr, 0);
    UINT codepage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (length <= 0) {
        length = MultiByteToWideChar(
            CP_ACP, 0, narrow.data(), static_cast<int>(narrow.size()), nullptr, 0);
        codepage = CP_ACP;
        flags = 0;
    }
    if (length <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            codepage, flags, narrow.data(), static_cast<int>(narrow.size()),
            wide.data(), length) <= 0) {
        return {};
    }
    return wide;
}

// Decode one raw .env line: strict UTF-8 first, ANSI fallback for files saved
// with a legacy editor codepage (e.g. cp1252).
std::wstring decodeEnvLine(std::string_view line) {
    if (line.empty()) return {};
    int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, line.data(), static_cast<int>(line.size()), nullptr, 0);
    UINT codepage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (length <= 0) {
        length = MultiByteToWideChar(
            CP_ACP, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
        codepage = CP_ACP;
        flags = 0;
    }
    if (length <= 0) throw std::runtime_error("The selected .env file has undecodable lines");
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            codepage, flags, line.data(), static_cast<int>(line.size()),
            wide.data(), length) <= 0) {
        throw std::runtime_error("The selected .env file has undecodable lines");
    }
    return wide;
}

std::runtime_error savedCameraProfileError(std::string_view action, std::wstring_view profile) {
    return std::runtime_error(std::string(action) + ": " + utf8FromWide(profile));
}

std::optional<std::wstring> selectedSavedCameraProfile(const LauncherWindow& state) {
    const LRESULT selection = SendMessageW(state.saved_camera, LB_GETCURSEL, 0, 0);
    if (selection == LB_ERR) return std::nullopt;
    const LRESULT length = SendMessageW(state.saved_camera, LB_GETTEXTLEN, selection, 0);
    if (length == LB_ERR) return std::nullopt;
    std::wstring profile(static_cast<std::size_t>(length) + 1, L'\0');
    SendMessageW(state.saved_camera, LB_GETTEXT, selection, reinterpret_cast<LPARAM>(profile.data()));
    profile.resize(static_cast<std::size_t>(length));
    return isValidSavedCameraProfileName(profile) ? std::optional<std::wstring>(std::move(profile)) : std::nullopt;
}

std::vector<std::wstring> selectedSavedCameraProfiles(const LauncherWindow& state) {
    const LRESULT count = SendMessageW(state.saved_camera, LB_GETSELCOUNT, 0, 0);
    if (count == LB_ERR || count == 0) return {};
    std::vector<int> indices(static_cast<std::size_t>(count));
    SendMessageW(state.saved_camera, LB_GETSELITEMS, count, reinterpret_cast<LPARAM>(indices.data()));
    std::vector<std::wstring> result;
    for (const int index : indices) {
        const LRESULT length = SendMessageW(state.saved_camera, LB_GETTEXTLEN, index, 0);
        if (length == LB_ERR) continue;
        std::wstring profile(static_cast<std::size_t>(length) + 1, L'\0');
        SendMessageW(state.saved_camera, LB_GETTEXT, index, reinterpret_cast<LPARAM>(profile.data()));
        profile.resize(static_cast<std::size_t>(length));
        if (isValidSavedCameraProfileName(profile)) result.push_back(std::move(profile));
    }
    return result;
}

void refreshSavedCameraProfiles(LauncherWindow& state, std::wstring_view preferred = {}) {
    std::vector<std::wstring> profiles;
    PCREDENTIALW* credentials = nullptr;
    DWORD count{};
    const std::wstring filter = std::wstring(savedCameraCredentialTargetPrefix()) + L"*";
    if (CredEnumerateW(filter.c_str(), 0, &count, &credentials)) {
        for (DWORD index = 0; index < count; ++index) {
            const std::wstring_view target(credentials[index]->TargetName);
            const std::wstring_view prefix = savedCameraCredentialTargetPrefix();
            if (!target.starts_with(prefix)) continue;
            const std::wstring_view profile = target.substr(prefix.size());
            if (isValidSavedCameraProfileName(profile)) profiles.emplace_back(profile);
        }
        CredFree(credentials);
    } else if (GetLastError() != ERROR_NOT_FOUND) {
        throw std::runtime_error("Could not list saved camera profiles");
    }
    std::sort(profiles.begin(), profiles.end());
    profiles.erase(std::unique(profiles.begin(), profiles.end()), profiles.end());
    SendMessageW(state.saved_camera, LB_RESETCONTENT, 0, 0);
    for (const auto& profile : profiles) {
        const LRESULT index = SendMessageW(
            state.saved_camera, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(profile.c_str()));
        if (index != LB_ERR && profile == preferred) {
            SendMessageW(state.saved_camera, LB_SETCURSEL, index, 0);
            SendMessageW(state.saved_camera, LB_SETSEL, TRUE, index);
        }
    }
}

void writeSavedCameraProfile(const CameraConnectionProfile& profile) {
    const std::string payload = serializeCameraConnectionProfile(profile);
    if (payload.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) throw std::invalid_argument("Camera profile is too large");
    const std::wstring target = savedCameraCredentialTarget(profile.name);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<LPWSTR>(target.c_str());
    credential.CredentialBlobSize = static_cast<DWORD>(payload.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(payload.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    if (!CredWriteW(&credential, 0)) {
        throw savedCameraProfileError("Could not save camera profile", profile.name);
    }
}

CameraConnectionProfile readSavedCameraProfile(std::wstring_view profile) {
    const std::wstring target = savedCameraCredentialTarget(profile);
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        throw savedCameraProfileError("Could not load camera profile", profile);
    }
    std::vector<std::byte> blob(credential->CredentialBlobSize);
    if (!blob.empty() && credential->CredentialBlob != nullptr) {
        std::memcpy(blob.data(), credential->CredentialBlob, blob.size());
    }
    CredFree(credential);
    if (blob.empty()) throw savedCameraProfileError("Saved camera profile is invalid", profile);
    try {
        const std::string_view bytes(reinterpret_cast<const char*>(blob.data()), blob.size());
        if (bytes.starts_with("schema_version=")) {
            return parseCameraConnectionProfile(bytes, profile);
        }
        if (blob.size() % sizeof(wchar_t) != 0) throw std::invalid_argument("Invalid legacy profile");
        const auto* text = reinterpret_cast<const wchar_t*>(blob.data());
        return parseLegacyCameraUrl(
            std::wstring_view(text, blob.size() / sizeof(wchar_t)), profile);
    } catch (const std::exception&) {
        throw savedCameraProfileError("Saved camera profile is invalid", profile);
    }
}

void deleteSavedCameraProfile(LauncherWindow& state) {
    const auto profiles = selectedSavedCameraProfiles(state);
    if (profiles.empty()) throw std::invalid_argument("Select one or more saved camera profiles");
    for (const auto& profile : profiles) {
        const std::wstring target = savedCameraCredentialTarget(profile);
        if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
            throw savedCameraProfileError("Could not delete camera profile", profile);
        }
    }
    refreshSavedCameraProfiles(state);
    if (SendMessageW(state.saved_camera, LB_GETCOUNT, 0, 0) == 0) {
        CameraConnectionProfile default_profile;
        default_profile.name = L"CAMARA_AXIS_01";
        writeSavedCameraProfile(default_profile);
        refreshSavedCameraProfiles(state, default_profile.name);
    }
    setStatus(state, state.spanish ? L"Perfiles de cámara eliminados" : L"Camera profiles deleted");
}

std::map<std::wstring, std::wstring> readEnvFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open the selected .env file");
    std::map<std::wstring, std::wstring> values;
    std::string line;
    bool first_line = true;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first_line && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
        first_line = false;
        std::wstring wide = decodeEnvLine(line);
        // Strip a decoded BOM mark (covers UTF-8 BOM and ANSI-decoded "ï»¿").
        if (!wide.empty() && wide.front() == L'\uFEFF') wide.erase(0, 1);
        wide = trim(wide);
        if (wide.empty() || wide.starts_with(L"#")) continue;
        const std::size_t separator = wide.find(L'=');
        if (separator == std::wstring::npos) continue;
        std::wstring key = upper(trim(wide.substr(0, separator)));
        std::wstring value = trim(wide.substr(separator + 1));
        if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
            value = value.substr(1, value.size() - 2);
        }
        if (!key.empty()) values.insert_or_assign(std::move(key), std::move(value));
    }
    return values;
}

std::optional<std::wstring> envValue(
    const std::map<std::wstring, std::wstring>& values,
    std::wstring_view key) {
    const auto found = values.find(std::wstring(key));
    if (found == values.end() || found->second.empty()) return std::nullopt;
    return found->second;
}

std::filesystem::path resolveEnvPath(
    const std::filesystem::path& env_path,
    const std::wstring& configured) {
    std::filesystem::path path(configured);
    return path.is_absolute() ? path : env_path.parent_path() / path;
}

HWND createControl(
    LauncherWindow& state,
    DWORD extended_style,
    const wchar_t* class_name,
    const wchar_t* text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    int id) {
    HWND control = CreateWindowExW(
        extended_style, class_name, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, state.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    if (control == nullptr) throw std::runtime_error("Could not create launcher control");
    const HFONT font = std::wcscmp(class_name, L"BUTTON") == 0 && state.button_font != nullptr
        ? state.button_font
        : state.font;
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return control;
}

HWND createLabel(LauncherWindow& state, const wchar_t* text, int x, int y, int width) {
    return createControl(state, 0, L"STATIC", text, SS_LEFT, x, y + 4, width, 22, 0);
}

HWND createEdit(LauncherWindow& state, int id, int x, int y, int width, bool read_only = false) {
    DWORD style = WS_TABSTOP | ES_AUTOHSCROLL;
    if (read_only) style |= ES_READONLY;
    HWND control = createControl(
        state, WS_EX_CLIENTEDGE, L"EDIT", L"", style, x, y, width, 25, id);
    SendMessageW(control, EM_SETLIMITTEXT, 32767, 0);
    return control;
}

HWND createBrowseButton(LauncherWindow& state, int id, int y) {
    return createControl(
        state, 0, L"BUTTON", L"Browse...", WS_TABSTOP | BS_OWNERDRAW,
        778, y, 92, 25, id);
}

void drawButton(const LauncherWindow& state, const DRAWITEMSTRUCT& item) {
    const Palette colors = palette(state);
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
    const int id = static_cast<int>(item.CtlID);
    const bool primary = id == StartButton;
    const COLORREF fill = disabled ? colors.disabled
        : primary ? (pressed ? colors.primary_pressed : colors.primary)
        : (pressed ? colors.status : colors.input);
    const COLORREF border = disabled ? colors.border : primary ? fill : colors.border;
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_brush = SelectObject(item.hDC, brush);
    HGDIOBJ old_pen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom, 8, 8);
    SelectObject(item.hDC, old_brush);
    SelectObject(item.hDC, old_pen);
    DeleteObject(pen);
    DeleteObject(brush);

    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? RGB(156, 163, 175) : primary ? RGB(255, 255, 255) : colors.text);
    RECT content = item.rcItem;
    if (id == LanguageButton) {
        const int center_x = (content.left + content.right) / 2;
        const int center_y = (content.top + content.bottom) / 2;
        Ellipse(item.hDC, center_x - 8, center_y - 8, center_x + 8, center_y + 8);
        MoveToEx(item.hDC, center_x - 8, center_y, nullptr);
        LineTo(item.hDC, center_x + 8, center_y);
        Arc(item.hDC, center_x - 4, center_y - 8, center_x + 4, center_y + 8,
            center_x, center_y - 8, center_x, center_y + 8);
        return;
    }
    if (id == ThemeButton) {
        const int center_x = (content.left + content.right) / 2;
        const int center_y = (content.top + content.bottom) / 2;
        if (state.dark) {
            HBRUSH moon = CreateSolidBrush(colors.text);
            HGDIOBJ old = SelectObject(item.hDC, moon);
            Ellipse(item.hDC, center_x - 7, center_y - 8, center_x + 8, center_y + 7);
            SelectObject(item.hDC, old);
            DeleteObject(moon);
            HBRUSH cutout = CreateSolidBrush(fill);
            old = SelectObject(item.hDC, cutout);
            Ellipse(item.hDC, center_x - 1, center_y - 9, center_x + 9, center_y + 1);
            SelectObject(item.hDC, old);
            DeleteObject(cutout);
        } else {
            Ellipse(item.hDC, center_x - 5, center_y - 5, center_x + 5, center_y + 5);
            for (int offset : {-10, 10}) {
                MoveToEx(item.hDC, center_x + offset, center_y, nullptr);
                LineTo(item.hDC, center_x + (offset > 0 ? 7 : -7), center_y);
                MoveToEx(item.hDC, center_x, center_y + offset, nullptr);
                LineTo(item.hDC, center_x, center_y + (offset > 0 ? 7 : -7));
            }
        }
        return;
    }
    wchar_t text[128]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    DrawTextW(item.hDC, text, -1, &content, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (id == MenuButton && (item.itemState & ODS_FOCUS) != 0) {
        InflateRect(&content, -4, -4);
        DrawFocusRect(item.hDC, &content);
    }
}

std::filesystem::path knownProgramData() {
    PWSTR raw_path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &raw_path);
    if (FAILED(result) || raw_path == nullptr) {
        throw std::runtime_error("SHGetKnownFolderPath(FOLDERID_ProgramData) failed");
    }
    std::filesystem::path path(raw_path);
    CoTaskMemFree(raw_path);
    return path / kProductName / L"runtime";
}

std::filesystem::path knownLocalAppData() {
    PWSTR raw_path = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw_path);
    if (FAILED(result) || raw_path == nullptr) {
        throw std::runtime_error("SHGetKnownFolderPath(FOLDERID_LocalAppData) failed");
    }
    std::filesystem::path path(raw_path);
    CoTaskMemFree(raw_path);
    return path / kProductName / L"operator-settings-v1.txt";
}

std::filesystem::path preferredModelRoot() {
    return siblingRuntime().parent_path() / L"models";
}

void replaceBrush(HBRUSH& brush, COLORREF color) {
    if (brush != nullptr) DeleteObject(brush);
    brush = CreateSolidBrush(color);
}

void applyTheme(LauncherWindow& state) {
    const Palette colors = palette(state);
    replaceBrush(state.window_brush, colors.window);
    replaceBrush(state.input_brush, colors.input);
    replaceBrush(state.status_brush, colors.status);
    const COLORREF caption = colors.window;
    const COLORREF caption_text = colors.text;
    DwmSetWindowAttribute(
        state.window, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(
        state.window, DWMWA_TEXT_COLOR, &caption_text, sizeof(caption_text));
    RedrawWindow(state.window, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void addTooltip(LauncherWindow& state, HWND control, const wchar_t* text) {
    HWND tooltip = CreateWindowExW(
        WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        state.window, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (tooltip == nullptr) throw std::runtime_error("Could not create launcher tooltip");
    TOOLINFOW info{sizeof(info)};
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = state.window;
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

HWND createClosedCombo(
    LauncherWindow& state,
    int id,
    int x,
    int y,
    int width) {
    return createControl(
        state, 0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST,
        x, y, width, 300, id);
}

void populateThresholdCombo(HWND combo, float selected) {
    for (int hundredths = 0; hundredths <= 100; ++hundredths) {
        wchar_t value[8]{};
        swprintf_s(value, L"%d.%02d", hundredths / 100, hundredths % 100);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    }
    SetWindowTextW(combo, formatPpeConfidenceThreshold(selected).c_str());
    SendMessageW(combo, CB_SETCURSEL,
        static_cast<WPARAM>(std::lround(selected * 100.0F)), 0);
}

void setThresholdComboValue(HWND combo, int hundredths) {
    const int clamped = std::clamp(hundredths, 0, 100);
    const std::wstring text = formatPpeConfidenceThreshold(static_cast<float>(clamped) / 100.0F);
    SendMessageW(combo, CB_SETCURSEL, clamped, 0);
    SetWindowTextW(combo, text.c_str());
}

struct CameraDialogContext {
    CameraConnectionProfile profile;
};

int dialogInteger(HWND dialog, int id, const char* field) {
    const std::wstring value = trim(editText(GetDlgItem(dialog, id)));
    if (value.empty()) throw std::invalid_argument(std::string(field) + " is required");
    std::size_t used{};
    const int result = std::stoi(value, &used);
    if (used != value.size()) throw std::invalid_argument(std::string(field) + " must be an integer");
    return result;
}

INT_PTR CALLBACK cameraDialogProcedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* context = reinterpret_cast<CameraDialogContext*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        context = reinterpret_cast<CameraDialogContext*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(context));
        const auto& profile = context->profile;
        SetDlgItemTextW(dialog, IDC_PROFILE_NAME, profile.name.c_str());
        SetDlgItemTextW(dialog, IDC_CAMERA_USER, profile.username.c_str());
        SetDlgItemTextW(dialog, IDC_CAMERA_PASSWORD, profile.password.c_str());
        SetDlgItemTextW(dialog, IDC_CAMERA_HOST, profile.host.c_str());
        SetDlgItemInt(dialog, IDC_CAMERA_PORT, profile.port, FALSE);
        SetDlgItemTextW(dialog, IDC_CAMERA_PATH, profile.path.c_str());
        for (const auto resolution : kStreamResolutions) {
            const std::wstring value(resolution);
            SendDlgItemMessageW(dialog, IDC_CAMERA_RESOLUTION, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
        }
        for (const int fps : kStreamFrameRates) {
            const std::wstring value = std::to_wstring(fps);
            SendDlgItemMessageW(dialog, IDC_CAMERA_FPS, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
        }
        const auto resolution = std::ranges::find(kStreamResolutions, std::wstring_view(profile.resolution));
        const auto fps = std::ranges::find(kStreamFrameRates, profile.fps);
        SendDlgItemMessageW(dialog, IDC_CAMERA_RESOLUTION, CB_SETCURSEL,
            resolution == kStreamResolutions.end() ? 3 : resolution - kStreamResolutions.begin(), 0);
        SendDlgItemMessageW(dialog, IDC_CAMERA_FPS, CB_SETCURSEL,
            fps == kStreamFrameRates.end() ? 4 : fps - kStreamFrameRates.begin(), 0);
        SetDlgItemInt(dialog, IDC_CAMERA_COMPRESSION, profile.compression, FALSE);
        SetDlgItemInt(dialog, IDC_CAMERA_BITRATE, profile.maximum_bitrate_kbps, FALSE);
        SetDlgItemInt(dialog, IDC_CAMERA_ZIPSTREAM, profile.zipstream_strength, FALSE);
        SetDlgItemInt(dialog, IDC_CAMERA_IFRAME, profile.keyframe_interval, FALSE);
        CheckDlgButton(dialog, IDC_CAMERA_DYNAMIC_FPS, profile.dynamic_fps ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_CAMERA_AUDIO, profile.audio ? BST_CHECKED : BST_UNCHECKED);
        for (const wchar_t* value : {L"RTSP/TCP", L"RTSP/UDP", L"Predeterminado"}) {
            SendDlgItemMessageW(dialog, IDC_CAMERA_TRANSPORT, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
        }
        SendDlgItemMessageW(dialog, IDC_CAMERA_TRANSPORT, CB_SETCURSEL,
            profile.transport == RtspTransport::Udp ? 1 : profile.transport == RtspTransport::Default ? 2 : 0, 0);
        for (const wchar_t* value : {L"Auto", L"D3D11", L"CPU"}) {
            SendDlgItemMessageW(dialog, IDC_CAMERA_ACCELERATION, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
        }
        SendDlgItemMessageW(dialog, IDC_CAMERA_ACCELERATION, CB_SETCURSEL,
            profile.video_acceleration == VideoAcceleration::D3d11 ? 1
                : profile.video_acceleration == VideoAcceleration::Cpu ? 2 : 0, 0);
        return TRUE;
    }
    if (message != WM_COMMAND || context == nullptr) return FALSE;
    if (LOWORD(wparam) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (LOWORD(wparam) != IDOK) return FALSE;
    try {
        auto& profile = context->profile;
        profile.name = trim(editText(GetDlgItem(dialog, IDC_PROFILE_NAME)));
        profile.username = editText(GetDlgItem(dialog, IDC_CAMERA_USER));
        profile.password = editText(GetDlgItem(dialog, IDC_CAMERA_PASSWORD));
        profile.host = trim(editText(GetDlgItem(dialog, IDC_CAMERA_HOST)));
        profile.port = static_cast<std::uint16_t>(dialogInteger(dialog, IDC_CAMERA_PORT, "Port"));
        profile.path = trim(editText(GetDlgItem(dialog, IDC_CAMERA_PATH)));
        const auto resolution = SendDlgItemMessageW(dialog, IDC_CAMERA_RESOLUTION, CB_GETCURSEL, 0, 0);
        const auto fps = SendDlgItemMessageW(dialog, IDC_CAMERA_FPS, CB_GETCURSEL, 0, 0);
        if (resolution == CB_ERR || fps == CB_ERR) throw std::invalid_argument("Resolution and FPS are required");
        profile.resolution = kStreamResolutions[static_cast<std::size_t>(resolution)];
        profile.fps = kStreamFrameRates[static_cast<std::size_t>(fps)];
        profile.compression = dialogInteger(dialog, IDC_CAMERA_COMPRESSION, "Compression");
        profile.maximum_bitrate_kbps = dialogInteger(dialog, IDC_CAMERA_BITRATE, "Maximum bitrate");
        profile.zipstream_strength = dialogInteger(dialog, IDC_CAMERA_ZIPSTREAM, "Zipstream");
        profile.keyframe_interval = dialogInteger(dialog, IDC_CAMERA_IFRAME, "I-frame interval");
        profile.dynamic_fps = IsDlgButtonChecked(dialog, IDC_CAMERA_DYNAMIC_FPS) == BST_CHECKED;
        profile.audio = IsDlgButtonChecked(dialog, IDC_CAMERA_AUDIO) == BST_CHECKED;
        const auto transport = SendDlgItemMessageW(dialog, IDC_CAMERA_TRANSPORT, CB_GETCURSEL, 0, 0);
        profile.transport = transport == 1 ? RtspTransport::Udp : transport == 2 ? RtspTransport::Default : RtspTransport::Tcp;
        const auto acceleration = SendDlgItemMessageW(dialog, IDC_CAMERA_ACCELERATION, CB_GETCURSEL, 0, 0);
        profile.video_acceleration = acceleration == 1 ? VideoAcceleration::D3d11
            : acceleration == 2 ? VideoAcceleration::Cpu : VideoAcceleration::Auto;
        validateCameraConnectionProfile(profile);
        EndDialog(dialog, IDOK);
    } catch (const std::exception& error) {
        const std::wstring error_text = errorMessageWide(error);
        MessageBoxW(dialog, error_text.c_str(), L"Perfil de cámara", MB_OK | MB_ICONERROR);
    }
    return TRUE;
}

bool editCameraProfile(LauncherWindow& state, CameraConnectionProfile& profile) {
    CameraDialogContext context{profile};
    const INT_PTR result = DialogBoxParamW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_CAMERA_PROFILE), state.window,
        cameraDialogProcedure, reinterpret_cast<LPARAM>(&context));
    if (result == -1) throw std::runtime_error("Could not open camera profile dialog");
    if (result != IDOK) return false;
    profile = std::move(context.profile);
    return true;
}

INT_PTR CALLBACK ppeDialogProcedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<LauncherWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
    constexpr std::array<std::size_t, cuajone::kPpeItemCount> item_class_ids{0, 2, 3, 4, 5, 6, 7};
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<LauncherWindow*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        for (std::size_t index = 0; index < kPpeOutputLabels.size(); ++index) {
            populateThresholdCombo(GetDlgItem(dialog, IDC_PPE_THRESHOLD_BASE + static_cast<int>(index)),
                state->preferences.ppe_class_confidences[index]);
        }
        for (std::size_t index = 0; index < item_class_ids.size(); ++index) {
            CheckDlgButton(dialog, IDC_PPE_ENABLED_BASE + static_cast<int>(index),
                state->preferences.ppe_enabled[index] ? BST_CHECKED : BST_UNCHECKED);
        }
        return TRUE;
    }
    if (message != WM_COMMAND || state == nullptr) return FALSE;
    if (LOWORD(wparam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    if (LOWORD(wparam) != IDOK) return FALSE;
    try {
        for (std::size_t index = 0; index < kPpeOutputLabels.size(); ++index) {
            state->preferences.ppe_class_confidences[index] = parsePpeConfidenceThreshold(
                editText(GetDlgItem(dialog, IDC_PPE_THRESHOLD_BASE + static_cast<int>(index))));
        }
        for (std::size_t index = 0; index < item_class_ids.size(); ++index) {
            state->preferences.ppe_enabled[index] = IsDlgButtonChecked(
                dialog, IDC_PPE_ENABLED_BASE + static_cast<int>(index)) == BST_CHECKED;
        }
        saveOperatorPreferencesAtomic(state->preferences_path, state->preferences);
        EndDialog(dialog, IDOK);
    } catch (const std::exception& error) {
        const std::wstring text = errorMessageWide(error);
        MessageBoxW(dialog, text.c_str(), L"Perfil EPP", MB_OK | MB_ICONERROR);
    }
    return TRUE;
}

INT_PTR CALLBACK advancedDialogProcedure(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<LauncherWindow*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<LauncherWindow*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        for (const wchar_t* value : {L"EPP + caídas", L"Solo EPP"}) SendDlgItemMessageW(dialog, IDC_ADV_ANALYTICS, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
        for (const wchar_t* value : {L"Auto", L"CUDA", L"CPU"}) SendDlgItemMessageW(dialog, IDC_ADV_COMPUTE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
        for (const int image_size : kAllowedImageSizes) {
            const std::wstring value = std::to_wstring(image_size);
            SendDlgItemMessageW(dialog, IDC_ADV_IMAGE_SIZE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
        }
        SendDlgItemMessageW(dialog, IDC_ADV_ANALYTICS, CB_SETCURSEL, state->analytics_mode == AnalyticsMode::PpeOnly ? 1 : 0, 0);
        SendDlgItemMessageW(dialog, IDC_ADV_COMPUTE, CB_SETCURSEL, state->compute_mode == ComputeMode::Cuda ? 1 : state->compute_mode == ComputeMode::Cpu ? 2 : 0, 0);
        const auto image = std::ranges::find(kAllowedImageSizes, state->preferences.image_size);
        SendDlgItemMessageW(dialog, IDC_ADV_IMAGE_SIZE, CB_SETCURSEL, image == kAllowedImageSizes.end() ? 0 : image - kAllowedImageSizes.begin(), 0);
        return TRUE;
    }
    if (message != WM_COMMAND || state == nullptr) return FALSE;
    if (LOWORD(wparam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    if (LOWORD(wparam) != IDOK) return FALSE;
    const auto analytics = SendDlgItemMessageW(dialog, IDC_ADV_ANALYTICS, CB_GETCURSEL, 0, 0);
    const auto compute = SendDlgItemMessageW(dialog, IDC_ADV_COMPUTE, CB_GETCURSEL, 0, 0);
    const auto image = SendDlgItemMessageW(dialog, IDC_ADV_IMAGE_SIZE, CB_GETCURSEL, 0, 0);
    if (analytics == CB_ERR || compute == CB_ERR || image == CB_ERR) return TRUE;
    state->analytics_mode = analytics == 1 ? AnalyticsMode::PpeOnly : AnalyticsMode::PpeFall;
    state->compute_mode = compute == 1 ? ComputeMode::Cuda : compute == 2 ? ComputeMode::Cpu : ComputeMode::Auto;
    state->preferences.image_size = kAllowedImageSizes[static_cast<std::size_t>(image)];
    saveOperatorPreferencesAtomic(state->preferences_path, state->preferences);
    EndDialog(dialog, IDOK);
    return TRUE;
}

void openPpeProfileDialog(LauncherWindow& state) {
    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_PPE_PROFILE),
            state.window, ppeDialogProcedure, reinterpret_cast<LPARAM>(&state)) == -1) {
        throw std::runtime_error("Could not open PPE profile dialog");
    }
}

void openAdvancedSettingsDialog(LauncherWindow& state) {
    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ADVANCED_SETTINGS),
            state.window, advancedDialogProcedure, reinterpret_cast<LPARAM>(&state)) == -1) {
        throw std::runtime_error("Could not open advanced settings dialog");
    }
}

void addCameraProfile(LauncherWindow& state) {
    CameraConnectionProfile profile;
    for (int suffix = 1; suffix < 1000; ++suffix) {
        profile.name = L"CAMARA_" + std::to_wstring(suffix);
        PCREDENTIALW existing = nullptr;
        if (!CredReadW(savedCameraCredentialTarget(profile.name).c_str(), CRED_TYPE_GENERIC, 0, &existing)) break;
        CredFree(existing);
    }
    if (!editCameraProfile(state, profile)) return;
    writeSavedCameraProfile(profile);
    refreshSavedCameraProfiles(state, profile.name);
    setStatus(state, (state.spanish ? L"Perfil creado: " : L"Created profile: ") + profile.name);
}

void editSelectedCameraProfile(LauncherWindow& state) {
    const auto selected = selectedSavedCameraProfiles(state);
    if (selected.size() != 1) throw std::invalid_argument("Select exactly one camera profile to edit");
    const std::wstring original = selected.front();
    CameraConnectionProfile profile = readSavedCameraProfile(original);
    if (!editCameraProfile(state, profile)) return;
    writeSavedCameraProfile(profile);
    if (profile.name != original) CredDeleteW(savedCameraCredentialTarget(original).c_str(), CRED_TYPE_GENERIC, 0);
    refreshSavedCameraProfiles(state, profile.name);
    setStatus(state, (state.spanish ? L"Perfil actualizado: " : L"Updated profile: ") + profile.name);
}

void selectAllCameraProfiles(LauncherWindow& state) {
    SendMessageW(state.saved_camera, LB_SETSEL, TRUE, -1);
    const auto count = SendMessageW(state.saved_camera, LB_GETSELCOUNT, 0, 0);
    setStatus(state, (state.spanish ? L"Cámaras seleccionadas: " : L"Selected cameras: ") + std::to_wstring(count));
}

HWND createThresholdCombo(LauncherWindow& state, int id, int x, int y, int width) {
    return createControl(
        state, 0, WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL,
        x, y, width, 300, id);
}

void createControls(LauncherWindow& state) {
    state.font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    state.heading_font = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    state.button_font = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    state.spanish = state.preferences.language == UiLanguage::Spanish;
    state.dark = state.preferences.theme == ThemeMode::Dark;
    state.preferences.show_window = true;
    applyTheme(state);

    state.menu_button = createControl(state, 0, L"BUTTON", L"Menú ▾", WS_TABSTOP | BS_OWNERDRAW,
        16, 18, 100, 30, MenuButton);
    addLocalizedText(state, state.menu_button, L"Menu ▾", L"Menú ▾");
    const HWND heading = createControl(state, 0, L"STATIC", kProductName, SS_LEFT, 130, 14, 400, 30, 0);
    SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(state.heading_font), TRUE);
    const HWND subtitle = createControl(state, 0, L"STATIC", L"Multi-camera analytics control center", SS_LEFT, 132, 46, 500, 20, 0);
    addLocalizedText(state, subtitle, L"Multi-camera analytics control center", L"Centro de analítica multicámara");
    state.language = createControl(state, 0, L"BUTTON", L"ES", WS_TABSTOP | BS_OWNERDRAW, 810, 18, 34, 30, LanguageButton);
    state.theme_button = createControl(state, 0, L"BUTTON", L"◐", WS_TABSTOP | BS_OWNERDRAW, 852, 18, 34, 30, ThemeButton);
    addTooltip(state, state.language, L"Language / Idioma");
    addTooltip(state, state.theme_button, L"Light or dark theme / Tema claro u oscuro");

    const HWND camera_heading = createLabel(state, L"Camera profiles", 16, 82, 280);
    addLocalizedText(state, camera_heading, L"Camera profiles", L"Perfiles de conexión a cámaras");
    state.saved_camera = createControl(state, WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_TABSTOP | WS_VSCROLL | LBS_EXTENDEDSEL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
        16, 106, 650, 190, SavedCameraCombo);
    state.add_camera = createControl(state, 0, L"BUTTON", L"New...", WS_TABSTOP | BS_OWNERDRAW,
        682, 106, 188, 29, AddCameraButton);
    addLocalizedText(state, state.add_camera, L"New profile...", L"Nuevo perfil...");
    state.edit_camera = createControl(state, 0, L"BUTTON", L"Edit...", WS_TABSTOP | BS_OWNERDRAW,
        682, 143, 188, 29, EditCameraButton);
    addLocalizedText(state, state.edit_camera, L"Edit profile...", L"Editar perfil...");
    state.delete_camera = createControl(state, 0, L"BUTTON", L"Delete", WS_TABSTOP | BS_OWNERDRAW,
        682, 180, 188, 29, DeleteCameraButton);
    addLocalizedText(state, state.delete_camera, L"Delete selected", L"Eliminar selección");
    state.select_all_camera = createControl(state, 0, L"BUTTON", L"Select all", WS_TABSTOP | BS_OWNERDRAW,
        682, 217, 188, 29, SelectAllCameraButton);
    addLocalizedText(state, state.select_all_camera, L"Select all cameras", L"Seleccionar todas");
    const HWND sharing = createControl(state, 0, L"STATIC",
        L"Selected cameras share one inference engine; tracking remains isolated per camera.",
        SS_LEFT, 16, 302, 850, 22, 0);
    addLocalizedText(state, sharing,
        L"Selected cameras share one inference engine; tracking remains isolated per camera.",
        L"Las cámaras seleccionadas comparten un motor de inferencia; el seguimiento se aísla por cámara.");

    const HWND output_label = createLabel(state, L"Output folder", 16, 336, 120);
    addLocalizedText(state, output_label, L"Output folder", L"Carpeta de salida");
    state.output = createEdit(state, OutputEdit, 146, 332, 620);
    addLocalizedText(state, createBrowseButton(state, OutputBrowse, 332), L"Browse...", L"Explorar...");

    // PPE profile, advanced settings and .env import live only in the Menu popup
    // (see showLauncherMenu); the main layout keeps Validate/Start/Stop here.
    state.validate = createControl(state, 0, L"BUTTON", L"Validate", WS_TABSTOP | BS_OWNERDRAW,
        146, 374, 110, 32, ValidateButton);
    addLocalizedText(state, state.validate, L"Validate", L"Validar");
    state.start = createControl(state, 0, L"BUTTON", L"Start", WS_TABSTOP | BS_OWNERDRAW,
        270, 374, 110, 32, StartButton);
    addLocalizedText(state, state.start, L"Start", L"Iniciar");
    state.stop = createControl(state, 0, L"BUTTON", L"Stop", WS_TABSTOP | BS_OWNERDRAW,
        394, 374, 110, 32, StopButton);
    addLocalizedText(state, state.stop, L"Stop", L"Detener");
    EnableWindow(state.stop, FALSE);

    const HWND status_label = createLabel(state, L"Status", 16, 422, 120);
    addLocalizedText(state, status_label, L"Status", L"Estado");
    state.status = createControl(state, WS_EX_CLIENTEDGE, L"STATIC", L"Ready", SS_LEFT | SS_CENTERIMAGE,
        146, 418, 724, 32, StatusText);
    const HWND log_label = createLabel(state, L"Log path", 16, 462, 120);
    addLocalizedText(state, log_label, L"Log path", L"Ruta del log");
    state.log_path = createEdit(state, LogPathEdit, 146, 458, 620, true);
    const HWND open_log = createControl(state, 0, L"BUTTON", L"Open log", WS_TABSTOP | BS_OWNERDRAW,
        778, 458, 92, 25, OpenLogButton);
    addLocalizedText(state, open_log, L"Open log", L"Abrir log");

    state.program_data = knownProgramData();
    state.managed_model_root = preferredModelRoot();
    // Dev-layout fallback: a freshly built launcher runs next to the build
    // tree where <exe>/models does not exist. Adopt the first candidate root
    // holding a complete strict bundle (for example the staged installer
    // bundle) so Validate/Start work without an MSI install. The installed
    // layout keeps the preferred root, which is always tried first.
    if (const auto best = resolveBestManagedModelSet(
            managedModelRootCandidates(state.managed_model_root), true)) {
        state.managed_model_root = best->root;
    }
    setText(state.output, state.program_data / L"output");
    setText(state.log_path, state.program_data / L"logs");
    refreshSavedCameraProfiles(state);
    if (SendMessageW(state.saved_camera, LB_GETCOUNT, 0, 0) == 0) {
        CameraConnectionProfile default_profile;
        default_profile.name = L"CAMARA_AXIS_01";
        writeSavedCameraProfile(default_profile);
        refreshSavedCameraProfiles(state, default_profile.name);
    }
    if (!resolveManagedModelSet(state.managed_model_root, true).onnx_complete) {
        setStatus(state, state.spanish
            ? L"El conjunto obligatorio de modelos está incompleto."
            : L"The mandatory managed model set is incomplete.");
    }
    refreshLanguage(state);
}

std::filesystem::path pickFile(HWND owner, const COMDLG_FILTERSPEC* filters, UINT filter_count) {
    IFileOpenDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(result)) return {};
    DWORD options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dialog->SetFileTypes(filter_count, filters);
    result = dialog->Show(owner);
    std::filesystem::path path;
    if (SUCCEEDED(result)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR raw_path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path))) {
                path = raw_path;
                CoTaskMemFree(raw_path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return path;
}

void loadEnv(LauncherWindow& state) {
    static constexpr COMDLG_FILTERSPEC filters[] = {
        {L"Environment file", L"*.env;*.txt"}, {L"All files", L"*.*"},
    };
    const std::filesystem::path env_path = pickFile(
        state.window, filters, static_cast<UINT>(std::size(filters)));
    if (env_path.empty()) return;

    const auto values = readEnvFile(env_path);
    std::optional<CameraConnectionProfile> imported_camera;
    if (const auto source = envValue(values, L"RTSP_URL")) {
        const std::wstring name = envValue(values, L"CAMERA_ID").value_or(L"CAMARA_IMPORTADA");
        imported_camera = parseLegacyCameraUrl(*source, name);
    }
    if (const auto output = envValue(values, L"OUTPUT_DIR")) {
        setText(state.output, resolveEnvPath(env_path, *output));
    }
    if (const auto mode = envValue(values, L"ANALYTICS_MODE")) {
        state.analytics_mode = *mode == L"ppe-only" ? AnalyticsMode::PpeOnly : AnalyticsMode::PpeFall;
    }
    if (const auto transport = envValue(values, L"RTSP_TRANSPORT")) {
        const std::wstring mode = upper(*transport);
        if (mode != L"TCP" && mode != L"UDP" && mode != L"DEFAULT") {
            throw std::invalid_argument("RTSP_TRANSPORT must be tcp, udp, or default");
        }
        if (imported_camera) imported_camera->transport = mode == L"UDP" ? RtspTransport::Udp
            : mode == L"DEFAULT" ? RtspTransport::Default : RtspTransport::Tcp;
    }
    if (const auto acceleration = envValue(values, L"VIDEO_ACCELERATION")) {
        const std::wstring mode = upper(*acceleration);
        if (mode != L"AUTO" && mode != L"D3D11" && mode != L"CPU") {
            throw std::invalid_argument("VIDEO_ACCELERATION must be auto, d3d11, or cpu");
        }
        if (imported_camera) imported_camera->video_acceleration = mode == L"D3D11" ? VideoAcceleration::D3d11
            : mode == L"CPU" ? VideoAcceleration::Cpu : VideoAcceleration::Auto;
    }
    if (const auto resolution = envValue(values, L"RTSP_RESOLUTION")) {
        const auto found = std::ranges::find(kStreamResolutions, std::wstring_view(*resolution));
        if (found == kStreamResolutions.end()) {
            throw std::invalid_argument("RTSP_RESOLUTION is not supported by the launcher");
        }
        if (imported_camera) imported_camera->resolution = *resolution;
    }
    if (const auto fps = envValue(values, L"RTSP_FPS")) {
        wchar_t* end = nullptr;
        const long parsed = std::wcstol(fps->c_str(), &end, 10);
        const auto found = std::ranges::find(kStreamFrameRates, static_cast<int>(parsed));
        if (end != fps->c_str() + fps->size() || found == kStreamFrameRates.end()) {
            throw std::invalid_argument("RTSP_FPS must be 5, 10, 15, 20, 25, or 30");
        }
        if (imported_camera) imported_camera->fps = static_cast<int>(parsed);
    }
    if (const auto confidence = envValue(values, L"PPE_CONF")) {
        int selection{};
        try {
            selection = static_cast<int>(std::lround(parsePpeConfidenceThreshold(*confidence) * 100.0F));
        } catch (const std::invalid_argument&) {
            throw std::invalid_argument("PPE_CONF must be a decimal from 0.00 to 1.00");
        }
        state.preferences.ppe_class_confidences.fill(static_cast<float>(selection) / 100.0F);
    }
    const auto ppe_imgsz = envValue(values, L"PPE_IMGSZ");
    const auto pose_imgsz = envValue(values, L"POSE_IMGSZ");
    if (ppe_imgsz || pose_imgsz) {
        if (ppe_imgsz && pose_imgsz && *ppe_imgsz != *pose_imgsz) {
            throw std::invalid_argument("PPE_IMGSZ and POSE_IMGSZ must match");
        }
        const std::wstring& configured = ppe_imgsz ? *ppe_imgsz : *pose_imgsz;
        wchar_t* end = nullptr;
        const long parsed = std::wcstol(configured.c_str(), &end, 10);
        const auto found = std::ranges::find(kAllowedImageSizes, static_cast<int>(parsed));
        if (end != configured.c_str() + configured.size() || found == kAllowedImageSizes.end()) {
            throw std::invalid_argument("PPE_IMGSZ/POSE_IMGSZ must be 640, 768, 960, or 1280");
        }
        state.preferences.image_size = static_cast<int>(parsed);
    }

    std::vector<std::wstring> ignored;
    state.runtime_options.clear();
    const auto append = [&](std::wstring_view key, std::wstring_view option) {
        if (const auto value = envValue(values, key)) {
            state.runtime_options.emplace_back(std::wstring(option), *value);
        }
    };
    append(L"TARGET_INFERENCE_FPS", L"--target-fps");
    append(L"EVIDENCE_WRITER_QUEUE_CAPACITY", L"--evidence-writer-queue-capacity");
    append(L"POSE_CONF", L"--pose-conf");
    append(L"IOU_THRESHOLD", L"--nms-iou");
    append(L"EPP_WINDOW", L"--ppe-window");
    append(L"EPP_MIN_SAMPLES", L"--ppe-min-samples");
    append(L"EPP_PRESENT_RATIO", L"--ppe-present-ratio");
    append(L"EPP_ALERT_COOLDOWN_S", L"--ppe-cooldown");
    append(L"FALL_CONFIRM_FRAMES", L"--fall-confirm-frames");
    append(L"FALL_RESET_FRAMES", L"--fall-reset-frames");
    append(L"FALL_ALERT_COOLDOWN_S", L"--fall-cooldown");
    append(L"FALL_ASPECT_RATIO", L"--fall-aspect-ratio");
    append(L"FALL_TORSO_ANGLE_DEG", L"--fall-torso-angle");
    append(L"FALL_DESCENT_RATIO", L"--fall-descent-ratio");
    append(L"FALL_NEAR_FLOOR_RATIO", L"--fall-near-floor-ratio");
    if (const auto ttl = envValue(values, L"TRACK_TTL_S")) {
        state.runtime_options.emplace_back(L"--ppe-track-ttl", *ttl);
        state.runtime_options.emplace_back(L"--fall-track-ttl", *ttl);
    }
    append(L"RECONNECT_DELAY_S", L"--reconnect-delay");
    append(L"RTSP_OPEN_TIMEOUT_MS", L"--capture-open-timeout-ms");
    append(L"RTSP_READ_TIMEOUT_MS", L"--capture-read-timeout-ms");
    if (const auto device = envValue(values, L"YOLO_DEVICE")) {
        if (upper(*device) != L"CPU" && upper(*device) != L"AUTO") {
            state.runtime_options.emplace_back(L"--device", *device);
        }
    }

    for (const std::wstring_view key : {
             L"YOLO_TRACKER", L"USE_FP16", L"SHOW_TEMPORARY_TRACK_ID",
             L"EXCEL_EXPORT_EVERY_EVENTS",
             L"PPE_MODEL_PATH", L"POSE_MODEL_PATH", L"RTSP_SOCKET_TIMEOUT_S"}) {
        if (envValue(values, key)) ignored.emplace_back(key);
    }
    if (imported_camera) {
        writeSavedCameraProfile(*imported_camera);
        refreshSavedCameraProfiles(state, imported_camera->name);
    }
    if (ignored.empty()) {
        setStatus(state, state.spanish ? L"Configuración .env cargada" : L"Loaded .env settings");
    } else {
        setStatus(
            state,
            state.spanish
                ? L".env cargado; se ignoraron opciones exclusivas de Python"
                : L"Loaded .env; Python-only settings were ignored");
    }
    persistPreferences(state);
}

std::filesystem::path pickFolder(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(result)) return {};
    DWORD options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
    result = dialog->Show(owner);
    std::filesystem::path path;
    if (SUCCEEDED(result)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR raw_path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path))) {
                path = raw_path;
                CoTaskMemFree(raw_path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return path;
}

std::filesystem::path pickVideoFile(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(result)) return {};
    DWORD options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
    const COMDLG_FILTERSPEC filters[] = {
        {L"Video files", L"*.mp4;*.avi;*.mov;*.mkv"},
        {L"All files", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    result = dialog->Show(owner);
    std::filesystem::path path;
    if (SUCCEEDED(result)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR raw_path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path))) {
                path = raw_path;
                CoTaskMemFree(raw_path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return path;
}

LauncherSettings readSettings(const LauncherWindow& state) {
    LauncherSettings settings;
    settings.performance_report = state.performance_report;
    settings.telemetry_interval_seconds = state.telemetry_interval_seconds;
    settings.output = editText(state.output);
    settings.analytics_mode = state.analytics_mode;
    settings.compute_mode = state.compute_mode;
    settings.managed_model_root = state.managed_model_root;
    // Lets Validate/Start search ordered dev candidate roots when the stored
    // root has no complete set. The installed bundle path stays strict.
    settings.allow_dev_model_fallback = true;
    const auto selected = selectedSavedCameraProfiles(state);
    for (const auto& name : selected) {
        const auto profile = readSavedCameraProfile(name);
        settings.cameras.push_back({
            buildAxisRtspUrl(profile), profile.name, profile.transport, profile.video_acceleration});
    }
    settings.runtime_options = state.runtime_options;
    settings.image_size = state.preferences.image_size;
    settings.ppe_class_confidences = state.preferences.ppe_class_confidences;
    settings.ppe_enabled = state.preferences.ppe_enabled;
    settings.show_window = true;
    return settings;
}

void persistPreferences(LauncherWindow& state) {
    state.preferences.language = state.spanish ? UiLanguage::Spanish : UiLanguage::English;
    state.preferences.theme = state.dark ? ThemeMode::Dark : ThemeMode::Light;
    state.preferences.show_window = true;
    saveOperatorPreferencesAtomic(state.preferences_path, state.preferences);
}

std::filesystem::path siblingRuntime() {
    std::wstring module_path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (length == 0 || length == static_cast<DWORD>(module_path.size())) {
        throw std::runtime_error("Could not resolve the launcher executable path");
    }
    module_path.resize(length);
    return std::filesystem::path(module_path).parent_path() / kRuntimeExecutable;
}

// Double-click support without activate-native.ps1: dev build output lives at
// <repo>/.tools/native/build/presets/<preset>/ while third-party DLLs live in
// <repo>/.tools/native/{opencv/.../bin, onnxruntime-*/lib, cuda-runtime/...}.
// An Explorer launch inherits the plain system PATH, so the runtime child
// would fail with "opencv_world4120.dll was not found". Resolve those roots
// relative to this executable and prepend the ones that exist to this process
// PATH; the CreateProcessW child in launchRuntime() inherits it. Installed
// (MSI) layouts have no .tools tree, so every candidate is skipped and PATH is
// left untouched — the MSI relies solely on its app-local bin\ closure.
void prependDevNativeDllRoots() {
    std::wstring module_path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (length == 0 || length == static_cast<DWORD>(module_path.size())) return;
    module_path.resize(length);
    std::filesystem::path repo_root =
        std::filesystem::path(module_path).parent_path();
    // <preset> -> presets -> build -> native -> .tools -> <repo>.
    for (int level = 0; level < 5; ++level) {
        if (!repo_root.has_parent_path()) return;
        repo_root = repo_root.parent_path();
    }
    std::error_code marker_error;
    const std::filesystem::path native_root = repo_root / L".tools" / L"native";
    if (!std::filesystem::is_directory(native_root, marker_error) || marker_error) {
        return;  // Installed layout: keep the MSI app-local closure untouched.
    }
    const std::vector<std::filesystem::path> candidates{
        native_root / L"opencv" / L"opencv" / L"build" / L"x64" / L"vc16" / L"bin",
        native_root / L"onnxruntime-win-x64-1.25.0" / L"lib",
        native_root / L"onnxruntime-win-x64-gpu-1.25.0" / L"lib",
        native_root / L"cuda-runtime" / L"nvidia" / L"cuda_runtime" / L"bin",
        native_root / L"nvidia-libraries" / L"cublas" / L"nvidia" / L"cublas" / L"bin",
        native_root / L"nvidia-libraries" / L"cudnn" / L"nvidia" / L"cudnn" / L"bin",
        native_root / L"nvidia-libraries" / L"cufft" / L"nvidia" / L"cufft" / L"bin",
    };
    std::wstring current(32768, L'\0');
    const DWORD current_length = GetEnvironmentVariableW(
        L"PATH", current.data(), static_cast<DWORD>(current.size()));
    if (current_length >= current.size()) return;  // PATH unexpectedly huge; leave it.
    current.resize(current_length);
    std::wstring current_lower = current;
    std::transform(current_lower.begin(), current_lower.end(), current_lower.begin(), ::towlower);
    std::wstring prefix;
    for (const auto& candidate : candidates) {
        std::error_code candidate_error;
        if (!std::filesystem::is_directory(candidate, candidate_error) || candidate_error) {
            continue;
        }
        std::wstring entry = candidate.wstring();
        std::wstring entry_lower = entry;
        std::transform(entry_lower.begin(), entry_lower.end(), entry_lower.begin(), ::towlower);
        if (current_lower.find(entry_lower) != std::wstring::npos) continue;
        if (!prefix.empty()) prefix.push_back(L';');
        prefix += entry;
    }
    if (prefix.empty()) return;
    const std::wstring updated = prefix + (current.empty() ? L"" : L";" + current);
    SetEnvironmentVariableW(L"PATH", updated.c_str());
}

std::filesystem::path nextLogPath(const LauncherWindow& state) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t name[64]{};
    swprintf_s(
        name, L"cuajone-%04u%02u%02u-%02u%02u%02u-%03u.log",
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    return state.program_data / L"logs" / name;
}

void setRunning(LauncherWindow& state, bool running) {
    EnableWindow(state.validate, !running);
    EnableWindow(state.start, !running);
    EnableWindow(state.stop, running);
    EnableWindow(state.saved_camera, !running);
    EnableWindow(state.add_camera, !running);
    EnableWindow(state.edit_camera, !running);
    EnableWindow(state.delete_camera, !running);
    EnableWindow(state.select_all_camera, !running);
}

void closeProcessHandles(LauncherWindow& state) {
    if (state.process != nullptr) CloseHandle(state.process);
    if (state.job != nullptr) CloseHandle(state.job);
    state.process = nullptr;
    state.job = nullptr;
    state.process_id = 0;
}

void writeLog(HANDLE log, std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const DWORD remaining = static_cast<DWORD>(std::min<std::size_t>(
            text.size() - offset, MAXDWORD));
        DWORD written{};
        if (!WriteFile(log, text.data() + offset, remaining, &written, nullptr)
            || written == 0) {
            return;
        }
        offset += written;
    }
}

void launchRuntime(LauncherWindow& state, bool preflight) {
    if (state.process != nullptr) return;
    const LauncherSettings settings = readSettings(state);
    const LaunchPlan plan = buildLaunchPlan(settings, preflight);
    const std::filesystem::path runtime = siblingRuntime();
    std::error_code file_error;
    if (!std::filesystem::is_regular_file(runtime, file_error) || file_error) {
        throw std::runtime_error("Sibling " + runtimeExecutableName() + " was not found");
    }

    std::filesystem::create_directories(settings.output);
    const std::filesystem::path log_path = nextLogPath(state);
    std::filesystem::create_directories(log_path.parent_path());
    setText(state.log_path, log_path);

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE pipe_read = nullptr;
    HANDLE pipe_write = nullptr;
    if (!CreatePipe(&pipe_read, &pipe_write, &security, 0)
        || !SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0)) {
        if (pipe_read != nullptr) CloseHandle(pipe_read);
        if (pipe_write != nullptr) CloseHandle(pipe_write);
        throw std::runtime_error("Could not create the child output pipe");
    }
    HANDLE log = CreateFileW(
        log_path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_read);
        CloseHandle(pipe_write);
        throw std::runtime_error("Could not create the ProgramData log");
    }
    HANDLE null_input = CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_input == INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_read);
        CloseHandle(pipe_write);
        CloseHandle(log);
        throw std::runtime_error("Could not open NUL for child input");
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        CloseHandle(pipe_read);
        CloseHandle(pipe_write);
        CloseHandle(null_input);
        CloseHandle(log);
        throw std::runtime_error("Could not create the runtime Job Object");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job);
        CloseHandle(pipe_read);
        CloseHandle(pipe_write);
        CloseHandle(null_input);
        CloseHandle(log);
        throw std::runtime_error("Could not configure KILL_ON_JOB_CLOSE");
    }

    std::vector<std::wstring> command_arguments{runtime.wstring()};
    command_arguments.insert(
        command_arguments.end(), plan.arguments.begin(), plan.arguments.end());
    std::wstring command_line = buildWindowsCommandLine(command_arguments);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_input;
    startup.hStdOutput = pipe_write;
    startup.hStdError = pipe_write;
    PROCESS_INFORMATION process{};
    const std::wstring working_directory = runtime.parent_path().wstring();
    // NOTE: the child inherits this process environment, including the dev
    // .tools DLL roots prepended by prependDevNativeDllRoots() (MSI installs
    // rely on the app-local bin\ closure instead). Keep lpEnvironment nullptr.
    const BOOL created = CreateProcessW(
        runtime.c_str(), command_line.data(), nullptr, nullptr, TRUE,
        CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP,
        nullptr, working_directory.c_str(), &startup, &process);
    CloseHandle(null_input);
    CloseHandle(pipe_write);
    if (!created) {
        CloseHandle(pipe_read);
        CloseHandle(log);
        CloseHandle(job);
        throw std::runtime_error("CreateProcessW failed for " + runtimeExecutableName());
    }
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(pipe_read);
        CloseHandle(log);
        CloseHandle(job);
        throw std::runtime_error("Could not assign " + runtimeExecutableName() + " to its Job Object");
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        TerminateJobObject(job, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(pipe_read);
        CloseHandle(log);
        CloseHandle(job);
        throw std::runtime_error("Could not resume " + runtimeExecutableName());
    }
    CloseHandle(process.hThread);

    state.process = process.hProcess;
    state.job = job;
    state.process_id = process.dwProcessId;
    state.stop_requested.store(false, std::memory_order_relaxed);
    state.stop_deadline.store(0, std::memory_order_relaxed);

    try {
        state.output_pump = std::thread([pipe_read, log] {
            std::string pending;
            char buffer[4096];
            DWORD bytes_read{};
            while (ReadFile(pipe_read, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read != 0) {
                pending.append(buffer, bytes_read);
                std::size_t newline{};
                while ((newline = pending.find('\n')) != std::string::npos) {
                    const std::string safe = redactRtspCredentials(
                        std::string_view(pending).substr(0, newline + 1));
                    writeLog(log, safe);
                    pending.erase(0, newline + 1);
                }
            }
            if (!pending.empty()) writeLog(log, redactRtspCredentials(pending));
            FlushFileBuffers(log);
            CloseHandle(log);
            CloseHandle(pipe_read);
        });

        state.waiter = std::thread([&state, preflight, process_handle = state.process, job_handle = state.job] {
            bool forced = false;
            while (WaitForSingleObject(process_handle, 100) == WAIT_TIMEOUT) {
                if (!forced && state.stop_requested.load(std::memory_order_relaxed)
                    && GetTickCount64() >= state.stop_deadline.load(std::memory_order_relaxed)) {
                    TerminateJobObject(job_handle, 130);
                    forced = true;
                }
            }
            DWORD exit_code = 1;
            GetExitCodeProcess(process_handle, &exit_code);
            PostMessageW(
                state.window, kProcessFinished,
                static_cast<WPARAM>(exit_code), static_cast<LPARAM>(preflight));
        });
    } catch (...) {
        TerminateJobObject(state.job, 1);
        if (state.waiter.joinable()) state.waiter.join();
        if (state.output_pump.joinable()) state.output_pump.join();
        else {
            CloseHandle(log);
            CloseHandle(pipe_read);
        }
        closeProcessHandles(state);
        throw;
    }
    setRunning(state, true);
    setStatus(
        state,
        preflight
            ? (state.spanish ? L"Validando configuración..." : L"Validating configuration...")
            : (state.spanish ? L"El runtime está en ejecución" : L"Runtime is running"));
}

void requestStop(LauncherWindow& state) {
    if (state.process == nullptr || state.stop_requested.exchange(true)) return;
    state.stop_deadline.store(
        GetTickCount64() + kGracefulStopMilliseconds, std::memory_order_relaxed);
    const BOOL signaled = GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, state.process_id);
    EnableWindow(state.stop, FALSE);
    setStatus(
        state,
        signaled
            ? (state.spanish
                ? L"Deteniendo de forma segura; se forzará después de 30 segundos"
                : L"Stopping gracefully; forced termination follows after 30 seconds")
            : (state.spanish
                ? L"Falló CTRL_BREAK; se forzará la detención después de 30 segundos"
                : L"CTRL_BREAK delivery failed; forced termination follows after 30 seconds"));
}

void finishProcess(LauncherWindow& state, DWORD exit_code, bool preflight) {
    const bool stopped = state.stop_requested.load(std::memory_order_relaxed);
    if (state.waiter.joinable()) state.waiter.join();
    if (state.output_pump.joinable()) state.output_pump.join();
    closeProcessHandles(state);
    setRunning(state, false);
    if (stopped) {
        setStatus(
            state,
            (state.spanish ? L"Runtime detenido (código de salida " : L"Runtime stopped (exit code ")
                + std::to_wstring(exit_code) + L")");
    } else if (exit_code == 0) {
        setStatus(
            state,
            preflight
                ? (state.spanish ? L"Validación aprobada" : L"Validation passed")
                : (state.spanish ? L"Runtime completado correctamente" : L"Runtime completed successfully"));
    } else {
        setStatus(
            state,
            (preflight
                ? (state.spanish ? L"Falló la validación (código de salida " : L"Validation failed (exit code ")
                : (state.spanish ? L"Falló el runtime (código de salida " : L"Runtime failed (exit code "))
                + std::to_wstring(exit_code)
                + (state.spanish ? L"); consulta el log" : L"); see log"));
    }
    state.stop_requested.store(false, std::memory_order_relaxed);
    if (state.close_requested) DestroyWindow(state.window);
}

void showError(LauncherWindow& state, const std::exception& error) {
    const std::wstring message = errorMessageWide(error);
    setStatus(state, state.spanish ? L"Error de configuración" : L"Configuration error");
    MessageBoxW(state.window, message.c_str(), kProductName, MB_OK | MB_ICONERROR);
}

LRESULT CALLBACK thresholdWheelProcedure(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR reference) {
    if (message != WM_MOUSEWHEEL) return DefSubclassProc(window, message, wparam, lparam);

    auto& state = *reinterpret_cast<LauncherWindow*>(reference);
    HWND combo = GetParent(window) == state.window ? window : GetParent(window);
    const auto threshold = std::ranges::find(state.ppe_thresholds, combo);
    if (threshold == state.ppe_thresholds.end()) {
        return DefSubclassProc(window, message, wparam, lparam);
    }
    const int steps = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
    if (steps == 0) return 0;
    try {
        const int current = static_cast<int>(std::lround(
            parsePpeConfidenceThreshold(editText(combo)) * 100.0F));
        SetFocus(combo);
        setThresholdComboValue(combo, current + steps * 2);
        persistPreferences(state);
    } catch (const std::exception& error) {
        showError(state, error);
    }
    return 0;
}

void browseInto(LauncherWindow& state, int id) {
    if (id == SourceBrowse) {
        const auto path = pickVideoFile(state.window);
        if (!path.empty()) setText(state.source, path);
    } else if (id == OutputBrowse) {
        const auto path = pickFolder(state.window);
        if (!path.empty()) setText(state.output, path);
    }
}

void shutdown(LauncherWindow& state) {
    if (state.job != nullptr) TerminateJobObject(state.job, 130);
    if (state.waiter.joinable()) state.waiter.join();
    if (state.output_pump.joinable()) state.output_pump.join();
    closeProcessHandles(state);
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<LauncherWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<LauncherWindow*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(window, message, wparam, lparam);

    try {
        switch (message) {
            case WM_CREATE:
                createControls(*state);
                return 0;
            case WM_ERASEBKGND: {
                RECT client{};
                GetClientRect(window, &client);
                FillRect(reinterpret_cast<HDC>(wparam), &client, state->window_brush);
                return 1;
            }
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX: {
                const Palette colors = palette(*state);
                SetTextColor(reinterpret_cast<HDC>(wparam), colors.text);
                SetBkColor(reinterpret_cast<HDC>(wparam), colors.input);
                return reinterpret_cast<LRESULT>(state->input_brush);
            }
            case WM_CTLCOLORSTATIC:
                {
                const Palette colors = palette(*state);
                if (reinterpret_cast<HWND>(lparam) == state->status) {
                    SetTextColor(reinterpret_cast<HDC>(wparam), state->dark ? colors.text : colors.primary);
                    SetBkColor(reinterpret_cast<HDC>(wparam), colors.status);
                    return reinterpret_cast<LRESULT>(state->status_brush);
                }
                SetTextColor(reinterpret_cast<HDC>(wparam), colors.muted);
                SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
                return reinterpret_cast<LRESULT>(state->window_brush);
                }
            case WM_CTLCOLORBTN:
                SetTextColor(reinterpret_cast<HDC>(wparam), palette(*state).text);
                SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
                return reinterpret_cast<LRESULT>(state->window_brush);
            case WM_DRAWITEM:
                if (const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
                    item->CtlType == ODT_BUTTON) {
                    drawButton(*state, *item);
                    return TRUE;
                }
                break;
            case WM_COMMAND: {
                const int id = LOWORD(wparam);
                if (id == MenuButton) showLauncherMenu(*state);
                else if (id == ValidateButton) launchRuntime(*state, true);
                else if (id == OpenLogButton) {
                    const std::wstring path = editText(state->log_path);
                    if (!std::filesystem::is_regular_file(path)) {
                        throw std::runtime_error("The current log file does not exist yet");
                    }
                    if (reinterpret_cast<INT_PTR>(ShellExecuteW(
                            state->window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
                        throw std::runtime_error("Could not open the current log file");
                    }
                }
                else if (id == StartButton) launchRuntime(*state, false);
                else if (id == StopButton) requestStop(*state);
                else if (id == AddCameraButton) addCameraProfile(*state);
                else if (id == EditCameraButton
                    || (id == SavedCameraCombo && HIWORD(wparam) == LBN_DBLCLK)) editSelectedCameraProfile(*state);
                else if (id == DeleteCameraButton) deleteSavedCameraProfile(*state);
                else if (id == SelectAllCameraButton) selectAllCameraProfiles(*state);
                else if (id == IDM_PPE_PROFILE) {
                    openPpeProfileDialog(*state);
                }
                else if (id == IDM_ADVANCED_SETTINGS) {
                    openAdvancedSettingsDialog(*state);
                }
                else if (id == LanguageButton) {
                    state->spanish = !state->spanish;
                    refreshLanguage(*state);
                    persistPreferences(*state);
                }
                else if (id == ThemeButton) {
                    state->dark = !state->dark;
                    applyTheme(*state);
                    refreshLanguage(*state);
                    persistPreferences(*state);
                }
                else if (id == IDM_LOAD_ENV) loadEnv(*state);
                else if (id == SourceBrowse || id == OutputBrowse) {
                    browseInto(*state, id);
                }
                else if (id == ShowCheck && HIWORD(wparam) == BN_CLICKED) {
                    persistPreferences(*state);
                }
                else if (id >= PpeEnabledBase
                    && id < PpeEnabledBase + static_cast<int>(cuajone::kPpeItemCount)
                    && HIWORD(wparam) == BN_CLICKED) {
                    persistPreferences(*state);
                }
                else if ((id == ImageSizeCombo || id == RtspTransportCombo
                             || id == VideoAccelerationCombo || id == StreamResolutionCombo
                             || id == StreamFpsCombo)
                    && HIWORD(wparam) == CBN_SELCHANGE) {
                    persistPreferences(*state);
                }
                else if (id >= PpeThresholdBase
                    && id < PpeThresholdBase + static_cast<int>(kPpeOutputLabels.size())
                    && (HIWORD(wparam) == CBN_SELCHANGE || HIWORD(wparam) == CBN_KILLFOCUS)) {
                    persistPreferences(*state);
                }
                return 0;
            }
            case kProcessFinished:
                finishProcess(*state, static_cast<DWORD>(wparam), lparam != 0);
                return 0;
            case WM_CLOSE:
                if (state->process != nullptr) {
                    state->close_requested = true;
                    requestStop(*state);
                } else {
                    DestroyWindow(window);
                }
                return 0;
            case WM_DESTROY:
                shutdown(*state);
                DeleteObject(state->status_brush);
                DeleteObject(state->input_brush);
                DeleteObject(state->window_brush);
                DeleteObject(state->button_font);
                DeleteObject(state->heading_font);
                DeleteObject(state->font);
                PostQuitMessage(0);
                return 0;
            default:
                break;
        }
    } catch (const std::exception& error) {
        showError(*state, error);
        return message == WM_CREATE ? -1 : 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    // Repair PATH before anything else so both this process and the runtime
    // child resolve dev-tree DLLs on plain Explorer double-click (no-op for
    // MSI installs; see prependDevNativeDllRoots).
    prependDevNativeDllRoots();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) {
        MessageBoxW(nullptr, L"COM initialization failed", kProductName, MB_OK | MB_ICONERROR);
        return 1;
    }
    if (AllocConsole()) {
        const HWND console = GetConsoleWindow();
        if (console != nullptr) ShowWindow(console, SW_HIDE);
        SetConsoleCtrlHandler(nullptr, TRUE);
    }
    INITCOMMONCONTROLSEX common_controls{
        sizeof(common_controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&common_controls);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = windowProcedure;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    window_class.hIconSm = window_class.hIcon;
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0) {
        MessageBoxW(nullptr, L"Window registration failed", kProductName, MB_OK | MB_ICONERROR);
        FreeConsole();
        CoUninitialize();
        return 1;
    }

    LauncherWindow state;
    try {
        state.preferences_path = knownLocalAppData();
        state.preferences = loadOperatorPreferences(state.preferences_path);
    } catch (const std::exception&) {
        state.preferences = {};
    }
    HWND window = CreateWindowExW(
        0, kWindowClass, kProductName,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 906, 543,
        nullptr, nullptr, instance, &state);
    if (window == nullptr) {
        MessageBoxW(nullptr, L"Launcher window creation failed", kProductName, MB_OK | MB_ICONERROR);
        FreeConsole();
        CoUninitialize();
        return 1;
    }
    ShowWindow(window, show_command);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    FreeConsole();
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
