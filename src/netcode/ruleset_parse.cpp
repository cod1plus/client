// ruleset_parse.cpp - runtime port of tools/gen_ruleset.py. See ruleset_parse.h.

#include "netcode/ruleset_parse.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace patches {

namespace {

// entries that are not cvar rules at all (bind / alias / command checks PB expressed
// with the same keyword) - same list as the generator
const char* const SKIP_NAMES[] = { "set", "exec", "mouse2", "bind", "seta", "do_melee",
                                   "d0_melee", "d0_reload", "d0_attack", "pb_sleep",
                                   "cl_punkbuster" };

std::string lower(const std::string& s) {
    std::string r(s);
    for (size_t i = 0; i < r.size(); ++i) r[i] = (char)tolower((unsigned char)r[i]);
    return r;
}

bool ieq(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

bool is_ws(char c) { return c == ' ' || c == '\t'; }

bool num(const std::string& s, float* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = strtod(s.c_str(), &end);
    if (end == s.c_str() || *end) return false;
    *out = (float)v;
    return true;
}

struct RawRule {
    std::string              mode;   // IN / OUT / INCLUDE / EXCLUDE
    std::vector<std::string> vals;
    int                      line;
};

struct Group {
    std::string          display;    // name as written by the last rule
    std::vector<RawRule> rules;
};

struct Emitted {
    std::string name;
    int         mode;
    float       a, b;
    std::string str;
};

void warn(std::string* w, int line, const std::string& msg) {
    if (!w) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "line %d: ", line);
    *w += buf;
    *w += msg;
    *w += '\n';
}

// "pb_sv_cvar <name> <KEYWORD> <rest...>" -> false when the line is not a rule
bool match_rule(const std::string& line, std::string& name, std::string& mode,
                std::vector<std::string>& vals) {
    size_t i = 0;
    const size_t n = line.size();
    while (i < n && is_ws(line[i])) ++i;
    static const char KW[] = "pb_sv_cvar";
    if (n - i < sizeof(KW) - 1 || !ieq(line.c_str() + i, KW, sizeof(KW) - 1)) return false;
    i += sizeof(KW) - 1;
    if (i >= n || !is_ws(line[i])) return false;
    while (i < n && is_ws(line[i])) ++i;
    if (i < n && line[i] == '"') ++i;
    const size_t ns = i;
    while (i < n && (isalnum((unsigned char)line[i]) || line[i] == '_')) ++i;
    if (i == ns) return false;
    name.assign(line, ns, i - ns);
    if (i < n && line[i] == '"') ++i;
    if (i >= n || !is_ws(line[i])) return false;
    while (i < n && is_ws(line[i])) ++i;
    const size_t ks = i;
    while (i < n && isalpha((unsigned char)line[i])) ++i;
    mode.assign(line, ks, i - ks);
    for (size_t k = 0; k < mode.size(); ++k) mode[k] = (char)toupper((unsigned char)mode[k]);
    if (mode != "IN" && mode != "OUT" && mode != "INCLUDE" && mode != "EXCLUDE") return false;
    if (i < n && !is_ws(line[i])) return false;          // the \b of the generator
    vals.clear();
    while (i < n) {
        while (i < n && is_ws(line[i])) ++i;
        const size_t vs = i;
        while (i < n && !is_ws(line[i])) ++i;
        if (i > vs) vals.push_back(line.substr(vs, i - vs));
    }
    return true;
}

std::string strip_quotes(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] == '"') ++a;
    while (b > a && s[b - 1] == '"') --b;
    return s.substr(a, b - a);
}

}  // namespace

bool ruleset_parse(const char* text, size_t len, RuleList& out, std::string* warnings) {
    std::vector<Group>            groups;
    std::map<std::string, size_t> index;
    int version = 0;
    bool have_version = false;

    size_t pos = 0;
    int lineno = 0;
    while (pos < len) {
        size_t eol = pos;
        while (eol < len && text[eol] != '\n') ++eol;
        std::string line(text + pos, eol - pos);
        pos = eol + 1;
        ++lineno;
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        // "wwall1 IN 0****" junk, as the generator
        { std::string clean; clean.reserve(line.size());
          for (size_t i = 0; i < line.size(); ++i) if (line[i] != '*') clean += line[i];
          line.swap(clean); }

        // comment line: maybe the version header
        {
            size_t i = 0;
            while (i < line.size() && is_ws(line[i])) ++i;
            size_t c = 0;
            if (i < line.size() && (line[i] == ';' || line[i] == '#')) c = 1;
            else if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/') c = 2;
            if (c) {
                i += c;
                while (i < line.size() && is_ws(line[i])) ++i;
                if (!have_version && line.size() - i > 7 && ieq(line.c_str() + i, "version", 7) &&
                    is_ws(line[i + 7])) {
                    i += 7;
                    while (i < line.size() && is_ws(line[i])) ++i;
                    if (i < line.size() && isdigit((unsigned char)line[i])) {
                        version = atoi(line.c_str() + i);
                        have_version = true;
                    }
                }
                continue;
            }
        }

        std::string name, mode;
        std::vector<std::string> vals;
        if (!match_rule(line, name, mode, vals)) continue;
        const std::string key = lower(name);
        bool skip = false;
        for (size_t k = 0; k < sizeof(SKIP_NAMES) / sizeof(SKIP_NAMES[0]); ++k)
            if (key == SKIP_NAMES[k]) { skip = true; break; }
        if (skip) continue;

        std::map<std::string, size_t>::iterator it = index.find(key);
        if (it == index.end()) {
            index[key] = groups.size();
            groups.push_back(Group());
            it = index.find(key);
        }
        Group& g = groups[it->second];
        g.display = name;
        RawRule rr; rr.mode = mode; rr.vals = vals; rr.line = lineno;
        g.rules.push_back(rr);
    }

    std::vector<Emitted> emitted;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const Group& g = groups[gi];
        bool have_range = false;
        float lo = 0.f, hi = 0.f;
        bool have_exact_str = false;
        std::string exact_str;
        for (size_t ri = 0; ri < g.rules.size(); ++ri) {
            const RawRule& r = g.rules[ri];
            std::string joined;
            for (size_t v = 0; v < r.vals.size(); ++v) { if (v) joined += ' '; joined += r.vals[v]; }
            if (r.mode == "IN") {
                float a2 = 0.f, b2 = 0.f;
                if (r.vals.size() == 1) {
                    if (!num(r.vals[0], &a2)) { exact_str = r.vals[0]; have_exact_str = true; continue; }
                    b2 = a2;
                } else if (r.vals.size() >= 2 && num(r.vals[0], &a2) && num(r.vals[1], &b2)) {
                    /* range */
                } else {
                    warn(warnings, r.line, g.display + " IN " + joined + ": unparsed, skipped");
                    continue;
                }
                if (!have_range) { lo = a2; hi = b2; have_range = true; }
                else {
                    const float nlo = lo > a2 ? lo : a2, nhi = hi < b2 ? hi : b2;
                    if (nlo > nhi) {
                        warn(warnings, r.line, g.display + ": IN " + joined + " does not intersect the previous rule -> last rule wins");
                        lo = a2; hi = b2;
                    } else { lo = nlo; hi = nhi; }
                }
            } else if (r.mode == "OUT") {
                float a2 = 0.f, b2 = 0.f;
                if (r.vals.size() >= 2 && num(r.vals[0], &a2) && num(r.vals[1], &b2)) {
                    Emitted e; e.name = g.display; e.mode = RM_OUT; e.a = a2; e.b = b2; emitted.push_back(e);
                } else warn(warnings, r.line, g.display + " OUT " + joined + ": unparsed, skipped");
            } else if (!r.vals.empty()) {     // INCLUDE / EXCLUDE
                Emitted e; e.name = g.display; e.mode = (r.mode == "INCLUDE") ? RM_INCLUDE : RM_EXCLUDE;
                e.a = e.b = 0.f;
                for (size_t v = 0; v < r.vals.size(); ++v) { if (v) e.str += '|'; e.str += strip_quotes(r.vals[v]); }
                emitted.push_back(e);
            } else {
                warn(warnings, r.line, g.display + " " + r.mode + ": unparsed, skipped");
            }
        }
        if (have_exact_str) {
            Emitted e; e.name = g.display; e.mode = RM_INCLUDE; e.a = e.b = 0.f; e.str = exact_str; emitted.push_back(e);
        }
        if (have_range) {
            Emitted e; e.name = g.display; e.mode = (lo == hi) ? RM_EXACT : RM_RANGE; e.a = lo; e.b = hi; emitted.push_back(e);
        }
    }
    if (emitted.empty()) return false;

    // pack: one blob, pointers into it (the blob never reallocates afterwards)
    size_t need = 0;
    for (size_t i = 0; i < emitted.size(); ++i) need += emitted[i].name.size() + 1 + emitted[i].str.size() + 1;
    out.rules.clear();
    out.blob.assign(need, 0);
    out.rules.reserve(emitted.size());
    size_t at = 0;
    for (size_t i = 0; i < emitted.size(); ++i) {
        const Emitted& e = emitted[i];
        RuleDef d;
        d.cvar = &out.blob[at];
        memcpy(&out.blob[at], e.name.c_str(), e.name.size() + 1);
        at += e.name.size() + 1;
        d.str = &out.blob[at];
        memcpy(&out.blob[at], e.str.c_str(), e.str.size() + 1);
        at += e.str.size() + 1;
        d.mode = e.mode;
        d.cheat = 0;            // decided live from the cvar flags by the enforcer
        d.a = e.a;
        d.b = e.b;
        out.rules.push_back(d);
    }
    out.version = version;
    return true;
}

void ruleset_from_table(const RuleDef* table, int count, const char* id, int version, RuleList& out) {
    out.id = id ? id : "";
    out.version = version;
    out.source = "embedded";
    out.blob.clear();
    out.rules.assign(table, table + count);
}

void ruleset_split_id(const char* full, std::string& base, int& version) {
    base.clear();
    version = 0;
    if (!full) return;
    const char* at = strchr(full, '@');
    if (!at) { base = full; return; }
    base.assign(full, at - full);
    version = atoi(at + 1);
    if (version < 0) version = 0;
}

}  // namespace patches
