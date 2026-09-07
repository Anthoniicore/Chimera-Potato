#pragma once
#include "../command/command.h"

/// chimera_chat_fix [true/false]
/// Fixes the "text lag" bug where chat messages disappear after ALT+TAB.
/// When the game regains focus, restores fade timers of visible chat entries.
ChimeraCommandError chat_fix_command(size_t argc, const char **argv) noexcept;
