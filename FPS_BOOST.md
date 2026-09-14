# Chimera Potato (improved FPS mode)

Target: Halo Custom Edition 1.10 – Chimera600-derived fork.

This is a modern, safer recreation of the old `chimera_potato` feature that existed in early Chimera builds.  
It is designed to give the largest possible FPS increase on low-end / “potato” PCs while **never touching Active Camo**.

## Commands

```text
chimera_potato [off/low/medium/high/ultra]
chimera_potato [0-4]

chimera_fps_boost [same arguments]     # alias
```

## Levels

| Level   | Name    | What it does                                                                 | Camo safe? |
|---------|---------|------------------------------------------------------------------------------|------------|
| 0       | off     | Restores all visual changes                                                  | Yes        |
| 1       | low     | AF off + firing particles off + multitexture overlays off                    | Yes        |
| 2       | medium  | low + strip BSP detail/bump maps + force lowest lightmap detail              | Yes        |
| 3       | high    | medium + strip BSP base maps + remove water textures/brightness              | Yes        |
| 4       | ultra   | high + extra model detail stripping (player/weapon **base** maps kept)       | Yes        |

### What is **never** touched
- Any transparent shader tag (chicago, chicago_extended, etc.)
- The alpha render target (required for Active Camo distortion)
- Player / weapon base maps (so characters stay recognizable)
- Gameplay, magnetism, hit registration, object tables, interpolation

Only visual tag dependencies are modified, and they are fully restored when you go back to `off` or change map.

## Recommended usage for maximum FPS

```text
chimera_potato ultra
chimera_interpolate off          # or low
chimera_af false
chimera_block_firing_particles true
# + lower resolution (e.g. 1024x768 or 800x600)
```

On very weak GPUs (old integrated graphics, low VRAM cards) the combination of `ultra` + low resolution + interpolation off can easily give 2×–3× FPS or more compared with stock Chimera settings.

## Build

Just run `build.bat` as usual. The potato module is already included in the link step.
