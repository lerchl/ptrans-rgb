#include "graphics.h"
#include "led-matrix.h"

#include <atomic>
#include <cstddef>
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

#ifndef APP_VERSION
#define APP_VERSION "unknown"
#endif

using json = nlohmann::json;

enum Mode { PTRANS, TEXT };

std::condition_variable cv;
std::mutex cv_mutex;
std::atomic<bool> running{true};

rgb_matrix::RGBMatrix *matrix;
httplib::Server server;
std::thread http_thread;
std::thread ptrans_thread;

std::atomic<Mode> mode{PTRANS};
std::atomic<int> brightness{80};
std::atomic<std::shared_ptr<const std::string>> text;

static void interrupt_handler(int) {
    running = false;
    cv.notify_all();

    server.stop();
    http_thread.join();
    ptrans_thread.join();

    delete matrix;

    std::cout << std::endl;
    exit(0);
}

static int usage(const char *progname) {
    fprintf(stderr, "usage: %s [options]\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "\t-p <port>            : Port to listen on.\n");
    fprintf(stderr, "\t-d <data url>        : URL to ptrans-data.\n");
    fprintf(stderr,
            "\t-f <font-file>       : Use given font for small text (5x8).\n");
    fprintf(stderr,
            "\t-F <font-file>       : Use given font for large text (6x12).\n");
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

inline void to_json(json &j, const TextDto &t) { j = json{{"text", t.text}}; }

void http_server(const int &port) {
    server.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type"},
    });

    server.Options(".*", [](const httplib::Request &, httplib::Response &res) {
        res.status = 204;
    });

    server.Get("/version",
               [](const httplib::Request &, httplib::Response &res) {
                   json j = {{"version", APP_VERSION}};
                   res.status = 200;
                   res.set_content(j.dump(), "application/json");
               });

    server.Get("/mode", [](const httplib::Request &, httplib::Response &res) {
        ModeDto dto{.mode = mode.load(std::memory_order_acquire)};
        json j = dto;
        res.status = 200;
        res.set_content(j.dump(), "application/json");
    });

    server.Post(
        "/mode", [](const httplib::Request &req, httplib::Response &res) {
            try {
                Mode new_mode = json::parse(req.body).get<ModeDto>().mode;
                mode.store(new_mode);
                res.status = 204;
            } catch (const json::parse_error &e) {
                res.status = 400;
            }
        });

    server.Get(
        "/brightness", [](const httplib::Request &, httplib::Response &res) {
            BrightnessDto dto{.brightness =
                                  brightness.load(std::memory_order_acquire)};
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
                res.status = 204;
            } catch (const json::parse_error &e) {
                res.status = 400;
            }
        });

    server.Get("/text", [](const httplib::Request &, httplib::Response &res) {
        if (auto t = text.load(std::memory_order_acquire)) {
            TextDto dto{.text = *t};
            json j = dto;
            res.status = 200;
            res.set_content(j.dump(), "application/json");
        } else {
            res.status = 204;
        }
    });

    server.Post("/text",
                [](const httplib::Request &req, httplib::Response &res) {
                    try {
                        auto new_text = std::make_shared<std::string>(
                            json::parse(req.body).get<TextDto>().text);
                        text.store(new_text, std::memory_order_release);
                        res.status = 204;
                    } catch (const json::parse_error &e) {
                        res.status = 400;
                    }
                });

    server.listen("0.0.0.0", port);
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

void ptrans_job(const std::string &data_url) {
    httplib::Client cli(data_url);

    while (running) {
        auto result = cli.Get("/timetable");
        std::string formatted_time =
            std::format("{0:%F_%T}", std::chrono::system_clock::now());

        if (!result) {
            std::cerr << std::format("{} - Could not reach ptrans-data",
                                     formatted_time)
                      << std::endl;
        } else if (result->status == 200) {
            timetable.store(
                std::make_shared<TimetableDto>(parse_timetable(result->body)),
                std::memory_order_release);
        } else {
            ErrorDto error = parse_error(result->body);
            std::cerr << std::format("{} - {} Could not fetch timetable: {}",
                                     formatted_time, result->status,
                                     error.message)
                      << std::endl;
        }

        std::unique_lock<std::mutex> lock(cv_mutex);
        cv.wait_for(lock, std::chrono::seconds(30),
                    [] { return !running.load(); });
    }
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

std::string pad_utf8(const std::string &s, size_t width) {
    // Count codepoints (not bytes)
    size_t codepoints = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80)
            codepoints++; // skip continuation bytes
    size_t padding = (codepoints < width) ? width - codepoints : 0;
    return s + std::string(padding, ' ');
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

    int port = 0;
    std::string data_url = "";
    const char *bdf_font_file_small = NULL;
    const char *bdf_font_file_large = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "p:d:f:F:")) != -1) {
        switch (opt) {
        case 'p':
            port = std::stoi(optarg);
            break;
        case 'd':
            data_url = std::string(optarg);
            break;
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

    http_thread = std::thread([&port]() { http_server(port); });
    ptrans_thread = std::thread([&data_url]() { ptrans_job(data_url); });

    // --- SF shared constants and helpers ---
    const std::vector<std::string> SF_CHARSET = {
        " ", "A", "Ä", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K",
        "L", "M", "N", "O", "Ö", "P", "Q", "R", "S", "T", "U", "Ü", "V",
        "W", "X", "Y", "Z", "a", "ä", "b", "c", "d", "e", "f", "g", "h",
        "i", "j", "k", "l", "m", "n", "o", "ö", "p", "q", "r", "s", "t",
        "u", "ü", "v", "w", "x", "y", "z", "0", "1", "2", "3", "4", "5",
        "6", "7", "8", "9", ".", ":", ",", "!", "?", "-", "*", "\""};
    const int SF_CHARSET_SIZE = (int)SF_CHARSET.size();
    const int SF_MS_PER_STEP = 50;
    const int SF_MATRIX_W = 128;
    const int SF_MATRIX_H = 64;

    // TEXT mode grid (font_large)
    const int SF_CELL_W = 7;
    const int SF_CELL_GAP = 1;
    const int SF_CELL_H = font_large.baseline() + 2;
    const int SF_NUM_ROWS = SF_MATRIX_H / SF_CELL_H;
    const int SF_NUM_COLS = SF_MATRIX_W / (SF_CELL_W + SF_CELL_GAP);
    const int SF_NUM_CELLS = SF_NUM_ROWS * SF_NUM_COLS;

    // PTRANS mode grid (font_small)
    const int SF_CELL_W_SMALL = 4;
    const int SF_CELL_GAP_SMALL = 1;
    const int SF_CELL_H_SMALL = font_small.baseline() + 2;
    const int SF_NUM_COLS_SMALL = 25;
    const int SF_NUM_ROWS_SMALL = SF_MATRIX_H / SF_CELL_H_SMALL;
    const int SF_NUM_CELLS_SMALL = SF_NUM_ROWS_SMALL * SF_NUM_COLS_SMALL;

    auto sf_utf8_split = [](const std::string &s) {
        std::vector<std::string> result;
        size_t i = 0;
        while (i < s.size()) {
            unsigned char c = s[i];
            int len = 1;
            if ((c & 0xE0) == 0xC0)
                len = 2;
            else if ((c & 0xF0) == 0xE0)
                len = 3;
            else if ((c & 0xF8) == 0xF0)
                len = 4;
            result.push_back(s.substr(i, len));
            i += len;
        }
        return result;
    };

    auto sf_charset_index = [](const std::vector<std::string> &charset,
                               int charset_size, const std::string &cp) {
        for (int i = 0; i < charset_size; ++i)
            if (charset[i] == cp)
                return i;
        return 0;
    };

    struct SFCell {
        int charIndex = 0;
        int targetIndex = 0;
        int stepsLeft = 0;
        bool flipping = false;
    };

    std::vector<SFCell> sf_cells(SF_NUM_CELLS);
    std::string sf_last_target = "";

    std::vector<SFCell> sf_cells_ptrans(SF_NUM_CELLS_SMALL);
    std::string sf_last_target_ptrans = "";

    auto sf_last_step = std::chrono::steady_clock::now();

    for (;;) {
        int y_next_line = font_large.baseline();

        matrix->SetBrightness(brightness.load(std::memory_order_acquire));
        offscreen->Fill(bg_color.r, bg_color.g, bg_color.b);

        auto now = std::chrono::steady_clock::now();
        bool sf_step = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - sf_last_step)
                           .count() >= SF_MS_PER_STEP;
        if (sf_step)
            sf_last_step = now;

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
                if (*t != sf_last_target) {
                    sf_last_target = *t;
                    std::vector<std::string> codepoints = sf_utf8_split(*t);
                    for (int i = 0; i < SF_NUM_CELLS; ++i) {
                        std::string cp =
                            (i < (int)codepoints.size()) ? codepoints[i] : " ";
                        int ti =
                            sf_charset_index(SF_CHARSET, SF_CHARSET_SIZE, cp);
                        int steps =
                            (ti - sf_cells[i].charIndex + SF_CHARSET_SIZE) %
                            SF_CHARSET_SIZE;
                        sf_cells[i].targetIndex = ti;
                        sf_cells[i].stepsLeft = steps;
                        sf_cells[i].flipping = (steps > 0);
                    }
                }

                for (int i = 0; i < SF_NUM_CELLS; ++i) {
                    SFCell &cell = sf_cells[i];
                    int row = i / SF_NUM_COLS;
                    int col = i % SF_NUM_COLS;
                    int px = 1 + col * (SF_CELL_W + SF_CELL_GAP);
                    int py = font_large.baseline() + row * SF_CELL_H;

                    rgb_matrix::DrawText(offscreen, font_large, px, py,
                                         fg_color_default, nullptr,
                                         SF_CHARSET[cell.charIndex].c_str());

                    if (cell.flipping && sf_step) {
                        cell.charIndex = (cell.charIndex + 1) % SF_CHARSET_SIZE;
                        cell.stepsLeft--;
                        if (cell.stepsLeft <= 0) {
                            cell.charIndex = cell.targetIndex;
                            cell.flipping = false;
                        }
                    }
                }
            }
        } else if (mode == PTRANS) {
            auto tt = timetable.load(std::memory_order_acquire);

            std::string target_text = "";

            if (!tt) {
                target_text = "No timetable available";
            } else {
                std::vector<std::string> display_lines;

                for (int i : std::views::iota(0, (int)tt->trips.size())) {
                    std::string line_name = tt->trips[i].line;
                    std::string direction = tt->trips[i].direction;

                    if (tt->trips[i].departures.empty()) {
                        display_lines.push_back(
                            std::format("{:<3} {} {:>3}", line_name,
                                        pad_utf8(direction, 17), "N/A"));
                        display_lines.push_back("");
                        continue;
                    }

                    int countdown = tt->trips[i].departures[0].countdown;
                    bool real_time = tt->trips[i].departures[0].real_time;
                    bool late = tt->trips[i].departures[0].late;
                    bool traffic_jam = tt->trips[i].departures[0].traffic_jam;

                    std::string countdown_indicator =
                        (countdown == 0 ? "*" : std::to_string(countdown));

                    display_lines.push_back(std::format(
                        "{:<3} {} {:>3}", line_name, pad_utf8(direction, 17),
                        real_time_indicator(real_time, late, traffic_jam) +
                            countdown_indicator));

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
                        display_lines.push_back(std::format("{:>25}", str));
                    } else {
                        display_lines.push_back("");
                    }
                }

                for (int row = 0; row < SF_NUM_ROWS_SMALL; ++row) {
                    std::string row_str = (row < (int)display_lines.size())
                                              ? display_lines[row]
                                              : "";
                    auto cps = sf_utf8_split(row_str);
                    for (int col = 0; col < SF_NUM_COLS_SMALL; ++col) {
                        target_text += (col < (int)cps.size()) ? cps[col] : " ";
                    }
                }
            }

            if (target_text != sf_last_target_ptrans) {
                sf_last_target_ptrans = target_text;
                std::vector<std::string> codepoints =
                    sf_utf8_split(target_text);
                for (int i = 0; i < SF_NUM_CELLS_SMALL; ++i) {
                    std::string cp =
                        (i < (int)codepoints.size()) ? codepoints[i] : " ";
                    int ti = sf_charset_index(SF_CHARSET, SF_CHARSET_SIZE, cp);
                    int steps =
                        (ti - sf_cells_ptrans[i].charIndex + SF_CHARSET_SIZE) %
                        SF_CHARSET_SIZE;
                    sf_cells_ptrans[i].targetIndex = ti;
                    sf_cells_ptrans[i].stepsLeft = steps;
                    sf_cells_ptrans[i].flipping = (steps > 0);
                }
            }

            for (int i = 0; i < SF_NUM_CELLS_SMALL; ++i) {
                SFCell &cell = sf_cells_ptrans[i];
                int row = i / SF_NUM_COLS_SMALL;
                int col = i % SF_NUM_COLS_SMALL;
                int px = 1 + col * (SF_CELL_W_SMALL + SF_CELL_GAP_SMALL);
                int py = font_small.baseline() + row * SF_CELL_H_SMALL;

                rgb_matrix::DrawText(offscreen, font_small, px, py,
                                     fg_color_default, nullptr,
                                     SF_CHARSET[cell.charIndex].c_str());

                if (cell.flipping && sf_step) {
                    cell.charIndex = (cell.charIndex + 1) % SF_CHARSET_SIZE;
                    cell.stepsLeft--;
                    if (cell.stepsLeft <= 0) {
                        cell.charIndex = cell.targetIndex;
                        cell.flipping = false;
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
