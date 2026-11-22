# Third-party components and credits

This repository contains only the JC4 DLSS add-on source. It does **not** include ReShade, Dear ImGui, the NVIDIA DLSS /
NGX SDK, or NVIDIA's DLSS runtime. `scripts/get_deps.ps1` downloads the pinned build inputs from their official
repositories.

## Built with / linked against

| Component | Use | License |
|---|---|---|
| NVIDIA DLSS / NGX SDK v310.9.16 (<https://github.com/NVIDIA/DLSS>) | headers and the static library `nvsdk_ngx_d.lib`, linked into the add-on | NVIDIA RTX SDKs LICENSE (see that repository's `LICENSE.txt`) |
| ReShade add-on API headers v6.8.0 (<https://github.com/crosire/reshade>) | add-on events and overlay API | BSD 3-Clause |
| Dear ImGui 1.92.5 docking headers (<https://github.com/ocornut/imgui>) | settings panel (through ReShade's function table) | MIT |

DLSS is a trademark of NVIDIA Corporation.

## End-user terms for the NVIDIA components built into the released add-on

The released `jc4_dlss.addon64` contains object code from the NVIDIA DLSS / NGX SDK. By using it you agree to terms at
least as protective of NVIDIA as the NVIDIA RTX SDKs LICENSE:

1. You may not reverse engineer, decompile or disassemble the NVIDIA SDK portions, or remove NVIDIA's copyright or
   proprietary notices.
2. You may not sell, rent, sublicense or redistribute the NVIDIA SDK portions as a stand-alone product.
3. You may not state or imply that this add-on is sponsored or endorsed by NVIDIA.
4. NVIDIA and its suppliers keep all rights in the SDK. No rights are granted other than to use it as part of this add-on.
5. The NVIDIA SDK portions are provided AS IS, with no warranty. To the maximum extent allowed by law, NVIDIA and its
   affiliates are not liable for any damages arising from their use.
6. You must follow applicable export laws and regulations, and not use this add-on in life-critical systems.
7. Your rights end automatically if you do not comply with these terms.

NVIDIA's DLSS runtime (`nvngx_dlss.dll`) is not part of this project; it is governed by NVIDIA's own license.

## Research and inspiration (no code copied)

- The Luma Framework's Just Cause 3 mod by Pumbo / Filoppi (<https://github.com/Filoppi/Luma-Framework>) was read for ideas
  about jitter and sampler handling. Its license (MIT with attribution and commercial-use addenda) applies to its code;
  nothing from it is included here.
- NVIDIA's DLSS Programming Guide, for the general integration requirements.
