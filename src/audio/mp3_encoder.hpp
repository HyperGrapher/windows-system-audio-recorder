#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace sysrecord {

class Mp3Encoder final {
public:
    Mp3Encoder(const std::filesystem::path& outputPath, std::uint32_t sampleRate, std::uint32_t channels,
               int bitrateKbps, bool vbrMode, int vbrQuality);
    ~Mp3Encoder();

    Mp3Encoder(const Mp3Encoder&) = delete;
    Mp3Encoder& operator=(const Mp3Encoder&) = delete;

    void encode(std::span<const float> interleavedSamples, std::uint32_t frameCount);
    void finalize();

    [[nodiscard]] std::uint64_t bytesWritten() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sysrecord
