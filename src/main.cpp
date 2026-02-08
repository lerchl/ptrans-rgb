#include "graphics.h"
#include "led-matrix.h"

#include <atomic>
#include <cstdlib>
#include <getopt.h>
#include <httplib.h>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <vector>

using json = nlohmann::json;

enum Mode { PTRANS, TEXT };

rgb_matrix::RGBMatrix *matrix;
httplib::Server server;
std::thread http_thread;
std::thread ptrans_thread;

std::atomic<Mode> mode{PTRANS};
std::atomic<int> brightness{80};
std::atomic<std::shared_ptr<const std::string>> text;

static void interrupt_handler(int signo) {
    (void)signo;
    delete matrix;
    server.stop();
    http_thread.join();
    ptrans_thread.join();
    std::cout << std::endl;
    exit(0);
}

static int usage(const char *progname) {
    fprintf(stderr, "usage: %s [options]\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr,
            "\t-f <font-file>    : Use given font for small text (5x8).\n");
    fprintf(stderr,
            "\t-F <font-file>    : Use given font for large text (6x12).\n");
    rgb_matrix::PrintMatrixFlags(stderr);
    return 1;
}

struct ModeDto {
    Mode mode;
};

inline void to_json(json &j, const ModeDto &m) { j = json{{"mode", m.mode}}; }

inline void from_json(const json &j, ModeDto &m) {
    m.mode = j.at("mode").get<Mode>();
}

struct BrightnessDto {
    int brightness;
};

inline void to_json(json &j, const BrightnessDto &b) {
    j = json{{"brightness", b.brightness}};
}

inline void from_json(const json &j, BrightnessDto &b) {
    b.brightness = j.at("brightness").get<int>();
}

struct TextDto {
    std::string text;
};

inline void from_json(const json &j, TextDto &t) {
    t.text = j.at("text").get<std::string>();
}

void http_server() {
    server.Get("/mode",
               [](const httplib::Request &req, httplib::Response &res) {
                   (void)req;
                   ModeDto dto{.mode = mode.load()};
                   json j = dto;
                   res.status = 200;
                   res.set_content(j.dump(), "application/json");
               });
    server.Post(
        "/mode", [](const httplib::Request &req, httplib::Response &res) {
            try {
                Mode new_mode = json::parse(req.body).get<ModeDto>().mode;
                mode.store(new_mode);
                res.status = 200;
            } catch (const json::parse_error &e) {
                res.status = 400;
            }
        });

    server.Get("/brightness",
               [](const httplib::Request &req, httplib::Response &res) {
                   (void)req;
                   BrightnessDto dto{.brightness = brightness.load()};
                   json j = dto;
                   res.status = 200;
                   res.set_content(j.dump(), "application/json");
               });

    server.Post(
        "/brightness", [](const httplib::Request &req, httplib::Response &res) {
            try {
                int new_brightness =
                    json::parse(req.body).get<BrightnessDto>().brightness;

                if (new_brightness < 0 || new_brightness > 100) {
                    res.status = 400;
                    return;
                }

                brightness.store(new_brightness);
                res.status = 200;
            } catch (const json::parse_error &e) {
                res.status = 400;
            }
        });

    server.Post("/text",
                [](const httplib::Request &req, httplib::Response &res) {
                    try {
                        auto new_text = std::make_shared<std::string>(
                            json::parse(req.body).get<TextDto>().text);
                        text.store(new_text, std::memory_order_release);
                        res.status = 200;
                    } catch (const json::parse_error &e) {
                        res.status = 400;
                    }
                });

    server.listen("0.0.0.0", 8080);
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

struct ErrorDto {
    std::string message;
};

inline void from_json(const json &j, ErrorDto &e) {
    e.message = j.at("message").get<std::string>();
}

inline ErrorDto parse_error(const std::string &body) {
    json j = json::parse(body);
    return j.get<ErrorDto>();
}

std::atomic<std::shared_ptr<TimetableDto>> timetable;

void ptrans_job() {
    httplib::Client cli("10.0.0.164:3000");
    auto result = cli.Get("/timetable");

    if (result && result->status == 200) {
        timetable.store(
            std::make_shared<TimetableDto>(parse_timetable(result->body)),
            std::memory_order_release);
    } else {
        ErrorDto error = parse_error(result->body);
        std::string formatted_time =
            std::format("{0:%F_%T}", std::chrono::system_clock::now());
        std::cerr << std::format("{} - {} Could not fetch timetable: {}",
                                 formatted_time, result->status, error.message)
                  << std::endl;
    }

    std::this_thread::sleep_for(std::chrono::seconds(30));
}

std::string real_time_indicator(bool real_time, bool late, bool traffic_jam) {
    if (traffic_jam) {
        return "t";
    } else if (late) {
        return ".";
    } else if (real_time) {
        return "\"";
    }

    return "";
}

int write_line(rgb_matrix::FrameCanvas *canvas, rgb_matrix::Font &font, int y,
               rgb_matrix::Color color, std::string text) {
    rgb_matrix::DrawText(canvas, font, 0, y, color, NULL, text.c_str(), 0);

    // returns the y position for the next line
    return y + font.baseline() + 4;
}

int main(int argc, char *argv[]) {
    rgb_matrix::RGBMatrix::Options matrix_options;
    rgb_matrix::RuntimeOptions runtime_opt;

    if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv, &matrix_options,
                                           &runtime_opt)) {
        return usage(argv[0]);
    }

    rgb_matrix::Color fg_color_default(100, 0, 255);
    rgb_matrix::Color fg_color_late(255, 0, 0);
    rgb_matrix::Color bg_color(0, 0, 0);

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

    matrix =
        rgb_matrix::RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
    if (matrix == NULL) {
        return 1;
    }

    auto *offscreen = matrix->CreateFrameCanvas();

    signal(SIGTERM, interrupt_handler);
    signal(SIGINT, interrupt_handler);

    http_thread = std::thread(http_server);

    for (;;) {
        int y_next_line = font_large.baseline();

        matrix->SetBrightness(brightness.load());
        offscreen->Fill(bg_color.r, bg_color.g, bg_color.b);

        if (mode == TEXT) {
            auto t = text.load(std::memory_order_acquire);

            if (!t) {
                std::string headline = "No text set!";
                std::string text1 = "POST /text";
                std::string text2 = "{";
                std::string text3 = "  \"text\": \"Hello world!\"";
                std::string text4 = "}";
                y_next_line = write_line(offscreen, font_large, y_next_line,
                                         fg_color_default, headline);
                y_next_line = write_line(offscreen, font_small, y_next_line,
                                         fg_color_default, text1);
                y_next_line = write_line(offscreen, font_small, y_next_line,
                                         fg_color_default, text2);
                y_next_line = write_line(offscreen, font_small, y_next_line,
                                         fg_color_default, text3);
                y_next_line = write_line(offscreen, font_small, y_next_line,
                                         fg_color_default, text4);
            } else {
                y_next_line = write_line(offscreen, font_large, y_next_line,
                                         fg_color_default, *t);
            }
        } else if (mode == PTRANS) {
            auto tt = timetable.load(std::memory_order_acquire);

            if (!tt) {
                y_next_line =
                    write_line(offscreen, font_large, y_next_line,
                               fg_color_default, "No timetable available");
            } else {
                for (int i : std::views::iota(0, (int)tt->trips.size())) {
                    std::string line_name = tt->trips[i].line;
                    std::string direction = tt->trips[i].direction;

                    if (tt->trips[i].departures.empty()) {
                        std::string line = std::format(
                            "{:<3} {:<13} {:>3}", line_name, direction, "N/A");

                        y_next_line =
                            write_line(offscreen, font_large, y_next_line,
                                       fg_color_default, line);
                        continue;
                    }

                    int countdown = tt->trips[i].departures[0].countdown;
                    bool real_time = tt->trips[i].departures[0].real_time;
                    bool late = tt->trips[i].departures[0].late;
                    bool traffic_jam = tt->trips[i].departures[0].traffic_jam;

                    std::string line = std::format(
                        "{:<3} {:<13} {:>3}", line_name, direction,
                        real_time_indicator(real_time, late, traffic_jam) +
                            (countdown == 0 ? "*" : std::to_string(countdown)));

                    y_next_line = write_line(offscreen, font_large, y_next_line,
                                             fg_color_default, line);

                    if (tt->trips[i].departures.size() > 1) {
                        std::string str = "";

                        for (auto &&s :
                             tt->trips[i].departures | std::views::drop(1) |
                                 std::views::take(3) |
                                 std::views::transform(
                                     [](DepartureDto &departure) {
                                         return real_time_indicator(
                                                    departure.real_time,
                                                    departure.late,
                                                    departure.traffic_jam) +
                                                std::to_string(
                                                    departure.countdown);
                                     })) {
                            str += ((str.length() == 0 ? "" : ", ") + s);
                        }

                        y_next_line = write_line(offscreen, font_small,
                                                 y_next_line, fg_color_default,
                                                 std::format("{:>25}", str));
                    }
                }
            }
        } else {
            std::string headline = "No mode set!";
            std::string text1 = "POST /mode";
            std::string text2 = "{";
            std::string text3 = "  \"mode\": 0 (ptrans) | 1 (text)";
            std::string text4 = "}";
            y_next_line = write_line(offscreen, font_large, y_next_line,
                                     fg_color_default, headline);
            y_next_line = write_line(offscreen, font_small, y_next_line,
                                     fg_color_default, text1);
            y_next_line = write_line(offscreen, font_small, y_next_line,
                                     fg_color_default, text2);
            y_next_line = write_line(offscreen, font_small, y_next_line,
                                     fg_color_default, text3);
            y_next_line = write_line(offscreen, font_small, y_next_line,
                                     fg_color_default, text4);
        }

        offscreen = matrix->SwapOnVSync(offscreen);
    }
}
