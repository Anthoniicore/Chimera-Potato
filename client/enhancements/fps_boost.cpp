#include "fps_boost.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <ctype.h>
#include <windows.h>

#include "firing_particle.h"
#include "zoom_blur.h"
#include "../client_signature.h"
#include "../hooks/map_load.h"
#include "../hooks/tick.h"
#include "../halo_data/tag_data.h"
#include "../halo_data/table.h"
#include "../halo_data/spawn_object.h"
#include "../halo_data/tiarace/hce_tag_class_int.h"
#include "../messaging/messaging.h"
#include "../visuals/anisotropic_filtering.h"

// =============================================================================
// Chimera Potato – engine patch (original 2018) + camo-safe limits
// =============================================================================
// Original ultra sets rasterizer flags that make Active Camo fully invisible.
// We keep the FPS-oriented disables but NEVER:
//   - set OFF_00 / OFF_01 (opaque-only style flags in original ultra)
//   - zero OFF_04 word aggressively on high/ultra
//   - NOP potato_workaround (can break transparent paths)
//   - null shader_model multipurpose maps (Active Camo uses them)
//   - touch chicago / plasma / generic transparent shaders
// =============================================================================

namespace {
    constexpr uint32_t NULL_TAG_ID = 0xFFFFFFFFu;

    const short POTATO_SIG[] = {
        0x66, 0x39, 0x1D, -1, -1, -1, -1, 0x75,
        0x47, 0x38, 0x1D, -1, -1, -1, -1, 0x74,
        0x3F, 0xBF, 0x02, 0x00, 0x00, 0x00, 0xE8, 0x18,
        0xE5, 0x00, 0x00
    };

    constexpr size_t OFF_09 = 0x09;
    constexpr size_t OFF_0E = 0x0E;
    constexpr size_t OFF_10 = 0x10;
    constexpr size_t OFF_11 = 0x11;
    constexpr size_t OFF_14 = 0x14;
    constexpr size_t OFF_16 = 0x16;
    constexpr size_t OFF_17 = 0x17;

    constexpr size_t SHADER_DETAIL_LEVEL_OFFSET = 0x02;
    constexpr size_t SENV_FLAGS_OFFSET            = 0x28;
    constexpr size_t SENV_BASE_MAP_OFFSET         = 0x88;
    constexpr size_t SENV_PRIMARY_DETAIL_OFFSET   = 0xB8;
    constexpr size_t SENV_SECONDARY_DETAIL_OFFSET = 0xCC;
    constexpr size_t SENV_MICRO_DETAIL_OFFSET     = 0xFC;
    constexpr size_t SENV_BUMP_MAP_OFFSET         = 0x128;
    constexpr size_t SENV_REFLECTION_FLAGS_OFFSET = 0x2F4;
    constexpr size_t SENV_PERP_BRIGHTNESS_OFFSET  = 0x318;
    constexpr size_t SENV_PARA_BRIGHTNESS_OFFSET  = 0x31C;
    constexpr size_t SENV_REFLECTION_CUBE_OFFSET  = 0x340;
    constexpr size_t SOSO_DETAIL_MAP_OFFSET       = 0xEC;
    constexpr size_t SOSO_PERP_BRIGHTNESS_OFFSET  = 0x18C;
    constexpr size_t SOSO_PARA_BRIGHTNESS_OFFSET  = 0x19C;
    constexpr size_t SOSO_REFLECTION_CUBE_OFFSET  = 0x1AC;
    constexpr size_t SWAT_BASE_MAP_OFFSET                 = 0x4C;
    constexpr size_t SWAT_REFLECTION_MAP_OFFSET           = 0x7C;
    constexpr size_t SWAT_RIPPLE_MAP_OFFSET               = 0x9C;
    constexpr size_t SWAT_PERPENDICULAR_BRIGHTNESS_OFFSET = 0x5C;
    constexpr size_t SWAT_PARALLEL_BRIGHTNESS_OFFSET      = 0x6C;
    constexpr size_t SGLA_REFLECTION_MAP_OFFSET  = 0x70;
    constexpr size_t SGLA_DIFFUSE_DETAIL_OFFSET  = 0xB8;
    constexpr size_t SGLA_PERP_BRIGHTNESS_OFFSET = 0x50;
    constexpr size_t SGLA_PARA_BRIGHTNESS_OFFSET = 0x60;
    constexpr size_t SKY_MODEL_OFFSET = 0x00;

    constexpr uint16_t OBJECT_TYPE_BIPED = 0;

    struct DependencyPatch { uint32_t *datum; uint32_t old_value; };
    struct U16Patch { uint16_t *value; uint16_t old_value; };
    struct FloatPatch { float *value; float old_value; };
    struct BytePatch { uint8_t *addr; uint8_t old_value; };

    std::vector<DependencyPatch> dependency_patches;
    std::vector<U16Patch> u16_patches;
    std::vector<FloatPatch> float_patches;
    std::vector<BytePatch> engine_byte_patches;

    bool optional_zoom_blur = false;
    bool optional_multitexture_overlay = false;
    int active_level = 0;
    bool corpse_cleanup_enabled = false;
    bool engine_potato_found = false;
    uint8_t *rasterizer_flags = nullptr;

    static void *scan_module(const short *pattern, size_t pattern_len) noexcept {
        HMODULE halo = GetModuleHandleA(nullptr);
        if (!halo) return nullptr;
        auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(halo);
        auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(reinterpret_cast<char *>(halo) + dos->e_lfanew);
        auto *sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            auto *start = reinterpret_cast<uint8_t *>(halo) + sec[i].VirtualAddress;
            size_t size = sec[i].Misc.VirtualSize;
            if (size < pattern_len) continue;
            for (size_t off = 0; off + pattern_len <= size; ++off) {
                bool ok = true;
                for (size_t j = 0; j < pattern_len; ++j) {
                    if (pattern[j] == -1) continue;
                    if (start[off + j] != static_cast<uint8_t>(pattern[j])) {
                        ok = false;
                        break;
                    }
                }
                if (ok) return start + off;
            }
        }
        return nullptr;
    }

    static bool resolve_engine_potato() noexcept {
        if (engine_potato_found && rasterizer_flags) return true;
        void *match = scan_module(POTATO_SIG, sizeof(POTATO_SIG) / sizeof(POTATO_SIG[0]));
        if (!match) {
            engine_potato_found = false;
            rasterizer_flags = nullptr;
            return false;
        }
        rasterizer_flags = *reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(match) + 3);
        engine_potato_found = rasterizer_flags != nullptr;
        return engine_potato_found;
    }

    static void engine_write_byte(size_t offset, uint8_t value) noexcept {
        if (!rasterizer_flags) return;
        auto *p = rasterizer_flags + offset;
        engine_byte_patches.push_back({p, *p});
        DWORD old;
        VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old);
        *p = value;
        VirtualProtect(p, 1, old, &old);
    }

    static void restore_engine_patches() noexcept {
        for (auto it = engine_byte_patches.rbegin(); it != engine_byte_patches.rend(); ++it) {
            DWORD old;
            VirtualProtect(it->addr, 1, PAGE_EXECUTE_READWRITE, &old);
            *it->addr = it->old_value;
            VirtualProtect(it->addr, 1, old, &old);
        }
        engine_byte_patches.clear();
    }

    // Camo-safe engine level: disable expensive flags only.
    // Skips original OFF_00=1, OFF_01=1, OFF_04=0, OFF_18/1B/23/6C=0 and workaround NOP.
    static void apply_engine_level(int level) noexcept {
        restore_engine_patches();
        if (level <= 0) return;
        if (!resolve_engine_potato()) return;

        // low+
        engine_write_byte(OFF_0E, 0);
        engine_write_byte(OFF_14, 0);
        engine_write_byte(OFF_16, 0);

        if (level >= 3) {
            engine_write_byte(OFF_09, 0);
            engine_write_byte(OFF_11, 0);
            engine_write_byte(OFF_17, 0);
        }
        if (level >= 4) {
            engine_write_byte(OFF_10, 0);
        }
    }

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
        *value = 3;
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
        return (*reinterpret_cast<uint16_t *>(tag_data + SENV_FLAGS_OFFSET) & 0x1) != 0;
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
                    patch_dependency(tag.data, SENV_PRIMARY_DETAIL_OFFSET);
                    patch_dependency(tag.data, SENV_SECONDARY_DETAIL_OFFSET);
                    patch_dependency(tag.data, SENV_MICRO_DETAIL_OFFSET);
                    if (!alpha_tested)
                        patch_dependency(tag.data, SENV_BUMP_MAP_OFFSET);
                    if (level >= 3) {
                        if (!alpha_tested)
                            patch_dependency(tag.data, SENV_BASE_MAP_OFFSET);
                        patch_u16_flags(tag.data, SENV_REFLECTION_FLAGS_OFFSET, 0x1);
                        patch_float(tag.data, SENV_PERP_BRIGHTNESS_OFFSET, 0.0f);
                        patch_float(tag.data, SENV_PARA_BRIGHTNESS_OFFSET, 0.0f);
                        patch_dependency(tag.data, SENV_REFLECTION_CUBE_OFFSET);
                    }
                    break;
                }
                case HaloCE::TAG_CLASS_INT_SHADER_MODEL:
                    // KEEP multipurpose map – Active Camo depends on it
                    patch_detail_level(tag.data);
                    patch_dependency(tag.data, SOSO_DETAIL_MAP_OFFSET);
                    patch_float(tag.data, SOSO_PERP_BRIGHTNESS_OFFSET, 0.0f);
                    patch_float(tag.data, SOSO_PARA_BRIGHTNESS_OFFSET, 0.0f);
                    patch_dependency(tag.data, SOSO_REFLECTION_CUBE_OFFSET);
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
                    if (level >= 3) {
                        patch_float(tag.data, SGLA_PERP_BRIGHTNESS_OFFSET, 0.0f);
                        patch_float(tag.data, SGLA_PARA_BRIGHTNESS_OFFSET, 0.0f);
                        patch_dependency(tag.data, SGLA_REFLECTION_MAP_OFFSET);
                        patch_dependency(tag.data, SGLA_DIFFUSE_DETAIL_OFFSET);
                    }
                    break;
                case HaloCE::TAG_CLASS_INT_SKY:
                    if (level >= 4)
                        patch_dependency(tag.data, SKY_MODEL_OFFSET);
                    break;
                case HaloCE::TAG_CLASS_INT_SHADER_TRANSPARENT_CHICAGO:
                case HaloCE::TAG_CLASS_INT_SHADER_TRANSPARENT_CHICAGO_EXTENDED:
                case HaloCE::TAG_CLASS_INT_SHADER_TRANSPARENT_PLASMA:
                case HaloCE::TAG_CLASS_INT_SHADER_TRANSPARENT_GENERIC:
                    // Active Camo / shields – never touch
                    break;
                default:
                    break;
            }
        }
    }

    void cleanup_dead_corpses() noexcept {
        if (!corpse_cleanup_enabled) return;
        auto &ot = get_object_table();
        if (!ot.first || ot.size == 0 || ot.max_count == 0) return;
        std::vector<uint32_t> to_delete;
        auto *entries = reinterpret_cast<char *>(ot.first);
        for (uint16_t i = 0; i < ot.size && i < ot.max_count; ++i) {
            char *entry = entries + static_cast<size_t>(i) * ot.index_size;
            uint16_t salt = *reinterpret_cast<uint16_t *>(entry);
            if (salt == 0xFFFF) continue;
            char *obj = *reinterpret_cast<char **>(entry + 0x8);
            if (!obj) continue;
            auto *base = reinterpret_cast<BaseHaloObject *>(obj);
            if (base->object_type != OBJECT_TYPE_BIPED) continue;
            if (base->health > 0.0f) continue;
            to_delete.push_back((static_cast<uint32_t>(salt) << 16) | i);
        }
        for (uint32_t id : to_delete)
            delete_object(id);
    }

    void on_tick() noexcept { cleanup_dead_corpses(); }

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
            restore_engine_patches();
            restore_tag_patches();
            set_af(true);
            set_multitexture_overlay(false);
            const char *arg[] = {"false"};
            block_firing_particles_command(1, arg);
            corpse_cleanup_enabled = false;
        } else {
            set_af(false);
            set_multitexture_overlay(true);
            const char *arg[] = {"true"};
            block_firing_particles_command(1, arg);
            apply_engine_level(level);
            restore_tag_patches();
            apply_tag_profile(level);
            corpse_cleanup_enabled = true;
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
    resolve_engine_potato();
    add_map_load_event(on_map_load, EVENT_PRIORITY_AFTER);
    add_tick_event(on_tick, EVENT_PRIORITY_DEFAULT);
}

static ChimeraCommandError potato_impl(size_t argc, const char **argv) noexcept {
    if (argc == 1) {
        int level = parse_level(argv[0]);
        if (level < 0) {
            console_out_warning("Invalid potato level. Use: off, low, medium, high, ultra  (or 0-4)");
            return CHIMERA_COMMAND_ERROR_FAILURE;
        }
        apply_level(level);
    }

    char current[128] = {};
    sprintf(current, "%s (%d) | engine=%s | camo=SAFE | corpses=%s",
            level_name(active_level), active_level,
            engine_potato_found ? "potato_sig OK" : "sig MISSING",
            corpse_cleanup_enabled ? "instant" : "normal");
    console_out(current);
    return CHIMERA_COMMAND_ERROR_SUCCESS;
}

ChimeraCommandError fps_boost_command(size_t argc, const char **argv) noexcept {
    return potato_impl(argc, argv);
}

ChimeraCommandError potato_command(size_t argc, const char **argv) noexcept {
    return potato_impl(argc, argv);
}
