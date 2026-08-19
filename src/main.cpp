#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
#include <FL/platform.H>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include "audio/recorder.hpp"
#include "config/recording_config.hpp"

namespace {

constexpr wchar_t kAppName[] = L"SysRecord";
constexpr wchar_t kTrayWindowClass[] = L"SysRecord.TrayWindow";
constexpr wchar_t kSingleInstanceName[] = L"Local\\SysRecord.SingleInstance";
constexpr wchar_t kStartupRegistryPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kShowApplicationMessage = WM_APP + 2;
constexpr int kGlobalHotkeyId = 1;
constexpr UINT kToggleRecordingCommand = 1;
constexpr UINT kTogglePauseCommand = 2;
constexpr UINT kOpenFolderCommand = 3;
constexpr UINT kSettingsCommand = 4;
constexpr UINT kStartupCommand = 5;
constexpr UINT kExitCommand = 6;
constexpr int kWindowWidth = 460;
constexpr int kWindowHeight = 590;
constexpr std::array kBitrates{128, 192, 256, 320};

const Fl_Color kBackground = fl_rgb_color(30, 30, 30);
const Fl_Color kPanel = fl_rgb_color(37, 37, 38);
const Fl_Color kBorder = fl_rgb_color(60, 60, 60);
const Fl_Color kText = fl_rgb_color(224, 224, 224);
const Fl_Color kMuted = fl_rgb_color(154, 154, 154);
const Fl_Color kAccent = fl_rgb_color(229, 72, 77);
const Fl_Color kReady = fl_rgb_color(74, 158, 255);
const Fl_Color kInputBackground = fl_rgb_color(48, 48, 48);

enum class TrayCommand {
    ToggleRecording,
    TogglePause,
    OpenOutputFolder,
    Settings,
    ToggleStartup,
    Exit,
};

struct TrayPresentation {
    sysrecord::RecorderState state{sysrecord::RecorderState::Idle};
    std::wstring tooltip{L"SysRecord - Ready"};
    bool launchAtStartup{};
};

[[nodiscard]] std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("Unable to convert a Windows path to UTF-8.");
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr,
                        nullptr);
    return result;
}

[[nodiscard]] std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("The path contains invalid UTF-8 text.");
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(),
                        size);
    return result;
}

[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& path) {
    return wideToUtf8(path.wstring());
}

[[nodiscard]] std::filesystem::path pathFromUtf8(const std::string& value) {
    return std::filesystem::path{utf8ToWide(value)};
}

class TrayIcon final {
public:
    explicit TrayIcon(std::function<void(TrayCommand)> callback) : callback_(std::move(callback)) {}

    ~TrayIcon() {
        remove();
    }

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    [[nodiscard]] bool create();
    void update(const TrayPresentation& presentation);
    void showNotification(const std::wstring& title, const std::wstring& message) const;
    void setHotkey(UINT modifiers, UINT virtualKey);

private:
    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wordParameter, LPARAM longParameter);
    LRESULT handleMessage(HWND window, UINT message, WPARAM wordParameter, LPARAM longParameter);
    void remove();
    [[nodiscard]] HICON iconForState(sysrecord::RecorderState state) const;

    std::function<void(TrayCommand)> callback_;
    HWND window_{};
    std::array<HICON, 3> icons_{};
    std::array<bool, 3> ownsIcons_{};
    bool ownsWindowClass_{};
    NOTIFYICONDATAW notification_{};
    TrayPresentation presentation_;
    bool isHotkeyRegistered_{};
};

struct Hotkey {
    UINT modifiers{};
    UINT virtualKey{};
};

[[nodiscard]] Hotkey parseHotkey(const std::string& text) {
    std::istringstream input{text};
    std::string token;
    Hotkey hotkey;
    while (std::getline(input, token, '+')) {
        std::transform(token.begin(), token.end(), token.begin(), [](const unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
        if (token == "CTRL") {
            hotkey.modifiers |= MOD_CONTROL;
        } else if (token == "ALT") {
            hotkey.modifiers |= MOD_ALT;
        } else if (token == "SHIFT") {
            hotkey.modifiers |= MOD_SHIFT;
        } else if (token.size() == 1 && ((token[0] >= 'A' && token[0] <= 'Z') || (token[0] >= '0' && token[0] <= '9'))) {
            if (hotkey.virtualKey != 0) {
                throw std::invalid_argument("A hotkey can contain only one non-modifier key.");
            }
            hotkey.virtualKey = static_cast<UINT>(token[0]);
        } else if (token.size() >= 2 && token[0] == 'F') {
            const int functionNumber = std::stoi(token.substr(1));
            if (functionNumber < 1 || functionNumber > 24 || hotkey.virtualKey != 0) {
                throw std::invalid_argument("Use a letter, digit, or F1 through F24 for the hotkey.");
            }
            hotkey.virtualKey = VK_F1 + static_cast<UINT>(functionNumber - 1);
        } else {
            throw std::invalid_argument("Use a hotkey such as Ctrl+Alt+R.");
        }
    }
    if (hotkey.virtualKey == 0) {
        throw std::invalid_argument("Choose a non-modifier key for the hotkey.");
    }
    return hotkey;
}

[[nodiscard]] int parseBoundedInteger(const Fl_Input& input, const char* description, const int minimum,
                                      const int maximum) {
    const std::string_view text{input.value()};
    int value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < minimum || value > maximum) {
        throw std::invalid_argument(std::string{"Enter "} + description + " between " + std::to_string(minimum) +
                                    " and " + std::to_string(maximum) + '.');
    }
    return value;
}

class InstanceMutex final {
public:
    InstanceMutex() {
        handle_ = CreateMutexW(nullptr, TRUE, kSingleInstanceName);
        if (handle_ == nullptr) {
            throw std::runtime_error("Unable to create the single-instance mutex.");
        }
        alreadyExists_ = GetLastError() == ERROR_ALREADY_EXISTS;
    }

    ~InstanceMutex() {
        if (handle_ == nullptr) {
            return;
        }
        if (!alreadyExists_) {
            ReleaseMutex(handle_);
        }
        CloseHandle(handle_);
    }

    InstanceMutex(const InstanceMutex&) = delete;
    InstanceMutex& operator=(const InstanceMutex&) = delete;

    [[nodiscard]] bool alreadyExists() const {
        return alreadyExists_;
    }

private:
    HANDLE handle_{};
    bool alreadyExists_{};
};

[[nodiscard]] std::filesystem::path knownFolder(const KNOWNFOLDERID& folderId) {
    PWSTR rawPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(folderId, KF_FLAG_CREATE, nullptr, &rawPath))) {
        throw std::runtime_error("Unable to locate a required Windows folder.");
    }
    const std::filesystem::path path{rawPath};
    CoTaskMemFree(rawPath);
    return path;
}

[[nodiscard]] std::filesystem::path applicationDataDirectory() {
    const std::filesystem::path path = knownFolder(FOLDERID_RoamingAppData) / L"SysRecord";
    std::filesystem::create_directories(path);
    return path;
}

[[nodiscard]] sysrecord::RecordingConfig defaultConfig() {
    sysrecord::RecordingConfig config;
    config.outputFolder = knownFolder(FOLDERID_Documents) / L"SysRecord Recordings";
    return config;
}

void configureLogging(const std::filesystem::path& dataDirectory) {
    const std::filesystem::path logDirectory = dataDirectory / L"logs";
    std::filesystem::create_directories(logDirectory);
    auto logger = spdlog::rotating_logger_mt("application", (logDirectory / L"app.log").string(), 1024 * 1024, 3);
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    spdlog::flush_on(spdlog::level::info);
}

[[nodiscard]] std::filesystem::path executablePath() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error("Unable to determine the application path.");
        }
        if (length < buffer.size() - 1) {
            return std::filesystem::path{std::wstring{buffer.data(), length}};
        }
        buffer.resize(buffer.size() * 2);
    }
}

void setLaunchAtStartup(const bool enabled) {
    HKEY key = nullptr;
    const LSTATUS openResult = RegCreateKeyExW(HKEY_CURRENT_USER, kStartupRegistryPath, 0, nullptr, 0, KEY_SET_VALUE,
                                               nullptr, &key, nullptr);
    if (openResult != ERROR_SUCCESS) {
        throw std::runtime_error("Unable to open the Windows startup registry key.");
    }

    LSTATUS writeResult = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring command = L"\"" + executablePath().wstring() + L"\"";
        writeResult = RegSetValueExW(key, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                     static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        writeResult = RegDeleteValueW(key, kAppName);
        if (writeResult == ERROR_FILE_NOT_FOUND) {
            writeResult = ERROR_SUCCESS;
        }
    }
    RegCloseKey(key);
    if (writeResult != ERROR_SUCCESS) {
        throw std::runtime_error("Unable to update the Windows startup setting.");
    }
}

[[nodiscard]] std::tm currentLocalTime() {
    const std::time_t now = std::time(nullptr);
    std::tm localTime{};
    if (localtime_s(&localTime, &now) != 0) {
        throw std::runtime_error("Unable to read the local time.");
    }
    return localTime;
}

[[nodiscard]] std::filesystem::path uniqueOutputPath(const sysrecord::RecordingConfig& config) {
    std::filesystem::create_directories(config.outputFolder);
    const std::string filename = sysrecord::formatRecordingFilename(config.filenamePattern, currentLocalTime());
    std::filesystem::path candidate = config.outputFolder / pathFromUtf8(filename);
    const std::filesystem::path filenameStem = candidate.stem();
    for (int suffix = 2; std::filesystem::exists(candidate); ++suffix) {
        candidate =
            config.outputFolder / (filenameStem.wstring() + L"_" + std::to_wstring(suffix) + L".mp3");
    }
    return candidate;
}

[[nodiscard]] std::string formatDuration(const std::chrono::milliseconds elapsed) {
    const auto totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    const auto hours = totalSeconds / 3600;
    const auto minutes = (totalSeconds / 60) % 60;
    const auto seconds = totalSeconds % 60;
    char text[32]{};
    std::snprintf(text, std::size(text), "%02lld:%02lld:%02lld", hours, minutes, seconds);
    return text;
}

[[nodiscard]] std::string formatFileSize(const std::uint64_t bytes) {
    std::ostringstream text;
    if (bytes >= 1024 * 1024) {
        text.precision(1);
        text << std::fixed << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MB";
    } else {
        text << bytes / 1024 << " KB";
    }
    return text.str();
}

Fl_Box* addLabel(const int x, const int y, const int width, const int height, const char* text, const int size,
                 const Fl_Color color, const Fl_Font font = FL_HELVETICA) {
    auto* label = new Fl_Box(x, y, width, height, text);
    label->box(FL_NO_BOX);
    label->labelsize(size);
    label->labelcolor(color);
    label->labelfont(font);
    label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    return label;
}

void styleButton(Fl_Button& button, const Fl_Color color) {
    button.box(FL_BORDER_BOX);
    button.color(color);
    button.selection_color(kAccent);
    button.labelcolor(kText);
    button.labelsize(12);
}

void styleInput(Fl_Input& input) {
    input.box(FL_BORDER_BOX);
    input.color(kInputBackground);
    input.textcolor(kText);
    input.cursor_color(kText);
    input.selection_color(kReady);
    input.textsize(12);
}

class App final {
public:
    App()
        : dataDirectory_(applicationDataDirectory()), config_(sysrecord::loadConfig(configPath(), defaultConfig())),
          tray_([this](const TrayCommand command) { handleTrayCommand(command); }) {
        configureLogging(dataDirectory_);
        if (config_.outputFolder.empty()) {
            config_.outputFolder = defaultConfig().outputFolder;
        }
        refreshOutputDevices();
        sysrecord::saveConfig(configPath(), config_);
        buildUi();
        if (!tray_.create()) {
            throw std::runtime_error("Unable to create the notification-area icon.");
        }
        try {
            configureHotkey(config_);
        } catch (const std::exception& error) {
            config_.hotkeyEnabled = false;
            sysrecord::saveConfig(configPath(), config_);
            spdlog::warn("The configured hotkey was disabled: {}", error.what());
        }
        updateUiAndTray(true);
        Fl::add_timeout(0.25, timerCallback, this);
        spdlog::info("Application started in the notification area");
    }

    ~App() {
        Fl::remove_timeout(timerCallback, this);
        stopForShutdown();
        spdlog::info("Application stopped");
        spdlog::shutdown();
    }

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    [[nodiscard]] bool isRunning() const {
        return isRunning_;
    }

    [[nodiscard]] bool hasVisibleWindow() const {
        return window_ != nullptr && window_->shown();
    }

    void showSettings() {
        refreshSettingsControls();
        centerWindow();
        window_->show();
        const HWND nativeWindow = fl_xid(window_.get());
        SetForegroundWindow(nativeWindow);
        BringWindowToTop(nativeWindow);
        window_->take_focus();
    }

private:
    static void timerCallback(void* context) {
        auto* app = static_cast<App*>(context);
        app->onTimer();
        if (app->isRunning_) {
            Fl::repeat_timeout(0.25, timerCallback, context);
        }
    }

    static void hideCallback(Fl_Widget*, void* context) {
        static_cast<App*>(context)->window_->hide();
    }

    static void toggleRecordingCallback(Fl_Widget*, void* context) {
        static_cast<App*>(context)->toggleRecording();
    }

    static void togglePauseCallback(Fl_Widget*, void* context) {
        static_cast<App*>(context)->togglePause();
    }

    static void browseCallback(Fl_Widget*, void* context) {
        static_cast<App*>(context)->browseForOutputFolder();
    }

    static void saveCallback(Fl_Widget*, void* context) {
        static_cast<App*>(context)->saveSettings();
    }

    void buildUi() {
        Fl::scheme("gtk+");
        Fl::background(30, 30, 30);
        Fl::foreground(224, 224, 224);

        window_ = std::make_unique<Fl_Double_Window>(kWindowWidth, kWindowHeight, "SysRecord Settings");
        window_->color(kBackground);
        window_->resizable(nullptr);
        window_->callback(hideCallback, this);
        window_->begin();

        addLabel(20, 16, 260, 28, "SysRecord", 20, kText, FL_HELVETICA_BOLD);
        stateLabel_ = addLabel(300, 18, 140, 24, "Ready", 13, kReady, FL_HELVETICA_BOLD);
        stateLabel_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

        auto* statusPanel = new Fl_Box(20, 55, 420, 62);
        statusPanel->box(FL_BORDER_BOX);
        statusPanel->color(kPanel);
        statusPanel->labelcolor(kBorder);
        elapsedLabel_ = addLabel(34, 65, 150, 24, "00:00:00", 18, kText, FL_HELVETICA_BOLD);
        sizeLabel_ = addLabel(270, 65, 150, 24, "0 KB", 13, kMuted);
        sizeLabel_->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        fileLabel_ = addLabel(34, 91, 386, 18, "No active recording", 10, kMuted);

        addLabel(20, 133, 140, 20, "Output folder", 11, kMuted, FL_HELVETICA_BOLD);
        outputFolderInput_ = new Fl_Input(20, 155, 340, 30);
        styleInput(*outputFolderInput_);
        auto* browseButton = new Fl_Button(368, 155, 72, 30, "Browse");
        styleButton(*browseButton, kPanel);
        browseButton->callback(browseCallback, this);

        addLabel(20, 197, 180, 20, "Filename pattern", 11, kMuted, FL_HELVETICA_BOLD);
        filenamePatternInput_ = new Fl_Input(20, 219, 420, 30);
        styleInput(*filenamePatternInput_);

        addLabel(20, 261, 180, 20, "Output device", 11, kMuted, FL_HELVETICA_BOLD);
        outputDeviceChoice_ = new Fl_Choice(20, 283, 420, 30);
        outputDeviceChoice_->box(FL_BORDER_BOX);
        outputDeviceChoice_->color(kInputBackground);
        outputDeviceChoice_->textcolor(kText);

        addLabel(20, 325, 100, 20, "MP3 bitrate", 11, kMuted, FL_HELVETICA_BOLD);
        bitrateChoice_ = new Fl_Choice(20, 347, 130, 30);
        bitrateChoice_->add("128 kbps|192 kbps|256 kbps|320 kbps");
        bitrateChoice_->box(FL_BORDER_BOX);
        bitrateChoice_->color(kInputBackground);
        bitrateChoice_->textcolor(kText);
        vbrCheck_ = new Fl_Check_Button(178, 346, 118, 32, "VBR mode");
        vbrCheck_->labelcolor(kText);
        vbrQualityChoice_ = new Fl_Choice(304, 347, 136, 30);
        vbrQualityChoice_->add("VBR quality 0|VBR quality 1|VBR quality 2|VBR quality 3|VBR quality 4|VBR quality 5|VBR quality 6|VBR quality 7|VBR quality 8|VBR quality 9");
        vbrQualityChoice_->box(FL_BORDER_BOX);
        vbrQualityChoice_->color(kInputBackground);
        vbrQualityChoice_->textcolor(kText);

        addLabel(20, 389, 150, 20, "Silence stop (seconds)", 11, kMuted, FL_HELVETICA_BOLD);
        silenceTimeoutInput_ = new Fl_Input(20, 411, 130, 30);
        styleInput(*silenceTimeoutInput_);
        addLabel(178, 389, 150, 20, "Maximum duration (min)", 11, kMuted, FL_HELVETICA_BOLD);
        maxDurationInput_ = new Fl_Input(178, 411, 130, 30);
        styleInput(*maxDurationInput_);
        notificationsCheck_ = new Fl_Check_Button(320, 410, 120, 32, "Notifications");
        notificationsCheck_->labelcolor(kText);

        hotkeyCheck_ = new Fl_Check_Button(20, 454, 110, 30, "Enable hotkey");
        hotkeyCheck_->labelcolor(kText);
        hotkeyInput_ = new Fl_Input(140, 454, 150, 30);
        styleInput(*hotkeyInput_);
        startupCheck_ = new Fl_Check_Button(308, 454, 132, 30, "Start with Windows");
        startupCheck_->labelcolor(kText);

        auto* saveButton = new Fl_Button(20, 504, 130, 36, "Save settings");
        styleButton(*saveButton, kReady);
        saveButton->callback(saveCallback, this);
        recordButton_ = new Fl_Button(160, 504, 170, 36, "Start recording");
        styleButton(*recordButton_, kAccent);
        recordButton_->callback(toggleRecordingCallback, this);
        pauseButton_ = new Fl_Button(340, 504, 100, 36, "Pause");
        styleButton(*pauseButton_, kPanel);
        pauseButton_->callback(togglePauseCallback, this);

        configPathLabel_ = addLabel(20, 550, 420, 28, "", 9, kMuted);
        window_->end();
        refreshSettingsControls();
    }

    void refreshSettingsControls() {
        outputFolderInput_->value(pathToUtf8(config_.outputFolder).c_str());
        filenamePatternInput_->value(config_.filenamePattern.c_str());
        const auto bitrate = std::ranges::find(kBitrates, config_.bitrateKbps);
        bitrateChoice_->value(bitrate == kBitrates.end() ? 1 : static_cast<int>(bitrate - kBitrates.begin()));
        outputDeviceChoice_->clear();
        outputDeviceChoice_->add("System Default");
        int selectedDeviceIndex = 0;
        for (std::size_t index = 0; index < outputDevices_.size(); ++index) {
            outputDeviceChoice_->add(outputDevices_[index].name.c_str());
            if (outputDevices_[index].id == config_.outputDeviceId) {
                selectedDeviceIndex = static_cast<int>(index + 1);
            }
        }
        outputDeviceChoice_->value(selectedDeviceIndex);
        vbrCheck_->value(config_.vbrMode ? 1 : 0);
        vbrQualityChoice_->value(config_.vbrQuality);
        silenceTimeoutInput_->value(std::to_string(config_.silenceTimeoutSeconds).c_str());
        maxDurationInput_->value(std::to_string(config_.maxRecordingMinutes).c_str());
        hotkeyCheck_->value(config_.hotkeyEnabled ? 1 : 0);
        hotkeyInput_->value(config_.hotkey.c_str());
        notificationsCheck_->value(config_.showNotifications ? 1 : 0);
        startupCheck_->value(config_.launchAtStartup ? 1 : 0);
        const std::string configLocation = "Config: " + pathToUtf8(configPath());
        configPathLabel_->copy_label(configLocation.c_str());
    }

    void browseForOutputFolder() {
        Fl_Native_File_Chooser chooser;
        chooser.title("Select the SysRecord output folder");
        chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
        chooser.directory(outputFolderInput_->value());
        if (chooser.show() == 0 && chooser.filename() != nullptr) {
            outputFolderInput_->value(chooser.filename());
        }
    }

    void refreshOutputDevices() {
        try {
            outputDevices_ = sysrecord::listOutputDevices();
        } catch (const std::exception& error) {
            outputDevices_.clear();
            spdlog::warn("Unable to list output devices: {}", error.what());
        }
    }

    void configureHotkey(const sysrecord::RecordingConfig& config) {
        if (!config.hotkeyEnabled) {
            tray_.setHotkey(0, 0);
            return;
        }
        const Hotkey hotkey = parseHotkey(config.hotkey);
        tray_.setHotkey(hotkey.modifiers, hotkey.virtualKey);
    }

    void saveSettings() {
        try {
            sysrecord::RecordingConfig updated = config_;
            updated.outputFolder = pathFromUtf8(outputFolderInput_->value());
            if (updated.outputFolder.empty()) {
                throw std::runtime_error("Choose an output folder.");
            }
            updated.filenamePattern = filenamePatternInput_->value();
            if (updated.filenamePattern.empty()) {
                throw std::runtime_error("Enter a filename pattern.");
            }
            const int bitrateIndex = bitrateChoice_->value();
            if (bitrateIndex < 0 || bitrateIndex >= static_cast<int>(kBitrates.size())) {
                throw std::runtime_error("Choose an MP3 bitrate.");
            }
            updated.bitrateKbps = kBitrates[static_cast<std::size_t>(bitrateIndex)];
            const int outputDeviceIndex = outputDeviceChoice_->value();
            if (outputDeviceIndex < 0 || outputDeviceIndex > static_cast<int>(outputDevices_.size())) {
                throw std::runtime_error("Choose an output device.");
            }
            updated.outputDeviceId = outputDeviceIndex == 0 ? "default" : outputDevices_[outputDeviceIndex - 1].id;
            updated.vbrMode = vbrCheck_->value() != 0;
            updated.vbrQuality = vbrQualityChoice_->value();
            updated.silenceTimeoutSeconds = parseBoundedInteger(*silenceTimeoutInput_, "a silence timeout", 0, 3600);
            updated.maxRecordingMinutes = parseBoundedInteger(*maxDurationInput_, "a maximum duration", 1, 300);
            updated.hotkeyEnabled = hotkeyCheck_->value() != 0;
            updated.hotkey = hotkeyInput_->value();
            if (updated.hotkeyEnabled) {
                static_cast<void>(parseHotkey(updated.hotkey));
            }
            updated.showNotifications = notificationsCheck_->value() != 0;
            updated.launchAtStartup = startupCheck_->value() != 0;

            std::filesystem::create_directories(updated.outputFolder);
            if (!std::filesystem::is_directory(updated.outputFolder)) {
                throw std::runtime_error("The output path is not a folder.");
            }
            if (updated.launchAtStartup != config_.launchAtStartup) {
                setLaunchAtStartup(updated.launchAtStartup);
            }
            configureHotkey(updated);
            sysrecord::saveConfig(configPath(), updated);
            config_ = std::move(updated);
            updateUiAndTray(true);
            spdlog::info("Settings saved");
        } catch (const std::exception& error) {
            reportError("Unable to save settings", error.what());
        }
    }

    void toggleRecording() {
        const sysrecord::RecorderState state = recorder_.status().state;
        if (state == sysrecord::RecorderState::Idle) {
            startRecording();
        } else {
            stopRecording();
        }
    }

    void startRecording() {
        try {
            const std::filesystem::path outputPath = uniqueOutputPath(config_);
            recorder_.start(outputPath,
                            {.bitrateKbps = config_.bitrateKbps,
                             .vbrMode = config_.vbrMode,
                             .vbrQuality = config_.vbrQuality,
                             .outputDeviceId = config_.outputDeviceId});
            spdlog::info("Recording started: {}", pathToUtf8(outputPath));
            if (config_.showNotifications) {
                tray_.showNotification(L"Recording started", outputPath.filename().wstring());
            }
            updateUiAndTray(true);
        } catch (const std::exception& error) {
            reportError("Unable to start recording", error.what());
        }
    }

    void stopRecording() {
        const std::filesystem::path outputPath = recorder_.status().outputPath;
        try {
            recorder_.stop();
            spdlog::info("Recording stopped: {}", pathToUtf8(outputPath));
            if (config_.showNotifications) {
                tray_.showNotification(L"Recording saved", outputPath.filename().wstring());
            }
        } catch (const std::exception& error) {
            reportError("Recording stopped with an error", error.what());
        }
        updateUiAndTray(true);
    }

    void togglePause() {
        try {
            const sysrecord::RecorderState state = recorder_.status().state;
            if (state == sysrecord::RecorderState::Recording) {
                recorder_.pause();
                spdlog::info("Recording paused");
            } else if (state == sysrecord::RecorderState::Paused) {
                recorder_.resume();
                spdlog::info("Recording resumed");
            }
            updateUiAndTray(true);
        } catch (const std::exception& error) {
            reportError("Unable to change the recording state", error.what());
        }
    }

    void onTimer() {
        const sysrecord::RecorderStatus status = recorder_.status();
        if (status.state == sysrecord::RecorderState::Error) {
            stopRecording();
            return;
        }
        if ((status.state == sysrecord::RecorderState::Recording || status.state == sysrecord::RecorderState::Paused) &&
            status.elapsed >= std::chrono::minutes{config_.maxRecordingMinutes}) {
            stopRecording();
            return;
        }
        if (status.state == sysrecord::RecorderState::Recording && config_.silenceTimeoutSeconds > 0 &&
            status.silenceElapsed >= std::chrono::seconds{config_.silenceTimeoutSeconds}) {
            if (config_.showNotifications) {
                tray_.showNotification(L"Recording stopped", L"No audible system audio was detected.");
            }
            stopRecording();
            return;
        }
        updateUiAndTray(false);
    }

    void updateUiAndTray(const bool forceTrayUpdate) {
        const sysrecord::RecorderStatus status = recorder_.status();
        const std::string elapsed = formatDuration(status.elapsed);
        elapsedLabel_->copy_label(elapsed.c_str());
        const std::string fileSize = formatFileSize(status.bytesWritten);
        sizeLabel_->copy_label(fileSize.c_str());

        std::string stateText;
        Fl_Color stateColor = kReady;
        std::string recordText;
        std::string pauseText = "Pause";
        bool canPause = false;
        switch (status.state) {
            case sysrecord::RecorderState::Idle:
                stateText = "Ready";
                recordText = "Start recording";
                break;
            case sysrecord::RecorderState::Recording:
                stateText = "Recording";
                stateColor = kAccent;
                recordText = "Stop recording";
                canPause = true;
                break;
            case sysrecord::RecorderState::Paused:
                stateText = "Paused";
                stateColor = fl_rgb_color(218, 165, 32);
                recordText = "Stop recording";
                pauseText = "Resume";
                canPause = true;
                break;
            case sysrecord::RecorderState::Error:
                stateText = "Error";
                stateColor = kAccent;
                recordText = "Stop recording";
                break;
        }
        stateLabel_->copy_label(stateText.c_str());
        stateLabel_->labelcolor(stateColor);
        recordButton_->copy_label(recordText.c_str());
        pauseButton_->copy_label(pauseText.c_str());
        canPause ? pauseButton_->activate() : pauseButton_->deactivate();

        const std::string fileText = status.outputPath.empty() ? "No active recording" : pathToUtf8(status.outputPath);
        fileLabel_->copy_label(fileText.c_str());
        window_->redraw();

        const auto elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(status.elapsed).count();
        if (forceTrayUpdate || status.state != lastTrayState_ || elapsedSeconds != lastTrayElapsedSeconds_) {
            TrayPresentation presentation;
            presentation.state = status.state;
            presentation.launchAtStartup = config_.launchAtStartup;
            presentation.tooltip = L"SysRecord - " + utf8ToWide(stateText);
            if (status.state == sysrecord::RecorderState::Recording ||
                status.state == sysrecord::RecorderState::Paused) {
                presentation.tooltip += L" " + utf8ToWide(elapsed);
            }
            tray_.update(presentation);
            lastTrayState_ = status.state;
            lastTrayElapsedSeconds_ = elapsedSeconds;
        }
    }

    void openOutputFolder() {
        try {
            std::filesystem::create_directories(config_.outputFolder);
            const HINSTANCE result = ShellExecuteW(nullptr, L"open", config_.outputFolder.c_str(), nullptr, nullptr,
                                                   SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(result) <= 32) {
                throw std::runtime_error("Windows could not open the output folder.");
            }
        } catch (const std::exception& error) {
            reportError("Unable to open the output folder", error.what());
        }
    }

    void toggleStartup() {
        try {
            const bool enabled = !config_.launchAtStartup;
            setLaunchAtStartup(enabled);
            config_.launchAtStartup = enabled;
            sysrecord::saveConfig(configPath(), config_);
            startupCheck_->value(enabled ? 1 : 0);
            updateUiAndTray(true);
        } catch (const std::exception& error) {
            reportError("Unable to update the startup setting", error.what());
        }
    }

    void handleTrayCommand(const TrayCommand command) {
        switch (command) {
            case TrayCommand::ToggleRecording:
                toggleRecording();
                break;
            case TrayCommand::TogglePause:
                togglePause();
                break;
            case TrayCommand::OpenOutputFolder:
                openOutputFolder();
                break;
            case TrayCommand::Settings:
                showSettings();
                break;
            case TrayCommand::ToggleStartup:
                toggleStartup();
                break;
            case TrayCommand::Exit:
                requestExit();
                break;
        }
    }

    void requestExit() {
        stopForShutdown();
        window_->hide();
        isRunning_ = false;
    }

    void stopForShutdown() noexcept {
        if (recorder_.status().state == sysrecord::RecorderState::Idle) {
            return;
        }
        try {
            recorder_.stop();
        } catch (const std::exception& error) {
            spdlog::error("Unable to finalize the recording during shutdown: {}", error.what());
        }
    }

    void centerWindow() {
        POINT cursor{};
        GetCursorPos(&cursor);
        const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        GetMonitorInfoW(monitor, &monitorInfo);
        const int x = monitorInfo.rcWork.left + (monitorInfo.rcWork.right - monitorInfo.rcWork.left - kWindowWidth) / 2;
        const int y = monitorInfo.rcWork.top + (monitorInfo.rcWork.bottom - monitorInfo.rcWork.top - kWindowHeight) / 2;
        window_->position(x, y);
    }

    void reportError(const char* context, const std::string& detail) {
        spdlog::error("{}: {}", context, detail);
        const std::string message = std::string{context} + ":\n" + detail;
        if (config_.showNotifications) {
            tray_.showNotification(L"SysRecord error", utf8ToWide(detail));
        }
        fl_alert("%s", message.c_str());
    }

    [[nodiscard]] std::filesystem::path configPath() const {
        return dataDirectory_ / L"config.json";
    }

    std::filesystem::path dataDirectory_;
    sysrecord::RecordingConfig config_;
    std::vector<sysrecord::OutputDevice> outputDevices_;
    sysrecord::Recorder recorder_;
    TrayIcon tray_;
    std::unique_ptr<Fl_Double_Window> window_;
    Fl_Box* stateLabel_{};
    Fl_Box* elapsedLabel_{};
    Fl_Box* sizeLabel_{};
    Fl_Box* fileLabel_{};
    Fl_Box* configPathLabel_{};
    Fl_Input* outputFolderInput_{};
    Fl_Input* filenamePatternInput_{};
    Fl_Choice* outputDeviceChoice_{};
    Fl_Choice* bitrateChoice_{};
    Fl_Check_Button* vbrCheck_{};
    Fl_Choice* vbrQualityChoice_{};
    Fl_Input* silenceTimeoutInput_{};
    Fl_Input* maxDurationInput_{};
    Fl_Check_Button* hotkeyCheck_{};
    Fl_Input* hotkeyInput_{};
    Fl_Check_Button* notificationsCheck_{};
    Fl_Check_Button* startupCheck_{};
    Fl_Button* recordButton_{};
    Fl_Button* pauseButton_{};
    sysrecord::RecorderState lastTrayState_{sysrecord::RecorderState::Error};
    long long lastTrayElapsedSeconds_{-1};
    bool isRunning_{true};
};

bool TrayIcon::create() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kTrayWindowClass;
    const ATOM classAtom = RegisterClassW(&windowClass);
    if (classAtom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    ownsWindowClass_ = classAtom != 0;

    window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayWindowClass, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                              instance, this);
    if (window_ == nullptr) {
        remove();
        return false;
    }

    constexpr std::array resourceIds{101, 102, 103};
    for (std::size_t index = 0; index < resourceIds.size(); ++index) {
        icons_[index] = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(resourceIds[index]), IMAGE_ICON,
                                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                                      LR_DEFAULTCOLOR));
        ownsIcons_[index] = icons_[index] != nullptr;
    }
    if (icons_[0] == nullptr) {
        icons_[0] = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    }

    notification_.cbSize = sizeof(notification_);
    notification_.hWnd = window_;
    notification_.uID = 1;
    notification_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    notification_.uCallbackMessage = kTrayMessage;
    notification_.hIcon = icons_[0];
    lstrcpynW(notification_.szTip, L"SysRecord - Ready", static_cast<int>(std::size(notification_.szTip)));
    if (Shell_NotifyIconW(NIM_ADD, &notification_) != TRUE) {
        remove();
        return false;
    }
    notification_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &notification_);
    return true;
}

void TrayIcon::update(const TrayPresentation& presentation) {
    presentation_ = presentation;
    notification_.uFlags = NIF_ICON | NIF_TIP;
    notification_.hIcon = iconForState(presentation.state);
    lstrcpynW(notification_.szTip, presentation.tooltip.c_str(), static_cast<int>(std::size(notification_.szTip)));
    Shell_NotifyIconW(NIM_MODIFY, &notification_);
}

void TrayIcon::showNotification(const std::wstring& title, const std::wstring& message) const {
    NOTIFYICONDATAW notification = notification_;
    notification.uFlags = NIF_INFO;
    notification.dwInfoFlags = NIIF_INFO;
    lstrcpynW(notification.szInfoTitle, title.c_str(), static_cast<int>(std::size(notification.szInfoTitle)));
    lstrcpynW(notification.szInfo, message.c_str(), static_cast<int>(std::size(notification.szInfo)));
    Shell_NotifyIconW(NIM_MODIFY, &notification);
}

void TrayIcon::setHotkey(const UINT modifiers, const UINT virtualKey) {
    if (isHotkeyRegistered_) {
        UnregisterHotKey(window_, kGlobalHotkeyId);
        isHotkeyRegistered_ = false;
    }
    if (virtualKey == 0) {
        return;
    }
    if (RegisterHotKey(window_, kGlobalHotkeyId, modifiers | MOD_NOREPEAT, virtualKey) == FALSE) {
        throw std::runtime_error("Windows could not register that hotkey. It may already be in use.");
    }
    isHotkeyRegistered_ = true;
}

void TrayIcon::remove() {
    if (isHotkeyRegistered_ && window_ != nullptr) {
        UnregisterHotKey(window_, kGlobalHotkeyId);
        isHotkeyRegistered_ = false;
    }
    if (window_ != nullptr) {
        Shell_NotifyIconW(NIM_DELETE, &notification_);
        DestroyWindow(window_);
        window_ = nullptr;
    }
    for (std::size_t index = 0; index < icons_.size(); ++index) {
        if (ownsIcons_[index] && icons_[index] != nullptr) {
            DestroyIcon(icons_[index]);
        }
        icons_[index] = nullptr;
        ownsIcons_[index] = false;
    }
    if (ownsWindowClass_) {
        UnregisterClassW(kTrayWindowClass, GetModuleHandleW(nullptr));
        ownsWindowClass_ = false;
    }
}

HICON TrayIcon::iconForState(const sysrecord::RecorderState state) const {
    if (state == sysrecord::RecorderState::Recording && icons_[1] != nullptr) {
        return icons_[1];
    }
    if (state == sysrecord::RecorderState::Paused && icons_[2] != nullptr) {
        return icons_[2];
    }
    return icons_[0];
}

LRESULT CALLBACK TrayIcon::windowProcedure(HWND window, const UINT message, const WPARAM wordParameter,
                                           const LPARAM longParameter) {
    auto* trayIcon = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(longParameter);
        trayIcon = static_cast<TrayIcon*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(trayIcon));
    }
    if (trayIcon != nullptr) {
        return trayIcon->handleMessage(window, message, wordParameter, longParameter);
    }
    return DefWindowProcW(window, message, wordParameter, longParameter);
}

LRESULT TrayIcon::handleMessage(HWND window, const UINT message, const WPARAM wordParameter,
                                const LPARAM longParameter) {
    if (message == WM_HOTKEY && wordParameter == kGlobalHotkeyId) {
        callback_(TrayCommand::ToggleRecording);
        return 0;
    }
    if (message == kShowApplicationMessage) {
        callback_(TrayCommand::Settings);
        return 0;
    }
    if (message != kTrayMessage) {
        return DefWindowProcW(window, message, wordParameter, longParameter);
    }

    const UINT event = LOWORD(longParameter);
    if (event == WM_LBUTTONUP) {
        callback_(TrayCommand::ToggleRecording);
        return 0;
    }
    if (event != WM_RBUTTONUP && event != WM_CONTEXTMENU) {
        return 0;
    }

    const bool isIdle = presentation_.state == sysrecord::RecorderState::Idle;
    const bool isPaused = presentation_.state == sysrecord::RecorderState::Paused;
    const HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kToggleRecordingCommand, isIdle ? L"Start Recording" : L"Stop Recording");
    AppendMenuW(menu, MF_STRING | (isIdle ? MF_GRAYED : 0), kTogglePauseCommand,
                isPaused ? L"Resume" : L"Pause");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kOpenFolderCommand, L"Open Output Folder");
    AppendMenuW(menu, MF_STRING, kSettingsCommand, L"Settings...");
    AppendMenuW(menu, MF_STRING | (presentation_.launchAtStartup ? MF_CHECKED : 0), kStartupCommand,
                L"Launch at Startup");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExitCommand, L"Exit");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, window, nullptr);
    DestroyMenu(menu);
    PostMessageW(window, WM_NULL, 0, 0);

    switch (command) {
        case kToggleRecordingCommand:
            callback_(TrayCommand::ToggleRecording);
            break;
        case kTogglePauseCommand:
            callback_(TrayCommand::TogglePause);
            break;
        case kOpenFolderCommand:
            callback_(TrayCommand::OpenOutputFolder);
            break;
        case kSettingsCommand:
            callback_(TrayCommand::Settings);
            break;
        case kStartupCommand:
            callback_(TrayCommand::ToggleStartup);
            break;
        case kExitCommand:
            callback_(TrayCommand::Exit);
            break;
        default:
            break;
    }
    return 0;
}

void dispatchNativeMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        InstanceMutex instanceMutex;
        if (instanceMutex.alreadyExists()) {
            if (const HWND existingWindow = FindWindowW(kTrayWindowClass, kAppName); existingWindow != nullptr) {
                PostMessageW(existingWindow, kShowApplicationMessage, 0, 0);
            }
            return 0;
        }

        App app;
        while (app.isRunning()) {
            if (app.hasVisibleWindow()) {
                Fl::wait(0.1);
            } else {
                MsgWaitForMultipleObjectsEx(0, nullptr, 250, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
            dispatchNativeMessages();
            Fl::check();
        }
        return 0;
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "SysRecord", MB_OK | MB_ICONERROR);
        return 1;
    }
}
