#include "split_flap.hpp"

bool SFChar::operator==(const SFChar &other) const {
    return glyph == other.glyph && color == other.color;
}

std::vector<SFChar> sf_charset(const Color &color, bool block_chars) {
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
        cs.push_back({g, color});
    }

    return cs;
};

int sf_charset_index(const std::vector<SFChar> &charset,
                     const std::string &glyph) {
    int size = (int)charset.size();
    for (int i = 0; i < size; ++i) {
        if (charset[i].glyph == glyph) {
            return i;
        }
    }
    return 0;
};

std::vector<std::string> sf_utf8_split(const std::string &s) {
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

std::vector<SFChar> sf_pad_line(int num_cols, const std::string &s,
                                const Color &color) {
    auto cps = sf_utf8_split(s);
    std::vector<SFChar> result;
    for (int i = 0; i < num_cols; ++i) {
        std::string glyph = (i < (int)cps.size()) ? cps[i] : " ";
        result.push_back({glyph, color});
    }
    return result;
};

void sf_update_cells(int num_cells, const Color &black,
                     std::vector<SFCell> &cells,
                     std::vector<SFChar> &last_target,
                     const std::vector<SFChar> &new_target,
                     const std::vector<SFChar> &charset) {
    if (new_target == last_target) {
        return;
    }

    last_target = new_target;
    int charset_size = (int)charset.size();
    for (int i = 0; i < num_cells; ++i) {
        const SFChar &tc =
            (i < (int)new_target.size()) ? new_target[i] : SFChar{" ", black};
        int ti = sf_charset_index(charset, tc.glyph);
        int steps = (ti - cells[i].char_index + charset_size) % charset_size;
        cells[i].target_index = ti;
        cells[i].target_color =
            rgb_matrix::Color(tc.color.r, tc.color.g, tc.color.b);
        cells[i].steps_left = steps;
        cells[i].flipping = (steps > 0);
    }
};

// During a flip the cell cycles through charset glyphs. Intermediate glyphs
// are drawn in fg_default; only the final target glyph gets target_color.
void sf_render_cells(int num_cells, int num_cols, int cell_w, int cell_h,
                     int cell_gap, const rgb_matrix::Font &font,
                     rgb_matrix::FrameCanvas *offscreen,
                     std::vector<SFCell> &cells, bool step,
                     const std::vector<SFChar> &charset, int offset_x) {
    int charset_size = (int)charset.size();
    for (int i = 0; i < num_cells; ++i) {
        SFCell &cell = cells[i];
        int row = i / num_cols;
        int col = i % num_cols;
        int px = offset_x + 1 + col * (cell_w + cell_gap);
        int py = font.baseline() + row * cell_h;

        // Use target color only when settled on the target glyph.
        rgb_matrix::Color draw_color =
            cell.flipping ? charset[cell.char_index].color : cell.target_color;

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
