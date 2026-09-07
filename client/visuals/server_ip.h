#pragma once

#include <string>

#include "../command/command.h"

/// Function for command chimera_block_server_ip
ChimeraCommandError block_server_ip_command(size_t argc, const char **argv) noexcept;
ChimeraCommandError block_server_ip_command2() noexcept;

/// Sets the displayed IP address.
///
/// If @a only_address is true, @a string replaces only the IP address portion.
/// Otherwise, @a string replaces the entire line (including the prefix "Server IP Address - ").
///
/// @a string should NOT contain the character '%'.
void set_server_ip_string(std::string string, bool only_address = false);
