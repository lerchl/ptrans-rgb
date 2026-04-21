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
    fprintf(stderr, "\t-f <font-file>       : BDF font file to use.\n");
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
    size_t codepoints = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80)
            codepoints++;
    size_t padding = (codepoints < width) ? width - codepoints : 0;
    return s + std::string(padding, ' ');
}

int write_line(rgb_matrix::FrameCanvas *canvas, rgb_matrix::Font &font, int y,
               rgb_matrix::Color color, std::string text) {
    rgb_matrix::DrawText(canvas, font, 0, y, color, NULL, text.c_str(), 0);
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
    const char *bdf_font_file = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "p:d:f:")) != -1) {
        switch (opt) {
        case 'p':
            port = std::stoi(optarg);
            break;
        case 'd':
            data_url = std::string(optarg);
            break;
        case 'f':
            bdf_font_file = strdup(optarg);
            break;
        default:
            return usage(argv[0]);
        }
    }

    if (bdf_font_file == NULL) {
        fprintf(stderr, "Need to specify a BDF font-file with -f\n");
        return usage(argv[0]);
    }

    rgb_matrix::Font font;
    if (!font.LoadFont(bdf_font_file)) {
        fprintf(stderr, "Couldn't load font '%s'\n", bdf_font_file);
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

    // --- Derived layout constants ---
    // Matrix dimensions come from the matrix itself, so --led-chain etc. are
    // respected
    const int SF_MATRIX_W = matrix->width();
    const int SF_MATRIX_H = matrix->height();
    const int SF_MS_PER_STEP = 25;

    // Font metrics derived from the loaded font
    const int SF_CHAR_W =
        font.CharacterWidth('M'); // 'M' as representative fixed width
    const int SF_CELL_GAP = 1;
    const int SF_CELL_H = font.baseline() + 2;
    const int SF_CELL_W = SF_CHAR_W;

    const int SF_NUM_ROWS = SF_MATRIX_H / SF_CELL_H;
    const int SF_NUM_COLS = SF_MATRIX_W / (SF_CELL_W + SF_CELL_GAP);
    const int SF_NUM_CELLS = SF_NUM_ROWS * SF_NUM_COLS;

    // PTRANS column layout — calculated from font/matrix dimensions
    // <line(3)> <space> <direction(dynamic)> <space> <time(3)>
    const int SF_COL_LINE = 3;
    const int SF_COL_DEPS = 9;
    const int SF_COL_SPACE = 2;
    const int SF_COL_DIR =
        SF_NUM_COLS - SF_COL_LINE - SF_COL_DEPS - SF_COL_SPACE;

    const std::vector<std::string> SF_CHARSET = {
        " ", "A", "Ä", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K",
        "L", "M", "N", "O", "Ö", "P", "Q", "R", "S", "T", "U", "Ü", "V",
        "W", "X", "Y", "Z", "a", "ä", "b", "c", "d", "e", "f", "g", "h",
        "i", "j", "k", "l", "m", "n", "o", "ö", "p", "q", "r", "s", "t",
        "u", "ü", "v", "w", "x", "y", "z", "0", "1", "2", "3", "4", "5",
        "6", "7", "8", "9", ".", ":", ",", "!", "?", "-", "*", "\""};
    const int SF_CHARSET_SIZE = (int)SF_CHARSET.size();

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

    std::vector<SFCell> sf_cells_ptrans(SF_NUM_CELLS);
    std::string sf_last_target_ptrans = "";

    auto sf_last_step = std::chrono::steady_clock::now();

    auto sf_update_cells = [&](std::vector<SFCell> &cells,
                               std::string &last_target,
                               const std::string &new_target) {
        if (new_target == last_target)
            return;
        last_target = new_target;
        std::vector<std::string> codepoints = sf_utf8_split(new_target);
        for (int i = 0; i < SF_NUM_CELLS; ++i) {
            std::string cp = (i < (int)codepoints.size()) ? codepoints[i] : " ";
            int ti = sf_charset_index(SF_CHARSET, SF_CHARSET_SIZE, cp);
            int steps =
                (ti - cells[i].charIndex + SF_CHARSET_SIZE) % SF_CHARSET_SIZE;
            cells[i].targetIndex = ti;
            cells[i].stepsLeft = steps;
            cells[i].flipping = (steps > 0);
        }
    };
    auto sf_render_cells = [&](std::vector<SFCell> &cells, bool step) {
        for (int i = 0; i < SF_NUM_CELLS; ++i) {
            SFCell &cell = cells[i];
            int row = i / SF_NUM_COLS;
            int col = i % SF_NUM_COLS;
            int px = 1 + col * (SF_CELL_W + SF_CELL_GAP);
            int py = font.baseline() + row * SF_CELL_H;

            rgb_matrix::DrawText(offscreen, font, px, py, fg_color_default,
                                 nullptr, SF_CHARSET[cell.charIndex].c_str());

            if (cell.flipping && step) {
                cell.charIndex = (cell.charIndex + 1) % SF_CHARSET_SIZE;
                cell.stepsLeft--;
                if (cell.stepsLeft <= 0) {
                    cell.charIndex = cell.targetIndex;
                    cell.flipping = false;
                }
            }
        }
    };

    auto sf_pad_line = [&](const std::string &s) {
        auto cps = sf_utf8_split(s);
        std::string result = s;
        for (int i = (int)cps.size(); i < SF_NUM_COLS; ++i)
            result += " ";
        return result;
    };

    for (;;) {
        auto current_config = configuration.load(std::memory_order_acquire);
        matrix->SetBrightness(current_config->brightness);
        offscreen->Fill(bg_color.r, bg_color.g, bg_color.b);

        auto now = std::chrono::steady_clock::now();
        bool sf_step = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - sf_last_step)
                           .count() >= SF_MS_PER_STEP;
        if (sf_step)
            sf_last_step = now;

        std::string target_text;

        if (current_config->mode == TEXT) {
            auto t = text.load(std::memory_order_acquire);
            if (!t) {
                target_text = sf_pad_line("No text set!") +
                              sf_pad_line("Go to https://ptrans.home.l3rchl.at");
            } else {
                target_text = *t;
            }

        } else if (current_config->mode == PTRANS) {
            auto tt = timetable.load(std::memory_order_acquire);

            if (!tt) {
                target_text = "No timetable available";
            } else {
                auto departure_color = [&](bool real_time, bool late,
                                           bool traffic_jam) {
                    if (!real_time)
                        return fg_color_default;
                    if (traffic_jam)
                        return rgb_matrix::Color(255, 100, 0);
                    if (late)
                        return rgb_matrix::Color(255, 0, 0);
                    return rgb_matrix::Color(0, 255, 0);
                };

                std::vector<std::string> display_lines;
                std::vector<rgb_matrix::Color> display_line_colors;

                for (int i : std::views::iota(0, (int)tt->trips.size())) {
                    std::string line_name = tt->trips[i].line;
                    std::string direction = tt->trips[i].direction;

                    if (tt->trips[i].departures.empty()) {
                        display_lines.push_back(std::format(
                            "{:<{}} {:<{}} {:>{}}", line_name, SF_COL_LINE,
                            pad_utf8(direction, SF_COL_DIR), SF_COL_DIR, "N/A",
                            SF_COL_DEPS));
                        display_line_colors.push_back(fg_color_default);
                        continue;
                    }

                    std::string deps_str = "";
                    for (auto &&dep :
                         tt->trips[i].departures | std::views::take(3)) {
                        std::string s = (dep.countdown == 0
                                             ? "*"
                                             : std::to_string(dep.countdown));
                        deps_str += (deps_str.empty() ? "" : " ") + s;
                    }

                    display_lines.push_back(std::format(
                        "{:<{}} {:<{}} {:>{}}", line_name, SF_COL_LINE,
                        pad_utf8(direction, SF_COL_DIR), SF_COL_DIR, deps_str,
                        SF_COL_DEPS));

                    auto &d = tt->trips[i].departures[0];
                    display_line_colors.push_back(
                        departure_color(d.real_time, d.late, d.traffic_jam));
                }

                for (int row = 0; row < SF_NUM_ROWS; ++row) {
                    std::string row_str = (row < (int)display_lines.size())
                                              ? display_lines[row]
                                              : "";
                    auto cps = sf_utf8_split(row_str);
                    for (int col = 0; col < SF_NUM_COLS; ++col) {
                        target_text += (col < (int)cps.size()) ? cps[col] : " ";
                    }
                }
            }
        } else {
            target_text = sf_pad_line("No mode set!") +
                          sf_pad_line("Go to https://ptrans.home.l3rchl.at");
        }

        sf_update_cells(sf_cells_ptrans, sf_last_target_ptrans, target_text);
        sf_render_cells(sf_cells_ptrans, sf_step);
        offscreen = matrix->SwapOnVSync(offscreen);
    }
}
