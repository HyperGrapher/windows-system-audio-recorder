#include "config/recording_config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>

namespace sysrecord {
namespace {

constexpr std::string_view kDefaultFilenamePattern{"SysRecord_{yyyy-MM-dd}_{HH-mm-ss}.mp3"};
constexpr std::array kSupportedBitrates{128, 192, 256, 320};

[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] std::filesystem::path pathFromUtf8(const std::string& value) {
    return std::filesystem::path{std::u8string{
        reinterpret_cast<const char8_t*>(value.data()), reinterpret_cast<const char8_t*>(value.data() + value.size())}};
}

void replaceAll(std::string& value, const std::string_view token, const std::string_view replacement) {
    std::size_t position = 0;
    while ((position = value.find(token, position)) != std::string::npos) {
        value.replace(position, token.size(), replacement);
        position += replacement.size();
    }
}

[[nodiscard]] std::string twoDigits(const int value) {
    std::ostringstream output;
    output << std::setfill('0') << std::setw(2) << value;
    return output.str();
}

[[nodiscard]] bool hasMp3Extension(const std::string& value) {
    if (value.size() < 4) {
        return false;
    }

    std::string extension = value.substr(value.size() - 4);
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return extension == ".mp3";
}

[[nodiscard]] int boundedValue(const nlohmann::json& json, const char* key, const int fallback, const int minimum,
                               const int maximum) {
    const int value = json.value(key, fallback);
    return value >= minimum && value <= maximum ? value : fallback;
}

}  // namespace

std::string serializeConfig(const RecordingConfig& config) {
    return nlohmann::json{
        {"outputFolder", pathToUtf8(config.outputFolder)},
        {"filenamePattern", config.filenamePattern},
        {"bitrateKbps", config.bitrateKbps},
        {"vbrMode", config.vbrMode},
        {"vbrQuality", config.vbrQuality},
        {"outputDeviceId", config.outputDeviceId},
        {"hotkeyEnabled", config.hotkeyEnabled},
        {"hotkey", config.hotkey},
        {"launchAtStartup", config.launchAtStartup},
        {"showNotifications", config.showNotifications},
        {"silenceTimeoutSeconds", config.silenceTimeoutSeconds},
        {"maxRecordingMinutes", config.maxRecordingMinutes},
    }.dump(2);
}

RecordingConfig deserializeConfig(const std::string& jsonText, const RecordingConfig& defaults) {
    const nlohmann::json json = nlohmann::json::parse(jsonText);
    RecordingConfig config = defaults;

    config.outputFolder = pathFromUtf8(json.value("outputFolder", pathToUtf8(defaults.outputFolder)));
    config.filenamePattern = json.value("filenamePattern", defaults.filenamePattern);
    if (config.filenamePattern.empty()) {
        config.filenamePattern = std::string{kDefaultFilenamePattern};
    }

    const int bitrate = json.value("bitrateKbps", defaults.bitrateKbps);
    config.bitrateKbps = std::ranges::find(kSupportedBitrates, bitrate) != kSupportedBitrates.end()
                               ? bitrate
                               : defaults.bitrateKbps;
    config.vbrMode = json.value("vbrMode", defaults.vbrMode);
    config.vbrQuality = boundedValue(json, "vbrQuality", defaults.vbrQuality, 0, 9);
    config.outputDeviceId = json.value("outputDeviceId", defaults.outputDeviceId);
    config.hotkeyEnabled = json.value("hotkeyEnabled", defaults.hotkeyEnabled);
    config.hotkey = json.value("hotkey", defaults.hotkey);
    config.launchAtStartup = json.value("launchAtStartup", defaults.launchAtStartup);
    config.showNotifications = json.value("showNotifications", defaults.showNotifications);
    config.silenceTimeoutSeconds =
        boundedValue(json, "silenceTimeoutSeconds", defaults.silenceTimeoutSeconds, 0, 3600);
    config.maxRecordingMinutes = boundedValue(json, "maxRecordingMinutes", defaults.maxRecordingMinutes, 1, 300);
    return config;
}

RecordingConfig loadConfig(const std::filesystem::path& path, const RecordingConfig& defaults) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return defaults;
    }

    try {
        const std::string jsonText{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        return deserializeConfig(jsonText, defaults);
    } catch (const std::exception&) {
        return defaults;
    }
}

void saveConfig(const std::filesystem::path& path, const RecordingConfig& config) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Unable to open the configuration file.");
    }

    output << serializeConfig(config) << '\n';
    if (!output) {
        throw std::runtime_error("Unable to write the configuration file.");
    }
}

std::string formatRecordingFilename(const std::string& pattern, const std::tm& localTime) {
    std::string filename = pattern.empty() ? std::string{kDefaultFilenamePattern} : pattern;
    replaceAll(filename, "{yyyy}", std::to_string(localTime.tm_year + 1900));
    replaceAll(filename, "{MM}", twoDigits(localTime.tm_mon + 1));
    replaceAll(filename, "{dd}", twoDigits(localTime.tm_mday));
    replaceAll(filename, "{HH}", twoDigits(localTime.tm_hour));
    replaceAll(filename, "{mm}", twoDigits(localTime.tm_min));
    replaceAll(filename, "{ss}", twoDigits(localTime.tm_sec));

    constexpr std::string_view invalidCharacters{"<>:\"/\\|?*"};
    for (char& character : filename) {
        const auto unsignedCharacter = static_cast<unsigned char>(character);
        if (unsignedCharacter < 32 || invalidCharacters.find(character) != std::string_view::npos) {
            character = '_';
        }
    }
    while (!filename.empty() && (filename.back() == ' ' || filename.back() == '.')) {
        filename.back() = '_';
    }
    if (filename.empty() || filename == "." || filename == "..") {
        filename = "SysRecord";
    }
    if (!hasMp3Extension(filename)) {
        filename += ".mp3";
    }
    return filename;
}

}  // namespace sysrecord
