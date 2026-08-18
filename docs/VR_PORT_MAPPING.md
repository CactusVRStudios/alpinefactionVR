# Alpine Faction VR prototype port mapping

This mapping records the port from the uncommitted Dash Faction prototype at
`D:\Red Faction\dashfaction-source` (Git `9534cba807beee49aa1ef9003e0a40be1ae6bc30` plus its working-tree changes)
to Alpine Faction baseline `f5c7570f29fd345fcb19a93429480d8d8c512ca9` on branch `alpine-vr`.
The Dash tree is an input only; all implementation and validation occurs in this Alpine tree.

## Source-to-destination mapping

| Dash prototype area | Alpine destination | Integration decision |
| --- | --- | --- |
| Root and `game_patch/CMakeLists.txt` OpenXR option | Same CMake layers | Renamed to `AF_ENABLE_OPENXR`; official Khronos SDK 1.1.60 is pinned and its Win32 static loader is linked only when enabled. |
| `game_patch/vr/vr.*` | `game_patch/vr/vr.*` | Namespace/log prefix renamed to `afvr`/`AFVR`; Alpine game settings and command registration are retained. |
| `game_patch/vr/openxr_context.*` | Same path | Existing Alpine-owned D3D11 device is bound through `XR_KHR_D3D11_enable`; no second device or dynamic loader DLL is used. |
| `game_patch/vr/vr_render_bridge.*` | Same path plus Alpine D3D11 renderer | External eye/HUD targets, target restoration, menu copy, and right-eye mirror use Alpine's renderer and gamma/present pipeline. |
| Dash D3D11 projection/renderer/hooks/mesh changes | `game_patch/graphics/d3d11/gr_d3d11_{transform,context,hooks,mesh}.*` | Asymmetric eye projection and external targets are layered onto Alpine's MSAA, gamma pass, outline renderer, emissive/per-pixel-lighting, and mesh paths. |
| Dash main-loop VR lifecycle | `game_patch/main/main.cpp` | Command-line registration, post-D3D11 initialization, one XR update per RF frame, timing, and teardown are inserted around Alpine's existing frame work. |
| Dash semantic action hook | `game_patch/input/control_input_filter.*` registry plus `vr.cpp` injection | Composed into Alpine's existing hook owner instead of installing a second hook at the same address. |
| Dash physical mouse policy | `game_patch/input/mouse.cpp` and VR mouse query hooks | Physical deltas/buttons are suppressed only during supported active VR states; controller UI input is separate. |
| Dash portal/world stereo hook and culling instrumentation | `game_patch/vr/vr.cpp`, Alpine D3D11 hooks | Simulation remains once per game frame; only audited portal/world/weapon render work is repeated per eye. |
| Dash first-person weapon transform and hand-material filter | `game_patch/misc/player_fpgun.cpp`, `gr_d3d11_mesh.cpp`, `vr.cpp` | Alpine outline/VFX flushes remain, the desktop viewmodel projection is bypassed only inside VR eye passes, and only dedicated arm/hand material batches are hidden. |
| Contextual two-hand gun handling | `game_patch/vr/vr.cpp`, shared firing/effect consumers | Left squeeze near a per-gun support point acquires a support grip. A stable blend of both aim poses and the physical hand baseline drives the visual weapon, calibrated muzzle, firing, projectiles, flamethrower, and shell orientation. Throwables and non-guns remain one-handed; left squeeze remains Use when outside a support capture region. |
| Dash shot/projectile aim override | `game_patch/misc/player.cpp` | The final common automatic-fire seam and final projectile factory are used; hitscan and projectiles share the tracked aim contract while Alpine weapon logic remains authoritative. |
| Dash shell ejection | `game_patch/object/entity.cpp` | Local singleplayer VR shells receive the prototype's upward/forward offset without replacing Alpine's existing shell-distance patch. |
| Dash HUD/weapon-selection changes | `game_patch/hud/hud_weapons.cpp`, `weapon_select.cpp`, `hud_internal.h` | Native HUD is rendered once to a composition quad; reticle and per-eye weapon-selection duplication are suppressed. |
| Dash launcher VR dialog and launch logic | `launcher/OptionsVrDlg.*`, display/options dialogs, resources, `LauncherApp.cpp` | The Alpine Faction VR Edition launcher enables VR by default for new configurations and exposes persistent VR/turn settings in its compact options UI; enabling VR locks D3D11 and launch adds `-vr`. |
| Dash launcher command-line diagnostic | `launcher_common/PatchedAppLauncher.cpp` | The effective Alpine RF command line is logged before injection. |
| Dash developer launch script | `tools/run-afvr.ps1` | Converted into a strictly non-launching prerequisite, runtime, command-preview, and SHA-256 validator. |

## Hook ownership and collision audit

| RF 1.20 NA seam | Prototype purpose | Alpine result |
| ---: | --- | --- |
| `0x0043D4F0` | `control_config_check_pressed` VR actions | **Collision found.** Alpine already owns this with a composable `FunHook`; AFVR registers a `ControlInputInjection` and installs no second hook. |
| `0x00431FF8` | Portal/world traversal | Free; AFVR owns the new stereo call hook. |
| `0x004D35FD` | Room-object frustum cull | Free; AFVR makes only active stereo traversals conservative. |
| `0x00432A18` | Gameplay HUD call | Free; AFVR captures the native HUD once. |
| `0x004A615D` | Local player controls | Free; AFVR layers movement/turn input after the original call. |
| `0x004AB1A0`, `0x004ABBC8`, `0x004ABD89` | FPGUN body/mesh/silencer | Free; AFVR keeps weapon geometry, filters arm/hand materials, and blocks the Glock silencer attachment. |
| `0x0051E450`, `0x0051E530`-`0x0051E600` | Mouse position/button queries | Free; AFVR gates only active VR capture. |
| `0x00425830`, `0x004C77A0` | Common shot and projectile creation | Free; local singleplayer pose overrides are applied at final shared seams. |
| `0x0042A1D0`, `0x0042A547` | Shell/debris creation | Free; integrated beside Alpine's existing entity patches. |

All other address matches found during the audit were mapped RF function pointers, profiler ranges, or adjacent Alpine
patches rather than competing detours. Addresses remain specific to `RF_120na.exe`; a live HMD/game pass is required
to validate binary behavior and visual correctness.

## Complete Dash working-tree inventory disposition

- `common/include/common/config/GameConfig.h` and `common/src/config/GameConfig.cpp`: adapted to Alpine's registry key and current config visitor.
- `game_patch/graphics/d3d11/gr_d3d11.cpp`, `.h`, `_hooks.cpp`, `_mesh.cpp`, and `_transform.h`: behavior merged into Alpine counterparts. The prototype's `_context.h` change was not copied because current Alpine already provides both `invalidate_mode()` and broader cached-state invalidation.
- `game_patch/hud/hud_internal.h`, `hud_weapons.cpp`, and `weapon_select.cpp`: native Alpine HUD/weapon-wheel behavior retained with VR capture gates.
- `game_patch/input/mouse.cpp`, `main/main.cpp`, `misc/player.cpp`, `misc/player_fpgun.cpp`, `object/entity.cpp`, `rf/gr/gr.h`, and `rf/input.h`: each VR delta was adapted at its current Alpine seam.
- `game_patch/misc/main_menu.cpp`: the Dash worktree reports only a line-ending change, so there is no semantic VR change to port.
- `game_patch/vr/openxr_context.cpp/.h`, `vr.cpp/.h`, and `vr_render_bridge.cpp/.h`: ported and Alpine-renamed, then integrated with the newer renderer/input systems.
- `launcher/CMakeLists.txt`, launcher resource script/header, `LauncherApp.cpp`, `OptionsDisplayDlg.cpp/.h`, and `OptionsDlg.cpp/.h`: adapted to Alpine's smaller current dialog and FactionFiles controls.
- `launcher/res/header.bmp`: intentionally not ported; it is Dash branding and Alpine's current artwork is preserved.
- `launcher_common/PatchedAppLauncher.cpp`: diagnostic adapted to `AlpineFaction.dll` and `[AFVR]`.
- Untracked `launcher/OptionsVrDlg.cpp/.h` and `tools/run-dfvr.ps1`: ported as Alpine-named files. The Dash-local `AGENTS.md` is an instruction file, not product source, and was not copied.
- Prototype `docs/VR_ARCHITECTURE.md`: used as architectural reference; this document supersedes its Dash-specific ownership/mapping claims for the Alpine port.

## Unsupported mode policy

The launcher rejects `-vr` combined with `-dedicated`. If multiplayer is entered after an OpenXR session starts, AFVR
logs an actionable warning, destroys its OpenXR context, restores the ordinary frame limiter, and continues in flat mode.
