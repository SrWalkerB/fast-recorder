#include "recording/AmfBackend.h"

#include "recording/AmfProbe.h"
#include "recording/AudioCapture.h"
#include "recording/GraphicsCapture.h"
#include "recording/Mp4Muxer.h"

#include <Windows.h>
#include <d3d11_4.h>
#include <mfapi.h>
#include <roapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef FASTRECORD_HAS_AMF
#define FASTRECORD_HAS_AMF 0
#endif

#if FASTRECORD_HAS_AMF
#include <core/Factory.h>
#include <components/VideoEncoderAV1.h>
#include <components/VideoEncoderVCE.h>
#endif

namespace fastrecord::recording {

namespace {

constexpr LONGLONG kHundredNanosecondsPerSecond = 10'000'000;

struct MediaRuntime {
    MediaRuntime() {
        winrt::check_hresult(RoInitialize(RO_INIT_MULTITHREADED));
        const HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(result)) {
            RoUninitialize();
            winrt::check_hresult(result);
        }
    }

    ~MediaRuntime() {
        winrt::clear_factory_cache();
        MFShutdown();
        RoUninitialize();
    }
};

std::filesystem::path createOutputPath(const std::filesystem::path& directory) {
    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t name[96]{};
    swprintf_s(
        name,
        _countof(name),
        L"Fast Record %04u-%02u-%02u %02u-%02u-%02u-%03u.mp4",
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds);
    return directory / name;
}

const wchar_t* codecName(VideoCodec codec) {
    return codec == VideoCodec::Av1 ? L"AV1" : L"H.264";
}

#if FASTRECORD_HAS_AMF

class AmfLibrary final {
public:
    AmfLibrary() {
#if defined(_WIN64)
        m_handle = LoadLibraryW(L"amfrt64.dll");
        if (m_handle == nullptr) m_handle = LoadLibraryW(L"amfrtlt64.dll");
#else
        m_handle = LoadLibraryW(L"amfrt32.dll");
        if (m_handle == nullptr) m_handle = LoadLibraryW(L"amfrtlt32.dll");
#endif
    }

    ~AmfLibrary() {
        if (m_handle != nullptr) FreeLibrary(m_handle);
    }

    AmfLibrary(const AmfLibrary&) = delete;
    AmfLibrary& operator=(const AmfLibrary&) = delete;

    HMODULE handle() const noexcept { return m_handle; }

private:
    HMODULE m_handle{nullptr};
};

std::wstring amfResultMessage(const std::wstring& operation, AMF_RESULT result) {
    std::wstring message = operation;
    message += L" (código AMF ";
    message += std::to_wstring(static_cast<int>(result));
    message += L").";
    return message;
}

// Makes QueryOutput wait for the packet instead of returning AMF_REPEAT at
// once. Polling with sleep_for(1 ms) costs a whole scheduler tick (~15.6 ms)
// per frame and capped 120 FPS recordings at about 64 FPS.
constexpr amf_int64 kQueryTimeoutMilliseconds = 50;

struct AmfVideoPacket {
    std::vector<std::uint8_t> bytes;
    LONGLONG timestamp{0};
    // Reported by the encoder for AV1. H.264 key frames are detected by the
    // MP4 writer from the IDR NAL units, so the hint stays empty there.
    std::optional<bool> keyFrame;
};

class AmfEncoder final {
public:
    ~AmfEncoder() {
        m_captureSurface = nullptr;
        if (m_encoder != nullptr) {
            m_encoder->Terminate();
            m_encoder = nullptr;
        }
        if (m_context != nullptr) {
            m_context->Terminate();
            m_context = nullptr;
        }
        m_factory = nullptr;
        m_library.reset();
    }

    bool initialize(
        ID3D11Device* device,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t fps,
        std::uint32_t bitrate,
        VideoCodec codec,
        std::wstring& error) {
        m_width = width;
        m_height = height;
        m_codec = codec;
        m_frameDuration = kHundredNanosecondsPerSecond / fps;
        m_keyFrameInterval = (std::max)(fps * 2, 1u);
        m_framesSubmitted = 0;

        m_library = std::make_unique<AmfLibrary>();
        if (m_library->handle() == nullptr) {
            error = L"AMF não disponível: amfrt64.dll não foi encontrada no driver AMD.";
            return false;
        }

        const auto init = reinterpret_cast<AMFInit_Fn>(
            GetProcAddress(m_library->handle(), AMF_INIT_FUNCTION_NAME));
        if (init == nullptr) {
            error = L"AMF não disponível: a DLL não exporta AMFInit.";
            return false;
        }

        if (init(AMF_FULL_VERSION, &m_factory) != AMF_OK || m_factory == nullptr) {
            error = L"AMF não disponível: AMFInit recusou a versão dos bindings.";
            return false;
        }
        if (m_factory->CreateContext(&m_context) != AMF_OK || m_context == nullptr) {
            error = L"AMF não disponível: não foi possível criar o contexto.";
            return false;
        }
        if (device == nullptr || m_context->InitDX11(device) != AMF_OK) {
            error = L"AMF não disponível: o contexto não aceitou o dispositivo Direct3D 11.";
            return false;
        }
        // AMF can touch the immediate context from its own threads; the AMD
        // samples always enable this before sharing a D3D11 device with it.
        Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&multithread)))) {
            multithread->SetMultithreadProtected(TRUE);
        }

        const wchar_t* component = codec == VideoCodec::Av1 ? AMFVideoEncoder_AV1 : AMFVideoEncoderVCE_AVC;
        if (m_factory->CreateComponent(m_context, component, &m_encoder) != AMF_OK ||
            m_encoder == nullptr) {
            error = std::wstring(L"AMF não disponível: o encoder AMD ") + codecName(codec) +
                L" não foi encontrado nesta GPU/driver.";
            return false;
        }

        const bool configured = codec == VideoCodec::Av1
            ? configureAv1(fps, bitrate, error)
            : configureH264(fps, bitrate, error);
        if (!configured) return false;

        const AMF_RESULT initResult = m_encoder->Init(amf::AMF_SURFACE_NV12, width, height);
        if (initResult != AMF_OK) {
            // Some drivers accept the alignment mode and only refuse the size
            // here, so the 64x16 hint is repeated for that case.
            error = codec == VideoCodec::Av1 && ((width % 64) != 0 || (height % 16) != 0)
                ? amfResultMessage(
                      L"Esta GPU AMD não aceitou AV1 nesta resolução; ela pode exigir múltiplos de 64×16. "
                      L"Escolha 720p, 1440p ou 4K, ou use H.264",
                      initResult)
                : amfResultMessage(
                      std::wstring(L"Não foi possível inicializar o encoder ") + codecName(codec) + L" AMF",
                      initResult);
            return false;
        }

        // H.264 drivers that do not repeat SPS/PPS in the first access unit
        // still expose them here; for AV1 this holds the sequence header OBU.
        amf::AMFVariant extraData;
        const wchar_t* extraDataProperty = codec == VideoCodec::Av1
            ? AMF_VIDEO_ENCODER_AV1_EXTRA_DATA
            : AMF_VIDEO_ENCODER_EXTRADATA;
        if (m_encoder->GetProperty(extraDataProperty, &extraData) == AMF_OK &&
            extraData.type == amf::AMF_VARIANT_INTERFACE && extraData.pInterface != nullptr) {
            amf::AMFBufferPtr buffer(extraData.pInterface);
            if (buffer != nullptr && buffer->GetNative() != nullptr && buffer->GetSize() > 0) {
                const auto* bytes = static_cast<const std::uint8_t*>(buffer->GetNative());
                m_extraData.assign(bytes, bytes + buffer->GetSize());
            }
        }
        return true;
    }

    const std::vector<std::uint8_t>& extraData() const noexcept {
        return m_extraData;
    }

    // nv12: the capture's output texture, on the device passed to initialize().
    // Its contents change on every GraphicsCapture::update(), so the encoder
    // receives a GPU copy owned by AMF instead of the texture itself.
    bool encode(
        ID3D11Texture2D* nv12,
        LONGLONG timestamp,
        std::vector<AmfVideoPacket>& packets,
        std::wstring& error) {
        packets.clear();
        if (m_encoder == nullptr || m_context == nullptr || nv12 == nullptr) {
            error = L"O encoder AMF não está pronto para receber frames.";
            return false;
        }

        AMF_RESULT result = AMF_OK;
        if (m_captureSurface == nullptr || m_captureTexture != nv12) {
            m_captureSurface = nullptr;
            result = m_context->CreateSurfaceFromDX11Native(nv12, &m_captureSurface, nullptr);
            if (result != AMF_OK || m_captureSurface == nullptr) {
                error = amfResultMessage(L"AMF não aceitou a textura NV12 da captura", result);
                return false;
            }
            m_captureTexture = nv12;
        }
        amf::AMFDataPtr copy;
        result = m_captureSurface->Duplicate(amf::AMF_MEMORY_DX11, &copy);
        amf::AMFSurfacePtr surface(copy);
        if (result != AMF_OK || surface == nullptr) {
            error = amfResultMessage(L"AMF não conseguiu copiar o frame na GPU", result);
            return false;
        }
        surface->SetPts(timestamp);
        surface->SetDuration(m_frameDuration);
        // The H.264 low-latency preset ignores AMF_VIDEO_ENCODER_IDR_PERIOD on
        // current drivers (an RX 9070 XT produced a single IDR in 12 s), which
        // leaves the MP4 with one sync sample and makes seeking decode from
        // the start. Request the IDR explicitly instead.
        if (m_codec == VideoCodec::H264 && m_framesSubmitted % m_keyFrameInterval == 0) {
            surface->SetProperty(
                AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                static_cast<amf_int64>(AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR));
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
        }

        for (;;) {
            result = m_encoder->SubmitInput(surface);
            if (result != AMF_INPUT_FULL) break;
            if (!collectOutput(packets, false, error)) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (result != AMF_OK && result != AMF_NEED_MORE_INPUT) {
            error = amfResultMessage(L"AMF não conseguiu enviar o frame", result);
            return false;
        }
        ++m_framesSubmitted;

        // Low-latency mode normally returns one packet immediately. The small
        // wait also handles drivers that schedule the first GPU submission.
        return collectOutput(packets, true, error);
    }

    bool drain(std::vector<AmfVideoPacket>& packets, std::wstring& error) {
        packets.clear();
        if (m_encoder == nullptr) return true;

        AMF_RESULT result = AMF_INPUT_FULL;
        while (result == AMF_INPUT_FULL) {
            result = m_encoder->Drain();
            if (result == AMF_INPUT_FULL) {
                if (!collectOutput(packets, false, error)) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (result != AMF_OK && result != AMF_EOF) {
            error = amfResultMessage(L"Não foi possível finalizar o encoder AMF", result);
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (;;) {
            const size_t before = packets.size();
            if (!collectOutput(packets, false, error)) return false;
            if (std::chrono::steady_clock::now() >= deadline || packets.size() == before) break;
        }
        return true;
    }

private:
    template <typename Value>
    bool set(const wchar_t* property, const Value& value) {
        return m_encoder->SetProperty(property, value) == AMF_OK;
    }

    bool configureH264(std::uint32_t fps, std::uint32_t bitrate, std::wstring& error) {
        if (!set(AMF_VIDEO_ENCODER_USAGE, static_cast<amf_int64>(AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY)) ||
            !set(AMF_VIDEO_ENCODER_PROFILE, static_cast<amf_int64>(AMF_VIDEO_ENCODER_PROFILE_HIGH)) ||
            !set(AMF_VIDEO_ENCODER_FRAMERATE, AMFConstructRate(fps, 1)) ||
            !set(AMF_VIDEO_ENCODER_TARGET_BITRATE, static_cast<amf_int64>(bitrate)) ||
            !set(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
                static_cast<amf_int64>(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR)) ||
            !set(AMF_VIDEO_ENCODER_B_PIC_PATTERN, static_cast<amf_int64>(0)) ||
            !set(AMF_VIDEO_ENCODER_IDR_PERIOD, static_cast<amf_int64>(fps * 2)) ||
            !set(AMF_VIDEO_ENCODER_FRAMESIZE,
                AMFConstructSize(static_cast<amf_int32>(m_width), static_cast<amf_int32>(m_height)))) {
            error = L"AMF não aceitou a configuração de H.264 da gravação.";
            return false;
        }
        set(AMF_VIDEO_ENCODER_QUERY_TIMEOUT, kQueryTimeoutMilliseconds);
        // Describe the BT.709 limited-range frames GraphicsCapture produces in
        // the VUI. Optional: older drivers may not expose these properties.
        set(AMF_VIDEO_ENCODER_INPUT_COLOR_PROFILE, static_cast<amf_int64>(AMF_VIDEO_CONVERTER_COLOR_PROFILE_709));
        set(AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE, static_cast<amf_int64>(AMF_VIDEO_CONVERTER_COLOR_PROFILE_709));
        set(AMF_VIDEO_ENCODER_OUTPUT_TRANSFER_CHARACTERISTIC,
            static_cast<amf_int64>(AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709));
        set(AMF_VIDEO_ENCODER_OUTPUT_COLOR_PRIMARIES, static_cast<amf_int64>(AMF_COLOR_PRIMARIES_BT709));
        return true;
    }

    bool configureAv1(std::uint32_t fps, std::uint32_t bitrate, std::wstring& error) {
        // USAGE resets every other property to its preset, so it goes first.
        if (!set(AMF_VIDEO_ENCODER_AV1_USAGE, static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_USAGE_LOW_LATENCY))) {
            error = L"AMF não aceitou o modo de baixa latência do AV1.";
            return false;
        }

        // RDNA 3 encodes AV1 in 64x16 blocks and turns 1080p into 1082 lines.
        // Newer encoders accept any size; ask for that and only fall back to
        // the block restriction when the requested size already satisfies it.
        const bool unrestricted = set(
            AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE,
            static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE_NO_RESTRICTIONS));
        if (!unrestricted && ((m_width % 64) != 0 || (m_height % 16) != 0)) {
            error = L"Esta GPU AMD só grava AV1 em resoluções múltiplas de 64×16. "
                    L"Escolha 720p, 1440p ou 4K, ou use H.264.";
            return false;
        }

        if (!set(AMF_VIDEO_ENCODER_AV1_FRAMESIZE,
                AMFConstructSize(static_cast<amf_int32>(m_width), static_cast<amf_int32>(m_height))) ||
            !set(AMF_VIDEO_ENCODER_AV1_FRAMERATE, AMFConstructRate(fps, 1)) ||
            !set(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD,
                static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR)) ||
            !set(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, static_cast<amf_int64>(bitrate)) ||
            !set(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, static_cast<amf_int64>(bitrate)) ||
            !set(AMF_VIDEO_ENCODER_AV1_GOP_SIZE, static_cast<amf_int64>(fps * 2)) ||
            !set(AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE,
                static_cast<amf_int64>(AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE_KEY_FRAME_ALIGNED))) {
            error = L"AMF não aceitou a configuração de AV1 da gravação.";
            return false;
        }
        // Without filler data a still desktop costs almost nothing, which is
        // the point of offering AV1. Older drivers may not expose the option.
        set(AMF_VIDEO_ENCODER_AV1_FILLER_DATA, false);
        set(AMF_VIDEO_ENCODER_AV1_QUERY_TIMEOUT, kQueryTimeoutMilliseconds);
        // Same color description as H.264, written to the sequence header.
        set(AMF_VIDEO_ENCODER_AV1_INPUT_COLOR_PROFILE, static_cast<amf_int64>(AMF_VIDEO_CONVERTER_COLOR_PROFILE_709));
        set(AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PROFILE, static_cast<amf_int64>(AMF_VIDEO_CONVERTER_COLOR_PROFILE_709));
        set(AMF_VIDEO_ENCODER_AV1_OUTPUT_TRANSFER_CHARACTERISTIC,
            static_cast<amf_int64>(AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709));
        set(AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PRIMARIES, static_cast<amf_int64>(AMF_COLOR_PRIMARIES_BT709));
        return true;
    }

    static bool keyFrameOf(amf::AMFBuffer* buffer) {
        amf::AMFVariant frameType;
        return buffer->GetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE, &frameType) == AMF_OK &&
            frameType.type == amf::AMF_VARIANT_INT64 &&
            frameType.int64Value == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY;
    }

    bool collectOutput(
        std::vector<AmfVideoPacket>& packets,
        bool waitForOne,
        std::wstring& error) {
        const auto deadline = std::chrono::steady_clock::now() +
            (waitForOne ? std::chrono::seconds(2) : std::chrono::milliseconds(2));
        bool received = false;
        for (;;) {
            amf::AMFData* data = nullptr;
            const AMF_RESULT result = m_encoder->QueryOutput(&data);
            if (result == AMF_OK && data != nullptr) {
                amf::AMFBufferPtr buffer(data);
                if (buffer == nullptr || buffer->GetNative() == nullptr || buffer->GetSize() == 0) {
                    error = std::wstring(L"O encoder AMF retornou um pacote ") + codecName(m_codec) + L" vazio.";
                    return false;
                }
                const auto* bytes = static_cast<const std::uint8_t*>(buffer->GetNative());
                AmfVideoPacket packet;
                packet.timestamp = static_cast<LONGLONG>(buffer->GetPts());
                if (m_codec == VideoCodec::Av1) {
                    const std::vector<std::uint8_t> encoded(bytes, bytes + buffer->GetSize());
                    if (!removeAv1TemporalDelimiters(encoded, packet.bytes)) {
                        error = L"O encoder AMF retornou um pacote AV1 inválido.";
                        return false;
                    }
                    packet.keyFrame = keyFrameOf(buffer);
                } else {
                    packet.bytes.assign(bytes, bytes + buffer->GetSize());
                }
                packets.push_back(std::move(packet));
                received = true;
                if (waitForOne) return true;
                continue;
            }
            if (result == AMF_EOF || result == AMF_REPEAT || result == AMF_NEED_MORE_INPUT) {
                if (!waitForOne || received || std::chrono::steady_clock::now() >= deadline) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            error = amfResultMessage(
                std::wstring(L"Não foi possível obter o pacote ") + codecName(m_codec) + L" do AMF",
                result);
            return false;
        }
    }

    std::unique_ptr<AmfLibrary> m_library;
    amf::AMFFactory* m_factory{nullptr};
    amf::AMFContextPtr m_context;
    amf::AMFComponentPtr m_encoder;
    amf::AMFSurfacePtr m_captureSurface;
    ID3D11Texture2D* m_captureTexture{nullptr};
    std::vector<std::uint8_t> m_extraData;
    std::uint32_t m_width{0};
    std::uint32_t m_height{0};
    VideoCodec m_codec{VideoCodec::H264};
    LONGLONG m_frameDuration{0};
    std::uint32_t m_keyFrameInterval{1};
    std::uint64_t m_framesSubmitted{0};
};

// Picks the data the MP4 writer needs to build avcC/av1C: the first packets
// normally carry it, the encoder's extra data covers drivers that do not.
bool findSequenceHeader(
    VideoCodec codec,
    const std::vector<AmfVideoPacket>& firstPackets,
    const std::vector<std::uint8_t>& extraData,
    std::vector<std::uint8_t>& sequenceHeader) {
    std::vector<std::uint8_t> scratch;
    const auto accepts = [&](const std::vector<std::uint8_t>& candidate) {
        if (candidate.empty()) return false;
        if (codec == VideoCodec::Av1) {
            if (!extractAv1CodecConfiguration(candidate, scratch)) return false;
            sequenceHeader = candidate;
            return true;
        }
        return extractH264SequenceHeader(candidate, sequenceHeader);
    };
    for (const auto& packet : firstPackets) {
        if (accepts(packet.bytes)) return true;
    }
    return accepts(extraData);
}

#endif // FASTRECORD_HAS_AMF

} // namespace

AmfBackend::~AmfBackend() {
    std::wstring ignored;
    stop(ignored);
}

bool AmfBackend::start(const RecordingSettings& settings, std::wstring& error) {
#if !FASTRECORD_HAS_AMF
    (void)settings;
    error = L"Esta build não inclui os headers do AMF. Reconfigure com FASTRECORD_FETCH_AMF_HEADERS=ON.";
    return false;
#else
    if (m_worker.joinable() || m_running.load()) {
        error = L"Já existe uma captura em andamento.";
        return false;
    }
    if (settings.codec != VideoCodec::H264 && settings.codec != VideoCodec::Av1) {
        error = L"O codec selecionado não é suportado pelo AMF.";
        return false;
    }
    const auto capability = probeAmf();
    const bool codecSupported = settings.codec == VideoCodec::Av1
        ? capability.av1Supported
        : capability.h264Supported;
    if (!codecSupported) {
        if (settings.codec == VideoCodec::Av1 && capability.h264Supported) {
            error = L"O encoder AMD desta GPU/driver não oferece AV1; use H.264.";
        } else {
            error = capability.description.empty()
                ? L"AMF não está disponível nesta máquina."
                : capability.description;
        }
        return false;
    }
    if (settings.outputDirectory.empty() || settings.width < 2 || settings.height < 2 ||
        settings.width > 3840 || settings.height > 2160 ||
        (settings.width % 2) != 0 || (settings.height % 2) != 0 ||
        settings.framesPerSecond == 0 || settings.framesPerSecond > 120 ||
        settings.bitrateMbps < 4 || settings.bitrateMbps > 80) {
        error = L"As configurações da gravação são inválidas.";
        return false;
    }

    std::error_code directoryError;
    std::filesystem::create_directories(settings.outputDirectory, directoryError);
    if (directoryError) {
        error = L"Não foi possível criar a pasta de gravações.";
        return false;
    }

    {
        std::scoped_lock lock(m_mutex);
        m_outputPath = createOutputPath(settings.outputDirectory);
        m_workerError.clear();
    }
    m_stopRequested = false;
    m_paused = false;

    std::promise<std::wstring> startupResult;
    auto startupFuture = startupResult.get_future();
    try {
        m_worker = std::thread([this, settings, promise = std::move(startupResult)]() mutable {
            try {
                recordLoop(settings, std::move(promise));
            } catch (const winrt::hresult_error& failure) {
                setWorkerError(failure.message().c_str());
            } catch (...) {
                setWorkerError(L"Falha inesperada no backend AMF.");
            }
            m_running = false;
        });
        error = startupFuture.get();
    } catch (...) {
        if (m_worker.joinable()) m_worker.join();
        std::scoped_lock lock(m_mutex);
        error = m_workerError.empty() ? L"Não foi possível iniciar o AMF." : m_workerError;
        return false;
    }
    if (!error.empty()) {
        if (m_worker.joinable()) m_worker.join();
        return false;
    }
    return true;
#endif
}

bool AmfBackend::pause(std::wstring& error) {
    if (!m_running.load() || m_paused.load()) {
        std::scoped_lock lock(m_mutex);
        error = m_workerError.empty() ? L"A gravação não está ativa." : m_workerError;
        return false;
    }
    m_paused = true;
    return true;
}

bool AmfBackend::resume(std::wstring& error) {
    if (!m_running.load() || !m_paused.load()) {
        std::scoped_lock lock(m_mutex);
        error = m_workerError.empty() ? L"A gravação não está pausada." : m_workerError;
        return false;
    }
    m_paused = false;
    return true;
}

bool AmfBackend::stop(std::wstring& error) {
    if (!m_worker.joinable()) {
        error = L"Não há uma captura ativa.";
        return false;
    }

    m_stopRequested = true;
    m_paused = false;
    m_worker.join();

    std::scoped_lock lock(m_mutex);
    error = m_workerError;
    return error.empty();
}

std::filesystem::path AmfBackend::outputPath() const {
    std::scoped_lock lock(m_mutex);
    return m_outputPath;
}

void AmfBackend::setWorkerError(std::wstring error) {
    std::scoped_lock lock(m_mutex);
    if (m_workerError.empty()) m_workerError = std::move(error);
}

void AmfBackend::recordLoop(
    RecordingSettings settings,
    std::promise<std::wstring> startupResult) {
    bool startupReported = false;
    auto reportStartup = [&](std::wstring message) {
        if (!startupReported) {
            startupResult.set_value(std::move(message));
            startupReported = true;
        }
    };

#if FASTRECORD_HAS_AMF
    const auto fail = [&](const std::wstring& message) {
        reportStartup(message);
        setWorkerError(message);
    };

    try {
        MediaRuntime runtime;
        const auto width = settings.width;
        const auto height = settings.height;
        const auto fps = settings.framesPerSecond;
        const auto bitrate = settings.bitrateMbps * 1'000'000u;
        const auto codec = settings.codec;
        const LONGLONG frameDuration = kHundredNanosecondsPerSecond / fps;

        // Frames stay on the GPU: the video processor scales and converts to
        // NV12, AMF copies the texture and encodes it.
        auto capture = std::make_unique<GraphicsCapture>(settings, CaptureOutputFormat::Nv12);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!capture->update()) {
            if (m_stopRequested || std::chrono::steady_clock::now() >= deadline) {
                throw winrt::hresult_error(E_ABORT, L"O Windows não entregou frames do monitor.");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        AmfEncoder encoder;
        std::wstring loopError;
        if (!encoder.initialize(capture->device(), width, height, fps, bitrate, codec, loopError)) {
            fail(loopError);
            return;
        }

        // Every encoded frame advances the timeline by exactly one frame, the
        // same rule the MP4 writer uses for its constant frame rate track.
        LONGLONG timestamp = 0;
        std::vector<AmfVideoPacket> firstPackets;
        for (std::uint32_t attempt = 0; attempt < 8 && firstPackets.empty(); ++attempt) {
            if (attempt > 0) capture->update();
            if (!encoder.encode(capture->texture(), timestamp, firstPackets, loopError)) {
                fail(loopError);
                return;
            }
            timestamp += frameDuration;
        }
        if (firstPackets.empty()) {
            fail(std::wstring(L"O AMF não produziu o primeiro frame ") + codecName(codec) + L".");
            return;
        }

        std::vector<std::uint8_t> sequenceHeader;
        if (!findSequenceHeader(codec, firstPackets, encoder.extraData(), sequenceHeader)) {
            fail(codec == VideoCodec::Av1
                ? L"O AMF não retornou o cabeçalho de sequência AV1."
                : L"O AMF não retornou SPS/PPS para montar o arquivo H.264.");
            return;
        }

        std::unique_ptr<AudioCapture> audioCapture;
        std::unique_ptr<AacEncoder> audioEncoder;
        if (settings.captureMicrophone || settings.captureSystemAudio) {
            audioCapture = std::make_unique<AudioCapture>();
            audioEncoder = std::make_unique<AacEncoder>();
            if (!audioEncoder->initialize(loopError) ||
                !audioCapture->start(settings.captureMicrophone, settings.captureSystemAudio, loopError)) {
                fail(loopError);
                return;
            }
        }

        Mp4Writer writer;
        if (!writer.start(
                outputPath(),
                width,
                height,
                fps,
                bitrate,
                codec,
                sequenceHeader,
                audioCapture != nullptr,
                loopError)) {
            fail(loopError);
            return;
        }

        const auto writeVideoPackets = [&](const std::vector<AmfVideoPacket>& packets) {
            for (const auto& packet : packets) {
                if (!writer.write(packet.bytes, packet.timestamp, loopError, packet.keyFrame)) return false;
            }
            return true;
        };
        if (!writeVideoPackets(firstPackets)) {
            fail(loopError);
            return;
        }

        m_running = true;
        reportStartup({});

        std::vector<std::uint8_t> audioPcm;
        std::vector<EncodedAudioPacket> audioPackets;
        const auto processAudio = [&](bool writeSamples) {
            if (!audioCapture || !audioEncoder) return true;
            std::uint32_t audioFrames = 0;
            if (!audioCapture->read(audioPcm, audioFrames, loopError)) return false;
            if (!writeSamples || audioFrames == 0) return true;
            audioPackets.clear();
            if (!audioEncoder->encode(audioPcm, audioFrames, audioPackets, loopError)) return false;
            for (const auto& audioPacket : audioPackets) {
                if (!writer.writeAudio(audioPacket, loopError)) return false;
            }
            return true;
        };

        auto nextFrame = std::chrono::steady_clock::now() + std::chrono::nanoseconds(frameDuration * 100);
        while (!m_stopRequested.load()) {
            if (m_paused.load()) {
                if (!processAudio(false)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                nextFrame = std::chrono::steady_clock::now();
                continue;
            }

            if (!processAudio(true)) break;

            const auto now = std::chrono::steady_clock::now();
            if (nextFrame > now) {
                std::this_thread::sleep_until(nextFrame);
            } else if (now - nextFrame > std::chrono::milliseconds(250)) {
                nextFrame = now;
            }

            // Windows only delivers frames when something changes. When
            // nothing new arrived, encode the previous image again so a still
            // desktop keeps its real duration in the video track.
            capture->update();

            std::vector<AmfVideoPacket> packets;
            if (!encoder.encode(capture->texture(), timestamp, packets, loopError)) break;
            if (!writeVideoPackets(packets)) break;
            timestamp += frameDuration;
            nextFrame += std::chrono::nanoseconds(frameDuration * 100);
        }

        if (loopError.empty() && !processAudio(true)) {
            // The audio error is reported below.
        }
        if (loopError.empty() && audioEncoder) {
            audioPackets.clear();
            if (audioEncoder->drain(audioPackets, loopError)) {
                for (const auto& audioPacket : audioPackets) {
                    if (!writer.writeAudio(audioPacket, loopError)) break;
                }
            }
        }

        std::vector<AmfVideoPacket> finalPackets;
        if (loopError.empty() && encoder.drain(finalPackets, loopError)) {
            writeVideoPackets(finalPackets);
        }
        std::wstring finalizeError;
        if (!writer.finalize(finalizeError) && loopError.empty()) {
            loopError = std::move(finalizeError);
        }
        if (!loopError.empty()) setWorkerError(std::move(loopError));
    } catch (const winrt::hresult_error& failure) {
        fail(failure.message().c_str());
    } catch (...) {
        fail(L"Falha inesperada no backend AMF.");
    }
#else
    (void)settings;
    reportStartup(L"Esta build não inclui suporte ao AMF.");
#endif
}

} // namespace fastrecord::recording
