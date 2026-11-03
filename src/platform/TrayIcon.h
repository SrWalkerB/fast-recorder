#pragma once

#include <Windows.h>
#include <shellapi.h>

#include <functional>
#include <string>

namespace fastrecord::platform {

class TrayIcon final {
public:
    static constexpr UINT kCallbackMessage = WM_APP + 1;
    using Callback = std::function<void(LPARAM)>;

    TrayIcon() = default;
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;
    ~TrayIcon();

    static HICON createPulseFrame(HICON baseIcon, bool expanded);

    bool create(HWND owner, HICON icon, const std::wstring& tooltip, Callback callback);
    void destroy();

    bool setIcon(HICON icon);
    bool setTooltip(const std::wstring& tooltip);
    bool showBalloon(
        const std::wstring& title,
        const std::wstring& message,
        DWORD infoFlags = NIIF_INFO);

    bool isCreated() const noexcept { return m_created; }

private:
    NOTIFYICONDATAW m_data{};
    Callback m_callback;
    bool m_created{false};
};

} // namespace fastrecord::platform
