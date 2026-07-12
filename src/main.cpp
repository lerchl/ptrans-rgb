#include "graphics.h"
#include "http_server.hpp"
#include "led-matrix.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <atomic>
#include <chrono>
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

#include "config.hpp"
#include "config_manager.hpp"
#include "http_server.hpp"
#include "lio.hpp"
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

struct SFChar {
    std::string glyph;
    Color color;
};

bool operator==(const Color &a, const Color &b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

bool operator==(const SFChar &a, const SFChar &b) {
    return a.glyph == b.glyph && a.color == b.color;
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
    spotify_job_thread.join();

    delete matrix;

    std::cout << std::endl;
    exit(0);
}

// Splits a URL into "scheme://host[:port]" and "/path" for httplib::Client.
// Very small and deliberately not a general-purpose URL parser.
bool split_url(const std::string &url, std::string *origin, std::string *path) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
        return false;

    const auto path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) {
        *origin = url;
        *path = "/";
    } else {
        *origin = url.substr(0, path_start);
        *path = url.substr(path_start);
    }
    return true;
}

// Downloads raw bytes from a URL into memory using httplib. Returns true on
// success.
bool download_to_memory(const std::string &url, std::string *out) {
    std::string origin, path;
    if (!split_url(url, &origin, &path)) {
        std::cerr << "Could not parse URL: " << url << "\n";
        return false;
    }

    httplib::Client client(origin);
    client.set_follow_location(true);
    client.set_connection_timeout(10);
    client.set_read_timeout(15);

    auto res = client.Get(path);
    if (!res) {
        std::cerr << "HTTP request failed: " << httplib::to_string(res.error())
                  << "\n";
        return false;
    }
    if (res->status < 200 || res->status >= 300) {
        std::cerr << "HTTP error: " << res->status << "\n";
        return false;
    }

    *out = std::move(res->body);
    return true;
}

// A simple RGB image decoded into a flat pixel buffer.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // RGB, 3 bytes per pixel, row-major
};

bool decode_image(const std::string &bytes, Image *img) {
    int w, h, channels;
    const auto *raw = reinterpret_cast<const uint8_t *>(bytes.data());
    uint8_t *data = stbi_load_from_memory(raw, static_cast<int>(bytes.size()),
                                          &w, &h, &channels, 3 /* force RGB */);
    if (!data) {
        std::cerr << "stb_image decode failed: " << stbi_failure_reason()
                  << "\n";
        return false;
    }
    img->width = w;
    img->height = h;
    img->pixels.assign(data, data + (static_cast<size_t>(w) * h * 3));
    stbi_image_free(data);
    return true;
}

// Resizes `src` to `dst_size` x `dst_size` using bilinear sampling and
// draws it into `canvas` at (offset_x, offset_y).
void draw_resized_square(const Image &src, int dst_size, int offset_x,
                         int offset_y, rgb_matrix::FrameCanvas *canvas) {
    const float scale_x = static_cast<float>(src.width) / dst_size;
    const float scale_y = static_cast<float>(src.height) / dst_size;

    for (int y = 0; y < dst_size; ++y) {
        for (int x = 0; x < dst_size; ++x) {
            const float sx = x * scale_x;
            const float sy = y * scale_y;

            const int x0 = std::min(static_cast<int>(sx), src.width - 2);
            const int y0 = std::min(static_cast<int>(sy), src.height - 2);
            const float fx = sx - x0;
            const float fy = sy - y0;

            auto lerp = [](float a, float b, float t) {
                return a + (b - a) * t;
            };

            uint8_t rgb[3];
            for (int c = 0; c < 3; ++c) {
                const float p00 = src.pixels[(y0 * src.width + x0) * 3 + c];
                const float p10 = src.pixels[(y0 * src.width + x0 + 1) * 3 + c];
                const float p01 =
                    src.pixels[((y0 + 1) * src.width + x0) * 3 + c];
                const float p11 =
                    src.pixels[((y0 + 1) * src.width + x0 + 1) * 3 + c];
                const float top = lerp(p00, p10, fx);
                const float bottom = lerp(p01, p11, fx);
                rgb[c] =
                    static_cast<uint8_t>(std::round(lerp(top, bottom, fy)));
            }

            canvas->SetPixel(offset_x + x, offset_y + y, rgb[0], rgb[1],
                             rgb[2]);
        }
    }
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
    size_t codepoints = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) {
            codepoints++;
        }
    }
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
    std::atomic<std::shared_ptr<CurrentlyPlayingDto>> currently_playing;

    auto run_http_server = make_http_server(APP_VERSION, config_manager, text);
    auto run_timetable_job =
        make_timetable_job(app_cv, app_mutex, app_running, timetable);
    auto run_spotify_job =
        make_spotify_job(app_cv, app_mutex, app_running, currently_playing);
    http_server_thread = std::thread(
        [&run_http_server, port]() { run_http_server(http_server, port); });
    timetable_job_thread = std::thread(
        [&run_timetable_job, data_url]() { run_timetable_job(data_url); });
    spotify_job_thread =
        std::thread([&run_spotify_job, data_url]() { run_spotify_job(data_url); });

    // --- Derived layout constants ---
    // Matrix dimensions come from the matrix itself, so --led-chain etc.
    // are respected
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

    const Color SF_BLACK = {0, 0, 0};

    const std::vector<SFChar> SF_BLOCK_CHARS = {
        {"█", {255, 80, 80}}, {"█", {255, 160, 0}}, {"█", {255, 255, 0}},
        {"█", {80, 255, 80}}, {"█", {0, 200, 255}}, {"█", {160, 80, 255}},
    };

    // charset is built with the current fg_default so normal glyphs carry that
    // color. Block chars always keep their fixed colors.
    auto sf_charset = [&](const Color &fg_default, bool block_chars) {
        std::vector<SFChar> cs;

        if (block_chars) {
            for (auto &b : SF_BLOCK_CHARS) {
                cs.push_back(b);
            }
        }

        for (auto &g : std::vector<std::string>{
                 " ", "A", "Ä", "B", "C", "D", "E",  "F", "G", "H", "I", "J",
                 "K", "L", "M", "N", "O", "Ö", "P",  "Q", "R", "S", "T", "U",
                 "Ü", "V", "W", "X", "Y", "Z", "a",  "ä", "b", "c", "d", "e",
                 "f", "g", "h", "i", "j", "k", "l",  "m", "n", "o", "ö", "p",
                 "q", "r", "s", "ß", "t", "u", "ü",  "v", "w", "x", "y", "z",
                 "0", "1", "2", "3", "4", "5", "6",  "7", "8", "9", ".", ",",
                 ":", ";", " ", "!", "?", "-", "–",  "(", ")", "/", "@", "#",
                 "%", "&", "=", "+", "_", "'", "\"", "$", "€", "*"}) {
            cs.push_back({g, fg_default});
        }

        return cs;
    };

    auto sf_charset_index = [&](const std::vector<SFChar> &charset,
                                const std::string &glyph) {
        int size = (int)charset.size();
        for (int i = 0; i < size; ++i) {
            if (charset[i].glyph == glyph) {
                return i;
            }
        }
        return 0;
    };

    auto sf_utf8_split = [](const std::string &s) {
        std::vector<std::string> result;
        size_t i = 0;
        while (i < s.size()) {
            unsigned char c = s[i];
            int len = 1;
            if ((c & 0xE0) == 0xC0) {
                len = 2;
            } else if ((c & 0xF0) == 0xE0) {
                len = 3;
            } else if ((c & 0xF8) == 0xF0) {
                len = 4;
            }
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
        rgb_matrix::Color target_color = rgb_matrix::Color(255, 255, 255);
    };

    std::vector<SFCell> cells(SF_NUM_CELLS);
    std::vector<SFChar> previous_target(SF_NUM_CELLS);

    auto sf_last_step = std::chrono::steady_clock::now();

    auto sf_update_cells = [&](std::vector<SFCell> &cells,
                               std::vector<SFChar> &last_target,
                               const std::vector<SFChar> &new_target,
                               const std::vector<SFChar> &charset) {
        if (new_target == last_target) {
            return;
        }

        last_target = new_target;
        int charset_size = (int)charset.size();
        for (int i = 0; i < SF_NUM_CELLS; ++i) {
            const SFChar &tc = (i < (int)new_target.size())
                                   ? new_target[i]
                                   : SFChar{" ", SF_BLACK};
            int ti = sf_charset_index(charset, tc.glyph);
            int steps =
                (ti - cells[i].char_index + charset_size) % charset_size;
            cells[i].target_index = ti;
            cells[i].target_color =
                rgb_matrix::Color(tc.color.r, tc.color.g, tc.color.b);
            cells[i].steps_left = steps;
            cells[i].flipping = (steps > 0);
        }
    };

    // During a flip the cell cycles through charset glyphs. Intermediate glyphs
    // are drawn in fg_default; only the final target glyph gets target_color.
    auto sf_render_cells = [&](std::vector<SFCell> &cells, bool step,
                               const std::vector<SFChar> &charset,
                               bool skip_first_display) {
        int charset_size = (int)charset.size();
        for (int i = skip_first_display ? 9 : 0; i < SF_NUM_CELLS; ++i) {
            SFCell &cell = cells[i];
            int row = i / SF_NUM_COLS;
            int col = i % SF_NUM_COLS;
            int px = 1 + col * (SF_CELL_W + SF_CELL_GAP);
            int py = font.baseline() + row * SF_CELL_H;

            // Use target color only when settled on the target glyph.
            rgb_matrix::Color draw_color = cell.flipping
                                               ? charset[cell.char_index].color
                                               : cell.target_color;

            const SFChar &sc = charset[cell.char_index];
            rgb_matrix::DrawText(offscreen, font, px, py, draw_color, nullptr,
                                 sc.glyph.c_str());

            if (cell.flipping && step) {
                cell.char_index = (cell.char_index + 1) % charset_size;
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

    auto build_footer_pages = [&](const std::string &text) {
        std::vector<std::string> pages;

        std::istringstream iss(text);
        std::string word;
        std::vector<std::string> words;

        while (iss >> word) {
            words.push_back(word);
        }

        // Worst case: " 99/99"
        constexpr int PAGE_INDICATOR_WIDTH = 6;
        const int TEXT_WIDTH = SF_NUM_COLS - PAGE_INDICATOR_WIDTH;

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
                new_target = sf_pad_line("No text set! Go to",
                                         current_config->colors.fg_default) +
                             sf_pad_line("ptrans.home.l3rchl.at",
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
                charset = sf_charset(current_config->colors.fg_default, false);
                const int charset_size = (int)charset.size();
                const int perimeter_len = 2 * (SF_NUM_ROWS + SF_NUM_COLS) - 4;

                static int frame_t = 0;
                static int revealed = 0;
                frame_t++;
                if (revealed < perimeter_len)
                    revealed++;

                const std::string WAITING = "Waiting for timetable";
                auto waiting_cps = sf_utf8_split(WAITING);
                int text_start_col =
                    (SF_NUM_COLS - (int)waiting_cps.size()) / 2;
                int text_row = SF_NUM_ROWS / 2;

                new_target.resize(SF_NUM_CELLS);
                for (int i = 0; i < SF_NUM_CELLS; ++i) {
                    int col = i % SF_NUM_COLS;
                    int row = i / SF_NUM_COLS;

                    bool is_frame = (row == 0 || row == SF_NUM_ROWS - 1 ||
                                     col == 0 || col == SF_NUM_COLS - 1);
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
                            : (col == SF_NUM_COLS - 1)
                                ? (SF_NUM_ROWS - 1 + SF_NUM_COLS - 1 +
                                   (SF_NUM_ROWS - 1 - row))
                                : (2 * (SF_NUM_ROWS - 1) + SF_NUM_COLS - 1 +
                                   (SF_NUM_COLS - 1 - col));

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
                             std::views::take(current_currently_playing ? 1
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

                    int pad = SF_COL_DEPS - (int)dep_cells.size();
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
                        static_cast<size_t>(SF_NUM_COLS)) {
                        auto pages = build_footer_pages(footer_line);

                        auto now = std::chrono::steady_clock::now();
                        auto seconds =
                            std::chrono::duration_cast<std::chrono::seconds>(
                                now.time_since_epoch())
                                .count();

                        size_t page = (seconds / 10) % pages.size();

                        std::string indicator =
                            std::format("{}/{}", page + 1, pages.size());

                        footer_line =
                            std::format("{:<{}}{:>{}}", pages[page],
                                        SF_NUM_COLS - (int)indicator.size(),
                                        indicator, (int)indicator.size());
                    }

                    for (auto &cp : sf_utf8_split(footer_line)) {
                        new_target.push_back(
                            {cp, current_config->colors.fg_default});
                    }
                }
            }
        } else {
            new_target = sf_pad_line("No mode set! Go to",
                                     current_config->colors.fg_default) +
                         sf_pad_line("ptrans.home.l3rchl.at",
                                     current_config->colors.fg_default);
        }

        offscreen->Fill(bg_color.r, bg_color.g, bg_color.b);

        static Image img;
        if (img.width == 0 &&
            current_currently_playing->album_cover_url.has_value()) {
            std::string raw_bytes;
            if (!download_to_memory(
                    current_currently_playing->album_cover_url.value(),
                    &raw_bytes)) {
                std::cerr << "Failed to download image from "
                          << current_currently_playing->album_cover_url.value()
                          << "\n";
                return 1;
            }

            if (!decode_image(raw_bytes, &img)) {
                std::cerr << "Failed to decode image\n";
                return 1;
            }
            std::cout << "Decoded image: " << img.width << "x" << img.height
                      << "\n";
        }
        draw_resized_square(img, 64, 0, 0, offscreen);

        sf_update_cells(cells, previous_target, new_target, charset);
        sf_render_cells(cells, step, charset,
                        current_currently_playing ? true : false);

        offscreen = matrix->SwapOnVSync(offscreen);
    }
}
