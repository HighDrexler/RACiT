#include "gfx.h"

#include <algorithm>
#include <cmath>

template <class T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

D2D1_COLOR_F d2c(uint32_t c)
{
    return D2D1::ColorF(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, ((c >> 24) & 255) / 255.0f);
}

static uint32_t with_alpha(uint32_t c, float a)
{
    const float base = ((c >> 24) & 255) / 255.0f;
    const uint32_t na = (uint32_t)(255.0f * base * a + 0.5f);
    return (na << 24) | (c & 0xFFFFFF);
}

bool Gfx::init(HWND hwnd)
{
    hwnd_ = hwnd;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &f_))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw_))) return false;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));

    // Bahnschrift (Windows 10+) has the condensed HUD look; Segoe UI otherwise.
    const wchar_t* fam = L"Bahnschrift";
    {
        IDWriteFontCollection* fc = nullptr; UINT32 idx = 0; BOOL ok = FALSE;
        if (SUCCEEDED(dw_->GetSystemFontCollection(&fc)) && fc) { fc->FindFamilyName(fam, &idx, &ok); fc->Release(); }
        if (!ok) fam = L"Segoe UI";
    }
    struct Def { DWRITE_FONT_WEIGHT w; DWRITE_FONT_STRETCH s; float size; };
    const Def d[F_COUNT] = {
        { DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STRETCH_CONDENSED, 15 },   // brand
        { DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STRETCH_CONDENSED, 44 },        // title
        { DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_CONDENSED, 20 },      // subtitle
        { DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16 },         // body
        { DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STRETCH_CONDENSED, 13.5f },// label
        { DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STRETCH_CONDENSED, 22 },   // button
        { DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_CONDENSED, 15 },      // button sub
        { DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_CONDENSED, 15 },      // hint
        { DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STRETCH_CONDENSED, 30 },        // big
    };
    for (int i = 0; i < F_COUNT; i++)
        if (FAILED(dw_->CreateTextFormat(fam, nullptr, d[i].w, DWRITE_FONT_STYLE_NORMAL, d[i].s, d[i].size, L"en-us", &fmt_[i])))
            return false;
    for (int i = 0; i < F_COUNT; i++) dw_->CreateEllipsisTrimmingSign(fmt_[i], &ell_[i]);
    dpi_ = (float)GetDpiForWindow(hwnd);
    return make_target();
}

void Gfx::shutdown()
{
    drop_target();
    for (auto& e : ell_) rel(e);
    for (auto& f : fmt_) rel(f);
    rel(wic_); rel(dw_); rel(f_);
}

bool Gfx::make_target()
{
    if (hrt_) return true;
    RECT rc; GetClientRect(hwnd_, &rc);
    const D2D1_SIZE_U sz = D2D1::SizeU((UINT32)std::max<LONG>(1, rc.right), (UINT32)std::max<LONG>(1, rc.bottom));
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi_, dpi_);
    if (FAILED(f_->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, sz), &hrt_))) return false;
    rt_ = hrt_;
    rt_->SetDpi(dpi_, dpi_);
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt_->CreateSolidColorBrush(d2c(0xFFFFFFFF), &br_);
    const D2D1_SIZE_F s = rt_->GetSize();
    w_ = s.width; h_ = s.height;
    return true;
}

void Gfx::drop_target()
{
    forget_images();
    rel(br_);
    rel(hrt_);
    rt_ = nullptr;
}

void Gfx::forget_images() { for (auto& i : imgs_) { rel(i.bmp); i.path.clear(); } }

void Gfx::set_dpi(float d)
{
    dpi_ = d;
    if (hrt_) { hrt_->SetDpi(d, d); const D2D1_SIZE_F s = hrt_->GetSize(); w_ = s.width; h_ = s.height; }
}

void Gfx::resize()
{
    if (!hrt_) return;
    RECT rc; GetClientRect(hwnd_, &rc);
    hrt_->Resize(D2D1::SizeU((UINT32)std::max<LONG>(1, rc.right), (UINT32)std::max<LONG>(1, rc.bottom)));
    const D2D1_SIZE_F s = hrt_->GetSize();
    w_ = s.width; h_ = s.height;
}

bool Gfx::begin()
{
    if (!make_target()) return false;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

void Gfx::end()
{
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET) drop_target();
}

ID2D1SolidColorBrush* Gfx::brush(uint32_t c) { br_->SetColor(d2c(c)); return br_; }

void Gfx::clear(uint32_t c) { rt_->Clear(d2c(c)); }
void Gfx::fill(const Rect& r, uint32_t c) { rt_->FillRectangle(D2D1::RectF(r.l, r.t, r.r, r.b), brush(c)); }

void Gfx::vgradient(const Rect& r, uint32_t top, uint32_t bottom)
{
    D2D1_GRADIENT_STOP st[2] = { { 0.0f, d2c(top) }, { 1.0f, d2c(bottom) } };
    ID2D1GradientStopCollection* sc = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
    if (SUCCEEDED(rt_->CreateGradientStopCollection(st, 2, &sc)) &&
        SUCCEEDED(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, r.t), D2D1::Point2F(0, r.b)), sc, &lb)))
        rt_->FillRectangle(D2D1::RectF(r.l, r.t, r.r, r.b), lb);
    rel(lb); rel(sc);
}

void Gfx::hgradient(const Rect& r, uint32_t left, uint32_t right)
{
    D2D1_GRADIENT_STOP st[2] = { { 0.0f, d2c(left) }, { 1.0f, d2c(right) } };
    ID2D1GradientStopCollection* sc = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
    if (SUCCEEDED(rt_->CreateGradientStopCollection(st, 2, &sc)) &&
        SUCCEEDED(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.l, 0), D2D1::Point2F(r.r, 0)), sc, &lb)))
        rt_->FillRectangle(D2D1::RectF(r.l, r.t, r.r, r.b), lb);
    rel(lb); rel(sc);
}

ID2D1PathGeometry* Gfx::chamfer_geo(const Rect& r, float cut)
{
    ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* s = nullptr;
    if (FAILED(f_->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) { rel(g); return nullptr; }
    cut = std::min(cut, std::min(r.w(), r.h()) * 0.5f);
    s->BeginFigure(D2D1::Point2F(r.l + cut, r.t), D2D1_FIGURE_BEGIN_FILLED);
    const D2D1_POINT_2F pts[5] = { { r.r, r.t }, { r.r, r.b - cut }, { r.r - cut, r.b }, { r.l, r.b }, { r.l, r.t + cut } };
    s->AddLines(pts, 5);
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close(); s->Release();
    return g;
}

void Gfx::chamfer_fill(const Rect& r, float cut, uint32_t c)
{
    if (ID2D1PathGeometry* g = chamfer_geo(r, cut)) { rt_->FillGeometry(g, brush(c)); g->Release(); }
}

void Gfx::chamfer_stroke(const Rect& r, float cut, uint32_t c, float width)
{
    if (ID2D1PathGeometry* g = chamfer_geo(r, cut)) { rt_->DrawGeometry(g, brush(c), width); g->Release(); }
}

void Gfx::chamfer_glow(const Rect& r, float cut, uint32_t c, float width)
{
    ID2D1PathGeometry* g = chamfer_geo(r, cut);
    if (!g) return;
    const float ws[3] = { width + 3, width + 7, width + 12 }, as[3] = { 0.40f, 0.18f, 0.07f };
    for (int i = 0; i < 3; i++) rt_->DrawGeometry(g, brush(with_alpha(c, as[i])), ws[i]);
    g->Release();
}

void Gfx::line(float x0, float y0, float x1, float y1, uint32_t c, float w)
{
    rt_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush(c), w);
}

void Gfx::hexagon(float cx, float cy, float r, uint32_t fillc, uint32_t stroke, float sw)
{
    ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* s = nullptr;
    if (FAILED(f_->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) { rel(g); return; }
    D2D1_POINT_2F p[6];
    for (int i = 0; i < 6; i++) { const double a = (60.0 * i - 90.0) * 3.14159265 / 180.0; p[i] = D2D1::Point2F(cx + (float)(r * cos(a)), cy + (float)(r * sin(a))); }
    s->BeginFigure(p[0], D2D1_FIGURE_BEGIN_FILLED); s->AddLines(p + 1, 5); s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close(); s->Release();
    if (fillc >> 24) rt_->FillGeometry(g, brush(fillc));
    if (stroke >> 24) rt_->DrawGeometry(g, brush(stroke), sw);
    g->Release();
}

void Gfx::circle(float cx, float cy, float r, uint32_t stroke, float sw)
{
    rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush(stroke), sw);
}

void Gfx::text(const std::string& s, const Rect& r, Font f, uint32_t c, Align a, bool wrap, bool vcenter)
{
    if (s.empty()) return;
    IDWriteTextFormat* fm = fmt_[f];
    fm->SetTextAlignment(a == A_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER : a == A_RIGHT ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    fm->SetParagraphAlignment(vcenter ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    fm->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    IDWriteInlineObject* ell = wrap ? nullptr : ell_[f];
    DWRITE_TRIMMING tr = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    if (ell) fm->SetTrimming(&tr, ell);
    const std::wstring w = widen(s);
    rt_->DrawText(w.c_str(), (UINT32)w.size(), fm, D2D1::RectF(r.l, r.t, r.r, r.b), brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    if (ell) { DWRITE_TRIMMING none = { DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0 }; fm->SetTrimming(&none, nullptr); }
}

float Gfx::text_width(const std::string& s, Font f)
{
    const std::wstring w = widen(s);
    IDWriteTextLayout* l = nullptr;
    fmt_[f]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if (FAILED(dw_->CreateTextLayout(w.c_str(), (UINT32)w.size(), fmt_[f], 10000, 1000, &l))) return 0;
    DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); l->Release();
    return m.widthIncludingTrailingWhitespace;
}

float Gfx::text_height(const std::string& s, Font f, float width)
{
    if (s.empty()) return 0;
    const std::wstring w = widen(s);
    IDWriteTextLayout* l = nullptr;
    fmt_[f]->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    if (FAILED(dw_->CreateTextLayout(w.c_str(), (UINT32)w.size(), fmt_[f], std::max(1.0f, width), 10000, &l))) return 0;
    DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); l->Release();
    return m.height;
}

void Gfx::clip_push(const Rect& r) { rt_->PushAxisAlignedClip(D2D1::RectF(r.l, r.t, r.r, r.b), D2D1_ANTIALIAS_MODE_ALIASED); }
void Gfx::clip_pop() { rt_->PopAxisAlignedClip(); }

Gfx::Img* Gfx::load(const fs::path& p)
{
    if (p.empty() || !wic_ || !rt_) return nullptr;
    for (auto& i : imgs_) if (i.bmp && i.path == p) return &i;
    const ULONGLONG now = GetTickCount64();
    for (auto& m : miss_) if (m.path == p && now < m.retry) return nullptr;
    auto missing = [&]() -> Img* {
        Miss* slot = &miss_[0];
        for (auto& m : miss_) { if (m.path == p) { slot = &m; break; } if (m.retry < slot->retry) slot = &m; }
        slot->path = p; slot->retry = now + 1000;
        return nullptr;
    };
    std::error_code ec;
    if (!fs::exists(p, ec)) return missing();
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    ID2D1Bitmap* bmp = nullptr;
    if (SUCCEEDED(wic_->CreateDecoderFromFilename(p.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(wic_->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
        rt_->CreateBitmapFromWicBitmap(cv, nullptr, &bmp);
    rel(cv); rel(fr); rel(dec);
    if (!bmp) return missing();
    Img* slot = &imgs_[0];
    for (auto& i : imgs_) if (!i.bmp) { slot = &i; break; }
    rel(slot->bmp);
    slot->bmp = bmp; slot->path = p;
    const D2D1_SIZE_U s = bmp->GetPixelSize(); slot->w = (float)s.width; slot->h = (float)s.height;
    return slot;
}

bool Gfx::image_size(const fs::path& p, float* w, float* h)
{
    Img* i = load(p);
    if (!i) return false;
    *w = i->w; *h = i->h;
    return true;
}

bool Gfx::image(const fs::path& p, const Rect& r, float opacity, bool cover)
{
    Img* i = load(p);
    if (!i) return false;
    D2D1_RECT_F src = D2D1::RectF(0, 0, i->w, i->h);
    if (cover) {   // fill r, cropping the image's excess
        const float ia = i->w / i->h, ra = r.w() / r.h();
        if (ia > ra) { const float cw = i->h * ra; src.left = (i->w - cw) / 2; src.right = src.left + cw; }
        else { const float ch = i->w / ra; src.top = (i->h - ch) / 2; src.bottom = src.top + ch; }
    }
    rt_->DrawBitmap(i->bmp, D2D1::RectF(r.l, r.t, r.r, r.b), opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
    return true;
}

bool Gfx::snapshot(const std::wstring& png, void (*draw)())
{
    if (!wic_ || !make_target()) return false;
    const UINT pw = (UINT)(w_ * dpi_ / 96.0f + 0.5f), ph = (UINT)(h_ * dpi_ / 96.0f + 0.5f);
    IWICBitmap* wb = nullptr; ID2D1RenderTarget* wrt = nullptr; ID2D1SolidColorBrush* wbr = nullptr;
    if (FAILED(wic_->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wb))) return false;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi_, dpi_);
    bool ok = SUCCEEDED(f_->CreateWicBitmapRenderTarget(wb, props, &wrt)) && SUCCEEDED(wrt->CreateSolidColorBrush(d2c(0xFFFFFFFF), &wbr));
    if (ok) {
        // Bitmaps belong to one target: reload them for this one, and again after.
        forget_images();
        ID2D1RenderTarget* save_rt = rt_; ID2D1SolidColorBrush* save_br = br_;
        rt_ = wrt; br_ = wbr;
        wrt->BeginDraw();
        draw();
        ok = SUCCEEDED(wrt->EndDraw());
        forget_images();
        rt_ = save_rt; br_ = save_br;
    }
    IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr;
    if (ok && SUCCEEDED(wic_->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(png.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr))) {
        fr->WriteSource(wb, nullptr); fr->Commit(); enc->Commit();
    } else ok = false;
    rel(fr); rel(enc); rel(st); rel(wbr); rel(wrt); rel(wb);
    return ok;
}
