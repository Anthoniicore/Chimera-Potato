#include "block_mouse_acceleration.h"
#include "../client_signature.h"
#include "../messaging/messaging.h"

static bool block_mouse_acceleration = false;

ChimeraCommandError block_mouse_acceleration_command(size_t argc, const char **argv) noexcept {
  if(argc == 1)
  {
    bool new_value = bool_value(argv[0]);
    if(new_value != block_mouse_acceleration)
    {
      auto &mouse_accel_1_sig = get_signature("mouse_accel_1_sig");
      auto &mouse_accel_2_sig = get_signature("mouse_accel_2_sig");
      if(new_value)
      {
        static float zero = 0;
        write_code_any_value(mouse_accel_1_sig.address(), static_cast<unsigned char>(0xEB));
        write_code_any_value(mouse_accel_2_sig.address() + 1, static_cast<unsigned char>(0x05));
        write_code_any_value(mouse_accel_2_sig.address() + 2, &zero);
        write_code_any_value(mouse_accel_2_sig.address() + 6 + 1, static_cast<unsigned char>(0x25));
        write_code_any_value(mouse_accel_2_sig.address() + 6 + 2, &zero);
      }
      else
      {
        mouse_accel_1_sig.undo();
        mouse_accel_2_sig.undo();
      }
      block_mouse_acceleration = new_value;
    }
  }
  console_out(block_mouse_acceleration ? "true" : "false");
  return CHIMERA_COMMAND_ERROR_SUCCESS;
}

bool is_block_mouse_acceleration_enabled() noexcept {
  return block_mouse_acceleration;
}