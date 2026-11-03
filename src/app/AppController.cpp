#include "app/AppController.h"

#include "app/AppSettings.h"
#include "BuildVersion.h"
#include "platform/CaptureSelector.h"
#include "platform/WindowsStartup.h"
#include "resources/resource.h"

#include "ui/AppPage.h"

#include <Windows.h>
#include <propkey.h>
#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fastrecord {

namespace {

constexpr wchar_t kSingleInstanceName[] = L"Local\\FastRecord.SingleInstance.WebView";
constexpr wchar_t kWindowTitle[] = L"Fast Record";
constexpr wchar_t kTrayTooltip[] = L"Fast Record - painel de gravação";

std::wstring escapeJavaScriptString(const std::wstring& input) {
    std::wostringstream output;
    for (const wchar_t character : input) {
        switch (character) {
        case L'\\': output << L"\\\\"; break;
        case L'"': output << L"\\\""; break;
        case L'\n': output << L"\\n"; break;
        case L'\r': output << L"\\r"; break;
        case L'\t': output << L"\\t"; break;
        default:
            if (character < 0x20) {
                output << L' ';
            } else {
                output << character;
            }
            break;
        }
    }
    return output.str();
}

const wchar_t* jsonBoolean(bool value) {
    return value ? L"true" : L"false";
}

} // namespace

AppController::AppController(HINSTANCE instance)
    : m_instance(instance) {}

AppController::~AppController() {
    shutdown();
}

bool AppController::initialize(bool startHidden) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    SetLastError(ERROR_SUCCESS);
    m_singleInstanceMutex = CreateMutexW(nullptr, TRUE, kSingleInstanceName);
    if (m_singleInstanceMutex == nullptr) {
        return false;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const HWND existingWindow = FindWindowW(kWindowClassName, kWindowTitle);
        if (existingWindow != nullptr) {
            DWORD existingProcessId = 0;
            GetWindowThreadProcessId(existingWindow, &existingProcessId);
            if (existingProcessId != 0) {
                AllowSetForegroundWindow(existingProcessId);
            }
            ShowWindowAsync(existingWindow, SW_SHOWNORMAL);
            SetWindowPos(
                existingWindow,
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            DWORD_PTR activationResult = 0;
            SendMessageTimeoutW(
                existingWindow,
                kShowWindowMessage,
                0,
                0,
                SMTO_ABORTIFHUNG,
                2000,
                &activationResult);
            BringWindowToTop(existingWindow);
            SetForegroundWindow(existingWindow);
        }
        CloseHandle(m_singleInstanceMutex);
        m_singleInstanceMutex = nullptr;
        return false;
    }

    const AppSettings savedSettings = loadAppSettings();
    m_recorder.setEngine(savedSettings.engine);
    m_codec = savedSettings.codec;
    m_area = savedSettings.captureArea;
    m_resolution = savedSettings.resolution;
    m_framesPerSecond = savedSettings.framesPerSecond;
    m_bitrateMbps = savedSettings.bitrateMbps;
    m_microphoneEnabled = savedSettings.microphoneEnabled;
    m_systemAudioEnabled = savedSettings.systemAudioEnabled;
    m_alwaysOnTop = savedSettings.alwaysOnTop;
    m_startWithWindows = savedSettings.startWithWindows;
    m_settingsLoaded = true;

    if (m_startWithWindows && !platform::isStartWithWindowsEnabled()) {
        m_startWithWindows = false;
        persistSettings();
    } else if (m_startWithWindows) {
        std::wstring startupError;
        if (!platform::setStartWithWindows(true, startupError)) {
            m_startWithWindows = false;
            m_status = startupError;
            persistSettings();
        }
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    m_comInitialized = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        shutdown();
        return false;
    }

    m_nvencProbe = recording::probeNvenc();
    m_amfProbe = recording::probeAmf();
    m_qsvProbe = recording::probeQsv();
    refreshRecordings();

    if (!createMainWindow() || !createTrayIcon()) {
        shutdown();
        return false;
    }

    std::wstring appPage(ui::kAppPage);
    const std::wstring versionToken = L"__FASTRECORD_VERSION__";
    const size_t versionPosition = appPage.find(versionToken);
    if (versionPosition != std::wstring::npos) {
        appPage.replace(versionPosition, versionToken.size(), FASTRECORD_VERSION);
    }

    const bool webViewStarted = m_webView.initialize(
        m_window,
        std::move(appPage),
        [this](const std::wstring& message) { handleWebMessage(message); },
        [this](bool ready) {
            if (!ready) {
                m_status = L"Não foi possível iniciar a interface WebView2";
                m_tray.showBalloon(
                    L"Fast Record",
                    L"Falha ao abrir a interface. Repare ou instale o Microsoft Edge WebView2 Runtime.",
                    NIIF_ERROR);
            }
        });

    if (!webViewStarted) {
        shutdown();
        return false;
    }

    if (!startHidden) {
        showMainWindow();
    }
    SetTimer(m_window, 1, 250, nullptr);
    beginUpdateCheck();
    return true;
}

int AppController::run() {
    MSG message{};
    while (true) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status == -1) {
            return 1;
        }
        if (status == 0) {
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void AppController::shutdown() {
    if (m_shutdownStarted) {
        return;
    }
    m_shutdownStarted = true;

    if (m_updateThread.joinable()) {
        m_updateThread.join();
    }

    if (m_recorder.isRecording()) {
        m_recorder.stop();
    }

    persistSettings();

    m_tray.destroy();
    m_webView.close();

    if (m_window != nullptr) {
        DestroyWindow(m_window);
        m_window = nullptr;
    }

    if (m_singleInstanceMutex != nullptr) {
        ReleaseMutex(m_singleInstanceMutex);
        CloseHandle(m_singleInstanceMutex);
        m_singleInstanceMutex = nullptr;
    }

    if (m_comInitialized) {
        CoUninitialize();
        m_comInitialized = false;
    }
}

bool AppController::createMainWindow() {
    m_icon = LoadIconW(m_instance, MAKEINTRESOURCEW(IDI_FAST_RECORD));
    if (m_icon == nullptr) {
        return false;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.hInstance = m_instance;
    windowClass.lpfnWndProc = &AppController::windowProcedure;
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = m_icon;
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hbrBackground = CreateSolidBrush(RGB(8, 9, 12));

    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    m_window = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kWindowClassName,
        kWindowTitle,
        WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        kWindowWidth,
        kWindowHeight,
        nullptr,
        nullptr,
        m_instance,
        this);

    if (m_window == nullptr) {
        return false;
    }

    const HRGN roundedRegion = CreateRoundRectRgn(
        0, 0, kWindowWidth + 1, kWindowHeight + 1, 36, 36);
    SetWindowRgn(m_window, roundedRegion, TRUE);
    return true;
}

bool AppController::createTrayIcon() {
    return m_icon != nullptr && m_tray.create(
        m_window,
        m_icon,
        kTrayTooltip,
        [this](LPARAM event) { handleTrayEvent(0, event); });
}

void AppController::dockWindowToBottom() {
    if (m_window == nullptr) {
        return;
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &monitorInfo)) {
        return;
    }

    const RECT workArea = monitorInfo.rcWork;
    const int left = std::max(workArea.left + 10, workArea.right - kWindowWidth - 18);
    const int top = std::max(workArea.top + 10, workArea.bottom - kWindowHeight - 14);
    SetWindowPos(
        m_window,
        m_alwaysOnTop ? HWND_TOPMOST : HWND_TOP,
        left,
        top,
        kWindowWidth,
        kWindowHeight,
        SWP_NOACTIVATE);
}

void AppController::showMainWindow() {
    if (m_window == nullptr) {
        return;
    }
    dockWindowToBottom();
    ShowWindowAsync(m_window, SW_RESTORE);
    SetWindowPos(
        m_window,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(m_window);
    SetForegroundWindow(m_window);
    if (!m_alwaysOnTop) {
        SetWindowPos(
            m_window,
            HWND_NOTOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
    UpdateWindow(m_window);
}

void AppController::handleTrayEvent(WPARAM eventCode, LPARAM eventData) {
    const UINT event = static_cast<UINT>(LOWORD(eventData));
    const UINT alternateEvent = static_cast<UINT>(LOWORD(eventCode));
    const auto isMouseOrSelectionEvent = [](UINT value) {
        return value == WM_LBUTTONDOWN || value == WM_LBUTTONUP ||
            value == WM_LBUTTONDBLCLK || value == NIN_SELECT ||
            value == NIN_KEYSELECT || value == WM_RBUTTONDOWN ||
            value == WM_RBUTTONUP || value == WM_CONTEXTMENU;
    };
    const UINT normalizedEvent = isMouseOrSelectionEvent(event) ? event : alternateEvent;

    switch (normalizedEvent) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case NIN_SELECT:
    case NIN_KEYSELECT:
        showMainWindow();
        break;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        showTrayMenu();
        break;
    default:
        break;
    }
}

void AppController::showTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    AppendMenuW(menu, MF_STRING, CommandShowWindow, L"Abrir Fast Record");
    AppendMenuW(
        menu,
        MF_STRING,
        CommandToggle,
        m_recorder.isRecording() ? L"Parar gravação" : L"Gravar monitor atual");
    AppendMenuW(menu, MF_STRING, CommandOpenFolder, L"Abrir pasta de gravações");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CommandAbout, L"Sobre");
    AppendMenuW(menu, MF_STRING, CommandExit, L"Sair");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(m_window);
    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        cursor.x,
        cursor.y,
        0,
        m_window,
        nullptr);
    PostMessageW(m_window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void AppController::handleCommand(UINT_PTR command) {
    switch (command) {
    case CommandShowWindow: showMainWindow(); break;
    case CommandToggle: toggleRecording(); break;
    case CommandOpenFolder: openRecordingsFolder(); break;
    case CommandAbout: showAboutDialog(); break;
    case CommandExit: PostQuitMessage(0); break;
    default: break;
    }
}

void AppController::handleWebMessage(const std::wstring& message) {
    if (message == L"ready") {
        m_webViewReady = true;
        syncInterface();
    } else if (message == L"check-updates") {
        beginUpdateCheck();
    } else if (message == L"download-update") {
        beginUpdateDownload();
    } else if (message == L"record") {
        startRecording();
    } else if (message == L"pause") {
        togglePause();
    } else if (message == L"stop") {
        stopRecording();
    } else if (message == L"toggle-mic") {
        m_microphoneEnabled = !m_microphoneEnabled;
        persistSettings();
        syncInterface();
    } else if (message == L"toggle-system-audio") {
        m_systemAudioEnabled = !m_systemAudioEnabled;
        persistSettings();
        syncInterface();
    } else if (message == L"toggle-start-with-windows") {
        const bool enabled = !m_startWithWindows;
        std::wstring startupError;
        if (platform::setStartWithWindows(enabled, startupError)) {
            m_startWithWindows = enabled;
            persistSettings();
        } else {
            m_status = startupError;
            m_tray.showBalloon(L"Fast Record", startupError, NIIF_ERROR);
        }
        syncInterface();
    } else if (message == L"open-folder") {
        openRecordingsFolder();
    } else if (message == L"refresh-recordings") {
        refreshRecordings();
        syncInterface();
    } else if (message.starts_with(L"open-recording:") || message.starts_with(L"show-recording:")) {
        const bool reveal = message.starts_with(L"show-recording:");
        const std::filesystem::path recordingPath(message.substr(15));
        const auto directory = recordingsDirectory();
        std::error_code pathError;
        std::wstring extension = recordingPath.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        const bool isMp4 = extension == L".mp4";
        const bool isInRecordingsDirectory =
            std::filesystem::equivalent(recordingPath.parent_path(), directory, pathError) &&
            !pathError;
        if (isMp4 && isInRecordingsDirectory &&
            std::filesystem::exists(recordingPath, pathError) && !pathError) {
            if (reveal) {
                const std::wstring parameters = L"/select,\"" + recordingPath.wstring() + L"\"";
                ShellExecuteW(m_window, L"open", L"explorer.exe", parameters.c_str(), nullptr, SW_SHOWNORMAL);
            } else {
                ShellExecuteW(m_window, L"open", recordingPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
    } else if (message == L"pin") {
        m_alwaysOnTop = !m_alwaysOnTop;
        SetWindowPos(
            m_window,
            m_alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        persistSettings();
        syncInterface();
    } else if (message == L"minimize") {
        ShowWindow(m_window, SW_MINIMIZE);
    } else if (message == L"hide") {
        ShowWindow(m_window, SW_HIDE);
    } else if (message == L"exit") {
        PostQuitMessage(0);
    } else if (message == L"drag") {
        ReleaseCapture();
        SendMessageW(m_window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    } else if (message.starts_with(L"engine:")) {
        const std::wstring value = message.substr(7);
        if (value == L"nvenc") {
            m_recorder.setEngine(recording::EncoderEngine::Nvenc);
        } else if (value == L"amf") {
            m_recorder.setEngine(recording::EncoderEngine::Amf);
        } else if (value == L"qsv") {
            m_recorder.setEngine(recording::EncoderEngine::Qsv);
        } else if (value == L"software") {
            m_recorder.setEngine(recording::EncoderEngine::Software);
        } else {
            m_recorder.setEngine(recording::EncoderEngine::Automatic);
        }
        persistSettings();
        syncInterface();
    } else if (message.starts_with(L"codec:")) {
        const std::wstring value = message.substr(6);
        if (value == L"av1") {
            m_codec = recording::VideoCodec::Av1;
        } else {
            m_codec = recording::VideoCodec::H264;
        }
        persistSettings();
        syncInterface();
    } else if (message.starts_with(L"area:")) {
        const std::wstring area = message.substr(5);
        if (area == L"monitor" || area == L"window" || area == L"region") {
            m_area = area;
            persistSettings();
        }
        syncInterface();
    } else if (message.starts_with(L"bitrate:")) {
        const std::wstring value = message.substr(8);
        wchar_t* end = nullptr;
        const long parsed = std::wcstol(value.c_str(), &end, 10);
        if (end != value.c_str() && *end == L'\0') {
            m_bitrateMbps = static_cast<int>(std::clamp(parsed, 4L, 80L));
            persistSettings();
        }
        syncInterface();
    } else if (message.starts_with(L"resolution:")) {
        const std::wstring resolution = message.substr(11);
        if (resolution == L"1280x720" || resolution == L"1920x1080" ||
            resolution == L"2560x1440" || resolution == L"3840x2160") {
            m_resolution = resolution;
            persistSettings();
        }
        syncInterface();
    } else if (message.starts_with(L"fps:")) {
        const std::wstring value = message.substr(4);
        wchar_t* end = nullptr;
        const long parsed = std::wcstol(value.c_str(), &end, 10);
        if (end != value.c_str() && *end == L'\0' &&
            (parsed == 24 || parsed == 30 || parsed == 60 || parsed == 120)) {
            m_framesPerSecond = static_cast<int>(parsed);
            persistSettings();
        }
        syncInterface();
    }
}

void AppController::beginUpdateCheck() {
    if (m_updateBusy || m_shutdownStarted) {
        return;
    }
    if (m_updateThread.joinable()) {
        m_updateThread.join();
    }

    m_updateBusy = true;
    m_updateStatus = L"Verificando atualizações…";
    const HWND window = m_window;
    try {
        m_updateThread = std::thread([this, window] {
            UpdateTaskResult result;
            result.kind = UpdateTaskResult::Kind::Check;
            result.check = updates::checkForUpdate(FASTRECORD_VERSION);
            {
                std::lock_guard lock(m_updateResultMutex);
                m_pendingUpdateResult = std::move(result);
            }
            PostMessageW(window, kUpdateTaskFinishedMessage, 0, 0);
        });
    } catch (...) {
        m_updateBusy = false;
        m_updateStatus = L"Não foi possível iniciar a verificação de atualizações.";
    }
    syncInterface();
}

void AppController::beginUpdateDownload() {
    if (m_updateBusy || m_shutdownStarted || !m_availableUpdate.success ||
        !m_availableUpdate.updateAvailable) {
        return;
    }
    if (m_updateThread.joinable()) {
        m_updateThread.join();
    }

    m_updateBusy = true;
    m_updateStatus = L"Baixando e validando o instalador…";
    const HWND window = m_window;
    const updates::CheckResult update = m_availableUpdate;
    try {
        m_updateThread = std::thread([this, window, update] {
            UpdateTaskResult result;
            result.kind = UpdateTaskResult::Kind::Download;
            if (!updates::downloadAndVerifyInstaller(update, result.installerPath, result.error)) {
                result.installerPath.clear();
            }
            {
                std::lock_guard lock(m_updateResultMutex);
                m_pendingUpdateResult = std::move(result);
            }
            PostMessageW(window, kUpdateTaskFinishedMessage, 0, 0);
        });
    } catch (...) {
        m_updateBusy = false;
        m_updateStatus = L"Não foi possível iniciar o download do instalador.";
    }
    syncInterface();
}

void AppController::handleUpdateTaskFinished() {
    if (m_updateThread.joinable()) {
        m_updateThread.join();
    }

    std::optional<UpdateTaskResult> result;
    {
        std::lock_guard lock(m_updateResultMutex);
        result = std::move(m_pendingUpdateResult);
        m_pendingUpdateResult.reset();
    }
    m_updateBusy = false;
    if (!result.has_value()) {
        m_updateStatus = L"A tarefa de atualização terminou sem um resultado.";
        syncInterface();
        return;
    }

    if (result->kind == UpdateTaskResult::Kind::Check) {
        m_availableUpdate = std::move(result->check);
        if (!m_availableUpdate.success) {
            m_updateStatus = m_availableUpdate.error.empty()
                ? L"Não foi possível verificar atualizações."
                : m_availableUpdate.error;
        } else if (m_availableUpdate.updateAvailable) {
            m_updateStatus = L"Versão " + m_availableUpdate.version + L" disponível.";
            if (!IsWindowVisible(m_window)) {
                m_tray.showBalloon(
                    L"Fast Record",
                    L"A versão " + m_availableUpdate.version +
                        L" está disponível. Abra as configurações para atualizar.",
                    NIIF_INFO);
            }
        } else {
            m_updateStatus = L"Fast Record atualizado · v" +
                std::wstring(FASTRECORD_VERSION) + L".";
        }
        syncInterface();
        return;
    }

    if (result->installerPath.empty()) {
        m_updateStatus = result->error.empty()
            ? L"Não foi possível baixar e validar o instalador."
            : result->error;
        syncInterface();
        return;
    }

    const HINSTANCE launchResult = ShellExecuteW(
        m_window,
        L"open",
        result->installerPath.c_str(),
        nullptr,
        nullptr,
        SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(launchResult) > 32) {
        PostQuitMessage(0);
        return;
    }
    m_updateStatus = L"Não foi possível abrir o instalador baixado.";
    syncInterface();
}

void AppController::syncInterface() {
    if (!m_webViewReady) {
        return;
    }

    const std::wstring script =
        L"window.fastRecord&&window.fastRecord.setState({"
        L"recording:" + std::wstring(jsonBoolean(m_recorder.isRecording())) +
        L",paused:" + std::wstring(jsonBoolean(m_recorder.isPaused())) +
        L",microphone:" + std::wstring(jsonBoolean(m_microphoneEnabled)) +
        L",systemAudio:" + std::wstring(jsonBoolean(m_systemAudioEnabled)) +
        L",pinned:" + std::wstring(jsonBoolean(m_alwaysOnTop)) +
        L",startWithWindows:" + std::wstring(jsonBoolean(m_startWithWindows)) +
        L",updateAvailable:" + std::wstring(jsonBoolean(
            m_availableUpdate.success && m_availableUpdate.updateAvailable)) +
        L",updateBusy:" + std::wstring(jsonBoolean(m_updateBusy)) +
        L",updateStatus:\"" + escapeJavaScriptString(m_updateStatus) +
        L"\"" +
        L",elapsedSeconds:" + std::to_wstring(recordingElapsedSeconds()) +
        L",bitrate:" + std::to_wstring(m_bitrateMbps) +
        L",resolution:\"" + escapeJavaScriptString(m_resolution) +
        L"\",fps:\"" + std::to_wstring(m_framesPerSecond) +
        L"\",engine:\"" + escapeJavaScriptString(engineValue()) +
        L"\",codec:\"" + escapeJavaScriptString(codecValue()) +
        L"\",area:\"" + escapeJavaScriptString(m_area) +
        L"\",recordings:" + m_recordingsJson +
        L",status:\"" + escapeJavaScriptString(statusText()) +
        L"\",capability:\"" + escapeJavaScriptString(selectedEngineStatus()) +
        L"\"});";
    m_webView.executeScript(script);
    updateTrayTooltip();
}

void AppController::persistSettings() {
    if (!m_settingsLoaded) {
        return;
    }

    const AppSettings settings{
        m_recorder.engine(),
        m_codec,
        m_area,
        m_resolution,
        m_framesPerSecond,
        m_bitrateMbps,
        m_microphoneEnabled,
        m_systemAudioEnabled,
        m_alwaysOnTop,
        m_startWithWindows,
    };
    saveAppSettings(settings);
}

void AppController::startRecording() {
    unsigned int width = 1920;
    unsigned int height = 1080;
    if (swscanf_s(m_resolution.c_str(), L"%ux%u", &width, &height) != 2) {
        width = 1920;
        height = 1080;
    }

    recording::RecordingSettings settings;
    POINT cursor{};
    GetCursorPos(&cursor);
    settings.target.monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);

    if (m_area == L"window") {
        const HWND selectedWindow = platform::chooseCaptureWindow(m_window, m_window);
        if (selectedWindow == nullptr) {
            m_status = L"Seleção de janela cancelada.";
            syncInterface();
            return;
        }
        settings.target.kind = recording::CaptureTargetKind::SelectedWindow;
        settings.target.window = selectedWindow;
        settings.target.monitor = MonitorFromWindow(selectedWindow, MONITOR_DEFAULTTONEAREST);
    } else if (m_area == L"region") {
        RECT selectedRegion{};
        if (!platform::chooseCaptureRegion(m_window, settings.target.monitor, selectedRegion)) {
            m_status = L"Seleção de região cancelada.";
            syncInterface();
            return;
        }
        settings.target.kind = recording::CaptureTargetKind::SelectedRegion;
        settings.captureRegion = selectedRegion;
    } else {
        settings.target.kind = recording::CaptureTargetKind::CurrentMonitor;
    }

    settings.outputDirectory = recordingsDirectory();
    settings.width = width;
    settings.height = height;
    settings.framesPerSecond = static_cast<std::uint32_t>(m_framesPerSecond);
    settings.bitrateMbps = static_cast<std::uint32_t>(m_bitrateMbps);
    settings.codec = m_codec;
    settings.captureCursor = true;
    settings.captureSystemAudio = m_systemAudioEnabled;
    settings.captureMicrophone = m_microphoneEnabled;

    const auto result = m_recorder.start(settings);
    m_status = result.message;
    if (result.success) {
        m_recordingElapsed = std::chrono::milliseconds{0};
        m_recordingResumedAt = std::chrono::steady_clock::now();
        m_recordingClockRunning = true;
    }
    if (result.success) {
        if (m_microphoneEnabled && m_systemAudioEnabled) {
            m_status = L"Gravando tela, microfone e áudio do PC.";
        } else if (m_microphoneEnabled) {
            m_status = L"Gravando tela e microfone.";
        } else if (m_systemAudioEnabled) {
            m_status = L"Gravando tela e áudio do PC.";
        } else {
            m_status = L"Gravando tela sem áudio.";
        }
    }
    if (!result.success) {
        m_tray.showBalloon(L"Fast Record", result.message, NIIF_WARNING);
    }
    syncInterface();
}

void AppController::togglePause() {
    const auto result = m_recorder.isPaused() ? m_recorder.resume() : m_recorder.pause();
    m_status = result.message;
    if (result.success) {
        if (m_recorder.isPaused()) {
            m_recordingElapsed += std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_recordingResumedAt);
            m_recordingClockRunning = false;
        } else {
            m_recordingResumedAt = std::chrono::steady_clock::now();
            m_recordingClockRunning = true;
        }
    }
    if (!result.success) {
        m_tray.showBalloon(L"Fast Record", result.message, NIIF_WARNING);
    }
    syncInterface();
}

void AppController::stopRecording() {
    if (m_recordingClockRunning) {
        m_recordingElapsed += std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_recordingResumedAt);
    }
    m_recordingClockRunning = false;
    const auto result = m_recorder.stop();
    const auto output = m_recorder.outputPath();
    m_status = result.success && !output.empty()
        ? L"Vídeo salvo em " + output.filename().wstring()
        : result.message;
    m_tray.showBalloon(
        L"Fast Record",
        result.message,
        result.success ? NIIF_INFO : NIIF_WARNING);
    refreshRecordings();
    syncInterface();
}

void AppController::toggleRecording() {
    if (m_recorder.isRecording()) {
        stopRecording();
    } else {
        startRecording();
    }
}

void AppController::updateTrayTooltip() {
    if (m_recorder.isPaused()) {
        m_tray.setTooltip(L"Fast Record - pausado");
    } else if (m_recorder.isRecording()) {
        m_tray.setTooltip(L"Fast Record - gravando");
    } else {
        m_tray.setTooltip(L"Fast Record - pronto");
    }
}

std::wstring AppController::engineValue() const {
    switch (m_recorder.engine()) {
    case recording::EncoderEngine::Nvenc: return L"nvenc";
    case recording::EncoderEngine::Amf: return L"amf";
    case recording::EncoderEngine::Qsv: return L"qsv";
    case recording::EncoderEngine::Software: return L"software";
    default: return L"auto";
    }
}

std::wstring AppController::codecValue() const {
    return m_codec == recording::VideoCodec::Av1 ? L"av1" : L"h264";
}

std::wstring AppController::selectedEngineStatus() const {
    const wchar_t* codec = m_codec == recording::VideoCodec::Av1 ? L"AV1" : L"H.264";
    if (m_codec == recording::VideoCodec::Av1 && m_recorder.engine() != recording::EncoderEngine::Nvenc) {
        return L"AV1 nesta versão exige selecionar um encoder NVENC compatível.";
    }

    switch (m_recorder.engine()) {
    case recording::EncoderEngine::Nvenc:
        if (m_codec == recording::VideoCodec::Av1 && !m_nvencProbe.av1Supported) {
            return m_nvencProbe.description + L" AV1 não está disponível nesta GPU/driver.";
        }
        return m_nvencProbe.description +
            L" A gravação atual usa o backend direto NVENC com " + codec + L".";
    case recording::EncoderEngine::Amf:
        return m_amfProbe.description +
            L" A gravação atual usa o backend direto AMD AMF para H.264.";
    case recording::EncoderEngine::Qsv:
        if (!m_qsvProbe.h264Supported) {
            return m_qsvProbe.description +
                L" Selecione Automático ou Software para continuar sem QSV.";
        }
        return m_qsvProbe.description +
            L" A gravação atual usa Intel Quick Sync via Media Foundation.";
    case recording::EncoderEngine::Software:
        return std::wstring(L"Compatível com qualquer GPU; utiliza a CPU para ") + codec + L".";
    default:
        if (m_codec == recording::VideoCodec::Av1) {
            if (m_nvencProbe.av1Supported) {
                return L"AV1 por NVENC disponível; selecione NVENC para gravar nesse formato.";
            }
            return L"AV1 requer NVENC compatível; selecione NVENC após atualizar o driver, se necessário.";
        }
        if (m_nvencProbe.h264Supported) {
            return L"H.264 por hardware disponível; o Media Foundation escolherá o encoder.";
        }
        if (m_amfProbe.h264Supported) {
            return L"H.264 por hardware disponível; o Media Foundation escolherá o encoder.";
        }
        if (m_qsvProbe.h264Supported) {
            return L"H.264 por hardware disponível, incluindo Intel QSV; o Media Foundation escolherá o encoder.";
        }
        if (m_amfProbe.runtimeLibraryFound) {
            return L"Runtime AMD AMF detectado; validação completa requer o SDK.";
        }
        if (m_nvencProbe.runtimeLibraryFound) {
            return L"Runtime NVIDIA NVENC detectado; validação completa requer o SDK.";
        }
        return L"Nenhum encoder de GPU confirmado; será usado software.";
    }
}

std::wstring AppController::statusText() const {
    if (m_recorder.isPaused()) {
        return L"Gravação pausada";
    }
    if (m_recorder.isRecording()) {
        return L"Gravando agora";
    }
    return m_status;
}

std::uint64_t AppController::recordingElapsedSeconds() const {
    auto elapsed = m_recordingElapsed;
    if (m_recordingClockRunning) {
        elapsed += std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_recordingResumedAt);
    }
    return elapsed.count() > 0
        ? static_cast<std::uint64_t>(elapsed.count() / 1000)
        : 0;
}

std::filesystem::path AppController::recordingsDirectory() const {
    PWSTR rawPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_DEFAULT, nullptr, &rawPath)) ||
        rawPath == nullptr) {
        return {};
    }

    std::filesystem::path directory(rawPath);
    CoTaskMemFree(rawPath);
    directory /= L"Fast Record";
    return directory;
}

void AppController::refreshRecordings() {
    struct RecordingFile {
        std::filesystem::path path;
        std::filesystem::file_time_type modified;
        std::uintmax_t size{0};
        std::uint64_t duration{0};
    };

    std::vector<RecordingFile> files;
    const auto directory = recordingsDirectory();
    std::error_code error;
    if (directory.empty() || !std::filesystem::exists(directory, error) || error) {
        m_recordingsJson = L"[]";
        return;
    }

    for (std::filesystem::directory_iterator iterator(
             directory, std::filesystem::directory_options::skip_permission_denied, error),
         end;
         iterator != end && !error;
         iterator.increment(error)) {
        const auto& entry = *iterator;
        std::error_code entryError;
        if (!entry.is_regular_file(entryError) || entryError) {
            continue;
        }

        std::wstring extension = entry.path().extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
        if (extension != L".mp4") {
            continue;
        }

        const auto modified = entry.last_write_time(entryError);
        if (entryError) {
            continue;
        }
        const auto size = entry.file_size(entryError);
        std::uint64_t duration = 0;
        Microsoft::WRL::ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(SHGetPropertyStoreFromParsingName(
                entry.path().c_str(),
                nullptr,
                GPS_BESTEFFORT,
                IID_PPV_ARGS(&properties)))) {
            PROPVARIANT value{};
            PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(PKEY_Media_Duration, &value)) && value.vt == VT_UI8) {
                duration = value.uhVal.QuadPart / 10'000'000ull;
            }
            PropVariantClear(&value);
        }
        files.push_back({entry.path(), modified, entryError ? 0 : size, duration});
    }

    std::sort(files.begin(), files.end(), [](const RecordingFile& left, const RecordingFile& right) {
        return left.modified > right.modified;
    });
    if (files.size() > 50) {
        files.resize(50);
    }

    const auto formatSize = [](std::uintmax_t bytes) {
        std::wostringstream output;
        if (bytes >= 1024ull * 1024ull * 1024ull) {
            output << std::fixed << std::setprecision(1)
                << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << L" GB";
        } else if (bytes >= 1024ull * 1024ull) {
            output << std::fixed << std::setprecision(1)
                << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << L" MB";
        } else {
            output << std::max<std::uintmax_t>(1, bytes / 1024ull) << L" KB";
        }
        return output.str();
    };

    const auto formatTime = [](const std::filesystem::path& path) {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
            return std::wstring(L"Data desconhecida");
        }

        FILETIME localTime{};
        SYSTEMTIME systemTime{};
        if (!FileTimeToLocalFileTime(&attributes.ftLastWriteTime, &localTime) ||
            !FileTimeToSystemTime(&localTime, &systemTime)) {
            return std::wstring(L"Data desconhecida");
        }

        wchar_t formatted[64]{};
        swprintf_s(
            formatted,
            _countof(formatted),
            L"%02u/%02u/%04u · %02u:%02u",
            static_cast<unsigned>(systemTime.wDay),
            static_cast<unsigned>(systemTime.wMonth),
            static_cast<unsigned>(systemTime.wYear),
            static_cast<unsigned>(systemTime.wHour),
            static_cast<unsigned>(systemTime.wMinute));
        return std::wstring(formatted);
    };

    const auto formatDuration = [](std::uint64_t seconds) {
        if (seconds == 0) {
            return std::wstring{};
        }
        const auto hours = seconds / 3600;
        const auto minutes = (seconds % 3600) / 60;
        const auto remainder = seconds % 60;
        wchar_t formatted[24]{};
        if (hours > 0) {
            swprintf_s(formatted, _countof(formatted), L"%02llu:%02llu:%02llu", hours, minutes, remainder);
        } else {
            swprintf_s(formatted, _countof(formatted), L"%02llu:%02llu", minutes, remainder);
        }
        return std::wstring(formatted);
    };

    std::wstring json = L"[";
    for (size_t index = 0; index < files.size(); ++index) {
        if (index != 0) {
            json += L",";
        }
        const auto& file = files[index];
        json += L"{\"name\":\"" + escapeJavaScriptString(file.path.filename().wstring()) +
            L"\",\"path\":\"" + escapeJavaScriptString(file.path.wstring()) +
            L"\",\"size\":\"" + escapeJavaScriptString(formatSize(file.size)) +
            L"\",\"duration\":\"" + escapeJavaScriptString(formatDuration(file.duration)) +
            L"\",\"time\":\"" + escapeJavaScriptString(formatTime(file.path)) + L"\"}";
    }
    json += L"]";
    m_recordingsJson = std::move(json);
}

void AppController::openRecordingsFolder() {
    const auto directory = recordingsDirectory();
    if (directory.empty()) {
        m_status = L"Não foi possível localizar a pasta Vídeos";
        syncInterface();
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        m_status = L"Não foi possível criar a pasta de gravações";
        syncInterface();
        return;
    }

    ShellExecuteW(m_window, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void AppController::showAboutDialog() {
    const std::wstring message =
        std::wstring(L"Fast Record ") + FASTRECORD_VERSION +
        L"\n\nGravador rápido de tela para Windows.\n\n" +
        m_nvencProbe.description + L"\n" + m_amfProbe.description + L"\n" +
        m_qsvProbe.description;
    MessageBoxW(m_window, message.c_str(), L"Sobre o Fast Record", MB_OK | MB_ICONINFORMATION);
}

LRESULT CALLBACK AppController::windowProcedure(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto* app = static_cast<AppController*>(createStruct->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }

    auto* app = reinterpret_cast<AppController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return app == nullptr
        ? DefWindowProcW(hwnd, message, wParam, lParam)
        : app->handleMessage(hwnd, message, wParam, lParam);
}

LRESULT AppController::handleMessage(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
    if (message == platform::TrayIcon::kCallbackMessage) {
        if (m_tray.isCreated()) {
            handleTrayEvent(wParam, lParam);
        }
        return 0;
    }

    if (message == kShowWindowMessage) {
        showMainWindow();
        return 0;
    }

    if (message == kUpdateTaskFinishedMessage) {
        handleUpdateTaskFinished();
        return 0;
    }

    switch (message) {
    case WM_TIMER:
        if (wParam == 1) {
            if (m_recorder.hasFinished()) {
                stopRecording();
            } else if (m_recorder.isRecording()) {
                syncInterface();
            }
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            ShowWindow(hwnd, SW_HIDE);
        } else {
            m_webView.resize();
        }
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        dockWindowToBottom();
        return 0;
    case WM_COMMAND:
        handleCommand(LOWORD(wParam));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

} // namespace fastrecord
