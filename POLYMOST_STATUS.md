# POLYMOST NV2A Renderer - Status (Feb 27, 2026)

## Current State: TEXTURE BUG ISOLATED

The Polymost GL-to-NV2A renderer is functional but has a crash caused by
real game textures. With the null texture forced (white 4x4), 3D geometry
renders without crashing and the player can move around the level.

## What Works
- Vertex shader (vp20 program) runs correctly on NV2A
- MVP with viewport bake produces visible, recognizable geometry
- Index submission via ARRAY_ELEMENT16 works
- Register combiners (tex0 * diffuse) work
- VBO streaming pool works
- Frame swap (pb_reset / pb_target_back_buffer / pb_finished) works
- 2D HUD draws (Frame 0, orthographic) work with real textures
- 3D world draws work with null texture

## The Bug
When real game textures (e.g., 64x128 ARGB8) are bound for 3D world draws,
the GPU throws "object state invalid" (error 0x800) on register 0x1800
(ARRAY_ELEMENT16/INDEX_DATA). The crash occurs on Frame 1's first 3D draw.

### What Was Ruled Out
- **Depth testing**: Forced depth_test=0 for all draws -> still crashes
- **Depth mask**: Forced depth_mask=0 -> still crashes
- **Index format**: Same ARRAY_ELEMENT16 approach works for Frame 0's 91 draws
- **Vertex format**: Same attrib setup works for Frame 0
- **Push buffer overflow**: Sync every 500 draws
- **Shader constants**: Same upload pattern works for Frame 0

### What Fixed It
- Forcing ALL draws to use the null texture (4x4 white, POT, pitch=16)
  eliminates the crash entirely. 3D geometry is visible and navigable.

### Likely Root Cause
Something about larger/real game textures triggers the error. Possibilities:
1. Texture address alignment issue for larger allocations
2. Texture format mismatch (NPOT handling, pitch calculation)
3. Texture memory corruption (write-combined memory fence issue)
4. Texture dimensions exceeding some NV2A constraint
5. The small 2D textures (14x15 etc.) happen to work by luck

## Diagnostic Changes in Current Build
Two forced overrides in `glbuild_xbox.c` xbox_glDrawElements():
1. `if (0 && tex && tex->addr)` - forces null texture for all draws
2. `NV097_SET_DEPTH_TEST_ENABLE, 0` - forces depth test off
3. `NV097_SET_DEPTH_MASK, 0` - forces depth mask off

These must be reverted once the texture bug is fixed.

## Known Rendering Issues (Pre-existing, Fix After Crash)
- **No near-plane clipping**: NV2A PROGRAM mode has no hardware frustum clip.
  Vertices behind camera get inverted by perspective divide (w < 0).
  Shows as "garbage overlaid on real geometry."
- **TEX0.q uninitialized**: Shader outputs float2 tex0, but register combiners
  use 2D_PROJECTIVE (s/q, t/q). TEX0.q may not be 1.0, causing wrong UVs.
- **2D elements missing**: With null texture forced, HUD/sprites are invisible
  (they need their real textures to be visible).

## Key Files
- `jfbuild/src/glbuild_xbox.c` - Main GL shim (~1900 lines)
- `jfbuild/src/sdlayer2.c` - pbkit init, frame swap
- `polymost_vs.vs.cg` / `polymost_vs.inl` - Vertex shader (9 instructions)
- `polymost_ps.ps.cg` / `polymost_ps.inl` - Register combiner setup
- `dn3d_debug.log` - Runtime debug log from Xbox

## Build
```
bash /c/Claude/jfduke3d-xbox/build_xbox.sh
```
Output: `bin/default.xbe`
