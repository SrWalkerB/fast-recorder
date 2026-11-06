// Records the screen through the AMD AMF backend and then decodes the
// resulting MP4 with Media Foundation to check that the file is playable and
// that the video timeline matches the wall-clock time of the recording.
//
// Usage:
//   amf-smoke <output-dir> [--codec=h264|av1] [--seconds=N] [--size=WxH]
//             [--fps=N] [--bitrate=N] [--pause=N] [--animate] [--mic]
//             [--no-system-audio]
//
// --pause=N pauses for N seconds in the middle; the paused time must not
// appear in the video.
//
// --animate opens a small window that changes color every frame, forcing
// Windows Graphics Capture to deliver new frames continuously. Without it the
// desktop is usually static, which exercises the constant-frame-rate path.

#include "recording/AmfBackend.h"
#include "recording/AmfProbe.h"

#include <Windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace fastrecord::recording;

namespace {

constexpr double kHundredNanoseconds = 10'000'000.0;

struct Options {
    std::filesystem::path outputDirectory;
    VideoCodec codec{VideoCodec::H264};
    double seconds{5.0};
    std::uint32_t width{1280};
    std::uint32_t height{720};
    std::uint32_t fps{30};
    std::uint32_t bitrateMbps{12};
    double pauseSeconds{0.0};
    bool animate{false};
    bool microphone{false};
    bool systemAudio{true};
};

bool startsWith(const std::wstring& value, const wchar_t* prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool parseOptions(int argc, wchar_t** argv, Options& options) {
    options.outputDirectory = std::filesystem::current_path() / L"amf-smoke-output";
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (startsWith(argument, L"--codec=")) {
            const auto value = argument.substr(8);
            if (value == L"av1") {
                options.codec = VideoCodec::Av1;
            } else if (value == L"h264") {
                options.codec = VideoCodec::H264;
            } else {
                return false;
            }
        } else if (startsWith(argument, L"--seconds=")) {
            options.seconds = std::wcstod(argument.c_str() + 10, nullptr);
        } else if (startsWith(argument, L"--size=")) {
            if (swscanf_s(argument.c_str() + 7, L"%ux%u", &options.width, &options.height) != 2) {
                return false;
            }
        } else if (startsWith(argument, L"--fps=")) {
            options.fps = static_cast<std::uint32_t>(std::wcstoul(argument.c_str() + 6, nullptr, 10));
        } else if (startsWith(argument, L"--bitrate=")) {
            options.bitrateMbps = static_cast<std::uint32_t>(std::wcstoul(argument.c_str() + 10, nullptr, 10));
        } else if (startsWith(argument, L"--pause=")) {
            options.pauseSeconds = std::wcstod(argument.c_str() + 8, nullptr);
        } else if (argument == L"--animate") {
            options.animate = true;
        } else if (argument == L"--mic") {
            options.microphone = true;
        } else if (argument == L"--no-system-audio") {
            options.systemAudio = false;
        } else if (!startsWith(argument, L"--")) {
            options.outputDirectory = argument;
        } else {
            return false;
        }
    }
    return options.seconds > 0.5 && options.fps > 0;
}

// A small top-most window whose color changes on every timer tick.
class AnimatedWindow final {
public:
    void start() {
        m_thread = std::thread([this] { run(); });
        while (!m_ready.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ~AnimatedWindow() {
        if (m_thread.joinable()) {
            PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
            m_thread.join();
        }
    }

private:
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_TIMER) {
            auto* self = reinterpret_cast<AnimatedWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (self != nullptr) {
                ++self->m_tick;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        if (message == WM_PAINT) {
            auto* self = reinterpret_cast<AnimatedWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            const unsigned tick = self != nullptr ? self->m_tick : 0;
            HBRUSH brush = CreateSolidBrush(RGB((tick * 7) & 0xff, (tick * 13) & 0xff, (tick * 29) & 0xff));
            FillRect(dc, &paint.rcPaint, brush);
            DeleteObject(brush);
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void run() {
        m_threadId = GetCurrentThreadId();
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = procedure;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"FastRecordAmfSmokeAnimation";
        RegisterClassW(&windowClass);
        HWND window = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            windowClass.lpszClassName,
            L"amf-smoke",
            WS_POPUP | WS_VISIBLE,
            40, 40, 360, 240,
            nullptr, nullptr, windowClass.hInstance, nullptr);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        SetTimer(window, 1, 10, nullptr);
        m_ready = true;

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        DestroyWindow(window);
        UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
    }

    std::thread m_thread;
    DWORD m_threadId{0};
    std::atomic_bool m_ready{false};
    unsigned m_tick{0};
};

struct StreamReport {
    bool present{false};
    GUID subtype{GUID_NULL};
    std::uint32_t width{0};
    std::uint32_t height{0};
    // As reported by the decoder from the bitstream's color description.
    UINT32 yuvMatrix{MFVideoTransferMatrix_Unknown};
    UINT32 nominalRange{MFNominalRange_Unknown};
    std::uint64_t samples{0};
    LONGLONG firstTimestamp{-1};
    LONGLONG endTimestamp{0};
};

struct FileReport {
    double containerSeconds{0};
    StreamReport video;
    StreamReport audio;
};

std::wstring hresultText(const wchar_t* operation, HRESULT result) {
    wchar_t buffer[160]{};
    swprintf_s(buffer, L"%s (HRESULT 0x%08X)", operation, static_cast<unsigned>(result));
    return buffer;
}

// Decodes every sample so a broken bitstream fails here instead of in a player.
bool decodeFile(const std::filesystem::path& path, FileReport& report, std::wstring& error) {
    ComPtr<IMFAttributes> attributes;
    HRESULT result = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(result)) result = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(result)) result = MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader);
    if (FAILED(result)) {
        error = hresultText(L"O Media Foundation não abriu o MP4", result);
        return false;
    }

    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(
            static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration))) {
        report.containerSeconds = static_cast<double>(duration.uhVal.QuadPart) / kHundredNanoseconds;
    }
    PropVariantClear(&duration);

    const auto videoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    const auto audioStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);

    ComPtr<IMFMediaType> nativeVideo;
    if (FAILED(reader->GetNativeMediaType(videoStream, 0, &nativeVideo))) {
        error = L"O MP4 não tem faixa de vídeo.";
        return false;
    }
    report.video.present = true;
    nativeVideo->GetGUID(MF_MT_SUBTYPE, &report.video.subtype);
    MFGetAttributeSize(nativeVideo.Get(), MF_MT_FRAME_SIZE, &report.video.width, &report.video.height);

    ComPtr<IMFMediaType> decodedVideo;
    result = MFCreateMediaType(&decodedVideo);
    if (SUCCEEDED(result)) result = decodedVideo->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(result)) result = decodedVideo->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(result)) result = reader->SetCurrentMediaType(videoStream, nullptr, decodedVideo.Get());
    if (FAILED(result)) {
        error = hresultText(L"Nenhum decodificador aceitou a faixa de vídeo", result);
        return false;
    }
    ComPtr<IMFMediaType> currentVideo;
    if (SUCCEEDED(reader->GetCurrentMediaType(videoStream, &currentVideo))) {
        currentVideo->GetUINT32(MF_MT_YUV_MATRIX, &report.video.yuvMatrix);
        currentVideo->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &report.video.nominalRange);
    }

    ComPtr<IMFMediaType> nativeAudio;
    if (SUCCEEDED(reader->GetNativeMediaType(audioStream, 0, &nativeAudio))) {
        report.audio.present = true;
        nativeAudio->GetGUID(MF_MT_SUBTYPE, &report.audio.subtype);
        ComPtr<IMFMediaType> decodedAudio;
        result = MFCreateMediaType(&decodedAudio);
        if (SUCCEEDED(result)) result = decodedAudio->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(result)) result = decodedAudio->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        if (SUCCEEDED(result)) result = reader->SetCurrentMediaType(audioStream, nullptr, decodedAudio.Get());
        if (FAILED(result)) {
            error = hresultText(L"Nenhum decodificador aceitou a faixa de áudio", result);
            return false;
        }
    }

    for (const DWORD stream : {videoStream, audioStream}) {
        StreamReport& target = stream == videoStream ? report.video : report.audio;
        if (!target.present) continue;
        for (;;) {
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            result = reader->ReadSample(stream, 0, nullptr, &flags, &timestamp, &sample);
            if (FAILED(result)) {
                error = hresultText(
                    stream == videoStream ? L"Falha ao decodificar o vídeo" : L"Falha ao decodificar o áudio",
                    result);
                return false;
            }
            if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
                error = L"O Media Foundation sinalizou erro na leitura do arquivo.";
                return false;
            }
            if (sample) {
                LONGLONG sampleDuration = 0;
                sample->GetSampleDuration(&sampleDuration);
                ++target.samples;
                if (target.firstTimestamp < 0) target.firstTimestamp = timestamp;
                target.endTimestamp = (std::max)(target.endTimestamp, timestamp + sampleDuration);
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) break;
        }
    }
    return true;
}

std::wstring subtypeName(const GUID& subtype) {
    if (subtype == MFVideoFormat_H264) return L"H.264";
    if (subtype == MFVideoFormat_AV1) return L"AV1";
    if (subtype == MFAudioFormat_AAC) return L"AAC";
    return L"desconhecido";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        std::wcerr << L"Uso: amf-smoke <pasta> [--codec=h264|av1] [--seconds=N] [--size=WxH] "
                      L"[--fps=N] [--bitrate=N] [--pause=N] [--animate] [--mic] [--no-system-audio]\n";
        return 64;
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::wcerr << L"Não foi possível inicializar o COM.\n";
        return 3;
    }

    const auto capability = probeAmf();
    std::wcout << L"Sonda AMF: " << capability.description << L"\n";

    RecordingSettings settings{};
    settings.target.kind = CaptureTargetKind::CurrentMonitor;
    settings.outputDirectory = options.outputDirectory;
    settings.width = options.width;
    settings.height = options.height;
    settings.framesPerSecond = options.fps;
    settings.bitrateMbps = options.bitrateMbps;
    settings.engine = EncoderEngine::Amf;
    settings.codec = options.codec;
    settings.captureCursor = true;
    settings.captureMicrophone = options.microphone;
    settings.captureSystemAudio = options.systemAudio;

    AnimatedWindow animation;
    if (options.animate) animation.start();

    AmfBackend backend;
    std::wstring error;
    if (!backend.start(settings, error)) {
        std::wcerr << L"AMF smoke falhou ao iniciar: " << error << L"\n";
        return 1;
    }
    // wallSeconds counts only the time spent recording, excluding the pause.
    const auto startedAt = std::chrono::steady_clock::now();
    double pausedSeconds = 0.0;
    if (options.pauseSeconds > 0.0) {
        std::this_thread::sleep_for(std::chrono::duration<double>(options.seconds / 2));
        if (!backend.pause(error)) {
            std::wcerr << L"AMF smoke falhou ao pausar: " << error << L"\n";
            return 2;
        }
        const auto pausedAt = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::duration<double>(options.pauseSeconds));
        if (!backend.resume(error)) {
            std::wcerr << L"AMF smoke falhou ao retomar: " << error << L"\n";
            return 2;
        }
        pausedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - pausedAt).count();
        std::this_thread::sleep_for(std::chrono::duration<double>(options.seconds / 2));
    } else {
        std::this_thread::sleep_for(std::chrono::duration<double>(options.seconds));
    }
    const double wallSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt).count() - pausedSeconds;
    if (!backend.stop(error)) {
        std::wcerr << L"AMF smoke falhou ao parar: " << error << L"\n";
        return 2;
    }

    const auto output = backend.outputPath();
    std::error_code sizeError;
    const auto bytes = std::filesystem::file_size(output, sizeError);
    std::wcout << L"Arquivo: " << output.wstring() << L"\n"
               << L"Tamanho: " << (sizeError ? 0 : bytes / 1024) << L" KiB\n"
               << L"Tempo gravado: " << wallSeconds << L" s\n";

    FileReport report;
    if (FAILED(MFStartup(MF_VERSION))) {
        std::wcerr << L"Não foi possível iniciar o Media Foundation para validar o arquivo.\n";
        return 3;
    }
    const bool decoded = decodeFile(output, report, error);
    MFShutdown();
    if (!decoded) {
        std::wcerr << L"Validação falhou: " << error << L"\n";
        return 4;
    }

    const double videoSeconds =
        static_cast<double>(report.video.endTimestamp - (std::max<LONGLONG>)(0, report.video.firstTimestamp)) /
        kHundredNanoseconds;
    const double audioSeconds = report.audio.present
        ? static_cast<double>(report.audio.endTimestamp) / kHundredNanoseconds
        : 0.0;
    std::wcout << L"Duração no contêiner: " << report.containerSeconds << L" s\n"
               << L"Vídeo: " << subtypeName(report.video.subtype) << L" "
               << report.video.width << L"x" << report.video.height << L", "
               << report.video.samples << L" frames decodificados, " << videoSeconds << L" s ("
               << (videoSeconds > 0 ? report.video.samples / videoSeconds : 0) << L" fps)\n"
               << L"Cor: matriz "
               << (report.video.yuvMatrix == MFVideoTransferMatrix_BT709 ? L"BT.709"
                   : report.video.yuvMatrix == MFVideoTransferMatrix_BT601 ? L"BT.601" : L"não declarada")
               << L", faixa "
               << (report.video.nominalRange == MFNominalRange_16_235 ? L"16-235"
                   : report.video.nominalRange == MFNominalRange_0_255 ? L"0-255" : L"não declarada")
               << L"\n";
    if (report.audio.present) {
        std::wcout << L"Áudio: " << subtypeName(report.audio.subtype) << L", "
                   << report.audio.samples << L" blocos decodificados, " << audioSeconds << L" s\n";
    } else {
        std::wcout << L"Áudio: sem faixa\n";
    }

    int failures = 0;
    const auto check = [&](bool condition, const wchar_t* description) {
        std::wcout << (condition ? L"[OK]     " : L"[FALHOU] ") << description << L"\n";
        if (!condition) ++failures;
    };
    const GUID expectedSubtype = options.codec == VideoCodec::Av1 ? MFVideoFormat_AV1 : MFVideoFormat_H264;
    check(report.video.subtype == expectedSubtype, L"o vídeo usa o codec pedido");
    check(report.video.width == options.width && report.video.height == options.height,
        L"a resolução do arquivo é a configurada");
    check(report.video.samples > 0, L"o vídeo tem frames decodificáveis");
    check(std::abs(videoSeconds - wallSeconds) <= (std::max)(0.5, wallSeconds * 0.1),
        L"a duração do vídeo acompanha o tempo real da gravação (±10%)");
    const double expectedFrames = wallSeconds * options.fps;
    check(static_cast<double>(report.video.samples) >= expectedFrames * 0.9,
        L"o encoder manteve pelo menos 90% dos frames esperados");
    if (!options.microphone && !options.systemAudio) {
        check(!report.audio.present, L"sem fontes de áudio, o arquivo não tem faixa de áudio");
    } else if (options.microphone) {
        // The microphone delivers samples continuously, so its track must
        // cover the whole recording.
        check(report.audio.present, L"o microfone gerou uma faixa de áudio");
        check(std::abs(audioSeconds - videoSeconds) <= 0.5,
            L"áudio e vídeo terminam juntos (±0,5 s)");
    } else if (!report.audio.present) {
        std::wcout << L"[INFO]   sem faixa de áudio: o loopback do Windows não entrega "
                      L"amostras enquanto nada está tocando\n";
    }

    std::wcout << (failures == 0 ? L"RESULTADO: OK\n" : L"RESULTADO: FALHOU\n");
    return failures == 0 ? 0 : 5;
}
