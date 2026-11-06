#include "recording/NvencBackend.h"
#include "recording/AudioCapture.h"
#include "recording/GraphicsCapture.h"
#include "recording/Mp4Muxer.h"

#include <Windows.h>
#include <mfapi.h>
#include <roapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

#ifndef FASTRECORD_HAS_NVENC
#define FASTRECORD_HAS_NVENC 0
#endif

#if FASTRECORD_HAS_NVENC
#include <nvEncodeAPI.h>
#endif

namespace fastrecord::recording {

namespace {

using Microsoft::WRL::ComPtr;

constexpr LONGLONG kHundredNanosecondsPerSecond = 10'000'000;

std::filesystem::path createOutputPath(const std::filesystem::path& directory) {
    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t name[96]{};
    swprintf_s(
        name,
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

#if FASTRECORD_HAS_NVENC

class NvencLibrary final {
public:
    NvencLibrary() {
#if defined(_WIN64)
        handle = LoadLibraryW(L"nvEncodeAPI64.dll");
#else
        handle = LoadLibraryW(L"nvEncodeAPI.dll");
#endif
    }

    ~NvencLibrary() {
        if (handle != nullptr) {
            FreeLibrary(handle);
        }
    }

    NvencLibrary(const NvencLibrary&) = delete;
    NvencLibrary& operator=(const NvencLibrary&) = delete;

    HMODULE handle{nullptr};
};

using NvEncodeAPICreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

std::wstring nvencStatusMessage(
    const wchar_t* operation,
    NVENCSTATUS status,
    const NV_ENCODE_API_FUNCTION_LIST* functions,
    void* session) {
    std::wstring message = operation;
    message += L" (código NVENC ";
    message += std::to_wstring(static_cast<int>(status));
    message += L").";

    if (functions != nullptr && session != nullptr && functions->nvEncGetLastErrorString != nullptr) {
        if (const char* detail = functions->nvEncGetLastErrorString(session); detail != nullptr && *detail != '\0') {
            const int length = MultiByteToWideChar(CP_UTF8, 0, detail, -1, nullptr, 0);
            if (length > 1) {
                std::wstring converted(static_cast<size_t>(length - 1), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, detail, -1, converted.data(), length);
                message += L" ";
                message += converted;
            }
        }
    }
    return message;
}

class NvencEncoder final {
public:
    ~NvencEncoder() {
        shutdown();
    }

    bool initialize(
        ID3D11Device* device,
        ID3D11Texture2D* texture,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t fps,
        std::uint32_t bitrate,
        VideoCodec codec,
        std::wstring& error) {
        if (device == nullptr || texture == nullptr) {
            error = L"O dispositivo Direct3D 11 não está disponível para o NVENC.";
            return false;
        }

        m_library = std::make_unique<NvencLibrary>();
        if (m_library->handle == nullptr) {
            error = L"NVENC não disponível: nvEncodeAPI.dll não foi encontrada.";
            return false;
        }

        const auto createInstance = reinterpret_cast<NvEncodeAPICreateInstanceFn>(
            GetProcAddress(m_library->handle, "NvEncodeAPICreateInstance"));
        if (createInstance == nullptr) {
            error = L"NVENC não disponível: a API do driver não foi exportada.";
            return false;
        }

        m_functions = {};
        m_functions.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        NVENCSTATUS status = createInstance(&m_functions);
        if (status != NV_ENC_SUCCESS) {
            error = nvencStatusMessage(L"Não foi possível inicializar a API do NVENC", status, &m_functions, nullptr);
            return false;
        }

        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams{};
        openParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        openParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        openParams.device = device;
        openParams.apiVersion = NVENCAPI_VERSION;
        status = m_functions.nvEncOpenEncodeSessionEx(&openParams, &m_session);
        if (status != NV_ENC_SUCCESS || m_session == nullptr) {
            error = nvencStatusMessage(L"Não foi possível abrir uma sessão NVENC", status, &m_functions, m_session);
            shutdown();
            return false;
        }

        m_codec = codec;
        const GUID encodeGuid = codec == VideoCodec::Av1
            ? NV_ENC_CODEC_AV1_GUID
            : NV_ENC_CODEC_H264_GUID;
        const wchar_t* codecLabel = codec == VideoCodec::Av1 ? L"AV1" : L"H.264";

        GUID presetGuid = NV_ENC_PRESET_P3_GUID;
        NV_ENC_PRESET_CONFIG preset{};
        preset.version = NV_ENC_PRESET_CONFIG_VER;
        preset.presetCfg.version = NV_ENC_CONFIG_VER;
        if (m_functions.nvEncGetEncodePresetConfigEx != nullptr) {
            status = m_functions.nvEncGetEncodePresetConfigEx(
                m_session,
                encodeGuid,
                presetGuid,
                NV_ENC_TUNING_INFO_LOW_LATENCY,
                &preset);
        } else {
            status = NV_ENC_ERR_UNSUPPORTED_PARAM;
        }
        if (status != NV_ENC_SUCCESS) {
            status = m_functions.nvEncGetEncodePresetConfig(
                m_session, encodeGuid, presetGuid, &preset);
        }
        if (status != NV_ENC_SUCCESS) {
            // Some older driver branches expose the legacy presets only.
            presetGuid = NV_ENC_PRESET_P1_GUID;
            preset = {};
            preset.version = NV_ENC_PRESET_CONFIG_VER;
            preset.presetCfg.version = NV_ENC_CONFIG_VER;
            if (m_functions.nvEncGetEncodePresetConfigEx != nullptr) {
                status = m_functions.nvEncGetEncodePresetConfigEx(
                    m_session,
                    encodeGuid,
                    presetGuid,
                    NV_ENC_TUNING_INFO_LOW_LATENCY,
                    &preset);
            } else {
                status = NV_ENC_ERR_UNSUPPORTED_PARAM;
            }
            if (status != NV_ENC_SUCCESS) {
                status = m_functions.nvEncGetEncodePresetConfig(
                    m_session, encodeGuid, presetGuid, &preset);
            }
        }
        if (status != NV_ENC_SUCCESS) {
            std::wstring operation = L"Não foi possível consultar o preset ";
            operation += codecLabel;
            operation += L" do NVENC";
            error = nvencStatusMessage(operation.c_str(), status, &m_functions, m_session);
            shutdown();
            return false;
        }

        m_config = preset.presetCfg;
        m_config.version = NV_ENC_CONFIG_VER;
        m_config.profileGUID = codec == VideoCodec::Av1
            ? NV_ENC_AV1_PROFILE_MAIN_GUID
            : NV_ENC_H264_PROFILE_HIGH_GUID;
        m_config.gopLength = std::max<std::uint32_t>(fps * 2, 1);
        m_config.frameIntervalP = 1;
        m_config.rcParams.version = NV_ENC_RC_PARAMS_VER;
        m_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        m_config.rcParams.averageBitRate = bitrate;
        m_config.rcParams.maxBitRate = bitrate;
        m_config.rcParams.enableLookahead = 0;
        m_config.rcParams.lookaheadDepth = 0;
        m_config.rcParams.zeroReorderDelay = 1;
        if (codec == VideoCodec::H264) {
            m_config.encodeCodecConfig.h264Config.idrPeriod = m_config.gopLength;
            m_config.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
            m_config.encodeCodecConfig.h264Config.disableSPSPPS = 0;
            m_config.encodeCodecConfig.h264Config.chromaFormatIDC = 1;
            m_config.encodeCodecConfig.h264Config.inputBitDepth = NV_ENC_BIT_DEPTH_8;
            m_config.encodeCodecConfig.h264Config.outputBitDepth = NV_ENC_BIT_DEPTH_8;
        } else {
            m_config.encodeCodecConfig.av1Config.idrPeriod = m_config.gopLength;
            m_config.encodeCodecConfig.av1Config.outputAnnexBFormat = 0;
            m_config.encodeCodecConfig.av1Config.disableSeqHdr = 0;
            m_config.encodeCodecConfig.av1Config.repeatSeqHdr = 1;
            m_config.encodeCodecConfig.av1Config.chromaFormatIDC = 1;
            m_config.encodeCodecConfig.av1Config.inputBitDepth = NV_ENC_BIT_DEPTH_8;
            m_config.encodeCodecConfig.av1Config.outputBitDepth = NV_ENC_BIT_DEPTH_8;
        }

        NV_ENC_INITIALIZE_PARAMS initializeParams{};
        initializeParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
        initializeParams.encodeGUID = encodeGuid;
        initializeParams.presetGUID = presetGuid;
        initializeParams.encodeWidth = width;
        initializeParams.encodeHeight = height;
        initializeParams.darWidth = width;
        initializeParams.darHeight = height;
        initializeParams.frameRateNum = fps;
        initializeParams.frameRateDen = 1;
        initializeParams.enableEncodeAsync = 0;
        initializeParams.enablePTD = 1;
        initializeParams.maxEncodeWidth = width;
        initializeParams.maxEncodeHeight = height;
        initializeParams.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
        initializeParams.encodeConfig = &m_config;
        status = m_functions.nvEncInitializeEncoder(m_session, &initializeParams);
        if (status != NV_ENC_SUCCESS) {
            std::wstring operation = L"Não foi possível inicializar o encoder ";
            operation += codecLabel;
            operation += L" NVENC";
            error = nvencStatusMessage(operation.c_str(), status, &m_functions, m_session);
            shutdown();
            return false;
        }

        NV_ENC_CREATE_BITSTREAM_BUFFER bitstream{};
        bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        status = m_functions.nvEncCreateBitstreamBuffer(m_session, &bitstream);
        if (status != NV_ENC_SUCCESS || bitstream.bitstreamBuffer == nullptr) {
            error = nvencStatusMessage(L"Não foi possível criar o buffer de saída do NVENC", status, &m_functions, m_session);
            shutdown();
            return false;
        }
        m_bitstream = bitstream.bitstreamBuffer;

        NV_ENC_REGISTER_RESOURCE resource{};
        resource.version = NV_ENC_REGISTER_RESOURCE_VER;
        resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        resource.width = width;
        resource.height = height;
        resource.pitch = 0;
        resource.subResourceIndex = 0;
        resource.resourceToRegister = texture;
        resource.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        resource.bufferUsage = NV_ENC_INPUT_IMAGE;
        status = m_functions.nvEncRegisterResource(m_session, &resource);
        if (status != NV_ENC_SUCCESS || resource.registeredResource == nullptr) {
            error = nvencStatusMessage(L"Não foi possível registrar a textura Direct3D 11 no NVENC", status, &m_functions, m_session);
            shutdown();
            return false;
        }
        m_registeredResource = resource.registeredResource;
        m_width = width;
        m_height = height;
        m_frameDuration = kHundredNanosecondsPerSecond / fps;
        m_firstFrame = true;
        m_frameIndex = 0;
        return true;
    }

    bool encode(
        ID3D11Texture2D* texture,
        LONGLONG timestamp,
        std::vector<std::uint8_t>& packet,
        std::wstring& error) {
        packet.clear();
        if (texture == nullptr || m_session == nullptr || m_registeredResource == nullptr) {
            error = L"O encoder NVENC não está pronto para receber o frame.";
            return false;
        }

        NV_ENC_MAP_INPUT_RESOURCE map{};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = m_registeredResource;
        NVENCSTATUS status = m_functions.nvEncMapInputResource(m_session, &map);
        if (status != NV_ENC_SUCCESS) {
            error = nvencStatusMessage(L"Não foi possível mapear a textura para o NVENC", status, &m_functions, m_session);
            return false;
        }

        bool mapped = true;
        NV_ENC_PIC_PARAMS picture{};
        picture.version = NV_ENC_PIC_PARAMS_VER;
        picture.inputWidth = m_width;
        picture.inputHeight = m_height;
        picture.inputPitch = 0;
        picture.encodePicFlags = m_firstFrame
            ? NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS
            : 0;
        picture.frameIdx = m_frameIndex++;
        picture.inputTimeStamp = static_cast<std::uint64_t>(timestamp);
        picture.inputDuration = static_cast<std::uint64_t>(m_frameDuration);
        picture.inputBuffer = map.mappedResource;
        picture.outputBitstream = m_bitstream;
        picture.bufferFmt = map.mappedBufferFmt;
        picture.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        picture.pictureType = NV_ENC_PIC_TYPE_UNKNOWN;

        status = m_functions.nvEncEncodePicture(m_session, &picture);
        if (status == NV_ENC_ERR_NEED_MORE_INPUT) {
            m_functions.nvEncUnmapInputResource(m_session, map.mappedResource);
            return true;
        }
        if (status != NV_ENC_SUCCESS) {
            m_functions.nvEncUnmapInputResource(m_session, map.mappedResource);
            error = nvencStatusMessage(L"O NVENC não conseguiu codificar o frame", status, &m_functions, m_session);
            return false;
        }
        m_firstFrame = false;

        NV_ENC_LOCK_BITSTREAM lock{};
        lock.version = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = m_bitstream;
        status = m_functions.nvEncLockBitstream(m_session, &lock);
        if (status == NV_ENC_SUCCESS) {
            const auto* data = static_cast<const std::uint8_t*>(lock.bitstreamBufferPtr);
            if (data != nullptr && lock.bitstreamSizeInBytes > 0) {
                packet.assign(data, data + lock.bitstreamSizeInBytes);
            }
            const NVENCSTATUS unlockStatus = m_functions.nvEncUnlockBitstream(m_session, m_bitstream);
            if (unlockStatus != NV_ENC_SUCCESS && error.empty()) {
                error = nvencStatusMessage(L"Não foi possível liberar o pacote do NVENC", unlockStatus, &m_functions, m_session);
                status = unlockStatus;
            }
        }
        if (mapped) {
            const NVENCSTATUS unmapStatus = m_functions.nvEncUnmapInputResource(m_session, map.mappedResource);
            mapped = false;
            if (unmapStatus != NV_ENC_SUCCESS && error.empty()) {
                error = nvencStatusMessage(L"Não foi possível liberar a textura do NVENC", unmapStatus, &m_functions, m_session);
                status = unmapStatus;
            }
        }
        if (status != NV_ENC_SUCCESS) {
            if (error.empty()) {
                error = nvencStatusMessage(
                    m_codec == VideoCodec::Av1
                        ? L"Não foi possível obter o pacote AV1 do NVENC"
                        : L"Não foi possível obter o pacote H.264 do NVENC",
                    status,
                    &m_functions,
                    m_session);
            }
            return false;
        }
        return true;
    }

    bool flush(std::vector<std::uint8_t>& packet, std::wstring& error) {
        packet.clear();
        if (m_session == nullptr || m_bitstream == nullptr) {
            return true;
        }

        NV_ENC_PIC_PARAMS picture{};
        picture.version = NV_ENC_PIC_PARAMS_VER;
        picture.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        NVENCSTATUS status = m_functions.nvEncEncodePicture(m_session, &picture);
        if (status == NV_ENC_ERR_NEED_MORE_INPUT) {
            return true;
        }
        if (status != NV_ENC_SUCCESS) {
            error = nvencStatusMessage(L"Não foi possível finalizar o fluxo do NVENC", status, &m_functions, m_session);
            return false;
        }
        return true;
    }

private:
    void shutdown() noexcept {
        if (m_session != nullptr) {
            if (m_registeredResource != nullptr && m_functions.nvEncUnregisterResource != nullptr) {
                m_functions.nvEncUnregisterResource(m_session, m_registeredResource);
                m_registeredResource = nullptr;
            }
            if (m_bitstream != nullptr && m_functions.nvEncDestroyBitstreamBuffer != nullptr) {
                m_functions.nvEncDestroyBitstreamBuffer(m_session, m_bitstream);
                m_bitstream = nullptr;
            }
            if (m_functions.nvEncDestroyEncoder != nullptr) {
                m_functions.nvEncDestroyEncoder(m_session);
            }
            m_session = nullptr;
        }
        m_library.reset();
    }

    std::unique_ptr<NvencLibrary> m_library;
    NV_ENCODE_API_FUNCTION_LIST m_functions{};
    void* m_session{nullptr};
    NV_ENC_REGISTERED_PTR m_registeredResource{nullptr};
    NV_ENC_OUTPUT_PTR m_bitstream{nullptr};
    NV_ENC_CONFIG m_config{};
    VideoCodec m_codec{VideoCodec::H264};
    std::uint32_t m_width{0};
    std::uint32_t m_height{0};
    LONGLONG m_frameDuration{0};
    std::uint32_t m_frameIndex{0};
    bool m_firstFrame{false};
};

#endif // FASTRECORD_HAS_NVENC

} // namespace

NvencBackend::~NvencBackend() {
    std::wstring ignored;
    stop(ignored);
}

bool NvencBackend::start(const RecordingSettings& settings, std::wstring& error) {
#if !FASTRECORD_HAS_NVENC
    error = L"Esta build não inclui os headers do NVENC. Reconfigure com FASTRECORD_FETCH_ENCODER_HEADERS=ON.";
    return false;
#else
    if (m_worker.joinable() || m_running.load()) {
        error = L"Já existe uma captura em andamento.";
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
                setWorkerError(L"Falha inesperada no backend NVENC.");
            }
            m_running = false;
        });
        error = startupFuture.get();
    } catch (...) {
        if (m_worker.joinable()) {
            m_worker.join();
        }
        std::scoped_lock lock(m_mutex);
        error = m_workerError.empty() ? L"Não foi possível iniciar o NVENC." : m_workerError;
        return false;
    }
    if (!error.empty()) {
        if (m_worker.joinable()) {
            m_worker.join();
        }
        return false;
    }
    return true;
#endif
}

bool NvencBackend::pause(std::wstring& error) {
    if (!m_running.load() || m_paused.load()) {
        std::scoped_lock lock(m_mutex);
        error = m_workerError.empty() ? L"A gravação não está ativa." : m_workerError;
        return false;
    }
    m_paused = true;
    return true;
}

bool NvencBackend::resume(std::wstring& error) {
    if (!m_running.load() || !m_paused.load()) {
        std::scoped_lock lock(m_mutex);
        error = m_workerError.empty() ? L"A gravação não está pausada." : m_workerError;
        return false;
    }
    m_paused = false;
    return true;
}

bool NvencBackend::stop(std::wstring& error) {
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

std::filesystem::path NvencBackend::outputPath() const {
    std::scoped_lock lock(m_mutex);
    return m_outputPath;
}

void NvencBackend::setWorkerError(std::wstring error) {
    std::scoped_lock lock(m_mutex);
    if (m_workerError.empty()) {
        m_workerError = std::move(error);
    }
}

void NvencBackend::recordLoop(
    RecordingSettings settings,
    std::promise<std::wstring> startupResult) {
    bool startupReported = false;
    auto reportStartup = [&](std::wstring message) {
        if (!startupReported) {
            startupResult.set_value(std::move(message));
            startupReported = true;
        }
    };

#if FASTRECORD_HAS_NVENC
    try {
        MediaRuntime runtime;
        const auto width = settings.width;
        const auto height = settings.height;
        const auto fps = settings.framesPerSecond;
        const auto bitrate = settings.bitrateMbps * 1'000'000u;
        const LONGLONG frameDuration = kHundredNanosecondsPerSecond / fps;

        auto capture = std::make_unique<GraphicsCapture>(settings);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!capture->update()) {
            if (m_stopRequested || std::chrono::steady_clock::now() >= deadline) {
                throw winrt::hresult_error(E_ABORT, L"O Windows não entregou frames do monitor.");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        NvencEncoder encoder;
        std::wstring encoderError;
        if (!encoder.initialize(
                capture->device(),
                capture->texture(),
                width,
                height,
                fps,
                bitrate,
                settings.codec,
                encoderError)) {
            reportStartup(encoderError);
            setWorkerError(encoderError);
            return;
        }

        std::vector<std::uint8_t> firstPacket;
        LONGLONG timestamp = 0;
        std::wstring loopError;
        for (std::uint32_t attempt = 0; attempt < 4 && firstPacket.empty(); ++attempt) {
            if (attempt > 0) {
                if (!capture->update()) {
                    continue;
                }
                timestamp = static_cast<LONGLONG>(attempt) * frameDuration;
            }
            if (!encoder.encode(capture->texture(), timestamp, firstPacket, loopError)) {
                reportStartup(loopError);
                setWorkerError(loopError);
                return;
            }
        }
        if (firstPacket.empty()) {
            loopError = settings.codec == VideoCodec::Av1
                ? L"O NVENC não produziu o primeiro frame AV1."
                : L"O NVENC não produziu o primeiro frame H.264.";
            reportStartup(loopError);
            setWorkerError(loopError);
            return;
        }

        std::vector<std::uint8_t> sequenceHeader;
        if (settings.codec == VideoCodec::H264) {
            if (!extractH264SequenceHeader(firstPacket, sequenceHeader)) {
                loopError = L"O primeiro pacote H.264 do NVENC não contém SPS e PPS.";
                reportStartup(loopError);
                setWorkerError(loopError);
                return;
            }
        } else {
            sequenceHeader = firstPacket;
        }

        std::unique_ptr<AudioCapture> audioCapture;
        std::unique_ptr<AacEncoder> audioEncoder;
        if (settings.captureMicrophone || settings.captureSystemAudio) {
            audioCapture = std::make_unique<AudioCapture>();
            audioEncoder = std::make_unique<AacEncoder>();
            std::wstring audioError;
            if (!audioEncoder->initialize(audioError) ||
                !audioCapture->start(
                    settings.captureMicrophone,
                    settings.captureSystemAudio,
                    audioError)) {
                reportStartup(audioError);
                setWorkerError(audioError);
                return;
            }
        }

        Mp4Writer writer;
        const auto output = outputPath();
        std::wstring writerError;
        if (!writer.start(
                output,
                width,
                height,
                fps,
                bitrate,
                settings.codec,
                sequenceHeader,
                audioCapture != nullptr,
                writerError)) {
            reportStartup(writerError);
            setWorkerError(writerError);
            return;
        }
        if (!writer.write(firstPacket, timestamp, writerError)) {
            reportStartup(writerError);
            setWorkerError(writerError);
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

        timestamp += frameDuration;
        auto nextFrame = std::chrono::steady_clock::now();
        while (!m_stopRequested.load()) {
            if (m_paused.load()) {
                if (!processAudio(false)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                nextFrame = std::chrono::steady_clock::now();
                continue;
            }

            if (!processAudio(true)) break;

            if (!capture->update()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            std::vector<std::uint8_t> packet;
            if (!encoder.encode(capture->texture(), timestamp, packet, loopError)) {
                break;
            }
            if (!packet.empty() && !writer.write(packet, timestamp, loopError)) {
                break;
            }

            timestamp += frameDuration;
            nextFrame += std::chrono::nanoseconds(frameDuration * 100);
            const auto now = std::chrono::steady_clock::now();
            if (nextFrame > now) {
                std::this_thread::sleep_until(nextFrame);
            } else if (now - nextFrame > std::chrono::milliseconds(250)) {
                nextFrame = now;
            }
        }

        if (loopError.empty() && !processAudio(true)) {
            // The audio error is reported below.
        }
        if (loopError.empty() && audioEncoder) {
            audioPackets.clear();
            if (!audioEncoder->drain(audioPackets, loopError)) {
                // The audio error is reported below.
            } else {
                for (const auto& audioPacket : audioPackets) {
                    if (!writer.writeAudio(audioPacket, loopError)) break;
                }
            }
        }

        std::vector<std::uint8_t> finalPacket;
        if (loopError.empty() && !encoder.flush(finalPacket, loopError)) {
            // The encoder error is reported below together with the finalize error.
        }
        if (loopError.empty() && !finalPacket.empty() && !writer.write(finalPacket, timestamp, loopError)) {
            // Keep the first meaningful error.
        }
        std::wstring finalizeError;
        if (!writer.finalize(finalizeError) && loopError.empty()) {
            loopError = std::move(finalizeError);
        }
        if (!loopError.empty()) {
            setWorkerError(std::move(loopError));
        }
    } catch (const winrt::hresult_error& failure) {
        const std::wstring message = failure.message().c_str();
        reportStartup(message);
        setWorkerError(message);
    } catch (...) {
        reportStartup(L"Falha inesperada no backend NVENC.");
        setWorkerError(L"Falha inesperada no backend NVENC.");
    }
#else
    reportStartup(L"Esta build não inclui suporte ao NVENC.");
#endif
}

} // namespace fastrecord::recording
