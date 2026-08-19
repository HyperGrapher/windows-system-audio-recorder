#include "app_state.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("application state round-trips through JSON") {
    const AppState expected{"A custom tray app", 42};

    const AppState actual = deserializeAppState(serializeAppState(expected));

    REQUIRE(actual == expected);
}

TEST_CASE("missing JSON fields use starter defaults") {
    const AppState state = deserializeAppState("{}");

    REQUIRE(state.displayName == "Sys Record");
    REQUIRE(state.sampleCount == 0);
}

