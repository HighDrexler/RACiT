// Gfx on macOS: the drawing API of gfx.h, done by CoreGraphics into a bitmap the size of the
// window's pixels (Retina: twice its points), shown through an SDL texture. Coordinates are
// points with y down, as Direct2D's DIPs are; text is CoreText in the system font's condensed
// width (Bahnschrift's place), images come through ImageIO. snapshot() draws the same frame
// into its own bitmap and writes it as a PNG, with or without a window.
// MacTypes.h has a QuickDraw `struct Rect` of its own: renamed while the system headers
// are read, so gfx.h's Rect is the only one here.
#define Rect MacTypes_Rect
#import <AppKit/AppKit.h>
#include <CoreText/CoreText.h>
#include <ImageIO/ImageIO.h>
#undef Rect
#include <SDL2/SDL.h>

#include "gfx.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

namespace {
uint64_t now_ms()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint32_t with_alpha(uint32_t c, float a)
{
    const float base = ((c >> 24) & 255) / 255.0f;
    const uint32_t na = (uint32_t)(255.0f * base * a + 0.5f);
    return (na << 24) | (c & 0xFFFFFF);
}

void comps(uint32_t c, CGFloat* o)
{
    o[0] = ((c >> 16) & 255) / 255.0; o[1] = ((c >> 8) & 255) / 255.0; o[2] = (c & 255) / 255.0; o[3] = ((c >> 24) & 255) / 255.0;
}

// Laid-out text, kept between frames (the screens draw the same strings 30 times a second).
struct Laid {
    std::vector<CTLineRef> lines;
    std::vector<double> widths;   // each line's, trailing whitespace left out (for alignment)
    double max_width = 0;         // with trailing whitespace (text_width)
    ~Laid() { for (CTLineRef l : lines) CFRelease(l); }
};
}  // namespace

struct Gfx::Mac {
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    SDL_Texture* tex = nullptr;
    int tex_w = 0, tex_h = 0;
    CGColorSpaceRef cs = nullptr;
    CGContextRef win_ctx = nullptr;   // the window's frame
    CGContextRef ctx = nullptr;       // what draws now: win_ctx, or a snapshot's
    int pw = 0, ph = 0;
    float scale = 1;
    CTFontRef font[F_COUNT] = {};
    double ascent[F_COUNT] = {}, line_h[F_COUNT] = {};
    std::unordered_map<std::string, std::unique_ptr<Laid>> laid;

    struct Img { fs::path path; CGImageRef img = nullptr; float w = 0, h = 0;
                 CGImageRef scaled = nullptr; int sw = 0, sh = 0; CGRect src{}; };
    Img imgs[4];
    struct Miss { fs::path path; uint64_t retry = 0; };
    Miss miss[4];

    CGContextRef make_ctx(int w, int h)
    {
        CGContextRef c = CGBitmapContextCreate(nullptr, (size_t)w, (size_t)h, 8, (size_t)w * 4, cs,
                                               kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        if (!c) return nullptr;
        // Points, y down: Direct2D's coordinate system.
        CGContextTranslateCTM(c, 0, h);
        CGContextScaleCTM(c, scale, -scale);
        CGContextSetShouldSmoothFonts(c, false);   // grayscale anti-aliasing, as the Windows launcher
        CGContextSetInterpolationQuality(c, kCGInterpolationHigh);
        CGContextSetTextMatrix(c, CGAffineTransformMakeScale(1, -1));
        return c;
    }
};

static CTFontRef make_font(float size, int weight /*0 normal, 1 semibold, 2 bold*/, bool condensed)
{
    const NSFontWeight w = weight == 2 ? NSFontWeightBold : weight == 1 ? NSFontWeightSemibold : NSFontWeightRegular;
    NSFont* f = nil;
    if (@available(macOS 13.0, *)) f = [NSFont systemFontOfSize:size weight:w width:condensed ? NSFontWidthCondensed : NSFontWidthStandard];
    if (!f && condensed)
        f = [NSFont fontWithName:weight == 2 ? @"AvenirNextCondensed-Bold" : weight == 1 ? @"AvenirNextCondensed-DemiBold" : @"AvenirNextCondensed-Regular" size:size];
    if (!f) f = [NSFont systemFontOfSize:size weight:w];
    return (CTFontRef)CFBridgingRetain(f);
}

bool Gfx::init(void* sdl_window)
{
    m_ = new Mac();
    m_->cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    // The Windows launcher's text formats (gfx.cpp): weight, condensed, size in DIPs = points.
    struct Def { int w; bool cond; float size; };
    const Def d[F_COUNT] = {
        { 1, true, 15 },     // brand
        { 2, true, 44 },     // title
        { 0, true, 20 },     // subtitle
        { 0, false, 16 },    // body
        { 1, true, 13.5f },  // label
        { 1, true, 22 },     // button
        { 0, true, 15 },     // button sub
        { 0, true, 15 },     // hint
        { 2, true, 30 },     // big
    };
    for (int i = 0; i < F_COUNT; i++) {
        m_->font[i] = make_font(d[i].size, d[i].w, d[i].cond);
        m_->ascent[i] = CTFontGetAscent(m_->font[i]);
        m_->line_h[i] = CTFontGetAscent(m_->font[i]) + CTFontGetDescent(m_->font[i]) + CTFontGetLeading(m_->font[i]);
    }
    if (!sdl_window) { set_offscreen(1280, 800, 2); return true; }
    m_->win = (SDL_Window*)sdl_window;
    m_->ren = SDL_CreateRenderer(m_->win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!m_->ren) m_->ren = SDL_CreateRenderer(m_->win, -1, 0);
    if (!m_->ren) return false;
    resize();
    return m_->win_ctx != nullptr;
}

void Gfx::set_offscreen(float w, float h, float scale)
{
    w_ = w; h_ = h; m_->scale = scale; dpi_ = 96.0f * scale;
}

void Gfx::shutdown()
{
    if (!m_) return;
    forget_images();
    m_->laid.clear();
    for (auto& f : m_->font) if (f) CFRelease(f);
    if (m_->win_ctx) CGContextRelease(m_->win_ctx);
    if (m_->tex) SDL_DestroyTexture(m_->tex);
    if (m_->ren) SDL_DestroyRenderer(m_->ren);
    if (m_->cs) CGColorSpaceRelease(m_->cs);
    delete m_; m_ = nullptr;
}

void Gfx::resize()
{
    if (!m_ || !m_->win) return;
    int ww = 1, wh = 1, pw = 1, ph = 1;
    SDL_GetWindowSize(m_->win, &ww, &wh);
    SDL_GetRendererOutputSize(m_->ren, &pw, &ph);
    ww = std::max(1, ww); wh = std::max(1, wh); pw = std::max(1, pw); ph = std::max(1, ph);
    // Points as DIPs; a Retina display (or a move to one) is the scale, as WM_DPICHANGED.
    w_ = (float)ww; h_ = (float)wh;
    m_->scale = (float)pw / (float)ww;
    dpi_ = 96.0f * m_->scale;
    if (m_->win_ctx && pw == m_->pw && ph == m_->ph) return;
    if (m_->win_ctx) CGContextRelease(m_->win_ctx);
    m_->pw = pw; m_->ph = ph;
    m_->win_ctx = m_->make_ctx(pw, ph);
    if (m_->tex) SDL_DestroyTexture(m_->tex);
    m_->tex = SDL_CreateTexture(m_->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, pw, ph);
    m_->tex_w = pw; m_->tex_h = ph;
}

void Gfx::set_dpi(float d) { dpi_ = d; resize(); }

bool Gfx::begin()
{
    if (!m_ || !m_->win_ctx) return false;
    m_->ctx = m_->win_ctx;
    CGContextSaveGState(m_->ctx);
    return true;
}

void Gfx::end()
{
    if (!m_ || !m_->ctx) return;
    CGContextRestoreGState(m_->ctx);
    CGContextFlush(m_->ctx);
    if (m_->tex) {
        SDL_UpdateTexture(m_->tex, nullptr, CGBitmapContextGetData(m_->ctx), (int)CGBitmapContextGetBytesPerRow(m_->ctx));
        SDL_RenderClear(m_->ren);
        SDL_RenderCopy(m_->ren, m_->tex, nullptr, nullptr);
        SDL_RenderPresent(m_->ren);
    }
    m_->ctx = nullptr;
}

static void set_fill(CGContextRef c, uint32_t argb) { CGFloat k[4]; comps(argb, k); CGContextSetRGBFillColor(c, k[0], k[1], k[2], k[3]); }
static void set_stroke(CGContextRef c, uint32_t argb) { CGFloat k[4]; comps(argb, k); CGContextSetRGBStrokeColor(c, k[0], k[1], k[2], k[3]); }

void Gfx::clear(uint32_t c)
{
    CGContextRef x = m_->ctx; if (!x) return;
    CGContextSaveGState(x);
    CGContextSetBlendMode(x, kCGBlendModeCopy);
    set_fill(x, c);
    CGContextFillRect(x, CGRectMake(0, 0, w_, h_));
    CGContextRestoreGState(x);
}

void Gfx::fill(const Rect& r, uint32_t c)
{
    CGContextRef x = m_->ctx; if (!x) return;
    set_fill(x, c);
    CGContextFillRect(x, CGRectMake(r.l, r.t, r.w(), r.h()));
}

static void gradient(CGContextRef x, CGColorSpaceRef cs, const Rect& r, uint32_t a, uint32_t b, CGPoint p0, CGPoint p1)
{
    if (!x) return;
    CGFloat k[8]; comps(a, k); comps(b, k + 4);
    const CGFloat at[2] = { 0, 1 };
    CGGradientRef g = CGGradientCreateWithColorComponents(cs, k, at, 2);
    CGContextSaveGState(x);
    CGContextClipToRect(x, CGRectMake(r.l, r.t, r.w(), r.h()));
    CGContextDrawLinearGradient(x, g, p0, p1, kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
    CGContextRestoreGState(x);
    CGGradientRelease(g);
}

void Gfx::vgradient(const Rect& r, uint32_t top, uint32_t bottom) { gradient(m_->ctx, m_->cs, r, top, bottom, CGPointMake(0, r.t), CGPointMake(0, r.b)); }
void Gfx::hgradient(const Rect& r, uint32_t left, uint32_t right) { gradient(m_->ctx, m_->cs, r, left, right, CGPointMake(r.l, 0), CGPointMake(r.r, 0)); }

static void chamfer_path(CGContextRef x, const Rect& r, float cut)
{
    cut = std::min(cut, std::min(r.w(), r.h()) * 0.5f);
    CGContextBeginPath(x);
    CGContextMoveToPoint(x, r.l + cut, r.t);
    CGContextAddLineToPoint(x, r.r, r.t);
    CGContextAddLineToPoint(x, r.r, r.b - cut);
    CGContextAddLineToPoint(x, r.r - cut, r.b);
    CGContextAddLineToPoint(x, r.l, r.b);
    CGContextAddLineToPoint(x, r.l, r.t + cut);
    CGContextClosePath(x);
}

void Gfx::chamfer_fill(const Rect& r, float cut, uint32_t c)
{
    CGContextRef x = m_->ctx; if (!x) return;
    chamfer_path(x, r, cut); set_fill(x, c); CGContextFillPath(x);
}

void Gfx::chamfer_stroke(const Rect& r, float cut, uint32_t c, float width)
{
    CGContextRef x = m_->ctx; if (!x) return;
    chamfer_path(x, r, cut); set_stroke(x, c); CGContextSetLineWidth(x, width); CGContextStrokePath(x);
}

void Gfx::chamfer_glow(const Rect& r, float cut, uint32_t c, float width)
{
    CGContextRef x = m_->ctx; if (!x) return;
    const float ws[3] = { width + 3, width + 7, width + 12 }, as[3] = { 0.40f, 0.18f, 0.07f };
    for (int i = 0; i < 3; i++) {
        chamfer_path(x, r, cut); set_stroke(x, with_alpha(c, as[i])); CGContextSetLineWidth(x, ws[i]); CGContextStrokePath(x);
    }
}

void Gfx::line(float x0, float y0, float x1, float y1, uint32_t c, float w)
{
    CGContextRef x = m_->ctx; if (!x) return;
    CGContextBeginPath(x);
    CGContextMoveToPoint(x, x0, y0);
    CGContextAddLineToPoint(x, x1, y1);
    set_stroke(x, c); CGContextSetLineWidth(x, w); CGContextStrokePath(x);
}

void Gfx::hexagon(float cx, float cy, float r, uint32_t fillc, uint32_t stroke, float sw)
{
    CGContextRef x = m_->ctx; if (!x) return;
    CGPoint p[6];
    for (int i = 0; i < 6; i++) { const double a = (60.0 * i - 90.0) * 3.14159265 / 180.0; p[i] = CGPointMake(cx + r * cos(a), cy + r * sin(a)); }
    auto path = [&] { CGContextBeginPath(x); CGContextAddLines(x, p, 6); CGContextClosePath(x); };
    if (fillc >> 24) { path(); set_fill(x, fillc); CGContextFillPath(x); }
    if (stroke >> 24) { path(); set_stroke(x, stroke); CGContextSetLineWidth(x, sw); CGContextStrokePath(x); }
}

void Gfx::circle(float cx, float cy, float r, uint32_t stroke, float sw)
{
    CGContextRef x = m_->ctx; if (!x) return;
    set_stroke(x, stroke); CGContextSetLineWidth(x, sw);
    CGContextStrokeEllipseInRect(x, CGRectMake(cx - r, cy - r, 2 * r, 2 * r));
}

// ---- text -------------------------------------------------------------------------------------
// The lines of s: wrapped at `width` (words, and inside a word too long for a line, as
// DirectWrite's WRAP), or one per paragraph cut with "..." at `width` (its character trimming).
// width <= 0: no cut. Hard line breaks always break.
// The screens' text is written for Windows (data\settings.txt, RaCACiT.exe): ui_text says it
// the Mac way (data/settings.txt, RaCACiT) as it is laid out, so app.cpp keeps one wording.
static Laid* lay(Gfx::Mac* m, const std::string& text, Font f, float width, bool wrap)
{
    char k[64]; snprintf(k, sizeof k, "%d|%d|%.2f|", (int)f, wrap ? 1 : 0, width);
    std::string key = k + text;
    auto it = m->laid.find(key);
    if (it != m->laid.end()) return it->second.get();
    const std::string s = ui_text(text);
    if (m->laid.size() > 2048) m->laid.clear();
    auto out = std::make_unique<Laid>();
    CFStringRef str = CFStringCreateWithBytes(nullptr, (const UInt8*)s.data(), (CFIndex)s.size(), kCFStringEncodingUTF8, false);
    if (!str) str = CFStringCreateWithBytes(nullptr, (const UInt8*)s.data(), (CFIndex)s.size(), kCFStringEncodingWindowsLatin1, false);
    if (str) {
        const void* keys[2] = { kCTFontAttributeName, kCTForegroundColorFromContextAttributeName };
        const void* vals[2] = { m->font[f], kCFBooleanTrue };
        CFDictionaryRef attrs = CFDictionaryCreate(nullptr, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFAttributedStringRef as = CFAttributedStringCreate(nullptr, str, attrs);
        const CFIndex n = CFStringGetLength(str);
        auto add = [&](CTLineRef l) {
            double asc, desc, lead;
            const double w = CTLineGetTypographicBounds(l, &asc, &desc, &lead);
            out->max_width = std::max(out->max_width, w);
            out->widths.push_back(w - CTLineGetTrailingWhitespaceWidth(l));
            out->lines.push_back(l);
        };
        if (wrap) {
            CTTypesetterRef ts = CTTypesetterCreateWithAttributedString(as);
            for (CFIndex at = 0; at < n;) {
                CFIndex cnt = CTTypesetterSuggestLineBreak(ts, at, std::max(1.0f, width));
                if (cnt <= 0) cnt = 1;
                add(CTTypesetterCreateLine(ts, CFRangeMake(at, cnt)));
                at += cnt;
            }
            CFRelease(ts);
        } else {
            CFAttributedStringRef dots = nullptr;
            for (CFIndex at = 0; at <= n;) {
                CFRange nl = CFStringFind(str, CFSTR("\n"), 0);
                CFIndex end = n;
                if (at < n) {
                    const CFRange rest = CFRangeMake(at, n - at);
                    if (CFStringFindWithOptions(str, CFSTR("\n"), rest, 0, &nl)) end = nl.location;
                }
                CFAttributedStringRef part = CFAttributedStringCreateWithSubstring(nullptr, as, CFRangeMake(at, end - at));
                CTLineRef l = CTLineCreateWithAttributedString(part);
                CFRelease(part);
                if (width > 0 && CTLineGetTypographicBounds(l, nullptr, nullptr, nullptr) - CTLineGetTrailingWhitespaceWidth(l) > width + 0.01) {
                    if (!dots) dots = CFAttributedStringCreate(nullptr, CFSTR("…"), attrs);
                    CTLineRef tok = CTLineCreateWithAttributedString(dots);
                    CTLineRef cut = CTLineCreateTruncatedLine(l, width, kCTLineTruncationEnd, tok);
                    CFRelease(tok);
                    if (cut) { CFRelease(l); l = cut; }
                }
                add(l);
                if (end >= n) break;
                at = end + 1;
            }
            if (dots) CFRelease(dots);
        }
        CFRelease(as); CFRelease(attrs); CFRelease(str);
    }
    Laid* r = out.get();
    m->laid.emplace(std::move(key), std::move(out));
    return r;
}

void Gfx::text(const std::string& s, const Rect& r, Font f, uint32_t c, Align a, bool wrap, bool vcenter)
{
    CGContextRef x = m_->ctx;
    if (s.empty() || !x) return;
    const Laid* l = lay(m_, s, f, r.w(), wrap);
    const double lh = m_->line_h[f];
    const double total = lh * (double)l->lines.size();
    double y = vcenter ? r.t + (r.h() - total) / 2 : r.t;
    CGContextSaveGState(x);
    CGContextClipToRect(x, CGRectMake(r.l, r.t, r.w(), r.h()));   // D2D1_DRAW_TEXT_OPTIONS_CLIP
    CGContextSetTextMatrix(x, CGAffineTransformMakeScale(1, -1));
    set_fill(x, c);
    for (size_t i = 0; i < l->lines.size(); i++, y += lh) {
        const double w = l->widths[i];
        const double lx = a == A_CENTER ? r.l + (r.w() - w) / 2 : a == A_RIGHT ? r.r - w : r.l;
        CGContextSetTextPosition(x, lx, y + m_->ascent[f]);
        CTLineDraw(l->lines[i], x);
    }
    CGContextRestoreGState(x);
}

float Gfx::text_width(const std::string& s, Font f)
{
    if (s.empty()) return 0;
    return (float)lay(m_, s, f, 0, false)->max_width;
}

float Gfx::text_height(const std::string& s, Font f, float width)
{
    if (s.empty()) return 0;
    return (float)(m_->line_h[f] * (double)lay(m_, s, f, std::max(1.0f, width), true)->lines.size());
}

void Gfx::clip_push(const Rect& r)
{
    CGContextRef x = m_->ctx; if (!x) return;
    CGContextSaveGState(x);
    CGContextClipToRect(x, CGRectMake(r.l, r.t, r.w(), r.h()));
}

void Gfx::clip_pop() { if (m_->ctx) CGContextRestoreGState(m_->ctx); }

// ---- images -----------------------------------------------------------------------------------
void Gfx::forget_images()
{
    if (!m_) return;
    for (auto& i : m_->imgs) {
        if (i.img) CGImageRelease(i.img);
        if (i.scaled) CGImageRelease(i.scaled);
        i = Mac::Img();
    }
}

static Gfx::Mac::Img* load_img(Gfx::Mac* m, const fs::path& p)
{
    if (p.empty()) return nullptr;
    for (auto& i : m->imgs) if (i.img && i.path == p) return &i;
    const uint64_t now = now_ms();
    for (auto& x : m->miss) if (x.path == p && now < x.retry) return nullptr;
    auto missing = [&]() -> Gfx::Mac::Img* {
        Gfx::Mac::Miss* slot = &m->miss[0];
        for (auto& x : m->miss) { if (x.path == p) { slot = &x; break; } if (x.retry < slot->retry) slot = &x; }
        slot->path = p; slot->retry = now + 1000;
        return nullptr;
    };
    std::error_code ec;
    if (!fs::exists(p, ec)) return missing();
    const std::string s = u8(p);
    CFURLRef u = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)s.data(), (CFIndex)s.size(), false);
    CGImageSourceRef src = u ? CGImageSourceCreateWithURL(u, nullptr) : nullptr;
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, nullptr) : nullptr;
    if (src) CFRelease(src);
    if (u) CFRelease(u);
    if (!img) return missing();
    Gfx::Mac::Img* slot = &m->imgs[0];
    for (auto& i : m->imgs) if (!i.img) { slot = &i; break; }
    if (slot->img) CGImageRelease(slot->img);
    if (slot->scaled) CGImageRelease(slot->scaled);
    *slot = Gfx::Mac::Img();
    slot->img = img; slot->path = p;
    slot->w = (float)CGImageGetWidth(img); slot->h = (float)CGImageGetHeight(img);
    return slot;
}

bool Gfx::image_size(const fs::path& p, float* w, float* h)
{
    Mac::Img* i = load_img(m_, p);
    if (!i) return false;
    *w = i->w; *h = i->h;
    return true;
}

bool Gfx::image(const fs::path& p, const Rect& r, float opacity, bool cover)
{
    CGContextRef x = m_->ctx;
    Mac::Img* i = load_img(m_, p);
    if (!i || !x) return i != nullptr;
    CGRect src = CGRectMake(0, 0, i->w, i->h);
    if (cover) {   // fill r, cropping the image's excess
        const float ia = i->w / i->h, ra = r.w() / r.h();
        if (ia > ra) { const float cw = i->h * ra; src.origin.x = (i->w - cw) / 2; src.size.width = cw; }
        else { const float ch = i->w / ra; src.origin.y = (i->h - ch) / 2; src.size.height = ch; }
    }
    // Scaled once to the pixels it covers (the full-window key art every frame otherwise
    // costs a resample each time), then drawn 1:1.
    const int sw = std::max(1, (int)lround(r.w() * m_->scale)), sh = std::max(1, (int)lround(r.h() * m_->scale));
    if (!i->scaled || i->sw != sw || i->sh != sh || !CGRectEqualToRect(i->src, src)) {
        if (i->scaled) { CGImageRelease(i->scaled); i->scaled = nullptr; }
        CGImageRef part = CGImageCreateWithImageInRect(i->img, CGRectIntegral(src));
        CGContextRef c = CGBitmapContextCreate(nullptr, (size_t)sw, (size_t)sh, 8, (size_t)sw * 4, m_->cs,
                                               kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
        if (part && c) {
            CGContextSetInterpolationQuality(c, kCGInterpolationHigh);
            CGContextDrawImage(c, CGRectMake(0, 0, sw, sh), part);
            i->scaled = CGBitmapContextCreateImage(c);
        }
        if (c) CGContextRelease(c);
        if (part) CGImageRelease(part);
        i->sw = sw; i->sh = sh; i->src = src;
        if (!i->scaled) return false;
    }
    CGContextSaveGState(x);
    CGContextSetAlpha(x, opacity);
    CGContextTranslateCTM(x, r.l, r.b);   // images draw bottom-up: flip back for this one
    CGContextScaleCTM(x, 1, -1);
    CGContextDrawImage(x, CGRectMake(0, 0, r.w(), r.h()), i->scaled);
    CGContextRestoreGState(x);
    return true;
}

// ---- snapshot ---------------------------------------------------------------------------------
bool Gfx::snapshot(const std::wstring& png, void (*draw)())
{
    if (!m_) return false;
    const int pw = (int)(w_ * dpi_ / 96.0f + 0.5f), ph = (int)(h_ * dpi_ / 96.0f + 0.5f);
    CGContextRef c = m_->make_ctx(std::max(1, pw), std::max(1, ph));
    if (!c) return false;
    CGContextRef save = m_->ctx;
    m_->ctx = c;
    CGContextSaveGState(c);
    draw();
    CGContextRestoreGState(c);
    m_->ctx = save;
    bool ok = false;
    if (CGImageRef img = CGBitmapContextCreateImage(c)) {
        const std::string path = narrow(png);
        CFURLRef u = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.data(), (CFIndex)path.size(), false);
        CGImageDestinationRef d = u ? CGImageDestinationCreateWithURL(u, CFSTR("public.png"), 1, nullptr) : nullptr;
        if (d) { CGImageDestinationAddImage(d, img, nullptr); ok = CGImageDestinationFinalize(d); CFRelease(d); }
        if (u) CFRelease(u);
        CGImageRelease(img);
    }
    CGContextRelease(c);
    return ok;
}
