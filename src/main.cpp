#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/fl_ask.H>
#include <FL/platform.H>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include "app_state.hpp"

namespace {

constexpr wchar_t kAppName[] = L"Sys Record";
constexpr wchar_t kAppId[] = L"SysRecord";
constexpr wchar_t kTrayWindowClass[] = L"SysRecord.TrayWindow";
constexpr wchar_t kSingleInstanceName[] = L"Local\\SysRecord.SingleInstance";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kShowApplicationMessage = WM_APP + 2;
constexpr UINT kTrayOpenCommand = 1;
constexpr UINT kTrayExitCommand = 2;

constexpr int kWindowWidth = 440;
constexpr int kWindowHeight = 360;
constexpr int kHeaderHeight = 54;
constexpr bool kHideOnFocusLoss = true;

const Fl_Color kBackground = fl_rgb_color(17, 23, 32);
const Fl_Color kHeader = fl_rgb_color(29, 39, 53);
const Fl_Color kPanel = fl_rgb_color(27, 36, 48);
const Fl_Color kPanelRaised = fl_rgb_color(42, 55, 72);
const Fl_Color kText = fl_rgb_color(235, 240, 247);
const Fl_Color kMuted = fl_rgb_color(149, 164, 182);
const Fl_Color kAccent = fl_rgb_color(55, 183, 158);
const Fl_Color kInputBackground = fl_rgb_color(247, 249, 252);
const Fl_Color kInputText = fl_rgb_color(24, 31, 42);

class App;

class MainWindow final : public Fl_Double_Window {
public:
    explicit MainWindow(App& app) : Fl_Double_Window(kWindowWidth, kWindowHeight), app_(app) {}

    int handle(int event) override;

private:
    App& app_;
    bool isDragging_{};
    int dragOffsetX_{};
    int dragOffsetY_{};
};

class TrayIcon final {
public:
    TrayIcon(std::function<void()> openCallback, std::function<void()> exitCallback)
        : openCallback_(std::move(openCallback)), exitCallback_(std::move(exitCallback)) {}

    ~TrayIcon() {
        remove();
    }

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    [[nodiscard]] bool create();
    void remove();

private:
    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wordParameter, LPARAM longParameter);
    LRESULT handleMessage(HWND window, UINT message, WPARAM wordParameter, LPARAM longParameter);

    std::function<void()> openCallback_;
    std::function<void()> exitCallback_;
    HWND window_{};
    HICON icon_{};
    bool ownsIcon_{};
    bool ownsWindowClass_{};
    NOTIFYICONDATAW notification_{};
};

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
        if (handle_ != nullptr) {
            if (!alreadyExists_) {
                ReleaseMutex(handle_);
            }
            CloseHandle(handle_);
        }
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

[[nodiscard]] std::filesystem::path applicationDataDirectory() {
    PWSTR rawPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &rawPath))) {
        throw std::runtime_error("Unable to locate the Local AppData directory.");
    }

    const std::filesystem::path path = std::filesystem::path{rawPath} / kAppId;
    CoTaskMemFree(rawPath);
    std::filesystem::create_directories(path);
    return path;
}

void configureLogging(const std::filesystem::path& dataDirectory) {
    const auto logDirectory = dataDirectory / L"logs";
    std::filesystem::create_directories(logDirectory);
    auto logger = spdlog::rotating_logger_mt("application", (logDirectory / L"app.log").string(), 1024 * 1024, 3);
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    spdlog::flush_on(spdlog::level::info);
}

[[nodiscard]] AppState loadState(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return {};
    }

    try {
        const std::string jsonText{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        return deserializeAppState(jsonText);
    } catch (const std::exception&) {
        return {};
    }
}

Fl_Box* addLabel(int x, int y, int width, int height, const char* text, int size, Fl_Color color,
                 Fl_Font font = FL_HELVETICA) {
    auto* label = new Fl_Box(x, y, width, height, text);
    label->box(FL_NO_BOX);
    label->labelsize(size);
    label->labelcolor(color);
    label->labelfont(font);
    label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    return label;
}

void styleButton(Fl_Button& button, Fl_Color color) {
    button.box(FL_THIN_UP_BOX);
    button.color(color);
    button.selection_color(kAccent);
    button.labelcolor(kText);
    button.labelsize(12);
}

class App final {
public:
    App()
        : dataDirectory_(applicationDataDirectory()), state_(loadState(statePath())),
          tray_([this] { show(); }, [this] { requestExit(); }) {
        configureLogging(dataDirectory_);
        buildUi();
        if (!tray_.create()) {
            throw std::runtime_error("Unable to create the notification-area icon.");
        }
        spdlog::info("Application started in the notification area");
    }

    ~App() {
        Fl::remove_timeout(focusCheckCallback, this);
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

    void show() {
        focusGraceUntil_ = std::chrono::steady_clock::now() + std::chrono::milliseconds{600};
        centerWindow();
        window_->show();

        const HWND nativeWindow = fl_xid(window_.get());
        const LONG_PTR extendedStyle = GetWindowLongPtrW(nativeWindow, GWL_EXSTYLE);
        SetWindowLongPtrW(nativeWindow, GWL_EXSTYLE, extendedStyle | WS_EX_TOOLWINDOW);
        SetWindowPos(
            nativeWindow,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        ShowWindow(nativeWindow, SW_SHOWNORMAL);
        SetForegroundWindow(nativeWindow);
        BringWindowToTop(nativeWindow);
        window_->take_focus();
        spdlog::debug("Main window opened");
    }

    void hide() {
        if (window_ != nullptr) {
            window_->hide();
            spdlog::debug("Main window hidden");
        }
    }

    void requestFocusCheck() {
        if constexpr (kHideOnFocusLoss) {
            Fl::remove_timeout(focusCheckCallback, this);
            Fl::add_timeout(0.12, focusCheckCallback, this);
        }
    }

    void requestExit() {
        hide();
        isRunning_ = false;
    }

private:
    std::filesystem::path dataDirectory_;
    AppState state_;
    TrayIcon tray_;
    std::unique_ptr<MainWindow> window_;
    Fl_Input* displayNameInput_{};
    Fl_Box* countValue_{};
    Fl_Box* stateSummary_{};
    bool isRunning_{true};
    std::chrono::steady_clock::time_point focusGraceUntil_{};

    static void focusCheckCallback(void* data) {
        static_cast<App*>(data)->hideIfFocusWasLost();
    }

    static void hideCallback(Fl_Widget*, void* data) {
        static_cast<App*>(data)->hide();
    }

    static void saveNameCallback(Fl_Widget*, void* data) {
        static_cast<App*>(data)->saveDisplayName();
    }

    static void incrementCallback(Fl_Widget*, void* data) {
        static_cast<App*>(data)->incrementSampleCount();
    }

    void buildUi() {
        Fl::scheme("gtk+");
        Fl::background(17, 23, 32);
        Fl::foreground(235, 240, 247);

        window_ = std::make_unique<MainWindow>(*this);
        window_->label("Sys Record");
        window_->border(0);
        window_->color(kBackground);
        window_->callback(hideCallback, this);
        window_->begin();

        auto* header = new Fl_Box(0, 0, kWindowWidth, kHeaderHeight);
        header->box(FL_FLAT_BOX);
        header->color(kHeader);
        addLabel(20, 0, 350, kHeaderHeight, "Sys Record", 16, kText, FL_HELVETICA_BOLD);

        auto* hideButton = new Fl_Button(390, 10, 34, 34, "_");
        styleButton(*hideButton, kPanelRaised);
        hideButton->callback(hideCallback, this);

        auto* panel = new Fl_Box(14, 68, 412, 220);
        panel->box(FL_FLAT_BOX);
        panel->color(kPanel);

        addLabel(28, 80, 380, 18, "STARTER STATE", 10, kMuted, FL_HELVETICA_BOLD);
        addLabel(28, 106, 95, 28, "Display name", 12, kText);

        displayNameInput_ = new Fl_Input(124, 106, 282, 28);
        displayNameInput_->value(state_.displayName.c_str());
        displayNameInput_->color(kInputBackground);
        displayNameInput_->textcolor(kInputText);
        displayNameInput_->cursor_color(kInputText);
        displayNameInput_->selection_color(kAccent);
        displayNameInput_->textsize(13);

        auto* saveButton = new Fl_Button(276, 144, 130, 30, "Save name");
        styleButton(*saveButton, kPanelRaised);
        saveButton->callback(saveNameCallback, this);

        addLabel(28, 188, 130, 26, "Sample counter", 12, kText);
        countValue_ = addLabel(158, 188, 80, 26, "", 18, kAccent, FL_HELVETICA_BOLD);

        auto* incrementButton = new Fl_Button(276, 186, 130, 30, "Increment");
        styleButton(*incrementButton, kPanelRaised);
        incrementButton->callback(incrementCallback, this);

        stateSummary_ = addLabel(28, 232, 378, 42, "", 11, kMuted);

        auto* hideToTrayButton = new Fl_Button(14, 300, 200, 42, "Hide to tray");
        styleButton(*hideToTrayButton, kPanelRaised);
        hideToTrayButton->callback(hideCallback, this);

        auto* readyLabel = addLabel(230, 300, 196, 42, "JSON state ready", 11, kAccent, FL_HELVETICA_BOLD);
        readyLabel->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);

        window_->end();
        updateStateLabels();
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

    void hideIfFocusWasLost() {
        if (!hasVisibleWindow() || std::chrono::steady_clock::now() < focusGraceUntil_) {
            return;
        }

        const HWND nativeWindow = fl_xid(window_.get());
        const HWND foregroundWindow = GetForegroundWindow();
        if (foregroundWindow != nativeWindow && !IsChild(nativeWindow, foregroundWindow)) {
            hide();
        }
    }

    void saveDisplayName() {
        try {
            std::string value = displayNameInput_->value();
            value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char character) {
                return character != ' ' && character != '\t' && character != '\r' && character != '\n';
            }));
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' ||
                                      value.back() == '\n')) {
                value.pop_back();
            }
            state_.displayName = value.empty() ? "Sys Record" : value;
            displayNameInput_->value(state_.displayName.c_str());
            persistState();
            updateStateLabels();
            spdlog::info("Display name updated");
        } catch (const std::exception& error) {
            reportError(error);
        }
    }

    void incrementSampleCount() {
        try {
            if (state_.sampleCount < std::numeric_limits<int>::max()) {
                ++state_.sampleCount;
            }
            persistState();
            updateStateLabels();
            spdlog::info("Sample counter changed to {}", state_.sampleCount);
        } catch (const std::exception& error) {
            reportError(error);
        }
    }

    void persistState() {
        std::ofstream output(statePath(), std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Unable to open the JSON state file.");
        }
        output << serializeAppState(state_);
        if (!output) {
            throw std::runtime_error("Unable to write the JSON state file.");
        }
    }

    [[nodiscard]] std::filesystem::path statePath() const {
        return dataDirectory_ / L"app_state.json";
    }

    void updateStateLabels() {
        const std::string count = std::to_string(state_.sampleCount);
        countValue_->copy_label(count.c_str());
        const std::string summary = "Persisted in " + statePath().string();
        stateSummary_->copy_label(summary.c_str());
        window_->redraw();
    }

    void reportError(const std::exception& error) {
        spdlog::error("UI action failed: {}", error.what());
        fl_alert("%s", error.what());
    }
};

int MainWindow::handle(int event) {
    if (event == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE && Fl::event_y() < kHeaderHeight &&
        Fl::event_x() < 380) {
        isDragging_ = true;
        dragOffsetX_ = Fl::event_x_root() - x();
        dragOffsetY_ = Fl::event_y_root() - y();
        return 1;
    }
    if (event == FL_DRAG && isDragging_) {
        position(Fl::event_x_root() - dragOffsetX_, Fl::event_y_root() - dragOffsetY_);
        return 1;
    }
    if (event == FL_RELEASE) {
        isDragging_ = false;
    }
    if (event == FL_UNFOCUS) {
        app_.requestFocusCheck();
    }
    if (event == FL_KEYDOWN && Fl::event_key() == FL_Escape) {
        app_.hide();
        return 1;
    }
    return Fl_Double_Window::handle(event);
}

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

    window_ = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kTrayWindowClass,
        kAppName,
        WS_POPUP,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        instance,
        this);
    if (window_ == nullptr) {
        remove();
        return false;
    }

    using LoadIconMetricFunction = HRESULT(WINAPI*)(HINSTANCE, PCWSTR, int, HICON*);
    const auto loadIconMetric = reinterpret_cast<LoadIconMetricFunction>(
        GetProcAddress(GetModuleHandleW(L"comctl32.dll"), "LoadIconMetric"));
    if (loadIconMetric != nullptr && SUCCEEDED(loadIconMetric(instance, MAKEINTRESOURCEW(101), LIM_SMALL, &icon_))) {
        ownsIcon_ = true;
    } else {
        icon_ = static_cast<HICON>(LoadImageW(
            instance,
            MAKEINTRESOURCEW(101),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            LR_DEFAULTCOLOR));
        ownsIcon_ = icon_ != nullptr;
    }
    if (icon_ == nullptr) {
        icon_ = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    }

    notification_ = {};
    notification_.cbSize = sizeof(notification_);
    notification_.hWnd = window_;
    notification_.uID = 1;
    notification_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    notification_.uCallbackMessage = kTrayMessage;
    notification_.hIcon = icon_;
    lstrcpynW(notification_.szTip, kAppName, static_cast<int>(std::size(notification_.szTip)));
    if (Shell_NotifyIconW(NIM_ADD, &notification_) != TRUE) {
        remove();
        return false;
    }
    return true;
}

void TrayIcon::remove() {
    if (window_ != nullptr) {
        Shell_NotifyIconW(NIM_DELETE, &notification_);
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (ownsIcon_ && icon_ != nullptr) {
        DestroyIcon(icon_);
    }
    icon_ = nullptr;
    ownsIcon_ = false;

    if (ownsWindowClass_) {
        UnregisterClassW(kTrayWindowClass, GetModuleHandleW(nullptr));
        ownsWindowClass_ = false;
    }
}

LRESULT CALLBACK TrayIcon::windowProcedure(HWND window, UINT message, WPARAM wordParameter, LPARAM longParameter) {
    TrayIcon* trayIcon = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(window, GWLP_USERDATA));
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

LRESULT TrayIcon::handleMessage(HWND window, UINT message, WPARAM wordParameter, LPARAM longParameter) {
    static_cast<void>(wordParameter);
    if (message == kShowApplicationMessage) {
        openCallback_();
        return 0;
    }
    if (message == kTrayMessage) {
        if (longParameter == WM_LBUTTONUP) {
            openCallback_();
            return 0;
        }
        if (longParameter == WM_RBUTTONUP || longParameter == WM_CONTEXTMENU) {
            const HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, kTrayOpenCommand, L"Open Sys Record");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kTrayExitCommand, L"Exit");

            POINT cursor{};
            GetCursorPos(&cursor);
            SetForegroundWindow(window);
            const UINT command = TrackPopupMenu(
                menu,
                TPM_RETURNCMD | TPM_RIGHTBUTTON,
                cursor.x,
                cursor.y,
                0,
                window,
                nullptr);
            DestroyMenu(menu);
            PostMessageW(window, WM_NULL, 0, 0);

            if (command == kTrayOpenCommand) {
                openCallback_();
            } else if (command == kTrayExitCommand) {
                exitCallback_();
            }
            return 0;
        }
    }
    return DefWindowProcW(window, message, wordParameter, longParameter);
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
        MessageBoxA(nullptr, error.what(), "Sys Record", MB_OK | MB_ICONERROR);
        return 1;
    }
}
