#include <string>
#include <iostream>
#include <fstream>

#include "client.h"
#include "../version.h"

#include "client_signature.h"
#include "keystone.h"
#include "path.h"
#include "settings.h"
#include "command/console.h"
#include "command/command.h"

//#include "contributors/contributors.h"

#include "debug/devmode.h"

#include "enhancements/disable_buffering.h"
#include "enhancements/firing_particle.h"
#include "enhancements/block_mouse_acceleration.h"
#include "enhancements/mouse_sensitivity.h"
#include "enhancements/mouse_delta_accumulator.h"
#include "enhancements/sound_volume.h"
#include "enhancements/overshield_glow.h"
#include "enhancements/movement_predict.h"
#include "enhancements/skip_loading.h"
#include "enhancements/throttle_fps.h"
#include "enhancements/zoom_blur.h"
#include "enhancements/fps_boost.h"

#include "fix/camo_fix.h"
#include "fix/descope_fix.h"
#include "fix/fov_fix.h"
#include "fix/magnetism_fix.h"
#include "fix/chat_fix.h"
#include "fix/scope_fix.h"
#include "fix/sniper_hud.h"
#include "fix/widescreen_fix.h"
#include "fix/block_camera_shake_fix.h"

#include "halo_data/chat.h"
#include "halo_data/keyboard.h"
#include "halo_data/map.h"
#include "halo_data/resolution.h"

#include "hooks/camera.h"
#include "hooks/frame.h"
#include "hooks/rcon_message.h"
#include "hooks/tick.h"
#include "hooks/map_load.h"

#include "interpolation/interpolation.h"

#include "lua/lua.h"

#include "messaging/messaging.h"

#include "startup/fast_startup.h"

#include "visuals/anisotropic_filtering.h"
#include "visuals/letterbox.h"
#include "visuals/server_ip.h"
#include "visuals/vertical_field_of_view.h"


#include "visuals/gametype_indicator.h"

extern std::vector<ChimeraSignature> *signatures;
extern std::vector<std::string> *missing_signatures;
std::vector<ChimeraCommand> *commands;

LARGE_INTEGER performance_frequency;

static void init() {
    extern bool already_set;
    auto &enabled = **reinterpret_cast<char **>(get_signature("enable_console_sig").address() + 1);
    already_set = enabled != 0;
    if(!already_set)
        enabled = 1;
    remove_tick_event(init);

    settings_read_only(1);
    char z[512] = {};
    sprintf(z,"%s\\chimera", halo_path());
    CreateDirectory(z, nullptr);
    read_init_file("chimerainit.txt", "chimerainit.txt");
    sprintf(z,"%s\\chimera\\chimerainit.txt", halo_path());
    auto *f = fopen(z, "r");
    if(f)
    {
        fclose(f);
        read_init_file(z, "[-path]/chimerainit.txt");
    }
    else
    {
        std::ofstream init(z);
        init << "###" << std::endl;
        init << "### chimerainit.txt" << std::endl;
        init << "###" << std::endl;
        init << "### This file can be used for configuring Chimera commands that are not saved" << std::endl;
        init << "### automatically such as chimera_set_resolution." << std::endl;
        init << "###" << std::endl;
        init << std::endl;
    }
    settings_read_only(0);

    settings_do_not_save(1);
    sprintf(z,"%s\\chimera\\chimerasave.txt", halo_path());
    read_init_file(z, "[-path]/chimerasave.txt");
    settings_do_not_save(0);

    save_all_changes();
    setup_lua();
}

void initialize_client() noexcept {
    commands = new std::vector<ChimeraCommand>;
    signatures = new std::vector<ChimeraSignature>;
    missing_signatures = new std::vector<std::string>;
    if(!find_required_signatures())
    {
        for(size_t i=0;i<(*missing_signatures).size();i++)
        {
            char message[256] = {};
            sprintf(message, "Could not find %s signature. Make sure you're using Halo Custom Edition version 1.10.", (*missing_signatures)[i].data());
            MessageBox(NULL, message, "Chimera cannot load", MB_OK);
        }
        return;
    }
    initialize_map_load();
    initialize_console();
    initialize_rcon_message();
    add_tick_event(init);
    add_tick_event(camo_fix);
    add_map_load_event(tps_check_svr);
    dart_fix();
    enable_descope_fix();

    QueryPerformanceFrequency(&performance_frequency);

    //add_tick_event(set_contributors);

    initialize_overshield_glow();
    initialize_fps_boost();

     if(find_magnetism_signatures()) {
        fix_magnetism();
    }

    (*commands).emplace_back("chimera", chimera_command, nullptr,
        "This command is the commands directory for Chimera.\n\n"
        "Syntax:\n"
        "  - chimera - Display version and a list of command categories.\n"
        "  - chimera <category> - Display a list of commands in category.\n"
        "  - chimera <command> - Display help for a command."
    , 0, 1, true);

    (*commands).emplace_back("chimera_chat", chat_command, nullptr,
        "Send a chat message.\n\n"
        "Syntax:\n"
        "  - chimera_chat all <message> - Send a message to all players.\n"
        "  - chimera_chat team <message> - Send a message to your team.\n"
        "  - chimera_chat vehicle <message> - Send a message to your vehicle."
    , 1, 1000, true, false);

    (*commands).emplace_back("chimera_reload_lua", reload_lua_command, "lua",
        "Reload all Lua scripts.\n\n"
        "Syntax:\n"
        "  - chimera_reload_lua"
    , 0, 0, true);

    (*commands).emplace_back("chimera_verbose_init", verbose_init_command, nullptr,
        "Get or set whether chimerainit.txt or chimeraname.txt commands should output messages.\n\n"
        "Syntax:\n"
        "  - chimera_verbose_init"
    , 0, 1, true);

    // Debug
    (*commands).emplace_back("chimera_block_mouse_acceleration", block_mouse_acceleration_command, "enhancements",
        "Get or set whether or not to block mouse acceleration. Note that some mice may still\n"
        "exhibit some mouse acceleration.\n\n"
        "Syntax:\n"
        "  - chimera_block_mouse_acceleration [true/false]"
    , 0, 1, find_block_mouse_acceleration_sigs(), true);
    
    (*commands).emplace_back("chimera_mouse_sensitivity", mouse_sensitivity_command, "enhancements",
        "Set the horizontal and vertical mouse sensitivities.\n\n"
        "Values less than 1 do not work properly if mouse acceleration is enabled.\n\n"
        "Syntax:\n"
        "  - chimera_mouse_sensitivity [<vertical> <horizontal>]\n"
        "  - chimera_mouse_sensitivity <false>"
    , 0, 2, find_mouse_sensitivity_sigs(), true);

    (*commands).emplace_back("chimera_mouse_delta_accumulator", mouse_delta_accumulator_command, "enhancements",
        "Accumulates mouse delta across all frames between ticks.\n\n"
        "Syntax:\n"
        "  - chimera_mouse_delta_accumulator [true/false] - Enable or disable."
    , 0, 1, find_mouse_sensitivity_sigs(), true);

    (*commands).emplace_back("chimera_ambient_volume", ambient_volume_command, "enhancements",
        "Controls the volume of ambient/environment sounds.\n\n"
        "Syntax:\n"
        "  - chimera_ambient_volume <0-1>    - Set gain.\n"
        "  - chimera_ambient_volume false    - Disable."
    , 0, 1, true, true);

    (*commands).emplace_back("chimera_weapon_volume", weapon_volume_command, "enhancements",
        "Controls the volume of all weapon fire sounds.\n\n"
        "Syntax:\n"
        "  - chimera_weapon_volume <0-1>     - Set gain.\n"
        "  - chimera_weapon_volume false     - Disable."
    , 0, 1, true, true);

    (*commands).emplace_back("chimera_steps_volume", steps_volume_command, "enhancements",
        "Controls the volume of footstep and landing sounds.\n\n"
        "Values above 1.0 amplify the sound (max 2.0 recommended).\n\n"
        "Syntax:\n"
        "  - chimera_steps_volume <0.0-2.0>  - Set gain.\n"
        "  - chimera_steps_volume false       - Disable."
    , 0, 1, true, true);


    (*commands).emplace_back("chimera_movement_predict", movement_predict_command, "enhancements",
        "Extrapolates object movement between ticks using engine velocity to reduce the\n"
        "teleport effect under lag. Works best with chimera_interpolate enabled.\n\n"
        "Syntax:\n"
        "  - chimera_movement_predict <0-1>  - Set strength (recommended: 0.5).\n"
        "  - chimera_movement_predict false  - Disable."
    , 0, 2, true, true);


    (*commands).emplace_back("chimera_skip_loading", skip_loading_command, "enhancements",
        "Get or set whether or not to skip the multiplayer loading screen.\n\n"
        "Syntax:\n"
        "  - chimera_skip_loading [true/false]"
    , 0, 1, find_loading_screen_signatures(), true);


    // Fixes
    /*(*commands).emplace_back("chimera_aim_assist", aim_assist_command, "fixes",
        "Get or set whether or not fix aim assist for gamepads. This feature is on by default.\n\n"
        "Syntax:\n"
        "  - chimera_aim_assist [true/false]"
    , 0, 1, true, true);*/

    (*commands).emplace_back("chimera_fov_fix", fov_fix_command, "fixes",
        "Get or set whether or not to fix the FOV. This will make FOV mods more accurate.\n\n"
        "Syntax:\n"
        "  - chimera_fov_fix [true/false]"
    , 0, 1, find_fov_fix_sigs() && find_simple_score_screen_sigs(), true);

    (*commands).emplace_back("chimera_sniper_hud_fix", sniper_hud_fix_command, "fixes",
        "Get or set whether or not to fix the sniper HUD. This may not work on protected maps.\n\n"
        "Syntax:\n"
        "  - chimera_sniper_hud_fix [true/false]"
    , 0, 1, true, true);

    (*commands).emplace_back("chimera_widescreen_fix", widescreen_fix_command, "fixes",
        "Get or set whether or not to (mostly) fix the HUD.\n\n"
        "Note that this will break the HUD if you are using any other widescreen fix.\n\n"
        "Settings:\n"
        "  0: Off\n"
        "  1: On\n"
        "  2: On (center HUD)\n\n"
        "Syntax:\n"
        "  - chimera_widescreen_fix [0-2]"
    , 0, 1, find_widescreen_fix_signatures() && find_widescreen_scope_signature(), true);

    (*commands).emplace_back("chimera_widescreen_scope_fix", widescreen_scope_fix_command, "fixes",
        "Enhance an existing widescreen fix by also fixing the scope mask if it's not fixed.\n\n"
        "Syntax:\n"
        "  - chimera_widescreen_scope_fix [true/false]"
    , 0, 1, find_widescreen_scope_signature(), true);

    (*commands).emplace_back("chimera_bcsf", bcsf_command, "fixes",
        "Get or set whether or not the first person camera shake should be blocked.\n\n"
        "Syntax:\n"
        "  - chimera_bcsf [true/false]"
    , 0, 1, find_block_camera_shake_signature(), true);

    (*commands).emplace_back("chimera_chat_fix", chat_fix_command, "fixes",
        "Fixes chat disappearing after ALT+TAB.\n\n"
        "Syntax:\n"
        "  - chimera_chat_fix [true/false]"
    , 0, 1, true, true);

    // Interpolation

    (*commands).emplace_back("chimera_interpolate", interpolate_command, "interpolation",
        "Get or set the interpolation level. Interpolation smoothes out object movement between\n"
        "ticks, providing a substantial visual improvement. Higher levels incur greater CPU usage and\n"
        "may impact framerate on slower CPUs.\n\n"
        "Syntax:\n"
        "  - chimera_interpolate [off/low/medium/high/ultra]"
    , 0, 1, find_interpolation_signatures(), true);

    (*commands).emplace_back("chimera_interpolate_predict", interpolate_predict_command, "interpolation",
        "Get or set whether the next tick should be predicted when interpolating. This will prevent\n"
        "objects from appearing as if they are one tick behind, but sudden object movement may\n"
        "cause jitteriness.\n\n"
        "Settings:\n"
        "  0: Off\n"
        "  1: On\n"
        "  2: On (Do not intepolate the first person camera.)\n"
        "  3: On (Only interpolate the first person camera while in a vehicle.)\n"
        "Syntax:\n"
        "  - chimera_interpolate_predict [0-3]"
    , 0, 1, find_interpolation_signatures(), true);


    // Visuals

    (*commands).emplace_back("chimera_af", af_command, "visuals",
        "Get or set whether or not to enable anisotropic filtering.\n\n"
        "Syntax:\n"
        "  - chimera_af [true/false]"
    , 0, 1, find_anisotropic_filtering_signature(), true);

    // Chimera Potato (main command) – improved, camo-safe FPS mode
    (*commands).emplace_back("chimera_potato", potato_command, "visuals",
        "Aggressive client-side FPS optimization (Chimera Potato mode).\n"
        "Designed for low-end PCs. Never touches Active Camo or transparent shaders.\n\n"
        "Levels:\n"
        "  off / 0     - Restore everything\n"
        "  low / 1     - AF off + firing particles + multitexture overlays off\n"
        "  medium / 2  - low + strip BSP detail/bump + lowest lightmap detail\n"
        "  high / 3    - medium + strip BSP base maps + remove water\n"
        "  ultra / 4   - high + extra model detail stripping (player/weapon base maps kept)\n\n"
        "Syntax:\n"
        "  - chimera_potato [off/low/medium/high/ultra]\n"
        "  - chimera_potato [0-4]"
    , 0, 1, true, true);

    // Alias for convenience
    (*commands).emplace_back("chimera_fps_boost", fps_boost_command, "visuals",
        "Alias of chimera_potato. Same levels and same camo-safe behaviour.\n\n"
        "Syntax:\n"
        "  - chimera_fps_boost [off/low/medium/high/ultra]\n"
        "  - chimera_fps_boost [0-4]"
    , 0, 1, true, true);

    (*commands).emplace_back("chimera_block_firing_particles", block_firing_particles_command, "visuals",
        "Get or set whether or not to block firing particles.\n\n"
        "Syntax:\n"
        "  - chimera_block_firing_particles [true/false]"
    , 0, 1, true, true);

    (*commands).emplace_back("chimera_block_gametype_indicator", block_gametype_indicator_command, "visuals",
        "Get or set whether or not to turn off the gametype indicator.\n\n"
        "\n"
        "Syntax:\n"
        "  - chimera_block_gametype_indicator [true/false]"
    , 0, 1, find_gametype_indicator_sig(), true);

    (*commands).emplace_back("chimera_block_server_ip", block_server_ip_command, "visuals",
        "Get or set whether or not to hide the server IP. This may be useful for streamers.\n\n"
        "Syntax:\n"
        "  - chimera_block_server_ip [true/false]"
    , 0, 1, find_hide_server_ip_sigs(), true);

    (*commands).emplace_back("chimera_block_vsync", block_vsync_command, "visuals",
        "Get or set whether or not to turn vSync off startup.\n\n"
        "\n"
        "Syntax:\n"
        "  - chimera_block_vsync [true/false]"
    , 0, 1, find_set_resolution_signatures(), true);

    (*commands).emplace_back("chimera_set_resolution", set_resolution_command, "visuals",
        "Change Halo's resolution. Width and height can be either resolution in pixels or an aspect\n"
        "ratio.\n\n"
        "Syntax:\n"
        "  - chimera_set_resolution <width> <height> [refresh rate] [vsync] [windowed]"
    , 2, 5, find_set_resolution_signatures(), true);

    (*commands).emplace_back("chimera_throttle_fps", throttle_fps_command, "visuals",
        "Throttle Halo's framerate.\n\n"
        "Syntax:\n"
        "  - chimera_throttle_fps [max FPS]"
    , 0, 1, find_uncap_cinematic_signatures(), true);

    (*commands).emplace_back("chimera_vfov", vfov_command, "visuals",
        "Get or change your FOV by attempting to lock to a specific vertical FOV. This will\n"
        "distort your FOV if HAC2, Open Sauce, etc. are modifying your horizontal FOV. 1\n"
        "defaults to 55.41 degrees, or Halo’s standard FOV.\n\n"
        "Syntax:\n"
        "  - chimera_vfov [VFOV]"
    , 0, 1, find_interpolation_signatures(), true);

    // Startup

    (*commands).emplace_back("chimera_cache", cache_command, "startup",
        "Get or set whether or not to use a cache for fast startup.\n\n"
        "Syntax:\n"
        "  - chimera_cache [true/false]"
    , 0, 1, find_fast_startup_sigs(), true);

    (*commands).emplace_back("chimera_cache_clear", cache_clear_command, "startup",
        "Clear the cache.\n\n"
        "Syntax:\n"
        "  - chimera_cache_clear"
    , 0, 0, find_fast_startup_sigs(), false);

    (*commands).emplace_back("chimera_modded_stock_maps", modded_stock_maps_command, "startup",
        "Get or set whether or not stock maps will use hardcoded CRC32s. This may be required for\n"
        "some maps to work.\n\n"
        "Syntax:\n"
        "  - chimera_modded_stock_maps [true/false]"
    , 0, 1, find_fast_startup_sigs(), true);

    if(find_fast_startup_sigs()) setup_fast_startup();

    if(custom_keystone_in_use())
    {
      if(find_pc_map_compat_sigs()) setup_pc_map_compatibility();
      if(find_keystone_sigs()) setup_keystone_override();
    }
    if(find_console_fade_fix_sig()) setup_console_text_fix();


    add_frame_event(check_keys);
}

void uninitialize_client() noexcept {
    destroy_lua();
    for(size_t i=0;i<signatures->size();i++)
    {
        (*signatures)[i].undo();
    }
    delete signatures;
    signatures = nullptr;
    delete missing_signatures;
    missing_signatures = nullptr;
    delete commands;
    commands = nullptr;
}
