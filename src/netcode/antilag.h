// antilag (g_antilag), CoD1 server module. docs/lag_compensation_cod1_plan.md

#ifndef COD1RELOADED_ANTILAG_H
#define COD1RELOADED_ANTILAG_H

namespace patches {

struct AntilagConfig {
    bool diag_enable;
    int  diag_log_count;    // 0 = unlimited
    bool fire_hook_enable;
    bool capture_enable;
    bool rewind_enable;
    int  rewind_test_z;     // !=0: fixed Z offset instead of ring
    bool rewind_test_self;  // also move the shooter
};

extern AntilagConfig g_antilag_config;

// call from DllMain; no-op if all phases disabled
void antilag_start();

}  // namespace patches

#endif
