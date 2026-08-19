add_executable(SysRecordTests
    recording_config_tests.cpp
)
target_link_libraries(SysRecordTests PRIVATE
    Catch2::Catch2WithMain
    sys_record_core
)

include(Catch)
catch_discover_tests(SysRecordTests)
