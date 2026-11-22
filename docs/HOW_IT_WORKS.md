# How JC4 DLSS works

Just Cause 4 (D3D11) renders its temporal AA as a compute shader (8x8 thread groups, dispatch of ceil(W/8) x ceil(H/8)):

| Slot | Content |
|---|---|
| t0 | HDR scene colour, R11G11B10 |
| t2 | depth, R32_FLOAT, reversed-Z |
| t4 | velocity, R16G16_FLOAT (UV units, current minus previous) |
| u0 | output, R11G11B10 |

After it the game runs a motion-blur composite pass and an FXAA pass.

## Hooking

The add-on uses ReShade's add-on events (`dispatch`, `draw`, `draw_indexed`, `push_descriptors`, `bind_render_targets`,
`present`, `create_sampler`, `reshade_overlay`). It tracks SRV/UAV bindings per slot, recognises the TAA dispatch by its
group counts and resource formats, and, just before that dispatch, evaluates NGX DLSS (DLAA, native resolution) from
t0/t2/t4 into its own R11G11B10 texture. That result is copied over u0 afterwards.

- Motion vectors: the game writes velocity only while its Motion Blur option is on, so that option must stay on. The
  motion-blur composite is bypassed by a copy pass so the blur is not visible.
- Jitter: the game's two-phase jitter is folded into the camera matrix. The add-on passes DLSS a best-effort jitter.
- FXAA is bypassed by a copy pass; an optional contrast-adaptive sharpen runs instead.
- Samplers are upgraded to anisotropic at creation time.
- Compute state is saved and restored around NGX calls. No lock is held across NGX/D3D calls.

## Known limits

Native-resolution DLAA only (no upscaling modes), no Ray Reconstruction, no Frame Generation, jitter fed to DLSS is
approximate. Single-player only.
