// Lembas Desktop - the Lembas scene, gently animated, behind your desktop icons, with the
// time, the Shire date, the Quenya hour and the current hobbit mealtime.
//
// One small native program: Win32 + Direct3D 11 + Direct2D + DirectWrite, nothing else.
// Battery: the still parts of each scene are pre-rendered images; only the small moving
// pieces are redrawn, and only the parts of the screen they touch are presented (flip-model
// dirty rectangles). It draws 24 frames a second on mains power, 12 on battery, and stops
// completely when the desktop is covered, the screen is off or locked, or Battery saver is on
// (the clock still turns over each minute).
//
// It follows whichever Lembas wallpaper Lembas Sun has set, so the four looks switch with it.
// If the wallpaper isn't a Lembas one, it steps aside.
//
//   LembasDesktop.exe          start (does nothing if it is already running)
//   LembasDesktop.exe --quit   stop the running copy
//   LembasDesktop.exe --reload the running copy re-reads lembas.json (after a sync)

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
#include <oleacc.h>
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
#include <atomic>
#include <map>
#include <set>
#include <mmdeviceapi.h>
#include "Words.h"

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
static ComPtr<IDWriteFontCollection> gFonts;   // the system's fonts (the kit installs the Lembas ones for you)
static ComPtr<IWICImagingFactory> gWic;
static ComPtr<ID3D11Device> gD3D;
static ComPtr<ID2D1Device> gD2DDevice;
static ComPtr<ID2D1DeviceContext> gCtx;
static ComPtr<IDXGISwapChain1> gSwap;
static ComPtr<ID2D1Bitmap1> gTarget;
static ComPtr<ID2D1SolidColorBrush> gBrush;
static HWND gWnd, gMsgWnd, gParent;
static bool gSaver = false;   // running as the screensaver (Lembas.scr /s): our own full-screen window
static HWND gSaverWnd;
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

// ------------------------------------------------------------------------------------ the real sky
// The bright stars and the planets where they really are, and the Moon's true height, phase and
// distance (the same low-precision sums as the phone's Heavens.kt: Schlyter's Moon, JPL's planets).
namespace Heavens {
static const double D2R = 3.14159265358979 / 180;
static double rev(double x) { return x - std::floor(x / 360.0) * 360.0; }
static double sind(double x) { return std::sin(x * D2R); }
static double cosd(double x) { return std::cos(x * D2R); }
static double DayNow() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    double unix = ((double)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000.0) / 1e7;
    return unix / 86400.0 + 2440587.5 - 2451543.5;
}
struct AltAz { double alt, az; };
static double Lst(double d, double lon) { double ms = rev(356.0470 + 0.9856002585 * d), ws = 282.9404 + 4.70935e-5 * d, ut = (d - std::floor(d)) * 24; return rev(ms + ws + 180 + ut * 15.04107 + lon); }
static AltAz ToAltAz(double ra, double dec, double d, double lat, double lon) {
    double ha = rev(Lst(d, lon) - ra), x = cosd(ha) * cosd(dec), y = sind(ha) * cosd(dec), z = sind(dec);
    double xh = x * sind(lat) - z * cosd(lat), zh = x * cosd(lat) + z * sind(lat);
    return { std::asin(std::max(-1.0, std::min(1.0, zh))) / D2R, rev(std::atan2(y, xh) / D2R + 180) };
}
static void EclToEq(double lon, double lat, double d, double& ra, double& dec) {
    double e = 23.4393 - 3.563e-7 * d, x = cosd(lon) * cosd(lat), y = sind(lon) * cosd(lat), z = sind(lat);
    double ye = y * cosd(e) - z * sind(e), ze = y * sind(e) + z * cosd(e);
    ra = rev(std::atan2(ye, x) / D2R); dec = std::atan2(ze, std::hypot(x, ye)) / D2R;
}
static double SunLon(double d) {
    double w = 282.9404 + 4.70935e-5 * d, e = 0.016709 - 1.151e-9 * d, m = rev(356.0470 + 0.9856002585 * d);
    double ea = m + e / D2R * sind(m) * (1 + e * cosd(m)), xv = cosd(ea) - e, yv = std::sqrt(1 - e * e) * sind(ea);
    return rev(std::atan2(yv, xv) / D2R + w);
}
struct MoonNow { AltAz pos; double phase, km, eclipse; bool Full() const { return std::fabs(phase - .5) < .05; } bool Super() const { return Full() && km < 360000; } };
static MoonNow Moon(double lat, double lon) {
    double d = DayNow();
    double n = rev(125.1228 - 0.0529538083 * d), i = 5.1454, w = rev(318.0634 + 0.1643573223 * d), a = 60.2666, e = 0.0549, mm = rev(115.3654 + 13.0649929509 * d);
    double ea = mm + e / D2R * sind(mm) * (1 + e * cosd(mm));
    for (int k = 0; k < 3; k++) ea -= (ea - e / D2R * sind(ea) - mm) / (1 - e * cosd(ea));
    double xv = a * (cosd(ea) - e), yv = a * std::sqrt(1 - e * e) * sind(ea), v = std::atan2(yv, xv) / D2R, r = std::hypot(xv, yv);
    double xh = r * (cosd(n) * cosd(v + w) - sind(n) * sind(v + w) * cosd(i)), yh = r * (sind(n) * cosd(v + w) + cosd(n) * sind(v + w) * cosd(i)), zh = r * sind(v + w) * sind(i);
    double lo = rev(std::atan2(yh, xh) / D2R), la = std::atan2(zh, std::hypot(xh, yh)) / D2R;
    double ms0 = rev(356.0470 + 0.9856002585 * d), ls = rev(ms0 + 282.9404 + 4.70935e-5 * d), lm = rev(mm + w + n), dd = rev(lm - ls), f = rev(lm - n);
    lo += -1.274 * sind(mm - 2 * dd) + 0.658 * sind(2 * dd) - 0.186 * sind(ms0) - 0.059 * sind(2 * mm - 2 * dd) - 0.057 * sind(mm - 2 * dd + ms0) + 0.053 * sind(mm + 2 * dd)
        + 0.046 * sind(2 * dd - ms0) + 0.041 * sind(mm - ms0) - 0.035 * sind(dd) - 0.031 * sind(mm + ms0) - 0.015 * sind(2 * f - 2 * dd) + 0.011 * sind(mm - 4 * dd);
    la += -0.173 * sind(f - 2 * dd) - 0.055 * sind(mm - f - 2 * dd) - 0.046 * sind(mm + f - 2 * dd) + 0.033 * sind(f + 2 * dd) + 0.017 * sind(2 * mm + f);
    r += -0.58 * cosd(mm - 2 * dd) - 0.46 * cosd(2 * dd);
    double ra, dec; EclToEq(lo, la, d, ra, dec);
    AltAz p = ToAltAz(ra, dec, d, lat, lon); p.alt -= 0.95 * cosd(p.alt);
    double sl = SunLon(d), dl = rev(lo - (sl + 180) + 180) - 180, sep = std::hypot(dl * cosd(la), la);
    return { p, rev(lo - sl) / 360.0, r * 6371.0, std::max(0.0, std::min(1.0, (0.96 - sep) / 0.52)) };
}
struct Orbit { double a, e, i, l, p, n, da, de, di, dl, dp, dn; };
static const Orbit kEarth = { 1.00000261, 0.01671123, -0.00001531, 100.46457166, 102.93768193, 0.0, 0.00000562, -0.00004392, -0.01294668, 35999.37244981, 0.32327364, 0.0 };
struct PlanetDef { const wchar_t* name; uint32_t col; float size; Orbit o; };
static const PlanetDef kPlanets[] = {
    { L"Mercury", 0xE8DCC8, 1.6f, { 0.38709927, 0.20563593, 7.00497902, 252.25032350, 77.45779628, 48.33076593, 0.00000037, 0.00001906, -0.00594749, 149472.67411175, 0.16047689, -0.12534081 } },
    { L"Venus", 0xFFF6E0, 3.2f, { 0.72333566, 0.00677672, 3.39467605, 181.97909950, 131.60246718, 76.67984255, 0.00000390, -0.00004107, -0.00078890, 58517.81538729, 0.00268329, -0.27769418 } },
    { L"Mars", 0xFFB08A, 2.0f, { 1.52371034, 0.09339410, 1.84969142, -4.55343205, -23.94362959, 49.55953891, 0.00001847, 0.00007882, -0.00813131, 19140.30268499, 0.44441088, -0.29257343 } },
    { L"Jupiter", 0xFFF0D8, 2.8f, { 5.20288700, 0.04838624, 1.30439695, 34.39644051, 14.72847983, 100.47390909, -0.00011607, -0.00013253, -0.00183714, 3034.74612775, 0.21252668, 0.20469106 } },
    { L"Saturn", 0xF2E2B0, 2.2f, { 9.53667594, 0.05386179, 2.48599187, 49.95424423, 92.59887831, 113.66242448, -0.00125060, -0.00050991, 0.00193609, 1222.49362201, -0.41897216, -0.28867794 } },
};
static void Helio(const Orbit& o, double t, double out[3]) {
    double a = o.a + o.da * t, e = o.e + o.de * t, i = o.i + o.di * t, l = o.l + o.dl * t, p = o.p + o.dp * t, n = o.n + o.dn * t;
    double m = rev(l - p), w = p - n, ea = m + e / D2R * sind(m);
    for (int k = 0; k < 5; k++) ea -= (ea - e / D2R * sind(ea) - m) / (1 - e * cosd(ea));
    double xp = a * (cosd(ea) - e), yp = a * std::sqrt(1 - e * e) * sind(ea);
    out[0] = (cosd(w) * cosd(n) - sind(w) * sind(n) * cosd(i)) * xp + (-sind(w) * cosd(n) - cosd(w) * sind(n) * cosd(i)) * yp;
    out[1] = (cosd(w) * sind(n) + sind(w) * cosd(n) * cosd(i)) * xp + (-sind(w) * sind(n) + cosd(w) * cosd(n) * cosd(i)) * yp;
    out[2] = sind(w) * sind(i) * xp + cosd(w) * sind(i) * yp;
}
static AltAz Planet(int k, double lat, double lon) {
    double d = DayNow(), t = (d + 2451543.5 - 2451545.0) / 36525.0, e[3], h[3];
    Helio(kEarth, t, e); Helio(kPlanets[k].o, t, h);
    double x = h[0] - e[0], y = h[1] - e[1], z = h[2] - e[2], ra, dec;
    EclToEq(rev(std::atan2(y, x) / D2R), std::atan2(z, std::hypot(x, y)) / D2R, d, ra, dec);
    return ToAltAz(ra, dec, d, lat, lon);
}
struct StarDef { double ra, dec, mag; uint32_t col; };
static const StarDef kStars[] = {
    { 12.443, -63.10, 0.8, 0xD6E2FF }, { 12.795, -59.69, 1.3, 0xD6E2FF }, { 12.519, -57.11, 1.6, 0xFFB090 }, { 12.252, -58.75, 2.8, 0xF4F2EE }, { 14.660, -60.83, -0.3, 0xFFD2A6 }, { 14.064, -60.37, 0.6, 0xD6E2FF },
    { 5.919, 7.41, 0.5, 0xFFB090 }, { 5.242, -8.20, 0.1, 0xD6E2FF }, { 5.419, 6.35, 1.6, 0xD6E2FF }, { 5.679, -1.94, 1.8, 0xD6E2FF }, { 5.604, -1.20, 1.7, 0xD6E2FF }, { 5.533, -0.30, 2.2, 0xD6E2FF }, { 5.796, -9.67, 2.1, 0xD6E2FF },
    { 11.062, 61.75, 1.8, 0xFFD2A6 }, { 11.031, 56.38, 2.4, 0xF4F2EE }, { 11.897, 53.69, 2.4, 0xF4F2EE }, { 12.257, 57.03, 3.3, 0xF4F2EE }, { 12.900, 55.96, 1.8, 0xF4F2EE }, { 13.399, 54.93, 2.2, 0xF4F2EE }, { 13.792, 49.31, 1.9, 0xD6E2FF },
    { 0.675, 56.54, 2.2, 0xFFD2A6 }, { 0.153, 59.15, 2.3, 0xF4F2EE }, { 0.945, 60.72, 2.2, 0xD6E2FF }, { 1.430, 60.24, 2.7, 0xF4F2EE }, { 1.907, 63.67, 3.4, 0xD6E2FF }, { 2.530, 89.26, 2.0, 0xF4F2EE },
    { 16.490, -26.43, 1.0, 0xFFB090 }, { 17.560, -37.10, 1.6, 0xD6E2FF }, { 17.622, -43.00, 1.9, 0xF4F2EE }, { 16.006, -22.62, 2.3, 0xD6E2FF }, { 16.090, -19.81, 2.6, 0xD6E2FF },
    { 6.752, -16.72, -1.5, 0xD6E2FF }, { 6.399, -52.70, -0.7, 0xF4F2EE }, { 14.261, 19.18, -0.1, 0xFFD2A6 }, { 18.616, 38.78, 0.0, 0xD6E2FF }, { 5.278, 46.00, 0.1, 0xFFD2A6 }, { 7.655, 5.22, 0.4, 0xF4F2EE },
    { 1.629, -57.24, 0.5, 0xD6E2FF }, { 19.846, 8.87, 0.8, 0xF4F2EE }, { 4.599, 16.51, 0.9, 0xFFB090 }, { 13.420, -11.16, 1.0, 0xD6E2FF }, { 7.755, 28.03, 1.1, 0xFFD2A6 }, { 22.961, -29.62, 1.2, 0xF4F2EE },
    { 20.690, 45.28, 1.3, 0xF4F2EE }, { 10.139, 11.97, 1.4, 0xD6E2FF }, { 7.577, 31.89, 1.6, 0xF4F2EE }, { 3.791, 24.11, 2.9, 0xD6E2FF }, { 16.811, -69.03, 1.9, 0xFFD2A6 }, { 20.427, -56.74, 1.9, 0xD6E2FF },
    { 9.220, -69.72, 1.7, 0xF4F2EE }, { 8.375, -59.51, 1.9, 0xFFD2A6 }, { 22.137, -46.96, 1.7, 0xD6E2FF },
};
// Durin's Day: the last new moon of autumn (for your hemisphere) this year, as a day of the year, or -1.
static int DurinsDay(int year, bool south, int& month, int& dayOut) {
    double ref = 947182440.0, syn = 29.530588853 * 86400.0;
    SYSTEMTIME w = {}; w.wYear = (WORD)year; w.wMonth = south ? 6 : 12; w.wDay = 1;
    FILETIME ft; SystemTimeToFileTime(&w, &ft);
    double winter = ((double)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000.0) / 1e7;
    double nm = ref + std::floor((winter - ref) / syn) * syn;
    uint64_t q = (uint64_t)((nm + 11644473600.0) * 1e7);
    FILETIME nf = { (DWORD)q, (DWORD)(q >> 32) }, lf; SYSTEMTIME st;
    FileTimeToLocalFileTime(&nf, &lf); FileTimeToSystemTime(&lf, &st);
    if (st.wMonth < (south ? 3 : 9)) return -1;
    month = st.wMonth; dayOut = st.wDay;
    return 1;
}
}  // namespace Heavens

// ------------------------------------------------------------------------------------ small touches
// Days since 1970-01-01 for a local date (for the day's saying, and the Deeds).
static long EpochDay(const SYSTEMTIME& st) {
    int y = st.wYear, m = st.wMonth, d = st.wDay;
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
static long Today() { SYSTEMTIME st; GetLocalTime(&st); return EpochDay(st); }

// Today's hobbit saying: the same one the phone shows today.
static std::wstring SayingToday() {
    const long n = (long)(sizeof kSayings / sizeof kSayings[0]);
    long i = (Today() * 37 + 11) % n; if (i < 0) i += n;
    return kSayings[i];
}

// A Sindarin greeting for the hour, and its English (as on the phone and the watch).
static void Greeting(int hour, std::wstring& elvish, std::wstring& english) {
    struct G { int from, to; const wchar_t *el, *en; };
    static const G g[] = { {4, 6, L"Le suilon", L"I greet you"}, {7, 10, L"Aur vaer", L"good morning"}, {11, 13, L"Suilad", L"greetings"},
                           {14, 17, L"Galu", L"good fortune"}, {18, 20, L"Mae govannen", L"well met"}, {21, 23, L"Losto mae", L"sleep well"} };
    elvish = L"Novaer"; english = L"be well";
    for (auto& x : g) if (hour >= x.from && hour <= x.to) { elvish = x.el; english = x.en; }
}

// The Deeds of the Red Book seen on this PC: the rare things (a fox, a falling star, a rainbow, the
// moon in shadow...), each written down the first time and counted after. Kept in deeds.txt beside
// the settings, one line each: id, first day, last day, times.
struct DeedRec { long first = 0, last = 0; int count = 0; };
static std::map<std::string, DeedRec> gDeeds;
static bool gDeedsLoaded = false;
static std::wstring DeedsPath() {
    wchar_t base[MAX_PATH]; GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    return std::wstring(base) + L"\\Lembas\\deeds.txt";
}
static void LoadDeeds() {
    if (gDeedsLoaded) return;
    gDeedsLoaded = true;
    FILE* f = _wfopen(DeedsPath().c_str(), L"r");
    if (!f) return;
    char id[64]; long a, b; int c;
    while (std::fscanf(f, "%63s %ld %ld %d", id, &a, &b, &c) == 4) gDeeds[id] = { a, b, c };
    std::fclose(f);
}
static void Deed(const char* id) {
    static std::set<std::string> today; static long day = 0;
    long d = Today();
    if (d != day) { day = d; today.clear(); }
    if (!today.insert(id).second) return;
    LoadDeeds();
    auto& r = gDeeds[id];
    if (r.last == d) return;
    if (!r.first) r.first = d;
    r.last = d; r.count++;
    FILE* f = _wfopen(DeedsPath().c_str(), L"w");
    if (!f) return;
    for (auto& kv : gDeeds) std::fprintf(f, "%s %ld %ld %d\n", kv.first.c_str(), kv.second.first, kv.second.last, kv.second.count);
    std::fclose(f);
}

// English in Tengwar (a simple general-use tehtar mode; the same rules as the phone's Tengwar.kt):
// letters by sound-ish spelling, each vowel as a tehta on the consonant after it (on a short carrier
// when none follows), a silent final e as a dot beneath, doubled letters with a bar beneath, n or m
// before a stop as a bar above, and Tengwar numerals. For the Alcarin Tengwar font (Free Tengwar layout).
namespace Tengwar {
struct U { bool vowel; int code; bool lng; std::vector<int> marks; };
static int Cons(const std::wstring& k) {
    static const std::map<std::wstring, int> c = {
        {L"th", 0xE008}, {L"sh", 0xE00A}, {L"ch", 0xE002}, {L"ng", 0xE012}, {L"wh", 0xE029}, {L"ph", 0xE009}, {L"ck", 0xE003}, {L"qu", 0xE003},
        {L"t", 0xE000}, {L"p", 0xE001}, {L"k", 0xE003}, {L"c", 0xE003}, {L"d", 0xE004}, {L"b", 0xE005}, {L"j", 0xE006}, {L"g", 0xE007},
        {L"f", 0xE009}, {L"v", 0xE00D}, {L"n", 0xE010}, {L"m", 0xE011}, {L"w", 0xE015}, {L"l", 0xE022}, {L"s", 0xE024}, {L"z", 0xE026},
        {L"h", 0xE028}, {L"r", 0xE014}, {L"x", 0xE003}, {L"y", 0xE016} };
    auto it = c.find(k); return it == c.end() ? 0 : it->second;
}
static int Vow(wchar_t v) { switch (v) { case 'a': return 0xE040; case 'e': return 0xE046; case 'i': case 'y': return 0xE044; case 'o': return 0xE04A; case 'u': return 0xE04C; } return 0; }
static bool IsV(wchar_t c) { return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }
static std::wstring Word(std::wstring w) {
    for (auto& ch : w) ch = (wchar_t)towlower(ch);
    std::wstring out;
    if (!w.empty() && std::all_of(w.begin(), w.end(), iswdigit)) { for (wchar_t ch : w) out += (wchar_t)(0xE070 + (ch - '0')); return out; }
    std::vector<U> u;
    for (size_t i = 0; i < w.size();) {
        wchar_t ch = w[i], nx = i + 1 < w.size() ? w[i + 1] : 0;
        if (ch == 'y') { if (IsV(nx)) u.push_back({ false, Cons(L"y"), false, {} }); else u.push_back({ true, Vow('y'), false, {} }); i++; continue; }
        if (IsV(ch)) { if (nx == ch && (ch == 'e' || ch == 'o')) { u.push_back({ true, Vow(ch), true, {} }); i += 2; } else { u.push_back({ true, Vow(ch), false, {} }); i++; } continue; }
        std::wstring two = w.substr(i, 2);
        if (two == L"gh") { i += 2; continue; }
        if (two.size() == 2 && two[0] != two[1] && Cons(two)) { u.push_back({ false, Cons(two), false, {} }); i += 2; continue; }
        if (ch == 'c' && (nx == 'e' || nx == 'i' || nx == 'y')) { u.push_back({ false, 0xE024, false, {} }); i++; continue; }
        if (ch == 'x') { u.push_back({ false, 0xE003, false, {} }); u.push_back({ false, 0xE024, false, {} }); i++; continue; }
        int code = Cons(std::wstring(1, ch));
        if (code) { if (nx == ch) { u.push_back({ false, code, false, { 0xE051 } }); i += 2; } else { u.push_back({ false, code, false, {} }); i++; } continue; }
        i++;
    }
    if (u.size() >= 3 && u.back().vowel && u.back().code == 0xE046 && !u.back().lng && !u[u.size() - 2].vowel && w.back() == 'e') { u.pop_back(); u.back().marks.push_back(0xE045); }
    static const std::set<int> stops = { 0xE000, 0xE001, 0xE002, 0xE003, 0xE004, 0xE005, 0xE006, 0xE007 };
    for (size_t k = 0; k + 1 < u.size();) {
        if (!u[k].vowel && (u[k].code == 0xE010 || u[k].code == 0xE011) && u[k].marks.empty() && !u[k + 1].vowel && stops.count(u[k + 1].code)) { u[k + 1].marks.push_back(0xE050); u.erase(u.begin() + k); continue; }
        k++;
    }
    for (size_t k = 0; k + 1 < u.size(); k++) if (!u[k].vowel && u[k].code == 0xE014 && u[k + 1].vowel) u[k].code = 0xE020;
    for (size_t k = 0; k < u.size();) {
        auto& x = u[k];
        if (x.vowel) {
            int tehta = (x.lng && x.code == 0xE046) ? 0xE048 : x.code;
            if (k + 1 < u.size() && !u[k + 1].vowel && !x.lng) {
                auto& n = u[k + 1];
                int code = n.code == 0xE024 ? 0xE025 : n.code == 0xE026 ? 0xE027 : n.code;
                out += (wchar_t)code; out += (wchar_t)tehta; for (int m : n.marks) out += (wchar_t)m;
                k += 2; continue;
            }
            out += (wchar_t)(x.lng ? 0xE02C : 0xE02E); out += (wchar_t)tehta; k++; continue;
        }
        out += (wchar_t)x.code; for (int m : x.marks) out += (wchar_t)m; k++;
    }
    return out;
}
static std::wstring Text(const std::wstring& s) {
    std::wstring out, tok;
    auto flush = [&]() { if (!tok.empty()) { out += Word(tok); tok.clear(); } };
    for (wchar_t ch : s) {
        bool letter = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'), digit = ch >= '0' && ch <= '9';
        if ((letter || digit) && !tok.empty() && (iswdigit(tok.back()) != 0) != digit) flush();
        if (letter || digit) { tok += ch; continue; }
        flush();
        out += ch == ':' ? (wchar_t)0xE061 : ch == ',' ? (wchar_t)0xE060 : ch;
    }
    flush();
    return out;
}
}  // namespace Tengwar

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
    D2D1_RECT_F Bounds(double) override { return RectF(-480, 0, 2400, 1080); }
    void Draw(double, const D2D1_MATRIX_3X2_F&) override {}  // drawn by the renderer (per monitor bitmaps)
};

// The artboard: the 1920x1080 design, with 480 more on each side that wider screens show.
static const float ART_L = -480, ART_R = 2400;

static double gLatitude = 0, gLongitude = 151.21;
// Today's weather, from weather.json (Lembas Sun fetches it hourly when weather is on):
// 0 clear, 1 cloudy, 2 fog, 3 rain, 4 snow, 5 storm.
static int gWx = 0;
static bool gWind = false;     // a windy day (over 30 km/h): the clouds race and the smoke leans
static bool gRescene = false;  // the season or the weather changed: rebuild the scene
// The light of the hour (worked out once a minute): a tint over the whole scene, and the glow of
// sunrise and sunset rising from the horizon. 0 = none.
static uint32_t gTintCol = 0; static float gTintA = 0, gGlowA = 0;
static float gMoonAlt = 1;     // how high tonight's moon is (0 horizon .. 1), below 0 when it has set
static bool gFullMoon = false, gMeteors = false;
static double gMoonPhase = 0, gEclipse = 0;   // tonight's phase (0 new, .5 full) and how deep in Earth's shadow (0..1)
static bool gSuperMoon = false;
// The sky's moods, from the same weather.json: a rainbow when the sun comes out after rain, frost
// on a cold morning, a shimmer on a hot afternoon, a haze of smoke when the air is bad, beacons lit
// for severe weather, a storm's lightning. And the day's weather in a line of hobbit talk.
struct Sky { bool rainbow = false, frost = false, heat = false, beacons = false, storm = false; float smoke = 0;
    bool operator!=(const Sky& o) const { return rainbow != o.rainbow || frost != o.frost || heat != o.heat || beacons != o.beacons || storm != o.storm || std::abs(smoke - o.smoke) > .09f; } };
static Sky gSky;
static std::wstring gForecast;
static bool gSeasonTurn = false;     // the season changed while running: blow the new one in

// --- shared pieces
struct Glow : Element {  // a soft round light that breathes
    ComPtr<ID2D1RadialGradientBrush> b;
    float x, y, r, dur, off, s0, s1, o0, o1;
    Glow(float x_, float y_, float r_, std::initializer_list<D2D1_GRADIENT_STOP> stops, float d, float of = 0,
         float sMin = 1, float sMax = 1.08f, float oMin = .8f, float oMax = 1)
        : b(Radial(stops)), x(x_), y(y_), r(r_), dur(d), off(of), s0(sMin), s1(sMax), o0(oMin), o1(oMax) { slow = true; rate = 8; }
    float K(double t) { float p = Phase(t, dur, off); return .5f - .5f * std::cos(2 * PI * p); }
    D2D1_RECT_F Bounds(double) override { return Circle(x, y, r * s1 + 2); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float k = Apply(EASE_IN_OUT, K(t)), s = s0 + (s1 - s0) * k;
        gCtx->SetTransform(art);
        FillGlow(b.Get(), x, y, r * s, r * s, o0 + (o1 - o0) * k);
    }
};
struct Glint : Element {  // a short line of light on water that comes and goes
    float x, y, w, dur, off, width; uint32_t col;
    Glint(float x_, float y_, float w_, float d, float o, float wd, uint32_t c) : x(x_), y(y_), w(w_), dur(d), off(o), width(wd), col(c) {}
    D2D1_RECT_F Bounds(double) override { return RectF(x - 16 - width, y - width, x + w + 16 + width, y + width); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float p = Phase(t, dur, off), k = .5f - .5f * std::cos(2 * PI * p), dx = -14 + 28 * p;
        gCtx->SetTransform(art);
        Solid(col, .85f * k);
        gCtx->DrawLine(Point2F(x + dx, y), Point2F(x + dx + w, y), gBrush.Get(), width, gRound.Get());
    }
    static ComPtr<ID2D1StrokeStyle> gRound;
};
ComPtr<ID2D1StrokeStyle> Glint::gRound;
struct Cloud : Element {
    static ComPtr<ID2D1Geometry> shape;
    float y, sc, dur, off, op; uint32_t col;
    Cloud(float x0, float y_, float s, float o, float d, uint32_t c = 0xFFFFFF) : y(y_), sc(s), dur(d), op(o), col(c) {
        off = (x0 + 700) / 3900 * d;  // starts where the design puts it
    }
    D2D1_MATRIX_3X2_F M(double t) {
        float d = gWind ? dur / 4 : dur;
        return Matrix3x2F::Scale(sc, sc) * Matrix3x2F::Translation(-700 + 3900 * Phase(t, d, off * d / dur), y);
    }
    D2D1_RECT_F Bounds(double t) override { return Inflate(TransformRect(RectF(-92, -66, 112, 28), M(t)), 2); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        gCtx->SetTransform(M(t) * art);
        Solid(col, op);
        gCtx->FillGeometry(shape.Get(), gBrush.Get());
    }
};
ComPtr<ID2D1Geometry> Cloud::shape;

// --- Shire Morning
struct Smoke : Element {  // a puff rising from the chimney
    float x, y, r, off;
    Track op{ {0, .15f, 1}, {0, .6f, 0}, EASE_OUT };
    Smoke(float x_, float y_, float r_, float o) : x(x_), y(y_), r(r_), off(o) {}
    void State(double t, float& cx, float& cy, float& cr, float& o) {
        float p = Phase(t, 5, off), k = Apply(EASE_OUT, p);
        cx = x + (gWind ? 170 : 50) * k; cy = y - (gWind ? 120 : 200) * k; cr = r * (.6f + 1.3f * k); o = op(p);
    }
    D2D1_RECT_F Bounds(double t) override { float a, b, c, o; State(t, a, b, c, o); return Circle(a, b, c + 1); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float a, b, c, o; State(t, a, b, c, o);
        gCtx->SetTransform(art);
        Solid(0xFFFFFF, o);
        gCtx->FillEllipse(Ellipse(Point2F(a, b), c, c), gBrush.Get());
    }
};

// --- Red Book: the candle standing on the open book (its wax and dish are in the art; the flame moves)
static const D2D1_MATRIX_3X2_F kFire = Matrix3x2F::Scale(.52f, .52f) * Matrix3x2F::Translation(1210, 816);
struct Flame : Element {
    ComPtr<ID2D1PathGeometry> g; float dur, off; uint32_t col; D2D1_POINT_2F origin; D2D1_RECT_F local;
    Track sx{ {0, .25f, .5f, .75f, 1}, {1, .94f, 1.04f, .97f, 1}, EASE_IN_OUT };
    Track sy{ {0, .25f, .5f, .75f, 1}, {1, 1.08f, .93f, 1.05f, 1}, EASE_IN_OUT };
    Track sk{ {0, .25f, .5f, .75f, 1}, {0, -3, 2, -1, 0}, EASE_IN_OUT };
    Flame(const char* d, float du, float o, uint32_t c) : g(Path(d)), dur(du), off(o), col(c) {
        g->GetBounds(nullptr, &local);
        origin = Point2F((local.left + local.right) / 2, local.bottom);
    }
    D2D1_RECT_F Bounds(double) override { return TransformRect(RectF(local.left - 8, local.top - 12, local.right + 8, local.bottom + 1), kFire); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float p = Phase(t, dur, off);
        auto m = Matrix3x2F::Translation(-origin.x, -origin.y) * Matrix3x2F::Skew(sk(p), 0) *
                 Matrix3x2F::Scale(sx(p), sy(p)) * Matrix3x2F::Translation(origin.x, origin.y) * kFire;
        gCtx->SetTransform(m * art);
        Solid(col, 1);
        gCtx->FillGeometry(g.Get(), gBrush.Get());
    }
};
struct Mote : Element {  // dust drifting over the map
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
struct Needle : Element {  // the compass needle, swaying
    ComPtr<ID2D1PathGeometry> ns = Path("M0,-96 L14,0 L0,96 L-14,0 Z");
    ComPtr<ID2D1PathGeometry> ew = Path("M-96,0 L0,-14 L96,0 L0,14 Z");
    Track rot{ {0, .5f, 1}, {-6, 5, -6}, EASE_IN_OUT };
    Needle() { slow = true; }
    D2D1_RECT_F Bounds(double) override { return Circle(1760, 900, 100); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        gCtx->SetTransform(Matrix3x2F::Rotation(rot(Phase(t, 7, 0))) * Matrix3x2F::Translation(1760, 900) * art);
        Solid(0x5A3F22, .55f);
        gCtx->FillGeometry(ew.Get(), gBrush.Get());
        Solid(0x9A2A1C, .85f);
        gCtx->FillGeometry(ns.Get(), gBrush.Get());
    }
};

// --- Starlit Night
struct Twinkle : Element {
    float x, y, r, dur, off;
    D2D1_RECT_F Bounds(double) override { return Circle(x, y, r * 1.3f + 1); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float p = Phase(t, dur, off), k = .5f - .5f * std::cos(2 * PI * p);
        gCtx->SetTransform(art);
        Solid(0xFFF8E6, .15f + .85f * k);
        float s = .6f + .65f * k;
        gCtx->FillEllipse(Ellipse(Point2F(x, y), r * s, r * s), gBrush.Get());
    }
};
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
        gCtx->CreateLinearGradientBrush(LinearGradientBrushProperties(Point2F(1100, 110), Point2F(1250, 110)), c.Get(), &tail);
    }
    bool State(double t, float& o, D2D1_MATRIX_3X2_F& m) {
        if (t < 3) return false;
        float p = Phase(t - 3, gMeteors ? 5.f : 11.f, 0);
        o = op(p);
        if (o < .01f) return false;
        m = Matrix3x2F::Rotation(-24, Point2F(1100, 110)) * Matrix3x2F::Translation(tx(p), ty(p));
        return true;
    }
    D2D1_RECT_F Bounds(double t) override {
        float o; D2D1_MATRIX_3X2_F m;
        if (!State(t, o, m)) return Empty();
        return Inflate(TransformRect(RectF(1100, 110, 1250, 112), m), 2);
    }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float o; D2D1_MATRIX_3X2_F m;
        if (!State(t, o, m)) return;
        if (o > .5f) Deed("meteor");
        gCtx->SetTransform(m * art);
        tail->SetOpacity(o);
        gCtx->FillRectangle(RectF(1100, 110, 1250, 112), tail.Get());
    }
};
struct Moon : Element {  // tonight's real phase, lit on the right side for your hemisphere, where it really is
    float cx, cy, r, baseY, r0;
    static ComPtr<ID2D1RadialGradientBrush> glow;
    ComPtr<ID2D1PathGeometry> lit;
    bool flip = false;
    double lastPh = -1;
    float lastAlt = -9;
    Moon(float x, float y, float rr) : cx(x), cy(y), r(rr), baseY(y), r0(rr) { still = true; }
    void Update() {
        float wantY = baseY + (1 - std::max(0.f, gMoonAlt)) * 330;
        if (std::fabs(gMoonPhase - lastPh) < .002 && lit && lastAlt == gMoonAlt) return;
        lastAlt = gMoonAlt; lastPh = gMoonPhase;
        cy = wantY;
        r = r0 * (gSuperMoon ? 1.32f : gFullMoon ? 1.18f : 1.f);
        double ph = gMoonPhase;
        float tw = r * (float)std::cos(2 * PI * ph), aw = std::max(std::fabs(tw), .5f);
        char d[256];
        std::snprintf(d, sizeof d, "M%g,%g A%g,%g 0 0 1 %g,%g A%g,%g 0 0 %d %g,%g Z", cx, cy - r, r, r, cx, cy + r, aw, r, tw >= 0 ? 0 : 1, cx, cy - r);
        lit = Path(d);
        flip = (ph > .5) != (gLatitude < 0);
    }
    D2D1_RECT_F Bounds(double) override { Update(); return gMoonAlt <= 0 ? Empty() : Circle(cx, cy, r * 3.3f + 1); }
    void Draw(double, const D2D1_MATRIX_3X2_F& art) override {
        Update();
        if (gMoonAlt <= 0) return;   // set: not in the sky tonight
        gCtx->SetTransform(art);
        FillGlow(glow.Get(), cx, cy, r * 3.2f * (gFullMoon ? 1.3f : 1), r * 3.2f * (gFullMoon ? 1.3f : 1), .9f * (1 - .7f * (float)gEclipse));
        gCtx->SetTransform(art);
        Solid(0x2A3058, 1);
        gCtx->FillEllipse(Ellipse(Point2F(cx, cy), r, r), gBrush.Get());
        gCtx->SetTransform((flip ? Matrix3x2F(-1, 0, 0, 1, 2 * cx, 0) : Matrix3x2F::Identity()) * art);
        Solid(0xFFF3D6, 1);
        gCtx->FillGeometry(lit.Get(), gBrush.Get());
        if (gEclipse > .3) Deed("eclipse");
        if (gSuperMoon) Deed("supermoon");
        if (gEclipse > 0) { gCtx->SetTransform(art); Solid(0x9A3A22, .92f * (float)gEclipse); gCtx->FillEllipse(Ellipse(Point2F(cx, cy), r, r), gBrush.Get()); }   // the blood-red moon
    }
};
ComPtr<ID2D1RadialGradientBrush> Moon::glow;
struct RealSky : Element {  // the bright stars and the planets where they really are (night looks), moved once a minute
    struct Dot { float x, y, r; uint32_t col; bool planet; };
    std::vector<Dot> dots; float top, horizon; int minute = -1;
    RealSky(float skyTop, float hz) : top(skyTop), horizon(hz) { slow = true; rate = 1.f / 30; }
    void Update() {
        SYSTEMTIME st; GetLocalTime(&st);
        int m = st.wHour * 60 + st.wMinute;
        if (m == minute) return;
        minute = m; dots.clear();
        double lat = gLatitude, lon = gLongitude, az0 = lat < 0 ? 180 : 0, d = Heavens::DayNow();   // face the pole you can see
        auto place = [&](double alt, double az, float& x, float& y) {
            double da = std::fmod(std::fmod(az - az0, 360) + 540, 360) - 180;
            x = (float)(960 + da * 16); y = (float)(horizon - alt * 9);
            return alt > 2 && x > ART_L && x < ART_R && y < top && y > 0;
        };
        for (auto& sd : Heavens::kStars) {
            auto p = Heavens::ToAltAz(sd.ra * 15, sd.dec, d, lat, lon); float x, y;
            if (place(p.alt, p.az, x, y)) dots.push_back({ x, y, std::max(.9f, std::min(3.4f, (float)(2.7 - .5 * sd.mag))), sd.col, false });
        }
        for (int k = 0; k < 5; k++) {
            auto p = Heavens::Planet(k, lat, lon); float x, y;
            if (place(p.alt, p.az, x, y)) dots.push_back({ x, y, Heavens::kPlanets[k].size, Heavens::kPlanets[k].col, true });
        }
    }
    D2D1_RECT_F Bounds(double) override { Update(); return RectF(ART_L, 0, ART_R, top + 8); }
    void Draw(double, const D2D1_MATRIX_3X2_F& art) override {
        Update();
        gCtx->SetTransform(art);
        for (auto& dt : dots) {
            if (dt.planet) { Solid(dt.col, .24f); gCtx->FillEllipse(Ellipse(Point2F(dt.x, dt.y), dt.r * 2.4f, dt.r * 2.4f), gBrush.Get()); }
            Solid(dt.col, 1); gCtx->FillEllipse(Ellipse(Point2F(dt.x, dt.y), dt.r, dt.r), gBrush.Get());
        }
    }
};
struct CabinWindow : Element {  // the cabin's lit windows, flickering now and then
    float x, y;
    CabinWindow(float x_, float y_) : x(x_), y(y_) { slow = true; rate = 10; }
    D2D1_RECT_F Bounds(double) override { return RectF(x - 16, y - 5, x + 22, y + 13); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float p = Phase(t, 3.2f, 0), o = (p > .31f && p < .33f) ? .82f : (p > .64f && p < .67f) ? .88f : 1;
        gCtx->SetTransform(art);
        Solid(0xFFC867, o);
        gCtx->FillRectangle(RectF(x - 14, y - 3, x, y + 11), gBrush.Get());
        gCtx->FillRectangle(RectF(x + 6, y - 3, x + 20, y + 11), gBrush.Get());
    }
};

// --- Elven Silver
struct Mist : Element {  // a band of mist drifting along the valley
    ComPtr<ID2D1LinearGradientBrush> b;
    float y, h;
    float off;
    Mist(float y_, float h_, uint32_t col = 0x9FC7D0, float a = .11f, float off_ = 0) : y(y_), h(h_), off(off_) {
        slow = true; rate = 4;
        D2D1_GRADIENT_STOP st[3] = { {0, Hex(col, 0)}, {.5f, Hex(col, a)}, {1, Hex(col, 0)} };
        ComPtr<ID2D1GradientStopCollection> c;
        gCtx->CreateGradientStopCollection(st, 3, &c);
        gCtx->CreateLinearGradientBrush(LinearGradientBrushProperties(Point2F(0, y), Point2F(0, y + h)), c.Get(), &b);
    }
    float X(double t) { float p = Phase(t, 80, off); return -120 * std::cos(2 * PI * p); }
    D2D1_RECT_F Bounds(double t) override { float dx = X(t); return RectF(ART_L - 200 + dx, y, ART_R + 200 + dx, y + h); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        gCtx->SetTransform(art);
        gCtx->FillRectangle(Bounds(t), b.Get());
    }
};
struct Lamp : Element {  // a lantern among the trees, glowing softly (silver in Rivendell, gold in Lórien)
    float x, y, dur, off; bool gold;
    static ComPtr<ID2D1RadialGradientBrush> glow, goldGlow;
    Lamp(float x_, float y_, float d, float o, bool g = false) : x(x_), y(y_), dur(d), off(o), gold(g) { slow = true; rate = 10; }
    D2D1_RECT_F Bounds(double) override { return Circle(x, y, gold ? 30.f : 26.f); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float k = .5f - .5f * std::cos(2 * PI * Phase(t, dur, off)), s = .85f + .3f * k, r = gold ? 24.f : 22.f;
        gCtx->SetTransform(art);
        FillGlow(gold ? goldGlow.Get() : glow.Get(), x, y, r * s, r * s, .6f + .4f * k);
        Solid(gold ? 0xFFF0C4 : 0xF3E3B4, 1);
        gCtx->FillEllipse(Ellipse(Point2F(x, y), 4.5f, 4.5f), gBrush.Get());
    }
};
ComPtr<ID2D1RadialGradientBrush> Lamp::glow, Lamp::goldGlow;
struct Leaf : Element {  // a golden leaf drifting down through Lórien, turning as it falls
    float x, dur, off, ph; uint32_t col;
    Leaf(float x_, float d, float o, float p, uint32_t c) : x(x_), dur(d), off(o), ph(p), col(c) { smooth = true; }
    D2D1_POINT_2F At(double t) { float p = Phase(t, dur, off); return Point2F(x + 60 * std::sin(p * 2 * PI * 1.5f + ph), -20 + 1120 * p); }
    D2D1_RECT_F Bounds(double t) override { auto c = At(t); return Circle(c.x, c.y, 9); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float p = Phase(t, dur, off); auto c = At(t);
        gCtx->SetTransform(Matrix3x2F::Rotation(p * 540 + ph * 57, Point2F(0, 0)) * Matrix3x2F::Translation(c.x, c.y) * art);
        Solid(col, .9f * (p > .85f ? (1 - p) / .15f : 1));
        gCtx->FillEllipse(Ellipse(Point2F(0, 0), 7, 3.4f), gBrush.Get());
    }
};
struct Ship : Element {  // the white ship in the Grey Havens, rising and falling on the swell
    ComPtr<ID2D1PathGeometry> hull = Path("M-120,0 C-90,26 80,26 120,0 C124,-10 132,-30 150,-44 C140,-28 136,-14 134,0 Z M150,-44 C158,-50 166,-46 164,-38 C160,-40 156,-40 150,-44 Z");
    ComPtr<ID2D1PathGeometry> keel = Path("M-120,0 C-90,26 80,26 120,0 L118,8 C80,32 -90,32 -118,8 Z");
    ComPtr<ID2D1PathGeometry> sailA = Path("M-2,-186 C60,-150 74,-80 64,-18 L-2,-18 Z"), sailB = Path("M-10,-170 C-60,-130 -70,-70 -62,-20 L-10,-20 Z");
    ComPtr<ID2D1LinearGradientBrush> sail;
    float x, y;
    Ship(float x_, float y_) : x(x_), y(y_) {
        smooth = true;
        D2D1_GRADIENT_STOP st[2] = { {0, Hex(0xF6F6F2, 1)}, {1, Hex(0xC9CED8, 1)} };
        ComPtr<ID2D1GradientStopCollection> c;
        gCtx->CreateGradientStopCollection(st, 2, &c);
        gCtx->CreateLinearGradientBrush(LinearGradientBrushProperties(Point2F(-70, 0), Point2F(70, 0)), c.Get(), &sail);
    }
    D2D1_MATRIX_3X2_F M(double t) { float rock = 1.2f * std::sin(2 * PI * Phase(t, 8, 0)), bob = 3 * std::sin(2 * PI * Phase(t, 8, 2)); return Matrix3x2F::Rotation(rock) * Matrix3x2F::Translation(x, y + bob); }
    D2D1_RECT_F Bounds(double) override { return RectF(x - 140, y - 200, x + 180, y + 40); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        gCtx->SetTransform(M(t) * art);
        Solid(0xB9C0CE, 1); gCtx->FillGeometry(keel.Get(), gBrush.Get());
        Solid(0xF2F2EE, 1); gCtx->FillGeometry(hull.Get(), gBrush.Get());
        Solid(0xD8DCE4, 1); gCtx->DrawLine(Point2F(-6, -6), Point2F(-6, -190), gBrush.Get(), 5);
        sail->SetOpacity(1); gCtx->FillGeometry(sailA.Get(), sail.Get());
        sail->SetOpacity(.9f); gCtx->FillGeometry(sailB.Get(), sail.Get());
        Solid(0xFFF2D0, 1); gCtx->FillEllipse(Ellipse(Point2F(-112, -8), 5, 5), gBrush.Get());
    }
};
struct Rising : Element {  // a mote of light drifting up through the grove
    float x, y, dur, off;
    Track op{ {0, .2f, .8f, 1}, {0, .8f, .6f, 0}, LINEAR };
    D2D1_RECT_F Bounds(double t) override { return Circle(x, y - 360 * Phase(t, dur, off), 7); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float p = Phase(t, dur, off), o = op(p), yy = y - 360 * p;
        gCtx->SetTransform(art);
        Solid(0xC8E1EB, .22f * o);
        gCtx->FillEllipse(Ellipse(Point2F(x, yy), 6, 6), gBrush.Get());
        Solid(0xDCEBF0, o);
        gCtx->FillEllipse(Ellipse(Point2F(x, yy), 2, 2), gBrush.Get());
    }
};

// --- The sky's moods
struct Rainbow : Element {  // after rain, the sun out: a faint bow behind the hills
    float cx, cy, r;
    Rainbow(float x, float y, float rr) : cx(x), cy(y), r(rr) { still = true; }
    D2D1_RECT_F Bounds(double) override { return RectF(cx - r - 10, cy - r - 10, cx + r + 10, cy); }
    void Draw(double, const D2D1_MATRIX_3X2_F& art) override {
        static const uint32_t col[7] = { 0xE8574A, 0xF29A3A, 0xF6D35A, 0x7CC26A, 0x5AA8D8, 0x6A6CC8, 0x9A6AC0 };
        gCtx->SetTransform(art);
        gCtx->PushAxisAlignedClip(RectF(cx - r - 10, cy - r - 10, cx + r + 10, cy), D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 0; i < 7; i++) { Solid(col[i], .21f); gCtx->DrawEllipse(Ellipse(Point2F(cx, cy), r - i * 12.f, r - i * 12.f), gBrush.Get(), 12); }
        gCtx->PopAxisAlignedClip();
    }
};
struct Beacon : Element {  // a beacon fire on a far hill, lit for severe weather
    float x, y, ph;
    static ComPtr<ID2D1RadialGradientBrush> glow;
    ComPtr<ID2D1PathGeometry> flame = Path("M-5,0 C-3,-0.333 -1.333,-0.667 0,-1 C1.333,-0.667 3,-0.333 5,0 Z");   // one unit tall: scaled to the flicker
    Beacon(float x_, float y_, float p) : x(x_), y(y_), ph(p) { slow = true; rate = 12; }
    D2D1_RECT_F Bounds(double) override { return Circle(x, y - 8, 46); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float k = .55f + .45f * std::fabs(std::sin((float)t * (5 + ph) + ph));
        gCtx->SetTransform(art);
        FillGlow(glow.Get(), x, y - 8, 34 * (.85f + .25f * k), 34 * (.85f + .25f * k), .6f + .4f * k);
        float fh = 14 * (.8f + .3f * k);
        gCtx->SetTransform(Matrix3x2F::Scale(1, fh) * Matrix3x2F::Translation(x, y) * art);
        Solid(0xFFD27A, 1); gCtx->FillGeometry(flame.Get(), gBrush.Get());
    }
};
ComPtr<ID2D1RadialGradientBrush> Beacon::glow;
struct Bolt : Element {  // a storm: a bolt of lightning far off now and then, and the sky flashing
    bool flash;
    explicit Bolt(bool f) : flash(f) { smooth = true; }
    float P(double t) { return Phase(t, 9, 0); }
    D2D1_RECT_F Bounds(double t) override {
        float p = P(t);
        if (flash) return (p < .035f) ? RectF(ART_L, 0, ART_R, 1080) : Empty();
        return p < .03f ? RectF(ART_L, 0, ART_R, 600) : Empty();
    }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float p = P(t);
        gCtx->SetTransform(art);
        if (flash) {
            float f = p < .012f ? .3f : (p > .02f && p < .03f) ? .18f : 0;
            if (f > 0) { Solid(0xF4F2FF, f); gCtx->FillRectangle(RectF(ART_L, 0, ART_R, 1080), gBrush.Get()); }
            return;
        }
        if (p >= .03f) return;
        int n = (int)std::floor(t / 9);
        float x = ART_L + (ART_R - ART_L) * (.15f + .7f * std::fmod(n * .618f, 1.f)), y = 80;
        Rng r((uint32_t)n + 7);
        Solid(0xF4F2FF, 1 - p / .03f);
        while (y < 560) { float nx = x + ((float)r() - .5f) * 60, ny = y + 40 + (float)r() * 40; gCtx->DrawLine(Point2F(x, y), Point2F(nx, ny), gBrush.Get(), 3, Glint::gRound.Get()); x = nx; y = ny; }
    }
};
struct Haze : Element {  // smoke in the air (bushfires), or frost on the ground: a still wash
    ComPtr<ID2D1LinearGradientBrush> b; float top;
    Haze(std::initializer_list<D2D1_GRADIENT_STOP> st, float y0, float y1) : top(y0) {
        still = true;
        std::vector<D2D1_GRADIENT_STOP> v(st);
        ComPtr<ID2D1GradientStopCollection> c;
        gCtx->CreateGradientStopCollection(v.data(), (UINT32)v.size(), &c);
        gCtx->CreateLinearGradientBrush(LinearGradientBrushProperties(Point2F(0, y0), Point2F(0, y1)), c.Get(), &b);
    }
    D2D1_RECT_F Bounds(double) override { return RectF(ART_L, top, ART_R, 1080); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override { gCtx->SetTransform(art); gCtx->FillRectangle(Bounds(t), b.Get()); }
};
struct Sparkle : Element {  // frost glinting on the grass
    float x, y, r, dur, off;
    Sparkle(float x_, float y_, float r_, float d, float o) : x(x_), y(y_), r(r_), dur(d), off(o) { slow = true; rate = 6; }
    D2D1_RECT_F Bounds(double) override { return Circle(x, y, r + 1); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float k = .5f - .5f * std::cos(2 * PI * Phase(t, dur, off));
        gCtx->SetTransform(art); Solid(0xFFFFFF, .78f * k); gCtx->FillEllipse(Ellipse(Point2F(x, y), r, r), gBrush.Get());
    }
};
struct Shimmer : Element {  // the air wavering over the land on a hot afternoon
    float y, ph;
    Shimmer(float y_, float p) : y(y_), ph(p) { slow = true; rate = 15; }
    D2D1_RECT_F Bounds(double) override { return RectF(ART_L - 80, y - 12, ART_R + 80, y + 12); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float dx = 30 * std::sin((float)t * .9f + ph), dy = 4 * std::sin((float)t * 2 + ph);
        gCtx->SetTransform(art); Solid(0xFFF6E0, .085f);
        gCtx->DrawLine(Point2F(ART_L - 60 + dx, y + dy), Point2F(ART_R + 60 + dx, y - dy), gBrush.Get(), 6, Glint::gRound.Get());
    }
};
struct WindowDrop : Element {  // rain on the window by the Red Book: the shadow of a drop sliding down the page
    float x, dur, off, r;
    WindowDrop(float x_, float d, float o, float rr) : x(x_), dur(d), off(o), r(rr) { slow = true; rate = 15; }
    float Y(double t) { float p = Phase(t, dur, off); return -40 + 1160 * (p * p * (3 - 2 * p)); }
    D2D1_RECT_F Bounds(double t) override { float y = Y(t); return RectF(x - r - 1, y - r * 9, x + r + 1, y + r * 1.4f); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float y = Y(t);
        gCtx->SetTransform(art);
        Solid(0x3A2614, .1f); gCtx->FillEllipse(Ellipse(Point2F(x, y), r, r * 1.3f), gBrush.Get());
        Solid(0x3A2614, .05f); gCtx->DrawLine(Point2F(x, y - r * 9), Point2F(x, y - r), gBrush.Get(), r * .7f, Glint::gRound.Get());
    }
};
struct SeasonBit : Element {  // the new season blowing in for a minute: blossom, leaves, snow or thistledown
    float bx, by, sp, ph; int kind; uint32_t col; double t0 = -1;
    SeasonBit(float x, float y, float s, float p, int k, uint32_t c) : bx(x), by(y), sp(s), ph(p), kind(k), col(c) { smooth = true; }
    D2D1_POINT_2F At(double t) {
        if (t0 < 0) t0 = t;
        float e = (float)(t - t0);
        return Point2F(ART_L + std::fmod(bx + e * 38 * sp + 30 * std::sin(e * .8f + ph), ART_R - ART_L), std::fmod(by + e * 54 * sp, 1180.f) - 50);
    }
    float Amount(double t) { float f = (float)((t - t0) / 60); return (t0 < 0 || f >= 1) ? (t0 < 0 ? 1.f : 0.f) : std::sin(PI * f); }
    D2D1_RECT_F Bounds(double t) override { auto c = At(t); return Amount(t) > 0 ? Circle(c.x, c.y, 10) : Empty(); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float a = Amount(t); if (a <= 0) return;
        auto c = At(t); float e = (float)(t - t0);
        gCtx->SetTransform(Matrix3x2F::Rotation(e * 70 * sp + ph * 57) * Matrix3x2F::Translation(c.x, c.y) * art);
        Solid(col, .9f * a);
        if (kind == 3) gCtx->FillEllipse(Ellipse(Point2F(0, 0), 2.5f + sp * 2, 2.5f + sp * 2), gBrush.Get());
        else if (kind == 1) gCtx->FillEllipse(Ellipse(Point2F(0, 0), 1.8f, 1.8f), gBrush.Get());
        else gCtx->FillEllipse(Ellipse(Point2F(0, 0), kind == 2 ? 6.f : 4.f, kind == 2 ? 2.8f : 2.4f), gBrush.Get());
    }
};

// --- Weather, over the outdoor scenes
struct Drop : Element {  // rain: a slanting streak
    float x, dur, off, len;
    Drop(float x_, float d, float o, float l) : x(x_), dur(d), off(o), len(l) { smooth = true; }
    D2D1_POINT_2F At(double t) { float p = Phase(t, dur, off); return Point2F(x - 120 * p, -60 + 1200 * p); }
    D2D1_RECT_F Bounds(double t) override { auto a = At(t); return RectF(a.x - 3 - len * .1f, a.y - 2, a.x + 3, a.y + len + 2); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        auto a = At(t);
        gCtx->SetTransform(art);
        Solid(0xDCE6EE, .45f);
        gCtx->DrawLine(a, Point2F(a.x - len * .1f, a.y + len), gBrush.Get(), 1.6f, Glint::gRound.Get());
    }
};
struct Flake : Element {  // snow: a flake drifting down
    float x, dur, off, r;
    Flake(float x_, float d, float o, float rr) : x(x_), dur(d), off(o), r(rr) {}
    D2D1_POINT_2F At(double t) { float p = Phase(t, dur, off); return Point2F(x + 30 * std::sin(p * 2 * PI * 2 + x), -20 + 1120 * p); }
    D2D1_RECT_F Bounds(double t) override { auto a = At(t); return Circle(a.x, a.y, r + 1); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        auto a = At(t);
        gCtx->SetTransform(art);
        Solid(0xFFFFFF, .85f);
        gCtx->FillEllipse(Ellipse(a, r, r), gBrush.Get());
    }
};

// --- The light of the hour, over everything (redrawn whenever it changes, at most once a minute)
struct Light : Element {
    ComPtr<ID2D1LinearGradientBrush> glow;
    Light() {
        still = true;
        D2D1_GRADIENT_STOP st[3] = { {0, Hex(0xFF8A5C, .55f)}, {.5f, Hex(0xF2789A, .28f)}, {1, Hex(0xF2789A, 0)} };
        ComPtr<ID2D1GradientStopCollection> c;
        gCtx->CreateGradientStopCollection(st, 3, &c);
        gCtx->CreateLinearGradientBrush(LinearGradientBrushProperties(Point2F(0, 1080), Point2F(0, 380)), c.Get(), &glow);
    }
    D2D1_RECT_F Bounds(double) override { return RectF(ART_L - 4, -4, ART_R + 4, 1084); }
    void Draw(double, const D2D1_MATRIX_3X2_F& art) override {
        gCtx->SetTransform(art);
        if (gTintA > 0) { Solid(gTintCol, gTintA); gCtx->FillRectangle(Bounds(0), gBrush.Get()); }
        if (gGlowA > 0) { glow->SetOpacity(gGlowA); gCtx->FillRectangle(Bounds(0), glow.Get()); }
    }
};

// --- Now and then (for a minute, eight times a day, at different times each day) a visitor:
// a fox trotting over the Shire, a boat crossing the starlit lake, an owl blinking in Rivendell.
static int VisitorShift() { SYSTEMTIME st; GetLocalTime(&st); return (int)(((unsigned long long)(st.wYear * 372 + st.wMonth * 31 + st.wDay) * 2654435761ULL) % 180); }
struct Visitor : Element {
    int kind;  // 0 fox, 1 boat, 2 owl
    ComPtr<ID2D1PathGeometry> body;
    explicit Visitor(int k) : kind(k) {
        smooth = k != 2;
        body = Path(k == 0 ? "M-30,0 C-24,-8 -8,-10 4,-6 L12,-12 L14,-4 C18,-2 20,2 16,4 L6,4 C2,8 -8,8 -16,6 C-22,10 -30,10 -34,6 C-28,6 -26,4 -30,0 Z"
                         : k == 1 ? "M-26,-4 L26,-4 L18,6 L-18,6 Z M8,-4 L8,-28 L10,-28 L10,-4 Z M10,-26 L22,-10 L10,-10 Z"
                                  : "M-12,0 C-12,-20 12,-20 12,0 C12,18 -12,18 -12,0 Z");
    }
    // seconds into this visitor's minute, or -1
    static double Into() {
        SYSTEMTIME st; GetLocalTime(&st);
        int m = st.wHour * 60 + st.wMinute;
        if ((m + VisitorShift()) % 180 != 0) return -1;
        return st.wSecond + st.wMilliseconds / 1000.0;
    }
    bool At(double, float& x, float& y, float& o) {
        double s = Into();
        if (s < 0) return false;
        float p = (float)(s / 60);
        o = 1;
        if (kind == 0) { x = -560 + 3000 * p; y = 1000 - 8 * std::fabs(std::sin(p * 60)); }
        else if (kind == 1) { x = 1150 + 520 * p; y = 838; }
        else { x = 690; y = 760; o = std::min(1.f, std::min(p, 1 - p) * 8); }
        return true;
    }
    D2D1_RECT_F Bounds(double t) override { float x, y, o; if (!At(t, x, y, o)) return Empty(); return RectF(x - 40, y - 40, x + 40, y + 30); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float x, y, o; if (!At(t, x, y, o)) return;
        Deed(kind == 0 ? "fox" : kind == 1 ? "boat" : "owl");
        gCtx->SetTransform(Matrix3x2F::Translation(x, y) * art);
        if (kind == 0) {
            Solid(0xD9733A, 1); gCtx->FillGeometry(body.Get(), gBrush.Get());
            float k = std::sin((float)(Into() / 60) * 120) * 4;
            Solid(0x8A3E1A, 1);
            gCtx->DrawLine(Point2F(-14, 8), Point2F(-16 + k, 20), gBrush.Get(), 2.6f, Glint::gRound.Get());
            gCtx->DrawLine(Point2F(14, 8), Point2F(16 - k, 20), gBrush.Get(), 2.6f, Glint::gRound.Get());
            Solid(0xF4EFE4, 1); gCtx->FillEllipse(Ellipse(Point2F(-30, 2), 4, 4), gBrush.Get());
        } else if (kind == 1) {
            Solid(0x0E0C28, 1); gCtx->FillGeometry(body.Get(), gBrush.Get());
            Solid(0xFFC867, .25f); gCtx->FillEllipse(Ellipse(Point2F(10, -22), 10, 10), gBrush.Get());
            Solid(0xFFC867, 1); gCtx->FillEllipse(Ellipse(Point2F(10, -22), 3.5f, 3.5f), gBrush.Get());
        } else {
            Solid(0x2E3A40, o); gCtx->FillGeometry(body.Get(), gBrush.Get());
            bool blink = std::fmod(Into(), 4.0) < .25;
            Solid(0xF3E3B4, o);
            if (blink) { gCtx->FillRectangle(RectF(-8, -6, -2, -5), gBrush.Get()); gCtx->FillRectangle(RectF(2, -6, 8, -5), gBrush.Get()); }
            else { gCtx->FillEllipse(Ellipse(Point2F(-5, -6), 3.4f, 3.4f), gBrush.Get()); gCtx->FillEllipse(Ellipse(Point2F(5, -6), 3.4f, 3.4f), gBrush.Get()); }
        }
    }
};

// ------------------------------------------------------------------------------------ scenes
enum SceneId { NONE = -1, SHIRE, REDBOOK, STARLIT, ELVEN, LORIEN, HAVENS };

// While music plays, fireflies pulse with it: the loudness of what's playing (Windows' own level
// meter for the speakers; no microphone, nothing recorded), smoothed, sets how brightly they glow.
// (the level meter's interface, which the MinGW headers only name)
struct MeterInfo : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float* peak) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 count, float* peaks) = 0;
    virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* mask) = 0;
};
__CRT_UUID_DECL(MeterInfo, 0xC02216F6, 0x8C67, 0x4B5B, 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64)
static ComPtr<MeterInfo> gMeter;
static bool gFirefliesOn = true;   // the setting (read with the others)
static float gMusic = 0;        // smoothed level 0..1
static ULONGLONG gMusicAt = 0;  // when sound was last heard
static void ReadMusic() {
    static ULONGLONG tried = 0;
    ULONGLONG now = GetTickCount64();
    if (!gMeter && now - tried > 30000) {
        tried = now;
        ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDevice> dev;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
            SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)))
            dev->Activate(__uuidof(MeterInfo), CLSCTX_ALL, nullptr, (void**)gMeter.GetAddressOf());
    }
    float peak = 0;
    if (gMeter && FAILED(gMeter->GetPeakValue(&peak))) { gMeter.Reset(); peak = 0; }   // the speakers changed: find them again
    if (peak > .02f) gMusicAt = now;
    gMusic += ((peak > gMusic) ? .5f : .12f) * (peak - gMusic);
}
static bool MusicOn() { return gFirefliesOn && GetTickCount64() - gMusicAt < 2500; }

struct Fireflies : Element {
    struct F { float x, y, a, b, g; };
    std::vector<F> f;
    uint32_t col;
    explicit Fireflies(SceneId id) {
        smooth = true;
        col = id == LORIEN ? 0xFFE08A : id == SHIRE ? 0xFFF6CC : id == REDBOOK ? 0xFFC56A : (id == HAVENS || id == ELVEN) ? 0xE6F6D8 : 0xE8F59A;
        Rng r(1966);
        for (int i = 0; i < 22; i++) f.push_back({ frand10(ART_L + r() * (ART_R - ART_L)), frand10(640 + r() * 330), frand10(r() * 6.28), frand10(r() * 6.28), (float)(i % 3) });
    }
    D2D1_RECT_F Bounds(double) override { return MusicOn() ? RectF(ART_L - 60, 580, ART_R + 60, 1030) : Empty(); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        if (!MusicOn()) return;
        gCtx->SetTransform(art);
        float lvl = std::min(1.f, gMusic * 1.6f);
        for (auto& x : f) {
            float px = x.x + 50 * std::sin((float)t * .31f + x.a), py = x.y + 30 * std::sin((float)t * .23f + x.b);
            // the level, and a gentle turn-taking between three groups so they don't all flash at once
            float k = .12f + .88f * lvl * (.55f + .45f * std::cos((float)t * 2.2f + x.g * 2.09f));
            Solid(col, .28f * k); gCtx->FillEllipse(Ellipse(Point2F(px, py), 11, 11), gBrush.Get());
            Solid(col, k); gCtx->FillEllipse(Ellipse(Point2F(px, py), 2.6f, 2.6f), gBrush.Get());
        }
    }
};

// The hidden phrases: type "mellon" or "ash nazg" with today's panel open. An Elven door draws
// itself in silver and opens on warm light; or the screen darkens while fire-letters kindle around a
// gold ring. Six seconds, then gone. (Drawn for Lembas: the letters belong to no real alphabet.)
static int gWonder = -1;            // 0 the door, 1 the ring
static ULONGLONG gWonderAt = 0;
static const float kWonderLen = 6000;
struct Wonder : Element {
    ComPtr<ID2D1PathGeometry> arch, trees, star, glyphs;
    Wonder() {
        smooth = true;
        arch = Path("M760,900 L760,420 C760,250 1160,250 1160,420 L1160,900");
        trees = Path("M832,900 L832,470 M832,560 C820,540 808,520 800,500 M832,660 C820,640 808,620 800,600 M832,760 C820,740 808,720 800,700 "
                     "M1088,900 L1088,470 M1088,560 C1100,540 1112,520 1120,500 M1088,660 C1100,640 1112,620 1120,600 M1088,760 C1100,740 1112,720 1120,700");
        std::string st = "M960,322";
        for (int k = 1; k <= 16; k++) { float a = k * 3.14159f / 8 - 1.5708f, r = k % 2 ? 11.f : 30.f; char b[48]; std::snprintf(b, 48, " L%.1f,%.1f", 960 + std::cos(a) * r, 352 + std::sin(a) * r); st += b; }
        star = Path((st + " Z").c_str());
        std::string g;
        Rng r(1954);
        for (int i = 0; i < 34; i++) {   // script-like strokes around the band
            float a = i / 34.f * 6.2832f + 1.5708f, x = 960 + std::cos(a) * 330, y = 520 + std::sin(a) * 140;
            float s = 1.1f; char b[200];
            switch ((int)(r() * 3)) {
            case 0: std::snprintf(b, 200, "M%.1f,%.1f C%.1f,%.1f %.1f,%.1f %.1f,%.1f M%.1f,%.1f L%.1f,%.1f ", x, y - 6 * s, x + 6 * s, y - 10 * s, x + 8 * s, y + 2 * s, x, y + 4 * s, x, y - 10 * s, x, y + 10 * s); break;
            case 1: std::snprintf(b, 200, "M%.1f,%.1f C%.1f,%.1f %.1f,%.1f %.1f,%.1f ", x - 4 * s, y - 8 * s, x + 6 * s, y - 8 * s, x + 6 * s, y + 6 * s, x - 2 * s, y + 6 * s); break;
            default: std::snprintf(b, 200, "M%.1f,%.1f L%.1f,%.1f M%.1f,%.1f C%.1f,%.1f %.1f,%.1f %.1f,%.1f ", x - 3 * s, y + 8 * s, x, y - 10 * s, x - 5 * s, y - 2 * s, x - 1 * s, y - 5 * s, x + 3 * s, y - 5 * s, x + 6 * s, y);
            }
            g += b;
        }
        glyphs = Path(g.c_str());
    }
    static float P() { return gWonder < 0 ? -1 : (GetTickCount64() - gWonderAt) / kWonderLen; }
    D2D1_RECT_F Bounds(double) override { float p = P(); if (p < 0 || p > 1.1f) { if (p > 1.1f) gWonder = -1; return Empty(); } return RectF(ART_L - 200, -200, ART_R + 200, 1300); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float p = P(); if (p < 0 || p > 1.1f) return;
        float a = std::min(1.f, std::min(p * 6, (1.1f - p) * 6));
        gCtx->SetTransform(art);
        Solid(gWonder == 0 ? 0x070A14 : 0x050203, .9f * a);
        gCtx->FillRectangle(RectF(ART_L - 200, -200, ART_R + 200, 1300), gBrush.Get());
        if (gWonder == 0) {
            float open = std::clamp((p - .45f) / .35f, 0.f, 1.f);
            if (open > 0) { Solid(0xFFD98A, .55f * a * open); gCtx->FillRectangle(RectF(760 + 200 * (1 - open), 330, 1160 - 200 * (1 - open), 900), gBrush.Get()); }
            float draw = std::clamp(p / .4f, 0.f, 1.f);
            Solid(0xCFE0FF, .3f * a * draw); gCtx->DrawGeometry(arch.Get(), gBrush.Get(), 12, Glint::gRound.Get());
            Solid(0xF2F6FF, a * draw); gCtx->DrawGeometry(arch.Get(), gBrush.Get(), 3, Glint::gRound.Get());
            float tr = std::clamp((p - .15f) / .3f, 0.f, 1.f);
            Solid(0xF2F6FF, a * tr); gCtx->DrawGeometry(trees.Get(), gBrush.Get(), 2.4f, Glint::gRound.Get());
            Solid(0xF2F6FF, a * std::clamp((p - .4f) * 6, 0.f, 1.f)); gCtx->FillGeometry(star.Get(), gBrush.Get());
            if (gDWrite && p > .25f) DrawWords(L"Mellon", L"friend, and the doors open", 0xF2F6FF, a * std::clamp((p - .25f) * 5, 0.f, 1.f), 960);
        } else {
            for (int i = 0; i < 3; i++) { Solid(0xD9A43A, (i == 1 ? 1.f : .45f) * a); gCtx->DrawEllipse(Ellipse(Point2F(960, 520), 330.f + (i - 1) * 9, 140.f + (i - 1) * 6), gBrush.Get(), i == 1 ? 26.f : 4.f); }
            Solid(0xFFF6D8, .6f * a); gCtx->DrawEllipse(Ellipse(Point2F(960, 512), 322, 132), gBrush.Get(), 3);
            float flick = .75f + .25f * std::sin((float)t * 9);
            float lit = std::clamp(p / .5f, 0.f, 1.f);
            Solid(0xFF6A1A, .45f * a * lit * flick); gCtx->DrawGeometry(glyphs.Get(), gBrush.Get(), 6, Glint::gRound.Get());
            Solid(0xFFE08A, a * lit * flick); gCtx->DrawGeometry(glyphs.Get(), gBrush.Get(), 2, Glint::gRound.Get());
            if (gDWrite && p > .6f) DrawWords(L"", L"Best put it away again.", 0xE8B66A, a * std::clamp((p - .6f) * 5, 0.f, 1.f), 960);
        }
    }
    static void DrawWords(const wchar_t* big, const wchar_t* small, uint32_t col, float o, float cx) {
        auto one = [&](const wchar_t* s, float size, float y) {
            if (!*s) return;
            ComPtr<IDWriteTextFormat> f;
            gDWrite->CreateTextFormat(L"Cormorant Garamond", gFonts.Get(), DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_ITALIC, DWRITE_FONT_STRETCH_NORMAL, size, L"en", &f);
            if (!f) return;
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            Solid(col, o);
            gCtx->DrawTextW(s, (UINT32)wcslen(s), f.Get(), RectF(cx - 600, y, cx + 600, y + size * 1.5f), gBrush.Get());
        };
        one(big, 64, 930); one(small, 34, big[0] ? 1006 : 760);
    }
};

static const wchar_t* kSceneKey[] = { L"shire", L"redbook", L"starlit", L"elven", L"lorien", L"havens" };
static const wchar_t* kWallName[] = { L"Lembas - Shire Morning.jpg", L"Lembas - Red Book.jpg", L"Lembas - Starlit Night.jpg", L"Lembas - Elven Silver.jpg", L"Lembas - Lothlorien.jpg", L"Lembas - The Grey Havens.jpg" };
static const wchar_t* kLayerName[] = { L"back", L"mid", L"front" };
static const wchar_t* kSeasonName[] = { L"spring", L"summer", L"autumn", L"winter" };

// The season, by the month and your hemisphere (the same rule as the phone and the watch).
static int SeasonNow() {
    SYSTEMTIME st; GetLocalTime(&st);
    static const int north[] = { 3, 3, 0, 0, 0, 1, 1, 1, 2, 2, 2, 3 };  // Jan..Dec
    int s = north[st.wMonth - 1];
    return gLatitude < 0 ? (s + 2) % 4 : s;
}

struct Scene {
    SceneId id = NONE;
    int season = 1, weather = 0;
    std::vector<Element*> els;
    std::vector<int> layers;  // which of back/mid/front exist
    ~Scene() { for (auto* e : els) delete e; }
};
static Scene* gScene;

// --- Middle-earth's calendar
static void FeastFlags(bool& fireworks, bool& lanterns, bool& durin);   // (below, with the Shire calendar)
struct Firework : Element {  // Hobbit Day: Gandalf's fireworks over the scene, clear of the clock
    int slot;
    explicit Firework(int s) : slot(s) { smooth = true; }
    void Burst(double t, int& n, float& p, float& cx, float& cy, uint32_t& col) {
        float dur = 2.6f, off = slot * .87f;
        n = (int)std::floor((t + off) / dur); p = (float)std::fmod(t + off, (double)dur) / dur;
        Rng r((uint32_t)(n * 7 + slot));
        cx = 1000 + 1200 * (float)r(); cy = 150 + 330 * (float)r();
        static const uint32_t cols[5] = { 0xFFD27A, 0x9FE6A0, 0xFF8A7A, 0xFFFFFF, 0xB9C8FF };
        col = cols[(int)(r() * 5) % 5];
    }
    D2D1_RECT_F Bounds(double t) override {
        int n; float p, cx, cy; uint32_t col; Burst(t, n, p, cx, cy, col);
        return p < .18f ? RectF(cx - 4, cy - 4, cx + 4, cy + 230) : RectF(cx - 200, cy - 200, cx + 200, cy + 240);
    }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        int n; float p, cx, cy; uint32_t col; Burst(t, n, p, cx, cy, col);
        gCtx->SetTransform(art);
        if (p < .18f) { float k = p / .18f; Solid(0xFFF0C4, .86f); gCtx->FillEllipse(Ellipse(Point2F(cx, cy + (1 - k) * 220), 2.2f, 2.2f), gBrush.Get()); return; }
        float k = (p - .18f) / .82f, reach = 170 * (1 - (1 - k) * (1 - k));
        Rng r((uint32_t)(n * 13 + slot));
        Solid(col, 1 - k);
        for (int i = 0; i < 26; i++) {
            float a = i / 26.f * 2 * PI + (float)r() * .2f;
            gCtx->FillEllipse(Ellipse(Point2F(cx + std::cos(a) * reach, cy + std::sin(a) * reach + k * k * 40), 2.4f - 1.2f * k, 2.4f - 1.2f * k), gBrush.Get());
        }
    }
};
struct Lanterns : Element {  // Yule and Lithe: paper lanterns strung across the sky, glowing
    Lanterns() { slow = true; rate = 8; }
    D2D1_RECT_F Bounds(double) override { return RectF(ART_L, 30, ART_R, 150); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        static const uint32_t cols[4] = { 0xFFC867, 0xFF9A6A, 0xFFE08A, 0xF2B8C6 };
        gCtx->SetTransform(art);
        float y0 = 60, sag = 27, w = ART_R - ART_L;
        Solid(0xFFFFFF, .6f);
        for (int i = 0; i < 60; i++) { float x1 = ART_L + w * i / 60, x2 = ART_L + w * (i + 1) / 60, f1 = (x1 - ART_L) / w, f2 = (x2 - ART_L) / w;
            gCtx->DrawLine(Point2F(x1, y0 + sag * 4 * f1 * (1 - f1)), Point2F(x2, y0 + sag * 4 * f2 * (1 - f2)), gBrush.Get(), 1.2f); }
        int n = 36;
        for (int i = 1; i < n; i++) {
            float x = ART_L + w * i / n, f = (x - ART_L) / w, y = y0 + sag * 4 * f * (1 - f) + 12;
            float s = std::sin((float)t * 1.3f + i), k = .7f + .3f * s * s;
            Solid(cols[i % 4], .24f * k); gCtx->FillEllipse(Ellipse(Point2F(x, y), 18, 18), gBrush.Get());
            Solid(cols[i % 4], .92f * k); gCtx->FillEllipse(Ellipse(Point2F(x, y), 7, 9), gBrush.Get());
            Solid(0x4A3A28, 1); gCtx->DrawLine(Point2F(x - 4, y - 9.5f), Point2F(x + 4, y - 9.5f), gBrush.Get(), 1.4f); gCtx->DrawLine(Point2F(x - 4, y + 9.5f), Point2F(x + 4, y + 9.5f), gBrush.Get(), 1.4f);
        }
    }
};
struct MoonLetters : Element {  // Durin's Day: moon-letters glowing silver-blue on the Red Book's page
    std::vector<ComPtr<ID2D1PathGeometry>> runes;
    MoonLetters() {
        slow = true; rate = 6;
        Rng r(1937);
        for (int i = 0; i < 26; i++) {
            std::string d = "M0,0 L0,22";
            int branches = 1 + (int)(r() * 2);
            for (int b = 0; b < branches; b++) {
                float y = 2 + (float)r() * 14, side = r() < .5 ? 1.f : -1.f, len = 5 + (float)r() * 4, dy = r() < .5 ? 6.f : -4.f;
                char seg[64]; std::snprintf(seg, sizeof seg, " M0,%.1f L%.1f,%.1f", y, side * len, y + dy); d += seg;
            }
            runes.push_back(Path(d.c_str()));
        }
    }
    D2D1_RECT_F Bounds(double) override { return RectF(1040, 320, 1720, 470); }
    void Draw(double t, const D2D1_MATRIX_3X2_F& art) override {
        float k = .45f + .3f * (.5f + .5f * std::sin((float)t * .7f));
        for (size_t i = 0; i < runes.size(); i++) {
            float x = 1060 + (i % 13) * 48.f, y = 330 + (i / 13) * 70.f;
            gCtx->SetTransform(Matrix3x2F::Scale(2, 2) * Matrix3x2F::Translation(x, y) * art);
            Solid(0xCFE6FF, .24f * k); gCtx->DrawGeometry(runes[i].Get(), gBrush.Get(), 3.4f, Glint::gRound.Get());
            Solid(0xCFE6FF, .9f * k); gCtx->DrawGeometry(runes[i].Get(), gBrush.Get(), 1.1f, Glint::gRound.Get());
        }
    }
};

// Where each look's sky moods go: behind the land, on the far hills, over everything.
static void AddSky(std::vector<Element*>& E, SceneId id) {
    auto at = [&](int layer) { for (size_t i = 0; i < E.size(); i++) if (E[i]->layer == layer) return (int)i; return -1; };
    auto insertBefore = [&](int layer, Element* e) { int i = at(layer); if (i < 0) E.push_back(e); else E.insert(E.begin() + i, e); };
    bool outdoors = id != REDBOOK;
    int midOrFront = at(1) >= 0 ? 1 : 2;
    if (gSky.rainbow && (id == SHIRE || id == LORIEN)) insertBefore(midOrFront, new Rainbow(1200, 1000, 800));
    if (gSky.storm && outdoors) { insertBefore(midOrFront, new Bolt(false)); E.push_back(new Bolt(true)); }
    if (gSky.beacons && outdoors) {
        std::vector<std::pair<float, float>> spots;
        if (id == SHIRE) spots = { {-220, 488}, {380, 480}, {1020, 492}, {1660, 484}, {2240, 490} };
        else if (id == STARLIT) spots = { {640, 540}, {1180, 498}, {1500, 610}, {-300, 570} };
        else if (id == ELVEN) spots = { {300, 530}, {1700, 520}, {-200, 560} };
        else if (id == HAVENS) spots = { {300, 690}, {1700, 700}, {900, 730} };
        int i = 0;
        for (auto& sp : spots) insertBefore(2, new Beacon(sp.first, sp.second, (float)i++));
    }
    if (gSky.heat && outdoors) for (int k = 0; k < 4; k++) insertBefore(2, new Shimmer(540.f + k * 22, k * 1.7f));
    if (gSky.frost && outdoors) {
        E.push_back(new Haze({ {0, Hex(0xE8F2FF, 0)}, {1, Hex(0xE8F2FF, .3f)} }, 560, 1080));
        Rng r(91);
        for (int i = 0; i < 70; i++) E.push_back(new Sparkle(frand10(ART_L + r() * (ART_R - ART_L)), frand10(560 + r() * 520), frand10(1 + r() * 1.6), frand10(3 + r() * 4), frand10(r() * 4)));
    }
    if (gSky.smoke > 0) {
        bool warm = id == SHIRE || id == REDBOOK || id == LORIEN;
        uint32_t c = warm ? 0xC0905A : 0x6A5040; float a = gSky.smoke * .43f;
        E.push_back(new Haze({ {0, Hex(c, a / 3)}, {.52f, Hex(c, a)}, {1, Hex(c, a * 2 / 3)} }, 0, 1080));
    }
    if (id == REDBOOK && (gWx == 3 || gWx == 5)) {
        E.push_back(new Haze({ {0, Hex(0x8AA0C0, .08f)}, {1, Hex(0x8AA0C0, .08f)} }, 0, 1080));
        Rng r(92);
        for (int i = 0; i < 12; i++) E.push_back(new WindowDrop(frand10(ART_L + r() * (ART_R - ART_L)), frand10(24 + r() * 32), frand10(r() * 14), frand10(4 + r() * 5)));
    }
    bool fireworks, lanterns, durin;
    FeastFlags(fireworks, lanterns, durin);
    if (gSky.rainbow && (id == SHIRE || id == LORIEN)) Deed("rainbow");
    if (gSky.storm && outdoors) Deed("storm");
    if (gSky.beacons && outdoors) Deed("beacons");
    if (gSky.frost && outdoors) Deed("frost");
    if (fireworks) Deed("fireworks");
    if (lanterns) Deed("lanterns");
    if (durin && id == REDBOOK) Deed("durin");
    if (gSeasonTurn) Deed("turning");
    if (lanterns) E.push_back(new Lanterns());
    if (durin && id == REDBOOK) E.push_back(new MoonLetters());
    if (fireworks) for (int k = 0; k < 3; k++) E.push_back(new Firework(k));
    if (gSeasonTurn) {
        int sn = SeasonNow();
        Rng r(2026);
        for (int i = 0; i < 48; i++) {
            uint32_t col = sn == 3 ? 0xFFFFFF : sn == 0 ? (i % 3 ? 0xF2B8C6 : 0xFFFFFF) : sn == 2 ? (i % 3 == 0 ? 0xD9843A : i % 3 == 1 ? 0xE8A646 : 0xC8562E) : 0xFFF6D8;
            E.push_back(new SeasonBit(frand10(r() * (ART_R - ART_L)), frand10(r() * 1100), frand10(.6 + r() * .8), frand10(r() * 6.28), sn == 3 ? 3 : sn == 1 ? 1 : sn == 2 ? 2 : 0, col));
        }
        gSeasonTurn = false;
    }
}

static void AddWeather(std::vector<Element*>& E, SceneId id) {
    if (id == REDBOOK) return;  // a map indoors: no weather
    Rng r(77);
    if (gWx == 1 || gWx == 5) {  // cloud
        uint32_t c = id == SHIRE ? 0xF2F4F4 : 0x3A4266;
        float o = id == SHIRE ? .85f : .55f;
        E.push_back(new Cloud(200, 90, 1.6f, o, 300, c));
        E.push_back(new Cloud(1100, 160, 1.3f, o, 360, c));
        E.push_back(new Cloud(1900, 70, 1.8f, o, 420, c));
    }
    if (gWx == 2) {  // fog
        E.push_back(new Mist(560, 260));
        E.push_back(new Mist(760, 320));
    }
    if (gWx == 3 || gWx == 5) for (int i = 0; i < 90; i++)
        E.push_back(new Drop(frand10(ART_L + r() * (ART_R - ART_L + 200)), frand10(.8 + r() * .5), frand10(r() * 2), frand10(30 + r() * 30)));
    if (gWx == 4) for (int i = 0; i < 80; i++)
        E.push_back(new Flake(frand10(ART_L + r() * (ART_R - ART_L)), frand10(10 + r() * 9), frand10(r() * 20), frand10(1.6 + r() * 2.2)));
}

static Scene* BuildScene(SceneId id) {
    Scene* s = new Scene();
    s->id = id;
    s->season = SeasonNow();
    s->weather = gWx;
    auto& E = s->els;
    if (id == SHIRE) {
        E.push_back(new LayerEl(0));
        E.push_back(new Glow(1390, 250, 300, { {0, Hex(0xFFF6D8, 1)}, {.28f, Hex(0xFFEFC0, 1)}, {.29f, Hex(0xFFE7A6, .55f)}, {1, Hex(0xFFE7A6, 0)} }, 6));
        if (gWx != 5) {
            E.push_back(new Cloud(1640, 190, 1, .9f, 260)); E.push_back(new Cloud(980, 140, .7f, .75f, 320));
            E.push_back(new Cloud(1240, 420, .55f, .8f, 380)); E.push_back(new Cloud(320, 470, .6f, .7f, 300));
            E.push_back(new Cloud(-260, 260, .8f, .8f, 340));
        }
        E.push_back(new LayerEl(1));
        const float g[6][3] = { {140, 775, 70}, {520, 792, 40}, {1010, 771, 90}, {1560, 752, 50}, {-300, 768, 60}, {2100, 770, 70} };
        for (int i = 0; i < 6; i++) E.push_back(new Glint(g[i][0], g[i][1], g[i][2], 4.f + i % 3, i * 1.3f, 3, 0xFFFFFF));
        E.push_back(new LayerEl(2));
        for (int k = 0; k < 4; k++) E.push_back(new Smoke(1392, 806, 12, k * 1.25f));
        s->layers = { 0, 1, 2 };
    } else if (id == REDBOOK) {
        Rng r(1937);
        E.push_back(new LayerEl(0));
        for (int i = 0; i < 18; i++) {
            auto* m = new Mote();
            m->x = frand10(ART_L + 100 + r() * (ART_R - ART_L - 200)); m->y = frand10(500 + r() * 520); m->dur = frand10(10 + r() * 9); m->off = frand10(r() * 16);
            E.push_back(m);
        }
        E.push_back(new Glow(1210, 786, 150, { {0, Hex(0xFFC06A, .6f)}, {.4f, Hex(0xFFB25B, .25f)}, {1, Hex(0xFFB25B, 0)} }, 2.2f, 0, 1, 1.04f, .7f, 1));
        E.push_back(new Flame("M0,-118 C34,-72 46,-46 30,-6 C22,14 -22,14 -30,-6 C-44,-40 -18,-62 0,-118 Z", 1.3f, 0, 0xE8762A));
        E.push_back(new Flame("M2,-74 C22,-46 26,-30 16,-8 C10,4 -12,4 -16,-8 C-24,-30 -8,-44 2,-74 Z", 1.05f, .4f, 0xF7B24A));
        E.push_back(new Flame("M2,-40 C12,-26 12,-16 7,-6 C4,0 -6,0 -8,-6 C-11,-16 -4,-24 2,-40 Z", .8f, .2f, 0xFFF0B0));
        E.push_back(new Needle());
        s->layers = { 0 };
    } else if (id == STARLIT) {
        Rng r(1954);
        E.push_back(new LayerEl(0));
        for (int i = 0; i < 46; i++) {
            auto* t = new Twinkle();
            t->x = frand10(ART_L + r() * (ART_R - ART_L)); t->y = frand10(r() * 430); t->r = frand10(1.4 + r() * 1.2);
            t->dur = frand10(2 + r() * 3.5); t->off = frand10(r() * 5);
            E.push_back(t);
        }
        E.push_back(new RealSky(430, 640));
        E.push_back(new Shooting());
        E.push_back(new Moon(1480, 230, 54));
        const float g[6][3] = { {-80, 0, 160}, {-60, 28, 120}, {-44, 58, 88}, {-30, 94, 60}, {-20, 136, 40}, {-12, 186, 24} };
        for (int i = 0; i < 6; i++) E.push_back(new Glint(1480 + g[i][0], 812 + g[i][1], g[i][2], 3 + i * .5f, i * .6f, 4, 0xFFF3D6));
        E.push_back(new Mist(820, 90, 0xB9C2EE, .22f, 20));   // mist drifting on the water
        E.push_back(new LayerEl(2));
        E.push_back(new CabinWindow(600, 905));
        s->layers = { 0, 2 };
    } else if (id == ELVEN) {
        Rng r(1955);
        E.push_back(new LayerEl(0));
        E.push_back(new RealSky(470, 640));
        E.push_back(new Glow(1160, 150, 90, { {0, Hex(0xBFE3EC, .5f)}, {1, Hex(0xBFE3EC, 0)} }, 6));
        E.push_back(new Mist(640, 140));
        E.push_back(new LayerEl(1));
        const float lamps[7][2] = { {880, 760}, {1030, 700}, {1180, 740}, {960, 820}, {1110, 800}, {1250, 830}, {760, 820} };
        for (int i = 0; i < 7; i++) E.push_back(new Lamp(lamps[i][0], lamps[i][1], 3.f + i % 3, i * .9f));
        for (int i = 0; i < 18; i++) {
            auto* m = new Rising();
            m->x = frand10(640 + r() * 760); m->y = frand10(900 + r() * 120); m->dur = frand10(9 + r() * 8); m->off = frand10(r() * 16);
            E.push_back(m);
        }
        s->layers = { 0, 1 };
    }
    if (id == LORIEN) {
        Rng r(86);
        E.push_back(new LayerEl(0));
        E.push_back(new LayerEl(1));
        const float lamps[9][2] = { {0, 515}, {250, 515}, {1440, 455}, {1730, 455}, {1700, 695}, {2150, 555}, {860, 640}, {1000, 700}, {1180, 620} };
        for (int i = 0; i < 9; i++) E.push_back(new Lamp(lamps[i][0], lamps[i][1], 3.f + i % 3, i * .9f, true));
        int sn = SeasonNow();
        uint32_t lc[3] = { 0xE9B23A, 0xF6D06A, 0xC98A2A };
        if (sn == 0) { lc[0] = 0xE8C45A; lc[1] = 0xF2D98A; lc[2] = 0xC9A23A; } else if (sn == 1) { lc[0] = 0xB9C46A; lc[1] = 0xD9D08A; lc[2] = 0x8FA64E; }
        for (int i = 0; i < 16; i++) E.push_back(new Leaf(frand10(ART_L + r() * (ART_R - ART_L)), frand10(9 + r() * 6), frand10(r() * 12), frand10(r() * 6.28), lc[i % 3]));
        E.push_back(new LayerEl(2));
        s->layers = { 0, 1, 2 };
    } else if (id == HAVENS) {
        Rng r(1956);
        E.push_back(new LayerEl(0));
        for (int i = 0; i < 40; i++) {
            auto* t = new Twinkle();
            t->x = frand10(ART_L + r() * (ART_R - ART_L)); t->y = frand10(r() * 560); t->r = frand10(1.3 + r() * 1.1);
            t->dur = frand10(2 + r() * 3.5); t->off = frand10(r() * 5);
            E.push_back(t);
        }
        E.push_back(new RealSky(680, 760));
        E.push_back(new Shooting());
        E.push_back(new Moon(1480, 210, 46));
        const float g[5][3] = { {-70, 14, 140}, {-50, 44, 100}, {-36, 80, 72}, {-24, 124, 48}, {-14, 176, 30} };
        for (int i = 0; i < 5; i++) E.push_back(new Glint(1480 + g[i][0], 760 + g[i][1], g[i][2], 3.2f + i * .5f, i * .7f, 4, 0xF2F4FA));
        E.push_back(new LayerEl(1));
        E.push_back(new Glow(2010, 650 - 197 * .9f, 54, { {0, Hex(0xFFF2D0, .7f)}, {1, Hex(0xFFF2D0, 0)} }, 4.5f, 0, .9f, 1.1f, .7f, 1));
        E.push_back(new Ship(1180, 860));
        E.push_back(new LayerEl(2));
        s->layers = { 0, 1, 2 };
    }
    AddWeather(E, id);
    AddSky(E, id);
    if (id == SHIRE) E.push_back(new Visitor(0));
    if (id == STARLIT) E.push_back(new Visitor(1));
    if (id == ELVEN) E.push_back(new Visitor(2));
    E.push_back(new Fireflies(id));
    E.push_back(new Wonder());
    if (id != REDBOOK) E.push_back(new Light());
    return s;
}

static void InitShared() {
    gFactory->CreateStrokeStyle(StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &Glint::gRound);
    Lamp::glow = Radial({ {0, Hex(0xF3E3B4, .45f)}, {1, Hex(0xF3E3B4, 0)} });
    Lamp::goldGlow = Radial({ {0, Hex(0xFFE7A8, .6f)}, {1, Hex(0xFFE7A8, 0)} });
    Beacon::glow = Radial({ {0, Hex(0xFF9A3A, .8f)}, {1, Hex(0xFF7A2A, 0)} });
    Moon::glow = Radial({ {0, Hex(0xFFF7E0, .55f)}, {1, Hex(0xFFF7E0, 0)} });
    // cloud = one shape (so overlapping puffs don't double up their transparency)
    ComPtr<ID2D1RoundedRectangleGeometry> rr;
    ComPtr<ID2D1EllipseGeometry> c1, c2, c3;
    gFactory->CreateRoundedRectangleGeometry(RoundedRect(RectF(-90, -8, 110, 26), 17, 17), &rr);
    gFactory->CreateEllipseGeometry(Ellipse(Point2F(-30, -6), 32, 32), &c1);
    gFactory->CreateEllipseGeometry(Ellipse(Point2F(20, -22), 42, 42), &c2);
    gFactory->CreateEllipseGeometry(Ellipse(Point2F(66, -2), 26, 26), &c3);
    ID2D1Geometry* parts[4] = { rr.Get(), c1.Get(), c2.Get(), c3.Get() };
    ComPtr<ID2D1GeometryGroup> grp;
    gFactory->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, parts, 4, &grp);
    Cloud::shape = grp;
}
static void ReleaseShared() {
    Glint::gRound.Reset(); Lamp::glow.Reset(); Lamp::goldGlow.Reset(); Beacon::glow.Reset(); Cloud::shape.Reset(); Moon::glow.Reset();
}

// ------------------------------------------------------------------------------------ settings
struct Settings { bool clock = true, meals = true, enabled = true, adaptive = true, modern = true, weather = true, span = true, pinned = false, greeting = true, fireflies = true; int tengwar = 0; double lat = -33.87, lon = 151.21; std::wstring look = L"adaptive"; } gSet;

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
    if (JsonValue(j, L"look", v)) for (auto* k : { L"shire", L"redbook", L"starlit", L"elven", L"lorien", L"havens" }) if (v.find(k) != std::wstring::npos) gSet.look = k;
    // the clock in the watch face's type (stacked heavy hours and light minutes), unless set to classic
    gSet.modern = !(JsonValue(j, L"desktopClockStyle", v) && v.find(L"classic") != std::wstring::npos);
    gSet.clock = !(JsonValue(j, L"desktopClock", v) && v == L"false");
    gSet.meals = !(JsonValue(j, L"meals", v) && v == L"false");
    gSet.enabled = !(JsonValue(j, L"livingDesktop", v) && v == L"false");
    gSet.weather = !(JsonValue(j, L"weather", v) && v == L"false");
    gSet.span = !(JsonValue(j, L"spanScreens", v) && v == L"false");      // one view across side-by-side screens
    gSet.pinned = JsonValue(j, L"panelPinned", v) && v == L"true";        // keep today's panel on the desktop
    gSet.greeting = !(JsonValue(j, L"greeting", v) && v == L"false");      // a Sindarin greeting under the clock
    gSet.fireflies = gFirefliesOn = !(JsonValue(j, L"fireflies", v) && v == L"false");    // fireflies while music plays
    gSet.tengwar = !JsonValue(j, L"tengwar", v) ? 0 : v.find(L"both") != std::wstring::npos ? 2 : v.find(L"date") != std::wstring::npos ? 1 : 0;
    gLatitude = gSet.lat; gLongitude = gSet.lon;
}
static SceneId gForced = NONE;  // --look <name>: for testing one look
static SceneId SceneFromWallpaper() {
    if (gForced != NONE) return gForced;
    wchar_t wall[MAX_PATH * 2] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH * 2, wall, 0);
    std::wstring w = wall;
    for (int i = 0; i < (int)(sizeof kWallName / sizeof *kWallName); i++) {
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
// The light of the hour, the moon's height and the night's events: worked out once a minute.
// Returns true if anything visible changed (then the whole scene is redrawn once).
static bool UpdateLight(int scene) {
    SYSTEMTIME st; GetLocalTime(&st);
    SunDay d = SunToday(st, gSet.lat, gSet.lon);
    int m = st.wHour * 60 + st.wMinute;
    uint32_t col = 0; float a = 0, g = 0;
    if (scene == 0) {        // Shire: warmer as the afternoon goes on
        float f = std::min(1.f, std::max(0.f, (float)(m - d.noon) / std::max(1, d.evening - d.noon)));
        col = 0xFFB45A; a = .10f * f;
    } else if (scene == 2) { // Starlit: deeper toward 11pm and in the small hours
        int fromDusk = m - d.set; if (fromDusk < 0) fromDusk += 1440;
        int toDawn = d.rise - m; if (toDawn < 0) toDawn += 1440;
        col = 0x050A26; a = .25f * std::min(1.f, std::min(fromDusk, toDawn) / 150.f);
    }
    if (scene != 1) {        // sunrise and sunset: a glow from the horizon
        int nearMin = std::min(std::abs(m - d.rise), std::abs(m - d.set));
        if (nearMin < 20) g = 1 - nearMin / 20.f;
    }
    // the moon, where it really is tonight: its height, phase and distance, and Earth's shadow on it
    Heavens::MoonNow mn = Heavens::Moon(gSet.lat, gSet.lon);
    float alt = std::min(1.f, (float)(mn.pos.alt / 60.0));
    alt = std::round(alt * 40) / 40;   // moves in small steps (a redraw each ~15 minutes)
    bool full = mn.Full();
    double ecl = std::round(mn.eclipse * 10) / 10;
    bool moonChanged = std::fabs(mn.phase - gMoonPhase) > .004 || ecl != gEclipse || mn.Super() != gSuperMoon;
    gMoonPhase = mn.phase; gEclipse = ecl; gSuperMoon = mn.Super();
    bool meteors = (st.wMonth == 8 && st.wDay >= 11 && st.wDay <= 13) || (st.wMonth == 12 && st.wDay >= 13 && st.wDay <= 15);
    a = std::round(a * 100) / 100; g = std::round(g * 20) / 20;
    bool changed = col != gTintCol || a != gTintA || g != gGlowA || alt != gMoonAlt || full != gFullMoon || moonChanged;
    gTintCol = col; gTintA = a; gGlowA = g; gMoonAlt = alt; gFullMoon = full; gMeteors = meteors;
    return changed;
}


static bool DurinToday() {
    SYSTEMTIME st; GetLocalTime(&st); int m, d;
    return Heavens::DurinsDay(st.wYear, gSet.lat < 0, m, d) > 0 && m == st.wMonth && d == st.wDay;
}

static SceneId SunScene(const SYSTEMTIME& now) {
    SunDay d = SunToday(now, gSet.lat, gSet.lon);
    int m = now.wHour * 60 + now.wMinute;
    if (m >= d.rise && m < d.evening) return SHIRE;
    if (m >= d.evening && m < d.set) return SeasonNow() == 2 && !DurinToday() ? LORIEN : REDBOOK;   // autumn: Lothlórien (Durin's Day: the Red Book, its moon-letters lit)
    if (m >= 23 * 60 || m < 2 * 60) return ELVEN;
    if (m < d.rise) return HAVENS;
    return STARLIT;
}
// The weather Lembas Sun saved (weather.json: {"wx": 0..5, "at": unix seconds}); clear if it's off,
// missing or more than four hours old.
static int ReadWeather() {
    gWind = false; gSky = Sky(); gForecast.clear();
    if (!gSet.weather) return 0;
    wchar_t base[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    std::wstring j = ReadAll(std::wstring(base) + L"\\Lembas\\weather.json"), v;
    if (!JsonValue(j, L"wx", v)) return 0;
    int wx = _wtoi(v.c_str());
    std::wstring wv;
    bool wind = JsonValue(j, L"wind", wv) && _wtoi(wv.c_str()) > 30;
    std::wstring at;
    if (JsonValue(j, L"at", at)) {
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        double now = ((double)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000.0) / 1e7;
        if (now - _wtof(at.c_str()) > 4 * 3600) return 0;
    }
    gWind = wind;
    // the sky's moods (worked out each minute, so they come and go on time)
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    double nowS = ((double)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000.0) / 1e7;
    SYSTEMTIME lt; GetLocalTime(&lt);
    auto num = [&](const wchar_t* k, double d) { std::wstring x; return JsonValue(j, k, x) ? _wtof(x.c_str()) : d; };
    double temp = num(L"temp", 15), rainAt = num(L"rainAt", 0), pm = num(L"pm25", 0), pmAt = num(L"pmAt", 0);
    bool day = num(L"day", 1) > .5;
    gSky.rainbow = day && (wx == 0 || wx == 1) && rainAt > 0 && nowS - rainAt < 50 * 60;
    gSky.frost = temp <= 2 && lt.wHour >= 4 && lt.wHour <= 10 && wx != 3;
    gSky.heat = day && temp >= 32 && lt.wHour >= 11 && lt.wHour <= 18;
    gSky.beacons = num(L"severe", 0) > .5;
    gSky.storm = wx == 5;
    gSky.smoke = nowS - pmAt < 3 * 3600 ? std::min(1.0, std::max(0.0, (pm - 25) / 75)) : 0.f;
    std::wstring fc;
    if (JsonValue(j, L"forecast", fc)) gForecast = fc;
    return std::min(5, std::max(0, wx));
}

static void CheckSun() {
    static int lastMinute = -1, sinceReload = 0;
    static ULONGLONG lastKick = 0;
    SYSTEMTIME st; GetLocalTime(&st);
    int minute = st.wHour * 60 + st.wMinute;
    if (minute == lastMinute) return;
    lastMinute = minute;
    if (++sinceReload >= 10) { sinceReload = 0; LoadSettings(); }  // the time zone (and so the city) may have changed
    static Sky lastSky;
    static int lastFeast = -1;
    { bool f1, f2, f3; FeastFlags(f1, f2, f3); int fk = f1 | (f2 << 1) | (f3 << 2); if (gScene && lastFeast >= 0 && fk != lastFeast) gRescene = true; lastFeast = fk; }
    gWx = ReadWeather();
    static bool lastWind = false;
    if (gScene && gScene->season != SeasonNow()) gSeasonTurn = true;   // a new season, while you watch
    if (gScene && (gScene->season != SeasonNow() || gScene->weather != gWx || gWind != lastWind || gSky != lastSky)) gRescene = true;
    lastSky = gSky;
    lastWind = gWind;
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
    D2D1_MATRIX_3X2_F art;  // design units (the 1920x1080 design; the artboard runs -480..2400) -> canvas
    D2D1_POINT_2F org;      // where the artboard's top left lands, in canvas pixels
    UINT bw, bh;            // size of the whole artboard on this monitor (it is cropped like "fill")
    ComPtr<ID2D1Bitmap1> layer[3];
};
static std::vector<Mon> gMons;

static void SetupMon(Mon& m, RECT rc) {
    m.rc = rc;
    float W = (float)(rc.right - rc.left), H = (float)(rc.bottom - rc.top);
    // The artboard is 2880 wide: a 16:9 screen shows its middle 1920 (the design), an ultrawide
    // more of the panorama, and a taller screen is filled by height. Centred, anchored to the bottom.
    float s = std::max(W / 2880.f, H / 1080.f);
    m.bw = (UINT)std::ceil(2880 * s);
    m.bh = (UINT)std::ceil(1080 * s);
    float ox = std::round((W - m.bw) / 2), oy = H - m.bh;
    m.org = Point2F(rc.left + ox, rc.top + oy);
    m.art = Matrix3x2F(m.bw / 2880.f, 0, 0, m.bh / 1080.f, rc.left + ox + 480 * (m.bw / 2880.f), rc.top + oy);
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
    bool shared = gSet.span && gMons.size() > 1;   // one panorama across the screens: load it once
    for (size_t k = 0; k < gMons.size(); k++) {
        auto& m = gMons[k];
        for (auto& l : m.layer) l.Reset();
        if (!gScene) continue;
        if (shared && k > 0) { for (int i = 0; i < 3; i++) m.layer[i] = gMons[0].layer[i]; continue; }
        for (int i : gScene->layers) {
            std::wstring ext = i == 0 ? L".jpg" : L".png";
            std::wstring name = std::wstring(kSceneKey[gScene->id]) + L"-" + kLayerName[i] + ext;
            // this season's layers, else the plain ones
            m.layer[i] = LoadLayer(gDir + L"\\" + kSeasonName[gScene->season] + L"\\" + name, m.bw, m.bh);
            if (!m.layer[i]) m.layer[i] = LoadLayer(gDir + L"\\" + name, m.bw, m.bh);
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
static void FeastFlags(bool& fireworks, bool& lanterns, bool& durin) {
    SYSTEMTIME st; GetLocalTime(&st);
    int day; std::wstring name; ShireDate(st, day, name);
    fireworks = st.wMonth == 9 && st.wDay == 22 && st.wHour >= 18;                 // Hobbit Day, from 6pm
    lanterns = day == 0 && (name.find(L"Yule") != std::wstring::npos || name.find(L"Lithe") != std::wstring::npos || name.find(L"year") != std::wstring::npos);
    durin = DurinToday();
}
// The next of Middle-earth's dates: "Hobbit Day in 12 days", "Durin's Day tomorrow".
static std::wstring FeastCountdown() {
    SYSTEMTIME st; GetLocalTime(&st);
    int64_t today = DaysFromCivil(st.wYear, st.wMonth, st.wDay), best = INT64_MAX; std::wstring bestName;
    auto consider = [&](const wchar_t* n, int y, int m, int d) { int64_t v = DaysFromCivil(y, m, d); if (v >= today && v < best) { best = v; bestName = n; } };
    for (int y = st.wYear; y <= st.wYear + 1; y++) {
        consider(L"The Professor\u2019s birthday", y, 1, 3);
        consider(L"Hobbit Day", y, 9, 22);
        consider(L"Yule", y, 12, 21);
        for (int d = 15; d <= 30; d++) { SYSTEMTIME s2 = {}; s2.wYear = (WORD)y; s2.wMonth = 6; s2.wDay = (WORD)d; int dd; std::wstring nm; ShireDate(s2, dd, nm); if (nm.find(L"Mid-year") == 0) { consider(L"Mid-year\u2019s Day", y, 6, d); break; } }
        int m, d; if (Heavens::DurinsDay(y, gSet.lat < 0, m, d) > 0) consider(L"Durin\u2019s Day", y, m, d);
    }
    int64_t n = best - today;
    return bestName + (n == 0 ? L" today" : n == 1 ? L" tomorrow" : L" in " + std::to_wstring(n) + L" days");
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
    // a second style from `split` on (the light minutes after the heavy hours)
    int split = -1;
    uint32_t color2 = 0;
    DWRITE_FONT_WEIGHT weight2 = DWRITE_FONT_WEIGHT_LIGHT;
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
    if (l.split >= 0 && l.split < (int)l.text.size()) lay->SetFontWeight(l.weight2, DWRITE_TEXT_RANGE{ (UINT32)l.split, (UINT32)(l.text.size() - l.split) });
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
    std::wstring meal = (st.wMonth == 1 && st.wDay == 3 && h == 21) ? L"A toast: the Professor" : gSet.meals ? Meal(h) : L"";   // the Tolkien Society's toast, at 9pm
    if (st.wMonth == 1 && st.wDay == 3 && h == 21) Deed("toast");

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
    float leftX = 9;  // the left edge for a left-aligned clock, in vw
    float top = 15;
    uint32_t firstColor = 0;
    std::wstring ampm;

    if (gSet.modern) {
        // The corner clock, as on the phone and the watch: the day and the Quenya hour, then the time
        // (heavy hours, light minutes), the mealtime, and the Shire and everyday dates. Top left, out
        // of the picture's way.
        static const uint32_t inkC[] = { 0x27452A, 0x2B1E12, 0xF4ECD8, 0xE6ECEE, 0xFFF6E0, 0xF2F4FA };
        static const uint32_t accC[] = { 0x3E6A3A, 0x9A2A1C, 0xE2C687, 0xB9C6CA, 0xF6D27A, 0xC9D4EA };
        static const uint32_t softC[] = { 0x55703F, 0x6A5236, 0x9C927B, 0x7D898D, 0xE8C8A0, 0x9AA8C0 };
        int k = sc < 0 ? 2 : (int)sc;
        int hh = h24 ? h : ((h % 12) ? h % 12 : 12);
        wchar_t hb[8], mb[8];
        std::swprintf(hb, 8, h24 ? L"%02d" : L"%d", hh);
        std::swprintf(mb, 8, L":%02d", st.wMinute);
        L.assign(4, Line());
        Line &d = L[0], &tm = L[1], &me = L[2], &dt = L[3];
        d.family = alegreya; d.weight = DWRITE_FONT_WEIGHT_MEDIUM; d.size = 1.6f; d.spacing = .24f; d.color = softC[k];
        d.text = Upper(wd + L" · " + q);
        tm.family = fraunces; tm.weight = DWRITE_FONT_WEIGHT_EXTRA_BOLD; tm.size = 13.9f; tm.time = true; tm.color = inkC[k]; tm.margin = .6f;
        tm.text = std::wstring(hb) + mb; tm.split = (int)wcslen(hb); tm.color2 = accC[k]; tm.weight2 = DWRITE_FONT_WEIGHT_LIGHT;
        me.family = garamond; me.style = DWRITE_FONT_STYLE_ITALIC; me.size = 3.15f; me.color = accC[k]; me.margin = .6f;
        me.text = meal;
        dt.family = alegreya; dt.weight = DWRITE_FONT_WEIGHT_MEDIUM; dt.size = 1.76f; dt.color = softC[k]; dt.margin = .2f;
        dt.text = shireTxt + L" · " + greg;
        left = true; leftX = 7.8f;
        top = 13.9f;
        shadow = sc == STARLIT || sc == ELVEN || sc == LORIEN || sc == HAVENS;
        time = L"";   // no am/pm suffix
        if (me.text.empty()) L.erase(L.begin() + 2);
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
    } else if (sc == LORIEN) {
        for (auto& l : L) l.color = 0xFFF6E0;
        lab.color = shi.color = 0xF6D27A; dat.color = 0xE8C8A0; mea.color = 0xF6D27A;
        shadow = true;
        lab.text = Upper(q + L" · " + en); shi.text = wd + L", " + shireTxt;
    } else if (sc == HAVENS) {
        for (auto& l : L) l.color = 0xF2F4FA;
        lab.color = shi.color = 0xC9D4EA; dat.color = 0x9AA8C0; mea.color = 0xC9D4EA;
        shadow = true;
        lab.text = Upper(q + L" · " + en); shi.text = wd + L", " + shireTxt;
    } else {  // Starlit Night
        for (auto& l : L) l.color = 0xF3EBD6;
        lab.color = shi.color = 0xD9BC7A; dat.color = 0xB9B19C;
        shadow = true;
        lab.text = Upper(q + L" · " + en); shi.text = wd + L", " + shireTxt;
    }
    if (!gSet.modern && mea.text.empty()) L.pop_back();

    // Tengwar, when chosen (Small touches): the date in Elvish letters, and the numerals too
    std::wstring tengwarFam = Family({ L"Alcarin Tengwar" });
    if (gSet.tengwar > 0 && tengwarFam != L"Georgia") {
        static const wchar_t* days[] = { L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday" };
        static const wchar_t* months[] = { L"January", L"February", L"March", L"April", L"May", L"June", L"July", L"August", L"September", L"October", L"November", L"December" };
        Line& dl = gSet.modern ? L.back() : L[3];
        dl.text = Tengwar::Text(std::wstring(days[st.wDayOfWeek]) + L" " + std::to_wstring(st.wDay) + L" " + months[st.wMonth - 1]);
        dl.family = tengwarFam; dl.weight = DWRITE_FONT_WEIGHT_NORMAL; dl.style = DWRITE_FONT_STYLE_NORMAL; dl.spacing = 0; dl.size *= 1.25f;
        if (gSet.tengwar > 1) {
            Line& tl = L[1];
            std::wstring digits = ampm.empty() ? tl.text : time;
            tl.text = Tengwar::Text(digits) + (ampm.empty() ? L"" : ampm);
            if (!ampm.empty()) time = Tengwar::Text(time);
            tl.family = tengwarFam; tl.weight = tl.weight2 = DWRITE_FONT_WEIGHT_NORMAL; tl.spacing = 0; tl.size *= .9f;
        }
    }
    // a Sindarin greeting for the hour, English after it
    if (gSet.greeting) {
        std::wstring el, en; Greeting(h, el, en);
        Line g; g.family = garamond; g.style = DWRITE_FONT_STYLE_ITALIC;
        g.size = gSet.modern ? 2.4f : 2.3f; g.margin = .8f;
        g.color = (gSet.modern ? L.back() : L[3]).color;
        g.text = el + L" \u00B7 " + en;
        L.push_back(g);
    }

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
        float x = left ? leftX * vw : (W - bw) / 2;
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
        if (p.l->split >= 0 && p.l->split < (int)p.l->text.size()) {
            ComPtr<ID2D1SolidColorBrush> two;
            gCtx->CreateSolidColorBrush(Hex(p.l->color2), &two);
            p.lay->SetDrawingEffect(two.Get(), DWRITE_TEXT_RANGE{ (UINT32)p.l->split, (UINT32)(p.l->text.size() - p.l->split) });
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
static int gFps = 24;  // frames a second (24 on mains power, 8 on battery)
static bool gBattery = false;
static ULONGLONG gPauseUntil = 0;  // "Hold still for an hour" (the tray)
static long long gStatFrames = 0, gStatPixels = 0;  // for the once-a-minute power report
static std::vector<RECT> gPrevDirty;   // what changed in the previous frame (the other buffer lacks it)
static int gFullFrames = 2;            // full repaints still needed (one per buffer)
// The part of the desktop no window covers, as a bounding box in canvas pixels: only what moves
// inside it is drawn (windows side by side or snapped can hide it all between them).
static RECT gVisBox = { 0, 0, 0, 0 };
static bool gRecheck = false;   // a window moved, opened, closed or was minimised (or the scene was rebuilt): look again now

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
                gCtx->DrawImage(b.Get(), m.org, RectF(0, 0, (float)m.bw, (float)m.bh), D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
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
    if (animate && gSet.fireflies) ReadMusic();   // one cheap look at the speakers' level a frame
    std::vector<RECT> cur;
    std::vector<Element*> moved;
    for (auto* e : gScene->els) {
        if (e->layer >= 0) continue;
        if (e->still) { if (IsEmpty(e->now)) e->now = e->Bounds(0); continue; }
        double r = e->slow ? e->rate : (e->smooth && !gBattery) ? gFps * 2.5 : gFps;
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
    if (!full) for (auto& r : draw) { RECT x; if (!IntersectRect(&x, &r, &gVisBox)) x = { 0, 0, 0, 0 }; r = x; }   // hidden behind windows: skip it
    draw.erase(std::remove_if(draw.begin(), draw.end(), [](const RECT& r) { return r.right <= r.left || r.bottom <= r.top; }), draw.end());
    if (!full && draw.empty()) { gPrevDirty = cur; return true; }
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
    // One view across your screens: the panorama laid over all of them together (like looking
    // through two windows side by side), instead of the same picture on each.
    if (gSet.span && gMons.size() > 1) {
        RECT u = gMons[0].rc;
        for (auto& m : gMons) UnionRect(&u, &u, &m.rc);
        Mon all{};
        SetupMon(all, u);
        for (auto& m : gMons) { m.art = all.art; m.org = all.org; m.bw = all.bw; m.bh = all.bh; }
    }
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
    gParent = gSaver ? gSaverWnd : FindDesktopHost();
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

static void PinPanel();
static void Rebuild() {  // device, swap chain, scene, layers and clock for the current state
    if (!gD3D && !CreateDevice()) return;
    if (!gAttached && !Attach()) return;
    Layout();
    if (!CreateSwap()) return;
    delete gScene;
    gWx = ReadWeather();
    gScene = BuildScene(gWanted);
    LoadLayers();
    for (auto& m : gMons) Log(L"monitor %ld,%ld %ldx%ld%s scene %s layers %d %d %d", m.rc.left, m.rc.top, m.rc.right - m.rc.left, m.rc.bottom - m.rc.top,
                              m.primary ? L" (main)" : L"", kSceneKey[gWanted], (int)!!m.layer[0], (int)!!m.layer[1], (int)!!m.layer[2]);
    BuildClock(gWanted);
    UpdateLight(gWanted);
    gClockMinute = -1;
    gPrevDirty.clear();
    gFullFrames = 2;
    gRecheck = true;
    PinPanel();
}

static void Stop() {
    Detach();
    ReleaseDevice();
    if (gSaver) return;
    // let Explorer repaint its own wallpaper where we were
    wchar_t wall[MAX_PATH * 2] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH * 2, wall, 0);
    if (wall[0]) SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, wall, 0);
}

// ------------------------------------------------------------------------------------ when to move
static bool gLocked = false, gDisplayOff = false, gCovered = false, gStill = false;

static BOOL CALLBACK CoverEnum(HWND h, LPARAM lp) {
    HRGN open = (HRGN)lp;
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
    if (r.right > r.left && r.bottom > r.top) {
        HRGN w = CreateRectRgn(r.left, r.top, r.right, r.bottom);
        CombineRgn(open, open, w, RGN_DIFF);
        DeleteObject(w);
    }
    return TRUE;
}
// How many pixels a region holds (its rectangles don't overlap).
static long long RegionArea(HRGN rg) {
    DWORD n = GetRegionData(rg, 0, nullptr);
    if (!n) return 0;
    std::vector<char> buf(n);
    auto* d = (RGNDATA*)buf.data();
    if (!GetRegionData(rg, n, d)) return 0;
    long long a = 0;
    auto* rc = (RECT*)d->Buffer;
    for (DWORD i = 0; i < d->rdh.nCount; i++) a += (long long)(rc[i].right - rc[i].left) * (rc[i].bottom - rc[i].top);
    return a;
}
static void CheckConditions() {
    RECT oldBox = gVisBox;
    gCovered = false;
    gVisBox = { 0, 0, 0, 0 };
    if (gSaver) gVisBox = { 0, 0, (LONG)gCanvasW, (LONG)gCanvasH };
    else if (!gMons.empty()) {
        HRGN open = CreateRectRgn(0, 0, 0, 0);
        long long total = 0;
        for (auto& m : gMons) {
            HRGN w = CreateRectRgn(m.work.left, m.work.top, m.work.right, m.work.bottom);
            CombineRgn(open, open, w, RGN_OR);
            DeleteObject(w);
            total += (long long)(m.work.right - m.work.left) * (m.work.bottom - m.work.top);
        }
        EnumWindows(CoverEnum, (LPARAM)open);
        // a sliver between two windows doesn't count (less than a fifth of a percent of the screen)
        if (RegionArea(open) * 500 < total) gCovered = true;
        else {
            RECT b; GetRgnBox(open, &b);
            POINT o = { 0, 0 };   // screen -> canvas
            if (gParent) ScreenToClient(gParent, &o);
            gVisBox = Clip(RECT{ b.left + o.x - 2, b.top + o.y - 2, b.right + o.x + 2, b.bottom + o.y + 2 });
        }
        DeleteObject(open);
    }
    // more of the desktop showing than was drawn: draw it all again
    RECT u;
    if (!gCovered && (!UnionRect(&u, &oldBox, &gVisBox) || !EqualRect(&u, &oldBox))) gFullFrames = std::max(gFullFrames, 2);
    // a game, a film or a presentation full screen on any monitor: hold still (nothing to see, nothing to spend)
    QUERY_USER_NOTIFICATION_STATE qs;
    if (!gSaver && SUCCEEDED(SHQueryUserNotificationState(&qs)) && (qs == QUNS_RUNNING_D3D_FULL_SCREEN || qs == QUNS_BUSY || qs == QUNS_PRESENTATION_MODE)) gCovered = true;
    SYSTEM_POWER_STATUS ps;
    bool saver = false, battery = false;
    if (GetSystemPowerStatus(&ps)) { saver = (ps.SystemStatusFlag & 1) != 0; battery = ps.ACLineStatus == 0; }
    // Only Battery saver holds the scene still. (Windows' "Animation effects" switch is often off
    // just to make menus snappier; Lembas's own tray switch turns the moving scene off.)
    gStill = saver || GetTickCount64() < gPauseUntil;
    gBattery = battery;
    gFps = battery ? 8 : 24;   // on battery: fewer frames, and the fast movers don't get extra ones
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

static bool gPanelDeeds = false;   // the next popup shows the Deeds instead of today
static void ShowPanel(POINT at);
enum { IDM_ADAPTIVE = 100, IDM_SHIRE, IDM_REDBOOK, IDM_STARLIT, IDM_ELVEN, IDM_LORIEN, IDM_HAVENS,
       IDM_MODERN = 200, IDM_CLASSIC, IDM_NOCLOCK, IDM_SCENE, IDM_MEALS, IDM_WEATHER,
       IDM_SPAN, IDM_PIN, IDM_PAUSE, IDM_SAVER,
       IDM_PREVIEW = 300, IDM_UPDATE, IDM_UNINSTALL, IDM_SYNC, IDM_PLACE,
       IDM_GREETING = 400, IDM_TENGWAR_OFF, IDM_TENGWAR_DATE, IDM_TENGWAR_BOTH, IDM_FIREFLIES, IDM_DEEDS };

// Lembas as the screensaver (the scene and the clock, full screen): set or cleared in your Windows settings.
static std::wstring SaverPath() { return LembasDir() + L"\\Living\\Lembas.scr"; }
static bool SaverOn() {
    wchar_t v[MAX_PATH * 2] = {}; DWORD n = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"SCRNSAVE.EXE", RRF_RT_REG_SZ, nullptr, v, &n) != ERROR_SUCCESS) return false;
    return _wcsicmp(v, SaverPath().c_str()) == 0;
}
static void SetSaver(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        // the screensaver is this program under a .scr name (Windows runs it with /s)
        wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
        CopyFileW(self, SaverPath().c_str(), FALSE);
        std::wstring p = SaverPath();
        RegSetValueExW(k, L"SCRNSAVE.EXE", 0, REG_SZ, (const BYTE*)p.c_str(), (DWORD)((p.size() + 1) * 2));
        RegSetValueExW(k, L"ScreenSaveActive", 0, REG_SZ, (const BYTE*)L"1", 4);
        wchar_t cur[16] = {}; DWORD n = sizeof cur;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"ScreenSaveTimeOut", RRF_RT_REG_SZ, nullptr, cur, &n) != ERROR_SUCCESS || !_wtoi(cur))
            RegSetValueExW(k, L"ScreenSaveTimeOut", 0, REG_SZ, (const BYTE*)L"600", 8);   // after 10 minutes
    } else {
        RegDeleteValueW(k, L"SCRNSAVE.EXE");
        RegSetValueExW(k, L"ScreenSaveActive", 0, REG_SZ, (const BYTE*)L"0", 4);
    }
    RegCloseKey(k);
    SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, on, nullptr, SPIF_SENDCHANGE);
}

// Signed in to sync with your phone and watch (through your Google account)?
static bool SyncSignedIn() {
    wchar_t base[MAX_PATH] = {};
    GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH);
    return GetFileAttributesW((std::wstring(base) + L"\\Lembas\\google-token.dat").c_str()) != INVALID_FILE_ATTRIBUTES;
}

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
    AppendMenuW(m, MF_STRING | check(gSet.look == L"lorien"), IDM_LORIEN, L"Lothl\u00F3rien");
    AppendMenuW(m, MF_STRING | check(gSet.look == L"havens"), IDM_HAVENS, L"The Grey Havens");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(clock, MF_STRING | check(gSet.clock && gSet.modern), IDM_MODERN, L"Modern");
    AppendMenuW(clock, MF_STRING | check(gSet.clock && !gSet.modern), IDM_CLASSIC, L"Classic");
    AppendMenuW(clock, MF_STRING | check(!gSet.clock), IDM_NOCLOCK, L"Off");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)clock, L"Desktop clock");
    AppendMenuW(m, MF_STRING | check(gSet.enabled), IDM_SCENE, L"Moving desktop scene");
    AppendMenuW(m, MF_STRING | check(gSet.meals), IDM_MEALS, L"Hobbit mealtimes");
    AppendMenuW(m, MF_STRING | check(gSet.weather), IDM_WEATHER, L"Weather in the scene");
    AppendMenuW(m, MF_STRING | check(GetTickCount64() < gPauseUntil), IDM_PAUSE, L"Hold the scene still for an hour");
    AppendMenuW(m, MF_STRING | check(gSet.pinned), IDM_PIN, L"Keep today's panel on the desktop");
    if (gMons.size() > 1) AppendMenuW(m, MF_STRING | check(gSet.span), IDM_SPAN, L"One view across your screens");
    AppendMenuW(m, MF_STRING | check(SaverOn()), IDM_SAVER, L"Lembas as the screensaver");
    HMENU touches = CreatePopupMenu(), tengwar = CreatePopupMenu();
    AppendMenuW(touches, MF_STRING | check(gSet.greeting), IDM_GREETING, L"Elvish greeting under the clock");
    AppendMenuW(tengwar, MF_STRING | check(gSet.tengwar == 0), IDM_TENGWAR_OFF, L"Off");
    AppendMenuW(tengwar, MF_STRING | check(gSet.tengwar == 1), IDM_TENGWAR_DATE, L"On the date");
    AppendMenuW(tengwar, MF_STRING | check(gSet.tengwar == 2), IDM_TENGWAR_BOTH, L"On the date and clock");
    AppendMenuW(touches, MF_POPUP, (UINT_PTR)tengwar, L"Tengwar");
    AppendMenuW(touches, MF_STRING | check(gSet.fireflies), IDM_FIREFLIES, L"Fireflies to the music");
    AppendMenuW(touches, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(touches, MF_STRING, IDM_DEEDS, L"Deeds of the Red Book...");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)touches, L"Small touches");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    bool synced = SyncSignedIn();
    AppendMenuW(m, MF_STRING | check(synced), IDM_SYNC, synced ? L"Sync with your phone (on)" : L"Sync with your phone...");
    AppendMenuW(m, MF_STRING, IDM_PLACE, L"Your location for the weather...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_PREVIEW, L"Preview the four looks");
    AppendMenuW(m, MF_STRING, IDM_UPDATE, L"Check for updates");
    AppendMenuW(m, MF_STRING, IDM_UNINSTALL, L"Uninstall Lembas...");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(gMsgWnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, gMsgWnd, nullptr);
    DestroyMenu(m);   // also destroys the clock submenu
    PostMessageW(gMsgWnd, WM_NULL, 0, 0);
    static const wchar_t* looks[] = { L"adaptive", L"shire", L"redbook", L"starlit", L"elven", L"lorien", L"havens" };
    switch (cmd) {
    case IDM_ADAPTIVE: case IDM_SHIRE: case IDM_REDBOOK: case IDM_STARLIT: case IDM_ELVEN: case IDM_LORIEN: case IDM_HAVENS:
        gSet.look = looks[cmd - IDM_ADAPTIVE];
        RunScript(L"Change-Look.ps1", std::wstring(L"-Look ") + looks[cmd - IDM_ADAPTIVE], false);
        break;
    case IDM_MODERN: SetConfig(L"desktopClock", L"true"); SetConfig(L"desktopClockStyle", L"\"modern\""); break;
    case IDM_CLASSIC: SetConfig(L"desktopClock", L"true"); SetConfig(L"desktopClockStyle", L"\"classic\""); break;
    case IDM_NOCLOCK: SetConfig(L"desktopClock", L"false"); break;
    case IDM_SCENE: SetConfig(L"livingDesktop", gSet.enabled ? L"false" : L"true"); break;
    case IDM_MEALS: SetConfig(L"meals", gSet.meals ? L"false" : L"true"); break;
    case IDM_WEATHER:
        SetConfig(L"weather", gSet.weather ? L"false" : L"true");
        if (!gSet.weather) ShellExecuteW(nullptr, L"open", L"schtasks.exe", L"/Run /TN \"\\Lembas\\Lembas Sun\"", nullptr, SW_HIDE);  // fetch it now
        gRescene = true;
        break;
    case IDM_PAUSE: gPauseUntil = GetTickCount64() < gPauseUntil ? 0 : GetTickCount64() + 3600ULL * 1000; CheckConditions(); break;
    case IDM_PIN: SetConfig(L"panelPinned", gSet.pinned ? L"false" : L"true"); break;
    case IDM_SPAN: SetConfig(L"spanScreens", gSet.span ? L"false" : L"true"); gReattach = true; break;
    case IDM_SAVER: SetSaver(!SaverOn()); break;
    case IDM_GREETING: SetConfig(L"greeting", gSet.greeting ? L"false" : L"true"); break;
    case IDM_TENGWAR_OFF: SetConfig(L"tengwar", L"\"off\""); break;
    case IDM_TENGWAR_DATE: SetConfig(L"tengwar", L"\"date\""); break;
    case IDM_TENGWAR_BOTH: SetConfig(L"tengwar", L"\"both\""); break;
    case IDM_FIREFLIES: SetConfig(L"fireflies", gSet.fireflies ? L"false" : L"true"); break;
    case IDM_DEEDS: { POINT at; GetCursorPos(&at); gPanelDeeds = true; ShowPanel(at); break; }
    case IDM_PREVIEW: RunScript(L"Preview-Lembas.ps1", L"", true); break;
    case IDM_UPDATE: RunScript(L"Update-Lembas.ps1", L"", true); break;
    case IDM_PLACE: RunScript(L"Lembas-Location.ps1", L"", false); break;
    case IDM_UNINSTALL: RunScript(L"Uninstall-Lembas.ps1", L"", true); break;
    case IDM_SYNC:
        if (!SyncSignedIn()) RunScript(L"Lembas-Sync.ps1", L"-SignIn", true);
        else if (MessageBoxW(nullptr, L"Stop keeping this PC's look in step with your phone and watch?\n\nLembas forgets the Google sign-in on this PC.",
                             L"Lembas", MB_YESNO | MB_ICONQUESTION) == IDYES) RunScript(L"Lembas-Sync.ps1", L"-SignOut -Quiet", false);
        break;
    }
    if (cmd >= IDM_MODERN && cmd < IDM_PREVIEW && SyncSignedIn()) RunScript(L"Lembas-Sync.ps1", L"-Quiet", false);   // tell your other devices
    if ((cmd >= IDM_MODERN && cmd < IDM_PREVIEW) || (cmd >= IDM_GREETING && cmd < IDM_DEEDS)) {   // settings changed: apply them now
        LoadSettings();
        gClockMinute = -1;
        gReload = true;
        PinPanel();
    }
}

// ------------------------------------------------------------------------------------ messages
static UINT gTaskbarCreated;
static const GUID kDisplayState = { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };  // GUID_CONSOLE_DISPLAY_STATE

// ------------------------------------------------------------------------------------ the clock panel
// Click the desktop clock: a small panel with today's sun, when the look changes next, and the
// Shire calendar for the week ahead. Click anywhere else (or press Esc) to close it.
#define WM_LEMBAS_PANEL (WM_APP + 8)
#define WM_LEMBAS_RELOAD (WM_APP + 9)   // settings changed elsewhere (sync): read them again
static HWND gPanel;
static HHOOK gMouseHook;
static std::atomic<LONG> gClockL{ 0 }, gClockT{ 0 }, gClockR{ 0 }, gClockB{ 0 };   // the clock on screen (for the click watcher's thread)
static ComPtr<ID2D1HwndRenderTarget> gPanelRT;
struct PanelLine { std::wstring left, right, family; float size; DWRITE_FONT_WEIGHT weight; DWRITE_FONT_STYLE style; int color; float gap; };
static std::vector<PanelLine> gPanelLines;
static uint32_t gPanelCol[4];  // background, ink, accent, soft

static RECT ClockOnScreen() {
    POINT o = { 0, 0 };
    ClientToScreen(gParent, &o);
    return RECT{ (LONG)gClock.rc.left + o.x, (LONG)gClock.rc.top + o.y, (LONG)gClock.rc.right + o.x, (LONG)gClock.rc.bottom + o.y };
}
// Only clicks on the bare desktop over the clock (not on an icon, not on a window) open the panel.
static LRESULT CALLBACK MouseLL(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && w == WM_LBUTTONUP) {
        auto* m = (MSLLHOOKSTRUCT*)l;
        RECT r = { gClockL, gClockT, gClockR, gClockB };
        if (PtInRect(&r, m->pt)) {
            wchar_t cls[64] = {};
            GetClassNameW(WindowFromPoint(m->pt), cls, 64);
            if (!wcscmp(cls, L"SysListView32") || !wcscmp(cls, L"SHELLDLL_DefView") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Progman"))
                PostMessageW(gMsgWnd, WM_LEMBAS_PANEL, (WPARAM)m->pt.x, (LPARAM)m->pt.y);
        }
    }
    return CallNextHookEx(gMouseHook, code, w, l);
}
static bool OnDesktopIcon(POINT pt) {
    IAccessible* acc = nullptr;
    VARIANT child; VariantInit(&child);
    bool hit = false;
    if (SUCCEEDED(AccessibleObjectFromPoint(pt, &acc, &child)) && acc) {
        VARIANT role; VariantInit(&role);
        if (SUCCEEDED(acc->get_accRole(child, &role)) && role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_LISTITEM) hit = true;
        VariantClear(&role);
        acc->Release();
    }
    VariantClear(&child);
    return hit;
}

static std::wstring HourMinute(int m) {
    wchar_t b[16];
    int h = m / 60, mi = m % 60;
    if (Use24h()) std::swprintf(b, 16, L"%02d:%02d", h, mi);
    else std::swprintf(b, 16, L"%d:%02d %s", (h % 12) ? h % 12 : 12, mi, h < 12 ? L"am" : L"pm");
    return b;
}
static SYSTEMTIME AddDays(const SYSTEMTIME& st, int days) {
    FILETIME ft; SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    u.QuadPart += (ULONGLONG)days * 864000000000ULL;
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    SYSTEMTIME o; FileTimeToSystemTime(&ft, &o);
    return o;
}
static const wchar_t* kLookTitle[] = { L"Shire Morning", L"the Red Book", L"Starlit Night", L"Elven Silver", L"Lothl\u00F3rien", L"the Grey Havens" };

static void PanelContent(SceneId sc) {
    static const uint32_t cols[6][4] = {
        { 0xFBF4E3, 0x27452A, 0x3E6A3A, 0x6A7D4E }, { 0xF6ECD2, 0x2B1E12, 0x9A2A1C, 0x6A5236 },
        { 0x101633, 0xF4ECD8, 0xE2C687, 0x9C927B }, { 0x0E1418, 0xE6ECEE, 0xB9C6CA, 0x7D898D },
        { 0x231C26, 0xFFF6E0, 0xF6D27A, 0xB89F80 }, { 0x0E1424, 0xF2F4FA, 0xC9D4EA, 0x9AA8C0 } };
    int k = sc == NONE ? 1 : (int)sc;
    for (int i = 0; i < 4; i++) gPanelCol[i] = cols[k][i];
    std::wstring fraunces = Family({ L"Fraunces", L"Fraunces Light" }), alegreya = Family({ L"Alegreya Sans Medium", L"Alegreya Sans" });
    std::wstring garamond = Family({ L"Cormorant Garamond", L"Cormorant Garamond Light" });
    SYSTEMTIME st; GetLocalTime(&st);
    int sd; std::wstring sname;
    ShireDate(st, sd, sname);
    gPanelLines.clear();
    auto add = [&](std::wstring l, std::wstring r, const std::wstring& fam, float size, int color, float gap,
                   DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE s = DWRITE_FONT_STYLE_NORMAL) {
        gPanelLines.push_back({ l, r, fam, size, w, s, color, gap });
    };
    add(std::wstring(kWeek[st.wDayOfWeek]) + L", " + (sd ? std::to_wstring(sd) + L" " : L"") + sname, L"", fraunces, 22, 1, 0, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    add(DateFmt(st, L"dddd d MMMM yyyy"), L"", alegreya, 13, 3, 2);
    // today's sun, and the next change of look
    SunDay d = SunToday(st, gSet.lat, gSet.lon);
    add(L"TODAY'S SUN", L"", alegreya, 11, 2, 16);
    add(L"Sunrise " + HourMinute(d.rise) + L"  ·  Sunset " + HourMinute(d.set), L"", alegreya, 14, 1, 6);
    if (!gForecast.empty()) add(gForecast, L"", garamond, 16, 1, 4, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC);   // the day's weather, hobbit-fashion
    add(FeastCountdown(), L"", garamond, 16, 2, 4, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC);
    // the walk to Mount Doom (from your phone, through sync)
    {
        wchar_t base[MAX_PATH]; GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        std::wstring j = ReadAll(std::wstring(base) + L"\\Lembas\\walk.json"), v;
        if (JsonValue(j, L"line", v) && v.size() > 2 && v.front() == L'"') add(v.substr(1, v.size() - 2), L"", garamond, 16, 2, 4, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC);
    }
    if (gSet.adaptive && gForced == NONE) {
        int m = st.wHour * 60 + st.wMinute;
        struct B { int at; SceneId to; } bs[] = { { d.rise, SHIRE }, { d.evening, SeasonNow() == 2 ? LORIEN : REDBOOK }, { d.set, STARLIT }, { 23 * 60, ELVEN }, { 2 * 60, HAVENS } };
        int best = -1, bestDelta = 99999;
        for (int i = 0; i < 5; i++) { int dl = (bs[i].at - m + 1440) % 1440; if (dl == 0) dl = 1440; if (dl < bestDelta) { bestDelta = dl; best = i; } }
        std::wstring now = kLookTitle[(int)SunScene(st)];
        now[0] = (wchar_t)towupper(now[0]);
        add(now + L" until " + HourMinute(bs[best].at) + L", then " + kLookTitle[(int)bs[best].to], L"", garamond, 17, 2, 4, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC);
    } else {
        add(L"Showing " + std::wstring(kLookTitle[k]) + L" (chosen in Lembas)", L"", garamond, 17, 2, 4, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC);
    }
    // the week ahead in the Shire calendar
    add(L"THE WEEK AHEAD", L"", alegreya, 11, 2, 16);
    for (int i = 0; i < 7; i++) {
        SYSTEMTIME dd = AddDays(st, i);
        int sdd; std::wstring sn;
        ShireDate(dd, sdd, sn);
        add(i == 0 ? L"Today" : DateFmt(dd, L"ddd d MMM"), std::wstring(kWeek[dd.wDayOfWeek]) + L" · " + (sdd ? std::to_wstring(sdd) + L" " : L"") + sn,
            alegreya, 14, i == 0 ? 1 : 3, i == 0 ? 6 : 4);
    }
    // today's saying, from the Red Book
    add(L"FROM THE RED BOOK", L"", alegreya, 11, 2, 16);
    add(SayingToday(), L"", garamond, 17, 1, 6, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_ITALIC);
    LoadDeeds();
    int seen = 0; for (auto& d : kDeeds) seen += gDeeds.count(d.id) ? 1 : 0;
    add(L"Deeds: " + std::to_wstring(seen) + L" of " + std::to_wstring(sizeof kDeeds / sizeof kDeeds[0]) + L" seen on this PC (Lembas menu \u203A Deeds)", L"", alegreya, 12, 3, 4);
    add(L"Click anywhere else to close", L"", alegreya, 11, 3, 14);
}

// The Deeds of the Red Book, in the panel: what you've seen on this PC, newest first, then hints.
static void DeedsContent(SceneId sc) {
    PanelContent(sc);   // for the colours
    gPanelLines.clear();
    std::wstring fraunces = Family({ L"Fraunces", L"Fraunces Light" }), alegreya = Family({ L"Alegreya Sans Medium", L"Alegreya Sans" });
    std::wstring garamond = Family({ L"Cormorant Garamond", L"Cormorant Garamond Light" });
    auto add = [&](std::wstring l, std::wstring r, const std::wstring& fam, float size, int color, float gap,
                   DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE st = DWRITE_FONT_STYLE_NORMAL) {
        gPanelLines.push_back({ l, r, fam, size, w, st, color, gap });
    };
    LoadDeeds();
    std::vector<const DeedInfo*> seen, unseen;
    for (auto& d : kDeeds) (gDeeds.count(d.id) ? seen : unseen).push_back(&d);
    std::sort(seen.begin(), seen.end(), [](const DeedInfo* a, const DeedInfo* b) { return gDeeds[a->id].first > gDeeds[b->id].first; });
    add(L"Deeds of the Red Book", L"", fraunces, 22, 1, 0, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    add(std::to_wstring(seen.size()) + L" of " + std::to_wstring(seen.size() + unseen.size()) + L" seen on this PC", L"", alegreya, 13, 3, 2);
    if (!seen.empty()) add(L"SEEN", L"", alegreya, 11, 2, 16);
    for (auto* d : seen) {
        auto& r = gDeeds[d->id];
        SYSTEMTIME day = {}; { long z = r.first + 719468, era = z / 146097; unsigned doe = (unsigned)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153; day.wDay = (WORD)(doy - (153 * mp + 2) / 5 + 1);
            day.wMonth = (WORD)(mp < 10 ? mp + 3 : mp - 9); day.wYear = (WORD)(yoe + era * 400 + (day.wMonth <= 2)); }
        add(d->name, L"", garamond, 18, 1, 8, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_ITALIC);
        add(std::wstring(d->line) + L"  \u00B7  first seen " + DateFmt(day, L"d MMMM yyyy") + (r.count > 1 ? L", " + std::to_wstring(r.count) + L" times" : L""), L"", alegreya, 12, 3, 1);
    }
    if (!unseen.empty()) add(L"STILL TO FIND", L"", alegreya, 11, 2, 16);
    for (auto* d : unseen) add(L"\u00B7 \u00B7 \u00B7   " + std::wstring(d->hint), L"", alegreya, 13, 3, 5);
    add(L"Kept on this PC only. Click anywhere else to close", L"", alegreya, 11, 3, 14);
}

static float PanelScale() { return GetDpiForWindow(gPanel ? gPanel : gMsgWnd) / 96.f; }
static ComPtr<IDWriteTextLayout> PanelLayout(const std::wstring& t, const PanelLine& l, float s) {
    ComPtr<IDWriteTextFormat> f;
    gDWrite->CreateTextFormat(l.family.c_str(), gFonts.Get(), l.weight, l.style, DWRITE_FONT_STRETCH_NORMAL, l.size * s, L"en", &f);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteTextLayout> lay;
    gDWrite->CreateTextLayout(t.c_str(), (UINT32)t.size(), f.Get(), 2000, 200, &lay);
    return lay;
}
static float PanelHeight(float s, float& width) {
    float y = 22 * s, w = 0;
    for (auto& l : gPanelLines) {
        y += l.gap * s;
        DWRITE_TEXT_METRICS m; PanelLayout(l.left, l, s)->GetMetrics(&m);
        float lw = m.width;
        if (!l.right.empty()) { DWRITE_TEXT_METRICS r; PanelLayout(l.right, l, s)->GetMetrics(&r); lw = 104 * s + r.width; }
        w = std::max(w, lw);
        y += m.height;
    }
    width = std::max(w + 48 * s, 320 * s);
    return y + 20 * s;
}
static void PaintPanel() {
    if (!gPanel) return;
    RECT rc; GetClientRect(gPanel, &rc);
    if (!gPanelRT) {
        gFactory->CreateHwndRenderTarget(RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, PixelFormat(), 96, 96),
                                         HwndRenderTargetProperties(gPanel, SizeU(rc.right, rc.bottom)), &gPanelRT);
        if (!gPanelRT) return;
    }
    float s = PanelScale();
    ComPtr<ID2D1SolidColorBrush> br;
    gPanelRT->CreateSolidColorBrush(Hex(gPanelCol[1]), &br);
    gPanelRT->BeginDraw();
    gPanelRT->Clear(Hex(gPanelCol[0]));
    float y = 22 * s;
    for (auto& l : gPanelLines) {
        y += l.gap * s;
        br->SetColor(Hex(gPanelCol[l.color]));
        auto lay = PanelLayout(l.left, l, s);
        gPanelRT->DrawTextLayout(Point2F(24 * s, y), lay.Get(), br.Get());
        if (!l.right.empty()) {
            br->SetColor(Hex(gPanelCol[1]));
            gPanelRT->DrawTextLayout(Point2F(128 * s, y), PanelLayout(l.right, l, s).Get(), br.Get());
        }
        DWRITE_TEXT_METRICS m; lay->GetMetrics(&m);
        y += m.height;
    }
    if (gPanelRT->EndDraw() == D2DERR_RECREATE_TARGET) gPanelRT.Reset();
}
static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); PaintPanel(); EndPaint(h, &ps); return 0; }
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE) DestroyWindow(h); return 0;
    case WM_KEYDOWN: if (w == VK_ESCAPE) DestroyWindow(h); return 0;
    case WM_CHAR: {   // the hidden phrases, typed while the panel is open
        static std::wstring typed;
        wchar_t c = (wchar_t)towlower((wchar_t)w);
        if (c < 'a' || c > 'z') return 0;
        typed = (typed + c).substr(typed.size() + 1 > 10 ? typed.size() + 1 - 10 : 0);
        int which = typed.size() >= 6 && typed.compare(typed.size() - 6, 6, L"mellon") == 0 ? 0
                  : typed.size() >= 7 && typed.compare(typed.size() - 7, 7, L"ashnazg") == 0 ? 1 : -1;
        if (which >= 0) { typed.clear(); gWonder = which; gWonderAt = GetTickCount64(); Deed(which == 0 ? "mellon" : "ring"); DestroyWindow(h); }
        return 0;
    }
    case WM_LBUTTONUP: DestroyWindow(h); return 0;
    case WM_DESTROY: gPanel = nullptr; gPanelRT.Reset(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static bool gPanelChild = false;   // the panel is pinned to the desktop (part of it, not a popup)
static void ShowPanel(POINT at) {
    if (gPanelChild && !gPanelDeeds) return;           // pinned: it's already there
    if (gPanelChild && gPanel) { DestroyWindow(gPanel); gPanelChild = false; }   // the Deeds over a pinned panel: it comes back after
    else if (gPanel) { DestroyWindow(gPanel); if (!gPanelDeeds) return; }
    if (!gPanelDeeds && OnDesktopIcon(at)) return;
    if (gPanelDeeds) DeedsContent(gWanted); else PanelContent(gWanted);
    gPanelDeeds = false;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = PanelProc;
        wc.lpszClassName = L"LembasClockPanel";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }
    float s = GetDpiForSystem() / 96.f, width;
    float height = PanelHeight(s, width);
    RECT clock = ClockOnScreen();
    HMONITOR hm = MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof mi }; GetMonitorInfoW(hm, &mi);
    int x = std::max((int)mi.rcWork.left + 8, std::min((int)clock.left + (int)(24 * s), (int)mi.rcWork.right - (int)width - 8));
    int y = std::min((int)clock.bottom, (int)mi.rcWork.bottom - (int)height - 8);
    gPanel = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"LembasClockPanel", L"Lembas", WS_POPUP,
                             x, y, (int)width, (int)height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!gPanel) return;
    DWORD round = 2;  // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(gPanel, 33, &round, sizeof round);
    BOOL dark = gWanted == STARLIT || gWanted == ELVEN || gWanted == LORIEN || gWanted == HAVENS;
    DwmSetWindowAttribute(gPanel, 20, &dark, sizeof dark);  // DWMWA_USE_IMMERSIVE_DARK_MODE
    ShowWindow(gPanel, SW_SHOW);
    SetForegroundWindow(gPanel);
}
// Keep today's panel on the desktop: the same panel, drawn into the desktop under your icons, below
// the clock, refreshed each minute.
static void PinPanel() {
    bool want = gSet.pinned && gSet.clock && gAttached && gClock.bmp && IsWindow(gParent);
    if (!want) { if (gPanelChild && gPanel) DestroyWindow(gPanel); gPanelChild = false; return; }
    if (gPanel && !gPanelChild) return;                  // a popup is open: pin again once it's closed
    PanelContent(gWanted);
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
            if (m == WM_PAINT) { PAINTSTRUCT ps; BeginPaint(h, &ps); PaintPanel(); EndPaint(h, &ps); return 0; }
            if (m == WM_NCHITTEST) return HTTRANSPARENT;
            if (m == WM_DESTROY) { gPanel = nullptr; gPanelRT.Reset(); gPanelChild = false; return 0; }
            return DefWindowProcW(h, m, w, l);
        };
        wc.lpszClassName = L"LembasPinnedPanel";
        RegisterClassW(&wc);
        registered = true;
    }
    float s = GetDpiForSystem() / 96.f, width;
    float height = PanelHeight(s, width);
    int x = (int)gClock.rc.left + (int)(24 * s), y = (int)gClock.rc.bottom + (int)(8 * s);
    if (!gPanel) {
        gPanel = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, L"LembasPinnedPanel", L"Lembas today", WS_CHILD | WS_CLIPSIBLINGS,
                                 x, y, (int)width, (int)height, gParent, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!gPanel) return;
        gPanelChild = true;
        SetWindowPos(gPanel, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(gPanel, SW_SHOWNA);
    } else {
        SetWindowPos(gPanel, HWND_TOP, x, y, (int)width, (int)height, SWP_NOACTIVATE);
        gPanelRT.Reset();
    }
    InvalidateRect(gPanel, nullptr, FALSE);
}

// The click watcher runs on a thread of its own that does nothing else, so a mouse movement never
// waits on a frame being drawn (Windows holds every mouse movement until a low-level hook answers),
// and it's on only while the desktop clock can actually be seen.
static DWORD gHookThread = 0;
#define WM_LEMBAS_HOOK (WM_APP + 21)
static DWORD WINAPI HookThread(void*) {
    MSG m;
    PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);   // make the message queue
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_LEMBAS_HOOK) {
            if (m.wParam && !gMouseHook) gMouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseLL, GetModuleHandleW(nullptr), 0);
            if (!m.wParam && gMouseHook) { UnhookWindowsHookEx(gMouseHook); gMouseHook = nullptr; }
        }
        DispatchMessageW(&m);
    }
    if (gMouseHook) UnhookWindowsHookEx(gMouseHook);
    return 0;
}
static void UpdateMouseHook() {
    bool want = gSet.clock && gAttached && gScene && !gLocked && !gDisplayOff && !gCovered;
    RECT r = want && gClock.bmp ? ClockOnScreen() : RECT{ 0, 0, 0, 0 };
    gClockL = r.left; gClockT = r.top; gClockR = r.right; gClockB = r.bottom;
    static bool on = false;
    if (want == on) return;
    if (!gHookThread) CloseHandle(CreateThread(nullptr, 0, HookThread, nullptr, 0, &gHookThread));
    for (int i = 0; i < 50 && !PostThreadMessageW(gHookThread, WM_LEMBAS_HOOK, want, 0); i++) Sleep(2);   // until its queue exists
    on = want;
}

static LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == gTaskbarCreated) { gReattach = true; TrayAdd(); return 0; }
    switch (m) {
    case WM_LEMBAS_PANEL:
        ShowPanel(POINT{ (LONG)w, (LONG)l }); return 0;
    case WM_LEMBAS_RELOAD:
    {   // the look itself arrives as a wallpaper change; rebuild the scene only for what it shows
        bool wx = gSet.weather, span = gSet.span;
        LoadSettings(); gClockMinute = -1; gReload = true;
        if (gSet.weather != wx) gRescene = true;
        if (gSet.span != span) gReattach = true;
        return 0;
    }
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
    for (int i = 0; i < 6; i++) {
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
    if (cmd && wcsstr(cmd, L"--reload")) {   // the running copy re-reads its settings (after a sync)
        if (HWND other = FindWindowW(L"LembasDesktopMsg", nullptr)) PostMessageW(other, WM_LEMBAS_RELOAD, 0, 0);
        return 0;
    }
    // As the screensaver (Lembas.scr): /s shows it, /p (the small preview) and /c (settings) do nothing much.
    if (cmd && (wcsstr(cmd, L"/c") || wcsstr(cmd, L"/C") || wcsstr(cmd, L"-c"))) {
        MessageBoxW(nullptr, L"The Lembas screensaver shows the look of the hour, moving, with its clock.\n\nChoose it (or not) from the Lembas icon by the clock.", L"Lembas", MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    if (cmd && (wcsstr(cmd, L"/p") || wcsstr(cmd, L"/P"))) return 0;
    gSaver = cmd && (wcsstr(cmd, L"/s") || wcsstr(cmd, L"/S"));
    bool snapshot = cmd && wcsstr(cmd, L"--snapshot");
    HANDLE mutex = (snapshot || gSaver) ? nullptr : CreateMutexW(nullptr, TRUE, L"Local\\LembasDesktop");
    if (!snapshot && !gSaver && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

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
    for (int i = 0; i < 6; i++) if (cmd && wcsstr(cmd, (std::wstring(L"--look ") + kSceneKey[i]).c_str())) gForced = (SceneId)i;
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
    if (gSaver) {
        // the screensaver: one window over all screens; any key, click or real mouse move ends it
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
            static POINT first = { -1, -1 };
            switch (m) {
            case WM_MOUSEMOVE: {
                POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
                if (first.x < 0) { first = p; return 0; }
                if (std::abs(p.x - first.x) > 8 || std::abs(p.y - first.y) > 8) PostQuitMessage(0);
                return 0;
            }
            case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: PostQuitMessage(0); return 0;
            case WM_SETCURSOR: SetCursor(nullptr); return TRUE;
            case WM_ACTIVATEAPP: if (!w) PostQuitMessage(0); return 0;
            }
            return DefWindowProcW(h, m, w, l);
        };
        wc.lpszClassName = L"LembasSaver";
        wc.hCursor = nullptr;
        RegisterClassW(&wc);
        int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        gSaverWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"LembasSaver", L"Lembas", WS_POPUP | WS_VISIBLE,
                                    vx, vy, GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN), nullptr, nullptr, inst, nullptr);
        SetForegroundWindow(gSaverWnd);
        wc.lpfnWndProc = MsgProc;
        wc.lpszClassName = L"LembasSaverMsg";
        RegisterClassW(&wc);
        gMsgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"LembasSaverMsg", L"Lembas", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    } else {
        if (SaverOn()) { wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH); CopyFileW(self, SaverPath().c_str(), FALSE); }   // keep the screensaver up to date
        gMsgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"LembasDesktopMsg", L"Lembas Desktop", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
        gTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        WTSRegisterSessionNotification(gMsgWnd, NOTIFY_FOR_THIS_SESSION);
        RegisterPowerSettingNotification(gMsgWnd, &kDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
        TrayAdd();
    }

    if (!gSaver) {
        WINEVENTPROC look = [](HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) { gRecheck = true; };
        const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
        SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, look, 0, 0, flags);
        SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, nullptr, look, 0, 0, flags);
        SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, look, 0, 0, flags);
    }

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
            if (gSaver) { SYSTEMTIME st; GetLocalTime(&st); want = SceneFromWallpaper(); if (want == NONE) want = SunScene(st); }
            bool changed = want != gWanted || gReattach || (gWanted != NONE && !gAttached);
            gWanted = want;
            Log(L"wallpaper look: %s", gWanted == NONE ? L"(not Lembas)" : kSceneKey[gWanted]);
            if (gWanted == NONE) { if (gAttached || gD3D) Stop(); }
            else if (changed) { if (gReattach) Detach(); Rebuild(); }
            gReload = gReattach = false;
            nextCheck = 0;
        }
        if (gAttached && (!IsWindow(gParent) || !IsWindow(gWnd))) { gReattach = true; continue; }
        if (gRescene) { gRescene = false; if (gWanted != NONE && gAttached) Rebuild(); }

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
        static double lastCheck = 0;
        if (now >= nextCheck || (gRecheck && now - lastCheck > 0.1)) {
            gRecheck = false; lastCheck = now;
            static int tries = 0;
            if (gWanted != NONE && !gAttached && ++tries % 5 == 0) gReattach = true;  // Explorer not ready yet
            bool wasVisible = Visible();
            CheckConditions();
            if (!wasVisible && Visible()) gFullFrames = 2;
            nextCheck = now + 1.0;
            UpdateMouseHook();
        }

        CheckSun();
        // Signed in to sync: ask Google Drive for a change from the phone every 90 seconds (a quick,
        // hidden check; the look then changes within a minute or two of changing it on the phone)
        if (!gSaver) {
            static ULONGLONG lastSync = GetTickCount64();
            if (GetTickCount64() - lastSync > 90 * 1000) {
                lastSync = GetTickCount64();
                if (SyncSignedIn() && !gLocked && !gDisplayOff) RunScript(L"Lembas-Sync.ps1", L"-Quiet", false);
            }
        }
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
                if (UpdateLight(gWanted)) gFullFrames = std::max(gFullFrames, 1);
                PinPanel();
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
