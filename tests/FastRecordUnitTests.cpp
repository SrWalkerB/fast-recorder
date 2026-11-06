#include "app/AppSettings.h"
#include "app/UpdateService.h"
#include "platform/TrayIcon.h"
#include "recording/AmfBackend.h"
#include "recording/AmfProbe.h"
#include "recording/AudioCapture.h"
#include "recording/Mp4Muxer.h"
#include "recording/NvencProbe.h"
#include "recording/QsvProbe.h"
#include "recording/RecorderController.h"
#include "ui/AppPage.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool condition, const wchar_t* description) {
    if (!condition) {
        std::wcerr << L"[FALHOU] " << description << L"\n";
        ++g_failures;
    }
}

std::wstring testRegistryPath() {
    return L"Software\\Fast Record\\Tests\\Unit_" +
        std::to_wstring(GetCurrentProcessId());
}

void removeTestRegistryKey(const std::wstring& path) {
    RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
}

std::vector<std::uint8_t> iconColorPixels(HICON icon) {
    ICONINFO iconInfo{};
    if (icon == nullptr || !GetIconInfo(icon, &iconInfo)) {
        return {};
    }

    std::vector<std::uint8_t> pixels;
    BITMAP bitmap{};
    if (iconInfo.hbmColor != nullptr &&
        GetObjectW(iconInfo.hbmColor, sizeof(bitmap), &bitmap) != 0) {
        const int width = bitmap.bmWidth;
        const int height = std::abs(bitmap.bmHeight);
        if (width > 0 && height > 0) {
            BITMAPINFO description{};
            description.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            description.bmiHeader.biWidth = width;
            description.bmiHeader.biHeight = -height;
            description.bmiHeader.biPlanes = 1;
            description.bmiHeader.biBitCount = 32;
            description.bmiHeader.biCompression = BI_RGB;
            pixels.resize(static_cast<std::size_t>(width) * height * 4);

            HDC screen = GetDC(nullptr);
            if (screen == nullptr || GetDIBits(
                    screen,
                    iconInfo.hbmColor,
                    0,
                    static_cast<UINT>(height),
                    pixels.data(),
                    &description,
                    DIB_RGB_COLORS) != height) {
                pixels.clear();
            }
            if (screen != nullptr) {
                ReleaseDC(nullptr, screen);
            }
        }
    }

    if (iconInfo.hbmColor != nullptr) {
        DeleteObject(iconInfo.hbmColor);
    }
    if (iconInfo.hbmMask != nullptr) {
        DeleteObject(iconInfo.hbmMask);
    }
    return pixels;
}

void testSettingsPersistence() {
    const std::wstring path = testRegistryPath();
    removeTestRegistryKey(path);

    const fastrecord::AppSettings defaults = fastrecord::loadAppSettings(path);
    expect(defaults.engine == fastrecord::recording::EncoderEngine::Automatic,
        L"as configurações ausentes usam encoder automático");
    expect(defaults.codec == fastrecord::recording::VideoCodec::H264,
        L"as configurações ausentes usam H.264");
    expect(defaults.captureArea == L"monitor" && defaults.resolution == L"1920x1080",
        L"as configurações ausentes usam captura do monitor em 1080p");
    expect(defaults.framesPerSecond == 30 && defaults.bitrateMbps == 20,
        L"as configurações ausentes usam 30 FPS e 20 Mbps");
    expect(defaults.microphoneEnabled && defaults.systemAudioEnabled &&
            !defaults.alwaysOnTop && !defaults.startWithWindows,
        L"as configurações ausentes usam áudio ligado, janela sem pin e sem inicialização automática");

    fastrecord::AppSettings expected;
    expected.engine = fastrecord::recording::EncoderEngine::Amf;
    expected.codec = fastrecord::recording::VideoCodec::Av1;
    expected.captureArea = L"region";
    expected.resolution = L"2560x1440";
    expected.framesPerSecond = 120;
    expected.bitrateMbps = 63;
    expected.microphoneEnabled = false;
    expected.systemAudioEnabled = false;
    expected.alwaysOnTop = true;
    expected.startWithWindows = true;

    expect(fastrecord::saveAppSettings(expected, path),
        L"as configurações são gravadas no registro");
    const fastrecord::AppSettings loaded = fastrecord::loadAppSettings(path);
    expect(loaded.engine == expected.engine && loaded.codec == expected.codec,
        L"encoder e codec sobrevivem ao round-trip");
    expect(loaded.captureArea == expected.captureArea && loaded.resolution == expected.resolution,
        L"área e resolução sobrevivem ao round-trip");
    expect(loaded.framesPerSecond == expected.framesPerSecond &&
            loaded.bitrateMbps == expected.bitrateMbps,
        L"FPS e bitrate sobrevivem ao round-trip");
    expect(loaded.microphoneEnabled == expected.microphoneEnabled &&
            loaded.systemAudioEnabled == expected.systemAudioEnabled &&
            loaded.alwaysOnTop == expected.alwaysOnTop &&
            loaded.startWithWindows == expected.startWithWindows,
        L"os toggles sobrevivem ao round-trip");

    expected.engine = fastrecord::recording::EncoderEngine::Qsv;
    expect(fastrecord::saveAppSettings(expected, path),
        L"a seleção Intel QSV pode ser gravada");
    expect(fastrecord::loadAppSettings(path).engine ==
            fastrecord::recording::EncoderEngine::Qsv,
        L"a seleção Intel QSV sobrevive ao round-trip");

    fastrecord::AppSettings invalid = expected;
    invalid.engine = static_cast<fastrecord::recording::EncoderEngine>(99);
    invalid.codec = static_cast<fastrecord::recording::VideoCodec>(99);
    invalid.captureArea = L"invalid-area";
    invalid.resolution = L"9999x9999";
    invalid.framesPerSecond = 25;
    invalid.bitrateMbps = 100;
    expect(fastrecord::saveAppSettings(invalid, path),
        L"valores fora do domínio ainda podem ser escritos para validação");
    const fastrecord::AppSettings sanitized = fastrecord::loadAppSettings(path);
    expect(sanitized.engine == fastrecord::recording::EncoderEngine::Automatic &&
            sanitized.codec == fastrecord::recording::VideoCodec::H264,
        L"encoder e codec inválidos voltam para os padrões");
    expect(sanitized.captureArea == L"monitor" && sanitized.resolution == L"1920x1080",
        L"área e resolução inválidas voltam para os padrões");
    expect(sanitized.framesPerSecond == 30 && sanitized.bitrateMbps == 80,
        L"FPS inválido volta ao padrão e bitrate alto é limitado a 80 Mbps");

    invalid.bitrateMbps = 0;
    expect(fastrecord::saveAppSettings(invalid, path),
        L"o limite inferior do bitrate é gravado");
    expect(fastrecord::loadAppSettings(path).bitrateMbps == 4,
        L"bitrate baixo é limitado a 4 Mbps");

    removeTestRegistryKey(path);
}

struct FakeBackendState {
    bool startResult{true};
    bool pauseResult{true};
    bool resumeResult{true};
    bool stopResult{true};
    bool running{false};
    int startCalls{0};
    int pauseCalls{0};
    int resumeCalls{0};
    int stopCalls{0};
    fastrecord::recording::RecordingSettings lastSettings{};
    std::filesystem::path outputPath{L"C:\\FastRecordTests\\recording.mp4"};
};

class FakeBackend final : public fastrecord::recording::IRecordingBackend {
public:
    explicit FakeBackend(FakeBackendState& state) : m_state(state) {}

    bool start(
        const fastrecord::recording::RecordingSettings& settings,
        std::wstring& error) override {
        ++m_state.startCalls;
        m_state.lastSettings = settings;
        if (!m_state.startResult) {
            error = L"erro de início simulado";
            return false;
        }
        m_state.running = true;
        error.clear();
        return true;
    }

    bool pause(std::wstring& error) override {
        ++m_state.pauseCalls;
        if (!m_state.pauseResult) {
            error = L"erro de pausa simulado";
            return false;
        }
        error.clear();
        return true;
    }

    bool resume(std::wstring& error) override {
        ++m_state.resumeCalls;
        if (!m_state.resumeResult) {
            error = L"erro de retomada simulado";
            return false;
        }
        error.clear();
        return true;
    }

    bool stop(std::wstring& error) override {
        ++m_state.stopCalls;
        if (!m_state.stopResult) {
            error = L"erro de parada simulado";
            return false;
        }
        m_state.running = false;
        error.clear();
        return true;
    }

    std::filesystem::path outputPath() const override {
        return m_state.outputPath;
    }

    bool running() const noexcept override {
        return m_state.running;
    }

private:
    FakeBackendState& m_state;
};

fastrecord::recording::RecordingSettings testRecordingSettings() {
    fastrecord::recording::RecordingSettings settings;
    settings.width = 1280;
    settings.height = 720;
    settings.framesPerSecond = 60;
    settings.bitrateMbps = 12;
    settings.codec = fastrecord::recording::VideoCodec::H264;
    settings.captureMicrophone = true;
    settings.captureSystemAudio = true;
    return settings;
}

void testRecorderStateMachine() {
    using fastrecord::recording::EncoderEngine;
    using fastrecord::recording::RecorderController;
    using fastrecord::recording::RecorderState;

    FakeBackendState state;
    std::vector<EncoderEngine> selectedEngines;
    RecorderController recorder([&](EncoderEngine engine) {
        selectedEngines.push_back(engine);
        return std::make_unique<FakeBackend>(state);
    });
    auto settings = testRecordingSettings();

    expect(!recorder.hasFinished() && recorder.state() == RecorderState::Idle,
        L"o gravador começa ocioso sem backend criado");
    expect(!recorder.pause().success && !recorder.resume().success && !recorder.stop().success,
        L"operações de controle falham quando não há gravação");

    recorder.setEngine(EncoderEngine::Software);
    const auto started = recorder.start(settings);
    expect(started.success && recorder.isRecording() &&
            recorder.state() == RecorderState::Recording,
        L"iniciar muda o gravador para Recording");
    expect(selectedEngines.size() == 1 && selectedEngines.front() == EncoderEngine::Software,
        L"a fábrica recebe o encoder selecionado");
    expect(state.lastSettings.engine == EncoderEngine::Software &&
            state.lastSettings.width == 1280 && state.lastSettings.captureMicrophone,
        L"o controller repassa as configurações ao backend");
    expect(recorder.outputPath() == state.outputPath,
        L"o caminho de saída vem do backend ativo");

    expect(!recorder.start(settings).success && state.startCalls == 1,
        L"uma segunda gravação é recusada enquanto a primeira está ativa");
    expect(recorder.pause().success && recorder.isPaused() &&
            state.pauseCalls == 1,
        L"pausar muda o gravador para Paused");
    expect(!recorder.pause().success && state.pauseCalls == 1,
        L"pausar duas vezes não chama o backend novamente");
    expect(recorder.resume().success && recorder.isRecording() &&
            state.resumeCalls == 1,
        L"retomar volta para Recording");
    expect(!recorder.resume().success && state.resumeCalls == 1,
        L"retomar uma gravação que não está pausada é recusado");
    expect(recorder.stop().success && recorder.state() == RecorderState::Idle &&
            !state.running && state.stopCalls == 1,
        L"parar finaliza e volta para Idle");

    FakeBackendState qsvState;
    EncoderEngine selectedQsv = EncoderEngine::Automatic;
    RecorderController qsvRecorder([&](EncoderEngine engine) {
        selectedQsv = engine;
        return std::make_unique<FakeBackend>(qsvState);
    });
    qsvRecorder.setEngine(EncoderEngine::Qsv);
    expect(qsvRecorder.start(settings).success && selectedQsv == EncoderEngine::Qsv &&
            qsvState.lastSettings.engine == EncoderEngine::Qsv,
        L"a seleção Intel QSV chega ao backend de gravação");
    expect(qsvRecorder.stop().success, L"o cenário Intel QSV simulado pode ser finalizado");

    FakeBackendState startFailure;
    startFailure.startResult = false;
    RecorderController failedStart([&](EncoderEngine) {
        return std::make_unique<FakeBackend>(startFailure);
    });
    expect(!failedStart.start(settings).success && failedStart.state() == RecorderState::Idle &&
            failedStart.lastError() == L"erro de início simulado",
        L"falha de início não deixa o gravador preso em Starting");

    FakeBackendState transitionFailure;
    RecorderController failedTransition([&](EncoderEngine) {
        return std::make_unique<FakeBackend>(transitionFailure);
    });
    expect(failedTransition.start(settings).success, L"o cenário de falha de transição inicia");
    transitionFailure.pauseResult = false;
    expect(!failedTransition.pause().success && failedTransition.state() == RecorderState::Recording,
        L"falha ao pausar preserva o estado Recording");
    transitionFailure.pauseResult = true;
    expect(failedTransition.pause().success, L"pausa simulada pode ser retomada no cenário");
    transitionFailure.resumeResult = false;
    expect(!failedTransition.resume().success && failedTransition.state() == RecorderState::Paused,
        L"falha ao retomar preserva o estado Paused");
    transitionFailure.stopResult = false;
    expect(!failedTransition.stop().success && failedTransition.state() == RecorderState::Idle,
        L"falha ao parar retorna o controller para Idle");

    FakeBackendState av1State;
    RecorderController av1Recorder([&](EncoderEngine) {
        return std::make_unique<FakeBackend>(av1State);
    });
    av1Recorder.setEngine(EncoderEngine::Software);
    settings.codec = fastrecord::recording::VideoCodec::Av1;
    const auto av1Result = av1Recorder.start(settings);
    expect(!av1Result.success && av1State.startCalls == 0 &&
            av1Result.message.find(L"NVENC") != std::wstring::npos &&
            av1Result.message.find(L"AMF") != std::wstring::npos,
        L"AV1 sem NVENC ou AMF é recusado antes de iniciar o backend");

    FakeBackendState amfAv1State;
    EncoderEngine selectedAmf = EncoderEngine::Automatic;
    RecorderController amfAv1Recorder([&](EncoderEngine engine) {
        selectedAmf = engine;
        return std::make_unique<FakeBackend>(amfAv1State);
    });
    amfAv1Recorder.setEngine(EncoderEngine::Amf);
    expect(amfAv1Recorder.start(settings).success && selectedAmf == EncoderEngine::Amf &&
            amfAv1State.lastSettings.codec == fastrecord::recording::VideoCodec::Av1,
        L"AV1 com AMF chega ao backend AMD");
    expect(amfAv1Recorder.stop().success, L"o cenário AV1 com AMF simulado pode ser finalizado");
}

void testAudioContract() {
    using fastrecord::recording::AudioCapture;
    static_assert(AudioCapture::kSampleRate == 48'000);
    static_assert(AudioCapture::kChannels == 2);
    static_assert(AudioCapture::kBitsPerSample == 16);
    static_assert(AudioCapture::kBlockAlignment == 4);

    AudioCapture audio;
    std::vector<std::uint8_t> pcm;
    std::uint32_t frames = 0;
    std::wstring error;
    expect(!audio.read(pcm, frames, error) && !error.empty(),
        L"ler áudio antes de iniciar retorna erro");
    expect(!audio.start(false, false, error) &&
            error == L"Nenhuma fonte de áudio foi selecionada.",
        L"iniciar áudio sem microfone e sem áudio do PC retorna erro claro");
    audio.stop();
}

// Stereo 16-bit PCM where every sample has the same value.
std::vector<std::uint8_t> pcmFrames(std::uint32_t frames, std::int16_t value) {
    std::vector<std::uint8_t> pcm(static_cast<size_t>(frames) * fastrecord::recording::AudioCapture::kBlockAlignment);
    for (size_t offset = 0; offset < pcm.size(); offset += sizeof(value)) {
        std::memcpy(pcm.data() + offset, &value, sizeof(value));
    }
    return pcm;
}

bool allSamplesEqual(const std::vector<std::uint8_t>& pcm, std::int16_t value) {
    for (size_t offset = 0; offset + sizeof(value) <= pcm.size(); offset += sizeof(value)) {
        std::int16_t sample = 0;
        std::memcpy(&sample, pcm.data() + offset, sizeof(sample));
        if (sample != value) return false;
    }
    return true;
}

void testAudioMixer() {
    using fastrecord::recording::AudioCapture;
    constexpr std::uint32_t skew = 4'800;
    std::vector<std::uint8_t> pcm;

    auto microphone = pcmFrames(480, 1'000);
    std::vector<std::uint8_t> system;
    expect(AudioCapture::mixPending(microphone, true, system, false, skew, pcm) == 480 &&
            allSamplesEqual(pcm, 1'000) && microphone.empty(),
        L"só com o microfone, todo o áudio pendente sai sem alteração");

    microphone = pcmFrames(960, 1'000);
    system = pcmFrames(960, 3'000);
    expect(AudioCapture::mixPending(microphone, true, system, true, skew, pcm) == 960 &&
            allSamplesEqual(pcm, 2'000) && microphone.empty() && system.empty(),
        L"com as duas fontes em dia, o áudio é a média das duas");

    microphone = pcmFrames(2'000, 1'000);
    system = pcmFrames(500, 3'000);
    expect(AudioCapture::mixPending(microphone, true, system, true, skew, pcm) == 500 &&
            microphone.size() / AudioCapture::kBlockAlignment == 1'500 && system.empty(),
        L"uma fonte um pouco atrasada é aguardada para manter o alinhamento");

    // The reported bug: system audio sends nothing while the PC is silent.
    microphone = pcmFrames(skew + 2'400, 1'000);
    system.clear();
    const auto mixed = AudioCapture::mixPending(microphone, true, system, true, skew, pcm);
    expect(mixed == 2'400 && allSamplesEqual(pcm, 500) &&
            microphone.size() / AudioCapture::kBlockAlignment == skew,
        L"o microfone não fica preso quando o áudio do PC para de chegar");

    microphone.clear();
    for (int read = 0; read < 100; ++read) {
        const auto packet = pcmFrames(480, 1'000);
        microphone.insert(microphone.end(), packet.begin(), packet.end());
        AudioCapture::mixPending(microphone, true, system, true, skew, pcm);
    }
    expect(microphone.size() / AudioCapture::kBlockAlignment <= skew,
        L"o áudio pendente não cresce sem limite com uma fonte parada");

    microphone = pcmFrames(100, 1'000);
    system = pcmFrames(100, 1'000);
    expect(AudioCapture::mixPending(microphone, false, system, false, skew, pcm) == 0 && pcm.empty(),
        L"sem fontes ativas nada é misturado");
}

void testCapabilityProbes() {
    const auto nvenc = fastrecord::recording::probeNvenc();
    const auto amf = fastrecord::recording::probeAmf();
    const auto qsv = fastrecord::recording::probeQsv();
    expect(!nvenc.description.empty(), L"a sonda NVENC sempre retorna uma descrição");
    expect(!amf.description.empty(), L"a sonda AMF sempre retorna uma descrição");
    expect(!qsv.description.empty(), L"a sonda Intel QSV sempre retorna uma descrição");
    expect(!qsv.h264Supported || (qsv.hardwareMftFound && qsv.deviceReady),
        L"QSV só é anunciado quando há MFT Intel e dispositivo Intel pronto");
    expect(!nvenc.av1Supported || nvenc.encodeSessionOpened,
        L"AV1 só é anunciado pela sonda NVENC com uma sessão aberta");
    expect(!nvenc.h264Supported || nvenc.encodeSessionOpened,
        L"H.264 só é anunciado pela sonda NVENC com uma sessão aberta");
    expect(!amf.h264Supported || amf.contextInitialized,
        L"H.264 só é anunciado pela sonda AMF com contexto inicializado");
    expect(!amf.av1Supported || amf.contextInitialized,
        L"AV1 só é anunciado pela sonda AMF com contexto inicializado");
}

void testTrayPulseFrames() {
    const HICON baseIcon = LoadIconW(nullptr, IDI_APPLICATION);
    const HICON compactPulse = fastrecord::platform::TrayIcon::createPulseFrame(baseIcon, false);
    const HICON expandedPulse = fastrecord::platform::TrayIcon::createPulseFrame(baseIcon, true);
    expect(compactPulse != nullptr && expandedPulse != nullptr,
        L"os dois quadros pulsantes do ícone da bandeja são criados");

    if (compactPulse != nullptr && expandedPulse != nullptr) {
        const auto compactPixels = iconColorPixels(compactPulse);
        const auto expandedPixels = iconColorPixels(expandedPulse);
        expect(!compactPixels.empty() && !expandedPixels.empty() && compactPixels != expandedPixels,
            L"os quadros do ícone têm aparência diferente para produzir a pulsação");
    }
    if (compactPulse != nullptr) {
        DestroyIcon(compactPulse);
    }
    if (expandedPulse != nullptr) {
        DestroyIcon(expandedPulse);
    }
}

void testAmfBackendFailurePath() {
    auto settings = testRecordingSettings();
    settings.outputDirectory = std::filesystem::temp_directory_path() / L"FastRecordAmfTest";

    {
        fastrecord::recording::AmfBackend backend;
        auto unknownCodec = settings;
        unknownCodec.codec = static_cast<fastrecord::recording::VideoCodec>(99);
        std::wstring error;
        expect(!backend.start(unknownCodec, error) && !error.empty(),
            L"o backend AMF recusa um codec desconhecido com diagnóstico");
    }

    const auto capability = fastrecord::recording::probeAmf();
    if (capability.h264Supported && !capability.av1Supported) {
        fastrecord::recording::AmfBackend backend;
        auto av1 = settings;
        av1.codec = fastrecord::recording::VideoCodec::Av1;
        std::wstring error;
        expect(!backend.start(av1, error) && error.find(L"AV1") != std::wstring::npos,
            L"AV1 pelo AMF falha com diagnóstico em GPUs AMD sem encoder AV1");
    }
    if (capability.h264Supported) {
        return;
    }

    fastrecord::recording::AmfBackend backend;
    std::wstring error;
    expect(!backend.start(settings, error) && !error.empty(),
        L"a seleção AMF falha com diagnóstico quando o runtime/encoder AMD não existe");
}

// AV1 sequence header OBU produced by an RX 9070 XT (AMF, 1920x1080, profile
// 0, level 5.2), as stored at the start of its first key frame.
const std::vector<std::uint8_t> kAv1SequenceHeaderObu{
    0x0a, 0x98, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x7b, 0x00, 0x00,
    0x0e, 0x55, 0x77, 0xf8, 0x6f, 0xd0, 0x02, 0x7d, 0x10, 0x10, 0x10, 0x10, 0x40,
};

void testAv1Bitstream() {
    using fastrecord::recording::extractAv1CodecConfiguration;
    using fastrecord::recording::removeAv1TemporalDelimiters;

    const std::vector<std::uint8_t> temporalDelimiter{0x12, 0x00};
    const std::vector<std::uint8_t> frame{0x32, 0x02, 0xaa, 0xbb};
    std::vector<std::uint8_t> temporalUnit = temporalDelimiter;
    temporalUnit.insert(temporalUnit.end(), kAv1SequenceHeaderObu.begin(), kAv1SequenceHeaderObu.end());
    temporalUnit.insert(temporalUnit.end(), frame.begin(), frame.end());

    std::vector<std::uint8_t> expectedSample = kAv1SequenceHeaderObu;
    expectedSample.insert(expectedSample.end(), frame.begin(), frame.end());
    std::vector<std::uint8_t> sample;
    expect(removeAv1TemporalDelimiters(temporalUnit, sample) && sample == expectedSample,
        L"os temporal delimiters saem da amostra AV1 e os demais OBUs ficam intactos");
    expect(!removeAv1TemporalDelimiters({0x80, 0x00}, sample) && sample.empty(),
        L"um OBU com o bit proibido ligado é recusado");
    expect(!removeAv1TemporalDelimiters(temporalDelimiter, sample),
        L"uma amostra só com temporal delimiter não gera frame vazio");

    std::vector<std::uint8_t> configuration;
    expect(extractAv1CodecConfiguration(expectedSample, configuration) && configuration.size() == 4 +
            kAv1SequenceHeaderObu.size(),
        L"o av1C é montado a partir do cabeçalho de sequência real do AMF");
    if (configuration.size() == 4 + kAv1SequenceHeaderObu.size()) {
        expect(configuration[0] == 0x81 && configuration[1] == 0x0e && configuration[2] == 0x0c &&
                configuration[3] == 0x00,
            L"o av1C declara perfil 0, nível 5.2, 8 bits e 4:2:0");
        expect(std::equal(kAv1SequenceHeaderObu.begin(), kAv1SequenceHeaderObu.end(), configuration.begin() + 4),
            L"o av1C carrega o OBU de cabeçalho de sequência original");
    }
    expect(!extractAv1CodecConfiguration(frame, configuration),
        L"sem cabeçalho de sequência não há av1C");
}

std::vector<std::uint32_t> readSyncSamples(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    const std::vector<std::uint8_t> data{
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const auto readU32 = [&](size_t offset) {
        return (static_cast<std::uint32_t>(data[offset]) << 24) |
            (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
            (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
            static_cast<std::uint32_t>(data[offset + 3]);
    };
    std::vector<std::uint32_t> samples;
    for (size_t index = 4; index + 12 <= data.size(); ++index) {
        if (data[index] == 's' && data[index + 1] == 't' && data[index + 2] == 's' && data[index + 3] == 's') {
            const auto count = readU32(index + 8);
            for (std::uint32_t entry = 0; entry < count && index + 16 + entry * 4 <= data.size(); ++entry) {
                samples.push_back(readU32(index + 12 + entry * 4));
            }
            break;
        }
    }
    return samples;
}

void testMp4WriterSyncSamples() {
    using fastrecord::recording::Mp4Writer;
    using fastrecord::recording::VideoCodec;
    const auto directory = std::filesystem::temp_directory_path() /
        (L"FastRecordMp4Test_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);

    // AV1: the encoder's key frame report decides the sync samples.
    std::vector<std::uint8_t> keyFrame = kAv1SequenceHeaderObu;
    keyFrame.insert(keyFrame.end(), {0x32, 0x02, 0xaa, 0xbb});
    const std::vector<std::uint8_t> interFrame{0x32, 0x02, 0xcc, 0xdd};
    const auto av1Path = directory / L"av1.mp4";
    {
        Mp4Writer writer;
        std::wstring error;
        bool written = writer.start(av1Path, 1920, 1080, 30, 12'000'000, VideoCodec::Av1, keyFrame, false, error);
        written = written && writer.write(keyFrame, 0, error, true);
        written = written && writer.write(interFrame, 1, error, false);
        written = written && writer.write(interFrame, 2, error, false);
        written = written && writer.write(keyFrame, 3, error, true);
        written = written && writer.finalize(error);
        expect(written, L"o Mp4Writer grava uma faixa AV1 com dicas de key frame");
    }
    expect(readSyncSamples(av1Path) == std::vector<std::uint32_t>{1, 4},
        L"os key frames AV1 informados pelo encoder viram sync samples");

    // H.264 without hints: IDR NAL units are detected from the bitstream.
    const std::vector<std::uint8_t> sps{0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x28, 0xac};
    const std::vector<std::uint8_t> pps{0x00, 0x00, 0x00, 0x01, 0x68, 0xee, 0x3c, 0x80};
    std::vector<std::uint8_t> idr = sps;
    idr.insert(idr.end(), pps.begin(), pps.end());
    idr.insert(idr.end(), {0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84});
    const std::vector<std::uint8_t> predicted{0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x02};
    const auto h264Path = directory / L"h264.mp4";
    {
        Mp4Writer writer;
        std::wstring error;
        bool written = writer.start(h264Path, 1280, 720, 30, 12'000'000, VideoCodec::H264, idr, false, error);
        written = written && writer.write(idr, 0, error);
        written = written && writer.write(predicted, 1, error);
        written = written && writer.write(idr, 2, error);
        written = written && writer.finalize(error);
        expect(written, L"o Mp4Writer grava uma faixa H.264 sem dicas de key frame");
    }
    expect(readSyncSamples(h264Path) == std::vector<std::uint32_t>{1, 3},
        L"os IDR do H.264 viram sync samples sem depender do encoder");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

void testUpdateVersionComparison() {
    using fastrecord::updates::isNewerVersion;
    expect(isNewerVersion(L"0.1.12", L"v0.1.13"),
        L"o comparador reconhece uma release mais recente com prefixo v");
    expect(isNewerVersion(L"0.1.12", L"0.1.100"),
        L"o comparador trata componentes como números e não como texto");
    expect(!isNewerVersion(L"v0.1.13", L"0.1.13.0"),
        L"componentes adicionais zerados não tornam a mesma versão mais nova");
    expect(!isNewerVersion(L"0.1.13", L"0.1.12"),
        L"o comparador não oferece uma versão anterior para atualização");
    expect(!isNewerVersion(L"0.1.invalid", L"0.1.14"),
        L"versões locais fora do formato numérico não são comparadas");
}

void testUpdateReleaseMetadata() {
    constexpr std::wstring_view releaseJson = LR"({
        "tag_name":"v0.1.13",
        "assets":[{
            "name":"FastRecord-0.1.13-x64-Setup.exe",
            "browser_download_url":"https://github.com/srviniaviz/fast-recorder/releases/download/v0.1.13/FastRecord-0.1.13-x64-Setup.exe",
            "digest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "size":1024
        }]
    })";

    const auto update = fastrecord::updates::parseLatestReleaseJson(releaseJson, L"0.1.12");
    expect(update.success && update.updateAvailable && update.version == L"0.1.13",
        L"uma release válida mais nova aparece como atualização disponível");
    expect(update.downloadUrl.find(L"https://github.com/srviniaviz/fast-recorder/releases/download/") == 0 &&
            update.sha256 == std::wstring(64, L'a') && update.sizeBytes == 1024,
        L"metadados validados preservam URL, SHA-256 e tamanho do setup esperado");

    const auto current = fastrecord::updates::parseLatestReleaseJson(releaseJson, L"0.1.13");
    expect(current.success && !current.updateAvailable,
        L"a versão instalada não é marcada como atualização");

    std::wstring wrongUrl(releaseJson);
    const auto githubHost = wrongUrl.find(L"github.com");
    wrongUrl.replace(githubHost, 10, L"attacker.invalid");
    const auto rejectedUrl = fastrecord::updates::parseLatestReleaseJson(wrongUrl, L"0.1.12");
    expect(!rejectedUrl.success && !rejectedUrl.error.empty(),
        L"um instalador hospedado fora do endereço oficial é recusado");

    std::wstring missingDigest(releaseJson);
    const auto digestField = missingDigest.find(L"\"digest\":\"");
    const auto digestEnd = missingDigest.find(L'\"', digestField + 10);
    missingDigest.erase(digestField, digestEnd + 1 - digestField);
    const auto rejectedDigest = fastrecord::updates::parseLatestReleaseJson(missingDigest, L"0.1.12");
    expect(!rejectedDigest.success && !rejectedDigest.error.empty(),
        L"uma release sem digest SHA-256 não oferece instalador");
}

void testUiContract() {
    const std::wstring page(fastrecord::ui::kAppPage);
    const std::array<const wchar_t*, 33> requiredFragments{
        L"id=\"record\"",
        L"id=\"pause\"",
        L"id=\"stop\"",
        L"id=\"mic\"",
        L"id=\"systemAudio\"",
        L"id=\"areaSelect\"",
        L"id=\"engineSelect\"",
        L"id=\"codecSelect\"",
        L"id=\"resolutionSelect\"",
        L"id=\"fpsSelect\"",
        L"id=\"bitrate\"",
        L"id=\"startWithWindows\"",
        L"data-tab=\"files\"",
        L"data-tab=\"actions\"",
        L"data-tab=\"settings\"",
        L"send('record')",
        L"send('pause')",
        L"send('stop')",
        L"send('toggle-mic')",
        L"send('toggle-system-audio')",
        L"send('toggle-start-with-windows')",
        L"send('refresh-recordings')",
        L"open-recording:",
        L"show-recording:",
        L"elapsedSeconds",
        L"AV1 · menor tamanho",
        L"120 FPS",
        L"QSV · Intel",
        L"id=\"updateStatus\"",
        L"id=\"updateAction\"",
        L"check-updates",
        L"download-update",
        L"has-update",
    };

    for (const auto* fragment : requiredFragments) {
        expect(page.find(fragment) != std::wstring::npos, fragment);
    }

    expect(page.find(L"Ctrl +") == std::wstring::npos &&
            page.find(L"class=\"shortcut\"") == std::wstring::npos &&
            page.find(L"class=\"hotkey\"") == std::wstring::npos,
        L"a interface não anuncia atalhos de teclado para gravação");
    expect(page.find(L"Gravação rápida") == std::wstring::npos,
        L"a aba de ações rápidas não oferece gravação por atalho");
}

} // namespace

int wmain() {
    std::wcout << L"Executando testes unitários do Fast Record...\n";
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        std::wcerr << L"Não foi possível inicializar COM para os testes.\n";
        return 1;
    }
    testSettingsPersistence();
    testRecorderStateMachine();
    testAudioContract();
    testAudioMixer();
    testCapabilityProbes();
    testTrayPulseFrames();
    testAmfBackendFailurePath();
    testAv1Bitstream();
    testMp4WriterSyncSamples();
    testUpdateVersionComparison();
    testUpdateReleaseMetadata();
    testUiContract();

    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }

    if (g_failures == 0) {
        std::wcout << L"OK: todos os testes passaram.\n";
        return 0;
    }

    std::wcerr << L"Falhas: " << g_failures << L"\n";
    return 1;
}
