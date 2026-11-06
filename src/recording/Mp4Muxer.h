#pragma once

#include "recording/RecordingBackend.h"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fastrecord::recording {

// MP4 muxing shared by the backends that receive an already encoded
// bitstream from the GPU (NVENC and AMF). Media Foundation is only used for
// the AAC encoder; the container itself is written here.

// Collects the SPS/PPS NAL units of an H.264 access unit as Annex B. Accepts
// Annex B or 4-byte length-prefixed input.
bool extractH264SequenceHeader(
    const std::vector<std::uint8_t>& encoded,
    std::vector<std::uint8_t>& sequenceHeader);

// Builds the AV1CodecConfigurationRecord (the av1C box payload) from the first
// sequence header OBU found in a low-overhead AV1 bitstream.
bool extractAv1CodecConfiguration(
    const std::vector<std::uint8_t>& encoded,
    std::vector<std::uint8_t>& configuration);

// Copies an AV1 temporal unit without its temporal delimiter OBUs. The AV1
// ISOBMFF binding says MP4 samples should not carry them.
bool removeAv1TemporalDelimiters(
    const std::vector<std::uint8_t>& encoded,
    std::vector<std::uint8_t>& sample);

struct EncodedAudioPacket {
    std::vector<std::uint8_t> bytes;
    std::uint32_t durationFrames{1024};
};

// Media Foundation AAC-LC encoder for the 48 kHz stereo PCM produced by
// AudioCapture. Requires COM and MFStartup on the calling thread.
class AacEncoder final {
public:
    AacEncoder();
    ~AacEncoder();
    AacEncoder(const AacEncoder&) = delete;
    AacEncoder& operator=(const AacEncoder&) = delete;

    bool initialize(std::wstring& error);
    bool encode(
        const std::vector<std::uint8_t>& pcm,
        std::uint32_t frameCount,
        std::vector<EncodedAudioPacket>& packets,
        std::wstring& error);
    bool drain(std::vector<EncodedAudioPacket>& packets, std::wstring& error);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// Writes a constant frame rate MP4 with one H.264 or AV1 video track and an
// optional AAC track. Every video sample lasts exactly 1/fps.
class Mp4Writer final {
public:
    Mp4Writer();
    ~Mp4Writer();
    Mp4Writer(const Mp4Writer&) = delete;
    Mp4Writer& operator=(const Mp4Writer&) = delete;

    // sequenceHeader: SPS/PPS as Annex B for H.264, or any AV1 data that
    // contains the sequence header OBU.
    bool start(
        const std::filesystem::path& output,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t fps,
        std::uint32_t bitrate,
        VideoCodec codec,
        const std::vector<std::uint8_t>& sequenceHeader,
        bool audioEnabled,
        std::wstring& error);
    bool writeAudio(const EncodedAudioPacket& encoded, std::wstring& error);
    // keyFrame: what the encoder reported for this packet. When absent, H.264
    // key frames are detected from the IDR NAL units and, for AV1, only the
    // first sample is marked as a sync sample.
    bool write(
        const std::vector<std::uint8_t>& encoded,
        LONGLONG timestamp,
        std::wstring& error,
        std::optional<bool> keyFrame = std::nullopt);
    bool finalize(std::wstring& error);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fastrecord::recording
