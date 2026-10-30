# Deferred Liquid Glass HDR design

Research snapshot: 2026-10-30. **Paused at the user's request; no HDR implementation or Windows runtime validation.** The user wants Liquid Glass disabled for now and code refactoring prioritized. This note is not approval to resume implementation.

## Source-informed findings

The current `liquid_glass/capture_source.cpp` rejects HDR and uses BGRA8 WGC frames/crops. `material_pass.cpp` uses sRGB SRV/RTV views of BGRA8 resources. `session.cpp` returns BGRA8. `renderer.cpp` wraps the result in a BGRA8 D2D bitmap brush, paints into a BGRA8 canvas, and copies that canvas to a BGRA8 swapchain. Removing the SDR guard or changing capture alone would not preserve HDR.

Microsoft recommends FP16 throughout HDR capture to avoid overclipping: [Windows Graphics Capture](https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture).

For genuine HDR output, use `R16G16B16A16_FLOAT` capture, crop, optics intermediates, and final presentation; process in linear scRGB, without sRGB views or RGB saturation. Preserve negative gamut components and values above 1. FP16 flip-model presentation with `RGB_FULL_G10_NONE_P709` is the general-purpose path, including alpha composition. Existing UI colors require conversion, not simply an FP16 canvas substitution. [Microsoft Advanced Color guidance](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range).

## Recommended architecture when resumed

- Preserve the existing normal renderer when glass is disabled. Introduce an explicit HDR glass output mode rather than changing all rendering implicitly.
- Render existing SDR UI separately, then GPU-composite it over the FP16 glass into an FP16 presentation target. For an encoded premultiplied SDR layer: unpremultiply, decode sRGB, scale linear RGB by SDR-white/80, premultiply, then linear source-over blend; do not scale alpha. A separate background DComposition visual is another possibility, but requires validation of clipping, masks and alpha semantics.
- Preserve panel/layout masks: current glass is a bitmap brush used within UI fills, not merely a full-window underlay.
- Do not apply SDR-white scaling a second time to captured HDR desktop content. New UI whites and glass highlights need an explicit reference-white policy; current highlight interpolation toward RGB=alpha assumes normalized SDR white.
- Query the selected output's SDR white through `QueryDisplayConfig` / `DisplayConfigGetDeviceInfo`; `SDRWhiteLevel / 1000` is the linear multiplier relative to 80 nits. [DISPLAYCONFIG_SDR_WHITE_LEVEL](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_sdr_white_level).
- Refresh display capabilities and white level on changes; retain fail-closed behavior on unknown mode, output identity/geometry changes, cross-adapter movement, device loss, or unavailable supported presentation. Keep explicit restart/consent requirements.

## Smaller but different deliverable

HDR-compatible **tone-mapped SDR** could keep the existing BGRA8 output: capture/process FP16, tone-map and gamut-map on GPU, normalize for SDR output white, then encode once to sRGB. This loses HDR highlights and is not HDR-preserving output. DWM boosts SDR presentation surfaces in HDR mode; avoid double boosting. Microsoft documents [HDR tone mapping](https://learn.microsoft.com/en-us/windows/win32/direct2d/hdr-tone-map-effect) and the [white-level adjustment matrix](https://learn.microsoft.com/en-us/windows/win32/direct2d/white-level-adjustment-effect). Do not substitute float-to-UNORM clipping for tone mapping.

## Risks and future acceptance evidence

FP16 doubles per-pixel capture/intermediate bandwidth and storage; address existing drag latency before expanding capture cost. WGC luminance behavior, SDR-white changes, mixed SDR/HDR content, non-sRGB colors, monitor transitions, HDR toggles, D2D foreground conversion and transparent composition all need physical Windows/HDR validation. Test neutral SDR white against surrounding desktop, HDR highlight detail, saturated wide-gamut content, and unchanged SDR rendering. No such validation has been performed here.

Retain off-by-default session consent, visible capture border, verified self-exclusion, bounded GPU resources, and all desktop-content readback protections. Never introduce CPU desktop readback, storage or upload, including for tone-map analysis.
