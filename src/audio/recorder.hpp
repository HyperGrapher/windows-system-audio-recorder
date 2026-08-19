#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

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
    std::chrono::milliseconds silenceElapsed{};
    std::uint64_t bytesWritten{};
    std::uint64_t droppedFrames{};
    std::string errorMessage;
};

struct RecorderSettings {
    int bitrateKbps{192};
    bool vbrMode{false};
    int vbrQuality{4};
    std::string outputDeviceId{"default"};
};

struct OutputDevice {
    std::string id;
    std::string name;
};

[[nodiscard]] std::vector<OutputDevice> listOutputDevices();

class Recorder final {
public:
    Recorder();
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    void start(const std::filesystem::path& outputPath, const RecorderSettings& settings);
    void stop();
    void pause();
    void resume();

    [[nodiscard]] RecorderStatus status() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sysrecord
