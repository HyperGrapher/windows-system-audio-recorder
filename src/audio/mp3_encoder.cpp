#include "audio/mp3_encoder.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <lame/lame.h>

namespace sysrecord {
namespace {

void requireLameSuccess(const int result, const char* operation) {
    if (result < 0) {
        throw std::runtime_error(std::string{"LAME failed to "} + operation + '.');
    }
}

}  // namespace

class Mp3Encoder::Impl final {
public:
    Impl(const std::filesystem::path& outputPath, const std::uint32_t sampleRate, const std::uint32_t channels,
         const int bitrateKbps, const bool vbrMode, const int vbrQuality)
        : output_(outputPath, std::ios::binary | std::ios::trunc), channels_(channels) {
        if (!output_) {
            throw std::runtime_error("Unable to create the MP3 output file.");
        }
        if (channels != 1 && channels != 2) {
            throw std::invalid_argument("LAME encoding requires mono or stereo PCM input.");
        }
        if (sampleRate > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("The capture sample rate is unsupported.");
        }

        lame_ = lame_init();
        if (lame_ == nullptr) {
            throw std::runtime_error("Unable to initialize LAME.");
        }

        try {
            requireLameSuccess(lame_set_num_channels(lame_, static_cast<int>(channels)), "set the channel count");
            requireLameSuccess(lame_set_in_samplerate(lame_, static_cast<int>(sampleRate)), "set the input sample rate");
            if (vbrMode) {
                requireLameSuccess(lame_set_VBR(lame_, vbr_mtrh), "select variable bitrate encoding");
                requireLameSuccess(lame_set_VBR_q(lame_, vbrQuality), "set the variable bitrate quality");
            } else {
                requireLameSuccess(lame_set_VBR(lame_, vbr_off), "select constant bitrate encoding");
                requireLameSuccess(lame_set_brate(lame_, bitrateKbps), "set the bitrate");
            }
            requireLameSuccess(lame_set_quality(lame_, 2), "set the encoding quality");
            requireLameSuccess(lame_set_bWriteVbrTag(lame_, 0), "disable the VBR tag");
            requireLameSuccess(lame_init_params(lame_), "finish initialization");
        } catch (...) {
            lame_close(lame_);
            lame_ = nullptr;
            throw;
        }
    }

    ~Impl() {
        if (lame_ != nullptr) {
            lame_close(lame_);
        }
    }

    void encode(const std::span<const float> interleavedSamples, const std::uint32_t frameCount) {
        if (isFinalized_) {
            throw std::logic_error("Cannot encode after the MP3 stream has been finalized.");
        }
        if (interleavedSamples.size() < static_cast<std::size_t>(frameCount) * channels_) {
            throw std::invalid_argument("The PCM buffer is smaller than the requested frame count.");
        }
        if (frameCount > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("The PCM buffer is too large for LAME.");
        }

        const std::size_t requiredBytes = static_cast<std::size_t>(frameCount) * 5 / 4 + 7200;
        encodedBytes_.resize(requiredBytes);

        int encodedByteCount = 0;
        if (channels_ == 1) {
            encodedByteCount = lame_encode_buffer_ieee_float(
                lame_, interleavedSamples.data(), interleavedSamples.data(), static_cast<int>(frameCount),
                encodedBytes_.data(), static_cast<int>(encodedBytes_.size()));
        } else {
            encodedByteCount = lame_encode_buffer_interleaved_ieee_float(
                lame_, interleavedSamples.data(), static_cast<int>(frameCount), encodedBytes_.data(),
                static_cast<int>(encodedBytes_.size()));
        }
        requireLameSuccess(encodedByteCount, "encode PCM audio");
        writeEncodedBytes(encodedByteCount);
    }

    void finalize() {
        if (isFinalized_) {
            return;
        }

        encodedBytes_.resize(7200);
        const int encodedByteCount =
            lame_encode_flush(lame_, encodedBytes_.data(), static_cast<int>(encodedBytes_.size()));
        requireLameSuccess(encodedByteCount, "finalize the MP3 stream");
        writeEncodedBytes(encodedByteCount);
        output_.flush();
        if (!output_) {
            throw std::runtime_error("Unable to flush the MP3 output file.");
        }
        isFinalized_ = true;
    }

    [[nodiscard]] std::uint64_t bytesWritten() const {
        return bytesWritten_;
    }

private:
    void writeEncodedBytes(const int encodedByteCount) {
        if (encodedByteCount == 0) {
            return;
        }

        output_.write(reinterpret_cast<const char*>(encodedBytes_.data()), encodedByteCount);
        if (!output_) {
            throw std::runtime_error("Unable to write to the MP3 output file. The disk may be full.");
        }
        bytesWritten_ += static_cast<std::uint64_t>(encodedByteCount);
    }

    std::ofstream output_;
    lame_t lame_{};
    std::vector<unsigned char> encodedBytes_;
    std::uint32_t channels_{};
    std::uint64_t bytesWritten_{};
    bool isFinalized_{};
};

Mp3Encoder::Mp3Encoder(const std::filesystem::path& outputPath, const std::uint32_t sampleRate,
                       const std::uint32_t channels, const int bitrateKbps, const bool vbrMode, const int vbrQuality)
    : impl_(std::make_unique<Impl>(outputPath, sampleRate, channels, bitrateKbps, vbrMode, vbrQuality)) {}

Mp3Encoder::~Mp3Encoder() = default;

void Mp3Encoder::encode(const std::span<const float> interleavedSamples, const std::uint32_t frameCount) {
    impl_->encode(interleavedSamples, frameCount);
}

void Mp3Encoder::finalize() {
    impl_->finalize();
}

std::uint64_t Mp3Encoder::bytesWritten() const {
    return impl_->bytesWritten();
}

}  // namespace sysrecord
