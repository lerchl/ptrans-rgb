#include "graphics.h"
#include "http_server.hpp"
#include "led-matrix.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <getopt.h>
#include <httplib.h>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <vector>

#include "config.hpp"
#include "http_server.hpp"
#include "lio.hpp"

#ifndef APP_VERSION
#define APP_VERSION "unknown"
#endif

using json = nlohmann::json;

std::condition_variable app_cv;
std::mutex app_mutex;
std::atomic<bool> app_running{true};

rgb_matrix::RGBMatrix *matrix;
httplib::Server http_server;
std::thread http_server_thread;
std::thread timetable_job_thread;

static void interrupt_handler(int) {
    app_running = false;
    app_cv.notify_all();

    http_server.stop();
    http_server_thread.join();
    timetable_job_thread.join();

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

    std::atomic<std::shared_ptr<Configuration>> configuration =
        std::make_shared<Configuration>(Configuration{
            .mode = PTRANS, .brightness = 80, .blackout_window = std::nullopt});
    std::atomic<std::shared_ptr<TimetableDto>> timetable;
    std::atomic<std::shared_ptr<const std::string>> text;

    auto run_http_server = make_http_server(APP_VERSION, configuration, text);
    auto run_timetable_job =
        make_timetable_job(app_cv, app_mutex, app_running, timetable);
    http_server_thread = std::thread(
        [&run_http_server, port]() { run_http_server(http_server, port); });
    timetable_job_thread = std::thread(
        [&run_timetable_job, data_url]() { run_timetable_job(data_url); });

    // --- SF shared constants and helpers ---
    const std::vector<std::string> SF_CHARSET = {
        " ", "A", "Ä", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K",
        "L", "M", "N", "O", "Ö", "P", "Q", "R", "S", "T", "U", "Ü", "V",
        "W", "X", "Y", "Z", "a", "ä", "b", "c", "d", "e", "f", "g", "h",
        "i", "j", "k", "l", "m", "n", "o", "ö", "p", "q", "r", "s", "t",
        "u", "ü", "v", "w", "x", "y", "z", "0", "1", "2", "3", "4", "5",
        "6", "7", "8", "9", ".", ":", ",", "!", "?", "-", "*", "\""};
    const int SF_CHARSET_SIZE = (int)SF_CHARSET.size();
    const int SF_MS_PER_STEP = 25;
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

        auto current_config = configuration.load(std::memory_order_acquire);
        matrix->SetBrightness(current_config->brightness);
        offscreen->Fill(bg_color.r, bg_color.g, bg_color.b);

        auto now = std::chrono::steady_clock::now();
        bool sf_step = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - sf_last_step)
                           .count() >= SF_MS_PER_STEP;
        if (sf_step)
            sf_last_step = now;

        if (current_config->mode == TEXT) {
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
        } else if (current_config->mode == PTRANS) {
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
