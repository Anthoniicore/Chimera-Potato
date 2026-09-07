# Chimera 777 — review, fixes & feature work

This is the `chimera 777` fork after (1) a full correctness / memory-safety / build review and (2) targeted
feature work (overshield glow, active-camo assessment, and first-person aim feel). The whole tree **compiles
cleanly** with 32-bit MinGW (g++ 10.5.0, `i686-w64-mingw32`), both debug and release.

Build artifacts (`bin/`, `*.o`, `*.a`, `*.dll`) were stripped — rebuild with `build.bat` (or `build.bat release`).

---

## Feature work (this round)

### `interpolation/camera.cpp` — crisp 1:1 first-person aim (less floaty/laggy)
**Problem:** the camera interpolation lerped **both position and orientation** between the previous and current
tick, so the rendered view (your aim) was always ~1 tick (~33 ms) behind the live input — it felt floaty/delayed.

**Fix:** in **first person** we now interpolate only the camera **position** (movement stays smooth) and use the
**live current-tick orientation** for the aim — removing the ~1-tick aim latency. Vehicle/cinematic/third-person
cameras are still fully interpolated. Trade-off: the view angle now updates at the 30 Hz tick rate (crisp, not
delayed) instead of being smoothed; this is the standard competitive-aim configuration.

Reversible via the `fp_crisp_aim` flag near the top of `camera.cpp` (set to `false` for the old interpolated-aim
behavior). **For even lower input latency, also run (or add to `chimerainit.txt`):**
`chimera_disable_buffering true` and `chimera_block_mouse_acceleration true`.

### `overshield_glow.cpp` — rewritten so the shield glows like a hit while overshield is up
**Goal:** when a player has overshield, make the shield shimmer the way it does when it *takes damage*, and stop
when the overshield runs out.

**Verified mechanism (cross-checked vs. SnowyMouse/chimera, aLTis94/Halo_CE offsets.lua, c20.reclaimers):** the
engine renders the shield-impact shader while `current_shield_damage` (object **+0xE8**) / `shields_hit`
(**+0x124**) are `> 0`, and auto-decays them each tick. So we re-arm those fields every pretick while
`shield > max_shields`; when overshield ends we stop and the engine fades the glow out. These are **render-only**
fields — they don't change real shield health or recharge (recharge is gated by `+0x104`).

**Changes vs. the old version:** applies to **all bipeds** with overshield (visible on yourself in
third-person/vehicles and on other players in MP); uses a strong, tunable `GLOW_INTENSITY` (default `1.0f`)
instead of the old subtle `0.1f`; removed the redundant `was_overshield` state and the unnecessary per-tick
`VirtualProtect`.

### `camo_fix.cpp` — honest assessment of "liquid camo with alpha render target disabled"
**Not achievable as a memory/code patch — I did not fake a fix.** The "liquid" look is screen-space refraction,
which needs an **alpha render target** (render-to-texture). With it disabled, the engine can only do a flat alpha
blend; no offset/memory write can recreate the distortion (that needs a custom D3D9 hook + shader Chimera doesn't
expose). Also documented in-code: `dart_fix()` currently writes `0.9f`, which is the **same value already present**
(`0x3F666666`), so as written it's a **no-op**. To get the real effect, don't disable the alpha render target.

---

## Earlier review fixes (still included)

**Memory safety:** `table.cpp` `object_data` `>`→`>=` + `unichar_equal` over-read/false-match; `keystone.cpp`
switch-break over-read; `offset_hud_elements.cpp` `objects[65535]`→`[65536]` + reordered `is_valid()`;
`codefinder.cpp` `boyerFind`/`findCode` OOB reads; `lua.cpp` `check_for_it` `size_t` underflow; `server_ip.cpp`
unbounded `swprintf` → `%.200s`.
**UB:** `lua_io.cpp` `1<<bit`→`1u<<bit`.
**Logic:** `lua_game.cpp` `stop_timer` inversion + `spawn_object` stack index; `lua_callback.cpp` dead priority
branch; `settings.cpp` blank message + swapped arg-count wording; `sound_volume.cpp` tick-1 drop;
`mouse_sensitivity.cpp` dead shadow flag; `data_types.cpp` dominant-axis argmax.
**Hygiene:** `map_load.h`→`.cpp` static footgun; removed `first_tick`/`enable_camera_shake_fix`/
`camera_shake_fix_enabled`; `console.cpp` VirtualProtect range; `client.cpp` help newline.
**`movement_predict.cpp`** — rewritten (wrong event phase, interpolation feedback, dead correction).

## Needs the author / in-game testing (flagged in code, not patched)
- `magnetism_fix.cpp` — reuses `movement_info_sig`, whose operand is `MovementInfo.move_up` (player input), not a
  magnetism scalar → currently a no-op. Needs a dedicated magnetism signature.
- `mouse_delta_accumulator.cpp` — shares the same patched operand as `mouse_sensitivity` (int-vs-float conflict).
  Don't use both at once until the real delta accumulator is located by disassembly.
- `chat_fix.cpp` — hardcoded absolute address instead of a signature; should be re-anchored.

## Build verified
- Toolchain: WinLibs MinGW **g++ 10.5.0** (`i686-w64-mingw32`, MSVCRT).
- `build.bat` (debug) and `build.bat release` both produce `bin/chimera.dll` with **no errors**. The only warnings
  are pre-existing and in files not touched here.
