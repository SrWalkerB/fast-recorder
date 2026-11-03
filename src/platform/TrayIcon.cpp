#include "platform/TrayIcon.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <utility>

namespace fastrecord::platform {

namespace {

template <size_t Size>
void copyText(wchar_t (&destination)[Size], const std::wstring& source) {
    wcsncpy_s(destination, Size, source.c_str(), _TRUNCATE);
}

struct IconBitmapGuard {
    ICONINFO& info;
    ~IconBitmapGuard() {
        if (info.hbmColor != nullptr) {
            DeleteObject(info.hbmColor);
        }
        if (info.hbmMask != nullptr) {
            DeleteObject(info.hbmMask);
        }
    }
};

std::uint8_t blendChannel(std::uint8_t original, std::uint8_t overlay, std::uint8_t alpha) {
    const unsigned int inverse = 255u - alpha;
    return static_cast<std::uint8_t>(
        (static_cast<unsigned int>(overlay) * alpha +
         static_cast<unsigned int>(original) * inverse + 127u) / 255u);
}

} // namespace

TrayIcon::~TrayIcon() {
    destroy();
}

HICON TrayIcon::createPulseFrame(HICON baseIcon, bool expanded) {
    if (baseIcon == nullptr) {
        return nullptr;
    }

    ICONINFO sourceInfo{};
    if (!GetIconInfo(baseIcon, &sourceInfo) || sourceInfo.hbmColor == nullptr) {
        if (sourceInfo.hbmColor != nullptr) {
            DeleteObject(sourceInfo.hbmColor);
        }
        if (sourceInfo.hbmMask != nullptr) {
            DeleteObject(sourceInfo.hbmMask);
        }
        return nullptr;
    }
    const IconBitmapGuard sourceBitmaps{sourceInfo};

    BITMAP sourceBitmap{};
    if (GetObjectW(sourceInfo.hbmColor, sizeof(sourceBitmap), &sourceBitmap) == 0) {
        return nullptr;
    }
    const int width = sourceBitmap.bmWidth;
    const int height = std::abs(sourceBitmap.bmHeight);
    if (width < 8 || height < 8 || width > 128 || height > 128) {
        return nullptr;
    }

    BITMAPINFO colorDescription{};
    colorDescription.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    colorDescription.bmiHeader.biWidth = width;
    colorDescription.bmiHeader.biHeight = -height;
    colorDescription.bmiHeader.biPlanes = 1;
    colorDescription.bmiHeader.biBitCount = 32;
    colorDescription.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return nullptr;
    }
    void* colorPixels = nullptr;
    HBITMAP colorBitmap = CreateDIBSection(
        screen,
        &colorDescription,
        DIB_RGB_COLORS,
        &colorPixels,
        nullptr,
        0);
    if (colorBitmap == nullptr || colorPixels == nullptr) {
        if (colorBitmap != nullptr) {
            DeleteObject(colorBitmap);
        }
        ReleaseDC(nullptr, screen);
        return nullptr;
    }
    std::memset(colorPixels, 0, static_cast<std::size_t>(width) * height * 4);

    HDC memoryDc = CreateCompatibleDC(screen);
    if (memoryDc == nullptr) {
        DeleteObject(colorBitmap);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }
    const HGDIOBJ previousBitmap = SelectObject(memoryDc, colorBitmap);
    const bool iconDrawn = DrawIconEx(
        memoryDc,
        0,
        0,
        baseIcon,
        width,
        height,
        0,
        nullptr,
        DI_NORMAL) != FALSE;
    if (previousBitmap != nullptr && previousBitmap != HGDI_ERROR) {
        SelectObject(memoryDc, previousBitmap);
    }
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screen);
    if (!iconDrawn) {
        DeleteObject(colorBitmap);
        return nullptr;
    }

    auto* pixels = static_cast<std::uint8_t*>(colorPixels);
    const int dimension = std::min(width, height);
    const float centerX = static_cast<float>(width - 1) / 2.0F;
    const float centerY = static_cast<float>(height - 1) / 2.0F;
    const float radius = static_cast<float>(dimension) * (expanded ? 0.44F : 0.37F);
    const float halfThickness = std::max(0.7F, static_cast<float>(dimension) * 0.055F);
    const std::uint8_t peakAlpha = expanded ? 150 : 90;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float dx = static_cast<float>(x) - centerX;
            const float dy = static_cast<float>(y) - centerY;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float coverage = std::clamp(
                halfThickness + 0.55F - std::abs(distance - radius),
                0.0F,
                1.0F);
            if (coverage <= 0.0F) {
                continue;
            }

            const auto alpha = static_cast<std::uint8_t>(peakAlpha * coverage);
            const std::size_t offset =
                (static_cast<std::size_t>(y) * width + x) * 4;
            pixels[offset] = blendChannel(pixels[offset], 61, alpha);
            pixels[offset + 1] = blendChannel(pixels[offset + 1], 58, alpha);
            pixels[offset + 2] = blendChannel(pixels[offset + 2], 255, alpha);
            pixels[offset + 3] = static_cast<std::uint8_t>(
                alpha + static_cast<unsigned int>(pixels[offset + 3]) * (255u - alpha) / 255u);
        }
    }

    struct MonochromeBitmapInfo {
        BITMAPINFOHEADER header;
        RGBQUAD colors[2];
    } maskDescription{};
    maskDescription.header.biSize = sizeof(BITMAPINFOHEADER);
    maskDescription.header.biWidth = width;
    maskDescription.header.biHeight = -height;
    maskDescription.header.biPlanes = 1;
    maskDescription.header.biBitCount = 1;
    maskDescription.header.biCompression = BI_RGB;
    maskDescription.colors[0] = RGBQUAD{0, 0, 0, 0};
    maskDescription.colors[1] = RGBQUAD{255, 255, 255, 0};
    void* maskPixels = nullptr;
    screen = GetDC(nullptr);
    if (screen == nullptr) {
        DeleteObject(colorBitmap);
        return nullptr;
    }
    HBITMAP maskBitmap = CreateDIBSection(
        screen,
        reinterpret_cast<BITMAPINFO*>(&maskDescription),
        DIB_RGB_COLORS,
        &maskPixels,
        nullptr,
        0);
    ReleaseDC(nullptr, screen);
    if (maskBitmap == nullptr || maskPixels == nullptr) {
        if (maskBitmap != nullptr) {
            DeleteObject(maskBitmap);
        }
        DeleteObject(colorBitmap);
        return nullptr;
    }

    const std::size_t maskStride = ((static_cast<std::size_t>(width) + 31u) / 32u) * 4u;
    auto* maskBytes = static_cast<std::uint8_t*>(maskPixels);
    std::memset(maskBytes, 0, maskStride * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t colorOffset = (static_cast<std::size_t>(y) * width + x) * 4;
            if (pixels[colorOffset + 3] == 0) {
                const std::size_t maskOffset = static_cast<std::size_t>(y) * maskStride +
                    static_cast<std::size_t>(x) / 8u;
                maskBytes[maskOffset] |= static_cast<std::uint8_t>(0x80u >> (x % 8));
            }
        }
    }

    ICONINFO frameInfo{};
    frameInfo.fIcon = TRUE;
    frameInfo.hbmColor = colorBitmap;
    frameInfo.hbmMask = maskBitmap;
    const HICON frame = CreateIconIndirect(&frameInfo);
    DeleteObject(maskBitmap);
    DeleteObject(colorBitmap);
    return frame;
}

bool TrayIcon::create(
    HWND owner,
    HICON icon,
    const std::wstring& tooltip,
    Callback callback) {
    if (m_created || owner == nullptr || icon == nullptr) {
        return false;
    }

    m_data = {};
    m_data.cbSize = sizeof(m_data);
    m_data.hWnd = owner;
    m_data.uID = 1;
    m_data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    m_data.uCallbackMessage = kCallbackMessage;
    m_data.hIcon = icon;
    copyText(m_data.szTip, tooltip);

    if (!Shell_NotifyIconW(NIM_ADD, &m_data)) {
        m_data = {};
        return false;
    }

    m_data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &m_data);

    m_callback = std::move(callback);
    m_created = true;
    return true;
}

void TrayIcon::destroy() {
    if (!m_created) {
        return;
    }

    Shell_NotifyIconW(NIM_DELETE, &m_data);
    m_callback = {};
    m_data = {};
    m_created = false;
}

bool TrayIcon::setIcon(HICON icon) {
    if (!m_created || icon == nullptr) {
        return false;
    }

    m_data.hIcon = icon;
    m_data.uFlags = NIF_ICON;
    return Shell_NotifyIconW(NIM_MODIFY, &m_data) == TRUE;
}

bool TrayIcon::setTooltip(const std::wstring& tooltip) {
    if (!m_created) {
        return false;
    }

    copyText(m_data.szTip, tooltip);
    m_data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    return Shell_NotifyIconW(NIM_MODIFY, &m_data) == TRUE;
}

bool TrayIcon::showBalloon(
    const std::wstring& title,
    const std::wstring& message,
    DWORD infoFlags) {
    if (!m_created) {
        return false;
    }

    m_data.uFlags = NIF_INFO;
    copyText(m_data.szInfoTitle, title);
    copyText(m_data.szInfo, message);
    m_data.dwInfoFlags = infoFlags;
    return Shell_NotifyIconW(NIM_MODIFY, &m_data) == TRUE;
}

} // namespace fastrecord::platform
