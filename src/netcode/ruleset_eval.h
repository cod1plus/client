#ifndef COD1RELOADED_RULESET_EVAL_H
#define COD1RELOADED_RULESET_EVAL_H

// Rule semantics of a PunkBuster-style cvar list, without any engine dependency so the
// logic can be unit-tested on the host (tools/test_ruleset.cpp). The engine glue that
// walks the real cvar table is ruleset.cpp; the table itself is generated into
// ruleset_table.h by tools/gen_ruleset.py.

namespace patches {

enum RuleMode {
    RM_EXACT   = 0,   // value == a                    (numeric, tolerance 1e-4)
    RM_RANGE   = 1,   // a <= value <= b
    RM_OUT     = 2,   // NOT (a <= value <= b)
    RM_INCLUDE = 3,   // value contains one of str's '|'-separated parts (case-insensitive)
    RM_EXCLUDE = 4,   // value contains none of them
    RM_PROBE   = 5,   // the cvar must not exist at all (cheat config knob)
};

struct RuleDef {
    const char* cvar;
    int         mode;      // RuleMode
    int         cheat;     // 1 = CVAR_CHEAT in the engine (never ROM-lock those)
    float       a;
    float       b;
    const char* str;
};

// True when `value` (the cvar's string) satisfies the rule. RM_PROBE is not a value
// rule: the caller decides on existence. Numeric rules on a non-numeric value fail.
bool rule_ok(const RuleDef& r, const char* value);

// Write into out[outsz] the value the cvar should take to satisfy the rule, given the
// cvar's engine default (resetString). Policy: exact -> a; range -> nearest bound;
// out/include/exclude -> the default if it satisfies the rule, else a bound (OUT: just
// above b) or the first allowed substring (INCLUDE). Returns false when no fix exists.
bool rule_fix(const RuleDef& r, const char* value, const char* def, char* out, int outsz);

// Format a float the way the engine prints cvars ("1", "0.022", "-20").
void rule_fmt(float v, char* out, int outsz);

}  // namespace patches

#endif
