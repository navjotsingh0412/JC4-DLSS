# JC4 DLSS - DLSS (DLAA) for Just Cause 4

A [ReShade](https://reshade.me) add-on that runs NVIDIA DLSS (DLAA, native resolution) inside **Just Cause 4**'s own
temporal anti-aliasing pass on the DirectX 11 renderer. It also replaces the game's blurry final FXAA pass, can hide the
motion-blur look, can sharpen the result, and can upgrade the game's texture filtering to 16x anisotropic.
Single-player use only.

> Prebuilt download and screenshots: https://www.nexusmods.com/justcause4/mods/41

## What it does

- Finds the game's temporal-AA compute pass (8x8 thread groups, HDR `R11G11B10` colour, `R32_FLOAT` reversed-Z depth,
  `R16G16_FLOAT` velocity) and evaluates DLSS DLAA on it just before the game's own TAA dispatch. The DLSS result is
  copied over the game's TAA output right after.
- Bypasses the game's final FXAA fullscreen pass while DLSS is active (optionally sharpening with a small
  contrast-adaptive compute shader).
- Optionally hides the game's motion-blur composite (the game's Motion Blur option must stay ON, because the game only
  produces its velocity buffer while that option is on).
- Optionally upgrades the game's trilinear/bilinear texture samplers to 16x anisotropic and applies a LOD bias
  (applies when the game starts).
- Settings panel inside ReShade (Home key, "JC4 DLSS" tab and the "Add-ons" tab). F6 toggles DLSS.

See [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md) for the technical details.

## What it does NOT do

- It does not modify any game file on disk. It runs in the game process through ReShade.
- The add-on itself opens no network connections. It writes `jc4_dlss.ini`, `jc4_dlss.log` and an `ngx_data` folder next
  to `JustCause4.exe`.
- It contains no NVIDIA runtime (`nvngx_dlss.dll`) and no ReShade binaries.

## Requirements (to use it)

- NVIDIA RTX GPU, Just Cause 4 on its DirectX 11 renderer.
- ReShade 6.x **with add-on support**, installed for `JustCause4.exe` as DirectX 10/11/12.
- NVIDIA's `nvngx_dlss.dll` (310.x) next to `JustCause4.exe`, from <https://github.com/NVIDIA/DLSS>
  (`lib/Windows_x86_64/rel/nvngx_dlss.dll`) or via DLSS Swapper.

Install: copy `jc4_dlss.addon64` next to `JustCause4.exe`. In the game set Anti-Aliasing to **Temporal AA** and keep
Motion Blur **ON**. Press Home for the panel.

## Build from source

Requirements: Windows, Visual Studio 2022 Build Tools (MSVC x64, Windows SDK).

```powershell
# 1. Fetch the pinned third-party headers/lib into .\deps (downloads from the official repositories).
#    The NVIDIA files are covered by NVIDIA's RTX SDKs LICENSE; the switch confirms you have read it.
powershell -ExecutionPolicy Bypass -File scripts\get_deps.ps1 -AcceptNvidiaLicense

# 2. Build.
build\build.bat        # produces build\out\jc4_dlss.addon64
```

Pinned versions: ReShade add-on API headers `v6.8.0`, Dear ImGui `v1.92.5-docking`, NVIDIA DLSS SDK `v310.9.16`.
The NVIDIA SDK files, ImGui and ReShade headers are **not** stored in this repository.

A rebuilt binary will not be byte-identical to the released one (compiler timestamps and paths differ). The release
checksums in [release/SHA256SUMS.txt](release/SHA256SUMS.txt) identify the released files only.

## Credits and licenses

Source code in this repository: MIT, see [LICENSE](LICENSE). Third-party components, trademarks and the NVIDIA
end-user terms are in [THIRD_PARTY.md](THIRD_PARTY.md). Not affiliated with or endorsed by NVIDIA, Avalanche Studios or
Square Enix. Most of the code was written by an AI assistant (Claude) under the author's direction and tested in game.
