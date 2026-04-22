#pragma once
#include "graphics.h"
#include <nlohmann/json.hpp>
#include <optional>

using json = nlohmann::json;

enum Mode { PTRANS, TEXT };

struct Time {
    int hour;
    int minute;

    int toMinutes() const;
};
void from_json(const json &j, Time &t);
void to_json(json &j, const Time &t);

struct BlackoutWindow {
    Time start;
    Time end;
    bool override;

    bool isDuringBlackout(const Time&) const;
};
void from_json(const json &j, BlackoutWindow &b);
void to_json(json &j, const BlackoutWindow &b);

struct Color {
    uint8_t r;
    uint8_t g;
    uint8_t b;

    operator rgb_matrix::Color() const;
};
void from_json(const json &j, Color &c);
void to_json(json &j, const Color &c);

struct Colors {
    Color fg_default;
    Color fg_late;
    Color fg_traffic;
    Color fg_punctual;
};
void from_json(const json &j, Colors &c);
void to_json(json &j, const Colors &c);

struct Configuration {
    Mode mode;
    int brightness;
    std::optional<BlackoutWindow> blackout_window;
    Colors colors;
};

struct PatchConfigurationDto {
    std::optional<int> brightness;
    std::optional<Mode> mode;
    std::optional<BlackoutWindow> blackout_window;
    std::optional<Colors> colors;
};
void from_json(const json &j, PatchConfigurationDto &p);
void to_json(json &j, const Configuration &c);
