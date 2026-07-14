#include <graphics.h>
#include <led-matrix.h>
#include <string>

#include "config.hpp"

struct SFChar {
    std::string glyph;
    Color color;
    bool operator==(const SFChar &other) const;
};

struct SFCell {
    int char_index = 0;
    int target_index = 0;
    int steps_left = 0;
    bool flipping = false;
    rgb_matrix::Color target_color = rgb_matrix::Color(255, 255, 255);
};

const std::vector<SFChar> SF_BLOCK_CHARS = {
    {"█", {255, 80, 80}}, {"█", {255, 160, 0}}, {"█", {255, 255, 0}},
    {"█", {80, 255, 80}}, {"█", {0, 200, 255}}, {"█", {160, 80, 255}},
};

std::vector<SFChar> sf_charset(const Color &color, bool block_chars);

int sf_charset_index(const std::vector<SFChar> &charset,
                     const std::string &glyph);

std::vector<std::string> sf_utf8_split(const std::string &s);

std::vector<SFChar> sf_pad_line(int num_cols, const std::string &s,
                                const Color &color);

void sf_update_cells(int num_cells, const Color &black,
                     std::vector<SFCell> &cells,
                     std::vector<SFChar> &last_target,
                     const std::vector<SFChar> &new_target,
                     const std::vector<SFChar> &charset);

// During a flip the cell cycles through charset glyphs. Intermediate glyphs
// are drawn in fg_default; only the final target glyph gets target_color.
void sf_render_cells(int num_cells, int num_cols, int cell_w, int cell_h,
                     int cell_gap, const rgb_matrix::Font &font,
                     rgb_matrix::FrameCanvas *offscreen,
                     std::vector<SFCell> &cells, bool step,
                     const std::vector<SFChar> &charset);
