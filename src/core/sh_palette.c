#include "sh_palette.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// the neutrals of qml/Theme.qml that accent roles have to read on; tests/contrast.py fails if they drift apart
static const char *const DARK_BG[] = {"#10141A", "#141920", "#1B212A", "#222A35"};
static const char *const LIGHT_BG[] = {"#E8EBEE", "#F5F6F7", "#FFFFFF"};
#define DARK_INK_BG "#E8ECF1"
#define LIGHT_INK_BG "#11161C"
#define STEP 0.004

typedef struct { double r, g, b; } rgb;

static double to_linear(double c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
static double to_gamma(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * pow(c, 1 / 2.4) - 0.055; }

static bool parse_hex(const char *s, rgb *out) {
    unsigned r, g, b;
    if (!s || strlen(s) != 7 || s[0] != '#' || sscanf(s + 1, "%2x%2x%2x", &r, &g, &b) != 3) return false;
    *out = (rgb){r / 255.0, g / 255.0, b / 255.0};
    return true;
}

// Björn Ottosson's OKLab, from linear sRGB and back
static void lab_to_linear(double l, double a, double b, rgb *o) {
    const double m = l + 0.3963377774 * a + 0.2158037573 * b, k = l - 0.1055613458 * a - 0.0638541728 * b, s = l - 0.0894841775 * a - 1.2914855480 * b;
    const double m3 = m * m * m, k3 = k * k * k, s3 = s * s * s;
    o->r = 4.0767416621 * m3 - 3.3077115913 * k3 + 0.2309699292 * s3;
    o->g = -1.2684380046 * m3 + 2.6097574011 * k3 - 0.3413193965 * s3;
    o->b = -0.0041960863 * m3 - 0.7034186147 * k3 + 1.7076147010 * s3;
}

bool sh_oklch_from_hex(const char *hex, double *l, double *c, double *h) {
    rgb v;
    if (!parse_hex(hex, &v)) return false;
    const double r = to_linear(v.r), g = to_linear(v.g), b = to_linear(v.b);
    const double m = cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double k = cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    const double ll = 0.2104542553 * m + 0.7936177850 * k - 0.0040720468 * s;
    const double aa = 1.9779984951 * m - 2.4285922050 * k + 0.4505937099 * s;
    const double bb = 0.0259040371 * m + 0.7827717662 * k - 0.8086757660 * s;
    *l = ll;
    *c = sqrt(aa * aa + bb * bb);
    double deg = atan2(bb, aa) * 180.0 / 3.14159265358979323846;
    *h = deg < 0 ? deg + 360 : deg;
    return true;
}

void sh_oklch_to_hex(double l, double c, double h, char out[8]) {
    const double rad = h * 3.14159265358979323846 / 180.0;
    rgb v = {0, 0, 0};
    for (; c >= 0; c -= 0.002) {
        lab_to_linear(l, c * cos(rad), c * sin(rad), &v);
        if (v.r >= -0.0005 && v.r <= 1.0005 && v.g >= -0.0005 && v.g <= 1.0005 && v.b >= -0.0005 && v.b <= 1.0005) break;
    }
#define CH(x) ((int)(fmin(1.0, fmax(0.0, to_gamma(fmax(0.0, x)))) * 255.0 + 0.5))
    snprintf(out, 8, "#%02X%02X%02X", CH(v.r), CH(v.g), CH(v.b));
#undef CH
}

static double luminance(const rgb *v) { return 0.2126 * to_linear(v->r) + 0.7152 * to_linear(v->g) + 0.0722 * to_linear(v->b); }

double sh_palette_contrast(const char *a, const char *b) {
    rgb x, y;
    if (!parse_hex(a, &x) || !parse_hex(b, &y)) return 1;
    const double p = luminance(&x), q = luminance(&y);
    return (fmax(p, q) + 0.05) / (fmin(p, q) + 0.05);
}

static bool reads_on(const char *fg, const char *const *bgs, size_t n, double need) {
    for (size_t i = 0; i < n; ++i)
        if (sh_palette_contrast(fg, bgs[i]) < need) return false;
    return true;
}

// walks lightness in `dir` from `l` until the colour reaches `need` on all of `bgs`
static void solve(double l, double c, double h, int dir, const char *const *bgs, size_t n, double need, char out[8]) {
    for (; l >= 0 && l <= 1; l += dir * STEP) {
        sh_oklch_to_hex(l, c, h, out);
        if (reads_on(out, bgs, n, need)) return;
    }
    sh_oklch_to_hex(dir > 0 ? 1 : 0, 0, h, out);
}

static const char *clash_of(double c, double h) {
    static const struct { const char *name; double hue; } near[] = {{"amber", 75}, {"red", 27}, {"lime", 125}};
    if (c < 0.05) return "";
    for (size_t i = 0; i < sizeof near / sizeof near[0]; ++i) {
        const double d = fabs(fmod(h - near[i].hue + 540, 360) - 180);
        if (d < 15) return near[i].name;
    }
    return "";
}

bool sh_palette_derive(const char *seed, bool dark, sh_palette *p) {
    double l, c, h;
    if (!sh_oklch_from_hex(seed, &l, &c, &h)) return false;
    memset(p, 0, sizeof *p);
    p->clash = clash_of(c, h);
    if (c > 0.17) c = 0.17;
    if (dark) {
        memcpy(p->selected, DARK_BG[3], 8);
        solve(fmax(l, 0.7), c, h, 1, DARK_BG + 1, 3, 3.0, p->mark);
        sh_oklch_to_hex(0.3, fmin(c, 0.06), h, p->mark_low);
        const char *const text_on[] = {DARK_BG[0], DARK_BG[1], DARK_BG[3], p->mark_low};
        solve(fmax(l, 0.72), c, h, 1, text_on, 4, 4.5, p->mark_text);
        char ink[8];
        sh_oklch_to_hex(0.17, fmin(c, 0.04), h, ink);
        memcpy(p->action, p->mark, 8);
        memcpy(p->action_ink, ink, 8);
        const char *const on_action[] = {ink};
        solve(fmax(l, 0.7), c, h, 1, on_action, 1, 4.5, p->action);  // ink stays dark, so the fill gets lighter if it must
        memcpy(p->focus, p->mark, 8);
        const char *const ink_bg[] = {DARK_INK_BG};
        solve(0.55, c, h, -1, ink_bg, 1, 3.0, p->ink_ok);
    } else {
        memcpy(p->action, LIGHT_INK_BG, 8);
        memcpy(p->action_ink, "#FFFFFF", 8);
        sh_oklch_to_hex(0.94, fmin(c, 0.03), h, p->selected);
        sh_oklch_to_hex(0.915, fmin(c, 0.04), h, p->mark_low);
        const char *const graphics[] = {LIGHT_BG[1], LIGHT_BG[2], p->selected};
        solve(fmin(l, 0.62), c, h, -1, graphics, 3, 3.0, p->mark);
        const char *const text_on[] = {LIGHT_BG[0], LIGHT_BG[1], p->selected, p->mark_low};
        solve(fmin(l, 0.55), c, h, -1, text_on, 4, 4.5, p->mark_text);
        memcpy(p->focus, p->mark, 8);
        const char *const ink_bg[] = {LIGHT_INK_BG};
        solve(0.7, c, h, 1, ink_bg, 1, 3.0, p->ink_ok);
    }
    return true;
}
