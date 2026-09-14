#pragma once

#include "../command/command.h"

/// Initializes optional renderer signatures and the map-load hook used by the
/// potato / FPS profiles.
void initialize_fps_boost() noexcept;

/// Chimera Potato (improved, camo-safe)
///
/// Levels (both numeric and classic names accepted):
///   0 / off      - Restore everything
///   1 / low      - AF off + firing particles off + multitexture overlays off
///   2 / medium   - low + strip BSP detail/bump maps + lowest lightmap detail
///   3 / high    - medium + strip BSP base maps + remove water
///   4 / ultra    - high + extra model detail stripping (still keeps player/weapon base maps)
///
/// CRITICAL: Never touches transparent shader tags (chicago / chicago_extended),
/// nor the alpha render target. Active Camo stays fully functional.
ChimeraCommandError fps_boost_command(size_t argc, const char **argv) noexcept;

/// Alias for the classic name people remember.
ChimeraCommandError potato_command(size_t argc, const char **argv) noexcept;
