#ifndef COD1RELOADED_RULESET_PARSE_H
#define COD1RELOADED_RULESET_PARSE_H

// Runtime parser of a PunkBuster cvar list (pbsv.cfg grammar), engine-free so it is
// unit-tested on the host (tools/test_ruleset.cpp). Same grammar as tools/gen_ruleset.py:
//     pb_sv_cvar <cvar> IN a          -> RM_EXACT a      (non-numeric a: RM_INCLUDE a)
//     pb_sv_cvar <cvar> IN a b        -> RM_RANGE [a, b]  (several IN rules intersect)
//     pb_sv_cvar <cvar> OUT a b       -> RM_OUT
//     pb_sv_cvar <cvar> INCLUDE s...  -> RM_INCLUDE (s joined by '|')
//     pb_sv_cvar <cvar> EXCLUDE s...  -> RM_EXCLUDE
//     ; version N                     -> list version (any comment line, first one wins)
// Unlike the generator there is no PROBE classification here (no cvar table at hand):
// a rule on a name the client never registers is simply never enforced, and a rule on
// a cheat-config knob that DOES exist is enforced like any other (set + locked, which
// neutralises the knob - PB could only kick). Existence reporting of the curated cheat
// names stays with cheat_scan.cpp.

#include "netcode/ruleset_eval.h"

#include <string>
#include <vector>

namespace patches {

// A parsed list, self-owned: rules[].cvar / .str point into blob (or into static
// storage for a list built from the compiled table).
struct RuleList {
    std::string          id;        // base id, e.g. "codbase-2023-05" (the file name)
    int                  version;   // "; version N" header, 0 when absent
    std::string          source;    // "embedded" / "cache" / "download" (log only)
    std::vector<RuleDef> rules;
    std::vector<char>    blob;
    RuleList() : version(0) {}
    // never copied: rules[] points into THIS object's blob
    RuleList(const RuleList&) = delete;
    RuleList& operator=(const RuleList&) = delete;
};

// Returns false when the text holds no rule at all. `warnings` (optional) receives one
// line per rule skipped or overridden.
bool ruleset_parse(const char* text, size_t len, RuleList& out, std::string* warnings);

// The embedded fallback: wrap a compiled table (string pointers stay static).
void ruleset_from_table(const RuleDef* table, int count, const char* id, int version, RuleList& out);

// "<base>@<version>" as the server publishes it -> base + version (0 when absent).
void ruleset_split_id(const char* full, std::string& base, int& version);

}  // namespace patches

#endif
