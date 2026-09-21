// test_ruleset.cpp - host unit test of the rule semantics (no engine needed).
//   g++ -std=c++17 -I src tools/test_ruleset.cpp src/netcode/ruleset_eval.cpp src/netcode/ruleset_parse.cpp -o build/test_ruleset && build/test_ruleset
//   (run from the repo root: it reads rulesets/codbase-2023-05.pb)
#include "netcode/ruleset_eval.h"
#include "netcode/ruleset_parse.h"
#include "netcode/ruleset_table.h"

#include <cctype>
#include <cstdio>
#include <cstring>

using namespace patches;

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { ++fails; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

// x87 excess precision: `r.a == 0.022f` compares the float against the literal in long
// double on i686 (-fexcess-precision=fast), so go through float parameters
static bool feq(float a, float b) { return a == b; }
static bool ieq(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return !*a && !*b;
}

static bool fix_eq(const RuleDef& r, const char* v, const char* def, const char* want) {
    char out[64] = "";
    const bool ok = rule_fix(r, v, def, out, sizeof(out));
    if (!ok || strcmp(out, want)) { printf("   fix(%s '%s' def '%s') = '%s' (ok=%d), want '%s'\n", r.cvar, v, def, out, ok, want); return false; }
    return true;
}

int main() {
    RuleDef exact0   = { "cl_nodelta",  RM_EXACT,   0, 0.f,     0.f,   "" };
    RuleDef exactF   = { "m_yaw",       RM_EXACT,   0, 0.022f,  0.022f,"" };
    RuleDef range    = { "cl_timenudge",RM_RANGE,   0, -20.f,   0.f,   "" };
    RuleDef range0   = { "r_picmip",    RM_RANGE,   0, 0.f,     3.f,   "" };
    RuleDef out      = { "m_pitch",     RM_OUT,     0, -0.011f, 0.011f,"" };
    RuleDef outRef   = { "r_displayrefresh", RM_OUT, 0, 0.1f,   59.f,  "" };
    RuleDef incl     = { "r_textureMode", RM_INCLUDE, 0, 0.f,   0.f,   "GL_LINEAR_MIPMAP_" };
    RuleDef incl2    = { "r_nv_fogdist_mode", RM_INCLUDE, 0, 0.f, 0.f, "nv|gl_eye_radial_nv" };
    RuleDef excl     = { "r_xdebug",    RM_EXCLUDE, 0, 0.f,     0.f,   "axes|boxes|both" };

    // exact, incl. the zero that the old integer code dropped
    CHECK(rule_ok(exact0, "0"));
    CHECK(rule_ok(exact0, "0.0"));
    CHECK(!rule_ok(exact0, "1"));
    CHECK(rule_ok(exact0, ""));                 // "" = a string cvar that is off = 0
    CHECK(!rule_ok(exactF, ""));                // but "" is not 0.022
    CHECK(rule_ok(range0, ""));                 // 0 is inside [0,3]
    CHECK(!rule_ok(range, "") == false);        // 0 is inside [-20,0]
    CHECK(!rule_ok(exact0, "abc"));
    CHECK(fix_eq(exact0, "1", "0", "0"));
    CHECK(rule_ok(exactF, "0.022"));
    CHECK(!rule_ok(exactF, "0.0219"));
    CHECK(fix_eq(exactF, "0.5", "0.022", "0.022"));

    // ranges with negative and zero bounds
    CHECK(rule_ok(range, "-20"));
    CHECK(rule_ok(range, "0"));
    CHECK(rule_ok(range, "-7.5"));
    CHECK(!rule_ok(range, "-21"));
    CHECK(!rule_ok(range, "1"));
    CHECK(fix_eq(range, "-50", "0", "-20"));
    CHECK(fix_eq(range, "5", "0", "0"));
    CHECK(rule_ok(range0, "0"));
    CHECK(rule_ok(range0, "3"));
    CHECK(!rule_ok(range0, "4"));
    CHECK(fix_eq(range0, "9", "1", "3"));
    CHECK(fix_eq(range0, "junk", "1", "0"));

    // OUT: forbidden band; non-numeric never "inside"
    CHECK(rule_ok(out, "0.022"));
    CHECK(rule_ok(out, "-0.022"));
    CHECK(!rule_ok(out, "0"));
    CHECK(!rule_ok(out, "0.005"));
    CHECK(!rule_ok(out, "-0.01"));
    CHECK(rule_ok(out, "0.011001") == false);   // boundary with tolerance counts as inside
    CHECK(fix_eq(out, "0", "0.022", "0.022"));  // default satisfies -> default
    CHECK(fix_eq(out, "0", "0.005", "0.012"));  // default violates -> just past the band
    CHECK(rule_ok(outRef, "0"));
    CHECK(rule_ok(outRef, "144"));
    CHECK(!rule_ok(outRef, "50"));
    CHECK(fix_eq(outRef, "50", "0", "0"));

    // INCLUDE / EXCLUDE, case-insensitive, '|' alternatives
    CHECK(rule_ok(incl, "GL_LINEAR_MIPMAP_NEAREST"));
    CHECK(rule_ok(incl, "gl_linear_mipmap_linear"));
    CHECK(!rule_ok(incl, "GL_NEAREST"));
    CHECK(fix_eq(incl, "GL_NEAREST", "GL_LINEAR_MIPMAP_NEAREST", "GL_LINEAR_MIPMAP_NEAREST"));
    CHECK(fix_eq(incl, "GL_NEAREST", "GL_NEAREST", "GL_LINEAR_MIPMAP_"));
    CHECK(rule_ok(incl2, "GL_EYE_RADIAL_NV"));
    CHECK(!rule_ok(incl2, "GL_EYE_PLANE"));
    CHECK(rule_ok(excl, "0"));
    CHECK(!rule_ok(excl, "boxes"));
    CHECK(!rule_ok(excl, "BOTH"));
    CHECK(fix_eq(excl, "axes", "0", "0"));
    {
        char o[8]; CHECK(!rule_fix(excl, "axes", "axes", o, sizeof(o)));   // no fix possible
    }

    // formatting
    char f[32];
    rule_fmt(25000.f, f, sizeof(f)); CHECK(!strcmp(f, "25000"));
    rule_fmt(-20.f, f, sizeof(f));   CHECK(!strcmp(f, "-20"));
    rule_fmt(0.022f, f, sizeof(f));  CHECK(!strcmp(f, "0.022"));
    rule_fmt(0.0075f, f, sizeof(f)); CHECK(!strcmp(f, "0.0075"));

    // the generated table: sane and every value rule self-consistent
    int probes = 0, values = 0;
    for (int i = 0; i < RULESET_COUNT; ++i) {
        const RuleDef& r = RULESET_TABLE[i];
        CHECK(r.cvar && r.cvar[0]);
        if (r.mode == RM_PROBE) { ++probes; continue; }
        ++values;
        char o[64];
        if (r.mode == RM_EXACT)  { rule_fmt(r.a, o, sizeof(o)); CHECK(rule_ok(r, o)); }
        if (r.mode == RM_RANGE)  { CHECK(r.a <= r.b); rule_fmt(r.a, o, sizeof(o)); CHECK(rule_ok(r, o)); }
        if (r.mode == RM_INCLUDE || r.mode == RM_EXCLUDE) CHECK(r.str && r.str[0]);
    }
    printf("table: %d rules = %d value rules + %d probes, id %s version %d\n", RULESET_COUNT, values, probes, RULESET_ID, RULESET_VERSION);
    CHECK(!strcmp(RULESET_ID, "codbase-2023-05"));
    CHECK(RULESET_VERSION >= 1);

    // the runtime parser on the same .pb must yield every value rule of the table, equal
    {
        FILE* pf = fopen("rulesets/codbase-2023-05.pb", "rb");
        CHECK(pf != nullptr);
        if (pf) {
            fseek(pf, 0, SEEK_END); long n = ftell(pf); fseek(pf, 0, SEEK_SET);
            char* text = new char[n + 1];
            CHECK((long)fread(text, 1, n, pf) == n);
            text[n] = 0;
            fclose(pf);
            RuleList rl;
            std::string warns;
            CHECK(ruleset_parse(text, (size_t)n, rl, &warns));
            CHECK(rl.version == RULESET_VERSION);
            int matched = 0, missing = 0;
            for (int i = 0; i < RULESET_COUNT; ++i) {
                const RuleDef& t = RULESET_TABLE[i];
                if (t.mode == RM_PROBE) continue;
                bool found = false;
                for (size_t j = 0; j < rl.rules.size() && !found; ++j) {
                    const RuleDef& r = rl.rules[j];
                    found = ieq(r.cvar, t.cvar) && r.mode == t.mode && feq(r.a, t.a) && feq(r.b, t.b) && !strcmp(r.str, t.str);
                }
                if (found) ++matched; else { ++missing; printf("   runtime parse lacks: %s mode %d %g %g '%s'\n", t.cvar, t.mode, t.a, t.b, t.str); }
            }
            CHECK(missing == 0);
            // every probe name of the table is now a value rule (enforced if the cvar exists)
            int probes_covered = 0;
            for (int i = 0; i < RULESET_COUNT; ++i) {
                if (RULESET_TABLE[i].mode != RM_PROBE) continue;
                for (size_t j = 0; j < rl.rules.size(); ++j)
                    if (ieq(rl.rules[j].cvar, RULESET_TABLE[i].cvar)) { ++probes_covered; break; }
            }
            printf("runtime parse: %d rules (version %d), %d/%d table value rules matched, %d/%d probe names covered, warnings:\n%s",
                   (int)rl.rules.size(), rl.version, matched, values, probes_covered, probes, warns.c_str());
            for (size_t j = 0; j < rl.rules.size(); ++j) CHECK(rl.rules[j].mode != RM_PROBE);
            delete[] text;
        }
    }

    // parser corner cases
    {
        const char* txt =
            "// version 7\r\n"
            "pb_sv_cvar \"m_yaw\" IN 0.022\n"
            "pb_sv_cvar r_picmip IN 0 3\n"
            "pb_sv_cvar r_picmip in 1 5\n"              // intersects -> [1,3]
            "pb_sv_cvar   r_textureMode INCLUDE \"GL_LINEAR_MIPMAP_\"\n"
            "pb_sv_cvar r_xdebug EXCLUDE axes boxes both\n"
            "pb_sv_cvar m_pitch OUT -0.011 0.011\n"
            "pb_sv_cvar wwall1 IN 0****\n"
            "pb_sv_cvar bind IN 0\n"                    // skipped name
            "pb_sv_cvar r_mode IN abc\n"                // non-numeric exact -> INCLUDE
            "pb_sv_cvar broken INSIDE 1\n"              // not a keyword
            "seta something 1\n";
        RuleList rl; std::string w;
        CHECK(ruleset_parse(txt, strlen(txt), rl, &w));
        CHECK(rl.version == 7);
        CHECK(rl.rules.size() == 7);
        bool ok_yaw = false, ok_picmip = false, ok_tex = false, ok_xdbg = false, ok_pitch = false, ok_wwall = false, ok_mode = false;
        for (size_t j = 0; j < rl.rules.size(); ++j) {
            const RuleDef& r = rl.rules[j];
            if (!strcmp(r.cvar, "m_yaw"))        ok_yaw    = r.mode == RM_EXACT && feq(r.a, 0.022f);
            if (!strcmp(r.cvar, "r_picmip"))     ok_picmip = r.mode == RM_RANGE && feq(r.a, 1.f) && feq(r.b, 3.f);
            if (!strcmp(r.cvar, "r_textureMode"))ok_tex    = r.mode == RM_INCLUDE && !strcmp(r.str, "GL_LINEAR_MIPMAP_");
            if (!strcmp(r.cvar, "r_xdebug"))     ok_xdbg   = r.mode == RM_EXCLUDE && !strcmp(r.str, "axes|boxes|both");
            if (!strcmp(r.cvar, "m_pitch"))      ok_pitch  = r.mode == RM_OUT && feq(r.a, -0.011f) && feq(r.b, 0.011f);
            if (!strcmp(r.cvar, "wwall1"))       ok_wwall  = r.mode == RM_EXACT && feq(r.a, 0.f);
            if (!strcmp(r.cvar, "r_mode"))       ok_mode   = r.mode == RM_INCLUDE && !strcmp(r.str, "abc");
            CHECK(strcmp(r.cvar, "bind") != 0);
            CHECK(strcmp(r.cvar, "broken") != 0);
        }
        CHECK(ok_yaw); CHECK(ok_picmip); CHECK(ok_tex); CHECK(ok_xdbg); CHECK(ok_pitch); CHECK(ok_wwall); CHECK(ok_mode);
        RuleList empty; CHECK(!ruleset_parse("; nothing\n", 10, empty, nullptr));
        std::string base; int ver = -1;
        ruleset_split_id("codbase-2023-05@7", base, ver); CHECK(base == "codbase-2023-05" && ver == 7);
        ruleset_split_id("codbase-2023-05", base, ver);   CHECK(base == "codbase-2023-05" && ver == 0);
        ruleset_split_id("", base, ver);                  CHECK(base.empty() && ver == 0);
        RuleList emb; ruleset_from_table(RULESET_TABLE, RULESET_COUNT, RULESET_ID, RULESET_VERSION, emb);
        CHECK((int)emb.rules.size() == RULESET_COUNT && emb.version == RULESET_VERSION && emb.id == RULESET_ID);
    }

    printf(fails ? "%d FAILURE(S)\n" : "all checks passed\n", fails);
    return fails ? 1 : 0;
}
