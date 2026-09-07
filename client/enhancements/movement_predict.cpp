#include "movement_predict.h"
#include "../messaging/messaging.h"
#include "../hooks/tick.h"
#include "../hooks/frame.h"
#include "../halo_data/table.h"
#include "../../math/data_types.h"
#include <cstring>
#include <cstdlib>
#include <cmath>

// ── Qué hace ──────────────────────────────────────────────────────────────────
//
// Bajo lag, las posiciones de los objetos llegan del servidor cada varios ticks.
// Entre ticks el objeto se ve "congelado" y luego salta — el efecto teleport.
//
// Este módulo extrapola la posición usando la VELOCIDAD real que el motor de Halo
// ya calcula (BaseHaloObject::velocity), adelantando el render una fracción de tick:
//
//     render_pos = position_script + velocity * tick_progress * strength
//
// Es extrapolación lineal pura (no corrección de error): es simple, estable y no
// hace overshoot agresivo mientras strength se mantenga moderado (~0.5).
//
// ── Cómo se integra sin romper la interpolación ──────────────────────────────
//
// El campo modificado, position_script (offset 0xA0), es exactamente el que usa
// la interpolación y el render. El orden de eventos es:
//
//   PREFRAME: interpolate_objects (DEFAULT)  → escribe la posición interpolada
//             predict_apply       (AFTER)    → guarda esa posición y le suma la
//                                              predicción (este módulo)
//   <render>                                 → dibuja la posición predicha
//   FRAME:    predict_restore     (BEFORE)   → restaura lo que guardamos
//             rollback_interp.    (DEFAULT)  → restaura la posición real (si interp on)
//
// Como predict_restore corre ANTES del rollback de la interpolación y, sin
// interpolación, restaura directamente el valor real, position_script siempre
// vuelve a su valor verdadero al terminar el frame. Así el siguiente preframe
// (y la lógica del juego / scripting) nunca ven un valor corrupto, y no hay
// realimentación que haga derivar la posición.

static bool  active           = false;
static float predict_strength = 0.5f; // 0 = sin predicción, 1 = un tick completo

// Copia de seguridad por objeto del valor de position_script que sobrescribimos,
// para restaurarlo después del render.
static Vector3D saved_pos[2048];
static bool     pos_saved[2048] = {};

// Definidos por el sistema de interpolación (interpolation.cpp).
extern float  interpolation_tick_progress;
extern size_t chimera_interpolate_setting;

static bool predictable(const BaseHaloObject &object) noexcept {
    // Solo bipeds (0) y vehicles (1); el resto no se beneficia.
    if(object.object_type > 1) return false;
    if(object.phased_out)      return false;
    // No predecir objetos recién aparecidos (respawn, etc.).
    if(object.existence_time <= 2) return false;
    return true;
}

// PREFRAME, prioridad AFTER: corre después de interpolate_objects.
// Guarda position_script y le añade la extrapolación de velocidad.
static void predict_apply() noexcept {
    if(!active) return;

    // Con interpolación usamos su progreso de tick; sin ella lo calculamos nosotros
    // para que el módulo también funcione de forma autónoma.
    float tp = chimera_interpolate_setting ? interpolation_tick_progress
                                           : static_cast<float>(tick_progress());

    for(uint32_t i = 0; i < 2048; i++) {
        HaloObject obj(i);
        auto *data = obj.object_data();
        if(!data) { pos_saved[i] = false; continue; }

        auto &object = *reinterpret_cast<BaseHaloObject *>(data);
        if(!predictable(object)) { pos_saved[i] = false; continue; }

        const auto &vel = object.velocity;
        float speed_sq = vel.x*vel.x + vel.y*vel.y + vel.z*vel.z;
        if(speed_sq < 0.00001f) { pos_saved[i] = false; continue; } // quieto: nada que predecir

        auto &pos = object.position_script;
        saved_pos[i] = pos;                       // guardar para restaurar tras el render
        pos.x += vel.x * tp * predict_strength;
        pos.y += vel.y * tp * predict_strength;
        pos.z += vel.z * tp * predict_strength;
        pos_saved[i] = true;
    }
}

// FRAME (post-render), prioridad BEFORE: corre antes del rollback de la interpolación.
// Restaura el valor de position_script que habíamos sobrescrito.
static void predict_restore() noexcept {
    for(uint32_t i = 0; i < 2048; i++) {
        if(!pos_saved[i]) continue;
        HaloObject obj(i);
        auto *data = obj.object_data();
        if(data) {
            reinterpret_cast<BaseHaloObject *>(data)->position_script = saved_pos[i];
        }
        pos_saved[i] = false;
    }
}

static void enable() noexcept {
    if(active) return;
    memset(pos_saved, 0, sizeof(pos_saved));
    add_preframe_event(predict_apply, EVENT_PRIORITY_AFTER);
    add_frame_event(predict_restore, EVENT_PRIORITY_BEFORE);
    active = true;
}

static void disable() noexcept {
    if(!active) return;
    remove_preframe_event(predict_apply);
    remove_frame_event(predict_restore);
    // predict_restore ya devolvió position_script a su valor real el frame anterior.
    memset(pos_saved, 0, sizeof(pos_saved));
    active = false;
}

ChimeraCommandError movement_predict_command(size_t argc, const char **argv) noexcept {
    if(argc >= 1) {
        std::string arg = argv[0];
        if(arg == "false" || arg == "off" || arg == "0") {
            disable();
            console_out("chimera_movement_predict: off");
            return CHIMERA_COMMAND_ERROR_SUCCESS;
        }

        float new_strength = strtof(argv[0], nullptr);
        if(new_strength < 0.0f || new_strength > 1.0f) {
            console_out_error("chimera_movement_predict: value must be between 0.0 and 1.0.");
            return CHIMERA_COMMAND_ERROR_FAILURE;
        }
        // El segundo argumento (correction speed) ya no se usa; se acepta por
        // compatibilidad con configuraciones antiguas pero se ignora.

        predict_strength = new_strength;
        enable();
    }

    if(active)
        console_out("chimera_movement_predict: strength=" + std::to_string(predict_strength));
    else
        console_out("chimera_movement_predict: off");

    return CHIMERA_COMMAND_ERROR_SUCCESS;
}
