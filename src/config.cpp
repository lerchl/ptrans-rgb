#include "config.hpp"

int Time::toMinutes() const { return hour * 60 + minute; }

void from_json(const json &j, Time &t) {
    t.hour = j.at("hour").get<uint8_t>();
    t.minute = j.at("minute").get<uint8_t>();
}

void to_json(json &j, const Time &t) {
    j = json{{"hour", t.hour}, {"minute", t.minute}};
}

bool BlackoutWindow::isDuringBlackout(const Time &time) const {
    if (override) {
        return false;
    }

    int s = start.toMinutes();
    int e = end.toMinutes();
    int t = time.toMinutes();

    if (s <= e) {
        // Normal case (e.g., 08:00 → 17:00)
        return t >= s && t < e;
    } else {
        // Crosses midnight (e.g., 22:00 → 08:00)
        return t >= s || t < e;
    }
}

void from_json(const json &j, BlackoutWindow &b) {
    b.start = j.at("start").get<Time>();
    b.end = j.at("end").get<Time>();
    b.override = j.at("override").get<bool>();
}

void to_json(json &j, const BlackoutWindow &b) {
    j = json{{"start", b.start}, {"end", b.end}, {"override", b.override}};
}

void from_json(const json &j, Color &c) {
    j.at("r").get_to(c.r);
    j.at("g").get_to(c.g);
    j.at("b").get_to(c.b);
}

void to_json(json &j, const Color &c) {
    j = json{{"r", c.r}, {"g", c.g}, {"b", c.b}};
}

void from_json(const json &j, Colors &c) {
    j.at("fgDefault").get_to(c.fg_default);
    j.at("fgLate").get_to(c.fg_late);
    j.at("fgTraffic").get_to(c.fg_traffic);
    j.at("fgPunctual").get_to(c.fg_punctual);
}

void to_json(json &j, const Colors &c) {
    j = json{{"fgDefault", c.fg_default},
             {"fgLate", c.fg_late},
             {"fgTraffic", c.fg_traffic},
             {"fgPunctual", c.fg_punctual}};
}

Color::operator rgb_matrix::Color() const { return rgb_matrix::Color(r, g, b); }

void from_json(const json &j, PatchConfigurationDto &p) {
    if (j.contains("brightness")) {
        p.brightness = j.at("brightness").get<int>();
    }

    if (j.contains("mode")) {
        p.mode = j.at("mode").get<Mode>();
    }

    if (j.contains("blackoutWindow")) {
        if (j.at("blackoutWindow").is_null()) {
            p.blackout_window = std::nullopt;
        } else {
            p.blackout_window = j.at("blackoutWindow").get<BlackoutWindow>();
        }
    }
    if (j.contains("colors")) {
        p.colors = j.at("colors").get<Colors>();
    }
}

void to_json(json &j, const Configuration &c) {
    j = json{{"brightness", c.brightness},
             {"mode", c.mode},
             {"blackoutWindow", c.blackout_window},
             {"colors", c.colors}};
}
