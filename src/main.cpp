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

struct SFChar {
    std::string glyph;
    Color color;
};

bool operator==(const SFChar &a, const SFChar &b) {
    return a.glyph == b.glyph && a.color.r == b.color.r &&
           a.color.g == b.color.g && a.color.b == b.color.b;
}

std::vector<SFChar> operator+(std::vector<SFChar> a,
                              const std::vector<SFChar> &b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

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

std::string pad_utf8(const std::string &s, size_t width) {
    size_t codepoints = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80)
            codepoints++;
    size_t padding = (codepoints < width) ? width - codepoints : 0;
    return s + std::string(padding, ' ');
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
            .mode = PTRANS,
            .brightness = 80,
            .blackout_window = std::nullopt,
            .colors = {.fg_default = {.r = 100, .g = 0, .b = 255},
                       .fg_late = {.r = 255, .g = 0, .b = 0},
                       .fg_traffic = {.r = 255, .g = 100, .b = 0},
                       .fg_punctual = {.r = 0, .g = 255, .b = 0}}});

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

    const int SF_COL_LINE = 3;
    const int SF_COL_DEPS = 9;
    const int SF_COL_SPACE = 2;
    const int SF_COL_DIR =
        SF_NUM_COLS - SF_COL_LINE - SF_COL_DEPS - SF_COL_SPACE;

    const Color SF_WHITE = {255, 255, 255};

    const std::vector<SFChar> SF_CHARSET = {
        {" ", SF_WHITE},       {"█", {255, 80, 80}}, {"█", {255, 160, 0}},
        {"█", {255, 255, 0}},  {"█", {80, 255, 80}}, {"█", {0, 200, 255}},
        {"█", {160, 80, 255}}, {"A", SF_WHITE},      {"Ä", SF_WHITE},
        {"B", SF_WHITE},       {"C", SF_WHITE},      {"D", SF_WHITE},
        {"E", SF_WHITE},       {"F", SF_WHITE},      {"G", SF_WHITE},
        {"H", SF_WHITE},       {"I", SF_WHITE},      {"J", SF_WHITE},
        {"K", SF_WHITE},       {"L", SF_WHITE},      {"M", SF_WHITE},
        {"N", SF_WHITE},       {"O", SF_WHITE},      {"Ö", SF_WHITE},
        {"P", SF_WHITE},       {"Q", SF_WHITE},      {"R", SF_WHITE},
        {"S", SF_WHITE},       {"T", SF_WHITE},      {"U", SF_WHITE},
        {"Ü", SF_WHITE},       {"V", SF_WHITE},      {"W", SF_WHITE},
        {"X", SF_WHITE},       {"Y", SF_WHITE},      {"Z", SF_WHITE},
        {"a", SF_WHITE},       {"ä", SF_WHITE},      {"b", SF_WHITE},
        {"c", SF_WHITE},       {"d", SF_WHITE},      {"e", SF_WHITE},
        {"f", SF_WHITE},       {"g", SF_WHITE},      {"h", SF_WHITE},
        {"i", SF_WHITE},       {"j", SF_WHITE},      {"k", SF_WHITE},
        {"l", SF_WHITE},       {"m", SF_WHITE},      {"n", SF_WHITE},
        {"o", SF_WHITE},       {"ö", SF_WHITE},      {"p", SF_WHITE},
        {"q", SF_WHITE},       {"r", SF_WHITE},      {"s", SF_WHITE},
        {"t", SF_WHITE},       {"u", SF_WHITE},      {"ü", SF_WHITE},
        {"v", SF_WHITE},       {"w", SF_WHITE},      {"x", SF_WHITE},
        {"y", SF_WHITE},       {"z", SF_WHITE},      {"0", SF_WHITE},
        {"1", SF_WHITE},       {"2", SF_WHITE},      {"3", SF_WHITE},
        {"4", SF_WHITE},       {"5", SF_WHITE},      {"6", SF_WHITE},
        {"7", SF_WHITE},       {"8", SF_WHITE},      {"9", SF_WHITE},
        {".", SF_WHITE},       {":", SF_WHITE},      {",", SF_WHITE},
        {"!", SF_WHITE},       {"?", SF_WHITE},      {"-", SF_WHITE},
        {"*", SF_WHITE},       {"\"", SF_WHITE},
    };
    const int SF_CHARSET_SIZE = (int)SF_CHARSET.size();

    auto sf_charset_index = [&](const std::string &glyph, const Color &color) {
        for (int i = 0; i < SF_CHARSET_SIZE; ++i)
            if (SF_CHARSET[i].glyph == glyph &&
                SF_CHARSET[i].color.r == color.r &&
                SF_CHARSET[i].color.g == color.g &&
                SF_CHARSET[i].color.b == color.b)
                return i;
        for (int i = 0; i < SF_CHARSET_SIZE; ++i)
            if (SF_CHARSET[i].glyph == glyph)
                return i;
        return 0;
    };

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

    struct SFCell {
        int char_index = 0;
        int target_index = 0;
        int steps_left = 0;
        bool flipping = false;
        rgb_matrix::Color fg_color = rgb_matrix::Color(255, 255, 255);
    };

    std::vector<SFCell> cells(SF_NUM_CELLS);
    std::vector<SFChar> previous_target(SF_NUM_CELLS);

    auto sf_last_step = std::chrono::steady_clock::now();

    auto sf_update_cells = [&](std::vector<SFCell> &cells,
                               std::vector<SFChar> &last_target,
                               const std::vector<SFChar> &new_target) {
        if (new_target == last_target)
            return;
        last_target = new_target;
        for (int i = 0; i < SF_NUM_CELLS; ++i) {
            const SFChar &tc = (i < (int)new_target.size())
                                   ? new_target[i]
                                   : SFChar{" ", SF_WHITE};
            int ti = sf_charset_index(tc.glyph, tc.color);
            int steps =
                (ti - cells[i].char_index + SF_CHARSET_SIZE) % SF_CHARSET_SIZE;
            cells[i].target_index = ti;
            cells[i].steps_left = steps;
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

            const SFChar &sc = SF_CHARSET[cell.char_index];
            rgb_matrix::DrawText(offscreen, font, px, py, sc.color, nullptr,
                                 sc.glyph.c_str());

            if (cell.flipping && step) {
                cell.char_index = (cell.char_index + 1) % SF_CHARSET_SIZE;
                cell.steps_left--;
                if (cell.steps_left <= 0) {
                    cell.char_index = cell.target_index;
                    cell.flipping = false;
                }
            }
        }
    };

    auto sf_pad_line = [&](const std::string &s, const Color &color) {
        auto cps = sf_utf8_split(s);
        std::vector<SFChar> result;
        for (int i = 0; i < SF_NUM_COLS; ++i) {
            std::string glyph = (i < (int)cps.size()) ? cps[i] : " ";
            result.push_back({glyph, color});
        }
        return result;
    };

    for (;;) {
        auto current_config = configuration.load(std::memory_order_acquire);
        matrix->SetBrightness(current_config->brightness);
        offscreen->Fill(bg_color.r, bg_color.g, bg_color.b);

        auto now = std::chrono::steady_clock::now();
        bool step = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - sf_last_step)
                        .count() >= SF_MS_PER_STEP;
        if (step)
            sf_last_step = now;

        std::vector<SFChar> new_target;

        if (current_config->mode == TEXT) {
            auto t = text.load(std::memory_order_acquire);
            if (!t) {
                new_target = sf_pad_line("No text set!",
                                         current_config->colors.fg_default) +
                             sf_pad_line("Go to https://ptrans.home.l3rchl.at",
                                         current_config->colors.fg_default);
            } else {
                auto cps = sf_utf8_split(*t);
                new_target.resize(SF_NUM_CELLS);
                for (int i = 0; i < SF_NUM_CELLS; ++i) {
                    new_target[i] = {i < std::ssize(cps) ? cps[i] : " ",
                                     current_config->colors.fg_default};
                }
            }
        } else if (current_config->mode == PTRANS) {
            auto tt = timetable.load(std::memory_order_acquire);
            if (!tt) {
                new_target = sf_pad_line("No timetable available",
                                         current_config->colors.fg_default);
            } else {
                auto departure_color = [&](bool real_time, bool late,
                                           bool traffic_jam) -> Color {
                    if (!real_time)
                        return current_config->colors.fg_default;
                    if (traffic_jam)
                        return current_config->colors.fg_traffic;
                    if (late)
                        return current_config->colors.fg_late;
                    return current_config->colors.fg_punctual;
                };

                struct DisplayLine {
                    std::string line_name;
                    std::string direction;
                    std::string deps_str;
                    Color deps_color;
                };

                std::vector<DisplayLine> display_lines;

                for (int i : std::views::iota(0, (int)tt->trips.size())) {
                    auto &trip = tt->trips[i];
                    DisplayLine dl;
                    dl.line_name = trip.line;
                    dl.direction = trip.direction;

                    if (trip.departures.empty()) {
                        dl.deps_str = "N/A";
                        dl.deps_color = current_config->colors.fg_default;
                    } else {
                        for (auto &&dep :
                             trip.departures | std::views::take(3)) {
                            std::string s = dep.countdown == 0
                                                ? "*"
                                                : std::to_string(dep.countdown);
                            dl.deps_str += (dl.deps_str.empty() ? "" : " ") + s;
                        }
                        auto &d = trip.departures[0];
                        dl.deps_color =
                            departure_color(d.real_time, d.late, d.traffic_jam);
                    }

                    display_lines.push_back(dl);
                }

                new_target.clear();
                for (int row = 0; row < SF_NUM_ROWS; ++row) {
                    if (row >= (int)display_lines.size()) {
                        auto padding =
                            sf_pad_line("", current_config->colors.fg_default);
                        new_target.insert(new_target.end(), padding.begin(),
                                          padding.end());
                        continue;
                    }

                    auto &dl = display_lines[row];

                    // line name + direction in default color
                    std::string prefix = std::format(
                        "{:<{}} {:<{}} ", dl.line_name, SF_COL_LINE,
                        pad_utf8(dl.direction, SF_COL_DIR), SF_COL_DIR);
                    auto prefix_cps = sf_utf8_split(prefix);
                    for (auto &cp : prefix_cps)
                        new_target.push_back(
                            {cp, current_config->colors.fg_default});

                    // departure times in their own color
                    std::string deps =
                        std::format("{:>{}}", dl.deps_str, SF_COL_DEPS);
                    auto tt = timetable.load(std::memory_order_acquire);
                    if (!tt) {
                        new_target =
                            sf_pad_line("No timetable available",
                                        current_config->colors.fg_default);
                    } else {
                        auto departure_color = [&](bool real_time, bool late,
                                                   bool traffic_jam) -> Color {
                            if (!real_time)
                                return current_config->colors.fg_default;
                            if (traffic_jam)
                                return current_config->colors.fg_traffic;
                            if (late)
                                return current_config->colors.fg_late;
                            return current_config->colors.fg_punctual;
                        };

                        struct DisplayLine {
                            std::string line_name;
                            std::string direction;
                            std::string deps_str;
                            Color deps_color;
                        };

                        std::vector<DisplayLine> display_lines;

                        for (int i :
                             std::views::iota(0, (int)tt->trips.size())) {
                            auto &trip = tt->trips[i];
                            DisplayLine dl;
                            dl.line_name = trip.line;
                            dl.direction = trip.direction;

                            if (trip.departures.empty()) {
                                dl.deps_str = "N/A";
                                dl.deps_color =
                                    current_config->colors.fg_default;
                            } else {
                                for (auto &&dep :
                                     trip.departures | std::views::take(3)) {
                                    std::string s =
                                        dep.countdown == 0
                                            ? "*"
                                            : std::to_string(dep.countdown);
                                    dl.deps_str +=
                                        (dl.deps_str.empty() ? "" : " ") + s;
                                }
                                auto &d = trip.departures[0];
                                dl.deps_color = departure_color(
                                    d.real_time, d.late, d.traffic_jam);
                            }

                            display_lines.push_back(dl);
                        }

                        new_target.clear();
                        for (int row = 0; row < SF_NUM_ROWS; ++row) {
                            if (row >= (int)display_lines.size()) {
                                auto padding = sf_pad_line(
                                    "", current_config->colors.fg_default);
                                new_target.insert(new_target.end(),
                                                  padding.begin(),
                                                  padding.end());
                                continue;
                            }

                            auto &dl = display_lines[row];

                            // line name + direction in default color
                            std::string prefix = std::format(
                                "{:<{}} {:<{}} ", dl.line_name, SF_COL_LINE,
                                pad_utf8(dl.direction, SF_COL_DIR), SF_COL_DIR);
                            auto prefix_cps = sf_utf8_split(prefix);
                            for (auto &cp : prefix_cps)
                                new_target.push_back(
                                    {cp, current_config->colors.fg_default});

                            // departure times in their own color
                            std::string deps =
                                std::format("{:>{}}", dl.deps_str, SF_COL_DEPS);
                            auto deps_cps = sf_utf8_split(deps);
                            for (auto &cp : deps_cps)
                                new_target.push_back({cp, dl.deps_color});
                        }
                    }
                    auto deps_cps = sf_utf8_split(deps);
                    for (auto &cp : deps_cps)
                        new_target.push_back({cp, dl.deps_color});
                }
            }
        } else {
            new_target =
                sf_pad_line("No mode set!", current_config->colors.fg_default) +
                sf_pad_line("Go to https://ptrans.home.l3rchl.at",
                            current_config->colors.fg_default);
        }

        sf_update_cells(cells, previous_target, new_target);
        sf_render_cells(cells, step);

        offscreen = matrix->SwapOnVSync(offscreen);
    }
}
