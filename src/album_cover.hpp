#include <httplib.h>
#include <led-matrix.h>

#include <string>

// A simple RGB image decoded into a flat pixel buffer.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // RGB, 3 bytes per pixel, row-major
};

// Splits a URL into "scheme://host[:port]" and "/path" for httplib::Client.
// Very small and deliberately not a general-purpose URL parser.
bool split_url(const std::string &url, std::string *origin, std::string *path);

// Downloads raw bytes from a URL into memory using httplib. Returns true on
// success.
bool download_to_memory(const std::string &url, std::string *out);

bool decode_image(const std::string &bytes, Image *img);

// Resizes `src` to `dst_size` x `dst_size` using bilinear sampling and
// draws it into `canvas` at (offset_x, offset_y).
void draw_resized_square(const Image &src, int dst_size, int offset_x,
                         int offset_y, rgb_matrix::FrameCanvas *canvas);

// Rotates `src` by `angle_radians` around its own center, crops to a circle
// of diameter `dst_size`, and draws it into `canvas` at (offset_x, offset_y).
// Pixels outside the circle are left untouched (transparent), so make sure
// you've already cleared/filled the canvas background before calling this.
void draw_spinning_circle(const Image &src, float angle_radians, int dst_size,
                          int offset_x, int offset_y,
                          rgb_matrix::FrameCanvas *canvas, float padding);
