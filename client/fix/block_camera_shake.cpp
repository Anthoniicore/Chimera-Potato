#include "block_camera_shake_fix.h"

#include "../messaging/messaging.h"
#include "../client_signature.h"

static void block_camera_shake(bool block)
{
  auto &block_shake_sig = get_signature("block_camera_shake_sig");
  if (block)
  {
    const unsigned char mod[] = { 0x90, 0xE9, 0xD1, 0x00, 0x00, 0x00, 0x8B, 0x15 };
    write_code_s(block_shake_sig.address(), mod);
  }
  else
  {
    const unsigned char mod[] = { 0x0F, 0x85, 0xD1, 0x00, 0x00, 0x00, 0x8B, 0x15 };
    write_code_s(block_shake_sig.address(), mod);
  }
}

ChimeraCommandError bcsf_command(size_t argc, const char **argv) noexcept
{
  static auto active = false;
  if (argc == 1)
  {
    bool new_value = bool_value(argv[0]);
    if (new_value != active)
    {
      if (new_value)
      {
        block_camera_shake(true);
        console_out("1");
      }
      else
      {
        block_camera_shake(false);
        console_out("0");
      }
      active = new_value;
    }
  }
  console_out(active ? "Camera shake disabled" : "Camera shake enabled");
  return CHIMERA_COMMAND_ERROR_SUCCESS;
}
