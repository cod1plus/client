// ruleset_eval.cpp - PunkBuster rule semantics, engine-free. See ruleset_eval.h.

#include "netcode/ruleset_eval.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

namespace patches {

namespace {

// tolerance scales with the magnitude: float32 round trips of "0.022" and "25000" both pass,
// 0.0219 vs 0.022 does not
inline float eps_for(float a) { const float e = fabsf(a) * 1e-4f; return e > 1e-6f ? e : 1e-6f; }

bool parse_num(const char* s, float* out) {
    if (!s || !*s) return false;
    char* end = nullptr;
    const float v = (float)strtod(s, &end);
    if (end == s) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end) return false;
    *out = v;
    return true;
}

// case-insensitive substring
bool contains_ci(const char* hay, const char* needle, int nlen) {
    if (nlen <= 0) return true;
    const int hlen = (int)strlen(hay);
    for (int i = 0; i + nlen <= hlen; ++i) {
        int j = 0;
        while (j < nlen && tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j])) ++j;
        if (j == nlen) return true;
    }
    return false;
}

// iterate the '|'-separated parts of r.str; returns true if any part is contained
bool any_part_in(const char* value, const char* parts) {
    const char* p = parts;
    while (p && *p) {
        const char* bar = strchr(p, '|');
        const int len = bar ? (int)(bar - p) : (int)strlen(p);
        if (contains_ci(value, p, len)) return true;
        if (!bar) break;
        p = bar + 1;
    }
    return false;
}

}  // namespace

void rule_fmt(float v, char* out, int outsz) {
    if (fabsf(v - roundf(v)) < 1e-6f) snprintf(out, outsz, "%d", (int)roundf(v));
    else                              snprintf(out, outsz, "%g", v);
}

bool rule_ok(const RuleDef& r, const char* value) {
    if (!value) value = "";
    // A STRING cvar that is off holds "" (r_shownormals, r_xdebug: the renderer resets them
    // to "" and prints its usage line on anything it cannot parse). PB's "IN 0" on those
    // means "off", and the engine reads "" as 0 anyway - so "" satisfies a numeric 0.
    // Setting "0" there would start a set/reset fight with the renderer (console spam,
    // 2026-09-17).
    if (!value[0] && (r.mode == RM_EXACT || r.mode == RM_RANGE)) value = "0";
    float v = 0.f;
    switch (r.mode) {
    case RM_EXACT:
        return parse_num(value, &v) && fabsf(v - r.a) <= eps_for(r.a);
    case RM_RANGE:
        return parse_num(value, &v) && v >= r.a - eps_for(r.a) && v <= r.b + eps_for(r.b);
    case RM_OUT:
        // a non-numeric value cannot be "inside" a numeric interval
        if (!parse_num(value, &v)) return true;
        return !(v >= r.a - eps_for(r.a) && v <= r.b + eps_for(r.b));
    case RM_INCLUDE:
        return any_part_in(value, r.str);
    case RM_EXCLUDE:
        return !any_part_in(value, r.str);
    case RM_PROBE:
    default:
        return true;
    }
}

bool rule_fix(const RuleDef& r, const char* value, const char* def, char* out, int outsz) {
    if (!def) def = "";
    float v = 0.f;
    switch (r.mode) {
    case RM_EXACT:
        rule_fmt(r.a, out, outsz);
        return true;
    case RM_RANGE:
        if (!parse_num(value, &v)) v = r.a;
        rule_fmt(v < r.a ? r.a : (v > r.b ? r.b : v), out, outsz);
        return true;
    case RM_OUT:
        if (rule_ok(r, def)) { snprintf(out, outsz, "%s", def); return true; }
        rule_fmt(r.b + (fabsf(r.b) < 1.f ? 0.001f : 1.f), out, outsz);   // just past the band
        return true;
    case RM_INCLUDE:
        if (rule_ok(r, def)) { snprintf(out, outsz, "%s", def); return true; }
        {
            const char* bar = strchr(r.str, '|');
            const int len = bar ? (int)(bar - r.str) : (int)strlen(r.str);
            snprintf(out, outsz, "%.*s", len, r.str);
        }
        return true;
    case RM_EXCLUDE:
        if (rule_ok(r, def)) { snprintf(out, outsz, "%s", def); return true; }
        return false;
    case RM_PROBE:
    default:
        return false;
    }
}

}  // namespace patches
