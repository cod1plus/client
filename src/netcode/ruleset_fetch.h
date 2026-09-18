#ifndef COD1RELOADED_RULESET_FETCH_H
#define COD1RELOADED_RULESET_FETCH_H

// Ruleset store: which list is enforced for a given id, and where it comes from.
//
// Sources, best version wins (ties: the most recent download):
//   embedded   ruleset_table.h, compiled in - the offline fallback, always present
//   cache      <game dir>\rulesets\<id>.pb, the last successful download
//   download   <ruleset_url><id>.pb over HTTPS (default: the cod1plus/rulesets GitHub repo,
//              the same trust root that ships mss32.dll itself)
//
// HOT RELOAD: the server publishes "<id>@<version>" (sv_competitive_ruleset). When a
// client's best list for <id> is older than <version> it downloads again, in game, and
// switches lists on the next enforcement pass. At start-up the embedded id is refreshed
// unconditionally (a rule edited without a version bump still reaches everyone at the
// next launch). Downloads are rate limited (60 s after a failure, 5 min when the file
// is up but still behind the version the server asks for).
//
// THREADING: downloads run on their own thread; the store is a critical section. The
// enforcer takes the list with ruleset_store_acquire() and walks it while holding the
// lock (a few hundred string compares), then ruleset_store_release().

#include "netcode/ruleset_parse.h"

namespace patches {

struct RulesetFetchConfig {
    bool enable;            // ruleset_fetch_enable: false = embedded / cache only
    char base_url[256];     // ruleset_url: directory URL, "<id>.pb" is appended
};
extern RulesetFetchConfig g_ruleset_fetch_config;

void ruleset_store_init();   // once, from the watcher thread (after the .ini was read)

// Periodic refresh: every RULESET_REFRESH_MIN minutes every list we hold is downloaded
// again, so an edit online reaches running clients without any server involvement
// (the server's "<id>@<version>" only makes it immediate). Cheap: one small GET.
constexpr int RULESET_REFRESH_MIN = 10;
void ruleset_store_tick();

// Best list for <base> with version >= <version>, or the best we have for <base> (older),
// or nullptr when no list of that id exists at all. Starts a download when what we have
// is not good enough. The lock is held on a non-null return: call release() after use.
const RuleList* ruleset_store_acquire(const char* base, int version);
void            ruleset_store_release();

}  // namespace patches

#endif
