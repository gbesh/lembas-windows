// Lembas Desktop - the Lembas scene, gently animated, behind your desktop icons, with the
// time, the Shire date, the Quenya hour and the current hobbit mealtime.
//
// One small native program: Win32 + Direct3D 11 + Direct2D + DirectWrite, nothing else.
// Battery: the still parts of each scene are pre-rendered images; only the small moving
// pieces are redrawn, and only the parts of the screen they touch are presented (flip-model
// dirty rectangles). It draws 24 frames a second on mains power, 12 on battery, and stops
// completely when the desktop is covered, the screen is off or locked, Battery saver is on,
// or Windows' "Animation effects" setting is off (the clock still turns over each minute).
//
// It follows whichever Lembas wallpaper Lembas Sun has set, so the four looks switch with it.
// If the wallpaper isn't a Lembas one, it steps aside.
//
//   LembasDesktop.exe          start (does nothing if it is already running)
//   LembasDesktop.exe --quit   stop the running copy

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <dwrite_1.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <wtsapi32.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cwchar>
#include <string>
#include <vector>
#include <algorithm>

using Microsoft::WRL::ComPtr;
using namespace D2D1;

// ------------------------------------------------------------------------------------ helpers
static const float PI = 3.14159265358979f;
static inline float frand10(double n) { return (float)(std::floor(n * 10 + 0.5) / 10); }  // the scene's f()

// mulberry32, exactly as in scenes/index.html, so stars land where the designs put them
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed) {}
    double operator()() {
        s += 0x6D2B79F5u;
        uint32_t t = (s ^ (s >> 15)) * (1u | s);
        t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
        return (double)(t ^ (t >> 14)) / 4294967296.0;
    }
};

static D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.f) {
    return ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}

// CSS timing functions
static float Bezier(float x1, float y1, float x2, float y2, float x) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    float lo = 0, hi = 1, t = x;
    for (int i = 0; i < 24; i++) {
        float cx = 3 * (1 - t) * (1 - t) * t * x1 + 3 * (1 - t) * t * t * x2 + t * t * t;
        if (std::fabs(cx - x) < 1e-5f) break;
        if (cx < x) lo = t; else hi = t;
        t = (lo + hi) / 2;
    }
    return 3 * (1 - t) * (1 - t) * t * y1 + 3 * (1 - t) * t * t * y2 + t * t * t;
}
enum Ease { LINEAR, EASE_IN_OUT, EASE_OUT };
static float Apply(Ease e, float x) {
    switch (e) {
    case EASE_IN_OUT: return Bezier(.42f, 0, .58f, 1, x);
    case EASE_OUT: return Bezier(0, 0, .58f, 1, x);
    default: return x;
    }
}
// One animated property: keyframe offsets and values, with the timing function per segment.
struct Track {
    std::vector<float> at, v;
    Ease ease;
    float operator()(float p) const {
        if (p <= at.front()) return v.front();
        for (size_t i = 1; i < at.size(); i++) {
            if (p <= at[i]) {
                float k = (p - at[i - 1]) / (at[i] - at[i - 1]);
                return v[i - 1] + (v[i] - v[i - 1]) * Apply(ease, k);
            }
        }
        return v.back();
    }
};
static float Phase(double t, float dur, float offset) {  // progress 0..1 through a looping animation
    double p = std::fmod(t + offset, (double)dur);
    if (p < 0) p += dur;
    return (float)(p / dur);
}

static D2D1_RECT_F Empty() { return RectF(0, 0, 0, 0); }
static bool IsEmpty(const D2D1_RECT_F& r) { return r.right <= r.left || r.bottom <= r.top; }
static D2D1_RECT_F Union(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    if (IsEmpty(a)) return b;
    if (IsEmpty(b)) return a;
    return RectF(std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom));
}
static D2D1_RECT_F Circle(float x, float y, float r) { return RectF(x - r, y - r, x + r, y + r); }
static D2D1_RECT_F Inflate(D2D1_RECT_F r, float d) { return IsEmpty(r) ? r : RectF(r.left - d, r.top - d, r.right + d, r.bottom + d); }
static D2D1_RECT_F TransformRect(const D2D1_RECT_F& r, const D2D1_MATRIX_3X2_F& m) {
    if (IsEmpty(r)) return r;
    D2D1_POINT_2F p[4] = { {r.left, r.top}, {r.right, r.top}, {r.left, r.bottom}, {r.right, r.bottom} };
    D2D1_RECT_F o = { 1e9f, 1e9f, -1e9f, -1e9f };
    for (auto& q : p) {
        float x = q.x * m._11 + q.y * m._21 + m._31, y = q.x * m._12 + q.y * m._22 + m._32;
        o.left = std::min(o.left, x); o.top = std::min(o.top, y); o.right = std::max(o.right, x); o.bottom = std::max(o.bottom, y);
    }
    return o;
}

// ------------------------------------------------------------------------------------ globals
static ComPtr<ID2D1Factory1> gFactory;
static ComPtr<IDWriteFactory> gDWrite;
static ComPtr<IWICImagingFactory> gWic;
static ComPtr<ID3D11Device> gD3D;
static ComPtr<ID2D1Device> gD2DDevice;
static ComPtr<ID2D1DeviceContext> gCtx;
static ComPtr<IDXGISwapChain1> gSwap;
static ComPtr<ID2D1Bitmap1> gTarget;
static ComPtr<ID2D1SolidColorBrush> gBrush;
static HWND gWnd, gMsgWnd, gParent;
static int gCanvasW, gCanvasH;
static std::wstring gDir;  // folder with the exe and the scene layers
static double gT0;
static LARGE_INTEGER gFreq;

static void Log(const wchar_t* fmt, ...) {
    static std::wstring path;
    if (path.empty()) {
        wchar_t base[MAX_PATH] = {};
        GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        path = std::wstring(base) + L"\\Lembas\\desktop.log";
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) && a.nFileSizeLow > 256 * 1024) DeleteFileW(path.c_str());
    }
    wchar_t msg[1024];
    va_list ap; va_start(ap, fmt); vswprintf(msg, 1024, fmt, ap); va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    char line[1400];
    int n = std::snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d  ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    n += WideCharToMultiByte(CP_UTF8, 0, msg, -1, line + n, (int)sizeof line - n - 2, nullptr, nullptr) - 1;
    line[n++] = '\r'; line[n++] = '\n';
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, line, n, &w, nullptr); CloseHandle(h); }
}

static double Now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)gFreq.QuadPart;
}

// ------------------------------------------------------------------------------------ SVG paths
// The subset the scenes use: M L H V C S A Z (absolute and relative).
static ComPtr<ID2D1PathGeometry> Path(const char* d) {
    ComPtr<ID2D1PathGeometry> g;
    ComPtr<ID2D1GeometrySink> s;
    gFactory->CreatePathGeometry(&g);
    g->Open(&s);
    s->SetFillMode(D2D1_FILL_MODE_WINDING);
    const char* p = d;
    char cmd = 0;
    float cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0;
    bool open = false;
    auto skip = [&]() { while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') p++; };
    auto num = [&]() -> float { skip(); char* e; float v = std::strtof(p, &e); p = e; return v; };
    auto isnum = [&]() { skip(); return (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == '+'; };
    while (true) {
        skip();
        if (!*p) break;
        if (std::isalpha((unsigned char)*p)) cmd = *p++;
        else if (cmd == 'M') cmd = 'L';
        else if (cmd == 'm') cmd = 'l';
        bool rel = std::islower((unsigned char)cmd);
        char c = (char)std::toupper((unsigned char)cmd);
        float ox = rel ? cx : 0, oy = rel ? cy : 0;
        if (c == 'Z') {
            if (open) s->EndFigure(D2D1_FIGURE_END_CLOSED);
            open = false; cx = sx; cy = sy;
            continue;
        }
        if (c == 'M') {
            float x = num() + ox, y = num() + oy;
            if (open) s->EndFigure(D2D1_FIGURE_END_OPEN);
            s->BeginFigure(Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED);
            open = true; cx = sx = lcx = x; cy = sy = lcy = y;
        } else if (c == 'L') {
            float x = num() + ox, y = num() + oy;
            s->AddLine(Point2F(x, y)); cx = lcx = x; cy = lcy = y;
        } else if (c == 'H') {
            float x = num() + ox; s->AddLine(Point2F(x, cy)); cx = lcx = x; lcy = cy;
        } else if (c == 'V') {
            float y = num() + oy; s->AddLine(Point2F(cx, y)); cy = lcy = y; lcx = cx;
        } else if (c == 'C') {
            float x1 = num() + ox, y1 = num() + oy, x2 = num() + ox, y2 = num() + oy, x = num() + ox, y = num() + oy;
            s->AddBezier(BezierSegment(Point2F(x1, y1), Point2F(x2, y2), Point2F(x, y)));
            lcx = x2; lcy = y2; cx = x; cy = y;
        } else if (c == 'S') {
            float x1 = 2 * cx - lcx, y1 = 2 * cy - lcy;
            float x2 = num() + ox, y2 = num() + oy, x = num() + ox, y = num() + oy;
            s->AddBezier(BezierSegment(Point2F(x1, y1), Point2F(x2, y2), Point2F(x, y)));
            lcx = x2; lcy = y2; cx = x; cy = y;
        } else if (c == 'A') {
            float rx = num(), ry = num(), rot = num(), large = num(), sweep = num(), x = num() + ox, y = num() + oy;
            s->AddArc(ArcSegment(Point2F(x, y), SizeF(rx, ry), rot,
                                 sweep != 0 ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                 large != 0 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            cx = lcx = x; cy = lcy = y;
        } else {
            p++;  // unknown: skip
        }
        while (c != 'M' && isnum()) {  // implicit repeats
            if (c == 'L') { float x = num() + (rel ? cx : 0), y = num() + (rel ? cy : 0); s->AddLine(Point2F(x, y)); cx = lcx = x; cy = lcy = y; }
            else break;
        }
    }
    if (open) s->EndFigure(D2D1_FIGURE_END_OPEN);
    s->Close();
    return g;
}

static ComPtr<ID2D1RadialGradientBrush> Radial(std::initializer_list<D2D1_GRADIENT_STOP> stops) {
    ComPtr<ID2D1GradientStopCollection> c;
    std::vector<D2D1_GRADIENT_STOP> v(stops);
    gCtx->CreateGradientStopCollection(v.data(), (UINT32)v.size(), &c);
    ComPtr<ID2D1RadialGradientBrush> b;
    gCtx->CreateRadialGradientBrush(RadialGradientBrushProperties(Point2F(0, 0), Point2F(0, 0), 1, 1), c.Get(), &b);
    return b;
}
static void FillGlow(ID2D1RadialGradientBrush* b, float x, float y, float rx, float ry, float opacity) {
    b->SetCenter(Point2F(x, y));
    b->SetRadiusX(rx);
    b->SetRadiusY(ry);
    b->SetOpacity(opacity);
    gCtx->FillEllipse(Ellipse(Point2F(x, y), rx, ry), b);
}
static void Solid(uint32_t rgb, float a) {
    gBrush->SetColor(Hex(rgb, a));
    gBrush->SetOpacity(1);
}

// ------------------------------------------------------------------------------------ scene elements
// Everything is in the scenes' 1920x1080 artboard units. Each element knows where it is at time t
// (for dirty rectangles) and how to draw itself.
struct Element {
    bool slow = false;        // gentle motion: updated `rate` times a second instead of every frame
    float rate = 6;
    bool smooth = false;      // fast mover: updated 2.5x the frame rate so it glides instead of jumping
    double q = -1;            // the time this element was last drawn at
    bool still = false;       // never moves; drawn only when something around it changes
    int layer = -1;           // >= 0: one of the pre-rendered image layers
    D2D1_RECT_F last = { 0, 0, 0, 0 };
    D2D1_RECT_F now = { 0, 0, 0, 0 };   // this frame's bounds (worked out once per frame)
    virtual ~Element() {}
    virtual D2D1_RECT_F Bounds(double t) = 0;
    virtual void Draw(double t, const D2D1_MATRIX_3X2_F& art) = 0;
};

struct LayerEl : Element {
    explicit LayerEl(int i) { layer = i; still = true; }
    D2D1_RECT_F Bounds(double) override { return RectF(0, 0, 1920, 1080); }
    void Draw(double, const D2D1_MATRIX_3X2_F&) override {}  // drawn by the renderer (per monitor bitmaps)
};

// --- Starlit Night
struct Twinkle : Element {
    float x, y, r, dur, off; uint32_t col;
    Track op{ {0, .45f, .6f, 1}, {.12f, 1, .85f, .12f}, EASE_IN_OUT };
    Track sc{ {0, .45f, .6f, 1}, {.6f, 1.3f, 1, .6f}, EASE_IN_OUT };
    D2D1_RECT_F Bounds(double) override { return Circle(x, y, r * 1.3f + 1); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float p = Phase(t, dur, off);
        gCtx->SetTransform(art);
        Solid(col, op(p));
        gCtx->FillEllipse(Ellipse(Point2F(x, y), r * sc(p), r * sc(p)), gBrush.Get());
    }
};
struct Spark : Element {
    float x, y, dur, off;
    static ComPtr<ID2D1PathGeometry> shape;
    static ComPtr<ID2D1RadialGradientBrush> glow;
    Track sc{ {0, .5f, 1}, {.35f, 1, .35f}, EASE_IN_OUT };
    Track rot{ {0, .5f, 1}, {0, 20, 0}, EASE_IN_OUT };
    Track op{ {0, .5f, 1}, {.2f, 1, .2f}, EASE_IN_OUT };
    D2D1_RECT_F Bounds(double) override { return Circle(x, y, 17); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float p = Phase(t, dur, off), s = sc(p), o = op(p);
        gCtx->SetTransform(art);
        FillGlow(glow.Get(), x, y, 9 * s + 7, 9 * s + 7, o * .55f);
        gCtx->SetTransform(Matrix3x2F::Scale(s, s) * Matrix3x2F::Rotation(rot(p)) * Matrix3x2F::Translation(x, y) * art);
        Solid(0xFFF8E6, o);
        gCtx->FillGeometry(shape.Get(), gBrush.Get());
    }
};
ComPtr<ID2D1PathGeometry> Spark::shape;
ComPtr<ID2D1RadialGradientBrush> Spark::glow;

struct Shooting : Element {
    ComPtr<ID2D1LinearGradientBrush> tail;
    Track op{ {0, .86f, .88f, .96f, 1}, {0, 0, 1, 0, 0}, LINEAR };
    Track tx{ {0, .86f, .96f, 1}, {0, 0, -560, 0}, LINEAR };
    Track ty{ {0, .86f, .96f, 1}, {0, 0, 250, 0}, LINEAR };
    Shooting() {
        smooth = true;
        D2D1_GRADIENT_STOP st[2] = { {0, Hex(0xFFF4D8, .95f)}, {1, Hex(0xFFF4D8, 0)} };
        ComPtr<ID2D1GradientStopCollection> c;
        gCtx->CreateGradientStopCollection(st, 2, &c);
        gCtx->CreateLinearGradientBrush(LinearGradientBrushProperties(Point2F(1500, 110), Point2F(1650, 110)), c.Get(), &tail);
    }
    bool State(double t, float& o, D2D1_MATRIX_3X2_F& m) {
        if (t < 3) return false;
        float p = Phase(t - 3, 11, 0);
        o = op(p);
        if (o < .01f) return false;
        m = Matrix3x2F::Rotation(-24, Point2F(1500, 110)) * Matrix3x2F::Translation(tx(p), ty(p));
        return true;
    }
    D2D1_RECT_F Bounds(double t) override {
        float o; D2D1_MATRIX_3X2_F m;
        if (!State(t, o, m)) return Empty();
        return Inflate(TransformRect(RectF(1500, 110, 1650, 112), m), 2);
    }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float o; D2D1_MATRIX_3X2_F m;
        if (!State(t, o, m)) return;
        gCtx->SetTransform(m * art);
        tail->SetOpacity(o);
        gCtx->FillRectangle(RectF(1500, 110, 1650, 112), tail.Get());
    }
};
struct MoonGlow : Element {
    ComPtr<ID2D1RadialGradientBrush> b = Radial({ {0, Hex(0xCFD8F0, .28f)}, {1, Hex(0xCFD8F0, 0)} });
    Track op{ {0, .5f, 1}, {.75f, 1, .75f}, EASE_IN_OUT };
    Track sc{ {0, .5f, 1}, {1, 1.06f, 1}, EASE_IN_OUT };
    MoonGlow() { slow = true; }
    D2D1_RECT_F Bounds(double) override { return Circle(1530, 250, 172); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float p = Phase(t, 7, 0);
        gCtx->SetTransform(art);
        FillGlow(b.Get(), 1530, 250, 160 * sc(p), 160 * sc(p), op(p));
    }
};
static double gLatitude = 0;
struct Moon : Element {
    ComPtr<ID2D1PathGeometry> lit;
    bool flip = false;
    int day = -1;
    Moon() { still = true; }
    void Update() {
        SYSTEMTIME st; GetLocalTime(&st);
        if (st.wDay == day && lit) return;
        day = st.wDay;
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        double ms = ((double)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000.0) / 10000.0;
        double ph = std::fmod((ms - 947182440000.0) / 864e5 / 29.530588853, 1.0);
        if (ph < 0) ph += 1;
        float r = 48, tw = r * (float)std::cos(2 * PI * ph), aw = std::max(std::fabs(tw), .5f);
        char d[256];
        std::snprintf(d, sizeof d, "M1530,%g A%g,%g 0 0 1 1530,%g A%g,%g 0 0 %d 1530,%g Z", 250 - r, r, r, 250 + r, aw, r, tw >= 0 ? 0 : 1, 250 - r);
        lit = Path(d);
        flip = (ph > .5) != (gLatitude < 0);
    }
    D2D1_RECT_F Bounds(double) override { return Circle(1530, 250, 49); }
    void Draw(double, const D2D1_MATRIX_3X2_F& art) override {
        Update();
        gCtx->SetTransform(art);
        Solid(0x1C2235, 1);
        gCtx->FillEllipse(Ellipse(Point2F(1530, 250), 48, 48), gBrush.Get());
        gCtx->SetTransform((flip ? Matrix3x2F(-1, 0, 0, 1, 3060, 0) : Matrix3x2F::Identity()) * art);
        Solid(0xEFE8D6, 1);
        gCtx->FillGeometry(lit.Get(), gBrush.Get());
    }
};

// --- Red Book
struct Candle : Element {
    ComPtr<ID2D1RadialGradientBrush> b = Radial({ {0, Hex(0xFFECBE, .5f)}, {1, Hex(0xFFECBE, 0)} });
    Track tx{ {0, .33f, .66f, 1}, {-60, 50, -20, -60}, EASE_IN_OUT };
    Track ty{ {0, .33f, .66f, 1}, {-30, 20, 40, -30}, EASE_IN_OUT };
    Track sc{ {0, .33f, .66f, 1}, {1, 1.05f, .97f, 1}, EASE_IN_OUT };
    // The glow drifts so little that no pixel changes by even one shade: keep it still and save the work
    Candle() { still = true; }
    D2D1_RECT_F Bounds(double t) override {
        float p = Phase(t, 20, 0);
        return Circle(960 + tx(p), 470 + ty(p), 620 * sc(p) + 2);
    }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float p = Phase(t, 20, 0);
        gCtx->SetTransform(art);
        FillGlow(b.Get(), 960 + tx(p), 470 + ty(p), 620 * sc(p), 620 * sc(p), 1);
    }
};
static const D2D1_MATRIX_3X2_F kFire = Matrix3x2F(2.4f, 0, 0, 2.4f, 960, 960);  // translate(960,960) scale(2.4)
struct FireGlow : Element {
    ComPtr<ID2D1RadialGradientBrush> b = Radial({ {0, Hex(0xF2B65A, .55f)}, {1, Hex(0xF2B65A, 0)} });
    Track op{ {0, .5f, 1}, {.55f, .85f, .55f}, EASE_IN_OUT };
    D2D1_RECT_F Bounds(double) override { return TransformRect(RectF(-41, -31, 41, 19), kFire); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        gCtx->SetTransform(kFire * art);
        FillGlow(b.Get(), 0, -6, 40, 24, op(Phase(t, 2.2f, 0)));
    }
};
struct Wisp : Element {
    ComPtr<ID2D1PathGeometry> g; float off; D2D1_RECT_F local;
    Track op{ {0, .18f, 1}, {0, .75f, 0}, EASE_OUT };
    static ComPtr<ID2D1StrokeStyle> round;
    Wisp(const char* d, float o) : g(Path(d)), off(o) { g->GetBounds(nullptr, &local); }
    D2D1_MATRIX_3X2_F M(float p) {
        float k = Apply(EASE_OUT, p);
        return Matrix3x2F::Scale(.6f + .7f * k, 1) * Matrix3x2F::Translation(18 * k, -170 * k) * kFire;
    }
    D2D1_RECT_F Bounds(double t) override {
        float p = Phase(t, 4.2f, off);
        if (op(p) < .01f) return Empty();
        return Inflate(TransformRect(local, M(p)), 3);
    }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float p = Phase(t, 4.2f, off);
        gCtx->SetTransform(M(p) * art);
        Solid(0x6A5236, op(p));
        gCtx->DrawGeometry(g.Get(), gBrush.Get(), .9f, round.Get());
    }
};
ComPtr<ID2D1StrokeStyle> Wisp::round;
struct Flame : Element {
    ComPtr<ID2D1PathGeometry> g; float dur, off; uint32_t col; D2D1_POINT_2F origin; D2D1_RECT_F local;
    Track sx{ {0, .2f, .45f, .7f, 1}, {1, .94f, 1.04f, .97f, 1}, EASE_IN_OUT };
    Track sy{ {0, .2f, .45f, .7f, 1}, {1, 1.08f, .92f, 1.05f, 1}, EASE_IN_OUT };
    Track sk{ {0, .2f, .45f, .7f, 1}, {0, -3, 2, -1, 0}, EASE_IN_OUT };
    Flame(const char* d, float du, float o, uint32_t c) : g(Path(d)), dur(du), off(o), col(c) {
        g->GetBounds(nullptr, &local);
        origin = Point2F((local.left + local.right) / 2, local.bottom);
    }
    D2D1_RECT_F Bounds(double) override { return TransformRect(Inflate(RectF(local.left - 3, local.top - 4, local.right + 3, local.bottom), 1), kFire); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float p = Phase(t, dur, off);
        auto m = Matrix3x2F::Translation(-origin.x, -origin.y) * Matrix3x2F::Skew(sk(p), 0) *
                 Matrix3x2F::Scale(sx(p), sy(p)) * Matrix3x2F::Translation(origin.x, origin.y) * kFire;
        gCtx->SetTransform(m * art);
        Solid(col, 1);
        gCtx->FillGeometry(g.Get(), gBrush.Get());
    }
};
struct Logs : Element {
    ComPtr<ID2D1PathGeometry> sticks = Path("M-14,6 L14,-3 M-14,-3 L14,6");
    Logs() { still = true; }
    D2D1_RECT_F Bounds(double) override { return TransformRect(RectF(-23, -5, 24, 16), kFire); }
    void Draw(double, const D2D1_MATRIX_3X2_F& art) override {
        gCtx->SetTransform(kFire * art);
        Solid(0x4A3420, 1);
        gCtx->DrawGeometry(sticks.Get(), gBrush.Get(), 2.2f, Wisp::round.Get());
        Solid(0x6A5236, 1);
        const float st[6][3] = { {-19, 5, 3}, {-12, 10, 3.2f}, {-3, 12, 3}, {6, 12, 3.2f}, {14, 9, 3}, {20, 4, 2.8f} };
        for (auto& s : st) gCtx->DrawEllipse(Ellipse(Point2F(s[0], s[1]), s[2], s[2]), gBrush.Get(), .9f);
    }
};
struct Mote : Element {  // Red Book dust
    float x, y, dur, off;
    Track op{ {0, .15f, .85f, 1}, {0, .7f, .7f, 0}, LINEAR };
    D2D1_RECT_F Bounds(double t) override { float p = Phase(t, dur, off); return Circle(x + 60 * p, y - 300 * p, 3.5f); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float p = Phase(t, dur, off);
        gCtx->SetTransform(art);
        Solid(0xB08A55, op(p));
        gCtx->FillEllipse(Ellipse(Point2F(x + 60 * p, y - 300 * p), 2.4f, 2.4f), gBrush.Get());
    }
};
struct Needle : Element {
    ComPtr<ID2D1PathGeometry> diamond = Path("M1700,930 L1760,922 L1820,930 L1760,938 Z");
    ComPtr<ID2D1PathGeometry> body = Path("M1760,866 L1768,930 L1760,994 L1752,930 Z");
    ComPtr<ID2D1PathGeometry> north = Path("M1760,866 L1768,930 L1752,930 Z");
    Track rot{ {0, .5f, 1}, {-6, 5, -6}, EASE_IN_OUT };
    Needle() { slow = true; }
    D2D1_RECT_F Bounds(double) override { return Union(Circle(1760, 930, 66), RectF(1699, 921, 1821, 939)); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        gCtx->SetTransform(art);
        gCtx->PushLayer(LayerParameters1(Bounds(t), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, Matrix3x2F::Identity(), .6f), nullptr);
        Solid(0x5A4026, .55f);
        gCtx->FillGeometry(diamond.Get(), gBrush.Get());
        gCtx->SetTransform(Matrix3x2F::Rotation(rot(Phase(t, 6, 0)), Point2F(1760, 930)) * art);
        Solid(0x5A4026, 1);
        gCtx->FillGeometry(body.Get(), gBrush.Get());
        Solid(0x9A2A1C, 1);
        gCtx->FillGeometry(north.Get(), gBrush.Get());
        gCtx->PopLayer();
    }
};

// --- Elven Silver
struct Shimmer : Element {
    ComPtr<ID2D1PathGeometry> arch = Path("M690,1010 L690,560 C690,390 820,250 960,150 C1100,250 1230,390 1230,560 L1230,1010");
    float len = 1;
    std::vector<D2D1_POINT_2F> pts;  // 257 points along the arch, so the bounds need no geometry maths per frame
    Shimmer() {
        smooth = true;
        arch->ComputeLength(nullptr, &len);
        for (int i = 0; i <= 256; i++) { D2D1_POINT_2F p; arch->ComputePointAtLength(len * i / 256, nullptr, &p, nullptr); pts.push_back(p); }
    }
    // The dash pattern repeats every 1.36 of the arch; moving exactly one repeat per loop makes
    // the end of each loop meet the start of the next (the original moved 1.3, so the glint
    // jumped forward by its own length where the straight side meets the curve).
    float Offset(double t) { return t < 1 ? 0.f : 1.15f - 1.36f * Phase(t - 1, 8, 0); }
    D2D1_RECT_F Bounds(double t) override {
        float off = Offset(t);
        D2D1_RECT_F r = Empty();
        for (int k = -1; k <= 2; k++) {
            float a = std::max(0.f, -off + k * 1.36f), b = std::min(1.f, -off + k * 1.36f + .06f);
            if (b <= a) continue;
            for (int i = (int)std::floor(a * 256); i <= std::min(256, (int)std::ceil(b * 256)); i++)
                r = Union(r, Circle(pts[i].x, pts[i].y, 4));
        }
        return r;
    }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        // only the short travelling piece of the dash pattern is ever on the arch: draw just that
        float off = Offset(t);
        gCtx->SetTransform(art);
        Solid(0xF2FAFC, .9f);
        for (int k = -1; k <= 2; k++) {
            float a = std::max(0.f, -off + k * 1.36f), b = std::min(1.f, -off + k * 1.36f + .06f);
            if (b <= a) continue;
            ComPtr<ID2D1PathGeometry> g;
            ComPtr<ID2D1GeometrySink> sk;
            gFactory->CreatePathGeometry(&g);
            g->Open(&sk);
            auto at = [&](float u) {
                float f = u * 256; int i = std::min(255, (int)f); float r = f - i;
                return Point2F(pts[i].x + (pts[i + 1].x - pts[i].x) * r, pts[i].y + (pts[i + 1].y - pts[i].y) * r);
            };
            sk->BeginFigure(at(a), D2D1_FIGURE_BEGIN_HOLLOW);
            for (int i = (int)std::floor(a * 256) + 1; i < b * 256; i++) sk->AddLine(pts[i]);
            sk->AddLine(at(b));
            sk->EndFigure(D2D1_FIGURE_END_OPEN);
            sk->Close();
            gCtx->DrawGeometry(g.Get(), gBrush.Get(), 3, Wisp::round.Get());
        }
    }
};
struct Halo : Element {
    ComPtr<ID2D1RadialGradientBrush> b = Radial({ {0, Hex(0xBFD8E6, .45f)}, {1, Hex(0xBFD8E6, 0)} });
    Track op{ {0, .5f, 1}, {.55f, 1, .55f}, EASE_IN_OUT };
    Track sc{ {0, .5f, 1}, {1, 1.18f, 1}, EASE_IN_OUT };
    Halo() { slow = true; }
    D2D1_RECT_F Bounds(double) override { return Circle(960, 124, 64); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float p = Phase(t, 5, 0);
        gCtx->SetTransform(art);
        FillGlow(b.Get(), 960, 124, 52 * sc(p), 52 * sc(p), op(p));
    }
};
struct Glyph : Element {
    ComPtr<ID2D1PathGeometry> star = Path("M0,-12 L3,-3 L12,0 L3,3 L0,12 L-3,3 L-12,0 L-3,-3 Z");
    ComPtr<ID2D1PathGeometry> cross = Path("M-7,-7 L7,7 M-7,7 L7,-7");
    Glyph() { still = true; }
    D2D1_RECT_F Bounds(double) override { return RectF(935, 99, 985, 149); }
    void Draw(double, const D2D1_MATRIX_3X2_F& art) override {
        gCtx->SetTransform(Matrix3x2F::Scale(2, 2) * Matrix3x2F::Translation(960, 124) * art);
        Solid(0xE8F0F2, 1);
        gCtx->FillGeometry(star.Get(), gBrush.Get());
        Solid(0xE8F0F2, .6f);
        gCtx->DrawGeometry(cross.Get(), gBrush.Get(), .6f);
    }
};
struct ElvenMote : Element {
    float x, y, dur, off;
    Track op{ {0, .2f, .8f, 1}, {0, .8f, .6f, 0}, LINEAR };
    D2D1_RECT_F Bounds(double t) override { return Circle(x, y - 420 * Phase(t, dur, off), 7); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float p = Phase(t, dur, off), o = op(p), yy = y - 420 * p;
        gCtx->SetTransform(art);
        Solid(0xC8E1EB, .22f * o);
        gCtx->FillEllipse(Ellipse(Point2F(x, yy), 6, 6), gBrush.Get());
        Solid(0xDCEBF0, o);
        gCtx->FillEllipse(Ellipse(Point2F(x, yy), 2, 2), gBrush.Get());
    }
};

// --- Shire Morning
struct Rays : Element {
    ComPtr<ID2D1PathGeometry> g;
    Rays() {
        slow = true; rate = 3;
        std::string d;
        char b[96];
        for (int i = 0; i < 12; i++) {
            float a = i * PI / 6;
            std::snprintf(b, sizeof b, "M%g,%g L%g,%g ", frand10(1560 + 150 * std::cos(a)), frand10(250 + 150 * std::sin(a)),
                          frand10(1560 + 200 * std::cos(a)), frand10(250 + 200 * std::sin(a)));
            d += b;
        }
        g = Path(d.c_str());
    }
    D2D1_RECT_F Bounds(double) override { return Circle(1560, 250, 212); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        gCtx->SetTransform(Matrix3x2F::Rotation(360 * Phase(t, 140, 0), Point2F(1560, 250)) * art);
        Solid(0xF2C969, .2f);
        gCtx->DrawGeometry(g.Get(), gBrush.Get(), 18, Wisp::round.Get());
    }
};
struct Sun : Element {
    ComPtr<ID2D1RadialGradientBrush> b = Radial({ {0, Hex(0xFFF6D6, 1)}, {.45f, Hex(0xFFE9A8, .55f)}, {1, Hex(0xFFE9A8, 0)} });
    Sun() { still = true; }
    D2D1_RECT_F Bounds(double) override { return Circle(1560, 250, 241); }
    void Draw(double, const D2D1_MATRIX_3X2_F& art) override {
        gCtx->SetTransform(art);
        FillGlow(b.Get(), 1560, 250, 240, 240, 1);
        Solid(0xFFF3C9, 1);
        gCtx->FillEllipse(Ellipse(Point2F(1560, 250), 78, 78), gBrush.Get());
    }
};
struct Cloud : Element {
    static ComPtr<ID2D1Geometry> shape;
    float y, sc, dur, off, op;
    Cloud(float y_, float s, float d, float o, float a) : y(y_), sc(s), dur(d), off(o), op(a) {}
    D2D1_MATRIX_3X2_F M(double t) { return Matrix3x2F::Scale(sc, sc) * Matrix3x2F::Translation(-400 + 2800 * Phase(t, dur, off), y); }
    D2D1_RECT_F Bounds(double t) override { return Inflate(TransformRect(RectF(14, 2, 104, 37), M(t)), 2); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        gCtx->SetTransform(M(t) * art);
        Solid(0xFFFFFF, op);
        gCtx->FillGeometry(shape.Get(), gBrush.Get());
    }
};
ComPtr<ID2D1Geometry> Cloud::shape;
struct Tree : Element {
    ComPtr<ID2D1PathGeometry> branch = Path("M426,640 L430,720 M430,694 L410,676 M430,684 L450,664");
    Track rot{ {0, .5f, 1}, {-1, 1, -1}, EASE_IN_OUT };
    Tree() { slow = true; }
    D2D1_RECT_F Bounds(double) override { return RectF(354, 556, 510, 728); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        gCtx->SetTransform(Matrix3x2F::Rotation(rot(Phase(t, 7, 0)), Point2F(432, 720)) * art);
        Solid(0x5B4A33, 1);
        gCtx->DrawGeometry(branch.Get(), gBrush.Get(), 6, Wisp::round.Get());
        Solid(0x6F954F, 1);
        gCtx->FillEllipse(Ellipse(Point2F(430, 614), 52, 52), gBrush.Get());
        gCtx->FillEllipse(Ellipse(Point2F(394, 638), 32, 32), gBrush.Get());
        gCtx->FillEllipse(Ellipse(Point2F(466, 634), 36, 36), gBrush.Get());
        Solid(0x7EA35C, 1);
        gCtx->FillEllipse(Ellipse(Point2F(438, 594), 32, 32), gBrush.Get());
    }
};
struct Chimney : Element {
    Chimney() { still = true; }
    D2D1_RECT_F Bounds(double) override { return RectF(1235, 747, 1271, 833); }
    void Draw(double, const D2D1_MATRIX_3X2_F& art) override {
        gCtx->SetTransform(art);
        Solid(0x7A6247, 1);
        gCtx->FillRoundedRectangle(RoundedRect(RectF(1240, 752, 1266, 832), 3, 3), gBrush.Get());
        Solid(0x6A533B, 1);
        gCtx->FillRoundedRectangle(RoundedRect(RectF(1236, 748, 1270, 757), 2, 2), gBrush.Get());
    }
};
struct Smoke : Element {
    float off;
    Track op{ {0, .15f, 1}, {0, .55f, 0}, EASE_OUT };
    explicit Smoke(float o) : off(o) {}
    void State(double t, float& x, float& y, float& r, float& o) {
        float p = Phase(t, 4.5f, off), k = Apply(EASE_OUT, p);
        x = 1253 + 60 * k; y = 738 - 200 * k; r = 12 * (.6f + 1.2f * k); o = op(p);
    }
    D2D1_RECT_F Bounds(double t) override { float x, y, r, o; State(t, x, y, r, o); return Circle(x, y, r + 1); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float x, y, r, o; State(t, x, y, r, o);
        gCtx->SetTransform(art);
        Solid(0xFFFFFF, o);
        gCtx->FillEllipse(Ellipse(Point2F(x, y), r, r), gBrush.Get());
    }
};

// ------------------------------------------------------------------------------------ scenes
enum SceneId { NONE = -1, SHIRE, REDBOOK, STARLIT, ELVEN };
static const wchar_t* kSceneKey[] = { L"shire", L"redbook", L"starlit", L"elven" };
static const wchar_t* kWallName[] = { L"Lembas - Shire Morning.jpg", L"Lembas - Red Book.jpg", L"Lembas - Starlit Night.jpg", L"Lembas - Elven Silver.jpg" };
static const wchar_t* kLayerName[] = { L"back", L"mid", L"front" };

struct Scene {
    SceneId id = NONE;
    std::vector<Element*> els;
    std::vector<int> layers;  // which of back/mid/front exist
    ~Scene() { for (auto* e : els) delete e; }
};
static Scene* gScene;

static Scene* BuildScene(SceneId id) {
    Scene* s = new Scene();
    s->id = id;
    auto& E = s->els;
    if (id == STARLIT) {
        Rng r(1954);
        for (int i = 0; i < 300 * 5; i++) r();  // the still stars are in the back layer
        auto col = [&]() { double k = r(); return k < .12 ? 0xFFE2B0u : k < .25 ? 0xBFD4FFu : 0xF4F1E8u; };
        E.push_back(new LayerEl(0));
        for (int i = 0; i < 70; i++) {
            auto* t = new Twinkle();
            t->x = frand10(r() * 1920); t->y = frand10(std::pow(r(), 1.3) * 720); t->r = frand10(1 + std::pow(r(), 3) * 2);
            t->col = col(); t->dur = frand10(1.8 + r() * 3.4); t->off = frand10(r() * 6);
            E.push_back(t);
        }
        for (int i = 0; i < 10; i++) {
            auto* k = new Spark();
            k->x = frand10(60 + r() * 1800); k->y = frand10(40 + r() * 600); k->dur = frand10(2.2 + r() * 2.5); k->off = frand10(r() * 4);
            E.push_back(k);
        }
        E.push_back(new Shooting());
        E.push_back(new MoonGlow());
        E.push_back(new Moon());
        E.push_back(new LayerEl(2));
        s->layers = { 0, 2 };
    } else if (id == REDBOOK) {
        Rng r(1937);
        for (int i = 0; i < 1100 * 5; i++) r();
        E.push_back(new LayerEl(0));
        E.push_back(new Candle());
        E.push_back(new LayerEl(2));
        E.push_back(new FireGlow());
        E.push_back(new Wisp("M0,-18 C-5,-26 5,-34 0,-42 S-5,-58 0,-66", 0));
        E.push_back(new Wisp("M-3,-18 C-8,-26 2,-34 -3,-42 S-8,-58 -3,-66", 1.4f));
        E.push_back(new Wisp("M3,-18 C-2,-26 8,-34 3,-42 S-2,-58 3,-66", 2.8f));
        E.push_back(new Flame("M0,0 C-10,-4 -10,-14 -5,-21 C-4,-16 -1,-15 -1,-18 C-2,-24 1,-30 6,-34 C4,-26 10,-22 9,-13 C9,-5 6,-1 0,0 Z", 1.4f, 0, 0x9A2A1C));
        E.push_back(new Flame("M0,0 C-4,-2 -5,-7 -3,-11 C-2,-8 0,-8 0,-10 C0,-13 2,-16 4,-18 C4,-13 6,-9 5,-5 C4,-2 2,0 0,0 Z", 1.1f, .5f, 0xD9973A));
        E.push_back(new Logs());
        for (int i = 0; i < 18; i++) {
            auto* m = new Mote();
            m->x = frand10(200 + r() * 1520); m->y = frand10(500 + r() * 520); m->dur = frand10(10 + r() * 9); m->off = frand10(r() * 16);
            E.push_back(m);
        }
        E.push_back(new Needle());
        s->layers = { 0, 2 };
    } else if (id == ELVEN) {
        Rng r(1955);
        for (int i = 0; i < 90 * 4; i++) r();
        E.push_back(new LayerEl(0));
        E.push_back(new Shimmer());
        E.push_back(new Halo());
        E.push_back(new Glyph());
        for (int i = 0; i < 22; i++) {
            auto* m = new ElvenMote();
            m->x = frand10(720 + r() * 480); m->y = frand10(620 + r() * 380); m->dur = frand10(9 + r() * 9); m->off = frand10(r() * 16);
            E.push_back(m);
        }
        s->layers = { 0 };
    } else if (id == SHIRE) {
        E.push_back(new LayerEl(0));
        E.push_back(new Rays());
        E.push_back(new Sun());
        E.push_back(new Cloud(500, 2.4f, 90, 20, .85f));
        E.push_back(new Cloud(560, 1.6f, 120, 70, .75f));
        E.push_back(new Cloud(70, 1.5f, 150, 110, .65f));
        E.push_back(new Cloud(530, 1.2f, 170, 30, .6f));
        E.push_back(new LayerEl(1));
        E.push_back(new Tree());
        E.push_back(new Chimney());
        for (int i = 0; i < 3; i++) E.push_back(new Smoke(i * 1.5f));
        E.push_back(new LayerEl(2));
        s->layers = { 0, 1, 2 };
    }
    return s;
}

static void InitShared() {
    gFactory->CreateStrokeStyle(StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &Wisp::round);
    Spark::shape = Path("M0,-9 L1.4,-1.4 L9,0 L1.4,1.4 L0,9 L-1.4,1.4 L-9,0 L-1.4,-1.4 Z");
    Spark::glow = Radial({ {0, Hex(0xFFF4D8, .9f)}, {1, Hex(0xFFF4D8, 0)} });
    // cloud = one shape (so overlapping puffs don't double up their transparency)
    ComPtr<ID2D1RoundedRectangleGeometry> rr;
    ComPtr<ID2D1EllipseGeometry> c1, c2, c3;
    gFactory->CreateRoundedRectangleGeometry(RoundedRect(RectF(14, 22, 104, 34), 6, 6), &rr);
    gFactory->CreateEllipseGeometry(Ellipse(Point2F(30, 22), 12, 12), &c1);
    gFactory->CreateEllipseGeometry(Ellipse(Point2F(60, 20), 17, 17), &c2);
    gFactory->CreateEllipseGeometry(Ellipse(Point2F(90, 23), 12, 12), &c3);
    ID2D1Geometry* parts[4] = { rr.Get(), c1.Get(), c2.Get(), c3.Get() };
    ComPtr<ID2D1GeometryGroup> grp;
    gFactory->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, parts, 4, &grp);
    Cloud::shape = grp;
}
static void ReleaseShared() {
    Wisp::round.Reset(); Spark::shape.Reset(); Spark::glow.Reset(); Cloud::shape.Reset();
}

// ------------------------------------------------------------------------------------ settings
struct Settings { bool clock = true, meals = true, enabled = true, adaptive = true, modern = true; double lat = -33.87, lon = 151.21; std::wstring look = L"adaptive"; } gSet;

static std::wstring ReadAll(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::string s;
    char buf[4096];
    DWORD n;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n) s.append(buf, n);
    CloseHandle(h);
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF) s = s.substr(3);
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], len);
    return w;
}
static bool JsonValue(const std::wstring& j, const wchar_t* key, std::wstring& out) {
    std::wstring k = std::wstring(L"\"") + key + L"\"";
    size_t p = j.find(k);
    if (p == std::wstring::npos) return false;
    p = j.find(L':', p + k.size());
    if (p == std::wstring::npos) return false;
    p++;
    while (p < j.size() && iswspace(j[p])) p++;
    size_t e = p;
    while (e < j.size() && j[e] != L',' && j[e] != L'}' && j[e] != L'\r' && j[e] != L'\n') e++;
    out = j.substr(p, e - p);
    while (!out.empty() && iswspace(out.back())) out.pop_back();
    return true;
}
static void LoadSettings() {
    wchar_t base[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    std::wstring j = ReadAll(std::wstring(base) + L"\\Lembas\\lembas.json"), v;
    if (JsonValue(j, L"latitude", v)) gSet.lat = _wtof(v.c_str());
    if (JsonValue(j, L"longitude", v)) gSet.lon = _wtof(v.c_str());
    gSet.adaptive = !JsonValue(j, L"look", v) || v.find(L"adaptive") != std::wstring::npos;
    gSet.look = L"adaptive";
    if (JsonValue(j, L"look", v)) for (auto* k : { L"shire", L"redbook", L"starlit", L"elven" }) if (v.find(k) != std::wstring::npos) gSet.look = k;
    // the clock in the watch face's type (stacked heavy hours and light minutes), unless set to classic
    gSet.modern = !(JsonValue(j, L"desktopClockStyle", v) && v.find(L"classic") != std::wstring::npos);
    gSet.clock = !(JsonValue(j, L"desktopClock", v) && v == L"false");
    gSet.meals = !(JsonValue(j, L"meals", v) && v == L"false");
    gSet.enabled = !(JsonValue(j, L"livingDesktop", v) && v == L"false");
    gLatitude = gSet.lat;
}
static SceneId gForced = NONE;  // --look <name>: for testing one look
static SceneId SceneFromWallpaper() {
    if (gForced != NONE) return gForced;
    wchar_t wall[MAX_PATH * 2] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH * 2, wall, 0);
    std::wstring w = wall;
    for (int i = 0; i < 4; i++) {
        std::wstring n = kWallName[i];
        if (w.size() >= n.size() && _wcsicmp(w.c_str() + w.size() - n.size(), n.c_str()) == 0) return (SceneId)i;
    }
    return NONE;
}

// ------------------------------------------------------------------------------------ the sun
// The same sums as the phone (Sun.kt), the watch and LembasSun.ps1, so every device changes look
// at the same minute. This app is always running, so it starts the Lembas Sun task at the exact
// minute of a switch (the task itself also runs every 10 minutes, at sign-in and on unlock).
struct SunDay { int rise, noon, evening, set; };
static SunDay SunToday(const SYSTEMTIME& now, double lat, double lon) {
    SYSTEMTIME noonLocal = now, utc;
    noonLocal.wHour = 12; noonLocal.wMinute = noonLocal.wSecond = noonLocal.wMilliseconds = 0;
    double offset = 0;
    if (TzSpecificLocalTimeToSystemTime(nullptr, &noonLocal, &utc)) {
        FILETIME a, b;
        SystemTimeToFileTime(&noonLocal, &a); SystemTimeToFileTime(&utc, &b);
        long long da = ((long long)a.dwHighDateTime << 32) | a.dwLowDateTime, db = ((long long)b.dwHighDateTime << 32) | b.dwLowDateTime;
        offset = (double)(da - db) / 600000000.0;  // 100ns ticks -> minutes
    }
    static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int y = now.wYear;
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    int doy = cum[now.wMonth - 1] + now.wDay + (leap && now.wMonth > 2 ? 1 : 0);
    const double PI = 3.14159265358979323846;
    double g = 2 * PI / 365.0 * (doy - 1);
    double eot = 229.18 * (0.000075 + 0.001868 * cos(g) - 0.032077 * sin(g) - 0.014615 * cos(2 * g) - 0.040849 * sin(2 * g));
    double dec = 0.006918 - 0.399912 * cos(g) + 0.070257 * sin(g) - 0.006758 * cos(2 * g) + 0.000907 * sin(2 * g) - 0.002697 * cos(3 * g) + 0.00148 * sin(3 * g);
    double noon = 720 - 4 * lon - eot + offset;
    double latR = lat * PI / 180;
    double cosHa = cos(90.833 * PI / 180) / (cos(latR) * cos(dec)) - tan(latR) * tan(dec);
    double ha = cosHa >= 1 ? 0 : cosHa <= -1 ? 180 : acos(cosHa) * 180 / PI;
    // late afternoon: the sun down to 15 degrees, when the fireside Red Book look takes over
    double cosHe = (sin(15 * PI / 180) - sin(latR) * sin(dec)) / (cos(latR) * cos(dec));
    double he = std::min(ha, cosHe >= 1 ? 0.0 : cosHe <= -1 ? 180.0 : acos(cosHe) * 180 / PI);
    auto clampMin = [](double m) { return std::min(1439, std::max(0, (int)m)); };
    return { clampMin(noon - 4 * ha), clampMin(noon), clampMin(noon + 4 * he), clampMin(noon + 4 * ha) };
}
static SceneId SunScene(const SYSTEMTIME& now) {
    SunDay d = SunToday(now, gSet.lat, gSet.lon);
    int m = now.wHour * 60 + now.wMinute;
    if (m >= d.rise && m < d.evening) return SHIRE;
    if (m >= d.evening && m < d.set) return REDBOOK;
    if (m >= 23 * 60 || m < 4 * 60) return ELVEN;
    return STARLIT;
}
static void CheckSun() {
    static int lastMinute = -1, sinceReload = 0;
    static ULONGLONG lastKick = 0;
    SYSTEMTIME st; GetLocalTime(&st);
    int minute = st.wHour * 60 + st.wMinute;
    if (minute == lastMinute) return;
    lastMinute = minute;
    if (++sinceReload >= 10) { sinceReload = 0; LoadSettings(); }  // the time zone (and so the city) may have changed
    if (!gSet.adaptive || gForced != NONE) return;
    SceneId want = SunScene(st), shown = SceneFromWallpaper();
    if (shown == NONE || want == shown) return;
    ULONGLONG t = GetTickCount64();
    if (lastKick && t - lastKick < 5 * 60 * 1000) return;
    lastKick = t;
    ShellExecuteW(nullptr, L"open", L"schtasks.exe", L"/Run /TN \"\\Lembas\\Lembas Sun\"", nullptr, SW_HIDE);
}

// ------------------------------------------------------------------------------------ monitors
struct Mon {
    RECT rc;          // in canvas pixels
    RECT work;        // work area in screen coordinates (for the "is it covered" check)
    RECT screen;
    bool primary;
    D2D1_MATRIX_3X2_F art;  // artboard -> canvas
    UINT bw, bh;            // size of the scene on this monitor (it is cropped like "fill")
    ComPtr<ID2D1Bitmap1> layer[3];
};
static std::vector<Mon> gMons;

static void SetupMon(Mon& m, RECT rc) {
    m.rc = rc;
    float W = (float)(rc.right - rc.left), H = (float)(rc.bottom - rc.top);
    float s = std::max(W / 1920.f, H / 1080.f);
    m.bw = (UINT)std::ceil(1920 * s);
    m.bh = (UINT)std::ceil(1080 * s);
    float ox = std::round((W - m.bw) / 2), oy = H - m.bh;  // centred, anchored to the bottom (xMidYMax slice)
    m.art = Matrix3x2F(m.bw / 1920.f, 0, 0, m.bh / 1080.f, rc.left + ox, rc.top + oy);
}
static BOOL CALLBACK MonEnum(HMONITOR hm, HDC, LPRECT, LPARAM) {
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(hm, &mi);
    Mon m{};
    m.screen = mi.rcMonitor;
    m.work = mi.rcWork;
    m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    POINT a = { mi.rcMonitor.left, mi.rcMonitor.top }, b = { mi.rcMonitor.right, mi.rcMonitor.bottom };
    ScreenToClient(gParent, &a);
    ScreenToClient(gParent, &b);
    SetupMon(m, RECT{ a.x, a.y, b.x, b.y });
    gMons.push_back(m);
    return TRUE;
}

static ComPtr<ID2D1Bitmap1> LoadLayer(const std::wstring& file, UINT w, UINT h) {
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> sc;
    ComPtr<IWICFormatConverter> conv;
    ComPtr<ID2D1Bitmap1> bmp;
    if (FAILED(gWic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return bmp;
    if (FAILED(dec->GetFrame(0, &frame))) return bmp;
    gWic->CreateBitmapScaler(&sc);
    if (FAILED(sc->Initialize(frame.Get(), w, h, WICBitmapInterpolationModeFant))) return bmp;
    gWic->CreateFormatConverter(&conv);
    if (FAILED(conv->Initialize(sc.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut))) return bmp;
    gCtx->CreateBitmapFromWicBitmap(conv.Get(), BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96), &bmp);
    return bmp;
}
static void LoadLayers() {
    for (auto& m : gMons) {
        for (auto& l : m.layer) l.Reset();
        if (!gScene) continue;
        for (int i : gScene->layers) {
            std::wstring ext = i == 0 ? L".jpg" : L".png";
            m.layer[i] = LoadLayer(gDir + L"\\" + kSceneKey[gScene->id] + L"-" + kLayerName[i] + ext, m.bw, m.bh);
        }
    }
}

// ------------------------------------------------------------------------------------ the clock text
static const wchar_t* kMonths[] = { L"Afteryule", L"Solmath", L"Rethe", L"Astron", L"Thrimidge", L"Forelithe", L"Afterlithe", L"Wedmath", L"Halimath", L"Winterfilth", L"Blotmath", L"Foreyule" };
static const wchar_t* kWeek[] = { L"Sunday", L"Monday", L"Trewsday", L"Hevensday", L"Mersday", L"Highday", L"Sterday" };

static int64_t DaysFromCivil(int y, unsigned m, unsigned d) {  // days since 1970-01-01
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static bool IsLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static void ShireDate(const SYSTEMTIME& st, int& day, std::wstring& name) {
    int y = st.wYear;
    bool after = st.wMonth == 12 && st.wDay >= 21;
    int64_t d = DaysFromCivil(y, st.wMonth, st.wDay) - DaysFromCivil(after ? y : y - 1, 12, 21);
    bool leap = IsLeap((after ? y : y - 1) + 1);
    day = 0;
    if (d == 0) { name = L"1 Yule"; return; }
    if (d == 1) { name = L"2 Yule"; return; }
    d -= 2;
    if (d < 180) { day = (int)(d % 30) + 1; name = kMonths[d / 30]; return; }
    d -= 180;
    std::vector<std::wstring> lithe = leap ? std::vector<std::wstring>{ L"1 Lithe", L"Mid-year’s Day", L"Overlithe", L"2 Lithe" }
                                           : std::vector<std::wstring>{ L"1 Lithe", L"Mid-year’s Day", L"2 Lithe" };
    if (d < (int64_t)lithe.size()) { name = lithe[(size_t)d]; return; }
    d -= lithe.size();
    if (d < 180) { day = (int)(d % 30) + 1; name = kMonths[6 + d / 30]; return; }
    name = L"1 Yule";
}
static std::wstring Ordinal(int n) {
    const wchar_t* s = L"th";
    if (!(n % 100 >= 11 && n % 100 <= 13)) { if (n % 10 == 1) s = L"st"; else if (n % 10 == 2) s = L"nd"; else if (n % 10 == 3) s = L"rd"; }
    return std::to_wstring(n) + s;
}
static void Period(int h, std::wstring& q, std::wstring& en) {
    if (h >= 4 && h <= 6) { q = L"Tindómë"; en = L"the star-dawn"; }
    else if (h >= 7 && h <= 17) { q = L"Aurë"; en = L"the day"; }
    else if (h >= 18 && h <= 20) { q = L"Undómë"; en = L"eventide"; }
    else { q = L"Lómë"; en = L"the night"; }
}
static std::wstring Meal(int h) {
    if (h == 5 || h == 6) return L"Kettle on";
    if (h == 7 || h == 8) return L"Breakfast";
    if (h == 9 || h == 10) return L"Second breakfast";
    if (h == 11) return L"Elevenses";
    if (h == 12 || h == 13) return L"Luncheon";
    if (h == 14) return L"A stroll in the garden";
    if (h == 15 || h == 16) return L"Afternoon tea";
    if (h == 17) return L"A pint at the Green Dragon";
    if (h == 18 || h == 19) return L"Dinner";
    if (h == 20 || h == 21) return L"Supper";
    return L"The Shire sleeps";
}
static std::wstring Occasion(const SYSTEMTIME& st) {
    if (st.wMonth == 1 && st.wDay == 3) return L"The Professor’s birthday";
    if (st.wMonth == 3 && st.wDay == 25) return L"Tolkien Reading Day";
    if (st.wMonth == 9 && st.wDay == 22) return L"Hobbit Day";
    return L"";
}
static std::wstring Upper(const std::wstring& s) {
    std::wstring o = s;
    if (!o.empty()) LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING, s.c_str(), (int)s.size(), &o[0], (int)o.size(), nullptr, nullptr, 0);
    return o;
}
static std::wstring DateFmt(const SYSTEMTIME& st, const wchar_t* pic) {
    wchar_t b[128] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, pic, b, 128, nullptr);
    return b;
}
static std::wstring LongDateNoYear(const SYSTEMTIME& st) {  // "Monday 19 October" / "Monday, October 19"
    wchar_t pat[128] = {};
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SLONGDATE, pat, 128);
    std::wstring p = pat, o;
    // drop the year and the separators that belonged to it
    size_t y = p.find(L'y');
    if (y != std::wstring::npos) {
        size_t a = y, b = y;
        while (b < p.size() && p[b] == L'y') b++;
        while (a > 0 && (p[a - 1] == L' ' || p[a - 1] == L',' || p[a - 1] == L'.' || p[a - 1] == L'/' || p[a - 1] == L'-')) a--;
        if (a == 0) while (b < p.size() && (p[b] == L' ' || p[b] == L',' || p[b] == L'.')) b++;
        p.erase(a, b - a);
    }
    if (p.empty()) p = L"dddd d MMMM";
    return DateFmt(st, p.c_str());
}
static bool Use24h() {
    wchar_t f[64] = {};
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STIMEFORMAT, f, 64);
    return wcschr(f, L'H') != nullptr;
}

// Font lookup: the Lembas fonts are installed for the user by the kit; fall back gracefully.
static ComPtr<IDWriteFontCollection> gFonts;
static std::wstring Family(std::initializer_list<const wchar_t*> names) {
    for (auto* n : names) {
        UINT32 i; BOOL ok = FALSE;
        if (gFonts && SUCCEEDED(gFonts->FindFamilyName(n, &i, &ok)) && ok) return n;
    }
    return L"Georgia";
}

struct Line {
    std::wstring text, family;
    float size;  // in vh
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
    uint32_t color = 0;
    float spacing = 0;  // letter-spacing in em
    float margin = 0;   // margin-top in vh
    bool time = false, pill = false;
};

struct ClockImage {
    ComPtr<ID2D1Bitmap1> bmp;
    D2D1_RECT_F rc = { 0, 0, 0, 0 };  // canvas pixels
};
static ClockImage gClock;
static int gClockMinute = -1;

static ComPtr<IDWriteTextLayout> MakeLayout(const Line& l, float vh) {
    ComPtr<IDWriteTextFormat> f;
    gDWrite->CreateTextFormat(l.family.c_str(), gFonts.Get(), l.weight, l.style, DWRITE_FONT_STRETCH_NORMAL, l.size * vh, L"en", &f);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteTextLayout> lay;
    gDWrite->CreateTextLayout(l.text.c_str(), (UINT32)l.text.size(), f.Get(), 10000, 1000, &lay);
    ComPtr<IDWriteTextLayout1> l1;
    if (l.spacing != 0 && SUCCEEDED(lay.As(&l1))) l1->SetCharacterSpacing(0, l.spacing * l.size * vh, 0, DWRITE_TEXT_RANGE{ 0, (UINT32)l.text.size() });
    return lay;
}

static void BuildClock(SceneId sc) {
    gClock.bmp.Reset();
    gClock.rc = Empty();
    if (!gSet.clock || sc == NONE) return;
    const Mon* pm = nullptr;
    for (auto& m : gMons) if (m.primary) pm = &m;
    if (!pm && !gMons.empty()) pm = &gMons[0];
    if (!pm) return;
    float W = (float)(pm->rc.right - pm->rc.left), H = (float)(pm->rc.bottom - pm->rc.top), vh = H / 100, vw = W / 100;

    SYSTEMTIME st; GetLocalTime(&st);
    int h = st.wHour, sd; std::wstring sname, q, en;
    ShireDate(st, sd, sname);
    Period(h, q, en);
    std::wstring wd = kWeek[st.wDayOfWeek], occ = Occasion(st);
    bool h24 = Use24h();
    wchar_t tb[16];
    std::swprintf(tb, 16, L"%d:%02d", h24 ? h : ((h % 12) ? h % 12 : 12), st.wMinute);
    std::wstring time = tb, shireTxt = sd ? std::to_wstring(sd) + L" " + sname : sname;
    std::wstring greg = LongDateNoYear(st) + (occ.empty() ? L"" : L" · " + occ);
    std::wstring meal = gSet.meals ? Meal(h) : L"";

    std::wstring garamond = Family({ L"Cormorant Garamond", L"Cormorant Garamond Light" });
    std::wstring cinzel = Family({ L"Cinzel" });
    std::wstring fell = Family({ L"IM FELL English", L"IM Fell English" }), fellsc = Family({ L"IM FELL English SC", L"IM Fell English SC" });
    std::wstring fraunces = Family({ L"Fraunces", L"Fraunces Light" }), alegreya = Family({ L"Alegreya Sans Medium", L"Alegreya Sans" });

    std::vector<Line> L(5);
    Line &lab = L[0], &tim = L[1], &shi = L[2], &dat = L[3], &mea = L[4];
    for (auto& l : L) l.family = garamond;
    lab.family = cinzel; lab.size = 1.6f; lab.spacing = .32f;
    tim.size = 15; tim.weight = DWRITE_FONT_WEIGHT_LIGHT; tim.time = true;
    shi.size = 3.1f; shi.style = DWRITE_FONT_STYLE_ITALIC; shi.margin = 1;
    dat.size = 2; dat.spacing = .04f;
    mea.size = 2.4f; mea.style = DWRITE_FONT_STYLE_ITALIC; mea.margin = 2.6f;
    tim.text = time; dat.text = greg; mea.text = meal;
    bool left = false, shadow = false;
    float top = 15;
    uint32_t firstColor = 0;
    std::wstring ampm;

    if (gSet.modern) {
        // The watch face's type: the date and the Shire date, then the hours (heavy, in the look's
        // colour) over the minutes (light), then the mealtime.
        static const uint32_t hoursC[] = { 0x3E6B2E, 0x9A2A1C, 0xE9CC88, 0xC6DDE8 };
        static const uint32_t textC[] = { 0x2F4A2A, 0x2B1E12, 0xF3EBD6, 0xE6ECEE };
        static const uint32_t loreC[] = { 0x6A7D4E, 0x9A2A1C, 0xD9BC7A, 0xC4D2D6 };
        int k = sc == SHIRE ? 0 : sc == REDBOOK ? 1 : sc == STARLIT ? 2 : 3;
        wchar_t hb[8], mb[8];
        std::swprintf(hb, 8, L"%02d", h24 ? h : ((h % 12) ? h % 12 : 12));
        std::swprintf(mb, 8, L"%02d", st.wMinute);
        L.assign(6, Line());
        Line &d = L[0], &s2 = L[1], &hh = L[2], &mm = L[3], &me = L[4];
        L.pop_back();
        d.family = alegreya; d.weight = DWRITE_FONT_WEIGHT_MEDIUM; d.size = 2.2f; d.spacing = .02f; d.color = textC[k];
        d.text = DateFmt(st, L"ddd d MMM");
        s2.family = fraunces; s2.style = DWRITE_FONT_STYLE_ITALIC; s2.size = 2.4f; s2.color = loreC[k];
        s2.text = wd + L" · " + shireTxt;
        hh.family = fraunces; hh.weight = DWRITE_FONT_WEIGHT_EXTRA_BOLD; hh.size = 19; hh.time = true; hh.color = hoursC[k]; hh.margin = 1.2f;
        hh.text = hb;
        mm.family = fraunces; mm.weight = DWRITE_FONT_WEIGHT_LIGHT; mm.size = 19; mm.time = true; mm.color = textC[k]; mm.margin = -2.2f;
        mm.text = mb;
        me.family = fraunces; me.style = DWRITE_FONT_STYLE_ITALIC; me.size = 2.2f; me.color = loreC[k]; me.margin = 1.4f;
        me.text = meal;
        top = sc == ELVEN ? 26 : 14;   // under the elven arch's star
        shadow = sc == STARLIT || sc == ELVEN;
        time = L"";   // no am/pm suffix
        if (me.text.empty()) L.pop_back();
    } else if (sc == REDBOOK) {
        for (auto& l : L) l.family = fell;
        lab.family = fellsc; lab.size = 2.2f; lab.spacing = .24f; lab.color = 0x9A2A1C;
        tim.weight = DWRITE_FONT_WEIGHT_NORMAL; tim.size = 16; firstColor = 0x9A2A1C;
        shi.size = 3.6f; dat.color = 0x6A5236; mea.size = 2.8f; mea.color = 0x9A2A1C;
        tim.color = shi.color = 0x2B1E12;
        lab.text = wd;
        shi.text = sd ? Ordinal(sd) + L" of " + sname : sname;
        dat.text = DateFmt(st, L"dddd") + L", the " + Ordinal(st.wDay) + L" of " + DateFmt(st, L"MMMM") + (occ.empty() ? L"" : L" · " + occ);
        static const wchar_t* timed[] = { L"Breakfast", L"breakfast", L"Elevenses", L"Luncheon", L"tea", L"Dinner", L"Supper" };
        for (auto* k : timed) if (!meal.empty() && meal.find(k) != std::wstring::npos) {
            mea.text = L"Time for " + std::wstring(1, (wchar_t)towlower(meal[0])) + meal.substr(1);
            break;
        }
    } else if (sc == ELVEN) {
        for (auto& l : L) l.color = 0xE6ECEE;
        lab.color = 0x8E9A9E; lab.spacing = .4f; lab.size = 1.5f;
        tim.size = 13; shi.color = 0xC4D2D6; shi.size = 2.8f;
        dat.family = cinzel; dat.color = 0x8E9A9E; dat.size = 1.5f; dat.spacing = .2f;
        mea.color = 0xA9C6D6; mea.weight = DWRITE_FONT_WEIGHT_LIGHT; mea.margin = 7;
        top = 27;
        lab.text = Upper(q); shi.text = wd + L" · " + shireTxt; dat.text = Upper(greg);
    } else if (sc == SHIRE) {
        for (auto& l : L) { l.family = fraunces; l.color = 0x2F4A2A; }
        lab.family = alegreya; lab.weight = DWRITE_FONT_WEIGHT_MEDIUM; lab.color = 0x6A7D4E; lab.spacing = .18f; lab.size = 1.8f;
        tim.size = 14; tim.spacing = -.03f;
        dat.family = alegreya; dat.weight = DWRITE_FONT_WEIGHT_MEDIUM; dat.color = 0x6A7D4E;
        mea.pill = true; mea.size = 2.2f;
        left = true; top = 13;
        lab.text = Upper(wd + L" · " + q); shi.text = shireTxt;
        if (!h24) { ampm = h < 12 ? L"am" : L"pm"; tim.text = time + ampm; }
    } else {  // Starlit Night
        for (auto& l : L) l.color = 0xF3EBD6;
        lab.color = shi.color = 0xD9BC7A; dat.color = 0xB9B19C;
        shadow = true;
        lab.text = Upper(q + L" · " + en); shi.text = wd + L", " + shireTxt;
    }
    if (!gSet.modern && mea.text.empty()) L.pop_back();

    // lay the lines out top to bottom, like the scene's CSS column (gap .4vh)
    struct Placed { ComPtr<IDWriteTextLayout> lay; float x, y, w, h; const Line* l; };
    std::vector<Placed> placed;
    float y = top * vh;
    D2D1_RECT_F box = Empty();
    for (size_t i = 0; i < L.size(); i++) {
        Line& l = L[i];
        auto lay = MakeLayout(l, vh);
        float size = l.size * vh;
        if (l.time) {
            // line-height: 1, with the text centred in it the way CSS does
            ComPtr<IDWriteFontFamily> fam; UINT32 idx; BOOL ok = FALSE;
            float asc = .9f, desc = .3f;
            if (gFonts && SUCCEEDED(gFonts->FindFamilyName(l.family.c_str(), &idx, &ok)) && ok && SUCCEEDED(gFonts->GetFontFamily(idx, &fam))) {
                ComPtr<IDWriteFont> font;
                if (SUCCEEDED(fam->GetFirstMatchingFont(l.weight, DWRITE_FONT_STRETCH_NORMAL, l.style, &font))) {
                    DWRITE_FONT_METRICS fm; font->GetMetrics(&fm);
                    asc = (float)fm.ascent / fm.designUnitsPerEm; desc = (float)fm.descent / fm.designUnitsPerEm;
                }
            }
            lay->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size, (size - (asc + desc) * size) / 2 + asc * size);
            if (!ampm.empty()) {
                DWRITE_TEXT_RANGE r{ (UINT32)time.size(), (UINT32)ampm.size() };
                lay->SetFontSize(size * .28f, r);
                lay->SetFontWeight(DWRITE_FONT_WEIGHT_MEDIUM, r);
                ComPtr<IDWriteTextLayout1> l1;
                if (SUCCEEDED(lay.As(&l1))) l1->SetCharacterSpacing(0, -.03f * size + .3f * size * .28f, 0, DWRITE_TEXT_RANGE{ (UINT32)time.size() - 1, 1 });
            }
        }
        DWRITE_TEXT_METRICS tm; lay->GetMetrics(&tm);
        float w = tm.widthIncludingTrailingWhitespace, hgt = l.time ? size : tm.height;
        if (i > 0) y += .4f * vh;
        y += l.margin * vh;
        float padX = l.pill ? 1.8f * vh : 0, padY = l.pill ? .7f * vh : 0;
        float bw = w + 2 * padX, bh = hgt + 2 * padY;
        float x = left ? 9 * vw : (W - bw) / 2;
        placed.push_back({ lay, x + padX, y + padY, w, hgt, &l });
        box = Union(box, RectF(x, y, x + bw, y + bh));
        y += bh;
    }
    float margin = shadow ? 4 * vh : 1 * vh;
    box = Inflate(box, margin);
    // keep the bitmap inside the monitor
    box.left = std::max(box.left, 0.f); box.top = std::max(box.top, 0.f);
    box.right = std::min(box.right, W); box.bottom = std::min(box.bottom, H);
    box = RectF(std::floor(box.left), std::floor(box.top), std::ceil(box.right), std::ceil(box.bottom));
    UINT bw = (UINT)(box.right - box.left), bh = (UINT)(box.bottom - box.top);
    if (!bw || !bh) return;

    auto props = BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> bmp, timeOnly;
    if (FAILED(gCtx->CreateBitmap(SizeU(bw, bh), nullptr, 0, props, &bmp))) return;
    ComPtr<ID2D1Image> old;
    gCtx->GetTarget(&old);
    gCtx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    auto shift = Matrix3x2F::Translation(-box.left, -box.top);

    auto drawLine = [&](const Placed& p) {
        Solid(p.l->color, 1);
        if (p.l->time && firstColor) {
            ComPtr<ID2D1SolidColorBrush> red;
            gCtx->CreateSolidColorBrush(Hex(firstColor), &red);
            p.lay->SetDrawingEffect(red.Get(), DWRITE_TEXT_RANGE{ 0, 1 });
        }
        if (p.l->time && !ampm.empty()) {
            ComPtr<ID2D1SolidColorBrush> sm;
            gCtx->CreateSolidColorBrush(Hex(0x6A7D4E), &sm);
            p.lay->SetDrawingEffect(sm.Get(), DWRITE_TEXT_RANGE{ (UINT32)time.size(), (UINT32)ampm.size() });
        }
        gCtx->DrawTextLayout(Point2F(p.x, p.y), p.lay.Get(), gBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    };

    if (shadow) {  // text-shadow: 0 .3vh 2vh rgba(0,0,0,.6) on the time
        if (SUCCEEDED(gCtx->CreateBitmap(SizeU(bw, bh), nullptr, 0, props, &timeOnly))) {
            gCtx->SetTarget(timeOnly.Get());
            gCtx->BeginDraw();
            gCtx->Clear(ColorF(0, 0, 0, 0));
            gCtx->SetTransform(shift);
            for (auto& p : placed) if (p.l->time) drawLine(p);
            gCtx->EndDraw();
        }
    }
    gCtx->SetTarget(bmp.Get());
    gCtx->BeginDraw();
    gCtx->Clear(ColorF(0, 0, 0, 0));
    if (timeOnly) {
        ComPtr<ID2D1Effect> sh;
        if (SUCCEEDED(gCtx->CreateEffect(CLSID_D2D1Shadow, &sh))) {
            sh->SetInput(0, timeOnly.Get());
            sh->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, vh);
            sh->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1_VECTOR_4F{ 0, 0, 0, .6f });
            gCtx->SetTransform(Matrix3x2F::Identity());
            gCtx->DrawImage(sh.Get(), Point2F(0, .3f * vh));
        }
    }
    gCtx->SetTransform(shift);
    for (auto& p : placed) {
        if (p.l->pill) {
            Solid(0xFFFFFF, .6f);
            float px = 1.8f * vh, py = .7f * vh;
            D2D1_RECT_F r = RectF(p.x - px, p.y - py, p.x + p.w + px, p.y + p.h + py);
            gCtx->FillRoundedRectangle(RoundedRect(r, (r.bottom - r.top) / 2, (r.bottom - r.top) / 2), gBrush.Get());
        }
        drawLine(p);
    }
    gCtx->EndDraw();
    gCtx->SetTarget(old.Get());
    gClock.bmp = bmp;
    gClock.rc = RectF(pm->rc.left + box.left, pm->rc.top + box.top, pm->rc.left + box.right, pm->rc.top + box.bottom);
}

// ------------------------------------------------------------------------------------ graphics setup
static bool CreateDevice() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fl, ARRAYSIZE(fl), D3D11_SDK_VERSION, &gD3D, nullptr, nullptr);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, fl, ARRAYSIZE(fl), D3D11_SDK_VERSION, &gD3D, nullptr, nullptr);
    if (FAILED(hr)) return false;
    ComPtr<IDXGIDevice1> dx;
    gD3D.As(&dx);
    dx->SetMaximumFrameLatency(1);
    if (FAILED(gFactory->CreateDevice(dx.Get(), &gD2DDevice))) return false;
    if (FAILED(gD2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &gCtx))) return false;
    gCtx->SetDpi(96, 96);
    gCtx->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    gCtx->CreateSolidColorBrush(ColorF(1, 1, 1), &gBrush);
    InitShared();
    return true;
}
static bool CreateSwap() {
    gTarget.Reset();
    gCtx->SetTarget(nullptr);
    gSwap.Reset();
    if (gCanvasW <= 0 || gCanvasH <= 0) return false;
    ComPtr<IDXGIDevice> dx; gD3D.As(&dx);
    ComPtr<IDXGIAdapter> ad; dx->GetAdapter(&ad);
    ComPtr<IDXGIFactory2> f; ad->GetParent(IID_PPV_ARGS(&f));
    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = gCanvasW; d.Height = gCanvasH;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_NONE;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    HRESULT hr = f->CreateSwapChainForHwnd(gD3D.Get(), gWnd, &d, nullptr, nullptr, &gSwap);
    Log(L"swap chain %dx%d: 0x%08x", gCanvasW, gCanvasH, (unsigned)hr);
    if (FAILED(hr)) return false;
    f->MakeWindowAssociation(gWnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    ComPtr<IDXGISurface> surf;
    gSwap->GetBuffer(0, IID_PPV_ARGS(&surf));
    gCtx->CreateBitmapFromDxgiSurface(surf.Get(), BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                                      PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &gTarget);
    // Explorer composes this window over its own wallpaper using our alpha channel, so every pixel
    // must be written fully opaque (the back layer is opaque; premultiplied mode keeps alpha exact).
    return true;
}
static void ReleaseDevice() {
    delete gScene; gScene = nullptr;
    gMons.clear();
    gClock = ClockImage();
    ReleaseShared();
    gTarget.Reset(); gSwap.Reset(); gBrush.Reset(); gCtx.Reset(); gD2DDevice.Reset(); gD3D.Reset();
}

// ------------------------------------------------------------------------------------ rendering
static int gFps = 24;  // frames a second (24 on mains power, 12 on battery)
static long long gStatFrames = 0, gStatPixels = 0;  // for the once-a-minute power report
static std::vector<RECT> gPrevDirty;   // what changed in the previous frame (the other buffer lacks it)
static int gFullFrames = 2;            // full repaints still needed (one per buffer)

static RECT ToRect(const D2D1_RECT_F& r) {
    return { (LONG)std::floor(r.left), (LONG)std::floor(r.top), (LONG)std::ceil(r.right), (LONG)std::ceil(r.bottom) };
}
static bool Intersects(const RECT& a, const D2D1_RECT_F& b) { return !(b.right <= a.left || b.left >= a.right || b.bottom <= a.top || b.top >= a.bottom); }
static RECT Clip(RECT r) {
    r.left = std::max(r.left, 0L); r.top = std::max(r.top, 0L);
    r.right = std::min(r.right, (LONG)gCanvasW); r.bottom = std::min(r.bottom, (LONG)gCanvasH);
    return r;
}
static void MergeRects(std::vector<RECT>& v) {
    bool merged = true;
    while (merged) {
        merged = false;
        for (size_t i = 0; i < v.size() && !merged; i++)
            for (size_t j = i + 1; j < v.size() && !merged; j++) {
                RECT u; UnionRect(&u, &v[i], &v[j]);
                long long au = (long long)(u.right - u.left) * (u.bottom - u.top);
                long long ai = (long long)(v[i].right - v[i].left) * (v[i].bottom - v[i].top);
                long long aj = (long long)(v[j].right - v[j].left) * (v[j].bottom - v[j].top);
                RECT x;
                if (IntersectRect(&x, &v[i], &v[j]) || au - ai - aj <= 8192) {  // merge only when it wastes little: fewer draws, scattered stars stay small
                    v[i] = u; v.erase(v.begin() + j); merged = true;
                }
            }
    }
}

static void DrawRegion(const RECT& R, double t) {
    gCtx->SetTransform(Matrix3x2F::Identity());  // the clip is in canvas pixels
    gCtx->PushAxisAlignedClip(RectF((float)R.left, (float)R.top, (float)R.right, (float)R.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    for (auto& m : gMons) {
        RECT x;
        if (!IntersectRect(&x, &R, &m.rc)) continue;
        for (auto* e : gScene->els) {
            double et = e->still ? 0 : e->q;  // each piece is drawn at the time it was last updated
            if (e->layer >= 0) {
                auto& b = m.layer[e->layer];
                if (!b) continue;
                gCtx->SetTransform(Matrix3x2F::Identity());
                D2D1_POINT_2F o = Point2F(m.art._31, m.art._32);
                gCtx->DrawImage(b.Get(), o, RectF(0, 0, (float)m.bw, (float)m.bh), D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                continue;
            }
            if (!Intersects(R, Inflate(TransformRect(e->now, m.art), 2))) continue;
            e->Draw(et, m.art);
        }
    }
    if (gClock.bmp && Intersects(R, gClock.rc)) {
        gCtx->SetTransform(Matrix3x2F::Identity());
        gCtx->DrawImage(gClock.bmp.Get(), Point2F(gClock.rc.left, gClock.rc.top));
    }
    gCtx->PopAxisAlignedClip();
}

// Draws one frame. animate=false: only the clock (and any full repaint) is updated.
static bool Render(bool animate, bool clockChanged, const D2D1_RECT_F& oldClock) {
    if (!gSwap || !gScene) return true;
    double t = Now() - gT0;
    bool full = gFullFrames > 0;
    std::vector<RECT> cur;
    std::vector<Element*> moved;
    for (auto* e : gScene->els) {
        if (e->layer >= 0) continue;
        if (e->still) { if (IsEmpty(e->now)) e->now = e->Bounds(0); continue; }
        double r = e->slow ? e->rate : e->smooth ? gFps * 2.5 : gFps;
        double q = std::floor(t * r) / r;
        if (e->q < 0 || ((animate || full) && q != e->q)) { e->q = q; e->now = e->Bounds(q); moved.push_back(e); }
    }
    if (animate) {
        for (auto* e : moved) {
            D2D1_RECT_F b = e->now;
            D2D1_RECT_F u = Union(b, e->last);
            e->last = b;
            if (IsEmpty(u)) continue;
            for (auto& m : gMons) {
                RECT r = Clip(ToRect(Inflate(TransformRect(u, m.art), 2)));
                RECT x;
                if (IntersectRect(&x, &r, &m.rc)) cur.push_back(x);
            }
        }
    }
    if (clockChanged) {
        if (!IsEmpty(oldClock)) cur.push_back(Clip(ToRect(oldClock)));
        if (!IsEmpty(gClock.rc)) cur.push_back(Clip(ToRect(gClock.rc)));
    }
    if (full && !animate) for (auto* e : moved) e->last = e->now;  // drawn where they are now
    if (!full && cur.empty()) return true;  // nothing moved this tick: no drawing, no present

    std::vector<RECT> draw = cur;
    if (!full) draw.insert(draw.end(), gPrevDirty.begin(), gPrevDirty.end());
    else draw = { RECT{ 0, 0, gCanvasW, gCanvasH } };
    draw.erase(std::remove_if(draw.begin(), draw.end(), [](const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }), draw.end());
    MergeRects(draw);

    gCtx->SetTarget(gTarget.Get());
    gCtx->BeginDraw();
    gCtx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (full) gCtx->Clear(ColorF(0, 0, 0, 1));
    for (auto& R : draw) DrawRegion(R, t);
    HRESULT hr = gCtx->EndDraw();
    gCtx->SetTarget(nullptr);
    if (hr == D2DERR_RECREATE_TARGET) { Log(L"draw: recreate target"); return false; }

    DXGI_PRESENT_PARAMETERS pp = {};
    if (!full) { pp.DirtyRectsCount = (UINT)draw.size(); pp.pDirtyRects = draw.data(); }
    hr = gSwap->Present1(1, 0, &pp);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) { Log(L"present: device lost 0x%08x", (unsigned)hr); return false; }
    if (FAILED(hr)) Log(L"present: 0x%08x", (unsigned)hr);
    if (full) gFullFrames--;
    gStatFrames++;
    for (auto& r : draw) gStatPixels += (long long)(r.right - r.left) * (r.bottom - r.top);
    gPrevDirty = cur;
    return true;
}

// ------------------------------------------------------------------------------------ desktop attachment
static HWND FindDesktopHost() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;
    // Ask Explorer for the layer that sits between the wallpaper and the icons.
    DWORD_PTR res;
    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &res);
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &res);
    // Windows 11 24H2 and later: it is a child of Progman
    HWND w = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
    if (w) return w;
    // Earlier: the top-level WorkerW after the one that holds the icons
    HWND found = nullptr;
    EnumWindows([](HWND top, LPARAM lp) -> BOOL {
        if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) {
            *(HWND*)lp = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&found);
    return found ? found : progman;
}

static SceneId gWanted = NONE;
static bool gAttached = false;

static void Layout() {
    RECT pc; GetClientRect(gParent, &pc);
    gCanvasW = pc.right - pc.left;
    gCanvasH = pc.bottom - pc.top;
    SetWindowPos(gWnd, nullptr, 0, 0, gCanvasW, gCanvasH, SWP_NOZORDER | SWP_NOACTIVATE);
    gMons.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonEnum, 0);
}

static void Detach() {
    if (gWnd) DestroyWindow(gWnd);
    gWnd = nullptr;
    gAttached = false;
    gSwap.Reset(); gTarget.Reset();
}

static LRESULT CALLBACK CanvasProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) { ValidateRect(h, nullptr); gFullFrames = 2; return 0; }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

static bool Attach() {
    Detach();
    gParent = FindDesktopHost();
    if (!gParent) return false;
    wchar_t pc[64] = {}; GetClassNameW(gParent, pc, 64);
    Log(L"attaching to %s %p", pc, (void*)gParent);
    gWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, L"LembasDesktopCanvas", L"Lembas Desktop", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!gWnd) return false;
    SetWindowLongPtrW(gWnd, GWL_STYLE, WS_CHILD | WS_CLIPSIBLINGS);
    SetParent(gWnd, gParent);
    Layout();
    ShowWindow(gWnd, SW_SHOWNA);
    gAttached = true;
    return true;
}

static void Rebuild() {  // device, swap chain, scene, layers and clock for the current state
    if (!gD3D && !CreateDevice()) return;
    if (!gAttached && !Attach()) return;
    Layout();
    if (!CreateSwap()) return;
    delete gScene;
    gScene = BuildScene(gWanted);
    LoadLayers();
    for (auto& m : gMons) Log(L"monitor %ld,%ld %ldx%ld%s scene %s layers %d %d %d", m.rc.left, m.rc.top, m.rc.right - m.rc.left, m.rc.bottom - m.rc.top,
                              m.primary ? L" (main)" : L"", kSceneKey[gWanted], (int)!!m.layer[0], (int)!!m.layer[1], (int)!!m.layer[2]);
    BuildClock(gWanted);
    gClockMinute = -1;
    gPrevDirty.clear();
    gFullFrames = 2;
}

static void Stop() {
    Detach();
    ReleaseDevice();
    // let Explorer repaint its own wallpaper where we were
    wchar_t wall[MAX_PATH * 2] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH * 2, wall, 0);
    if (wall[0]) SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, wall, 0);
}

// ------------------------------------------------------------------------------------ when to move
static bool gLocked = false, gDisplayOff = false, gCovered = false, gStill = false;

static BOOL CALLBACK CoverEnum(HWND h, LPARAM lp) {
    auto* covered = (std::vector<bool>*)lp;
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (ex & (WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return TRUE;
    int cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    if (cloaked) return TRUE;
    wchar_t cls[64]; GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd")) return TRUE;
    RECT r;
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(h, &r);
    for (size_t i = 0; i < gMons.size(); i++) {
        const RECT& wk = gMons[i].work;
        if (r.left <= wk.left + 2 && r.top <= wk.top + 2 && r.right >= wk.right - 2 && r.bottom >= wk.bottom - 2) (*covered)[i] = true;
    }
    return TRUE;
}
static void CheckConditions() {
    std::vector<bool> covered(gMons.size(), false);
    if (!gMons.empty()) EnumWindows(CoverEnum, (LPARAM)&covered);
    gCovered = !gMons.empty() && std::all_of(covered.begin(), covered.end(), [](bool b) { return b; });
    SYSTEM_POWER_STATUS ps;
    bool saver = false, battery = false;
    if (GetSystemPowerStatus(&ps)) { saver = (ps.SystemStatusFlag & 1) != 0; battery = ps.ACLineStatus == 0; }
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    gStill = saver || !anim;
    gFps = battery ? 12 : 24;
}
static bool Visible() { return gAttached && gScene && !gLocked && !gDisplayOff && !gCovered; }

// ------------------------------------------------------------------------------------ the tray menu
// A Lembas icon by the clock: choose the look, the clock style, the moving scene and mealtimes,
// or preview, update and uninstall, without opening anything.
#define WM_LEMBAS_TRAY (WM_APP + 7)
static bool gReattach = false, gReload = false;
static NOTIFYICONDATAW gTray{};
static std::wstring LembasDir() {
    wchar_t base[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    return std::wstring(base) + L"\\Lembas";
}
static void TrayAdd() {
    gTray = {};
    gTray.cbSize = sizeof gTray;
    gTray.hWnd = gMsgWnd;
    gTray.uID = 1;
    gTray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    gTray.uCallbackMessage = WM_LEMBAS_TRAY;
    std::wstring ico = LembasDir() + L"\\Icons\\favorites.ico";
    gTray.hIcon = (HICON)LoadImageW(nullptr, ico.c_str(), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE);
    if (!gTray.hIcon) gTray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(gTray.szTip, L"Lembas");
    Shell_NotifyIconW(NIM_ADD, &gTray);
}
static void TrayRemove() { if (gTray.cbSize) Shell_NotifyIconW(NIM_DELETE, &gTray); }

// Set one flat value in lembas.json (true/false/"text"), keeping everything else as it is.
static void SetConfig(const wchar_t* key, const std::wstring& value) {
    std::wstring path = LembasDir() + L"\\lembas.json", j = ReadAll(path);
    if (j.empty()) return;
    std::wstring k = std::wstring(L"\"") + key + L"\"";
    size_t p = j.find(k);
    if (p != std::wstring::npos) {
        size_t c = j.find(L':', p + k.size());
        size_t e = c + 1;
        while (e < j.size() && iswspace(j[e])) e++;
        size_t end = e;
        if (end < j.size() && j[end] == L'"') { end = j.find(L'"', end + 1); end = end == std::wstring::npos ? j.size() : end + 1; }
        else while (end < j.size() && j[end] != L',' && j[end] != L'}' && j[end] != L'\r' && j[end] != L'\n') end++;
        j.replace(e, end - e, value);
    } else {
        size_t close = j.rfind(L'}');
        if (close == std::wstring::npos) return;
        size_t last = close;
        while (last > 0 && iswspace(j[last - 1])) last--;
        j.insert(last, std::wstring(L",\r\n    ") + k + L": " + value);
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, j.c_str(), (int)j.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, j.c_str(), (int)j.size(), &out[0], n, nullptr, nullptr);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(h, out.data(), (DWORD)out.size(), &w, nullptr);
    CloseHandle(h);
}

static void RunScript(const wchar_t* script, const std::wstring& args, bool visible) {
    wchar_t ps[MAX_PATH]; GetSystemDirectoryW(ps, MAX_PATH);
    std::wstring exe = std::wstring(ps) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::wstring a = L"-NoProfile -ExecutionPolicy Bypass " + std::wstring(visible ? L"" : L"-WindowStyle Hidden ") +
        L"-File \"" + LembasDir() + L"\\Tools\\" + script + L"\" " + args;
    ShellExecuteW(nullptr, L"open", exe.c_str(), a.c_str(), nullptr, visible ? SW_SHOWNORMAL : SW_HIDE);
}

enum { IDM_ADAPTIVE = 100, IDM_SHIRE, IDM_REDBOOK, IDM_STARLIT, IDM_ELVEN,
       IDM_MODERN = 200, IDM_CLASSIC, IDM_NOCLOCK, IDM_SCENE, IDM_MEALS,
       IDM_PREVIEW = 300, IDM_UPDATE, IDM_UNINSTALL };

static void TrayMenu() {
    HMENU m = CreatePopupMenu(), clock = CreatePopupMenu();
    auto check = [](bool on) { return on ? MF_CHECKED : MF_UNCHECKED; };
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, L"Lembas");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | check(gSet.look == L"adaptive"), IDM_ADAPTIVE, L"Follow the sun");
    AppendMenuW(m, MF_STRING | check(gSet.look == L"shire"), IDM_SHIRE, L"Shire Morning");
    AppendMenuW(m, MF_STRING | check(gSet.look == L"redbook"), IDM_REDBOOK, L"Red Book");
    AppendMenuW(m, MF_STRING | check(gSet.look == L"starlit"), IDM_STARLIT, L"Starlit Night");
    AppendMenuW(m, MF_STRING | check(gSet.look == L"elven"), IDM_ELVEN, L"Elven Silver");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(clock, MF_STRING | check(gSet.clock && gSet.modern), IDM_MODERN, L"Modern");
    AppendMenuW(clock, MF_STRING | check(gSet.clock && !gSet.modern), IDM_CLASSIC, L"Classic");
    AppendMenuW(clock, MF_STRING | check(!gSet.clock), IDM_NOCLOCK, L"Off");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)clock, L"Desktop clock");
    AppendMenuW(m, MF_STRING | check(gSet.enabled), IDM_SCENE, L"Moving desktop scene");
    AppendMenuW(m, MF_STRING | check(gSet.meals), IDM_MEALS, L"Hobbit mealtimes");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_PREVIEW, L"Preview the four looks");
    AppendMenuW(m, MF_STRING, IDM_UPDATE, L"Check for updates");
    AppendMenuW(m, MF_STRING, IDM_UNINSTALL, L"Uninstall Lembas...");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(gMsgWnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, gMsgWnd, nullptr);
    DestroyMenu(m);   // also destroys the clock submenu
    PostMessageW(gMsgWnd, WM_NULL, 0, 0);
    static const wchar_t* looks[] = { L"adaptive", L"shire", L"redbook", L"starlit", L"elven" };
    switch (cmd) {
    case IDM_ADAPTIVE: case IDM_SHIRE: case IDM_REDBOOK: case IDM_STARLIT: case IDM_ELVEN:
        gSet.look = looks[cmd - IDM_ADAPTIVE];
        RunScript(L"Change-Look.ps1", std::wstring(L"-Look ") + looks[cmd - IDM_ADAPTIVE], false);
        break;
    case IDM_MODERN: SetConfig(L"desktopClock", L"true"); SetConfig(L"desktopClockStyle", L"\"modern\""); break;
    case IDM_CLASSIC: SetConfig(L"desktopClock", L"true"); SetConfig(L"desktopClockStyle", L"\"classic\""); break;
    case IDM_NOCLOCK: SetConfig(L"desktopClock", L"false"); break;
    case IDM_SCENE: SetConfig(L"livingDesktop", gSet.enabled ? L"false" : L"true"); break;
    case IDM_MEALS: SetConfig(L"meals", gSet.meals ? L"false" : L"true"); break;
    case IDM_PREVIEW: RunScript(L"Preview-Lembas.ps1", L"", true); break;
    case IDM_UPDATE: RunScript(L"Update-Lembas.ps1", L"", true); break;
    case IDM_UNINSTALL: RunScript(L"Uninstall-Lembas.ps1", L"", true); break;
    }
    if (cmd >= IDM_MODERN && cmd < IDM_PREVIEW) {   // settings changed: apply them now
        LoadSettings();
        gClockMinute = -1;
        gReload = true;
    }
}

// ------------------------------------------------------------------------------------ messages
static UINT gTaskbarCreated;
static const GUID kDisplayState = { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };  // GUID_CONSOLE_DISPLAY_STATE

static LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == gTaskbarCreated) { gReattach = true; TrayAdd(); return 0; }
    switch (m) {
    case WM_LEMBAS_TRAY:
        if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_LBUTTONUP || LOWORD(l) == WM_CONTEXTMENU) TrayMenu();
        return 0;
    case WM_SETTINGCHANGE:
        if (w == SPI_SETDESKWALLPAPER || w == SPI_SETCLIENTAREAANIMATION || w == SPI_SETWORKAREA) gReload = true;
        if (l && !wcscmp((const wchar_t*)l, L"intl")) gClockMinute = -1;
        return 0;
    case WM_DISPLAYCHANGE: case WM_DPICHANGED:
        gReattach = true; return 0;
    case WM_TIMECHANGE:
        gClockMinute = -1; return 0;
    case WM_WTSSESSION_CHANGE:
        if (w == WTS_SESSION_LOCK || w == WTS_CONSOLE_DISCONNECT || w == WTS_REMOTE_DISCONNECT) gLocked = true;
        if (w == WTS_SESSION_UNLOCK || w == WTS_CONSOLE_CONNECT || w == WTS_REMOTE_CONNECT) { gLocked = false; gFullFrames = 2; }
        return 0;
    case WM_POWERBROADCAST:
        if (w == PBT_POWERSETTINGCHANGE) {
            auto* p = (POWERBROADCAST_SETTING*)l;
            if (IsEqualGUID(p->PowerSetting, kDisplayState)) { gDisplayOff = *(DWORD*)p->Data == 0; if (!gDisplayOff) gFullFrames = 2; }
        }
        if (w == PBT_APMRESUMEAUTOMATIC) { gClockMinute = -1; gReattach = true; }
        return TRUE;
    case WM_ENDSESSION:
        if (w) { Log(L"Windows is signing out"); TrayRemove(); Stop(); }
        return 0;
    case WM_CLOSE:
        Log(L"asked to stop"); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ------------------------------------------------------------------------------------ snapshot (for checking)
// LembasDesktop.exe --snapshot <folder> [seconds] [width] [height]: renders each look to a PNG.
static bool SavePng(ID2D1Bitmap1* src, UINT w, UINT h, const std::wstring& file) {
    ComPtr<ID2D1Bitmap1> cpu;
    if (FAILED(gCtx->CreateBitmap(SizeU(w, h), nullptr, 0, BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                  PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &cpu))) return false;
    cpu->CopyFromBitmap(nullptr, src, nullptr);
    D2D1_MAPPED_RECT mr;
    if (FAILED(cpu->Map(D2D1_MAP_OPTIONS_READ, &mr))) return false;
    ComPtr<IWICStream> st; ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICBitmapFrameEncode> fr;
    gWic->CreateStream(&st);
    st->InitializeFromFilename(file.c_str(), GENERIC_WRITE);
    gWic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(st.Get(), WICBitmapEncoderNoCache);
    enc->CreateNewFrame(&fr, nullptr);
    fr->Initialize(nullptr);
    fr->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    fr->SetPixelFormat(&fmt);
    fr->WritePixels(h, mr.pitch, mr.pitch * h, mr.bits);
    fr->Commit(); enc->Commit();
    cpu->Unmap();
    return true;
}
static int Snapshot(const std::wstring& dir, double t, int W, int H) {
    if (!CreateDevice()) return 2;
    LoadSettings();
    gMons.clear();
    Mon m{}; m.primary = true; SetupMon(m, RECT{ 0, 0, W, H }); gMons.push_back(m);
    gCanvasW = W; gCanvasH = H;
    ComPtr<ID2D1Bitmap1> target;
    gCtx->CreateBitmap(SizeU(W, H), nullptr, 0, BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &target);
    for (int i = 0; i < 4; i++) {
        delete gScene;
        gScene = BuildScene((SceneId)i);
        LoadLayers();
        BuildClock((SceneId)i);
        gCtx->SetTarget(target.Get());
        gCtx->BeginDraw();
        gCtx->Clear(ColorF(1, 0, 1));
        for (auto* e : gScene->els) if (e->layer < 0) { e->q = t; e->now = e->Bounds(t); }
        DrawRegion(RECT{ 0, 0, W, H }, t);
        HRESULT hr = gCtx->EndDraw();
        gCtx->SetTarget(nullptr);
        Log(L"snapshot %s: draw 0x%08x, layers %d %d %d", kSceneKey[i], (unsigned)hr, (int)!!gMons[0].layer[0], (int)!!gMons[0].layer[1], (int)!!gMons[0].layer[2]);
        SavePng(target.Get(), W, H, dir + L"\\" + kSceneKey[i] + L".png");
    }
    return 0;
}

// ------------------------------------------------------------------------------------ main
static LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep) {
    HMODULE self = GetModuleHandleW(nullptr);
    Log(L"stopped by an error 0x%08x at +0x%llx", (unsigned)ep->ExceptionRecord->ExceptionCode,
        (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress - (uintptr_t)self));
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    if (cmd && wcsstr(cmd, L"--quit")) {  // ask the running copy to stop, and wait until it has
        HWND other = FindWindowW(L"LembasDesktopMsg", nullptr);
        if (!other) return 0;
        DWORD pid = 0; GetWindowThreadProcessId(other, &pid);
        HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        PostMessageW(other, WM_CLOSE, 0, 0);
        if (p) {
            if (WaitForSingleObject(p, 15000) == WAIT_TIMEOUT) TerminateProcess(p, 1);
            CloseHandle(p);
        }
        return 0;
    }
    bool snapshot = cmd && wcsstr(cmd, L"--snapshot");
    HANDLE mutex = snapshot ? nullptr : CreateMutexW(nullptr, TRUE, L"Local\\LembasDesktop");
    if (!snapshot && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    typedef BOOL(WINAPI * DpiFn)(DPI_AWARENESS_CONTEXT);
    if (auto f = (DpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")) f(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    // Tell Windows this is background work (efficiency mode on supported PCs)
    PROCESS_POWER_THROTTLING_STATE pt = { PROCESS_POWER_THROTTLING_CURRENT_VERSION, PROCESS_POWER_THROTTLING_EXECUTION_SPEED, PROCESS_POWER_THROTTLING_EXECUTION_SPEED };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    QueryPerformanceFrequency(&gFreq);
    gT0 = Now();
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    gDir = exe; gDir = gDir.substr(0, gDir.find_last_of(L'\\'));

    D2D1_FACTORY_OPTIONS fo = {};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, (void**)gFactory.GetAddressOf()))) return 1;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)gDWrite.GetAddressOf());
    gDWrite->GetSystemFontCollection(&gFonts, TRUE);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&gWic));

    if (snapshot) {
        int argc; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::wstring dir = argc > 2 ? argv[2] : gDir;
        double t = argc > 3 ? _wtof(argv[3]) : 5;
        int W = argc > 4 ? _wtoi(argv[4]) : 1920, H = argc > 5 ? _wtoi(argv[5]) : 1200;
        return Snapshot(dir, t, W, H);
    }
    for (int i = 0; i < 4; i++) if (cmd && wcsstr(cmd, (std::wstring(L"--look ") + kSceneKey[i]).c_str())) gForced = (SceneId)i;
    Log(L"Lembas Desktop starting (%s), pid %lu", gDir.c_str(), GetCurrentProcessId());
    SetUnhandledExceptionFilter(OnCrash);

    WNDCLASSW wc = {};
    wc.hInstance = inst;
    wc.lpfnWndProc = CanvasProc;
    wc.lpszClassName = L"LembasDesktopCanvas";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    wc.lpfnWndProc = MsgProc;
    wc.lpszClassName = L"LembasDesktopMsg";
    RegisterClassW(&wc);
    gMsgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"LembasDesktopMsg", L"Lembas Desktop", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    gTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WTSRegisterSessionNotification(gMsgWnd, NOTIFY_FOR_THIS_SESSION);
    RegisterPowerSettingNotification(gMsgWnd, &kDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    TrayAdd();

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    LoadSettings();
    gReload = true;
    double nextFrame = 0, nextCheck = 0;
    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;
        double now = Now();

        if (gReload || gReattach) {
            LoadSettings();
            SceneId want = gSet.enabled ? SceneFromWallpaper() : NONE;
            bool changed = want != gWanted || gReattach || (gWanted != NONE && !gAttached);
            gWanted = want;
            Log(L"wallpaper look: %s", gWanted == NONE ? L"(not Lembas)" : kSceneKey[gWanted]);
            if (gWanted == NONE) { if (gAttached || gD3D) Stop(); }
            else if (changed) { if (gReattach) Detach(); Rebuild(); }
            gReload = gReattach = false;
            nextCheck = 0;
        }
        if (gAttached && (!IsWindow(gParent) || !IsWindow(gWnd))) { gReattach = true; continue; }

        static double nextReport = now + 60;
        if (now >= nextReport) {
            FILETIME a, b, k, u; GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
            static double lastCpu = 0;
            double cpu = ((double)(((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (double)(((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 1e4;
            double area = gCanvasW > 0 ? 100.0 * gStatPixels / ((double)gCanvasW * gCanvasH * std::max(1LL, gStatFrames)) : 0;
            Log(L"last minute: %lld frames, %.1f%% of the screen redrawn per frame, CPU %.0f ms (%.2f%%)%s%s%s%s", gStatFrames, area, cpu - lastCpu, (cpu - lastCpu) / 600.0,
                gCovered ? L", covered" : L"", gLocked ? L", locked" : L"", gDisplayOff ? L", screen off" : L"", gStill ? L", still" : L"");
            lastCpu = cpu; gStatFrames = gStatPixels = 0;
            nextReport = now + 60;
        }
        if (now >= nextCheck) {
            static int tries = 0;
            if (gWanted != NONE && !gAttached && ++tries % 5 == 0) gReattach = true;  // Explorer not ready yet
            bool wasVisible = Visible();
            CheckConditions();
            if (!wasVisible && Visible()) gFullFrames = 2;
            nextCheck = now + 1.0;
        }

        CheckSun();
        bool visible = Visible();
        bool animate = visible && !gStill;
        if (visible) {
            SYSTEMTIME st; GetLocalTime(&st);
            int minute = st.wDay * 1440 + st.wHour * 60 + st.wMinute;
            bool clockChanged = false;
            D2D1_RECT_F oldClock = gClock.rc;
            if (minute != gClockMinute) {
                if (gClockMinute >= 0 && minute / 1440 != gClockMinute / 1440) gFullFrames = 2;  // new day: new moon phase
                BuildClock(gWanted); gClockMinute = minute; clockChanged = true;
            }
            if (clockChanged || gFullFrames > 0 || (animate && now >= nextFrame)) {
                if (!Render(animate, clockChanged, oldClock)) { ReleaseDevice(); gAttached = false; gReattach = true; Detach(); continue; }
                bool smooth = false;
                for (auto* e : gScene->els) smooth |= e->smooth;
                double period = 1.0 / (smooth ? gFps * 2.5 : gFps);
                nextFrame = std::max(nextFrame + period, now + period * .5);
                if (nextFrame < now) nextFrame = now + period;
            }
        }

        // sleep until the next frame, the next minute, or the next check, whichever is first
        double wake = nextCheck;
        if (animate) wake = std::min(wake, nextFrame);
        if (visible || gSet.adaptive) {  // the clock, and the minute a look changes
            SYSTEMTIME st; GetLocalTime(&st);
            wake = std::min(wake, now + (60 - st.wSecond) - st.wMilliseconds / 1000.0 + .02);
        }
        double dt = std::max(0.0, wake - Now());
        LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(dt * 1e7);
        if (due.QuadPart == 0) due.QuadPart = -1;
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        MsgWaitForMultipleObjectsEx(1, &timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    TrayRemove();
    Stop();
    Log(L"stopped");
    if (mutex) CloseHandle(mutex);
    CoUninitialize();
    return 0;
}
