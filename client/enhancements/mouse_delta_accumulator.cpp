#include "mouse_delta_accumulator.h"
#include "../client_signature.h"
#include "../messaging/messaging.h"
#include "../hooks/tick.h"
#include "../hooks/frame.h"
#include <windows.h>
#include <cstring>

// ── Problema que resuelve este módulo ────────────────────────────────────────
//
// Halo CE corre su física a 30 hz. A 240 hz tienes 8 frames por cada tick.
//
// El comportamiento ORIGINAL de Halo:
//   frame 1: mouse mueve +50 counts  → Halo guarda 50 en su acumulador
//   frame 2: mouse mueve +30 counts  → Halo SOBREESCRIBE a 30 (pierde los 50)
//   ...
//   tick:    Halo lee 30, aplica al aim
//   Resultado: perdiste 50 counts de movimiento real
//
// Con este fix:
//   frame 1: mouse mueve +50 → acumulamos 50
//   frame 2: mouse mueve +30 → acumulamos 80
//   ...
//   tick:    Halo lee 80, aplica al aim
//   Resultado: todo el movimiento llega al aim sin pérdida
//
// ── Cómo funciona técnicamente ───────────────────────────────────────────────
//
// Las firmas mouse_horiz_1_sig y mouse_vert_1_sig tienen la instrucción:
//   mov ecx, [ptr]   (8B 0D XX XX XX XX)
// donde ptr es la dirección del int32 que Halo usa como acumulador de delta.
//
// Halo escribe en ese int32 el delta del frame actual CADA FRAME,
// sobreescribiendo el valor anterior sin sumarlo. Nosotros:
//
//  1. Leemos el acumulador de Halo en cada preframe (antes de que Halo lo use)
//  2. Lo sumamos a nuestro propio acumulador
//  3. Ponemos el acumulador de Halo a 0 para que no se acumule doble
//  4. En el pretick (justo antes de que el tick consuma el delta)
//     escribimos nuestro total acumulado en el acumulador de Halo y lo reseteamos
//
// Así Halo siempre ve el delta completo de todos los frames del tick,
// no solo el del último frame.
//
// ── ⚠️ CONFLICTO CON mouse_sensitivity — REVISAR CONTRA EL BINARIO ⚠️ ──────────
//
// IMPORTANTE: este módulo lee el operando en mouse_horiz_1_sig+2 / mouse_vert_1_sig+2,
// que es EXACTAMENTE el mismo operando que chimera_mouse_sensitivity sobrescribe
// (mouse_sensitivity.cpp hace write_code_any_value(mouse_horiz_1_sig.address()+2, &horiz)).
// No son "punteros diferentes": es el mismo. Por lo tanto:
//
//   * Si activas mouse_sensitivity y luego este módulo, el puntero cacheado apunta a
//     los floats de escala de mouse_sensitivity y este código los pondría a 0 cada
//     frame (tratándolos como int32), rompiendo ambas funciones.
//   * Además el puntero se resuelve una sola vez al activar; si mouse_sensitivity se
//     activa/desactiva después, queda colgado (stale).
//   * La cola de la firma (… D9 E0 = fchs, … D9 1D = fstp) sugiere que [ptr] es un
//     FLOAT de escala del ratón, no un int32 de delta por frame, así que la premisa
//     misma podría ser incorrecta.
//
// NO usar junto con chimera_mouse_sensitivity. El arreglo correcto requiere localizar
// por desensamblado el acumulador real de delta (int32) con una firma propia y operar
// sobre ESE; eso debe verificarse contra haloce.exe.

static int32_t *halo_mouse_delta_x = nullptr; // acumulador horizontal de Halo
static int32_t *halo_mouse_delta_y = nullptr; // acumulador vertical de Halo

static int32_t our_accumulator_x = 0;         // nuestro total entre ticks
static int32_t our_accumulator_y = 0;

static bool active = false;

// Llamado en cada preframe: robamos el delta que Halo acaba de escribir,
// lo sumamos al nuestro, y dejamos el de Halo en 0.
static void accumulate_delta() noexcept {
    if(!halo_mouse_delta_x || !halo_mouse_delta_y) return;

    our_accumulator_x += *halo_mouse_delta_x;
    our_accumulator_y += *halo_mouse_delta_y;

    *halo_mouse_delta_x = 0;
    *halo_mouse_delta_y = 0;
}

// Llamado en pretick: justo antes de que el tick consuma el delta,
// escribimos el total acumulado en el acumulador de Halo.
// Halo lo leerá normalmente y aplicará todo el movimiento del período.
static void flush_delta_to_tick() noexcept {
    if(!halo_mouse_delta_x || !halo_mouse_delta_y) return;

    *halo_mouse_delta_x = our_accumulator_x;
    *halo_mouse_delta_y = our_accumulator_y;

    our_accumulator_x = 0;
    our_accumulator_y = 0;
}

ChimeraCommandError mouse_delta_accumulator_command(size_t argc, const char **argv) noexcept {
    auto &mouse_horiz_1_sig = get_signature("mouse_horiz_1_sig");
    auto &mouse_vert_1_sig  = get_signature("mouse_vert_1_sig");

    if(argc == 1) {
        bool new_value = bool_value(argv[0]);

        if(new_value && !active) {
            // Resolver los punteros a los acumuladores de Halo.
            // mouse_horiz_1_sig: 8B 0D [ptr 4 bytes] 50 51 E8 ...
            //                         ^+2              → ptr al int32 horizontal
            // mouse_vert_1_sig:  8B 0D [ptr 4 bytes] 50 51 E8 ...
            //                         ^+2              → ptr al int32 vertical
            halo_mouse_delta_x = *reinterpret_cast<int32_t **>(
                mouse_horiz_1_sig.address() + 2);
            halo_mouse_delta_y = *reinterpret_cast<int32_t **>(
                mouse_vert_1_sig.address() + 2);

            if(!halo_mouse_delta_x || !halo_mouse_delta_y) {
                console_out_error("chimera_mouse_delta_accumulator: no se pudieron resolver los acumuladores.");
                return CHIMERA_COMMAND_ERROR_FAILURE;
            }

            our_accumulator_x = 0;
            our_accumulator_y = 0;

            // preframe: capturar delta de cada frame
            add_preframe_event(accumulate_delta, EVENT_PRIORITY_BEFORE);

            // pretick: entregar el total acumulado a Halo justo antes del tick
            add_pretick_event(flush_delta_to_tick, EVENT_PRIORITY_BEFORE);

            active = true;
        }
        else if(!new_value && active) {
            remove_preframe_event(accumulate_delta);
            remove_pretick_event(flush_delta_to_tick);

            // Restaurar: dejar el acumulador de Halo intacto
            our_accumulator_x = 0;
            our_accumulator_y = 0;

            active = false;
        }
    }

    console_out(active ? "true" : "false");
    return CHIMERA_COMMAND_ERROR_SUCCESS;
}