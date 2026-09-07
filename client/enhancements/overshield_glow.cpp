#include "overshield_glow.h"
#include "../hooks/tick.h"
#include "../halo_data/table.h"
#include <stdint.h>

// ── Qué hace ──────────────────────────────────────────────────────────────────
//
// Mientras una unidad tiene OVERSHIELD (shield > max_shields), mantenemos el
// mismo brillo/shimmer que Halo dibuja cuando el escudo RECIBE DAÑO.
//
// El motor renderiza ese efecto mientras current_shield_damage (offset 0xE8) y
// shields_hit (0x124) sean > 0, y los va decayendo solo cada tick. Por eso basta
// con "rearmarlos" cada pretick: el efecto se mantiene visible de forma continua.
// Cuando el overshield se agota dejamos de escribir y el motor apaga el brillo
// solo (fade-out natural). Escribir estos campos es PURAMENTE VISUAL en el cliente
// — no toca la vida/escudo real ni la recarga (la recarga la controla 0x104).
//
// Offsets verificados (BaseHaloObject, table.h y aLTis94/Halo_CE offsets.lua):
//   max_shields           0xDC
//   shield                0xE4
//   current_shield_damage 0xE8   ← campo principal que enciende el shader
//   shields_hit           0x124  ← refuerza el feedback del impacto
//
// Se aplica a TODOS los bipeds (object_type 0) con overshield, no solo al jugador
// local, para que el brillo se vea en ti (3ra persona / vehículo / death-cam) y en
// los demás jugadores en multijugador.

// Intensidad del brillo, 0..1. 1.0 ≈ un impacto reciente a tope (lo más parecido
// a "como cuando recibe daño"). Bájalo si lo quieres más sutil.
static constexpr float GLOW_INTENSITY = 1.0f;

static void on_pretick() noexcept {
    for(uint32_t i = 0; i < 2048; i++) {
        HaloObject obj(i);
        auto *data = obj.object_data();
        if(!data) continue;

        auto &o = *reinterpret_cast<BaseHaloObject *>(data);
        if(o.object_type != 0) continue;                 // solo bipeds
        if(o.shield <= o.max_shields + 0.01f) continue;  // solo con overshield

        // La memoria del objeto está en el heap del juego (ya escribible),
        // así que no hace falta VirtualProtect.
        o.current_shield_damage = GLOW_INTENSITY; // dispara el shader del escudo
        o.shields_hit           = GLOW_INTENSITY; // refuerza el shimmer del impacto
    }
}

void initialize_overshield_glow() noexcept {
    add_pretick_event(on_pretick);
}
