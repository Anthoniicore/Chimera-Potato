#pragma once
#include "../command/command.h"

ChimeraCommandError ambient_volume_command(size_t argc, const char **argv) noexcept;
ChimeraCommandError weapon_volume_command(size_t argc, const char **argv) noexcept;
ChimeraCommandError steps_volume_command(size_t argc, const char **argv) noexcept;