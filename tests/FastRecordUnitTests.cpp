#include "app/AppSettings.h"
#include "app/UpdateService.h"
#include "recording/AmfBackend.h"
#include "recording/AmfProbe.h"
#include "recording/AudioCapture.h"
#include "recording/NvencProbe.h"
#include "recording/QsvProbe.h"
#include "recording/RecorderController.h"
#include "ui/AppPage.h"

#include <Windows.h>

#include <array>
#include <filesystem>
#include <iostream>
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
            av1Result.message.find(L"NVENC") != std::wstring::npos,
        L"AV1 sem NVENC é recusado antes de iniciar o backend");
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
}

void testAmfBackendFailurePath() {
    const auto capability = fastrecord::recording::probeAmf();
    if (capability.h264Supported) {
        return;
    }

    fastrecord::recording::AmfBackend backend;
    auto settings = testRecordingSettings();
    settings.outputDirectory = std::filesystem::temp_directory_path() / L"FastRecordAmfTest";
    std::wstring error;
    expect(!backend.start(settings, error) && !error.empty(),
        L"a seleção AMF falha com diagnóstico quando o runtime/encoder AMD não existe");
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
    testCapabilityProbes();
    testAmfBackendFailurePath();
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
