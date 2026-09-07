#include "throttle_fps.h"
#include "../client_signature.h"
#include "../hooks/frame.h"
#include "../messaging/messaging.h"
#include <windows.h>
#include <mmsystem.h>

// ── Problema con el throttle original ────────────────────────────────────────
//
// El código anterior usaba Sleep(ms) con el valor exacto de tiempo restante.
// Windows tiene una resolución de timer de ~15ms por defecto (timeBeginPeriod
// no estaba activo). A 240hz cada frame dura 4.16ms — Sleep(3) podía dormir
// 15ms, causando que el framecap efectivo fuera mucho menor al configurado.
//
// ── Solución: Sleep híbrido + spinwait ────────────────────────────────────────
//
// 1. timeBeginPeriod(1): eleva la resolución del timer de Windows a 1ms.
//    Esto hace que Sleep(1) duerma ~1ms en lugar de ~15ms.
//    Costo: aumenta ligeramente el consumo de energía del sistema.
//
// 2. Sleep mientras queda > SPINWAIT_THRESHOLD ms:
//    Cede CPU para no quemar innecesariamente.
//
// 3. Spinwait puro cuando queda < SPINWAIT_THRESHOLD:
//    Busy-loop con QueryPerformanceCounter hasta el momento exacto.
//    Consume 100% de un core durante ese período, pero es < 2ms.
//    Da precisión de microsegundos — necesaria a 240hz+.
//
// A 240hz: frame = 4.16ms → Sleep ~2ms + spinwait ~2ms = precisión perfecta.
// A 360hz: frame = 2.77ms → Sleep ~0ms + spinwait ~2ms = todo spinwait.
// A 144hz: frame = 6.94ms → Sleep ~4ms + spinwait ~2ms = balance óptimo.

// Tiempo en segundos por debajo del cual cambiamos a spinwait puro.
// 2ms es el punto óptimo: suficiente para absorber varianza del Sleep,
// lo suficientemente pequeño para no quemar CPU innecesariamente.
static constexpr double SPINWAIT_THRESHOLD = 0.002;

static LARGE_INTEGER prev_time;
static double        throttle_fps    = 0.0;
static bool          period_set      = false;

static void on_frame() noexcept {
    double seconds_per_frame = 1.0 / throttle_fps;

    while(true) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsed = counter_time_elapsed(prev_time, now);
        double remaining = seconds_per_frame - elapsed;

        if(remaining <= 0.0) {
            // Tiempo cumplido — avanzar el timestamp base y salir
            // Usamos prev_time += frame_period en lugar de now para
            // evitar acumulación de error cuando frames tardan más de lo esperado
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            prev_time.QuadPart += (LONGLONG)(seconds_per_frame * freq.QuadPart);

            // Si nos atrasamos más de 2 frames, resincronizar para evitar
            // que el juego intente "recuperar" frames acumulados
            double new_elapsed = counter_time_elapsed(prev_time, now);
            if(new_elapsed > seconds_per_frame) {
                prev_time = now;
            }
            break;
        }

        if(remaining > SPINWAIT_THRESHOLD) {
            // Todavía queda suficiente tiempo — ceder CPU
            // Sleep(1) con timeBeginPeriod(1) activo duerme ~1ms
            DWORD sleep_ms = (DWORD)((remaining - SPINWAIT_THRESHOLD) * 1000.0);
            if(sleep_ms > 0) Sleep(sleep_ms);
        }
        // else: spinwait puro — el bucle while re-evalúa inmediatamente
    }
}

ChimeraCommandError throttle_fps_command(size_t argc, const char **argv) noexcept {
    if(argc == 1) {
        double new_value = atof(argv[0]);

        if(new_value != throttle_fps) {
            if(new_value != 0.0 && new_value <= 5.0) {
                console_out_error("chimera_throttle_fps: invalid framerate.");
                return CHIMERA_COMMAND_ERROR_FAILURE;
            }

            if(new_value == 0.0) {
                remove_frame_event(on_frame);

                // Restaurar resolución del timer si la habíamos modificado
                if(period_set) {
                    timeEndPeriod(1);
                    period_set = false;
                }
            }
            else {
                // Activar resolución de 1ms para Sleep preciso
                if(!period_set) {
                    timeBeginPeriod(1);
                    period_set = true;
                }

                add_frame_event(on_frame, EVENT_PRIORITY_FINAL);
                QueryPerformanceCounter(&prev_time);
            }

            throttle_fps = new_value;
        }
    }

    if(throttle_fps == 0.0)
        console_out("off");
    else
        console_out(std::to_string(throttle_fps));

    return CHIMERA_COMMAND_ERROR_SUCCESS;
}