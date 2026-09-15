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
// Chimera Potato (aggressive, camo-safe)
// =============================================================================
// Goal: maximum FPS on potato hardware while NEVER breaking Active Camo.
//
// Rules:
//   1. NEVER touch transparent chicago / chicago_extended / plasma tags.
//   2. NEVER disable the alpha render target.
//   3. Glass reflections/mirrors and sky CAN be killed (not used by camo).
//   4. On ultra we intentionally flatten model/weapon/scenery textures.
// =============================================================================

namespace {
    constexpr uint32_t NULL_TAG_ID = 0xFFFFFFFFu;

    // Common shader header
    constexpr size_t SHADER_DETAIL_LEVEL_OFFSET = 0x02;

    // shader_environment (senv) – runtime offsets used by this fork
    constexpr size_t SENV_BASE_MAP_OFFSET         = 0x88;
    constexpr size_t SENV_PRIMARY_DETAIL_OFFSET   = 0xB8;
    constexpr size_t SENV_SECONDARY_DETAIL_OFFSET = 0xCC;
    constexpr size_t SENV_MICRO_DETAIL_OFFSET     = 0xFC;
    constexpr size_t SENV_BUMP_MAP_OFFSET         = 0x128;
    // Reflection section (cube map + brightness) – kills dynamic mirrors
    constexpr size_t SENV_REFLECTION_FLAGS_OFFSET = 0x2F4;
    constexpr size_t SENV_PERP_BRIGHTNESS_OFFSET  = 0x318;
    constexpr size_t SENV_PARA_BRIGHTNESS_OFFSET  = 0x31C;
    constexpr size_t SENV_REFLECTION_CUBE_OFFSET  = 0x340;

    // shader_model (soso) – characters, weapons, trees, rocks, scenery
    // Detail map offset was already verified in this tree (0xEC).
    // Base / multipurpose derived relative to that known anchor.
    constexpr size_t SOSO_BASE_MAP_OFFSET         = 0xB4;
    constexpr size_t SOSO_MULTIPURPOSE_MAP_OFFSET = 0xCC;
    constexpr size_t SOSO_DETAIL_MAP_OFFSET       = 0xEC;
    constexpr size_t SOSO_PERP_BRIGHTNESS_OFFSET  = 0x18C;
    constexpr size_t SOSO_PARA_BRIGHTNESS_OFFSET  = 0x19C;
    constexpr size_t SOSO_REFLECTION_CUBE_OFFSET  = 0x1AC;

    // shader_transparent_water (swat)
    constexpr size_t SWAT_BASE_MAP_OFFSET                 = 0x4C;
    constexpr size_t SWAT_REFLECTION_MAP_OFFSET           = 0x7C;
    constexpr size_t SWAT_RIPPLE_MAP_OFFSET               = 0x9C;
    constexpr size_t SWAT_PERPENDICULAR_BRIGHTNESS_OFFSET = 0x5C;
    constexpr size_t SWAT_PARALLEL_BRIGHTNESS_OFFSET      = 0x6C;

    // shader_transparent_glass (sgla) – mirrors / windows (NOT active camo)
    constexpr size_t SGLA_REFLECTION_MAP_OFFSET  = 0x70;
    constexpr size_t SGLA_BUMP_MAP_OFFSET         = 0x88;
    constexpr size_t SGLA_DIFFUSE_MAP_OFFSET      = 0xA0;
    constexpr size_t SGLA_DIFFUSE_DETAIL_OFFSET   = 0xB8;
    constexpr size_t SGLA_PERP_BRIGHTNESS_OFFSET  = 0x50;
    constexpr size_t SGLA_PARA_BRIGHTNESS_OFFSET  = 0x60;

    // sky tag – model reference is the expensive draw
    constexpr size_t SKY_MODEL_OFFSET = 0x00;

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
    int active_level = 0;

    void patch_dependency(char *tag_data, size_t offset) noexcept {
        auto *datum = reinterpret_cast<uint32_t *>(tag_data + offset + 12);
        if (*datum == NULL_TAG_ID) return;
        dependency_patches.push_back({datum, *datum});
        *datum = NULL_TAG_ID;
    }

    void patch_detail_level(char *tag_data) noexcept {
        auto *value = reinterpret_cast<uint16_t *>(tag_data + SHADER_DETAIL_LEVEL_OFFSET);
        if (*value == 3) return;
        detail_level_patches.push_back({value, *value});
        *value = 3; // turd
    }

    void patch_float(char *tag_data, size_t offset, float new_value) noexcept {
        auto *value = reinterpret_cast<float *>(tag_data + offset);
        water_float_patches.push_back({value, *value});
        *value = new_value;
    }

    void patch_u16_flags(char *tag_data, size_t offset, uint16_t clear_bits) noexcept {
        auto *value = reinterpret_cast<uint16_t *>(tag_data + offset);
        detail_level_patches.push_back({value, *value});
        *value = static_cast<uint16_t>(*value & ~clear_bits);
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
                    // World / terrain / rocks / grass (BSP)
                    patch_detail_level(tag.data);
                    patch_dependency(tag.data, SENV_PRIMARY_DETAIL_OFFSET);
                    patch_dependency(tag.data, SENV_SECONDARY_DETAIL_OFFSET);
                    patch_dependency(tag.data, SENV_MICRO_DETAIL_OFFSET);
                    patch_dependency(tag.data, SENV_BUMP_MAP_OFFSET);

                    if (level >= 3) {
                        patch_dependency(tag.data, SENV_BASE_MAP_OFFSET);
                        // Kill dynamic mirrors + cube reflections
                        patch_u16_flags(tag.data, SENV_REFLECTION_FLAGS_OFFSET, 0x1); // clear dynamic mirror
                        patch_float(tag.data, SENV_PERP_BRIGHTNESS_OFFSET, 0.0f);
                        patch_float(tag.data, SENV_PARA_BRIGHTNESS_OFFSET, 0.0f);
                        patch_dependency(tag.data, SENV_REFLECTION_CUBE_OFFSET);
                    }
                    break;

                case HaloCE::TAG_CLASS_INT_SHADER_MODEL:
                    // Characters, weapons, trees, rocks, vehicles, scenery props
                    patch_detail_level(tag.data);
                    patch_dependency(tag.data, SOSO_DETAIL_MAP_OFFSET);
                    patch_dependency(tag.data, SOSO_MULTIPURPOSE_MAP_OFFSET);
                    patch_float(tag.data, SOSO_PERP_BRIGHTNESS_OFFSET, 0.0f);
                    patch_float(tag.data, SOSO_PARA_BRIGHTNESS_OFFSET, 0.0f);
                    patch_dependency(tag.data, SOSO_REFLECTION_CUBE_OFFSET);

                    if (level >= 4) {
                        // Ultra: also strip the base diffuse of models
                        // (characters / weapons / trees look flat – intentional)
                        patch_dependency(tag.data, SOSO_BASE_MAP_OFFSET);
                    }
                    break;

                case HaloCE::TAG_CLASS_INT_SHADER_TRANSPARENT_WATER:
                    if (level >= 3) {
                        patch_dependency(tag.data, SWAT_BASE_MAP_OFFSET);
                        patch_dependency(tag.data, SWAT_REFLECTION_MAP_OFFSET);
                        patch_dependency(tag.data, SWAT_RIPPLE_MAP_OFFSET);
                        patch_float(tag.data, SWAT_PERPENDICULAR_BRIGHTNESS_OFFSET, 0.0f);
                        patch_float(tag.data, SWAT_PARALLEL_BRIGHTNESS_OFFSET, 0.0f);
                    }
                    break;

                case HaloCE::TAG_CLASS_INT_SHADER_TRANSPARENT_GLASS:
                    // Windows / mirrors – NOT used by Active Camo
                    if (level >= 3) {
                        patch_float(tag.data, SGLA_PERP_BRIGHTNESS_OFFSET, 0.0f);
                        patch_float(tag.data, SGLA_PARA_BRIGHTNESS_OFFSET, 0.0f);
                        patch_dependency(tag.data, SGLA_REFLECTION_MAP_OFFSET);
                        patch_dependency(tag.data, SGLA_BUMP_MAP_OFFSET);
                        patch_dependency(tag.data, SGLA_DIFFUSE_DETAIL_OFFSET);
                        if (level >= 4) {
                            patch_dependency(tag.data, SGLA_DIFFUSE_MAP_OFFSET);
                        }
                    }
                    break;

                case HaloCE::TAG_CLASS_INT_SKY:
                    // Disable sky model draw (big fillrate win outdoors)
                    if (level >= 3) {
                        patch_dependency(tag.data, SKY_MODEL_OFFSET);
                    }
                    break;

                default:
                    // chicago / chicago_extended / plasma left completely alone
                    // → Active Camo stays intact
                    break;
            }
        }
    }

    void on_map_load() noexcept {
        dependency_patches.clear();
        detail_level_patches.clear();
        water_float_patches.clear();
        apply_tag_profile(active_level);
    }

    void set_af(bool enabled) noexcept {
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

    int parse_level(const char *arg) noexcept {
        if (!arg) return -1;

        char *end = nullptr;
        long n = strtol(arg, &end, 10);
        if (end != arg && *end == '\0' && n >= 0 && n <= 4)
            return static_cast<int>(n);

        char buf[16] = {};
        size_t i = 0;
        for (; arg[i] && i < sizeof(buf) - 1; ++i)
            buf[i] = static_cast<char>(tolower(static_cast<unsigned char>(arg[i])));
        buf[i] = '\0';

        if (strcmp(buf, "off") == 0) return 0;
        if (strcmp(buf, "low") == 0) return 1;
        if (strcmp(buf, "medium") == 0 || strcmp(buf, "med") == 0) return 2;
        if (strcmp(buf, "high") == 0) return 3;
        if (strcmp(buf, "ultra") == 0 || strcmp(buf, "max") == 0) return 4;
        return -1;
    }

    void apply_level(int level) noexcept {
        if (level < 0) level = 0;
        if (level > 4) level = 4;

        if (level == 0) {
            restore_tag_patches();
            set_af(true);
            set_multitexture_overlay(false);
            set_zoom_blur(false);
            const char *arg[] = {"false"};
            block_firing_particles_command(1, arg);
        } else {
            set_af(false);
            set_multitexture_overlay(true);
            set_zoom_blur(false);
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
