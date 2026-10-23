#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace vision {

// ITU-T G.711 A-law. PCMA RTP always carries one 8-bit code for each mono
// 8 kHz PCM sample, so a 20 ms packet contains exactly 160 bytes.
inline std::uint8_t linear_to_alaw(std::int16_t sample) {
    static const int segment_end[8] = {
        0x00ff, 0x01ff, 0x03ff, 0x07ff,
        0x0fff, 0x1fff, 0x3fff, 0x7fff
    };

    int value = sample;
    int mask = 0xd5;
    if (value < 0) {
        mask = 0x55;
        value = -value - 1;
    }
    if (value > 0x7fff) value = 0x7fff;

    int segment = 0;
    while (segment < 8 && value > segment_end[segment]) ++segment;
    if (segment >= 8) return static_cast<std::uint8_t>(0x7f ^ mask);

    int code = segment << 4;
    code |= segment < 2 ? (value >> 4) & 0x0f
                        : (value >> (segment + 3)) & 0x0f;
    return static_cast<std::uint8_t>(code ^ mask);
}

inline void encode_g711a_mono(const std::int16_t* interleaved,
                              std::size_t frames, unsigned channels,
                              std::vector<std::uint8_t>& encoded) {
    if (!interleaved || !frames || !channels)
        throw std::invalid_argument("G.711A input must contain PCM frames");
    encoded.resize(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        std::int32_t sum = 0;
        for (unsigned channel = 0; channel < channels; ++channel)
            sum += interleaved[frame * channels + channel];
        encoded[frame] = linear_to_alaw(
            static_cast<std::int16_t>(sum / static_cast<std::int32_t>(channels)));
    }
}

} // namespace vision
