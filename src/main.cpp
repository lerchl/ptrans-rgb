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
    fprintf(stderr, "\t-f <font-file>    : Use given font.\n");
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

int main(int argc, char *argv[]) {
    RGBMatrix::Options matrix_options;
    rgb_matrix::RuntimeOptions runtime_opt;
    if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv, &matrix_options,
                                           &runtime_opt)) {
        return usage(argv[0]);
    }
    Color color(100, 0, 255);
    Color bg_color(0, 0, 0);
    const char *bdf_font_file = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "x:y:f:C:B:O:s:S:d:")) != -1) {
        switch (opt) {
        case 'f':
            bdf_font_file = strdup(optarg);
            break;
        default:
            return usage(argv[0]);
        }
    }

    if (bdf_font_file == NULL) {
        fprintf(stderr, "Need to specify BDF font-file with -f\n");
        return usage(argv[0]);
    }

    rgb_matrix::Font font;
    if (!font.LoadFont(bdf_font_file)) {
        fprintf(stderr, "Couldn't load font '%s'\n", bdf_font_file);
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
        }

        offscreen->Fill(bg_color.r, bg_color.g, bg_color.b);

        for (int index : std::views::iota(0, (int)timetable.trips.size())) {
            std::string line_name = timetable.trips[index].line;
            std::string direction = timetable.trips[index].direction;
            int countdown = timetable.trips[index].departures[0].countdown;
            bool real_time = timetable.trips[index].departures[0].real_time;

            std::string line =
                std::format("{:<3} {:<10} {:>3}", line_name, direction,
                            (real_time ? "\"" : " ") + countdown);

            rgb_matrix::DrawText(offscreen, font, 0,
                                 0 + (index + 1) * font.baseline() +
                                     (index > 0 ? 8 : 0),
                                 color, NULL, line.c_str(), 0);
        }

        // Atomic swap with double buffer
        offscreen = matrix->SwapOnVSync(offscreen);
        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
}
