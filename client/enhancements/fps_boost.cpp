#include "fps_boost.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <ctype.h>

#include "firing_particle.h"
#include "zoom_blur.h"
#include "../client_signature.h"
#include "../hooks/map_load.h"
#include "../halo_data/tag_data.h"
#include "../halo_data/tiarace/hce_tag_class_int.h"
#include "../messaging/messaging.h"
#include "../visuals/anisotropic_filtering.h"

// =============================================================================
// Chimera Potato (improved)
// =============================================================================
// Goal: give the biggest possible FPS boost on low-end hardware while keeping
// Active Camo 100% intact.
//
// Design rules (never break these):
//   1. NEVER touch transparent shader tags (TAG_CLASS_INT_SHADER_TRANSPARENT_*,
//      especially chicago / chicago_extended). Active Camo depends on them and
//      on the alpha render target.
//   2. Never disable the alpha render target.
//   3. Player / weapon base maps (shader_model) stay intact so characters still
//      look recognizable.
//   4. Only visual/tag data is modified. No gameplay, magnetism, hitreg, or
//      object table changes.
//
// The tag edits only null the last 4 bytes of a TagDependency (the datum index)
// and save the original value so everything can be restored cleanly.
// =============================================================================

namespace {
    constexpr uint32_t NULL_TAG_ID = 0xFFFFFFFFu;

    // Shader::detail_level (uint16) at offset 0x02 of every shader tag.
    constexpr size_t SHADER_DETAIL_LEVEL_OFFSET = 0x02;

    // shader_environment (senv)
    constexpr size_t SENV_BASE_MAP_OFFSET         = 0x88;
    constexpr size_t SENV_PRIMARY_DETAIL_OFFSET   = 0xB8;
    constexpr size_t SENV_SECONDARY_DETAIL_OFFSET = 0xCC;
    constexpr size_t SENV_MICRO_DETAIL_OFFSET     = 0xFC;
    constexpr size_t SENV_BUMP_MAP_OFFSET         = 0x128;

    // shader_model (soso)
    constexpr size_t SOSO_DETAIL_MAP_OFFSET = 0xEC;

    // shader_transparent_water (swat) - only water is allowed to disappear
    constexpr size_t SWAT_BASE_MAP_OFFSET                 = 0x4C;
    constexpr size_t SWAT_REFLECTION_MAP_OFFSET           = 0x7C;
    constexpr size_t SWAT_RIPPLE_MAP_OFFSET               = 0x9C;
    constexpr size_t SWAT_PERPENDICULAR_BRIGHTNESS_OFFSET = 0x5C;
    constexpr size_t SWAT_PARALLEL_BRIGHTNESS_OFFSET      = 0x6C;

    struct DependencyPatch {
        uint32_t *datum;
        uint32_t old_value;
    };

    struct U16Patch {
        uint16_t *value;
        uint16_t old_value;
    };

    struct FloatPatch {
        float *value;
        float old_value;
    };

    std::vector<DependencyPatch> dependency_patches;
    std::vector<U16Patch> detail_level_patches;
    std::vector<FloatPatch> water_float_patches;

    bool optional_zoom_blur = false;
    bool optional_multitexture_overlay = false;
    int active_level = 0;   // 0 = off, 1 = low, 2 = medium, 3 = high, 4 = ultra

    void patch_dependency(char *tag_data, size_t offset) noexcept {
        auto *datum = reinterpret_cast<uint32_t *>(tag_data + offset + 12);
        if (*datum == NULL_TAG_ID) return;
        dependency_patches.push_back({datum, *datum});
        *datum = NULL_TAG_ID;
    }

    void patch_detail_level(char *tag_data) noexcept {
        auto *value = reinterpret_cast<uint16_t *>(tag_data + SHADER_DETAIL_LEVEL_OFFSET);
        // 3 = "turd" (lowest lightmap / shader detail)
        if (*value == 3) return;
        detail_level_patches.push_back({value, *value});
        *value = 3;
    }

    void patch_water_float(char *tag_data, size_t offset, float new_value) noexcept {
        auto *value = reinterpret_cast<float *>(tag_data + offset);
        water_float_patches.push_back({value, *value});
        *value = new_value;
    }

    void restore_tag_patches() noexcept {
        for (auto it = dependency_patches.rbegin(); it != dependency_patches.rend(); ++it)
            *it->datum = it->old_value;
        for (auto it = detail_level_patches.rbegin(); it != detail_level_patches.rend(); ++it)
            *it->value = it->old_value;
        for (auto it = water_float_patches.rbegin(); it != water_float_patches.rend(); ++it)
            *it->value = it->old_value;

        dependency_patches.clear();
        detail_level_patches.clear();
        water_float_patches.clear();
    }

    void apply_tag_profile(int level) noexcept {
        if (level < 2) return;

        auto *tags = *reinterpret_cast<HaloTag **>(0x40440000);
        auto count = *reinterpret_cast<uint32_t *>(0x4044000C);

        if (!tags || count == 0 || count > 65535) return;

        for (uint32_t i = 0; i < count; ++i) {
            auto &tag = tags[i];
            if (!tag.data) continue;

            switch (tag.tag_class) {
                case HaloCE::TAG_CLASS_INT_SHADER_ENVIRONMENT:
                    // Medium+: remove expensive material stack from BSP
                    patch_detail_level(tag.data);
                    patch_dependency(tag.data, SENV_PRIMARY_DETAIL_OFFSET);
                    patch_dependency(tag.data, SENV_SECONDARY_DETAIL_OFFSET);
                    patch_dependency(tag.data, SENV_MICRO_DETAIL_OFFSET);
                    patch_dependency(tag.data, SENV_BUMP_MAP_OFFSET);

                    if (level >= 3) {
                        // High/Ultra: also remove the base texture of the world
                        patch_dependency(tag.data, SENV_BASE_MAP_OFFSET);
                    }
                    break;

                case HaloCE::TAG_CLASS_INT_SHADER_MODEL:
                    // Always keep base maps of players/weapons so they stay readable.
                    // Only strip detail maps + force lowest lightmap detail.
                    patch_detail_level(tag.data);
                    patch_dependency(tag.data, SOSO_DETAIL_MAP_OFFSET);
                    break;

                case HaloCE::TAG_CLASS_INT_SHADER_TRANSPARENT_WATER:
                    if (level >= 3) {
                        // Water is allowed to disappear completely.
                        // IMPORTANT: we deliberately do NOT touch any other
                        // transparent class (chicago, chicago_extended, etc.).
                        patch_dependency(tag.data, SWAT_BASE_MAP_OFFSET);
                        patch_dependency(tag.data, SWAT_REFLECTION_MAP_OFFSET);
                        patch_dependency(tag.data, SWAT_RIPPLE_MAP_OFFSET);
                        patch_water_float(tag.data, SWAT_PERPENDICULAR_BRIGHTNESS_OFFSET, 0.0f);
                        patch_water_float(tag.data, SWAT_PARALLEL_BRIGHTNESS_OFFSET, 0.0f);
                    }
                    break;

                default:
                    // Everything else (especially all transparent shaders used
                    // by Active Camo and visual effects) is left completely alone.
                    break;
            }
        }
    }

    void on_map_load() noexcept {
        // Map load replaces the entire tag array → old pointers are invalid.
        dependency_patches.clear();
        detail_level_patches.clear();
        water_float_patches.clear();

        apply_tag_profile(active_level);
    }

    void set_af(bool enabled) noexcept {
        // Force anisotropic filtering off for every potato level > 0
        auto &setting = **reinterpret_cast<char **>(get_signature("af_is_enabled_sig").address() + 1);
        setting = enabled ? 1 : 0;
    }

    void set_multitexture_overlay(bool disable) noexcept {
        if (!optional_multitexture_overlay) return;
        auto &sig = get_signature("multitexture_overlay_sig");
        if (disable) {
            const short mod[] = {-1, -1, 0x60};
            write_code_s(sig.address(), mod);
        } else {
            sig.undo();
        }
    }

    void set_zoom_blur(bool disable) noexcept {
        if (!optional_zoom_blur) return;
        const char *arg[] = {disable ? "true" : "false"};
        block_zoom_blur_command(1, arg);
    }

    // Convert classic potato names → numeric level
    int parse_level(const char *arg) noexcept {
        if (!arg) return -1;

        // Numeric first
        char *end = nullptr;
        long n = strtol(arg, &end, 10);
        if (end != arg && *end == '\0' && n >= 0 && n <= 4)
            return static_cast<int>(n);

        // Classic names (case-insensitive)
        char buf[16] = {};
        size_t i = 0;
        for (; arg[i] && i < sizeof(buf) - 1; ++i)
            buf[i] = static_cast<char>(tolower(static_cast<unsigned char>(arg[i])));
        buf[i] = '\0';

        if (strcmp(buf, "off") == 0 || strcmp(buf, "0") == 0) return 0;
        if (strcmp(buf, "low") == 0 || strcmp(buf, "1") == 0) return 1;
        if (strcmp(buf, "medium") == 0 || strcmp(buf, "med") == 0 || strcmp(buf, "2") == 0) return 2;
        if (strcmp(buf, "high") == 0 || strcmp(buf, "3") == 0) return 3;
        if (strcmp(buf, "ultra") == 0 || strcmp(buf, "max") == 0 || strcmp(buf, "4") == 0) return 4;

        return -1;
    }

    void apply_level(int level) noexcept {
        if (level < 0) level = 0;
        if (level > 4) level = 4;

        if (level == 0) {
            restore_tag_patches();
            set_af(true);                     // restore AF to whatever the user had
            set_multitexture_overlay(false);
            set_zoom_blur(false);
            const char *arg[] = {"false"};
            block_firing_particles_command(1, arg);
        } else {
            // All potato levels force these off
            set_af(false);
            set_multitexture_overlay(true);
            set_zoom_blur(false);             // keep normal zoom blur (user preference)
            const char *arg[] = {"true"};
            block_firing_particles_command(1, arg);

            restore_tag_patches();
            apply_tag_profile(level);
        }

        active_level = level;
    }

    const char *level_name(int level) noexcept {
        switch (level) {
            case 0: return "off";
            case 1: return "low";
            case 2: return "medium";
            case 3: return "high";
            case 4: return "ultra";
            default: return "?";
        }
    }
}

void initialize_fps_boost() noexcept {
    // Optional signatures – failure must never prevent Chimera from loading.
    optional_multitexture_overlay = find_multitexture_overlay_signature();
    optional_zoom_blur = find_zoom_blur_signatures();
    add_map_load_event(on_map_load, EVENT_PRIORITY_AFTER);
}

static ChimeraCommandError potato_impl(size_t argc, const char **argv) noexcept {
    if (argc == 1) {
        int level = parse_level(argv[0]);
        if (level < 0) {
            console_out_warning("Invalid potato level. Use: off, low, medium, high, ultra  (or 0-4)");
            char current[32] = {};
            sprintf(current, "Current: %s (%d)", level_name(active_level), active_level);
            console_out(current);
            return CHIMERA_COMMAND_ERROR_FAILURE;
        }
        apply_level(level);
    }

    char current[32] = {};
    sprintf(current, "%s (%d)", level_name(active_level), active_level);
    console_out(current);
    return CHIMERA_COMMAND_ERROR_SUCCESS;
}

ChimeraCommandError fps_boost_command(size_t argc, const char **argv) noexcept {
    return potato_impl(argc, argv);
}

ChimeraCommandError potato_command(size_t argc, const char **argv) noexcept {
    return potato_impl(argc, argv);
}
