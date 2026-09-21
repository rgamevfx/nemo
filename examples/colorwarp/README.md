# ColorWarp — an installed Nemo effect package

ColorWarp is an **external** effect package for Nemo: a 12-spoke × 3-ring colour
wheel whose control points displace a pixel's hue and saturation, with the drawn
mesh being the deformation the renderer actually applies. It is the reference
example of the installed-package path (issue #37): it registers nothing in the
built-in node inventory, links no host target, and is discovered only from a
package root.

| | |
| --- | --- |
| Package id | `org.nemo.colorwarp` |
| Node type | `org.nemo.colorwarp` (`ColorWarp`, group `Color`) |
| Manifest format | `1` |
| Package version / state version / processing version | `1` / `1` / `1` |
| API | `{ "minimum": 1, "maximum": 1 }` |
| Capabilities | `nemo.effect.pointwise.v1`, `nemo.ui.qml.v1` |
| GPU binding contract | `nemo.native.bindings.v8` |
| Payload layout | `org.nemo.colorwarp.payload.v1`, 592 bytes |
| Native entry point | `nemo_effect_v1()` (ABI 1, `src/nemo/extensions/EffectAbi.h`) |
| Dependencies | none |

## Contents

```
manifest.json                  the package's declaration (reviewed source copy)
src/ColorWarpSchema.hpp        wheel topology, stable point indexing, key names
src/ColorWarpMath.hpp          the CPU reference math and the fold criterion
src/ColorWarpLibrary.cpp       ABI 1 entry point: validate / process / prepare
shaders/ColorWarp.slang        the kernel, Slang -> SPIR-V
shaders/ColorWarp.glsl         the complete reference GLSL kernel
qml/ColorWarpEditor.qml        the registered wheel editor (section presentation)
qml/ColorWarpPanel.qml         the example panel contribution
CMakeLists.txt                 standalone build, package assembly, install rules
```

## Build and install

Requirements: a C++20 compiler, CMake ≥ 3.28, `nlohmann_json` (the repository's
pinned vcpkg manifest entry), a Nemo source checkout, and `slangc` for the
kernel.

```bash
# Run from the Nemo checkout with VCPKG_ROOT pointing to its vcpkg checkout.
# The root application build does not add this separate package.
cmake -S examples/colorwarp -B build/colorwarp \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
      -DVCPKG_MANIFEST_DIR="$PWD" \
      -DNEMO_DOWNLOAD_SLANGC=ON
cmake --build build/colorwarp --target colorwarp_package
cmake --install build/colorwarp --prefix "$HOME/.local"
```

`slangc` is resolved by the repository's own shader toolchain
(`cmake/NemoSlangShaders.cmake`), in the same order as the rest of the project:
`-D NEMO_SLANGC=<path>`, then `slangc` on `PATH`, then
`-D NEMO_DOWNLOAD_SLANGC=ON` (downloads the pinned Slang release at configure
time). Configuration fails without a shader compiler: this package declares a
native GPU implementation and must not install without `ColorWarp.spv`.

Useful CMake variables:

| Variable | Default | Meaning |
| --- | --- | --- |
| `NEMO_COLORWARP_SDK_DIR` | `../..` (this checkout) | Nemo source SDK the package compiles against |
| `NEMO_COLORWARP_PACKAGE_ROOT` | `<build-dir>/package` | root whose direct child `colorwarp` is the assembled package |
| `NEMO_COLORWARP_INSTALL_DIR` | `share/nemo/extensions/colorwarp` | package folder, relative to the install prefix |

The package uses `src/nemo/extensions/EffectAbi.h` (the C ABI),
`src/nemo/nodes/GpuGeometry.slang` (the shared binding support), and
`cmake/NemoSlangShaders.cmake` from the selected SDK. A copied example can build
outside the checkout by setting `NEMO_COLORWARP_SDK_DIR` and
`VCPKG_MANIFEST_DIR` to that checkout; the shared shader function supplies the
SDK include directory.

### What is installed

```
<prefix>/share/nemo/extensions/colorwarp/
    manifest.json
    libnemo_colorwarp.so        (nemo_colorwarp.dll / libnemo_colorwarp.dylib)
    ColorWarp.spv
    ColorWarp.glsl
    qml/ColorWarpEditor.qml
    qml/ColorWarpPanel.qml
```

The installed `manifest.json` is generated from the source copy at configure time
so its `library` entry is spelled the way the platform actually produces the
shared library (`CMAKE_SHARED_LIBRARY_PREFIX`/`SUFFIX`); the source copy is the
reviewed declaration and keeps the Linux spelling. Nothing else is generated —
the loader reads `node.parameters` directly, so no separate schema artifact
exists or is installed.

## Use

Put the package's **parent** directory on the extension path and the loader finds
it as a child package folder:

```bash
export NEMO_EXTENSION_PATH="$HOME/.local/share/nemo/extensions"
```

The default root is `$XDG_DATA_HOME/nemo/extensions` (`$HOME/.local/share` when
`XDG_DATA_HOME` is unset); on Windows it is `%LOCALAPPDATA%/Nemo/extensions` and
the separator is `;`. `$HOME/.local` as a prefix lands in the default root
exactly. Nothing is ever loaded from the current directory or from a project.

Inside Nemo, add a `ColorWarp` node from the `Color` group. Its inspector shows
one section control — the wheel — plus the shared numeric control for `Strength`.
The wheel draws every control point at the mapping of its own knot and samples
the same smoothstep-tensor field along every edge, so what is drawn is what is
rendered. Pin a point to protect it from dragging; `Reset All` clears pins like
any other authored value. A shape the fold criterion refuses is never shown and
never published: the editor drops the refused gesture and continues from the last
accepted values, and the host's validator is the final authority.
Shared numeric previews reach the mesh through the host's
`panel.editPreviewed(token, values)` notification. The editor follows accepted
values only, scoped to its target and live token; ordinary host refresh restores
committed values after cancellation or commit.

The example panel `ColorWarp Extension Example` is a demonstration of a
`panels` contribution (a static panel body with the host's injected properties);
it has no window or chrome of its own and does not touch the renderer.

## The math contract

**Encoding** (scene-linear RGB, no clamp, negatives and HDR included):

```
Y = .2126 R + .7152 G + .0722 B
u = (2R - G - B)/sqrt(6)      v = (G - B)/sqrt(2)
c = hypot(u, v)               s = 1 + |Y|
radius = c/(s + c)            hue = atan2(v, u)/(2pi) mod 1
```

**Cell coordinates.** `U = 12*hue` is periodic over `[0, 12)` (one cell = 1/12
turn) and `V = 4*radius` spans `[0, 4]` (one cell = 1/4 radius). Knots are
`(h, r)` for spokes `h = 0..11` and rings `r = 0..4`; the three editable interior
rings `r = 1..3` carry the stable index `i = (r-1)*12 + h`; the centre `r = 0`
and the outer boundary `r = 4` are fixed at zero displacement.

**Mapping.** `(U, V) -> (U, V) + strength * D(U, V)`, where `D` is the tensor
interpolation of a cell's four knot displacements with cubic smoothstep weights
`t*t*(3-2t)`. Smoothstep has zero slope at both ends, so the field is C1 across
every cell edge and periodic across the seam — the drawn curves sample this same
field, not decorative Béziers.

**Decoding** re-uses the pixel's own luma, so the mapping never changes `Y`:

```
complement' = s/(s + c) - strength * D_v/4        (= 1 - radius')
chroma'     = s * (1 - complement')/complement'
u', v'      = chroma' * cos/sin(2pi * hue')
q           = (sqrt(2/3)*u', -u'/sqrt(6) + v'/sqrt(2), -u'/sqrt(6) - v'/sqrt(2))
RGB'        = q + (Y - dot(lumaWeights, q))
```

`hue{i}`/`saturation{i}` are **displacements** in cell coordinates, not absolute
positions: a `hue` displacement of 1 moves a knot by one spoke, a `saturation`
displacement of 1 by one ring. Neither declares a hard range — the fold criterion
below is the only semantic constraint on a coordinate.

**Why the complement, not the radius.** `c/(s+c)` saturates at exactly `1` for a
large finite chroma, so `s*radius/(1-radius)` would divide by zero and report an
infinity for a perfectly representable input; `s/(s+c)` stays exact where the
radius saturates, and the inverse reads it directly. A pixel whose interpolated
displacement is exactly zero — the neutral axis, and every point of the fixed
boundary rings — is returned **unchanged**, so the identity is exact rather than
a round trip through the opponent basis. A hue displacement is carried modulo the
wheel's own period, which is the same mapping exactly (turns are modulo one) and
keeps the float payload's phase resolvable for any authored magnitude.

### Fold criterion (the validator)

For **every** cell `(spoke h, ring r)` and **both** displacement components:

```
1.5 * ( max(|right-bottom - left-bottom|, |right-top - left-top|)
      + max(|left-top - left-bottom|, |right-top - right-bottom|) )  <  0.95
```

Because the interpolation weights are a convex combination and smoothstep's slope
is at most 1.5, this bounds the Jacobian perturbation of the whole field by less
than 1 in the row-sum norm. With the boundary rings fixed, the mapping is
therefore injective *and* surjective on the wheel cylinder: it cannot fold, and a
mapped radius can never reach 1. The criterion is conservative and is applied to
the resolved effective parameters, so an animated coordinate is validated exactly
like an authored one. A refused mesh produces a diagnostic naming the cell and
component (`mesh folds: cell (spoke 3, ring 1) hue Jacobian bound 1.2 …`), and
`validate`, `process` and `prepare` all run it: a mesh that was never validated
can never be executed, and `prepare` in particular refuses before any payload
exists.

`validate` also refuses a strength outside the declared `[0, 1]` range, a
non-finite value, and a displacement the 592-byte float payload cannot carry.

## Execution contract

**CPU** (`process`): bulk interleaved scene-linear RGBA in **double** precision,
one warp per pixel. `flags` bit 0 (`NEMO_EFFECT_PREMULTIPLIED`) makes the sample
premultiplied and the RGB is unassociated and re-associated explicitly; a sample
whose alpha is exactly zero keeps its own values (there is no unassociated colour
to warp and none is invented). `flags` bit 1 (`NEMO_EFFECT_BYPASS_COLOR`) is the
host's decision not to colour-process the raster — an explicit bypass, and a
data-only or incomplete-RGB image, whose meaning a colour effect must not invent;
the output is then a byte-for-byte copy. Alpha is never warped.

**GPU** (`prepare` + kernel): `prepare` writes exactly the declared 592 bytes —
`float4 control[36]` of `(hue displacement, saturation displacement, 0, 0)` in
stable index order, then `float4 settings = (strength, flags, identity, 0)`,
where `identity` is 1 when every displacement is zero or `strength` is zero. The
kernel implements the same contract independently in float, and preserves every
auxiliary channel the pass does not write at unchanged coordinates through the
shared channel plan (`gpuStorePixel`), so a mask, normals or data layer survives
the warp.

### CPU and GPU agreement, and the overflow limit

The CPU reference and the kernels are independent implementations of one
contract (ADR-0004), not shared code, so their agreement is evidence rather than
a shared mistake. On the analytic fixtures below they agree well inside the
project's 2e-5 absolute + 2e-5 relative tolerance.

The one deliberate, documented divergence is at the **float32 representability
limit**, and it is a representability limit, not a colour-range policy:

* **CPU** — `process` returns nonzero with a bounded diagnostic naming the pixel
  when the mapped position has no representable value (the mapped complement
  reached zero or below) or when the result does not fit a finite float32. It
  never clamps, never substitutes a colour and never quietly keeps the sample.
* **GPU** — the kernel has **no per-pixel host status channel**. It writes the
  exact IEEE result, so a true float32 overflow becomes `±Inf` and an
  unrepresentable mapped position becomes a `NaN` marker. It does not fabricate
  the input, and it does not clamp.
* **Both paths agree on a non-finite sample.** A pixel whose input is not a
  number has no colour to map, and neither path invents one: both write the same
  `NaN` marker (alpha is carried as always). This is propagation of an undefined
  input, not a substitution.

This is distinct from ordinary HDR and negative-value support, which is fully
carried (nothing in the pipeline clamps): it needs an input at the very edge of
float32 range, or a mapped radius at the wheel's own limit. Closing the gap would
require a device readback of per-pixel status, which is out of this package's
scope; the marker values are what a host can observe today. The mesh-level gate
is not affected — `prepare` refuses an inadmissible mesh before any payload
exists, on both paths.

## Verification

Build and install the separate package first, then run the host's public
contract tests against that installed root:

```bash
cmake --build --preset debug --target nemo_extension_tests nemo_workspace_ui_tests
NEMO_TEST_EXTENSION_ROOT="$HOME/.local/share/nemo/extensions" \
  build/debug/tests/nemo_extension_tests --gtest_filter='ExtensionTest.*'

# From a native Wayland desktop session:
NEMO_TEST_EXTENSION_ROOT="$HOME/.local/share/nemo/extensions" \
NEMO_TEST_NATIVE_UI=1 NEMO_TEST_VIEWER_WINDOW=1 QT_QPA_PLATFORM=wayland \
  build/debug/tests/nemo_workspace_ui_tests --gtest_filter='*Issue37*'
```

`tests/ExtensionTests.cpp` contains independently derived literal color
fixtures: knots, interpolated cells, the periodic seam, neutral and signed/HDR
inputs. CPU, Slang and GLSL are compared to those fixtures, not just to each
other. Further cases cover exact identity, strength, luminance/alpha retention,
Data RGB, premultiplication, incomplete primary RGB, fold refusal, command
history, animation, save/reopen, admission failures, state compatibility and
in-flight library ownership.

`tests/ExtensionUiTests.cpp` loads the real application QML on native windows:
catalog creation, wheel selection/drag, shared cancellation/history, pin/reset,
strength, keying, project reopen, and the shared panel shell including missing
and restored package layouts. Set `NEMO37_EVIDENCE_DIR` to retain screenshots
and the authored project. Human image approval is separate from these checks.

The fixed center and outer boundary participate in validation: equal
displacements on all editable rings are not a constant field at those
boundaries. In particular, setting every hue displacement to `1` violates the
fold bound; it is not an admissible global hue-rotation fixture.

## Trust and safety

This package is **trusted installed native code**, not a sandbox. It is
deliberately small so that what it can do is auditable:

* The shared library exposes one C ABI entry point, `nemo_effect_v1()`, which
  returns immutable process-lifetime metadata. It references no host symbols
  and links neither Qt nor Vulkan.
* The three callbacks are stateless and reentrant, so they may run concurrently
  on worker threads; they hold no mutable global state, capture nothing and
  allocate only their own parameter parsing.
* Every callback catches every exception (including `...`) and reports failure as
  a nonzero return plus a NUL-terminated diagnostic, bounded to the host's own
  buffer capacity and never overrunning it.
* `nlohmann::json` is used privately for the resolved parameter object; no JSON
  object, string or allocation crosses the ABI, and the host owns every buffer.
* The only side effect is writing the output pixels, the payload, or the
  diagnostic buffer it was handed. The package opens no file, starts no thread
  and performs no I/O.
* The loader validates the manifest, schema, capabilities, dependencies,
  identity, versions and the existence of every declared file **before** it
  loads the library; a refused package is a diagnostic, not an application
  failure.

## Contributor notes

* **Topology lives in one place.** `src/ColorWarpSchema.hpp` owns the spoke/ring
  counts, the stable point indexing and the parameter key names; the manifest's
  `node.parameters` array is the declaration of the same 109 keys (`strength`,
  `hue0..35`, `saturation0..35`, `pin0..35`). Changing the topology means
  changing both, and the payload size follows from it (`36*16 + 16 = 592`).
* **`pin{i}` never reaches execution.** It is UI protection on the authored
  point: serialized, undoable, cleared by `Reset All`, and deliberately not read
  by the native code.
* **The GLSL front end is a copy.** The generic binding blocks at the top of
  `shaders/ColorWarp.glsl` are a verbatim copy of the host's shared front end
  (`nemo::nodes::kGpuRequestGlsl`/`kGpuInputGeometryGlsl`/`kGpuChannelImageGlsl`
  in `src/nemo/nodes/GpuCommon.hpp`), because an installed package cannot call
  host C++. When that shared contract changes, the copy moves with it — the
  Slang kernel includes the SDK file directly and needs no copy.
* **Adding a parameter** means declaring it in `manifest.json`, reading it in
  `ColorWarpLibrary.cpp`, carrying it in the payload and the kernel (which is a
  new payload layout identity, not a new file), and — if the editor should own it
  — adding it to the editor's `consumes`.
* **Keep the three implementations independent.** The CPU reference, the Slang
  kernel and the GLSL reference are three readings of one contract. Do not
  factor the wheel math into shared code between them: their agreement is the
  evidence.

### Platform status

Developed and exercised on Linux. The manifest's library name, the CMake targets
and the loader's own path handling are platform-aware, and the kernel and
reference sources are platform-independent, but **Windows and macOS runtime
behaviour has not been tested** and is not claimed here.
