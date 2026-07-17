#include "graphics.h"
#include "http_server.hpp"
#include "led-matrix.h"

#include <atomic>
#include <chrono>
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

#include "album_cover.hpp"
#include "config.hpp"
#include "config_manager.hpp"
#include "http_server.hpp"
#include "lio.hpp"
#include "split_flap.hpp"
#include "spotify.hpp"

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
std::thread spotify_job_thread;

static void interrupt_handler(int) {
    app_running = false;
    app_cv.notify_all();

    http_server.stop();
    http_server_thread.join();
    timetable_job_thread.join();
    spotify_job_thread.join();

    delete matrix;

    std::cout << std::endl;
    exit(0);
}

static int usage(const char *progname) {
    fprintf(stderr, "usage: %s [options]\n", progname);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "\t-p <port>            : Port to listen on.\n");
    fprintf(stderr, "\t-d <data url>        : URL to ptrans-data.\n");
    fprintf(stderr, "\t-f <font-file>       : BDF font file path to use.\n");
    fprintf(stderr, "\t-s <settings-file>   : Config file path to use (will be "
                    "created if it does not exist).\n");
    rgb_matrix::PrintMatrixFlags(stderr);
    return 1;
}

Time nowTime() {
    auto now = std::chrono::system_clock::now();

    std::chrono::zoned_time zt{std::chrono::current_zone(), now};
    auto local = zt.get_local_time();

    auto since_midnight = local - std::chrono::floor<std::chrono::days>(local);

    auto h = std::chrono::duration_cast<std::chrono::hours>(since_midnight);
    auto m =
        std::chrono::duration_cast<std::chrono::minutes>(since_midnight - h);

    return Time{static_cast<int>(h.count()), static_cast<int>(m.count())};
}

std::string pad_utf8(const std::string &s, size_t width) {
    // Find the byte offset where the `width`-th codepoint starts (if any),
    // so we can truncate without splitting a multi-byte UTF-8 sequence.
    size_t codepoints = 0;
    size_t truncate_at = s.size(); // default: no truncation needed

    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        if ((c & 0xC0) != 0x80) {
            if (codepoints == width) {
                truncate_at = i;
                break;
            }
            codepoints++;
        }
        ++i;
    }

    std::string truncated = s.substr(0, truncate_at);

    size_t padding = (codepoints < width) ? width - codepoints : 0;
    return truncated + std::string(padding, ' ');
}

std::vector<SFChar> operator+(std::vector<SFChar> a,
                              const std::vector<SFChar> &b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

int main(int argc, char *argv[]) {
    rgb_matrix::RGBMatrix::Options matrix_options;
    rgb_matrix::RuntimeOptions runtime_opt;

    if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv, &matrix_options,
                                           &runtime_opt)) {
        return usage(argv[0]);
    }

    rgb_matrix::Color bg_color(0, 0, 0);

    int port = 0;
    std::string data_url = "";
    const char *bdf_font_file_path = NULL;
    const char *config_file_path = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "p:d:f:s:")) != -1) {
        switch (opt) {
        case 'p':
            port = std::stoi(optarg);
            break;
        case 'd':
            data_url = std::string(optarg);
            break;
        case 'f':
            bdf_font_file_path = strdup(optarg);
            break;
        case 's':
            config_file_path = strdup(optarg);
            break;
        default:
            return usage(argv[0]);
        }
    }

    if (bdf_font_file_path == NULL) {
        fprintf(stderr, "Need to specify a BDF font file path with -f\n");
        return usage(argv[0]);
    }

    if (config_file_path == NULL) {
        fprintf(stderr, "Need to specify a config file path with -s\n");
        return usage(argv[0]);
    }

    rgb_matrix::Font font;
    if (!font.LoadFont(bdf_font_file_path)) {
        fprintf(stderr, "Couldn't load font '%s'\n", bdf_font_file_path);
        return 1;
    }

    auto config_manager = ConfigManager(config_file_path);

    matrix =
        rgb_matrix::RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
    if (matrix == NULL) {
        return 1;
    }

    auto *offscreen = matrix->CreateFrameCanvas();

    signal(SIGTERM, interrupt_handler);
    signal(SIGINT, interrupt_handler);

    std::atomic<std::shared_ptr<const std::string>> text;
    std::atomic<std::shared_ptr<TimetableDto>> timetable;
    std::atomic<std::shared_ptr<std::optional<CurrentlyPlayingDto>>>
        currently_playing =
            std::make_shared<std::optional<CurrentlyPlayingDto>>(std::nullopt);

    auto run_http_server = make_http_server(APP_VERSION, config_manager, text);
    auto run_timetable_job =
        make_timetable_job(app_cv, app_mutex, app_running, timetable);
    auto run_spotify_job =
        make_spotify_job(app_cv, app_mutex, app_running, currently_playing);
    http_server_thread = std::thread(
        [&run_http_server, port]() { run_http_server(http_server, port); });
    timetable_job_thread = std::thread(
        [&run_timetable_job, data_url]() { run_timetable_job(data_url); });
    spotify_job_thread = std::thread(
        [&run_spotify_job, data_url]() { run_spotify_job(data_url); });

    // --- Derived layout constants ---
    // Matrix dimensions come from the matrix itself, so --led-chain etc.
    // are respected
    const auto sf_matrix_w = [&](bool only_2_displays) {
        return only_2_displays ? matrix->width() - 64 : matrix->width();
    };
    const int SF_MATRIX_H = matrix->height();
    const int SF_MS_PER_STEP = 25;

    // Font metrics derived from the loaded font
    const int SF_CHAR_W =
        font.CharacterWidth('M'); // 'M' as representative fixed width
    const int SF_CELL_GAP = 1;
    const int SF_CELL_H = font.baseline() + 2;
    const int SF_CELL_W = SF_CHAR_W;

    const int SF_NUM_ROWS = SF_MATRIX_H / SF_CELL_H;
    const auto sf_num_cols = [&](bool only_2_displays) {
        return sf_matrix_w(only_2_displays) / (SF_CELL_W + SF_CELL_GAP);
    };
    const auto sf_num_cells = [&](bool only_2_displays) {
        return SF_NUM_ROWS * sf_num_cols(only_2_displays);
    };

    const int SF_COL_LINE = 3;
    const auto sf_col_deps = [](bool only_2_displays) {
        return only_2_displays ? 3 : 9;
    };
    const int SF_COL_SPACE = 2;
    const auto sf_col_dir = [&](bool only_2_displays) {
        return sf_num_cols(only_2_displays) - SF_COL_LINE -
               sf_col_deps(only_2_displays) - SF_COL_SPACE;
    };

    const Color SF_BLACK = {0, 0, 0};

    std::vector<SFCell> cells(sf_num_cells(false));
    std::vector<SFChar> previous_target(sf_num_cells(false));

    auto sf_last_step = std::chrono::steady_clock::now();

    auto build_footer_pages = [&](const std::string &text,
                                  bool only_2_displays) {
        std::vector<std::string> pages;

        std::istringstream iss(text);
        std::string word;
        std::vector<std::string> words;

        while (iss >> word) {
            words.push_back(word);
        }

        // Worst case: " 99/99"
        constexpr int PAGE_INDICATOR_WIDTH = 6;
        const int TEXT_WIDTH =
            sf_num_cols(only_2_displays) - PAGE_INDICATOR_WIDTH;

        std::string current;

        for (const auto &w : words) {
            int needed = current.empty()
                             ? (int)sf_utf8_split(w).size()
                             : (int)sf_utf8_split(current + " " + w).size();

            if (needed > TEXT_WIDTH) {
                pages.push_back(current);
                current = w;
            } else {
                if (!current.empty()) {
                    current += ' ';
                }
                current += w;
            }
        }

        if (!current.empty()) {
            pages.push_back(current);
        }

        return pages;
    };

    bool lastFrameInBlackoutWindow = false;
    std::optional<CurrentlyPlayingDto> last_currently_playing;
    Image current_album_art;

    for (;;) {
        auto current_config = config_manager.get();

        if (matrix->brightness() != current_config->brightness) {
            matrix->SetBrightness(current_config->brightness);
        }

        if (current_config->blackout_window.isDuringBlackout(nowTime())) {
            if (!lastFrameInBlackoutWindow) {
                std::cout << std::format("{0:%F_%T} - Entered blackout window.",
                                         std::chrono::system_clock::now())
                          << std::endl;
                lastFrameInBlackoutWindow = true;
            }

            offscreen->Fill(bg_color.r, bg_color.g, bg_color.b);
            offscreen = matrix->SwapOnVSync(offscreen);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        if (lastFrameInBlackoutWindow) {
            std::cout << std::format("{0:%F_%T} - Exited blackout window.",
                                     std::chrono::system_clock::now())
                      << std::endl;
            lastFrameInBlackoutWindow = false;
        }

        auto current_currently_playing =
            currently_playing.load(std::memory_order_acquire);

        auto frame_start = std::chrono::steady_clock::now();
        bool step = std::chrono::duration_cast<std::chrono::milliseconds>(
                        frame_start - sf_last_step)
                        .count() >= SF_MS_PER_STEP;

        if (step) {
            sf_last_step = frame_start;
        }

        auto charset = sf_charset(current_config->colors.fg_default, true);
        std::vector<SFChar> new_target;

        if (current_config->mode == TEXT) {
            auto t = text.load(std::memory_order_acquire);
            if (!t) {
                new_target =
                    sf_pad_line(
                        sf_num_cols(current_currently_playing->has_value()),
                        "No text set! Go to",
                        current_config->colors.fg_default) +
                    sf_pad_line(
                        sf_num_cols(current_currently_playing->has_value()),
                        "ptrans.home.l3rchl.at",
                        current_config->colors.fg_default);
            } else {
                auto cps = sf_utf8_split(*t);
                new_target.resize(
                    sf_num_cells(current_currently_playing->has_value()));
                for (int i = 0;
                     i < sf_num_cells(current_currently_playing->has_value());
                     ++i) {
                    new_target[i] = {i < std::ssize(cps) ? cps[i] : " ",
                                     current_config->colors.fg_default};
                }
            }
        } else if (current_config->mode == PTRANS) {
            auto tt = timetable.load(std::memory_order_acquire);
            if (!tt) {
                charset = sf_charset(current_config->colors.fg_default, false);
                const int charset_size = (int)charset.size();
                const int perimeter_len =
                    2 * (SF_NUM_ROWS +
                         sf_num_cols(current_currently_playing->has_value())) -
                    4;

                static int frame_t = 0;
                static int revealed = 0;
                frame_t++;
                if (revealed < perimeter_len)
                    revealed++;

                const std::string WAITING = "Waiting for timetable";
                auto waiting_cps = sf_utf8_split(WAITING);
                int text_start_col =
                    (sf_num_cols(current_currently_playing->has_value()) -
                     (int)waiting_cps.size()) /
                    2;
                int text_row = SF_NUM_ROWS / 2;

                new_target.resize(
                    sf_num_cells(current_currently_playing->has_value()));
                for (int i = 0;
                     i < sf_num_cells(current_currently_playing->has_value());
                     ++i) {
                    int col =
                        i % sf_num_cols(current_currently_playing->has_value());
                    int row =
                        i / sf_num_cols(current_currently_playing->has_value());

                    bool is_frame =
                        (row == 0 || row == SF_NUM_ROWS - 1 || col == 0 ||
                         col == sf_num_cols(
                                    current_currently_playing->has_value()) -
                                    1);
                    bool is_text =
                        (row == text_row && col >= text_start_col &&
                         col < text_start_col + (int)waiting_cps.size());

                    if (is_text && !is_frame) {
                        int text_col = col - text_start_col;
                        new_target[i] = {waiting_cps[text_col],
                                         current_config->colors.fg_default};
                    } else if (is_frame) {
                        int perimeter_pos =
                            (col == 0)                 ? row
                            : (row == SF_NUM_ROWS - 1) ? (SF_NUM_ROWS - 1 + col)
                            : (col ==
                               sf_num_cols(
                                   current_currently_playing->has_value()) -
                                   1)
                                ? (SF_NUM_ROWS - 1 +
                                   sf_num_cols(
                                       current_currently_playing->has_value()) -
                                   1 + (SF_NUM_ROWS - 1 - row))
                                : (2 * (SF_NUM_ROWS - 1) +
                                   sf_num_cols(
                                       current_currently_playing->has_value()) -
                                   1 +
                                   (sf_num_cols(current_currently_playing
                                                    ->has_value()) -
                                    1 - col));

                        if (perimeter_pos < revealed) {
                            int idx = (perimeter_pos + frame_t) % charset_size;
                            new_target[i] = {charset[idx].glyph,
                                             current_config->colors.fg_default};
                        } else {
                            new_target[i] = {" ", {0, 0, 0}};
                        }
                    } else {
                        new_target[i] = {" ", {0, 0, 0}};
                    }
                }
            } else {
                auto departure_color = [&](bool real_time, bool late,
                                           bool traffic_jam) -> Color {
                    if (!real_time) {
                        return current_config->colors.fg_default;
                    } else if (traffic_jam) {
                        return current_config->colors.fg_traffic;
                    } else if (late) {
                        return current_config->colors.fg_late;
                    } else {
                        return current_config->colors.fg_punctual;
                    }
                };

                struct DepEntry {
                    std::string str;
                    Color color;
                };
                struct DisplayLine {
                    std::string line_name;
                    std::string direction;
                    std::vector<DepEntry> deps;
                };

                std::vector<DisplayLine> display_lines;

                for (auto &trip : tt->trips) {
                    DisplayLine dl;
                    dl.line_name = trip.line;
                    dl.direction = trip.direction;

                    for (auto &&dep :
                         trip.departures |
                             std::views::take(
                                 current_currently_playing->has_value() ? 1
                                                                        : 3)) {
                        std::string s = dep.countdown == 0
                                            ? "*"
                                            : std::to_string(dep.countdown);
                        dl.deps.push_back(
                            {s, departure_color(dep.real_time, dep.late,
                                                dep.traffic_jam)});
                    }
                    if (dl.deps.empty()) {
                        dl.deps.push_back(
                            {"N/A", current_config->colors.fg_default});
                    }

                    display_lines.push_back(dl);
                }

                new_target.clear();
                for (int row = 0; row < SF_NUM_ROWS - 1; ++row) {
                    if (row >= (int)display_lines.size()) {
                        auto padding = sf_pad_line(
                            sf_num_cols(current_currently_playing->has_value()),
                            "", current_config->colors.fg_default);
                        new_target.insert(new_target.end(), padding.begin(),
                                          padding.end());
                        continue;
                    }

                    auto &dl = display_lines[row];

                    // line name + direction in default color
                    int SF_COL_DIR =
                        sf_col_dir(current_currently_playing->has_value());
                    std::string prefix = std::format(
                        "{:<{}} {:<{}} ", dl.line_name, SF_COL_LINE,
                        pad_utf8(dl.direction, SF_COL_DIR), SF_COL_DIR);
                    for (auto &cp : sf_utf8_split(prefix)) {
                        new_target.push_back(
                            {cp, current_config->colors.fg_default});
                    }

                    // departure times, each in its own color, right-aligned in
                    // SF_COL_DEPS columns
                    std::vector<SFChar> dep_cells;
                    for (int d = 0; d < (int)dl.deps.size(); ++d) {
                        if (d > 0) {
                            dep_cells.push_back(
                                {" ", current_config->colors.fg_default});
                        }

                        for (auto &cp : sf_utf8_split(dl.deps[d].str)) {
                            dep_cells.push_back({cp, dl.deps[d].color});
                        }
                    }

                    int pad =
                        sf_col_deps(current_currently_playing->has_value()) -
                        (int)dep_cells.size();
                    for (int p = 0; p < pad; ++p) {
                        new_target.push_back(
                            {" ", current_config->colors.fg_default});
                    }

                    for (auto &c : dep_cells) {
                        new_target.push_back(c);
                    }
                }

                if (tt->message.has_value()) {
                    std::string footer_line = tt->message.value();
                    if (footer_line.length() >
                        static_cast<size_t>(sf_num_cols(
                            current_currently_playing->has_value()))) {
                        auto pages = build_footer_pages(
                            footer_line,
                            current_currently_playing->has_value());

                        auto now = std::chrono::steady_clock::now();
                        auto seconds =
                            std::chrono::duration_cast<std::chrono::seconds>(
                                now.time_since_epoch())
                                .count();

                        size_t page = (seconds / 10) % pages.size();

                        std::string indicator =
                            std::format("{}/{}", page + 1, pages.size());

                        footer_line = std::format(
                            "{:<{}}{:>{}}", pages[page],
                            sf_num_cols(
                                current_currently_playing->has_value()) -
                                (int)indicator.size(),
                            indicator, (int)indicator.size());
                    }

                    for (auto &cp : sf_utf8_split(footer_line)) {
                        new_target.push_back(
                            {cp, current_config->colors.fg_default});
                    }
                }
            }
        } else {
            new_target =
                sf_pad_line(sf_num_cols(current_currently_playing->has_value()),
                            "No mode set! Go to",
                            current_config->colors.fg_default) +
                sf_pad_line(sf_num_cols(current_currently_playing->has_value()),
                            "ptrans.home.l3rchl.at",
                            current_config->colors.fg_default);
        }

        offscreen->Fill(bg_color.r, bg_color.g, bg_color.b);

        if (current_currently_playing->has_value() &&
            current_currently_playing->value().album_cover_url.has_value() &&
            (!last_currently_playing.has_value() ||
             !last_currently_playing.value().album_cover_url.has_value() ||
             last_currently_playing.value().album_cover_url.value() !=
                 current_currently_playing->value().album_cover_url.value())) {
            std::string raw_bytes;
            if (!download_to_memory(
                    current_currently_playing->value().album_cover_url.value(),
                    &raw_bytes)) {
                std::cerr << "Failed to download image from "
                          << current_currently_playing->value()
                                 .album_cover_url.value()
                          << "\n";
                return 1;
            }
            std::cout << "Downloaded" << std::endl;

            if (!decode_image(raw_bytes, &current_album_art)) {
                std::cerr << "Failed to decode image\n";
                return 1;
            }
            std::cout << "Decoded image: " << current_album_art.width << "x"
                      << current_album_art.height << "\n";
        }

        static float album_art_angle = 0.0f;
        static auto album_art_last_update = std::chrono::steady_clock::now();
        const float rotations_per_second = 0.05f; // tune to taste

        auto now = std::chrono::steady_clock::now();
        float delta_seconds =
            std::chrono::duration<float>(now - album_art_last_update).count();
        album_art_last_update = now;

        bool is_playing = current_currently_playing->has_value() &&
                          !current_currently_playing->value().is_paused;

        if (is_playing) {
            album_art_angle += 2.0f * static_cast<float>(M_PI) *
                               rotations_per_second * delta_seconds;
            if (album_art_angle > 2.0f * static_cast<float>(M_PI)) {
                album_art_angle -= 2.0f * static_cast<float>(M_PI);
            }
        }

        if (current_currently_playing->has_value() &&
            current_album_art.width > 0) {
            draw_spinning_circle(current_album_art, album_art_angle, 64, 128, 0,
                                 offscreen);
        }

        sf_update_cells(sf_num_cells(current_currently_playing->has_value()),
                        SF_BLACK, cells, previous_target, new_target, charset);
        sf_render_cells(sf_num_cells(current_currently_playing->has_value()),
                        sf_num_cols(current_currently_playing->has_value()),
                        SF_CELL_W, SF_CELL_H, SF_CELL_GAP, font, offscreen,
                        cells, step, charset);

        last_currently_playing = *current_currently_playing;

        offscreen = matrix->SwapOnVSync(offscreen);
    }
}
