#include "recording/AudioCapture.h"

#include <Windows.h>
#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace fastrecord::recording {

namespace {

using Microsoft::WRL::ComPtr;

std::wstring audioError(const wchar_t* operation, HRESULT result) {
    wchar_t message[128]{};
    swprintf_s(
        message,
        _countof(message),
        L"%s (HRESULT 0x%08X).",
        operation,
        static_cast<unsigned>(result));
    return message;
}

WAVEFORMATEX captureFormat() {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = AudioCapture::kChannels;
    format.nSamplesPerSec = AudioCapture::kSampleRate;
    format.wBitsPerSample = AudioCapture::kBitsPerSample;
    format.nBlockAlign = AudioCapture::kBlockAlignment;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    return format;
}

} // namespace

// WASAPI loopback only delivers packets while the render endpoint is mixing
// audio. With nothing playing it delivers nothing at all, so silence would
// vanish from the timeline: a sound played 10 s into the recording would land
// at 0 s, and the microphone mix would wait for system audio forever.
// Rendering silence to the same endpoint keeps it mixing, and the loopback
// stream then reports quiet periods as silent packets.
struct SilentRenderer {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    UINT32 bufferFrames{0};
    bool started{false};

    void stop() noexcept {
        if (started && client) client->Stop();
        started = false;
        render.Reset();
        client.Reset();
    }

    bool start(IMMDevice* device) {
        stop();
        HRESULT result = device->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(client.GetAddressOf()));
        WAVEFORMATEX* mixFormat = nullptr;
        if (SUCCEEDED(result)) result = client->GetMixFormat(&mixFormat);
        // One second of buffer: read() tops it up every few milliseconds, and
        // a stalled recording thread still has plenty of margin.
        constexpr REFERENCE_TIME bufferDuration = 10'000'000;
        if (SUCCEEDED(result)) {
            result = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0, mixFormat, nullptr);
        }
        CoTaskMemFree(mixFormat);
        if (SUCCEEDED(result)) result = client->GetBufferSize(&bufferFrames);
        if (SUCCEEDED(result)) result = client->GetService(IID_PPV_ARGS(&render));
        if (SUCCEEDED(result) && !fill()) result = E_FAIL;
        if (SUCCEEDED(result)) result = client->Start();
        if (FAILED(result)) {
            stop();
            return false;
        }
        started = true;
        return true;
    }

    // Queues silence for all the free space in the render buffer.
    bool fill() noexcept {
        if (!client || !render) return false;
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) return false;
        const UINT32 available = bufferFrames - (std::min)(padding, bufferFrames);
        if (available == 0) return true;
        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(available, &data))) return false;
        return SUCCEEDED(render->ReleaseBuffer(available, AUDCLNT_BUFFERFLAGS_SILENT));
    }
};

struct AudioSource {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    SilentRenderer silence;
    std::wstring label;
    bool started{false};

    void stop() noexcept {
        if (started && client) client->Stop();
        started = false;
        silence.stop();
        capture.Reset();
        client.Reset();
        device.Reset();
    }

    bool start(
        IMMDeviceEnumerator* enumerator,
        EDataFlow flow,
        ERole role,
        bool loopback,
        const wchar_t* sourceLabel,
        std::wstring& error) {
        stop();
        label = sourceLabel;
        HRESULT result = enumerator->GetDefaultAudioEndpoint(flow, role, &device);
        if (FAILED(result)) {
            error = result == E_NOTFOUND
                ? std::wstring(L"Nenhum dispositivo padrão foi encontrado para ") + label + L"."
                : audioError((std::wstring(L"Não foi possível abrir ") + label).c_str(), result);
            return false;
        }

        result = device->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(result)) {
            error = audioError((std::wstring(L"Não foi possível inicializar ") + label).c_str(), result);
            return false;
        }

        auto format = captureFormat();
        constexpr REFERENCE_TIME bufferDuration = 2'000'000;
        DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        result = client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            flags,
            bufferDuration,
            0,
            &format,
            nullptr);
        if (FAILED(result)) {
            error = audioError((std::wstring(L"O formato de áudio não pôde ser preparado para ") + label).c_str(), result);
            return false;
        }

        result = client->GetService(IID_PPV_ARGS(&capture));
        // Best effort: without it loopback still captures whatever plays, and
        // the mixer keeps the microphone flowing on its own.
        if (SUCCEEDED(result) && loopback) silence.start(device.Get());
        if (SUCCEEDED(result)) result = client->Start();
        if (FAILED(result)) {
            error = audioError((std::wstring(L"Não foi possível iniciar ") + label).c_str(), result);
            return false;
        }
        started = true;
        return true;
    }

    bool read(
        std::vector<std::uint8_t>& pcm,
        std::uint32_t& frameCount,
        std::wstring& error) {
        pcm.clear();
        frameCount = 0;
        if (!started || !capture) return true;
        if (silence.started) silence.fill();

        UINT32 packetFrames = 0;
        HRESULT result = capture->GetNextPacketSize(&packetFrames);
        while (SUCCEEDED(result) && packetFrames > 0) {
            BYTE* source = nullptr;
            DWORD flags = 0;
            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;
            result = capture->GetBuffer(
                &source,
                &packetFrames,
                &flags,
                &devicePosition,
                &qpcPosition);
            if (FAILED(result)) break;

            if (packetFrames > (std::numeric_limits<std::uint32_t>::max)() - frameCount) {
                capture->ReleaseBuffer(packetFrames);
                error = L"Um buffer de áudio ficou grande demais.";
                return false;
            }
            const size_t packetBytes = static_cast<size_t>(packetFrames) * AudioCapture::kBlockAlignment;
            const size_t previousSize = pcm.size();
            pcm.resize(previousSize + packetBytes);
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || source == nullptr) {
                std::memset(pcm.data() + previousSize, 0, packetBytes);
            } else {
                std::memcpy(pcm.data() + previousSize, source, packetBytes);
            }
            frameCount += packetFrames;
            capture->ReleaseBuffer(packetFrames);
            result = capture->GetNextPacketSize(&packetFrames);
        }

        if (FAILED(result)) {
            error = audioError((label + L" parou de responder").c_str(), result);
            return false;
        }
        return true;
    }
};

struct AudioCapture::Impl {
    AudioSource microphone;
    AudioSource systemAudio;
    bool microphoneEnabled{false};
    bool systemAudioEnabled{false};
    bool started{false};
    std::vector<std::uint8_t> microphonePending;
    std::vector<std::uint8_t> systemAudioPending;
};

AudioCapture::AudioCapture()
    : m_impl(std::make_unique<Impl>()) {}

AudioCapture::~AudioCapture() {
    stop();
}

bool AudioCapture::start(bool captureMicrophone, bool captureSystemAudio, std::wstring& error) {
    stop();
    m_impl = std::make_unique<Impl>();

    if (!captureMicrophone && !captureSystemAudio) {
        error = L"Nenhuma fonte de áudio foi selecionada.";
        return false;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) {
        error = audioError(L"Não foi possível acessar os dispositivos de áudio", result);
        return false;
    }

    if (captureMicrophone &&
        !m_impl->microphone.start(enumerator.Get(), eCapture, eCommunications, false, L"o microfone", error) &&
        !m_impl->microphone.start(enumerator.Get(), eCapture, eConsole, false, L"o microfone", error)) {
        stop();
        return false;
    }
    if (captureSystemAudio &&
        !m_impl->systemAudio.start(enumerator.Get(), eRender, eConsole, true, L"o áudio do PC", error)) {
        stop();
        return false;
    }

    m_impl->microphoneEnabled = captureMicrophone;
    m_impl->systemAudioEnabled = captureSystemAudio;
    m_impl->started = true;

    // Starting the second WASAPI client can take long enough for the first
    // one to accumulate stale samples. Both sources must begin the timeline
    // from the same point or the MP4 starts with an audio-only offset.
    std::vector<std::uint8_t> startupBuffer;
    std::uint32_t startupFrames = 0;
    if (captureMicrophone &&
        !m_impl->microphone.read(startupBuffer, startupFrames, error)) {
        stop();
        return false;
    }
    if (captureSystemAudio &&
        !m_impl->systemAudio.read(startupBuffer, startupFrames, error)) {
        stop();
        return false;
    }
    error.clear();
    return true;
}

std::uint32_t AudioCapture::mixPending(
    std::vector<std::uint8_t>& first,
    bool firstEnabled,
    std::vector<std::uint8_t>& second,
    bool secondEnabled,
    std::uint32_t maxSkewFrames,
    std::vector<std::uint8_t>& pcm) {
    const auto framesIn = [](const std::vector<std::uint8_t>& pending) {
        return static_cast<std::uint32_t>(pending.size() / kBlockAlignment);
    };
    const std::uint32_t firstFrames = firstEnabled ? framesIn(first) : 0;
    const std::uint32_t secondFrames = secondEnabled ? framesIn(second) : 0;

    std::uint32_t frameCount = 0;
    if (firstEnabled && secondEnabled) {
        const auto shorter = (std::min)(firstFrames, secondFrames);
        const auto longer = (std::max)(firstFrames, secondFrames);
        // Waiting keeps both sources aligned; giving up after maxSkewFrames
        // keeps a stalled source from silencing the other one.
        frameCount = longer - shorter > maxSkewFrames ? longer - maxSkewFrames : shorter;
    } else {
        frameCount = firstEnabled ? firstFrames : secondFrames;
    }

    const size_t mixedBytes = static_cast<size_t>(frameCount) * kBlockAlignment;
    pcm.assign(mixedBytes, 0);
    const int sourceCount = static_cast<int>(firstEnabled) + static_cast<int>(secondEnabled);
    const auto sampleAt = [](const std::vector<std::uint8_t>& pending, size_t index) {
        std::int16_t sample = 0;
        if ((index + 1) * sizeof(sample) <= pending.size()) {
            std::memcpy(&sample, pending.data() + index * sizeof(sample), sizeof(sample));
        }
        return static_cast<int>(sample);
    };
    const size_t sampleCount = static_cast<size_t>(frameCount) * kChannels;
    for (size_t index = 0; index < sampleCount; ++index) {
        const int firstSample = firstEnabled ? sampleAt(first, index) : 0;
        const int secondSample = secondEnabled ? sampleAt(second, index) : 0;
        const auto mixed = static_cast<std::int16_t>((firstSample + secondSample) / (std::max)(1, sourceCount));
        std::memcpy(pcm.data() + index * sizeof(mixed), &mixed, sizeof(mixed));
    }

    const auto consume = [mixedBytes](std::vector<std::uint8_t>& pending) {
        const auto bytes = (std::min)(mixedBytes, pending.size());
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(bytes));
    };
    if (firstEnabled) consume(first);
    if (secondEnabled) consume(second);
    return frameCount;
}

bool AudioCapture::read(
    std::vector<std::uint8_t>& pcm,
    std::uint32_t& frameCount,
    std::wstring& error) {
    pcm.clear();
    frameCount = 0;
    if (!m_impl || !m_impl->started) {
        error = L"A captura de áudio não está ativa.";
        return false;
    }

    std::vector<std::uint8_t> microphonePcm;
    std::vector<std::uint8_t> systemPcm;
    std::uint32_t microphoneFrames = 0;
    std::uint32_t systemFrames = 0;
    if (m_impl->microphoneEnabled &&
        !m_impl->microphone.read(microphonePcm, microphoneFrames, error)) return false;
    if (m_impl->systemAudioEnabled &&
        !m_impl->systemAudio.read(systemPcm, systemFrames, error)) return false;

    m_impl->microphonePending.insert(
        m_impl->microphonePending.end(), microphonePcm.begin(), microphonePcm.end());
    m_impl->systemAudioPending.insert(
        m_impl->systemAudioPending.end(), systemPcm.begin(), systemPcm.end());

    frameCount = mixPending(
        m_impl->microphonePending,
        m_impl->microphoneEnabled,
        m_impl->systemAudioPending,
        m_impl->systemAudioEnabled,
        kMaxSourceSkewFrames,
        pcm);
    error.clear();
    return true;
}

bool AudioCapture::discard(std::wstring& error) {
    std::vector<std::uint8_t> ignored;
    std::uint32_t ignoredFrames = 0;
    const bool result = read(ignored, ignoredFrames, error);
    if (m_impl) {
        m_impl->microphonePending.clear();
        m_impl->systemAudioPending.clear();
    }
    return result;
}

void AudioCapture::stop() noexcept {
    if (m_impl) {
        m_impl->microphone.stop();
        m_impl->systemAudio.stop();
        m_impl->started = false;
        m_impl->microphonePending.clear();
        m_impl->systemAudioPending.clear();
    }
}

} // namespace fastrecord::recording
