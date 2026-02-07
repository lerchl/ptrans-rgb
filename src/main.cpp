#include "graphics.h"
#include "led-matrix.h"

#include <cstdlib>
#include <getopt.h>
#include <httplib.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <ranges>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <vector>

using namespace rgb_matrix;
using json = nlohmann::json;

RGBMatrix *matrix;

static void interrupt_handler(int signo) {
    (void)signo;
    delete matrix;
    std::cout << std::endl;
    exit(0);
}

static int usage(const char *progname) {
    fprintf(stderr, "usage: %s [options]\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "\t-f <font-file>    : Use given font for small text.\n");
    fprintf(stderr, "\t-F <font-file>    : Use given font for large text.\n");
    rgb_matrix::PrintMatrixFlags(stderr);
    return 1;
}

struct DepartureDto {
    std::optional<std::string> direction;
    int countdown;
    bool real_time;
    bool late;
    bool traffic_jam;
};

inline void from_json(const json &j, DepartureDto &d) {
    d.direction = j.value("direction", std::optional<std::string>{});
    d.countdown = j.at("countdown").get<int>();
    d.real_time = j.at("real_time").get<bool>();
    d.late = j.at("late").get<bool>();
    d.traffic_jam = j.at("traffic_jam").get<bool>();
}

struct TripDto {
    std::string line;
    std::string direction;
    int foot_minutes_to_station;
    std::vector<DepartureDto> departures;
};

inline void from_json(const json &j, TripDto &t) {
    t.line = j.at("line").get<std::string>();
    t.direction = j.at("direction").get<std::string>();
    t.foot_minutes_to_station = j.at("foot_minutes_to_station").get<int>();
    t.departures = j.at("departures").get<std::vector<DepartureDto>>();
}

struct TimetableDto {
    std::vector<TripDto> trips;
    std::optional<std::string> message;
};

inline void from_json(const json &j, TimetableDto &tt) {
    tt.trips = j.at("trips").get<std::vector<TripDto>>();
    tt.message = j.value("message", std::optional<std::string>{});
}

inline TimetableDto parse_timetable(const std::string &body) {
    json j = json::parse(body);
    return j.get<TimetableDto>();
}

std::string real_time_indicator(bool real_time, bool late, bool traffic_jam) {
    if (traffic_jam) {
        return "t";
    } else if (late) {
        return "l";
    } else if (real_time) {
        return "\"";
    }

    return "";
}

int main(int argc, char *argv[]) {
    RGBMatrix::Options matrix_options;
    rgb_matrix::RuntimeOptions runtime_opt;
    if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv, &matrix_options,
                                           &runtime_opt)) {
        return usage(argv[0]);
    }
    Color color(100, 0, 255);
    Color bg_color(0, 0, 0);

    const char *bdf_font_file_small = NULL;
    const char *bdf_font_file_large = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "f:F:")) != -1) {
        switch (opt) {
        case 'f':
            bdf_font_file_small = strdup(optarg);
            break;
        case 'F':
            bdf_font_file_large = strdup(optarg);
            break;
        default:
            return usage(argv[0]);
        }
    }

    if (bdf_font_file_small == NULL) {
        fprintf(stderr, "Need to specify a 5x8 BDF font-file with -f\n");
        return usage(argv[0]);
    }

    if (bdf_font_file_large == NULL) {
        fprintf(stderr, "Need to specify a 6x12 BDF font-file with -F\n");
        return usage(argv[0]);
    }

    rgb_matrix::Font font_small;
    if (!font_small.LoadFont(bdf_font_file_small)) {
        fprintf(stderr, "Couldn't load font '%s'\n", bdf_font_file_small);
        return 1;
    }

    rgb_matrix::Font font_large;
    if (!font_large.LoadFont(bdf_font_file_large)) {
        fprintf(stderr, "Couldn't load font '%s'\n", bdf_font_file_large);
        return 1;
    }

    matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
    if (matrix == NULL) {
        return 1;
    }

    FrameCanvas *offscreen = matrix->CreateFrameCanvas();

    signal(SIGTERM, interrupt_handler);
    signal(SIGINT, interrupt_handler);

    httplib::Client cli("10.0.0.164:3000");

    for (;;) {
        TimetableDto timetable;
        if (auto res = cli.Get("/timetable")) {
            timetable = parse_timetable(res->body);
        } else {
            std::cerr << "Failed to fetch timetable" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(30));
            continue;
        }

        offscreen->Fill(bg_color.r, bg_color.g, bg_color.b);

        // for (int i : std::views::iota(0, (int)timetable.trips.size())) {
        for (int i : std::views::iota(0, 1)) {
            std::string line_name = timetable.trips[i].line;
            std::string direction = timetable.trips[i].direction;

            if (timetable.trips[i].departures.empty()) {
                std::string line = std::format("{:<3} {:<13} {:>3}", line_name,
                                               direction, "N/A");

                rgb_matrix::DrawText(offscreen, font_large, 0,
                                     0 + (i + 1) * font_large.baseline() +
                                         i * 4,
                                     color, NULL, line.c_str(), 0);
                continue;
            }

            int countdown = timetable.trips[i].departures[0].countdown;
            bool real_time = timetable.trips[i].departures[0].real_time;
            bool late = timetable.trips[i].departures[0].late;
            bool traffic_jam = timetable.trips[i].departures[0].traffic_jam;

            std::string line = std::format(
                "{:<3} {:<13} {:>3}", line_name, direction,
                real_time_indicator(real_time, late, traffic_jam) +
                    (countdown == 0 ? "*" : std::to_string(countdown)));

            rgb_matrix::DrawText(offscreen, font_large, 0,
                                 0 + (i + 1) * font_large.baseline() + i * 4,
                                 color, NULL, line.c_str(), 0);

            if (timetable.trips[i].departures.size() > 1) {
                std::string str = "";

                for (auto &&s :
                     timetable.trips[i].departures | std::views::drop(1) |
                         std::views::take(3) |
                         std::views::transform([](DepartureDto &departure) {
                             return real_time_indicator(departure.real_time,
                                                        departure.late,
                                                        departure.traffic_jam) +
                                    std::to_string(departure.countdown);
                         })) {
                    str += ((str.length() == 0 ? "" : ", ") + s);
                }

                std::string line = std::format("{:>31}", str);

                rgb_matrix::DrawText(offscreen, font_small, 0,
                                     0 + (i + 1) * font_large.baseline() +
                                         i * 4 + 4 + font_small.baseline(),
                                     color, NULL, line.c_str(), 0);
            }
        }

        // Atomic swap with double buffer
        offscreen = matrix->SwapOnVSync(offscreen);
        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
}
