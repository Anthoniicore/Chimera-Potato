#include "camo_fix.h"
#include "../client_signature.h"
#include "../hooks/tick.h"
#include "../halo_data/table.h"
#include "../messaging/messaging.h"
#include "../interpolation/camera.h"

void camo_fix() noexcept
{
    **reinterpret_cast<char **>(get_signature("camo_fix_sig").address() + 2) = 0;
    remove_tick_event(camo_fix);
}

// "DART" = Disable Alpha Render Target. Cuando el alpha render target está
// desactivado, el motor NO puede renderizar la refracción del camo (el efecto
// "líquido"), porque esa refracción necesita render-to-texture: capturar la
// pantalla y muestrearla distorsionada. Sin render target el motor cae a un
// alpha-blend plano y NINGÚN parche de memoria puede recrear la distorsión
// (haría falta hookear D3D9 y un shader propio, que Chimera no expone).
//
// Esto solo ajusta la transparencia del fallback plano. OJO: el valor 0.9f es
// EXACTAMENTE el mismo que ya tiene la firma (0x3F666666), así que tal cual está
// es un no-op. Para cambiar el aspecto del fallback, escribe un valor distinto
// (p.ej. 0.5f = más transparente). Para tener el efecto líquido de verdad hay que
// NO desactivar el alpha render target.
void dart_fix() noexcept
{
    write_code_any_value(get_signature("alpha_blend_transparency_sig").address() + 4, static_cast<float>(0.9));
}