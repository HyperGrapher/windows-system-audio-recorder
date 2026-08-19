#include "config/recording_config.hpp"

#include <ctime>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

using sysrecord::RecordingConfig;

TEST_CASE("recording configuration round-trips through JSON") {
    RecordingConfig expected;
    expected.outputFolder = std::filesystem::path{L"C:\\Recordings"};
    expected.filenamePattern = "Audio_{yyyy}-{MM}-{dd}_{HH}-{mm}-{ss}.mp3";
    expected.bitrateKbps = 320;
    expected.hotkeyEnabled = true;
    expected.launchAtStartup = true;
    expected.silenceTimeoutSeconds = 45;
    expected.maxRecordingMinutes = 120;

    const RecordingConfig actual = sysrecord::deserializeConfig(sysrecord::serializeConfig(expected), {});

    REQUIRE(actual == expected);
}

TEST_CASE("missing JSON fields retain supplied defaults") {
    RecordingConfig defaults;
    defaults.outputFolder = std::filesystem::path{L"D:\\Audio"};
    defaults.bitrateKbps = 256;

    const RecordingConfig config = sysrecord::deserializeConfig("{}", defaults);

    REQUIRE(config == defaults);
}

TEST_CASE("invalid bounded settings fall back to defaults") {
    const RecordingConfig config = sysrecord::deserializeConfig(
        R"({"bitrateKbps": 123, "vbrQuality": 10, "silenceTimeoutSeconds": -1, "maxRecordingMinutes": 301})",
        {});

    REQUIRE(config.bitrateKbps == 192);
    REQUIRE(config.vbrQuality == 4);
    REQUIRE(config.silenceTimeoutSeconds == 30);
    REQUIRE(config.maxRecordingMinutes == 300);
}

TEST_CASE("filename patterns expand timestamps and cannot create nested paths") {
    std::tm localTime{};
    localTime.tm_year = 126;
    localTime.tm_mon = 7;
    localTime.tm_mday = 19;
    localTime.tm_hour = 9;
    localTime.tm_min = 5;
    localTime.tm_sec = 3;

    REQUIRE(sysrecord::formatRecordingFilename("Capture/{yyyy}-{MM}-{dd}_{HH}-{mm}-{ss}", localTime) ==
            "Capture_2026-08-19_09-05-03.mp3");
}

TEST_CASE("the PRD default filename pattern expands its composite tokens") {
    std::tm localTime{};
    localTime.tm_year = 126;
    localTime.tm_mon = 7;
    localTime.tm_mday = 19;
    localTime.tm_hour = 9;
    localTime.tm_min = 5;
    localTime.tm_sec = 3;

    REQUIRE(sysrecord::formatRecordingFilename("SysRecord_{yyyy-MM-dd}_{HH-mm-ss}.mp3", localTime) ==
            "SysRecord_2026-08-19_09-05-03.mp3");
}
