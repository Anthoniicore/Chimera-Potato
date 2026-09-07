#include "fps_boost.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <vector>

#include "firing_particle.h"
#include "zoom_blur.h"
#include "../client_signature.h"
#include "../hooks/map_load.h"
#include "../halo_data/tag_data.h"
#include "../halo_data/tiarace/hce_tag_class_int.h"
#include "../messaging/messaging.h"
#include "../visuals/anisotropic_filtering.h"

// FPS profile for Halo CE 1.10.
//
// Important design rule:
//   - Never touch transparent shader tags. Active Camo uses the transparent
//     rendering path and its alpha render target must remain enabled.
//   - World (shader_environment) textures may be stripped because the user
//     explicitly prefers FPS over visual fidelity.
//   - Player/weapon (shader_model) base maps are preserved. Only their detail
//     maps are stripped.
//   - No object-table or gameplay state is modified.
//
// The tag edits below operate on loaded tag dependencies. We only clear the
// datum/index (the last 4 bytes of Halo's 16-byte TagDependency), then save the
// original value so the profile can be reversed safely on the current map.

namespace {
    constexpr uint32_t NULL_TAG_ID = 0xFFFFFFFFu;

    // Halo 1 Shader base struct starts at the beginning of each shader tag.
    // Shader::detail_level is the uint16 at offset 0x02.
    constexpr size_t SHADER_DETAIL_LEVEL_OFFSET = 0x02;

    // shader_environment (senv) offsets, derived from the H1 tag definition.
    constexpr size_t SENV_BASE_MAP_OFFSET       = 0x88;
    constexpr size_t SENV_PRIMARY_DETAIL_OFFSET = 0xB8;
    constexpr size_t SENV_SECONDARY_DETAIL_OFFSET = 0xCC;
    constexpr size_t SENV_MICRO_DETAIL_OFFSET   = 0xFC;
    constexpr size_t SENV_BUMP_MAP_OFFSET       = 0x128;

    // shader_model (soso) detail_map offset.
    constexpr size_t SOSO_DETAIL_MAP_OFFSET = 0xEC;

    // shader_transparent_water (swat) offsets.
    constexpr size_t SWAT_BASE_MAP_OFFSET = 0x4C;
    constexpr size_t SWAT_REFLECTION_MAP_OFFSET = 0x7C;
    constexpr size_t SWAT_RIPPLE_MAP_OFFSET = 0x9C;
    constexpr size_t SWAT_PERPENDICULAR_BRIGHTNESS_OFFSET = 0x5C;
    constexpr size_t SWAT_PARALLEL_BRIGHTNESS_OFFSET = 0x6C;

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
        if(*datum == NULL_TAG_ID) return;
        dependency_patches.push_back({datum, *datum});
        *datum = NULL_TAG_ID;
    }

    void patch_detail_level(char *tag_data) noexcept {
        auto *value = reinterpret_cast<uint16_t *>(tag_data + SHADER_DETAIL_LEVEL_OFFSET);
        // 3 = turd, the lowest shader lightmap detail level.
        if(*value == 3) return;
        detail_level_patches.push_back({value, *value});
        *value = 3;
    }

    void patch_water_float(char *tag_data, size_t offset, float new_value) noexcept {
        auto *value = reinterpret_cast<float *>(tag_data + offset);
        water_float_patches.push_back({value, *value});
        *value = new_value;
    }

    void restore_tag_patches() noexcept {
        for(auto it = dependency_patches.rbegin(); it != dependency_patches.rend(); ++it)
            *it->datum = it->old_value;
        for(auto it = detail_level_patches.rbegin(); it != detail_level_patches.rend(); ++it)
            *it->value = it->old_value;
        for(auto it = water_float_patches.rbegin(); it != water_float_patches.rend(); ++it)
            *it->value = it->old_value;

        dependency_patches.clear();
        detail_level_patches.clear();
        water_float_patches.clear();
    }

    void apply_tag_profile(int level) noexcept {
        if(level < 2) return;

        auto *tags = *reinterpret_cast<HaloTag **>(0x40440000);
        auto count = *reinterpret_cast<uint32_t *>(0x4044000C);

        if(!tags || count == 0 || count > 65535) return;

        for(uint32_t i = 0; i < count; ++i) {
            auto &tag = tags[i];
            if(!tag.data) continue;

            switch(tag.tag_class) {
                case HaloCE::TAG_CLASS_INT_SHADER_ENVIRONMENT:
                    // Level 2: keep geometry and lightmaps, but remove the
                    // expensive material texture stack from BSP surfaces.
                    patch_detail_level(tag.data);
                    patch_dependency(tag.data, SENV_PRIMARY_DETAIL_OFFSET);
                    patch_dependency(tag.data, SENV_SECONDARY_DETAIL_OFFSET);
                    patch_dependency(tag.data, SENV_MICRO_DETAIL_OFFSET);
                    patch_dependency(tag.data, SENV_BUMP_MAP_OFFSET);
                    if(level >= 3) {
                        // Ultra: remove the BSP base texture as well. This is
                        // intentionally visual-only and does not affect models.
                        patch_dependency(tag.data, SENV_BASE_MAP_OFFSET);
                    }
                    break;

                case HaloCE::TAG_CLASS_INT_SHADER_MODEL:
                    // Keep player/weapon base textures. Only remove detail maps
                    // and lower the lightmap detail level.
                    patch_detail_level(tag.data);
                    patch_dependency(tag.data, SOSO_DETAIL_MAP_OFFSET);
                    break;

                case HaloCE::TAG_CLASS_INT_SHADER_TRANSPARENT_WATER:
                    if(level >= 3) {
                        // Water is explicitly allowed to disappear. Do not
                        // touch any other transparent shader class, especially
                        // chicago/chicago_extended used by visual effects/camo.
                        patch_dependency(tag.data, SWAT_BASE_MAP_OFFSET);
                        patch_dependency(tag.data, SWAT_REFLECTION_MAP_OFFSET);
                        patch_dependency(tag.data, SWAT_RIPPLE_MAP_OFFSET);
                        patch_water_float(tag.data, SWAT_PERPENDICULAR_BRIGHTNESS_OFFSET, 0.0f);
                        patch_water_float(tag.data, SWAT_PARALLEL_BRIGHTNESS_OFFSET, 0.0f);
                    }
                    break;

                default:
                    // Deliberately leave all transparent shaders untouched.
                    break;
            }
        }
    }

    void on_map_load() noexcept {
        // The map load replaces the tag array. The old addresses must never be
        // restored after that replacement.
        dependency_patches.clear();
        detail_level_patches.clear();
        water_float_patches.clear();

        apply_tag_profile(active_level);
    }

    void set_af(bool enabled) noexcept {
        auto &setting = **reinterpret_cast<char **>(get_signature("af_is_enabled_sig").address() + 1);
        setting = enabled;
    }

    void set_multitexture_overlay(bool disable) noexcept {
        if(!optional_multitexture_overlay) return;
        auto &sig = get_signature("multitexture_overlay_sig");
        if(disable) {
            const short mod[] = {-1, -1, 0x60};
            write_code_s(sig.address(), mod);
        }
        else {
            sig.undo();
        }
    }

    void set_zoom_blur(bool disable) noexcept {
        if(!optional_zoom_blur) return;
        const char *arg[] = {disable ? "true" : "false"};
        block_zoom_blur_command(1, arg);
    }

    void apply_level(int level) noexcept {
        if(level < 0) level = 0;
        if(level > 3) level = 3;

        if(level == 0) {
            restore_tag_patches();
            set_af(false);
            set_multitexture_overlay(false);
            set_zoom_blur(false);
            const char *arg[] = {"false"};
            block_firing_particles_command(1, arg);
        }
        else {
            set_af(false);
            set_multitexture_overlay(true);
            // Keep Halo CE's normal zoom blur. Disabling it makes the zoomed
            // scene lose the surrounding blur/effect that the user wants to keep.
            set_zoom_blur(false);
            const char *arg[] = {"true"};
            block_firing_particles_command(1, arg);
            restore_tag_patches();
            apply_tag_profile(level);
        }

        active_level = level;
    }
}

void initialize_fps_boost() noexcept {
    // These are optional signatures. A failure here must never prevent Chimera
    // from loading; the FPS profile simply skips that optimization.
    optional_multitexture_overlay = find_multitexture_overlay_signature();
    optional_zoom_blur = find_zoom_blur_signatures();
    add_map_load_event(on_map_load, EVENT_PRIORITY_AFTER);
}

ChimeraCommandError fps_boost_command(size_t argc, const char **argv) noexcept {
    if(argc == 1) {
        char *end = nullptr;
        long parsed = strtol(argv[0], &end, 10);
        if(end == argv[0] || *end != '\0' || parsed < 0 || parsed > 3) {
            console_out_warning("Invalid FPS boost level. Use 0, 1, 2 or 3.");
            char current[8] = {};
            sprintf(current, "%d", active_level);
            console_out(current);
            return CHIMERA_COMMAND_ERROR_FAILURE;
        }
        apply_level(static_cast<int>(parsed));
    }

    char current[8] = {};
    sprintf(current, "%d", active_level);
    console_out(current);
    return CHIMERA_COMMAND_ERROR_SUCCESS;
}
