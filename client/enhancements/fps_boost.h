#pragma once

#include "../command/command.h"

/// Initializes optional renderer signatures and the map-load hook used by the
/// FPS profiles.
void initialize_fps_boost() noexcept;

/// FPS profiles:
///   0 = off
///   1 = safe: AF off + firing particles off + multitexture overlays off; normal zoom blur is preserved
///   2 = aggressive: level 1 + strip BSP detail/bump textures + lowest lightmap detail
///   3 = ultra: level 2 + strip BSP base textures + remove water textures/brightness
///
/// Active Camo is protected: transparent shader classes and the alpha render
/// target are never modified by this module.
ChimeraCommandError fps_boost_command(size_t argc, const char **argv) noexcept;
