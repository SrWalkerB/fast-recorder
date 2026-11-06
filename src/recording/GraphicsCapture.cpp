#include "recording/GraphicsCapture.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <algorithm>
#include <cstring>

namespace fastrecord::recording {
using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;

GraphicsCapture::GraphicsCapture(const RecordingSettings& settings, CaptureOutputFormat format)
    : m_width(settings.width), m_height(settings.height) {
    if (!GraphicsCaptureSession::IsSupported()) {
        throw hresult_error(E_NOTIMPL, L"Windows Graphics Capture não está disponível neste Windows.");
    }
    check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, m_device.put(), nullptr, m_context.put()));
    const auto dxgi = m_device.as<IDXGIDevice>();
    com_ptr<IInspectable> inspectable;
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    m_runtimeDevice = inspectable.as<Direct3D11::IDirect3DDevice>();

    const auto factory = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    const HMONITOR monitor = settings.target.monitor ? settings.target.monitor
        : MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY);
    m_monitor = monitor;
    m_monitorSource = settings.target.kind != CaptureTargetKind::SelectedWindow;
    if (settings.target.kind == CaptureTargetKind::SelectedWindow) {
        if (settings.target.window == nullptr || !IsWindow(settings.target.window) ||
            !IsWindowVisible(settings.target.window)) {
            throw hresult_error(E_INVALIDARG, L"A janela selecionada não está mais disponível.");
        }
        check_hresult(factory->CreateForWindow(
            settings.target.window, guid_of<GraphicsCaptureItem>(), put_abi(m_item)));
    } else {
        check_hresult(factory->CreateForMonitor(monitor, guid_of<GraphicsCaptureItem>(), put_abi(m_item)));
    }
    m_size = m_item.Size();
    if (m_size.Width <= 0 || m_size.Height <= 0) {
        throw hresult_error(E_INVALIDARG, L"O monitor não possui uma área válida de captura.");
    }

    m_videoDevice = m_device.as<ID3D11VideoDevice>();
    m_videoContext = m_context.as<ID3D11VideoContext>();
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = m_size.Width;
    content.InputHeight = m_size.Height;
    content.OutputWidth = m_width;
    content.OutputHeight = m_height;
    content.InputFrameRate = content.OutputFrameRate = {settings.framesPerSecond, 1};
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    check_hresult(m_videoDevice->CreateVideoProcessorEnumerator(&content, m_enumerator.put()));
    check_hresult(m_videoDevice->CreateVideoProcessor(m_enumerator.get(), 0, m_processor.put()));

    const bool nv12 = format == CaptureOutputFormat::Nv12;
    if (nv12) {
        UINT support = 0;
        if (FAILED(m_enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support)) ||
            (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
            throw hresult_error(E_NOTIMPL, L"O processador de vídeo da GPU não gera frames NV12.");
        }
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = m_width;
    desc.Height = m_height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = nv12 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    check_hresult(m_device->CreateTexture2D(&desc, nullptr, m_output.put()));
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view{};
    view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    check_hresult(m_videoDevice->CreateVideoProcessorOutputView(
        m_output.get(), m_enumerator.get(), &view, m_outputView.put()));
    if (!nv12) {
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.BindFlags = 0;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check_hresult(m_device->CreateTexture2D(&stagingDesc, nullptr, m_staging.put()));
    } else {
        // The desktop is full-range RGB. Players assume BT.709 for HD video
        // that does not say otherwise, so produce exactly that.
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE input{};
        input.RGB_Range = 0; // 0-255
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE output{};
        output.YCbCr_Matrix = 1; // BT.709
        output.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        m_videoContext->VideoProcessorSetStreamColorSpace(m_processor.get(), 0, &input);
        m_videoContext->VideoProcessorSetOutputColorSpace(m_processor.get(), &output);
    }

    // Fit the entire monitor, preserving aspect ratio; uncovered pixels are black.
    const double scale = std::min(static_cast<double>(m_width) / m_size.Width,
        static_cast<double>(m_height) / m_size.Height);
    const LONG fitWidth = static_cast<LONG>(m_size.Width * scale);
    const LONG fitHeight = static_cast<LONG>(m_size.Height * scale);
    const LONG left = (static_cast<LONG>(m_width) - fitWidth) / 2;
    const LONG top = (static_cast<LONG>(m_height) - fitHeight) / 2;
    RECT source{0, 0, m_size.Width, m_size.Height};
    if (settings.target.kind == CaptureTargetKind::SelectedRegion) {
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!GetMonitorInfoW(m_monitor, &monitorInfo)) {
            throw hresult_error(E_INVALIDARG, L"Não foi possível identificar o monitor da região.");
        }
        source.left = std::max<LONG>(0, settings.captureRegion.left - monitorInfo.rcMonitor.left);
        source.top = std::max<LONG>(0, settings.captureRegion.top - monitorInfo.rcMonitor.top);
        source.right = std::min<LONG>(m_size.Width, settings.captureRegion.right - monitorInfo.rcMonitor.left);
        source.bottom = std::min<LONG>(m_size.Height, settings.captureRegion.bottom - monitorInfo.rcMonitor.top);
        if (source.right - source.left < 2 || source.bottom - source.top < 2) {
            throw hresult_error(E_INVALIDARG, L"A região selecionada é pequena demais.");
        }
    }
    const RECT destination{left, top, left + fitWidth, top + fitHeight};
    const RECT target{0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
    D3D11_VIDEO_COLOR black{};
    black.RGBA.A = 1.f;
    m_videoContext->VideoProcessorSetOutputBackgroundColor(m_processor.get(), FALSE, &black);
    m_videoContext->VideoProcessorSetOutputTargetRect(m_processor.get(), TRUE, &target);
    m_videoContext->VideoProcessorSetStreamSourceRect(m_processor.get(), 0, TRUE, &source);
    m_videoContext->VideoProcessorSetStreamDestRect(m_processor.get(), 0, TRUE, &destination);
    m_videoContext->VideoProcessorSetStreamFrameFormat(m_processor.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    m_videoContext->VideoProcessorSetStreamAutoProcessingMode(m_processor.get(), 0, FALSE);

    m_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        m_runtimeDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, m_size);
    m_session = m_pool.CreateCaptureSession(m_item);
    if (const auto options = m_session.try_as<IGraphicsCaptureSession2>()) {
        options.IsCursorCaptureEnabled(settings.captureCursor);
    }
    // Windows Graphics Capture adds a colored border to the captured display
    // by default. It is useful for interactive capture tools, but it becomes
    // part of the user's desktop while Fast Record is recording.
    if (const auto options = m_session.try_as<IGraphicsCaptureSession3>()) {
        options.IsBorderRequired(false);
    }
    m_session.StartCapture();
}

GraphicsCapture::~GraphicsCapture() {
    try { if (m_session) m_session.Close(); } catch (...) {}
    try { if (m_pool) m_pool.Close(); } catch (...) {}
}

bool GraphicsCapture::update() {
    check_hresult(m_device->GetDeviceRemovedReason());
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (m_monitorSource && (!GetMonitorInfoW(m_monitor, &info) ||
        info.rcMonitor.right - info.rcMonitor.left != m_size.Width ||
        info.rcMonitor.bottom - info.rcMonitor.top != m_size.Height)) {
        throw hresult_error(E_ABORT, L"O monitor foi desconectado ou teve sua resolução alterada.");
    }
    // Drain only the bounded pool. A static desktop can legitimately produce no frames.
    auto frame = m_pool.TryGetNextFrame();
    if (!frame) return false;
    if (auto newer = m_pool.TryGetNextFrame()) {
        frame.Close();
        frame = std::move(newer);
    }
    const auto size = frame.ContentSize();
    if (size.Width != m_size.Width || size.Height != m_size.Height) {
        throw hresult_error(E_ABORT, L"A resolução do monitor mudou. Inicie uma nova gravação.");
    }
    const auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> texture;
    check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc{};
    inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    com_ptr<ID3D11VideoProcessorInputView> input;
    check_hresult(m_videoDevice->CreateVideoProcessorInputView(
        texture.get(), m_enumerator.get(), &inputDesc, input.put()));
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input.get();
    check_hresult(m_videoContext->VideoProcessorBlt(m_processor.get(), m_outputView.get(), 0, 1, &stream));
    frame.Close();
    return true;
}

bool GraphicsCapture::read(std::vector<BYTE>& pixels) {
    if (!m_staging) {
        throw hresult_error(E_NOT_VALID_STATE, L"Esta captura entrega frames NV12 somente na GPU.");
    }
    if (!update()) {
        return false;
    }
    m_context->CopyResource(m_staging.get(), m_output.get());
    pixels.resize(static_cast<size_t>(m_width) * m_height * 4);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check_hresult(m_context->Map(m_staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    for (UINT y = 0; y < m_height; ++y) {
        std::memcpy(pixels.data() + static_cast<size_t>(y) * m_width * 4,
            static_cast<const BYTE*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
            static_cast<size_t>(m_width) * 4);
    }
    m_context->Unmap(m_staging.get(), 0);
    return true;
}
} // namespace fastrecord::recording
