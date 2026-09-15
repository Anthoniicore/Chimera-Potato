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
// Chimera Potato – lower detail to minimum WITHOUT deleting objects
// =============================================================================
// Rules:
//   - NEVER make models / scenery / trees / rocks invisible
//   - NEVER strip bump maps on alpha-tested shaders (trees, grates, foliage)
//   - NEVER force LOD in a way that removes geometry
//   - Only reduce texture layers and reflections for FPS
//   - Active Camo stays 100% intact (no chicago / plasma / alpha RT)
// =============================================================================

namespace {
    constexpr uint32_t NULL_TAG_ID = 0xFFFFFFFFu;

    constexpr size_t SHADER_DETAIL_LEVEL_OFFSET = 0x02;

    // shader_environment – after Shader base (40 bytes)
    constexpr size_t SENV_FLAGS_OFFSET            = 0x28; // bit0 = alpha tested
    constexpr size_t SENV_BASE_MAP_OFFSET         = 0x88;
    constexpr size_t SENV_PRIMARY_DETAIL_OFFSET   = 0xB8;
    constexpr size_t SENV_SECONDARY_DETAIL_OFFSET = 0xCC;
    constexpr size_t SENV_MICRO_DETAIL_OFFSET     = 0xFC;
    constexpr size_t SENV_BUMP_MAP_OFFSET         = 0x128;
    constexpr size_t SENV_REFLECTION_FLAGS_OFFSET = 0x2F4;
    constexpr size_t SENV_PERP_BRIGHTNESS_OFFSET  = 0x318;
    constexpr size_t SENV_PARA_BRIGHTNESS_OFFSET  = 0x31C;
    constexpr size_t SENV_REFLECTION_CUBE_OFFSET  = 0x340;

    // shader_model – always keep BASE map so objects stay visible & colored
    constexpr size_t SOSO_MULTIPURPOSE_MAP_OFFSET = 0xCC;
    constexpr size_t SOSO_DETAIL_MAP_OFFSET       = 0xEC;
    constexpr size_t SOSO_PERP_BRIGHTNESS_OFFSET  = 0x18C;
    constexpr size_t SOSO_PARA_BRIGHTNESS_OFFSET  = 0x19C;
    constexpr size_t SOSO_REFLECTION_CUBE_OFFSET  = 0x1AC;

    // water / glass / sky
    constexpr size_t SWAT_BASE_MAP_OFFSET                 = 0x4C;
    constexpr size_t SWAT_REFLECTION_MAP_OFFSET           = 0x7C;
    constexpr size_t SWAT_RIPPLE_MAP_OFFSET               = 0x9C;
    constexpr size_t SWAT_PERPENDICULAR_BRIGHTNESS_OFFSET = 0x5C;
    constexpr size_t SWAT_PARALLEL_BRIGHTNESS_OFFSET      = 0x6C;

    constexpr size_t SGLA_REFLECTION_MAP_OFFSET  = 0x70;
    constexpr size_t SGLA_BUMP_MAP_OFFSET         = 0x88;
    constexpr size_t SGLA_DIFFUSE_DETAIL_OFFSET   = 0xB8;
    constexpr size_t SGLA_PERP_BRIGHTNESS_OFFSET  = 0x50;
    constexpr size_t SGLA_PARA_BRIGHTNESS_OFFSET  = 0x60;

    // Mild LOD preference (NOT deletion). Only nudges cutoffs down a bit.
    constexpr size_t GBX_SUPER_HIGH_CUTOFF = 0x08;
    constexpr size_t GBX_HIGH_CUTOFF       = 0x0C;
    constexpr size_t GBX_MEDIUM_CUTOFF     = 0x10;
    constexpr size_t GBX_LOW_CUTOFF        = 0x14;

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
    std::vector<U16Patch> u16_patches;
    std::vector<FloatPatch> float_patches;

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
        u16_patches.push_back({value, *value});
        *value = 3; // turd
    }

    void patch_float(char *tag_data, size_t offset, float new_value) noexcept {
        auto *value = reinterpret_cast<float *>(tag_data + offset);
        float_patches.push_back({value, *value});
        *value = new_value;
    }

    void patch_u16_flags(char *tag_data, size_t offset, uint16_t clear_bits) noexcept {
        auto *value = reinterpret_cast<uint16_t *>(tag_data + offset);
        u16_patches.push_back({value, *value});
        *value = static_cast<uint16_t>(*value & ~clear_bits);
    }

    bool is_alpha_tested_senv(char *tag_data) noexcept {
        auto flags = *reinterpret_cast<uint16_t *>(tag_data + SENV_FLAGS_OFFSET);
        return (flags & 0x1) != 0; // alpha tested
    }

    // Prefer lower LODs sooner, but NEVER hide the model (no impossible cutoffs)
    void prefer_lower_model_lod(char *tag_data, int level) noexcept {
        // Scale existing cutoffs down so low/medium LODs kick in earlier.
        // Multiplier: medium=0.5, high=0.35, ultra=0.25 of original thresholds.
        float scale = 1.0f;
        if (level >= 4) scale = 0.25f;
        else if (level >= 3) scale = 0.35f;
        else if (level >= 2) scale = 0.50f;
        else return;

        auto scale_cutoff = [&](size_t offset) {
            auto *v = reinterpret_cast<float *>(tag_data + offset);
            if (*v <= 0.0f) return; // leave zero alone
            float_patches.push_back({v, *v});
            *v = *v * scale;
            if (*v < 1.0f) *v = 1.0f; // keep a tiny positive threshold
        };

        scale_cutoff(GBX_SUPER_HIGH_CUTOFF);
        scale_cutoff(GBX_HIGH_CUTOFF);
        scale_cutoff(GBX_MEDIUM_CUTOFF);
        scale_cutoff(GBX_LOW_CUTOFF);
        // super-low cutoff untouched – model always has a valid mesh
    }

    void restore_tag_patches() noexcept {
        for (auto it = dependency_patches.rbegin(); it != dependency_patches.rend(); ++it)
            *it->datum = it->old_value;
        for (auto it = u16_patches.rbegin(); it != u16_patches.rend(); ++it)
            *it->value = it->old_value;
        for (auto it = float_patches.rbegin(); it != float_patches.rend(); ++it)
            *it->value = it->old_value;

        dependency_patches.clear();
        u16_patches.clear();
        float_patches.clear();
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
                case HaloCE::TAG_CLASS_INT_SHADER_ENVIRONMENT: {
                    const bool alpha_tested = is_alpha_tested_senv(tag.data);

                    patch_detail_level(tag.data);
                    // Detail layers are safe to remove (not used for alpha test)
                    patch_dependency(tag.data, SENV_PRIMARY_DETAIL_OFFSET);
                    patch_dependency(tag.data, SENV_SECONDARY_DETAIL_OFFSET);
                    patch_dependency(tag.data, SENV_MICRO_DETAIL_OFFSET);

                    // Bump map: ONLY strip if NOT alpha-tested
                    // (alpha-tested trees/foliage/grates need bump alpha to stay visible)
                    if (!alpha_tested) {
                        patch_dependency(tag.data, SENV_BUMP_MAP_OFFSET);
                    }

                    if (level >= 3) {
                        // Base map: only strip on solid terrain, never on alpha-tested
                        if (!alpha_tested) {
                            patch_dependency(tag.data, SENV_BASE_MAP_OFFSET);
                        }
                        // Kill mirrors / cube reflections
                        patch_u16_flags(tag.data, SENV_REFLECTION_FLAGS_OFFSET, 0x1);
                        patch_float(tag.data, SENV_PERP_BRIGHTNESS_OFFSET, 0.0f);
                        patch_float(tag.data, SENV_PARA_BRIGHTNESS_OFFSET, 0.0f);
                        patch_dependency(tag.data, SENV_REFLECTION_CUBE_OFFSET);
                    }
                    break;
                }

                case HaloCE::TAG_CLASS_INT_SHADER_MODEL:
                    // Characters, weapons, vehicles, scenery props
                    // ALWAYS keep base map → object stays visible and colored
                    patch_detail_level(tag.data);
                    patch_dependency(tag.data, SOSO_DETAIL_MAP_OFFSET);
                    patch_dependency(tag.data, SOSO_MULTIPURPOSE_MAP_OFFSET);
                    patch_float(tag.data, SOSO_PERP_BRIGHTNESS_OFFSET, 0.0f);
                    patch_float(tag.data, SOSO_PARA_BRIGHTNESS_OFFSET, 0.0f);
                    patch_dependency(tag.data, SOSO_REFLECTION_CUBE_OFFSET);
                    break;

                case HaloCE::TAG_CLASS_INT_GBXMODEL:
                    // Prefer lower LODs earlier – models NEVER disappear
                    prefer_lower_model_lod(tag.data, level);
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
                    // Only reflections – glass body stays so you still see windows
                    if (level >= 3) {
                        patch_float(tag.data, SGLA_PERP_BRIGHTNESS_OFFSET, 0.0f);
                        patch_float(tag.data, SGLA_PARA_BRIGHTNESS_OFFSET, 0.0f);
                        patch_dependency(tag.data, SGLA_REFLECTION_MAP_OFFSET);
                        patch_dependency(tag.data, SGLA_DIFFUSE_DETAIL_OFFSET);
                        // do NOT strip bump/diffuse – keep glass visible
                    }
                    break;

                case HaloCE::TAG_CLASS_INT_SKY:
                    // Soften sky cost without removing the whole sky model on high;
                    // only null model on ultra if user really wants max FPS
                    if (level >= 4) {
                        // Optional: leave sky present so outdoor orientation remains
                        // Comment next line if you want sky gone again on ultra:
                        // patch_dependency(tag.data, 0x00);
                    }
                    break;

                default:
                    break;
            }
        }
    }

    void on_map_load() noexcept {
        dependency_patches.clear();
        u16_patches.clear();
        float_patches.clear();
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
