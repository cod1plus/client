# cod1reloaded — Fonctionnalités

| # | Feature | Cible | Effet | Config INI |
|---|---------|-------|-------|------------|
| 1 | Viewheight lerp speed | `gamex86.dll` | Transition crouch/standup plus smooth (140 unités/s au lieu de 180) | `viewheight_lerp_speed` |
| 2 | Lean fix carapace | `cgame_mp_x86.dll` | Réduit le twist du corps en lean+strafe (yaw/roll des bones du dos dampés) | `lean_fix_enable`, `lean_*_yaw_scale`, `lean_*_roll_scale` |
| 3 | Crouch posture | `cgame_mp_x86.dll` | Permet de redresser le buste en crouch via offsets pitch | `crouch_back_*_pitch`, `crouch_origin_z_offset` |
| 4 | Short version | `CoDMP.exe` | Remplace le "1.5" du menu par texte custom | `short_version` |
| 5 | Borderless windowed | Windows API | Fenêtre sans bordures plein écran sur le monitor choisi | `window_borderless`, `window_monitor_index` |
| 6 | Force windowed default | `CoDMP.exe` | Démarre en mode fenêtré → permet alt-tab fluide | `force_windowed_default` |
| 7 | Minimize on focus loss | Windows API | Minimise le jeu auto quand alt-tab → accès aux autres apps | `window_minimize_on_focus_loss` |
| 8 | Timer 1ms | Windows API | `timeBeginPeriod(1)` pour timing précis (base du FPS cap) | `force_1ms_timer` |
| 9 | Frame limiter précis | `CoDMP.exe` | `com_maxfps 250` cap réellement à 250 (vs 240-248 vanilla) | `frame_limiter_enable`, `frame_limiter_bias_us` |

## Infrastructure

| Composant | Rôle |
|-----------|------|
| `mss32.dll` proxy | DLL audio interceptée pour auto-load (renomme `mss32.dll` → `mss32_original.dll`) |
| `cod1reloaded.dll` | Version standalone pour injection manuelle |
| `cod1reloaded.ini` | Config hot-reload (édite, relance, pas besoin de rebuild) |
| `cod1reloaded.log` | Log de démarrage + diagnostic, à côté du DLL |

## Cibles touchées

- **`CoDMP.exe`** : short_version, force_windowed, frame_limiter
- **`gamex86.dll`** (serveur/physique) : viewheight_lerp_speed
- **`cgame_mp_x86.dll`** (client/rendu) : lean_fix, crouch_posture
- **Windows API** : borderless, timer, minimize
