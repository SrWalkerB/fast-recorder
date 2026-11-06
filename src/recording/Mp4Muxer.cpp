#include "recording/Mp4Muxer.h"

#include "recording/AudioCapture.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace fastrecord::recording {

namespace {

using Microsoft::WRL::ComPtr;

constexpr LONGLONG kHundredNanosecondsPerSecond = 10'000'000;
constexpr std::uint32_t kAudioBitrate = 192'000;

size_t findStartCode(const std::vector<std::uint8_t>& data, size_t offset, size_t& codeSize) {
    for (size_t index = offset; index + 3 <= data.size(); ++index) {
        if (data[index] != 0 || data[index + 1] != 0) {
            continue;
        }
        if (data[index + 2] == 1) {
            codeSize = 3;
            return index;
        }
        if (index + 4 <= data.size() && data[index + 2] == 0 && data[index + 3] == 1) {
            codeSize = 4;
            return index;
        }
    }
    codeSize = 0;
    return std::numeric_limits<size_t>::max();
}

bool normalizeH264ToAnnexB(
    const std::vector<std::uint8_t>& encoded,
    std::vector<std::uint8_t>& packet) {
    packet.clear();
    if (encoded.empty()) {
        return false;
    }

    size_t codeSize = 0;
    size_t start = findStartCode(encoded, 0, codeSize);
    if (start != std::numeric_limits<size_t>::max()) {
        packet = encoded;
        return true;
    }

    // Keep compatibility with drivers that return AVCC/length-prefixed output.
    for (size_t offset = 0; offset + sizeof(DWORD) <= encoded.size();) {
        const DWORD nalSize =
            (static_cast<DWORD>(encoded[offset]) << 24) |
            (static_cast<DWORD>(encoded[offset + 1]) << 16) |
            (static_cast<DWORD>(encoded[offset + 2]) << 8) |
            static_cast<DWORD>(encoded[offset + 3]);
        offset += sizeof(DWORD);
        if (nalSize == 0 || nalSize > encoded.size() - offset) {
            packet.clear();
            return false;
        }

        packet.insert(packet.end(), {0, 0, 0, 1});
        packet.insert(packet.end(), encoded.begin() + static_cast<std::ptrdiff_t>(offset),
            encoded.begin() + static_cast<std::ptrdiff_t>(offset + nalSize));
        offset += nalSize;
    }

    return !packet.empty();
}

bool normalizeH264ToLengthPrefixed(
    const std::vector<std::uint8_t>& encoded,
    std::vector<std::uint8_t>& packet) {
    packet.clear();
    if (encoded.empty()) {
        return false;
    }

    size_t codeSize = 0;
    size_t start = findStartCode(encoded, 0, codeSize);
    if (start == std::numeric_limits<size_t>::max()) {
        packet = encoded;
        return true;
    }

    while (start != std::numeric_limits<size_t>::max()) {
        const size_t nalStart = start + codeSize;
        size_t nextCodeSize = 0;
        const size_t next = findStartCode(encoded, nalStart, nextCodeSize);
        size_t nalEnd = next == std::numeric_limits<size_t>::max() ? encoded.size() : next;
        while (nalEnd > nalStart && encoded[nalEnd - 1] == 0) {
            --nalEnd;
        }

        if (nalEnd > nalStart) {
            const size_t nalSize = nalEnd - nalStart;
            if (nalSize > UINT32_MAX) {
                packet.clear();
                return false;
            }
            packet.push_back(static_cast<std::uint8_t>((nalSize >> 24) & 0xff));
            packet.push_back(static_cast<std::uint8_t>((nalSize >> 16) & 0xff));
            packet.push_back(static_cast<std::uint8_t>((nalSize >> 8) & 0xff));
            packet.push_back(static_cast<std::uint8_t>(nalSize & 0xff));
            packet.insert(packet.end(), encoded.begin() + static_cast<std::ptrdiff_t>(nalStart),
                encoded.begin() + static_cast<std::ptrdiff_t>(nalEnd));
        }

        start = next;
        codeSize = nextCodeSize;
    }

    return !packet.empty();
}

struct Av1Obu {
    std::uint8_t type{0};
    size_t headerSize{0};
    size_t sizeFieldSize{0};
    size_t payloadOffset{0};
    size_t payloadSize{0};
};

bool readAv1Leb128(
    const std::vector<std::uint8_t>& data,
    size_t offset,
    std::uint64_t& value,
    size_t& bytesRead) {
    value = 0;
    bytesRead = 0;
    for (; bytesRead < 8 && offset + bytesRead < data.size(); ++bytesRead) {
        const std::uint8_t byte = data[offset + bytesRead];
        value |= static_cast<std::uint64_t>(byte & 0x7f) << (bytesRead * 7);
        if ((byte & 0x80) == 0) {
            ++bytesRead;
            return true;
        }
    }
    return false;
}

bool parseAv1Obu(
    const std::vector<std::uint8_t>& data,
    size_t offset,
    Av1Obu& obu,
    size_t& totalSize) {
    if (offset >= data.size()) {
        return false;
    }

    const std::uint8_t header = data[offset];
    if ((header & 0x80) != 0) {
        return false;
    }

    obu = {};
    obu.type = static_cast<std::uint8_t>((header >> 3) & 0x0f);
    obu.headerSize = 1 + ((header & 0x04) != 0 ? 1 : 0);
    if (offset + obu.headerSize > data.size()) {
        return false;
    }

    const bool hasSizeField = (header & 0x02) != 0;
    if (!hasSizeField) {
        obu.payloadOffset = offset + obu.headerSize;
        obu.payloadSize = data.size() - obu.payloadOffset;
        obu.sizeFieldSize = 0;
        totalSize = data.size() - offset;
        return obu.payloadSize > 0;
    }

    std::uint64_t payloadSize = 0;
    if (!readAv1Leb128(data, offset + obu.headerSize, payloadSize, obu.sizeFieldSize) ||
        payloadSize > data.size() - offset - obu.headerSize - obu.sizeFieldSize) {
        return false;
    }

    obu.payloadOffset = offset + obu.headerSize + obu.sizeFieldSize;
    obu.payloadSize = static_cast<size_t>(payloadSize);
    totalSize = obu.headerSize + obu.sizeFieldSize + obu.payloadSize;
    return totalSize > 0;
}

class Av1BitReader final {
public:
    Av1BitReader(const std::uint8_t* data, size_t size)
        : m_data(data), m_bitCount(size * 8) {}

    bool read(unsigned count, std::uint32_t& value) {
        if (count > 32 || m_position + count > m_bitCount) {
            return false;
        }

        value = 0;
        for (unsigned index = 0; index < count; ++index) {
            value = (value << 1) | ((m_data[(m_position + index) / 8] >> (7 - ((m_position + index) % 8))) & 1u);
        }
        m_position += count;
        return true;
    }

    bool skip(size_t count) {
        if (m_position + count > m_bitCount) {
            return false;
        }
        m_position += count;
        return true;
    }

    bool bit(bool& value) {
        std::uint32_t raw = 0;
        if (!read(1, raw)) {
            return false;
        }
        value = raw != 0;
        return true;
    }

    bool uvlc() {
        unsigned leadingZeros = 0;
        while (leadingZeros < 32) {
            bool value = false;
            if (!bit(value)) {
                return false;
            }
            if (value) {
                break;
            }
            ++leadingZeros;
        }
        return leadingZeros < 32 && skip(leadingZeros);
    }

private:
    const std::uint8_t* m_data{nullptr};
    size_t m_bitCount{0};
    size_t m_position{0};
};

struct Av1SequenceInfo {
    std::uint32_t profile{0};
    std::uint32_t level{0};
    std::uint32_t tier{0};
    std::uint32_t bitDepth{8};
    bool monochrome{false};
    bool chromaSubsamplingX{true};
    bool chromaSubsamplingY{true};
    std::uint32_t chromaSamplePosition{0};
};

bool parseAv1SequenceHeader(
    const std::vector<std::uint8_t>& payload,
    Av1SequenceInfo& info) {
    if (payload.empty()) {
        return false;
    }

    Av1BitReader bits(payload.data(), payload.size());
    std::uint32_t value = 0;
    if (!bits.read(3, info.profile) || !bits.skip(1)) {
        return false;
    }

    if (!bits.read(1, value)) {
        return false;
    }
    const bool reducedStillPictureHeader = value != 0;
    if (reducedStillPictureHeader) {
        if (!bits.read(5, info.level)) {
            return false;
        }
        info.tier = 0;
    } else {
        bool timingInfoPresent = false;
        if (!bits.bit(timingInfoPresent)) {
            return false;
        }

        bool decoderModelInfoPresent = false;
        unsigned bufferDelayLengthMinus1 = 0;
        if (timingInfoPresent) {
            if (!bits.skip(64)) {
                return false;
            }
            bool equalPictureInterval = false;
            if (!bits.bit(equalPictureInterval)) {
                return false;
            }
            if (equalPictureInterval && !bits.uvlc()) {
                return false;
            }
            if (!bits.bit(decoderModelInfoPresent)) {
                return false;
            }
            if (decoderModelInfoPresent) {
                if (!bits.read(5, value)) {
                    return false;
                }
                bufferDelayLengthMinus1 = static_cast<unsigned>(value);
                if (!bits.skip(42)) {
                    return false;
                }
            }
        }

        bool initialDisplayDelayPresent = false;
        std::uint32_t operatingPointCountMinus1 = 0;
        if (!bits.bit(initialDisplayDelayPresent) ||
            !bits.read(5, operatingPointCountMinus1)) {
            return false;
        }

        for (std::uint32_t index = 0; index <= operatingPointCountMinus1; ++index) {
            std::uint32_t level = 0;
            if (!bits.skip(12) || !bits.read(5, level)) {
                return false;
            }
            std::uint32_t tierValue = 0;
            if (level > 7) {
                bool tierBit = false;
                if (!bits.bit(tierBit)) {
                    return false;
                }
                tierValue = tierBit ? 1u : 0u;
            }

            if (decoderModelInfoPresent) {
                bool presentForOperation = false;
                if (!bits.bit(presentForOperation)) {
                    return false;
                }
                if (presentForOperation &&
                    (!bits.skip((bufferDelayLengthMinus1 + 1) * 2) || !bits.skip(1))) {
                    return false;
                }
            }

            if (initialDisplayDelayPresent) {
                bool presentForOperation = false;
                if (!bits.bit(presentForOperation)) {
                    return false;
                }
                if (presentForOperation && !bits.skip(4)) {
                    return false;
                }
            }
            if (index == 0) {
                info.level = level;
                info.tier = tierValue;
            }
        }
    }

    std::uint32_t frameWidthBitsMinus1 = 0;
    std::uint32_t frameHeightBitsMinus1 = 0;
    if (!bits.read(4, frameWidthBitsMinus1) ||
        !bits.read(4, frameHeightBitsMinus1) ||
        !bits.skip(frameWidthBitsMinus1 + 1) ||
        !bits.skip(frameHeightBitsMinus1 + 1)) {
        return false;
    }

    if (!reducedStillPictureHeader) {
        bool frameIdNumbersPresent = false;
        if (!bits.bit(frameIdNumbersPresent)) {
            return false;
        }
        if (frameIdNumbersPresent && !bits.skip(7)) {
            return false;
        }
    }

    if (!bits.skip(3)) {
        return false;
    }
    if (!reducedStillPictureHeader) {
        if (!bits.skip(4)) {
            return false;
        }
        bool enableOrderHint = false;
        if (!bits.bit(enableOrderHint)) {
            return false;
        }
        if (enableOrderHint && !bits.skip(2)) {
            return false;
        }

        bool chooseScreenContentTools = false;
        if (!bits.bit(chooseScreenContentTools)) {
            return false;
        }
        if (!chooseScreenContentTools) {
            bool forceScreenContentTools = false;
            if (!bits.bit(forceScreenContentTools)) {
                return false;
            }
            if (forceScreenContentTools) {
                bool chooseIntegerMv = false;
                if (!bits.bit(chooseIntegerMv)) {
                    return false;
                }
                if (!chooseIntegerMv && !bits.skip(1)) {
                    return false;
                }
            }
        }
        if (enableOrderHint && !bits.skip(3)) {
            return false;
        }
    }

    if (!bits.skip(3)) {
        return false;
    }

    bool highBitDepth = false;
    if (!bits.bit(highBitDepth)) {
        return false;
    }
    const bool twelveBit = info.profile == 2 && highBitDepth && [&bits]() {
        bool result = false;
        return bits.bit(result) && result;
    }();
    info.bitDepth = 8 + (highBitDepth ? 2u : 0u) + (twelveBit ? 2u : 0u);

    if (info.profile == 1) {
        info.monochrome = false;
    } else if (!bits.bit(info.monochrome)) {
        return false;
    }

    bool colorDescriptionPresent = false;
    if (!bits.bit(colorDescriptionPresent)) {
        return false;
    }
    std::uint32_t colorPrimaries = 0;
    std::uint32_t transferCharacteristics = 0;
    std::uint32_t matrixCoefficients = 0;
    if (colorDescriptionPresent &&
        (!bits.read(8, colorPrimaries) || !bits.read(8, transferCharacteristics) ||
            !bits.read(8, matrixCoefficients))) {
        return false;
    }

    if (info.monochrome) {
        if (!bits.skip(1)) {
            return false;
        }
        info.chromaSubsamplingX = true;
        info.chromaSubsamplingY = true;
        info.chromaSamplePosition = 0;
    } else if (colorPrimaries == 1 && transferCharacteristics == 13 && matrixCoefficients == 0) {
        info.chromaSubsamplingX = false;
        info.chromaSubsamplingY = false;
    } else if (info.profile == 0) {
        if (!bits.skip(1)) {
            return false;
        }
        info.chromaSubsamplingX = true;
        info.chromaSubsamplingY = true;
        info.chromaSamplePosition = 0;
    } else if (info.profile == 1) {
        if (!bits.skip(1)) {
            return false;
        }
        info.chromaSubsamplingX = false;
        info.chromaSubsamplingY = false;
        info.chromaSamplePosition = 0;
    } else if (twelveBit) {
        if (!bits.bit(info.chromaSubsamplingX)) {
            return false;
        }
        if (info.chromaSubsamplingX && !bits.bit(info.chromaSubsamplingY)) {
            return false;
        }
        if (info.chromaSubsamplingX && info.chromaSubsamplingY && !bits.read(2, info.chromaSamplePosition)) {
            return false;
        }
    } else {
        if (!bits.skip(1)) {
            return false;
        }
        info.chromaSubsamplingX = true;
        info.chromaSubsamplingY = false;
        info.chromaSamplePosition = 0;
    }

    return bits.skip(1);
}

} // namespace

bool extractAv1CodecConfiguration(
    const std::vector<std::uint8_t>& encoded,
    std::vector<std::uint8_t>& configuration) {
    configuration.clear();
    size_t offset = 0;
    while (offset < encoded.size()) {
        Av1Obu obu{};
        size_t totalSize = 0;
        if (!parseAv1Obu(encoded, offset, obu, totalSize)) {
            return false;
        }
        if (obu.type == 1) { // AV1_OBU_SEQUENCE_HEADER
            std::vector<std::uint8_t> payload(
                encoded.begin() + static_cast<std::ptrdiff_t>(obu.payloadOffset),
                encoded.begin() + static_cast<std::ptrdiff_t>(obu.payloadOffset + obu.payloadSize));
            Av1SequenceInfo info{};
            if (!parseAv1SequenceHeader(payload, info)) {
                return false;
            }

            configuration.push_back(static_cast<std::uint8_t>(0x81 | ((info.profile & 0x07) << 4)));
            configuration.push_back(static_cast<std::uint8_t>((info.tier ? 0x80 : 0) | (info.level & 0x1f)));
            configuration.push_back(static_cast<std::uint8_t>(
                (info.bitDepth > 8 ? 0x40 : 0) |
                (info.bitDepth == 12 ? 0x20 : 0) |
                (info.monochrome ? 0x10 : 0) |
                (info.chromaSubsamplingX ? 0x08 : 0) |
                (info.chromaSubsamplingY ? 0x04 : 0) |
                (info.chromaSamplePosition & 0x03)));
            configuration.push_back(0);
            configuration.insert(
                configuration.end(),
                encoded.begin() + static_cast<std::ptrdiff_t>(offset),
                encoded.begin() + static_cast<std::ptrdiff_t>(offset + totalSize));
            return true;
        }
        offset += totalSize;
    }
    return false;
}

bool removeAv1TemporalDelimiters(
    const std::vector<std::uint8_t>& encoded,
    std::vector<std::uint8_t>& sample) {
    sample.clear();
    sample.reserve(encoded.size());
    size_t offset = 0;
    while (offset < encoded.size()) {
        Av1Obu obu{};
        size_t totalSize = 0;
        if (!parseAv1Obu(encoded, offset, obu, totalSize)) {
            sample.clear();
            return false;
        }
        if (obu.type != 2) { // AV1_OBU_TEMPORAL_DELIMITER
            sample.insert(
                sample.end(),
                encoded.begin() + static_cast<std::ptrdiff_t>(offset),
                encoded.begin() + static_cast<std::ptrdiff_t>(offset + totalSize));
        }
        offset += totalSize;
    }
    return !sample.empty();
}

namespace {

bool normalizeAv1ToSample(
    const std::vector<std::uint8_t>& encoded,
    std::vector<std::uint8_t>& packet) {
    packet.clear();
    if (encoded.empty()) {
        return false;
    }

    size_t offset = 0;
    bool foundObu = false;
    while (offset < encoded.size()) {
        Av1Obu obu{};
        size_t totalSize = 0;
        if (!parseAv1Obu(encoded, offset, obu, totalSize)) {
            return false;
        }
        foundObu = true;
        offset += totalSize;
    }
    if (!foundObu) {
        return false;
    }
    packet = encoded;
    return true;
}

} // namespace

bool extractH264SequenceHeader(
    const std::vector<std::uint8_t>& encoded,
    std::vector<std::uint8_t>& sequenceHeader) {
    sequenceHeader.clear();
    bool hasSps = false;
    bool hasPps = false;

    auto appendNal = [&](const std::uint8_t* nal, size_t length) {
        sequenceHeader.insert(sequenceHeader.end(), {0, 0, 0, 1});
        sequenceHeader.insert(sequenceHeader.end(), nal, nal + length);
        if ((nal[0] & 0x1f) == 7) {
            hasSps = true;
        } else if ((nal[0] & 0x1f) == 8) {
            hasPps = true;
        }
    };

    size_t codeSize = 0;
    size_t start = findStartCode(encoded, 0, codeSize);
    if (start != std::numeric_limits<size_t>::max()) {
        while (start != std::numeric_limits<size_t>::max()) {
            const size_t nalStart = start + codeSize;
            size_t nextCodeSize = 0;
            const size_t next = findStartCode(encoded, nalStart, nextCodeSize);
            size_t nalEnd = next == std::numeric_limits<size_t>::max() ? encoded.size() : next;
            while (nalEnd > nalStart && encoded[nalEnd - 1] == 0) {
                --nalEnd;
            }
            if (nalEnd > nalStart && ((encoded[nalStart] & 0x1f) == 7 || (encoded[nalStart] & 0x1f) == 8)) {
                appendNal(encoded.data() + nalStart, nalEnd - nalStart);
            }
            start = next;
            codeSize = nextCodeSize;
        }
    } else {
        for (size_t offset = 0; offset + sizeof(DWORD) <= encoded.size();) {
            const DWORD nalLength =
                (static_cast<DWORD>(encoded[offset]) << 24) |
                (static_cast<DWORD>(encoded[offset + 1]) << 16) |
                (static_cast<DWORD>(encoded[offset + 2]) << 8) |
                static_cast<DWORD>(encoded[offset + 3]);
            offset += sizeof(DWORD);
            if (nalLength == 0 || nalLength > encoded.size() - offset) {
                sequenceHeader.clear();
                return false;
            }
            if ((encoded[offset] & 0x1f) == 7 || (encoded[offset] & 0x1f) == 8) {
                appendNal(encoded.data() + offset, nalLength);
            }
            offset += nalLength;
        }
    }

    return hasSps && hasPps;
}

namespace {

void writeBigEndian(std::ostream& output, std::uint64_t value, unsigned byteCount) {
    for (unsigned index = 0; index < byteCount; ++index) {
        const unsigned shift = (byteCount - index - 1) * 8;
        output.put(static_cast<char>((value >> shift) & 0xff));
    }
}

class Mp4Builder final {
public:
    void u8(std::uint8_t value) {
        bytes.push_back(value);
    }

    void u16(std::uint16_t value) {
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    }

    void u32(std::uint32_t value) {
        bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    }

    void u64(std::uint64_t value) {
        u32(static_cast<std::uint32_t>(value >> 32));
        u32(static_cast<std::uint32_t>(value & 0xffffffff));
    }

    void fullBox(std::uint8_t version, std::uint32_t flags) {
        u8(version);
        u8(static_cast<std::uint8_t>((flags >> 16) & 0xff));
        u8(static_cast<std::uint8_t>((flags >> 8) & 0xff));
        u8(static_cast<std::uint8_t>(flags & 0xff));
    }

    void fourcc(const char* value) {
        bytes.insert(bytes.end(), value, value + 4);
    }

    void append(const std::vector<std::uint8_t>& value) {
        bytes.insert(bytes.end(), value.begin(), value.end());
    }

    void zeros(size_t count) {
        bytes.insert(bytes.end(), count, 0);
    }

    size_t beginBox(const char* type) {
        const size_t position = bytes.size();
        u32(0);
        fourcc(type);
        return position;
    }

    void endBox(size_t position) {
        const auto size = bytes.size() - position;
        if (size > UINT32_MAX) {
            throw std::runtime_error("MP4 box is too large");
        }
        bytes[position] = static_cast<std::uint8_t>((size >> 24) & 0xff);
        bytes[position + 1] = static_cast<std::uint8_t>((size >> 16) & 0xff);
        bytes[position + 2] = static_cast<std::uint8_t>((size >> 8) & 0xff);
        bytes[position + 3] = static_cast<std::uint8_t>(size & 0xff);
    }

    std::vector<std::uint8_t> bytes;
};

bool containsH264NalType(const std::vector<std::uint8_t>& encoded, std::uint8_t expectedType) {
    std::vector<std::uint8_t> annexB;
    if (!normalizeH264ToAnnexB(encoded, annexB)) {
        return false;
    }

    size_t codeSize = 0;
    size_t start = findStartCode(annexB, 0, codeSize);
    while (start != std::numeric_limits<size_t>::max()) {
        const size_t nalStart = start + codeSize;
        if (nalStart < annexB.size() && (annexB[nalStart] & 0x1f) == expectedType) {
            return true;
        }
        size_t nextCodeSize = 0;
        start = findStartCode(annexB, nalStart, nextCodeSize);
        codeSize = nextCodeSize;
    }
    return false;
}

} // namespace

class AacEncoder::Impl final {
public:
    bool initialize(std::wstring& error) {
        HRESULT result = CoCreateInstance(
            CLSID_AACMFTEncoder,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&m_transform));

        ComPtr<IMFMediaType> outputType;
        if (SUCCEEDED(result)) result = MFCreateMediaType(&outputType);
        if (SUCCEEDED(result)) result = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(result)) result = outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AudioCapture::kChannels);
        if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioCapture::kSampleRate);
        if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, AudioCapture::kBitsPerSample);
        if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAudioBitrate / 8);
        if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);
        if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        if (SUCCEEDED(result)) result = outputType->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        if (SUCCEEDED(result)) result = m_transform->SetOutputType(0, outputType.Get(), 0);

        ComPtr<IMFMediaType> inputType;
        if (SUCCEEDED(result)) result = MFCreateMediaType(&inputType);
        if (SUCCEEDED(result)) result = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(result)) result = inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, AudioCapture::kChannels);
        if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, AudioCapture::kSampleRate);
        if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, AudioCapture::kBitsPerSample);
        if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, AudioCapture::kBlockAlignment);
        if (SUCCEEDED(result)) {
            result = inputType->SetUINT32(
                MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                AudioCapture::kSampleRate * AudioCapture::kBlockAlignment);
        }
        if (SUCCEEDED(result)) result = inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        if (SUCCEEDED(result)) result = m_transform->SetInputType(0, inputType.Get(), 0);
        if (SUCCEEDED(result)) result = m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (SUCCEEDED(result)) result = m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        if (FAILED(result)) {
            wchar_t message[128]{};
            swprintf_s(
                message,
                _countof(message),
                L"Não foi possível iniciar o encoder AAC (HRESULT 0x%08X).",
                static_cast<unsigned>(result));
            error = message;
            m_transform.Reset();
            return false;
        }
        return true;
    }

    bool encode(
        const std::vector<std::uint8_t>& pcm,
        std::uint32_t frameCount,
        std::vector<EncodedAudioPacket>& packets,
        std::wstring& error) {
        if (!m_transform || pcm.empty() || frameCount == 0) {
            return true;
        }

        ComPtr<IMFMediaBuffer> buffer;
        HRESULT result = MFCreateMemoryBuffer(static_cast<DWORD>(pcm.size()), &buffer);
        BYTE* destination = nullptr;
        if (SUCCEEDED(result)) result = buffer->Lock(&destination, nullptr, nullptr);
        if (SUCCEEDED(result)) {
            std::memcpy(destination, pcm.data(), pcm.size());
            buffer->Unlock();
            destination = nullptr;
            result = buffer->SetCurrentLength(static_cast<DWORD>(pcm.size()));
        }
        if (destination != nullptr) buffer->Unlock();

        ComPtr<IMFSample> sample;
        if (SUCCEEDED(result)) result = MFCreateSample(&sample);
        if (SUCCEEDED(result)) result = sample->AddBuffer(buffer.Get());
        if (SUCCEEDED(result)) {
            result = sample->SetSampleTime(static_cast<LONGLONG>(
                m_inputFrames * kHundredNanosecondsPerSecond / AudioCapture::kSampleRate));
        }
        if (SUCCEEDED(result)) {
            result = sample->SetSampleDuration(
                static_cast<LONGLONG>(frameCount) * kHundredNanosecondsPerSecond /
                AudioCapture::kSampleRate);
        }

        if (SUCCEEDED(result)) {
            result = m_transform->ProcessInput(0, sample.Get(), 0);
            if (result == MF_E_NOTACCEPTING) {
                if (!collectOutput(packets, error)) return false;
                result = m_transform->ProcessInput(0, sample.Get(), 0);
            }
        }
        if (FAILED(result)) {
            return fail(L"Não foi possível enviar o microfone ao encoder AAC", result, error);
        }
        m_inputFrames += frameCount;
        return collectOutput(packets, error);
    }

    bool drain(std::vector<EncodedAudioPacket>& packets, std::wstring& error) {
        if (!m_transform) return true;
        HRESULT result = m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        if (SUCCEEDED(result)) result = m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        if (FAILED(result)) return fail(L"Não foi possível finalizar o encoder AAC", result, error);
        const bool collected = collectOutput(packets, error);
        m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        return collected;
    }

private:
    static bool fail(const wchar_t* operation, HRESULT result, std::wstring& error) {
        wchar_t message[128]{};
        swprintf_s(
            message,
            _countof(message),
            L"%s (HRESULT 0x%08X).",
            operation,
            static_cast<unsigned>(result));
        error = message;
        return false;
    }

    bool collectOutput(std::vector<EncodedAudioPacket>& packets, std::wstring& error) {
        MFT_OUTPUT_STREAM_INFO streamInfo{};
        HRESULT result = m_transform->GetOutputStreamInfo(0, &streamInfo);
        if (FAILED(result)) return fail(L"Não foi possível consultar o encoder AAC", result, error);

        for (;;) {
            ComPtr<IMFSample> suppliedSample;
            if ((streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
                result = MFCreateSample(&suppliedSample);
                ComPtr<IMFMediaBuffer> outputBuffer;
                if (SUCCEEDED(result)) {
                    result = MFCreateMemoryBuffer(
                        (std::max)(streamInfo.cbSize, static_cast<DWORD>(64 * 1024)),
                        &outputBuffer);
                }
                if (SUCCEEDED(result)) result = suppliedSample->AddBuffer(outputBuffer.Get());
                if (FAILED(result)) return fail(L"Não foi possível criar o buffer AAC", result, error);
            }

            MFT_OUTPUT_DATA_BUFFER output{};
            output.dwStreamID = 0;
            output.pSample = suppliedSample.Get();
            DWORD status = 0;
            result = m_transform->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents != nullptr) output.pEvents->Release();
            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (FAILED(result)) return fail(L"Não foi possível obter o áudio AAC", result, error);

            ComPtr<IMFSample> producedSample;
            if (output.pSample == suppliedSample.Get()) {
                producedSample = suppliedSample;
            } else {
                producedSample.Attach(output.pSample);
            }
            if (!producedSample) {
                error = L"O encoder AAC retornou uma amostra vazia.";
                return false;
            }

            ComPtr<IMFMediaBuffer> contiguous;
            result = producedSample->ConvertToContiguousBuffer(&contiguous);
            BYTE* data = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(result)) result = contiguous->Lock(&data, nullptr, &length);
            EncodedAudioPacket packet;
            if (SUCCEEDED(result) && data != nullptr && length > 0) {
                packet.bytes.assign(data, data + length);
                contiguous->Unlock();
                data = nullptr;
            }
            if (data != nullptr) contiguous->Unlock();
            if (FAILED(result)) return fail(L"Não foi possível ler o áudio AAC", result, error);

            LONGLONG duration = 0;
            if (SUCCEEDED(producedSample->GetSampleDuration(&duration)) && duration > 0) {
                packet.durationFrames = static_cast<std::uint32_t>((std::max<LONGLONG>)(
                    1,
                    (duration * AudioCapture::kSampleRate + kHundredNanosecondsPerSecond / 2) /
                        kHundredNanosecondsPerSecond));
            }
            if (!packet.bytes.empty()) packets.push_back(std::move(packet));
        }
    }

    ComPtr<IMFTransform> m_transform;
    std::uint64_t m_inputFrames{0};
};

AacEncoder::AacEncoder() : m_impl(std::make_unique<Impl>()) {}

AacEncoder::~AacEncoder() = default;

bool AacEncoder::initialize(std::wstring& error) {
    return m_impl->initialize(error);
}

bool AacEncoder::encode(
    const std::vector<std::uint8_t>& pcm,
    std::uint32_t frameCount,
    std::vector<EncodedAudioPacket>& packets,
    std::wstring& error) {
    return m_impl->encode(pcm, frameCount, packets, error);
}

bool AacEncoder::drain(std::vector<EncodedAudioPacket>& packets, std::wstring& error) {
    return m_impl->drain(packets, error);
}

class Mp4Writer::Impl final {
public:
    ~Impl() {
        std::wstring ignored;
        finalize(ignored);
    }

    bool start(
        const std::filesystem::path& output,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t fps,
        std::uint32_t bitrate,
        VideoCodec codec,
        const std::vector<std::uint8_t>& sequenceHeader,
        bool audioEnabled,
        std::wstring& error) {
        m_codec = codec;
        if (m_codec == VideoCodec::H264) {
            if (!extractParameterSets(sequenceHeader, m_sps, m_pps)) {
                error = L"O cabeçalho H.264 do encoder não contém SPS e PPS válidos.";
                return false;
            }
        } else if (!extractAv1CodecConfiguration(sequenceHeader, m_av1Configuration)) {
            error = L"O primeiro pacote AV1 do encoder não contém um cabeçalho de sequência válido.";
            return false;
        }

        m_file.open(output, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (!m_file.is_open()) {
            error = L"Não foi possível criar o arquivo MP4 da gravação.";
            return false;
        }

        m_width = width;
        m_height = height;
        m_timeScale = fps;
        m_bitrate = bitrate;
        m_audioEnabled = audioEnabled;
        m_mdatOffset = 32;
        m_dataOffset = 48;
        m_dataBytes = 0;
        m_samples.clear();
        m_audioSamples.clear();
        m_keyFrames.clear();
        m_firstSample = true;

        writeBigEndian(m_file, 32, 4);
        m_file.write("ftyp", 4);
        m_file.write("isom", 4);
        writeBigEndian(m_file, 0x00000200, 4);
        m_file.write("isom", 4);
        m_file.write("iso2", 4);
        m_file.write(m_codec == VideoCodec::H264 ? "avc1" : "av01", 4);
        m_file.write("mp41", 4);

        writeBigEndian(m_file, 1, 4);
        m_file.write("mdat", 4);
        writeBigEndian(m_file, 0, 8);
        m_file.flush();
        if (!m_file.good()) {
            error = L"Não foi possível preparar o contêiner MP4 da gravação.";
            m_file.close();
            return false;
        }

        m_started = true;
        return true;
    }

    bool writeAudio(const EncodedAudioPacket& encoded, std::wstring& error) {
        if (!m_started || !m_audioEnabled) {
            error = L"A faixa de áudio do contêiner MP4 não está aberta.";
            return false;
        }
        if (encoded.bytes.empty() || encoded.bytes.size() > static_cast<size_t>(UINT32_MAX)) {
            error = L"O encoder AAC retornou um pacote vazio ou grande demais.";
            return false;
        }

        const auto offset = m_dataOffset + m_dataBytes;
        m_file.seekp(0, std::ios::end);
        m_file.write(
            reinterpret_cast<const char*>(encoded.bytes.data()),
            static_cast<std::streamsize>(encoded.bytes.size()));
        if (!m_file.good()) {
            error = L"Não foi possível gravar o áudio AAC no arquivo MP4.";
            return false;
        }
        m_dataBytes += encoded.bytes.size();
        m_audioSamples.push_back({
            offset,
            static_cast<std::uint32_t>(encoded.bytes.size()),
            encoded.durationFrames});
        return true;
    }

    bool write(
        const std::vector<std::uint8_t>& encoded,
        LONGLONG timestamp,
        std::wstring& error,
        std::optional<bool> keyFrame) {
        (void)timestamp;
        if (!m_started) {
            error = L"O contêiner MP4 não está aberto.";
            return false;
        }

        std::vector<std::uint8_t> packet;
        const bool normalized = m_codec == VideoCodec::H264
            ? normalizeH264ToLengthPrefixed(encoded, packet)
            : normalizeAv1ToSample(encoded, packet);
        if (!normalized) {
            error = m_codec == VideoCodec::H264
                ? L"O encoder retornou um pacote H.264 vazio ou inválido."
                : L"O encoder retornou um pacote AV1 vazio ou inválido.";
            return false;
        }
        if (packet.size() > static_cast<size_t>(UINT32_MAX)) {
            error = m_codec == VideoCodec::H264
                ? L"O pacote H.264 retornado pelo encoder é grande demais."
                : L"O pacote AV1 retornado pelo encoder é grande demais.";
            return false;
        }

        const auto offset = m_dataOffset + m_dataBytes;
        m_file.seekp(0, std::ios::end);
        m_file.write(reinterpret_cast<const char*>(packet.data()), static_cast<std::streamsize>(packet.size()));
        if (!m_file.good()) {
            error = m_codec == VideoCodec::H264
                ? L"Não foi possível gravar o pacote H.264 no arquivo MP4."
                : L"Não foi possível gravar o pacote AV1 no arquivo MP4.";
            return false;
        }

        m_dataBytes += packet.size();
        m_samples.push_back({offset, static_cast<std::uint32_t>(packet.size())});
        const bool isKeyFrame = keyFrame.has_value()
            ? *keyFrame
            : m_codec == VideoCodec::H264 && containsH264NalType(encoded, 5);
        if (m_firstSample || isKeyFrame) {
            m_keyFrames.push_back(m_samples.size());
        }
        m_firstSample = false;
        return true;
    }

    bool finalize(std::wstring& error) {
        if (!m_started) {
            return true;
        }

        try {
            if (m_samples.empty()) {
                error = m_codec == VideoCodec::H264
                    ? L"A gravação não produziu nenhum frame H.264."
                    : L"A gravação não produziu nenhum frame AV1.";
                m_file.close();
                m_started = false;
                return false;
            }

            Mp4Builder moov;
            buildMoov(moov);

            m_file.seekp(m_mdatOffset, std::ios::beg);
            writeBigEndian(m_file, 1, 4);
            m_file.write("mdat", 4);
            writeBigEndian(m_file, 16 + m_dataBytes, 8);
            m_file.seekp(0, std::ios::end);
            m_file.write(reinterpret_cast<const char*>(moov.bytes.data()),
                static_cast<std::streamsize>(moov.bytes.size()));
            m_file.flush();
            const bool good = m_file.good();
            m_file.close();
            m_started = false;
            if (!good) {
                error = L"Não foi possível finalizar o arquivo MP4 da gravação.";
                return false;
            }
            return true;
        } catch (const std::exception&) {
            m_file.close();
            m_started = false;
            error = L"Não foi possível montar o índice do arquivo MP4 da gravação.";
            return false;
        }
    }

private:
    struct SampleInfo {
        std::uint64_t offset;
        std::uint32_t size;
    };

    struct AudioSampleInfo {
        std::uint64_t offset;
        std::uint32_t size;
        std::uint32_t duration;
    };

    static bool extractParameterSets(
        const std::vector<std::uint8_t>& header,
        std::vector<std::uint8_t>& sps,
        std::vector<std::uint8_t>& pps) {
        sps.clear();
        pps.clear();
        size_t codeSize = 0;
        size_t start = findStartCode(header, 0, codeSize);
        while (start != std::numeric_limits<size_t>::max()) {
            const size_t nalStart = start + codeSize;
            size_t nextCodeSize = 0;
            const size_t next = findStartCode(header, nalStart, nextCodeSize);
            size_t nalEnd = next == std::numeric_limits<size_t>::max() ? header.size() : next;
            while (nalEnd > nalStart && header[nalEnd - 1] == 0) {
                --nalEnd;
            }
            if (nalEnd > nalStart) {
                const auto type = header[nalStart] & 0x1f;
                if (type == 7 && sps.empty()) {
                    sps.assign(header.begin() + static_cast<std::ptrdiff_t>(nalStart),
                        header.begin() + static_cast<std::ptrdiff_t>(nalEnd));
                } else if (type == 8 && pps.empty()) {
                    pps.assign(header.begin() + static_cast<std::ptrdiff_t>(nalStart),
                        header.begin() + static_cast<std::ptrdiff_t>(nalEnd));
                }
            }
            start = next;
            codeSize = nextCodeSize;
        }
        return sps.size() >= 4 && pps.size() >= 2;
    }

    static void writeIdentityMatrix(Mp4Builder& builder) {
        builder.u32(0x00010000);
        builder.u32(0);
        builder.u32(0);
        builder.u32(0);
        builder.u32(0x00010000);
        builder.u32(0);
        builder.u32(0);
        builder.u32(0);
        builder.u32(0x40000000);
    }

    void buildAvc1(Mp4Builder& builder) const {
        const size_t avc1 = builder.beginBox("avc1");
        builder.zeros(6);
        builder.u16(1);
        builder.u16(0);
        builder.u16(0);
        builder.zeros(12);
        builder.u16(static_cast<std::uint16_t>(m_width));
        builder.u16(static_cast<std::uint16_t>(m_height));
        builder.u32(0x00480000);
        builder.u32(0x00480000);
        builder.u32(0);
        builder.u16(1);
        builder.zeros(32);
        builder.u16(0x0018);
        builder.u16(0xffff);

        const size_t avcC = builder.beginBox("avcC");
        builder.u8(1);
        builder.u8(m_sps[1]);
        builder.u8(m_sps[2]);
        builder.u8(m_sps[3]);
        builder.u8(0xff);
        builder.u8(0xe1);
        builder.u16(static_cast<std::uint16_t>(m_sps.size()));
        builder.append(m_sps);
        builder.u8(1);
        builder.u16(static_cast<std::uint16_t>(m_pps.size()));
        builder.append(m_pps);
        builder.endBox(avcC);

        const size_t btrt = builder.beginBox("btrt");
        builder.u32(0);
        builder.u32(m_bitrate);
        builder.u32(m_bitrate);
        builder.endBox(btrt);
        builder.endBox(avc1);
    }

    void buildAv01(Mp4Builder& builder) const {
        const size_t av01 = builder.beginBox("av01");
        builder.zeros(6);
        builder.u16(1);
        builder.u16(0);
        builder.u16(0);
        builder.zeros(12);
        builder.u16(static_cast<std::uint16_t>(m_width));
        builder.u16(static_cast<std::uint16_t>(m_height));
        builder.u32(0x00480000);
        builder.u32(0x00480000);
        builder.u32(0);
        builder.u16(1);
        builder.zeros(32);
        builder.u16(0x0018);
        builder.u16(0xffff);

        const size_t av1C = builder.beginBox("av1C");
        builder.append(m_av1Configuration);
        builder.endBox(av1C);

        const size_t btrt = builder.beginBox("btrt");
        builder.u32(0);
        builder.u32(m_bitrate);
        builder.u32(m_bitrate);
        builder.endBox(btrt);
        builder.endBox(av01);
    }

    void buildVideoSampleEntry(Mp4Builder& builder) const {
        if (m_codec == VideoCodec::H264) {
            buildAvc1(builder);
        } else {
            buildAv01(builder);
        }
    }

    void buildAudioSampleEntry(Mp4Builder& builder) const {
        const size_t mp4a = builder.beginBox("mp4a");
        builder.zeros(6);
        builder.u16(1);
        builder.zeros(8);
        builder.u16(AudioCapture::kChannels);
        builder.u16(AudioCapture::kBitsPerSample);
        builder.u16(0);
        builder.u16(0);
        builder.u32(AudioCapture::kSampleRate << 16);

        const size_t esds = builder.beginBox("esds");
        builder.fullBox(0, 0);
        builder.u8(0x03);
        builder.u8(0x19);
        builder.u16(2);
        builder.u8(0);
        builder.u8(0x04);
        builder.u8(0x11);
        builder.u8(0x40);
        builder.u8(0x15);
        builder.u8(0);
        builder.u8(0);
        builder.u8(0);
        builder.u32(kAudioBitrate);
        builder.u32(kAudioBitrate);
        builder.u8(0x05);
        builder.u8(0x02);
        builder.u8(0x11);
        builder.u8(0x90);
        builder.u8(0x06);
        builder.u8(0x01);
        builder.u8(0x02);
        builder.endBox(esds);
        builder.endBox(mp4a);
    }

    void buildAudioTrack(Mp4Builder& builder, std::uint32_t movieDuration) const {
        std::uint64_t audioDuration = 0;
        for (const auto& sample : m_audioSamples) audioDuration += sample.duration;
        const auto mediaDuration = static_cast<std::uint32_t>((std::min<std::uint64_t>)(
            audioDuration,
            UINT32_MAX));

        const size_t trak = builder.beginBox("trak");
        const size_t tkhd = builder.beginBox("tkhd");
        builder.fullBox(0, 0x000007);
        builder.u32(0);
        builder.u32(0);
        builder.u32(2);
        builder.u32(0);
        builder.u32(movieDuration);
        builder.zeros(8);
        builder.u16(0);
        builder.u16(0);
        builder.u16(0x0100);
        builder.u16(0);
        writeIdentityMatrix(builder);
        builder.u32(0);
        builder.u32(0);
        builder.endBox(tkhd);

        const size_t mdia = builder.beginBox("mdia");
        const size_t mdhd = builder.beginBox("mdhd");
        builder.fullBox(0, 0);
        builder.u32(0);
        builder.u32(0);
        builder.u32(AudioCapture::kSampleRate);
        builder.u32(mediaDuration);
        builder.u16(0x55c4);
        builder.u16(0);
        builder.endBox(mdhd);

        const size_t hdlr = builder.beginBox("hdlr");
        builder.fullBox(0, 0);
        builder.u32(0);
        builder.fourcc("soun");
        builder.zeros(12);
        builder.append({'S', 'o', 'u', 'n', 'd', 'H', 'a', 'n', 'd', 'l', 'e', 'r', 0});
        builder.endBox(hdlr);

        const size_t minf = builder.beginBox("minf");
        const size_t smhd = builder.beginBox("smhd");
        builder.fullBox(0, 0);
        builder.u16(0);
        builder.u16(0);
        builder.endBox(smhd);

        const size_t dinf = builder.beginBox("dinf");
        const size_t dref = builder.beginBox("dref");
        builder.fullBox(0, 0);
        builder.u32(1);
        const size_t url = builder.beginBox("url ");
        builder.fullBox(0, 1);
        builder.endBox(url);
        builder.endBox(dref);
        builder.endBox(dinf);

        const size_t stbl = builder.beginBox("stbl");
        const size_t stsd = builder.beginBox("stsd");
        builder.fullBox(0, 0);
        builder.u32(1);
        buildAudioSampleEntry(builder);
        builder.endBox(stsd);

        std::vector<std::pair<std::uint32_t, std::uint32_t>> durationRuns;
        for (const auto& sample : m_audioSamples) {
            if (!durationRuns.empty() && durationRuns.back().second == sample.duration) {
                ++durationRuns.back().first;
            } else {
                durationRuns.push_back({1, sample.duration});
            }
        }
        const size_t stts = builder.beginBox("stts");
        builder.fullBox(0, 0);
        builder.u32(static_cast<std::uint32_t>(durationRuns.size()));
        for (const auto& [count, duration] : durationRuns) {
            builder.u32(count);
            builder.u32(duration);
        }
        builder.endBox(stts);

        const size_t stsc = builder.beginBox("stsc");
        builder.fullBox(0, 0);
        builder.u32(1);
        builder.u32(1);
        builder.u32(1);
        builder.u32(1);
        builder.endBox(stsc);

        const size_t stsz = builder.beginBox("stsz");
        builder.fullBox(0, 0);
        builder.u32(0);
        builder.u32(static_cast<std::uint32_t>(m_audioSamples.size()));
        for (const auto& sample : m_audioSamples) builder.u32(sample.size);
        builder.endBox(stsz);

        const size_t co64 = builder.beginBox("co64");
        builder.fullBox(0, 0);
        builder.u32(static_cast<std::uint32_t>(m_audioSamples.size()));
        for (const auto& sample : m_audioSamples) builder.u64(sample.offset);
        builder.endBox(co64);
        builder.endBox(stbl);
        builder.endBox(minf);
        builder.endBox(mdia);
        builder.endBox(trak);
    }

    void buildMoov(Mp4Builder& builder) const {
        const auto sampleCount = static_cast<std::uint32_t>(m_samples.size());
        std::uint64_t audioDuration = 0;
        for (const auto& sample : m_audioSamples) audioDuration += sample.duration;
        const auto audioMovieDuration = static_cast<std::uint32_t>((std::min<std::uint64_t>)(
            (audioDuration * m_timeScale + AudioCapture::kSampleRate - 1) /
                AudioCapture::kSampleRate,
            UINT32_MAX));
        const auto movieDuration = (std::max)(sampleCount, audioMovieDuration);
        const size_t moov = builder.beginBox("moov");

        const size_t mvhd = builder.beginBox("mvhd");
        builder.fullBox(0, 0);
        builder.u32(0);
        builder.u32(0);
        builder.u32(m_timeScale);
        builder.u32(movieDuration);
        builder.u32(0x00010000);
        builder.u16(0x0100);
        builder.u16(0);
        builder.zeros(8);
        writeIdentityMatrix(builder);
        builder.zeros(24);
        builder.u32(m_audioSamples.empty() ? 2 : 3);
        builder.endBox(mvhd);

        const size_t trak = builder.beginBox("trak");
        const size_t tkhd = builder.beginBox("tkhd");
        builder.fullBox(0, 0x000007);
        builder.u32(0);
        builder.u32(0);
        builder.u32(1);
        builder.u32(0);
        builder.u32(sampleCount);
        builder.zeros(8);
        builder.u16(0);
        builder.u16(0);
        builder.u16(0);
        builder.u16(0);
        writeIdentityMatrix(builder);
        builder.u32(m_width << 16);
        builder.u32(m_height << 16);
        builder.endBox(tkhd);

        const size_t mdia = builder.beginBox("mdia");
        const size_t mdhd = builder.beginBox("mdhd");
        builder.fullBox(0, 0);
        builder.u32(0);
        builder.u32(0);
        builder.u32(m_timeScale);
        builder.u32(sampleCount);
        builder.u16(0x55c4);
        builder.u16(0);
        builder.endBox(mdhd);

        const size_t hdlr = builder.beginBox("hdlr");
        builder.fullBox(0, 0);
        builder.u32(0);
        builder.fourcc("vide");
        builder.zeros(12);
        builder.append({
            'V', 'i', 'd', 'e', 'o', 'H', 'a', 'n', 'd', 'l', 'e', 'r', 0});
        builder.endBox(hdlr);

        const size_t minf = builder.beginBox("minf");
        const size_t vmhd = builder.beginBox("vmhd");
        builder.fullBox(0, 1);
        builder.u16(0);
        builder.u16(0);
        builder.u16(0);
        builder.u16(0);
        builder.endBox(vmhd);

        const size_t dinf = builder.beginBox("dinf");
        const size_t dref = builder.beginBox("dref");
        builder.fullBox(0, 0);
        builder.u32(1);
        const size_t url = builder.beginBox("url ");
        builder.fullBox(0, 1);
        builder.endBox(url);
        builder.endBox(dref);
        builder.endBox(dinf);

        const size_t stbl = builder.beginBox("stbl");
        const size_t stsd = builder.beginBox("stsd");
        builder.fullBox(0, 0);
        builder.u32(1);
        buildVideoSampleEntry(builder);
        builder.endBox(stsd);

        const size_t stts = builder.beginBox("stts");
        builder.fullBox(0, 0);
        builder.u32(1);
        builder.u32(sampleCount);
        builder.u32(1);
        builder.endBox(stts);

        const size_t stsc = builder.beginBox("stsc");
        builder.fullBox(0, 0);
        builder.u32(1);
        builder.u32(1);
        builder.u32(1);
        builder.u32(1);
        builder.endBox(stsc);

        const size_t stsz = builder.beginBox("stsz");
        builder.fullBox(0, 0);
        builder.u32(0);
        builder.u32(sampleCount);
        for (const auto& sample : m_samples) {
            builder.u32(sample.size);
        }
        builder.endBox(stsz);

        const size_t co64 = builder.beginBox("co64");
        builder.fullBox(0, 0);
        builder.u32(sampleCount);
        for (const auto& sample : m_samples) {
            builder.u64(sample.offset);
        }
        builder.endBox(co64);

        const size_t stss = builder.beginBox("stss");
        builder.fullBox(0, 0);
        builder.u32(static_cast<std::uint32_t>(m_keyFrames.size()));
        for (const auto frame : m_keyFrames) {
            builder.u32(static_cast<std::uint32_t>(frame));
        }
        builder.endBox(stss);
        builder.endBox(stbl);
        builder.endBox(minf);
        builder.endBox(mdia);
        builder.endBox(trak);
        if (!m_audioSamples.empty()) {
            buildAudioTrack(builder, audioMovieDuration);
        }
        builder.endBox(moov);
    }

    std::fstream m_file;
    std::vector<std::uint8_t> m_sps;
    std::vector<std::uint8_t> m_pps;
    std::vector<std::uint8_t> m_av1Configuration;
    std::vector<SampleInfo> m_samples;
    std::vector<AudioSampleInfo> m_audioSamples;
    std::vector<size_t> m_keyFrames;
    std::uint32_t m_width{0};
    std::uint32_t m_height{0};
    std::uint32_t m_timeScale{0};
    std::uint32_t m_bitrate{0};
    std::uint64_t m_mdatOffset{0};
    std::uint64_t m_dataOffset{0};
    std::uint64_t m_dataBytes{0};
    VideoCodec m_codec{VideoCodec::H264};
    bool m_audioEnabled{false};
    bool m_started{false};
    bool m_firstSample{true};
};

Mp4Writer::Mp4Writer() : m_impl(std::make_unique<Impl>()) {}

Mp4Writer::~Mp4Writer() = default;

bool Mp4Writer::start(
    const std::filesystem::path& output,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t fps,
    std::uint32_t bitrate,
    VideoCodec codec,
    const std::vector<std::uint8_t>& sequenceHeader,
    bool audioEnabled,
    std::wstring& error) {
    return m_impl->start(
        output, width, height, fps, bitrate, codec, sequenceHeader, audioEnabled, error);
}

bool Mp4Writer::writeAudio(const EncodedAudioPacket& encoded, std::wstring& error) {
    return m_impl->writeAudio(encoded, error);
}

bool Mp4Writer::write(
    const std::vector<std::uint8_t>& encoded,
    LONGLONG timestamp,
    std::wstring& error,
    std::optional<bool> keyFrame) {
    return m_impl->write(encoded, timestamp, error, keyFrame);
}

bool Mp4Writer::finalize(std::wstring& error) {
    return m_impl->finalize(error);
}

} // namespace fastrecord::recording
