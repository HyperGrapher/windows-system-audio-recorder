#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace sysrecord {

enum class RecorderState {
    Idle,
    Recording,
    Paused,
    Error,
};

struct RecorderStatus {
    RecorderState state{RecorderState::Idle};
    std::filesystem::path outputPath;
    std::chrono::milliseconds elapsed{};
    std::uint64_t bytesWritten{};
    std::uint64_t droppedFrames{};
    std::string errorMessage;
};

class Recorder final {
public:
    Recorder();
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    void start(const std::filesystem::path& outputPath, int bitrateKbps);
    void stop();
    void pause();
    void resume();

    [[nodiscard]] RecorderStatus status() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sysrecord
