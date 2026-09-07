#pragma once
#include "../command/command.h"

/// chimera_movement_predict [strength]
/// Extrapolates object movement between server ticks (using engine velocity) to
/// reduce the "teleport" effect under lag. Works best with chimera_interpolate on.
/// strength: 0.0-1.0, how much prediction to apply (default: 0.5)
/// Recommended starting point: chimera_movement_predict 0.5
ChimeraCommandError movement_predict_command(size_t argc, const char **argv) noexcept;
