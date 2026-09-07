#include "sound_volume.h"
#include "../client_signature.h"
#include "../messaging/messaging.h"
#include "../hooks/map_load.h"
#include "../hooks/tick.h"
#include "../halo_data/script.h"
#include <cstdio>
#include <windows.h>

// execute_script se llama en tick 1 de cada mapa — nunca en on_map_load_event
// porque el motor de scripts no está inicializado en ese momento y crashea.

static bool  ambient_active  = false;
static float ambient_gain    = 1.0f;
static bool  weapon_active   = false;
static float weapon_gain     = 1.0f;
static bool  steps_active    = false;
static float steps_gain      = 1.0f;
static bool  pending_apply   = false;
static bool  steps_pending   = false;
static bool  tick_registered = false;

static void apply_gains() noexcept {
    char script[64];
    if(ambient_active) {
        snprintf(script, sizeof(script), "sound_class_set_gain ambient %.4f 0", ambient_gain);
        execute_script(script);
        snprintf(script, sizeof(script), "sound_class_set_gain device_nature %.4f 0", ambient_gain);
        execute_script(script);
        snprintf(script, sizeof(script), "sound_class_set_gain device_machinery %.4f 0", ambient_gain);
        execute_script(script);
    }
    if(weapon_active) {
        snprintf(script, sizeof(script), "sound_class_set_gain weapon_fire %.4f 0", weapon_gain);
        execute_script(script);
    }
}

static void apply_steps_gain() noexcept {
    char script[64];
    snprintf(script, sizeof(script), "sound_class_set_gain unit_footsteps %.4f 0", steps_gain);
    execute_script(script);
    snprintf(script, sizeof(script), "sound_class_set_gain object_impacts %.4f 0", steps_gain);
    execute_script(script);
}

static void on_tick_sound() noexcept {
    auto tc = tick_count();
    if(tc == 1 && pending_apply) {
        apply_gains();
        pending_apply = false;
    }
    if(tc == 1 && steps_pending) {
        if(steps_active) apply_steps_gain();
        steps_pending = false;
    }
}

static void on_map_load_sound() noexcept {
    pending_apply = true;
    if(steps_active) steps_pending = true;
}

static void ensure_registered() noexcept {
    if(!tick_registered) {
        add_tick_event(on_tick_sound);
        add_map_load_event(on_map_load_sound);
        tick_registered = true;
    }
}

static void maybe_unregister() noexcept {
    if(tick_registered && !ambient_active && !weapon_active && !steps_active) {
        remove_tick_event(on_tick_sound);
        remove_map_load_event(on_map_load_sound);
        tick_registered = false;
    }
}

ChimeraCommandError ambient_volume_command(size_t argc, const char **argv) noexcept {
    if(argc == 1) {
        std::string arg = argv[0];
        if(arg == "false" || arg == "off" || arg == "0") {
            if(ambient_active) {
                execute_script("sound_class_set_gain ambient 1.0 0");
                execute_script("sound_class_set_gain device_nature 1.0 0");
                execute_script("sound_class_set_gain device_machinery 1.0 0");
                ambient_active = false;
                maybe_unregister();
            }
            console_out("chimera_ambient_volume: off");
            return CHIMERA_COMMAND_ERROR_SUCCESS;
        }
        float new_gain = strtof(argv[0], nullptr);
        if(new_gain < 0.0f || new_gain > 1.0f) {
            console_out_error("chimera_ambient_volume: value must be between 0.0 and 1.0.");
            return CHIMERA_COMMAND_ERROR_FAILURE;
        }
        ambient_gain   = new_gain;
        ambient_active = true;
        ensure_registered();
        if(tick_count() > 1) {
            char script[64];
            snprintf(script, sizeof(script), "sound_class_set_gain ambient %.4f 0", ambient_gain);
            execute_script(script);
            snprintf(script, sizeof(script), "sound_class_set_gain device_nature %.4f 0", ambient_gain);
            execute_script(script);
            snprintf(script, sizeof(script), "sound_class_set_gain device_machinery %.4f 0", ambient_gain);
            execute_script(script);
        }
        else pending_apply = true; // entered at tick 0/1: defer to on_tick_sound
    }
    if(ambient_active)
        console_out("chimera_ambient_volume: " + std::to_string(ambient_gain));
    else
        console_out("chimera_ambient_volume: off");
    return CHIMERA_COMMAND_ERROR_SUCCESS;
}

ChimeraCommandError weapon_volume_command(size_t argc, const char **argv) noexcept {
    if(argc == 1) {
        std::string arg = argv[0];
        if(arg == "false" || arg == "off" || arg == "0") {
            if(weapon_active) {
                execute_script("sound_class_set_gain weapon_fire 1.0 0");
                weapon_active = false;
                maybe_unregister();
            }
            console_out("chimera_weapon_volume: off");
            return CHIMERA_COMMAND_ERROR_SUCCESS;
        }
        float new_gain = strtof(argv[0], nullptr);
        if(new_gain < 0.0f || new_gain > 1.0f) {
            console_out_error("chimera_weapon_volume: value must be between 0.0 and 1.0.");
            return CHIMERA_COMMAND_ERROR_FAILURE;
        }
        weapon_gain   = new_gain;
        weapon_active = true;
        ensure_registered();
        if(tick_count() > 1) {
            char script[64];
            snprintf(script, sizeof(script), "sound_class_set_gain weapon_fire %.4f 0", weapon_gain);
            execute_script(script);
        }
        else pending_apply = true; // entered at tick 0/1: defer to on_tick_sound
    }
    if(weapon_active)
        console_out("chimera_weapon_volume: " + std::to_string(weapon_gain));
    else
        console_out("chimera_weapon_volume: off");
    return CHIMERA_COMMAND_ERROR_SUCCESS;
}

ChimeraCommandError steps_volume_command(size_t argc, const char **argv) noexcept {
    if(argc == 1) {
        std::string arg = argv[0];
        if(arg == "false" || arg == "off" || arg == "0") {
            if(steps_active) {
                execute_script("sound_class_set_gain unit_footsteps 1.0 0");
                execute_script("sound_class_set_gain object_impacts 1.0 0");
                steps_active = false;
                maybe_unregister();
            }
            console_out("chimera_steps_volume: off");
            return CHIMERA_COMMAND_ERROR_SUCCESS;
        }
        float new_gain = strtof(argv[0], nullptr);
        if(new_gain < 0.0f) {
            console_out_error("chimera_steps_volume: value must be >= 0.0 (max 2.0 recommended).");
            return CHIMERA_COMMAND_ERROR_FAILURE;
        }
        steps_gain   = new_gain;
        steps_active = true;
        ensure_registered();
        if(tick_count() > 1) apply_steps_gain();
        else steps_pending = true;
    }
    if(steps_active)
        console_out("chimera_steps_volume: " + std::to_string(steps_gain));
    else
        console_out("chimera_steps_volume: off");
    return CHIMERA_COMMAND_ERROR_SUCCESS;
}