#include "chat_fix.h"
#include "../client_signature.h"
#include "../messaging/messaging.h"
#include "../hooks/frame.h"
#include <windows.h>
#include <cstring>

// ── Por qué ocurre el text lag ────────────────────────────────────────────────
//
// Cada mensaje del chat tiene un fade_timer (int32) en la estructura de su
// entrada. El motor de Halo decrementa ese timer cada tick. Cuando llega a 0
// el mensaje deja de dibujarse (fade out).
//
// Cuando minimizas (ALT+TAB), Halo pausa su loop interno pero Windows sigue
// corriendo. Al volver, Halo calcula el tiempo transcurrido y aplica todos
// los ticks pendientes de golpe — lo que hace que los fade_timers caigan a 0
// o negativo inmediatamente, borrando todos los mensajes del chat de un golpe.
//
// ── Solución ──────────────────────────────────────────────────────────────────
//
// En cada frame detectamos si el juego acaba de recuperar el foco
// (GetForegroundWindow() == hwnd_halo). Si detectamos esa transición,
// recorremos las 14 entradas de la tabla del chat y restauramos los
// fade_timers que hayan caído a 0 en entradas que aún tienen texto.
//
// Estructura verificada desde haloce.exe:
//   chat_table_ptr en 0x0064DE30 → array de 14 entradas
//   cada entrada: 0x124 bytes
//   [entry + 0x000]: texto unicode (hasta 143 chars)
//   [entry + 0x120]: fade_timer (int32)

static constexpr uint32_t CHAT_TABLE_PTR_ADDR = 0x0064DE30;
static constexpr size_t   CHAT_ENTRY_STRIDE   = 0x124;
static constexpr size_t   CHAT_MAX_ENTRIES    = 14;
static constexpr size_t   CHAT_FADE_OFFSET    = 0x120;
static constexpr int32_t  CHAT_RESTORE_TICKS  = 150; // ~5 segundos a 30hz

static bool  active      = false;
static bool  had_focus   = true;
static HWND  hwnd_halo   = nullptr;

static void on_frame_chat_fix() noexcept {
    if(!active) return;

    // Obtener handle de la ventana de Halo la primera vez
    if(!hwnd_halo) {
        hwnd_halo = FindWindowA("Halo CE", nullptr);
        if(!hwnd_halo) hwnd_halo = FindWindowA("Halo", nullptr);
        if(!hwnd_halo) return;
    }

    bool has_focus = (GetForegroundWindow() == hwnd_halo);

    // Detectar transición sin foco → con foco
    if(has_focus && !had_focus) {
        // Leer el puntero a la tabla del chat
        auto *chat_table = *reinterpret_cast<char **>(CHAT_TABLE_PTR_ADDR);
        if(!chat_table) {
            had_focus = true;
            return;
        }

        for(size_t i = 0; i < CHAT_MAX_ENTRIES; i++) {
            auto *entry     = chat_table + i * CHAT_ENTRY_STRIDE;
            auto *fade_ptr  = reinterpret_cast<int32_t *>(entry + CHAT_FADE_OFFSET);
            auto *text_ptr  = reinterpret_cast<short *>(entry); // unicode

            // Si el timer cayó a 0 pero hay texto en la entrada, restaurar
            if(*fade_ptr <= 0 && text_ptr[0] != 0) {
                *fade_ptr = CHAT_RESTORE_TICKS;
            }
        }
    }

    had_focus = has_focus;
}

ChimeraCommandError chat_fix_command(size_t argc, const char **argv) noexcept {
    if(argc == 1) {
        bool new_value = bool_value(argv[0]);

        if(new_value && !active) {
            had_focus = true;
            hwnd_halo = nullptr;
            add_frame_event(on_frame_chat_fix);
            active = true;
        }
        else if(!new_value && active) {
            remove_frame_event(on_frame_chat_fix);
            active = false;
        }
    }

    console_out(active ? "true" : "false");
    return CHIMERA_COMMAND_ERROR_SUCCESS;
}
