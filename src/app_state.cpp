#include "app_state.hpp"

#include <nlohmann/json.hpp>

std::string serializeAppState(const AppState& state) {
    return nlohmann::json{
        {"displayName", state.displayName},
        {"sampleCount", state.sampleCount},
    }.dump();
}

AppState deserializeAppState(const std::string& jsonText) {
    const auto json = nlohmann::json::parse(jsonText);
    AppState state;
    state.displayName = json.value("displayName", std::string{"Sys Record"});
    state.sampleCount = json.value("sampleCount", 0);
    return state;
}
