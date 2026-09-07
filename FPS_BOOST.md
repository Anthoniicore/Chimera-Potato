# Chimera FPS Boost

Target: Halo Custom Edition 1.10, Chimera600-derived fork.

## Commands

```text
chimera_fps_boost 0
chimera_fps_boost 1
chimera_fps_boost 2
chimera_fps_boost 3
```

## Profiles

- **1:** AF off, firing particles off, optional normal zoom blur and multitexture overlays off.
- **2:** level 1 + BSP detail/bump maps removed and shader lightmap detail forced to the lowest level.
- **3:** level 2 + BSP base maps removed and water maps/brightness removed.

The module does not touch transparent shader tags or the alpha render target, preserving Active Camo.
Player/weapon shader model base maps remain intact.

The tag dependency patches are stored and restored when switching back to level 0 or another profile on the same map. On a map change, old patch addresses are discarded and the active profile is applied to the new tag array.

## Build

Run `build.bat` from the project root with the same MinGW32 environment used by the rest of Chimera600. The batch file now compiles `client/enhancements/fps_boost.cpp` and includes it in the wildcard link step.
