#pragma once

#include <ctime>
#include <filesystem>
#include <string>

namespace sysrecord {

struct RecordingConfig {
    std::filesystem::path outputFolder;
    std::string filenamePattern{"SysRecord_{yyyy-MM-dd}_{HH-mm-ss}.mp3"};
    int bitrateKbps{192};
    bool vbrMode{false};
    int vbrQuality{4};
    std::string outputDeviceId{"default"};
    bool hotkeyEnabled{false};
    std::string hotkey{"Ctrl+Alt+R"};
    bool launchAtStartup{false};
    bool showNotifications{true};
    int silenceTimeoutSeconds{30};
    int maxRecordingMinutes{300};

    bool operator==(const RecordingConfig&) const = default;
};

[[nodiscard]] std::string serializeConfig(const RecordingConfig& config);
[[nodiscard]] RecordingConfig deserializeConfig(const std::string& jsonText, const RecordingConfig& defaults);
[[nodiscard]] RecordingConfig loadConfig(const std::filesystem::path& path, const RecordingConfig& defaults);
void saveConfig(const std::filesystem::path& path, const RecordingConfig& config);

[[nodiscard]] std::string formatRecordingFilename(const std::string& pattern, const std::tm& localTime);

}  // namespace sysrecord
