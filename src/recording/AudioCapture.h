#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fastrecord::recording {

class AudioCapture final {
public:
    static constexpr std::uint32_t kSampleRate = 48'000;
    static constexpr std::uint16_t kChannels = 2;
    static constexpr std::uint16_t kBitsPerSample = 16;
    static constexpr std::uint16_t kBlockAlignment =
        kChannels * (kBitsPerSample / 8);
    // How far one source may run ahead of the other before the late one is
    // mixed as silence instead of holding the output back.
    static constexpr std::uint32_t kMaxSourceSkewFrames = kSampleRate / 5; // 200 ms

    // Mixes the pending PCM of two sources (16-bit stereo, kBlockAlignment
    // bytes per frame) into pcm and removes what was mixed from them. With
    // both sources enabled the output waits for the slower one, unless it
    // falls more than maxSkewFrames behind; then its missing part is silence.
    // Returns the number of frames written to pcm.
    static std::uint32_t mixPending(
        std::vector<std::uint8_t>& first,
        bool firstEnabled,
        std::vector<std::uint8_t>& second,
        bool secondEnabled,
        std::uint32_t maxSkewFrames,
        std::vector<std::uint8_t>& pcm);

    AudioCapture();
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    bool start(bool captureMicrophone, bool captureSystemAudio, std::wstring& error);
    bool read(std::vector<std::uint8_t>& pcm, std::uint32_t& frameCount, std::wstring& error);
    bool discard(std::wstring& error);
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fastrecord::recording
