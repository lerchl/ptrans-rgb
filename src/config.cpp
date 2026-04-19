#include "config.hpp"

void from_json(const json &j, Time &t) {
    t.hour = j.at("hour").get<uint8_t>();
    t.minute = j.at("minute").get<uint8_t>();
}

void to_json(json &j, const Time &t) {
    j = json{{"hour", t.hour}, {"minute", t.minute}};
}

void from_json(const json &j, BlackoutWindow &b) {
    b.start = j.at("start").get<Time>();
    b.end = j.at("end").get<Time>();
}

void to_json(json &j, const BlackoutWindow &b) {
    j = json{{"start", b.start}, {"end", b.end}};
}

void from_json(const json &j, PatchConfigurationDto &p) {
    if (j.contains("brightness")) {
        p.brightness = j.at("brightness").get<int>();
    }

    if (j.contains("mode")) {
        p.mode = j.at("mode").get<Mode>();
    }

    if (j.contains("blackout_window")) {
        p.blackout_window = j.at("blackout_window").get<BlackoutWindow>();
    }
}

void to_json(json &j, const Configuration &c) {
    j = json{{"brightness", c.brightness},
             {"mode", c.mode},
             {"blackout_window", c.blackout_window}};
}
