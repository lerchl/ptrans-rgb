#pragma once
#include <optional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

enum Mode { PTRANS, TEXT };

struct Time {
    int hour;
    int minute;
};

void from_json(const json &j, Time &t);
void to_json(json &j, const Time &t);

struct BlackoutWindow {
    Time start;
    Time end;
    bool override;
};

void from_json(const json &j, BlackoutWindow &b);
void to_json(json &j, const BlackoutWindow &b);

struct Configuration {
    Mode mode;
    int brightness;
    std::optional<BlackoutWindow> blackout_window;
};

struct PatchConfigurationDto {
    std::optional<int> brightness;
    std::optional<Mode> mode;
    std::optional<BlackoutWindow> blackout_window;
};

void from_json(const json &j, PatchConfigurationDto &p);
void to_json(json &j, const Configuration &c);
