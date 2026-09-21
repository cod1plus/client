#!/usr/bin/env python3
"""gen_ruleset.py - compile a PunkBuster cvar list into src/netcode/ruleset_table.h.

    python3 tools/gen_ruleset.py rulesets/codbase-2023-05.pb

The ruleset id is the file name without extension. Rule grammar (pbsv.cfg):
    pb_sv_cvar <cvar> IN a          -> value must equal a           (RM_EXACT)
    pb_sv_cvar <cvar> IN a b        -> a <= value <= b              (RM_RANGE)
    pb_sv_cvar <cvar> OUT a b       -> value must NOT be in [a, b]  (RM_OUT)
    pb_sv_cvar <cvar> INCLUDE s...  -> value must contain one of s  (RM_INCLUDE, case-insensitive)
    pb_sv_cvar <cvar> EXCLUDE s...  -> value must contain none of s (RM_EXCLUDE)
    ; version N                     -> RULESET_VERSION (the runtime fetch compares it)
The same grammar is parsed at runtime by src/netcode/ruleset_parse.cpp (lists downloaded from
the rulesets repo); this table is the offline fallback. Keep the two in sync.
A cvar that the CoD1 client never registers (tools/cvars_client.tsv, 504 engine + cgame cvars
with their flags) cannot be forced - PB used those names to DETECT cheat configs, so they become
RM_PROBE: if such a cvar exists at all on the client it is reported. CVAR_CHEAT cvars (0x200) are
reverted by the engine itself while sv_cheats is 0; their rules are still emitted (belt and
braces, cheap) but marked so the client can skip the ROM lock on them.
Several rules for one cvar are intersected (PB semantics: every rule must hold); an empty
intersection keeps the LAST rule in file order (the admin section wins) and warns.
"""
import os
import re
import sys
from collections import OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "src", "netcode", "ruleset_table.h")
TSV = os.path.join(HERE, "cvars_client.tsv")

RULE = re.compile(r'^\s*pb_sv_cvar\s+"?([A-Za-z0-9_]+)"?\s+(INCLUDE|EXCLUDE|IN|OUT)\b\s*(.*?)\s*$', re.I)
GAME = os.environ.get("COD1_GAME_DIR", r"C:\Users\bitpo\OneDrive\Bureau\Call of Duty - R 1.6 - dev")
# entries that are not cvar rules at all (bind / alias / command checks PB expressed with the
# same keyword); handled by the client's bind scan + the `wait` block, not by the table
SKIP_NAMES = {"set", "exec", "mouse2", "bind", "seta", "do_melee", "d0_melee", "d0_reload",
              "d0_attack", "pb_sleep", "cl_punkbuster"}


def num(s):
    try:
        return float(s)
    except ValueError:
        return None


def load_known():
    """cvar name -> (name, flags, default). The tsv (Cvar_Get call sites + cgame table) misses
    the cvars the engine registers through a computed flags register (com_maxfps, cl_avidemo,
    r_xdebug, s_show ...), so any PB name that is a C string in the client binaries is also
    accepted as an engine cvar, with unknown flags (0) and default "?"."""
    known = {}
    for line in open(TSV, encoding="utf-8"):
        if line.startswith("#"):
            continue
        n, f, d, src = line.rstrip("\n").split("\t")
        known[n.lower()] = (n, int(f, 16), d)
    strings = set()
    for rel in ("CoDMP.exe", os.path.join("Main", "cgame_mp_x86.dll"), os.path.join("Main", "ui_mp_x86.dll")):
        p = os.path.join(GAME, rel)
        if not os.path.exists(p):
            print("warning: %s not found, string check skipped" % p, file=sys.stderr)
            continue
        data = open(p, "rb").read()
        for m in re.finditer(rb"\x00([A-Za-z_][A-Za-z0-9_]{2,40})\x00", data):
            strings.add(m.group(1).decode().lower())
    return known, strings


VERSION = re.compile(r'^\s*(?:;|#|//)\s*version\s+(\d+)', re.I)


def parse(path):
    rules = OrderedDict()          # lower name -> list of (mode, vals, lineno)
    version = 0
    for ln, line in enumerate(open(path, encoding="latin-1"), 1):
        line = line.rstrip("\r\n").replace("*", "")      # "wwall1 IN 0****" junk
        if not version:
            mv = VERSION.match(line)
            if mv:
                version = int(mv.group(1))
        m = RULE.match(line)
        if not m:
            continue
        name, mode, rest = m.group(1), m.group(2).upper(), m.group(3).split()
        if name.lower() in SKIP_NAMES:
            continue
        rules.setdefault(name.lower(), []).append((name, mode, rest, ln))
    return rules, version


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "rulesets", "codbase-2023-05.pb")
    ruleset_id = os.path.splitext(os.path.basename(src))[0]
    known, strings = load_known()
    rules, version = parse(src)
    out, warns = [], []
    stats = {"exact": 0, "range": 0, "out": 0, "include": 0, "exclude": 0, "probe": 0, "cheatflag": 0}
    for key, lst in rules.items():
        display = lst[-1][0]
        k = known.get(key)
        if k is None and key in strings:
            k = (display, 0, "?")            # engine cvar with unknown flags (see load_known)
        if k is None and key.startswith("scr_"):
            # server-script cvar (PAM registers them in the game module): it exists on a
            # client only when that client HOSTS a match, and then it is a match rule to
            # hold, not a cheat-cvar probe
            k = (display, 0, "?")
        if k is None:
            # not an engine cvar: existence is the finding
            out.append((display, "RM_PROBE", 0, 0.0, 0.0, ""))
            stats["probe"] += 1
            continue
        name, flags, default = k
        cheat = 1 if flags & 0x200 else 0
        lo, hi = None, None            # intersection of IN rules
        exact_str = None
        for _, mode, vals, ln in lst:
            if mode == "IN":
                if len(vals) == 1:
                    a = num(vals[0])
                    if a is None:
                        exact_str = vals[0]
                        continue
                    a2, b2 = a, a
                elif len(vals) >= 2 and num(vals[0]) is not None and num(vals[1]) is not None:
                    a2, b2 = num(vals[0]), num(vals[1])
                else:
                    warns.append(f"line {ln}: {name} IN {' '.join(vals)}: unparsed, skipped")
                    continue
                if lo is None:
                    lo, hi = a2, b2
                else:
                    nlo, nhi = max(lo, a2), min(hi, b2)
                    if nlo > nhi:
                        warns.append(f"line {ln}: {name}: IN {a2:g} {b2:g} does not intersect [{lo:g},{hi:g}] -> last rule wins")
                        lo, hi = a2, b2
                    else:
                        lo, hi = nlo, nhi
            elif mode == "OUT" and len(vals) >= 2 and num(vals[0]) is not None and num(vals[1]) is not None:
                out.append((name, "RM_OUT", cheat, num(vals[0]), num(vals[1]), ""))
                stats["out"] += 1
            elif mode in ("INCLUDE", "EXCLUDE") and vals:
                subs = "|".join(v.strip('"') for v in vals)
                out.append((name, "RM_" + mode, cheat, 0.0, 0.0, subs))
                stats[mode.lower()] += 1
            else:
                warns.append(f"line {ln}: {name} {mode} {' '.join(vals)}: unparsed, skipped")
        if exact_str is not None:
            out.append((name, "RM_INCLUDE", cheat, 0.0, 0.0, exact_str))
            stats["include"] += 1
        if lo is not None:
            if lo == hi:
                out.append((name, "RM_EXACT", cheat, lo, lo, ""))
                stats["exact"] += 1
            else:
                out.append((name, "RM_RANGE", cheat, lo, hi, ""))
                stats["range"] += 1
        if cheat:
            stats["cheatflag"] += 1

    with open(OUT, "w", encoding="ascii", newline="\n") as f:
        f.write("// GENERATED by tools/gen_ruleset.py from rulesets/%s - do not edit.\n" % os.path.basename(src))
        f.write("// %d rules: %s.\n" % (len(out), ", ".join(f"{k} {v}" for k, v in stats.items())))
        f.write("#pragma once\n#include \"netcode/ruleset_eval.h\"\n\nnamespace patches {\n\n")
        f.write('constexpr const char* RULESET_ID = %s;\n' % cstr(ruleset_id))
        f.write('constexpr int RULESET_VERSION = %d;   // "; version N" header of the .pb\n\n' % version)
        f.write("constexpr RuleDef RULESET_TABLE[] = {\n")
        for name, mode, cheat, a, b, s in out:
            f.write("    { %s, %s, %d, %sf, %sf, %s },\n" % (cstr(name), mode, cheat, repr(float(a)), repr(float(b)), cstr(s)))
        f.write("};\nconstexpr int RULESET_COUNT = %d;\n\n}  // namespace patches\n" % len(out))
    print(f"{OUT}: {len(out)} rules, version {version} ({stats})")
    for w in warns:
        print("  warning:", w)


if __name__ == "__main__":
    main()
