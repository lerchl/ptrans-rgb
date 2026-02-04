#include "graphics.h"
#include "led-matrix.h"

#include <getopt.h>
#include <httplib.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <vector>

using namespace rgb_matrix;

volatile bool interrupt_received = false;
static void InterruptHandler(int signo) { interrupt_received = true; }

static int usage(const char *progname) {
    fprintf(stderr, "usage: %s [options]\n", progname);
    fprintf(stderr, "Reads text from stdin and displays it. "
                    "Empty string: clear screen\n");
    fprintf(stderr, "Options:\n");
    fprintf(
        stderr,
        "\t-d <time-format>  : Default '%%H:%%M'. See strftime()\n"
        "\t                    Can be provided multiple times for multiple "
        "lines\n"
        "\t-f <font-file>    : Use given font.\n"
        "\t-x <x-origin>     : X-Origin of displaying text (Default: 0)\n"
        "\t-y <y-origin>     : Y-Origin of displaying text (Default: 0)\n"
        "\t-s <line-spacing> : Extra spacing between lines when multiple -d "
        "given\n"
        "\t-S <spacing>      : Extra spacing between letters (Default: 0)\n"
        "\t-C <r,g,b>        : Color. Default 255,255,0\n"
        "\t-B <r,g,b>        : Background-Color. Default 0,0,0\n"
        "\t-O <r,g,b>        : Outline-Color, e.g. to increase contrast.\n"
        "\n");
    rgb_matrix::PrintMatrixFlags(stderr);
    return 1;
}

static bool parseColor(Color *c, const char *str) {
    return sscanf(str, "%hhu,%hhu,%hhu", &c->r, &c->g, &c->b) == 3;
}

static bool FullSaturation(const Color &c) {
    return (c.r == 0 || c.r == 255) && (c.g == 0 || c.g == 255) &&
           (c.b == 0 || c.b == 255);
}

using json = nlohmann::json;

struct DepartureDto {
    std::optional<std::string> direction;
    std::string when;
    std::optional<std::string> when_actually;
    bool traffic_jam;
};

inline void from_json(const json &j, DepartureDto &d) {
    d.direction = j.value("direction", std::optional<std::string>{});
    d.when = j.at("when").get<std::string>();
    d.when_actually = j.value("when_actually", std::optional<std::string>{});
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
    httplib::Client cli("10.0.0.164:3000");

    TimetableDto timetable;
    if (auto res = cli.Get("/timetable")) {
        timetable = parse_timetable(res->body);
    }

    RGBMatrix::Options matrix_options;
    rgb_matrix::RuntimeOptions runtime_opt;
    if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv, &matrix_options,
                                           &runtime_opt)) {
        return usage(argv[0]);
    }
    //
    // // We accept multiple format lines
    //
    // std::vector<std::string> format_lines;
    Color color(100, 0, 255);
    Color bg_color(0, 0, 0);
    // Color outline_color(0, 0, 0);
    // bool with_outline = false;
    //
    const char *bdf_font_file = NULL;
    // int x_orig = 0;
    // int y_orig = 0;
    // int letter_spacing = 0;
    // int line_spacing = 0;
    //
    int opt;
    while ((opt = getopt(argc, argv, "x:y:f:C:B:O:s:S:d:")) != -1) {
        switch (opt) {
        // case 'd':
        //     format_lines.push_back(optarg);
        //     break;
        // case 'x':
        //     x_orig = atoi(optarg);
        //     break;
        // case 'y':
        //     y_orig = atoi(optarg);
        //     break;
        case 'f':
            bdf_font_file = strdup(optarg);
            break;
        // case 's':
        //     line_spacing = atoi(optarg);
        //     break;
        // case 'S':
        //     letter_spacing = atoi(optarg);
        //     break;
        // case 'C':
        //     if (!parseColor(&color, optarg)) {
        //         fprintf(stderr, "Invalid color spec: %s\n", optarg);
        //         return usage(argv[0]);
        //     }
        //     break;
        // case 'B':
        //     if (!parseColor(&bg_color, optarg)) {
        //         fprintf(stderr, "Invalid background color spec: %s\n",
        //         optarg); return usage(argv[0]);
        //     }
        //     break;
        // case 'O':
        //     if (!parseColor(&outline_color, optarg)) {
        //         fprintf(stderr, "Invalid outline color spec: %s\n", optarg);
        //         return usage(argv[0]);
        //     }
        //     with_outline = true;
        //     break;
        default:
            return usage(argv[0]);
        }
    }
    //
    // if (format_lines.empty()) {
    //     format_lines.push_back("%H:%M");
    // }
    //
    if (bdf_font_file == NULL) {
        fprintf(stderr, "Need to specify BDF font-file with -f\n");
        return usage(argv[0]);
    }
    //
    // /*
    //  * Load font. This needs to be a filename with a bdf bitmap font.
    //  */
    rgb_matrix::Font font;
    if (!font.LoadFont(bdf_font_file)) {
        fprintf(stderr, "Couldn't load font '%s'\n", bdf_font_file);
        return 1;
    }

    RGBMatrix *matrix =
        RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
    if (matrix == NULL)
        return 1;
    //
    // const bool all_extreme_colors =
    //     (matrix_options.brightness == 100) && FullSaturation(color) &&
    //     FullSaturation(bg_color) && FullSaturation(outline_color);
    // if (all_extreme_colors)
    //     matrix->SetPWMBits(1);
    //
    // const int x = x_orig;
    // int y = y_orig;
    //
    FrameCanvas *offscreen = matrix->CreateFrameCanvas();
    //
    // struct timespec next_time;
    // next_time.tv_sec = time(NULL);
    // next_time.tv_nsec = 0;
    // struct tm tm;
    //
    signal(SIGTERM, InterruptHandler);
    signal(SIGINT, InterruptHandler);

    using namespace std::chrono;

    // Example ISO 8601 UTC string
    // Parse the ISO string into a time_point
    utc_time<seconds> tp_from_str;
    std::istringstream in{timetable.trips[0].departures[0].when};
    in >> parse("%Y-%m-%dT%H:%M:%SZ", tp_from_str);
    if (in.fail()) {
        std::cerr << "Failed to parse ISO datetime.\n";
        return 1;
    }

    // Get current UTC time
    auto now_tp = utc_clock::now();

    // Compute difference in minutes
    auto diff_minutes = duration_cast<minutes>(tp_from_str - now_tp).count();

    //
    while (!interrupt_received) {
        offscreen->Fill(bg_color.r, bg_color.g, bg_color.b);
        rgb_matrix::DrawText(
            offscreen, font, 0, 0 + font.baseline(), color, NULL,
            (timetable.trips[0].line + " " + timetable.trips[0].direction +
             " " + std::to_string(diff_minutes))
                .c_str(),
            0);

        // Atomic swap with double buffer
        offscreen = matrix->SwapOnVSync(offscreen);
    }

    // Finished. Shut down the RGB matrix.
    delete matrix;

    std::cout << std::endl; // Create a fresh new line after ^C on screen
    return 0;
}
