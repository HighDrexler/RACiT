// Direct2D / DirectWrite drawing in A Crack in Time's look (the same palette and
// shapes as the Android launcher's Acit.java): deep navy panels with thin glowing
// cyan edges and cut corners, orange for what is focused, condensed bold capitals.
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <string>
#include "util.h"

namespace col {
// Taken from the game's key art (PIC1): the Great Clock's teal lightning, the
// amber sun, deep blue-green space.
constexpr uint32_t BG_TOP = 0xFF06141A, BG_BOTTOM = 0xFF0B2228;
constexpr uint32_t PANEL = 0xC80A1C22, PANEL_DARK = 0xD0081519, PANEL_HOT = 0xE0163A40;
constexpr uint32_t EDGE = 0xFF6FE6F0, EDGE_DIM = 0xFF2E6B74, GLOW = 0x806FE6F0;
constexpr uint32_t ORANGE = 0xFFFFB43C, ORANGE_GLOW = 0xA0FFB43C;   // amber
constexpr uint32_t TEXT = 0xFFF4FBFF, TEXT_DIM = 0xFFA7C6CC, TEXT_BAD = 0xFFFF7A5C, TEXT_GOOD = 0xFF8EF0D0;
constexpr uint32_t TEXT_OFF = 0xFF557A80;
}

enum Font { F_BRAND, F_TITLE, F_SUB, F_BODY, F_LABEL, F_BUTTON, F_BUTTON_SUB, F_HINT, F_BIG, F_COUNT };
enum Align { A_LEFT, A_CENTER, A_RIGHT };

struct Rect { float l, t, r, b; float w() const { return r - l; } float h() const { return b - t; }
              bool in(float x, float y) const { return x >= l && x < r && y >= t && y < b; } };

class Gfx {
public:
    bool init(HWND hwnd);
    void shutdown();
    bool begin();                          // false = nothing to draw into right now
    void end();                            // handles a lost device
    void resize();
    float width() const { return w_; }
    float height() const { return h_; }
    float dpi() const { return dpi_; }
    void set_dpi(float d);

    void clear(uint32_t c);
    void fill(const Rect& r, uint32_t c);
    void vgradient(const Rect& r, uint32_t top, uint32_t bottom);
    void hgradient(const Rect& r, uint32_t left, uint32_t right);
    void chamfer_fill(const Rect& r, float cut, uint32_t c);
    void chamfer_stroke(const Rect& r, float cut, uint32_t c, float width);
    void chamfer_glow(const Rect& r, float cut, uint32_t c, float width);   // a soft halo outside the edge
    void line(float x0, float y0, float x1, float y1, uint32_t c, float w);
    void hexagon(float cx, float cy, float r, uint32_t fill, uint32_t stroke, float sw);
    void circle(float cx, float cy, float r, uint32_t stroke, float sw);
    void text(const std::string& s, const Rect& r, Font f, uint32_t c, Align a = A_LEFT, bool wrap = false, bool vcenter = true);
    float text_width(const std::string& s, Font f);
    float text_height(const std::string& s, Font f, float width);   // wrapped
    void clip_push(const Rect& r);
    void clip_pop();

    // Images (PNG via WIC), kept until the device goes; empty path = none.
    bool image(const fs::path& p, const Rect& r, float opacity, bool cover);
    bool image_size(const fs::path& p, float* w, float* h);
    void forget_images();

    // Draw one frame offscreen (WIC) and write it as a PNG: the launcher's test shots.
    bool snapshot(const std::wstring& png, void (*draw)());

private:
    ID2D1Factory* f_ = nullptr;
    IDWriteFactory* dw_ = nullptr;
    IWICImagingFactory* wic_ = nullptr;
    ID2D1RenderTarget* rt_ = nullptr;           // what draws: the window's target, or a snapshot's
    ID2D1HwndRenderTarget* hrt_ = nullptr;
    ID2D1SolidColorBrush* br_ = nullptr;
    IDWriteTextFormat* fmt_[F_COUNT] = {};
    HWND hwnd_ = nullptr;
    float w_ = 0, h_ = 0, dpi_ = 96;
    struct Img { fs::path path; ID2D1Bitmap* bmp = nullptr; float w = 0, h = 0; };
    Img imgs_[4];
    bool make_target();
    void drop_target();
    ID2D1SolidColorBrush* brush(uint32_t c);
    ID2D1PathGeometry* chamfer_geo(const Rect& r, float cut);
    Img* load(const fs::path& p);
};

D2D1_COLOR_F d2c(uint32_t argb);
