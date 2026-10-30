# Independent Liquid Glass optics component

This is **deterministic material work on explicitly supplied app-owned textures**,
not a desktop source, a production UI, or proof that Windows desktop capture works.
The standalone test does not invoke capture. Its shared material and HLSL now also
serve the explicitly opt-in experimental Windows application path.
No capture API, desktop readback, recording, file/image output or network is present.

## What is implemented

- `GlassMaterial`: immutable validated physical-pixel geometry. Rounded-rectangle
  signed distance drives a sinusoidal height ramp, unit normals and orthographic
  single-interface Snell refraction. Displacement is explicitly capped.
- A flat interior and a constrained rim (`0.25 <= rim <= corner radius`) keep
  distance-field medial-axis normal discontinuities out of the refracting region.
- A restrained, fixed-direction Fresnel-like rim highlight. This is a bounded UI
  approximation, **not** physically complete reflection or Apple's private shader.
- Optional nine-tap separable tent blur. This is not Gaussian-quality blur and has
  no claim of large-radius quality. Zero radius performs a single bilinear sample.
- One physical pixel of smooth silhouette coverage blends the complete optical
  result back to the original texel, including blur and highlights. This prevents
  hard discontinuities at the outer boundary; coverage is intentionally DPI-dependent
  at subpixel edges even though the physical geometry scales consistently.
- `GlassReferenceRenderer`: CPU reference operating on an immutable source and a
  distinct output. It is an executable specification/testing oracle, not a proposed
  per-frame CPU renderer for the terminal.
- `overlay`: a separate final foreground pass. Authored opaque glyph-mask pixels
  remain exact; terminal labels must never enter their own optical source.
- `src/platform/windows/liquid_glass/material.hlsl`: matching D3D11 Shader Model 5 pixel material and full-screen
  triangle vertex shader. Native shader compilation and GPU parity are separate gates.

The lens does spatially displace source detail with blur **disabled**. It is not a
blur-only mockup. It is also not a full Liquid Glass system: adaptive luminance/tint,
contextual shadow, interaction/morphing, native AppKit material, desktop-source
freshness, and actual terminal composition remain outside this small component.

## Contracts and bounds

`GlassMaterial::from_dip` converts geometry once for scales 0.5–4. Geometry/sampling
uses physical pixel centers (first texel at 0.5,0.5). All finite/range checks happen
in the material API. Thickness/displacement are capped at 64 px, blur at 16 px,
highlight strength at 0.3, refractive index at 1–2. The source reference is limited
to 4,194,304 pixels and 4096 per axis. Reference source/output together use up to
128 MiB of float RGBA storage; foreground is a separately budgeted input.

Images are **linear-light premultiplied RGBA** with finite 0 <= RGB <= alpha <= 1.
Reference operations validate inputs before output mutation. Off returns an exact
pixel copy without resampling. Outside the surface remains unchanged. Every blur
sample clamps to source texel centers; no undefined/outside texture data is used.
There is no retained frame, clock, random seed or render history.

### D3D11 binding contract

1. Obtain the 64-byte, 16-byte-aligned `ShaderParameters` from the validated material.
   Do not bind arbitrary raw constants: HLSL itself does not validate NaNs, zero
   sizes, invalid rim widths or ranges.
2. `SourceSize` must match source and output extents exactly. Use a zero-origin,
   source-sized viewport, mip zero, distinct source SRV and output RTV. Never bind
   the output as its own source; snapshot the **pre-glass** source each frame.
3. Use the intended linear-light premultiplied data and deliberate SRV/RTV
   decode/encode. RGBA32F is the initial parity fixture format. Bind a linear-clamp
   sampler. Avoid accidental double gamma conversion or alpha premultiplication.
4. Disable blending and enable all RGBA writes for the material pass: the shader
   already produces the complete source-plus-optics result. Blending it onto a
   populated target would double-composite alpha and break Off identity.
   Draw the material triangle with no depth test/culling dependency, then unbind
   the source and render crisp foreground labels in a separate pass with premultiplied
   ONE / INV_SRC_ALPHA blending. Do not sample
   the final foreground/previous glass result on the next frame.
5. Off uses unfiltered `Load`; **GPU bit-exact Off identity is not yet verified**.
   CPU Off identity is tested. Validate actual format conversions and raster coverage.
6. No desktop-source integration until the separate live-background/exclusion test
   passes. Native optics over authored textures can be tested independently.

## Build/test (no Ghostty or app dependency)

```sh
cmake -S tools/probes/glass_optics -B build/glass-optics -DCMAKE_BUILD_TYPE=Release
cmake --build build/glass-optics
ctest --test-dir build/glass-optics --output-on-failure
```

On a Windows SDK developer shell, optionally compile the shaders without running them:

```powershell
cmake -S tools/probes/glass_optics -B build/glass-optics -DSTORMTTY_GLASS_OPTICS_COMPILE_HLSL=ON
cmake --build build/glass-optics
```

`fxc` must be on PATH. Compilation uses strict syntax and warnings-as-errors.

## Evidence and next gates

Portable tests cover: center/outside identity; symmetric inward rim displacement;
finite unit normals and height; displacement/highlight bounds; boundary continuity;
inner-corner seam prevention; explicit UV clamping; blur-only vs displacement-only;
exact Off; source immutability and repeatability/no feedback; resize/fractional DPI;
crisp opaque glyphs; fractional-alpha compositing; transparent source; invalid inputs;
image budgets; shader-constant packing values.

Local validation: GCC 14.2 C++20 warnings-enabled compile passed; CMake/CTest passed;
ASan+UBSan passed with leak detection disabled. LeakSanitizer cannot run in this
ptrace-constrained executor; that is not leak verification. No FXC/DXC is available
here: **HLSL compile, D3D11 execution, CPU/GPU parity, GPU timing and native UI
appearance have not been verified.** No generated images/screenshots were needed.

Next authorized native test should upload only authored grid/ramp/alpha textures,
render to an offscreen target, and compare against this reference with explicit
format-dependent tolerance. Cover 100/125/150/200% scale, moving/resizing geometry,
texture-edge sampling, transparent colors and repeated renders from the same source.
Test Off bitwise where the chosen GPU format permits. Profile GPU work separately;
portable math success is not native performance or Apple fidelity evidence.
