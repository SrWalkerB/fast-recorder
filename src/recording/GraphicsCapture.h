#pragma once

#include "recording/RecordingBackend.h"
#include <d3d11.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <vector>

namespace fastrecord::recording {

enum class CaptureOutputFormat {
    // 32-bit BGRA, readable on the CPU through read().
    Bgra,
    // NV12 in BT.709 limited range, converted by the GPU video processor.
    // GPU only: read() is not available in this mode.
    Nv12,
};

// Owned exclusively by the recording worker, including the immediate D3D context.
class GraphicsCapture final {
public:
    explicit GraphicsCapture(
        const RecordingSettings& settings,
        CaptureOutputFormat format = CaptureOutputFormat::Bgra);
    ~GraphicsCapture();
    GraphicsCapture(const GraphicsCapture&) = delete;
    GraphicsCapture& operator=(const GraphicsCapture&) = delete;
    // Scales the newest frame into texture(). Returns false when Windows has
    // not delivered a new frame; texture() then still holds the previous one.
    bool update();
    bool read(std::vector<BYTE>& pixels);
    ID3D11Device* device() const noexcept { return m_device.get(); }
    ID3D11Texture2D* texture() const noexcept { return m_output.get(); }

private:
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_pool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{nullptr};
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_runtimeDevice{nullptr};
    winrt::com_ptr<ID3D11Device> m_device;
    winrt::com_ptr<ID3D11DeviceContext> m_context;
    winrt::com_ptr<ID3D11VideoDevice> m_videoDevice;
    winrt::com_ptr<ID3D11VideoContext> m_videoContext;
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> m_enumerator;
    winrt::com_ptr<ID3D11VideoProcessor> m_processor;
    winrt::com_ptr<ID3D11Texture2D> m_output;
    winrt::com_ptr<ID3D11Texture2D> m_staging;
    winrt::com_ptr<ID3D11VideoProcessorOutputView> m_outputView;
    winrt::Windows::Graphics::SizeInt32 m_size{};
    UINT m_width{}, m_height{};
    HMONITOR m_monitor{};
    bool m_monitorSource{true};
};

} // namespace fastrecord::recording
