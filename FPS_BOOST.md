# Chimera Potato (aggressive FPS mode)

Target: Halo Custom Edition 1.10

Much more aggressive than the first version. Designed for very old / low-end PCs.

## Commands

```text
chimera_potato [off/low/medium/high/ultra]
chimera_potato [0-4]
chimera_fps_boost   # alias
```

## Levels

| Level | Name   | Changes |
|-------|--------|---------|
| 0     | off    | Restore everything |
| 1     | low    | AF off, firing particles off, multitexture overlays off |
| 2     | medium | + strip BSP detail/bump maps, lowest lightmap detail |
| 3     | high   | + strip BSP base maps, kill water, **kill mirrors/reflections**, **disable sky model**, strip glass reflections |
| 4     | ultra  | + **strip base maps of characters / weapons / trees / rocks / scenery** (flat look, max FPS) |

## What ultra now does (your complaints)

- **Espejos / mirrors**: reflection cube maps cleared on environment + glass, dynamic mirror flag cleared, reflection brightness forced to 0.
- **Texturas de árboles, piedras, pasto, enemigos, personaje, armas**: `shader_model` base + multipurpose + detail + reflections stripped on ultra.
- **Cielo**: sky model dependency nulled on high/ultra (no sky draw).

## Still protected (Active Camo)

- Never touches `shader_transparent_chicago` / `chicago_extended` / `plasma`
- Never touches the alpha render target
- Only visual tag dependencies are edited; fully reversible with `chimera_potato off`

## Recommended for max FPS

```text
chimera_potato ultra
chimera_interpolate off
chimera_af false
```

+ lower resolution (800x600 / 1024x768)

Rebuild with `build.bat release` or wait for the GitHub Actions artifact.
