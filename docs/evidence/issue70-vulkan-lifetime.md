# Issue 70: Mesa EGL query-only device-list leak

Related: [#70](https://github.com/rgamevfx/nemo/issues/70),
[#67](https://github.com/rgamevfx/nemo/issues/67), roadmap
[#24](https://github.com/rgamevfx/nemo/issues/24).

Retained proof: [session and exact commands](assets/issue70-vulkan-lifetime/session.json),
[allocation stacks](assets/issue70-vulkan-lifetime/allocation-stacks.txt),
[complete CTest log](assets/issue70-vulkan-lifetime/asan-full.log.gz), and
[complete JUnit results](assets/issue70-vulkan-lifetime/asan-full.xml.gz).

## Finding and resolution

Mesa EGL 26.1.6 allocates its device list through `eglQueryDevicesEXT` without
registering the existing cleanup handler. `eglInitialize` registers that handler,
but a query-only client need not initialize a display. Unloading the vendor loses
one `_EGLDevice` (104 bytes) owning a libdrm `drmDevice` (144 bytes) on this machine.
This is a host-heap lifetime defect, not a VRAM measurement or a Nemo resource leak.

The [one-line patch](assets/issue70-vulkan-lifetime/mesa-egl-query-cleanup.patch)
registers `_eglAtExit` in the public `eglQueryDevicesEXT` wrapper before dispatch.
Registration is idempotent and occurs outside the internal query's nonrecursive
`_eglGlobal.Mutex`. Existing `_eglFiniDevice` then frees the owned DRM device and
list node at teardown. No new cleanup mechanism or Nemo-side workaround is added.

The unmodified debug-symbol build reproduced the same 248-byte failure. The
otherwise-identical patched build passed the standalone Vulkan and EGL probes,
11/11 native GPU tests, and the complete native Linux ASan+UBSan suite:
**505 discovered, 505 passed, zero failures, zero disabled, zero skips**. This
includes every family that previously failed at process exit (GPU bootstrap and
submission, effects, color, hardware media, viewer and viewer cache): none
remains failing under the repaired selection. Debug/release GPU compatibility
subsets passed 11/11 each. No application or application-build code changed; the
prior #67 full debug/release results are not presented as new full-suite runs.
Runs without the private selection still fail 61/505 at process exit, so this
number is not a property of the unmodified installed stack.

The probes distinguish failure causes by exit code so a red leak run and a
misconfigured environment cannot be confused: `0` clean, `1` leak reported by
the sanitizer, `2` invalid usage, `3` environment failure (no usable Vulkan ICD,
or libEGL/symbol unavailable), `4` unexpected enumeration result or lost device
count.

**Deployment scope:** the repaired library is selected only for explicitly
launched processes. Installed Mesa, NVIDIA, Vulkan, libdrm and system configuration
remain unchanged. Plain `ctest --preset asan` without the private library still
uses the defective installed Mesa. This is a demonstrated, reproducible local
upstream-source repair, not an installed package update or an upstream release.
This report and patch are retained for upstream submission; neither was submitted
upstream by this work. No issue closure or push is implied.

## Allocation ownership evidence

The call path captured at allocation time is:

```text
vkCreateInstance
  Vulkan loader -> NVIDIA ICD negotiation -> GLVND EGL device query
    Mesa eglQueryDevicesEXT                 eglapi.c:2712 (unmodified)
      _eglQueryDevicesEXT                   egldevice.c:476
        _eglDeviceRefreshList               egldevice.c:448
          _eglAddDRMDevice -> calloc(104)    egldevice.c:151
```

`_eglDeviceRefreshList` calls `drmGetDevices2` at `egldevice.c:441`; the accepted
DRM device becomes `dev->device`. `_eglFiniDevice` owns both frees
(`egldevice.c:82,87`). `_eglRegisterAtExit` registers `_eglAtExit` at
`eglglobals.c:118`; the missing caller is the query-only wrapper.

A temporary sanitizer allocation hook captured stacks while the modules were
still mapped. The installed allocating frame is
`libEGL_mesa.so.0+0x1f53d`, build ID
`1edc731c3d85674bc3ec19b086ce2e1f63e87e85`. In the unmodified debug build,
`addr2line` resolves `0x21870`, `0x22147`, `0x22233`, `0x1d14d` to the four Mesa
functions above. The hook's 104-byte pointer matches LSan's leaked-object address.
The post-fix hook still observes Mesa allocating the object, but LSan reports no
leak. No preload/pinning of the vendor or prevention of unload was used to pass.
Final-at-exit stacks can name an unloaded address incorrectly; the conclusion
uses live module identity plus offline source symbolization, not those names.

Upstream source authority (26.1.6):
[eglapi.c](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.1.6/src/egl/main/eglapi.c),
[egldevice.c](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.1.6/src/egl/main/egldevice.c),
[eglglobals.c](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.1.6/src/egl/main/eglglobals.c).
The actual build uses the checksum-pinned Pop source package, not moving `main`.

## Controlled observations

| Experiment | Unmodified | Patched |
| --- | --- | --- |
| One create/destroy, no physical-device query | 248 bytes leaked | exit 0 |
| One instance, 100 physical-device queries | 248 bytes leaked; two devices | exit 0; two devices |
| Ten create/query/destroy cycles | 2,480 bytes leaked; two devices each cycle | exit 0; two devices each cycle |
| Mesa-only public EGL query and `dlclose`, no Vulkan or NVIDIA vendor | 248 bytes leaked | exit 0; two EGL devices; no suppression file |
| `Gpu.BootstrapCreateDestroy` | assertions passed, LSan failed | passed |

NVIDIA-only Vulkan ICD selection previously failed because it does not exclude
Mesa's EGL vendor: NVIDIA's ICD calls GLVND EGL, which enumerates both vendors.
Disabling the Mesa selection layer or all implicit Vulkan layers also failed.
NVIDIA-only **EGL** selection passes, but merely bypasses the offending vendor and
is not the repair or acceptance environment. Native acceptance retains NVIDIA
and the repaired Mesa EGL vendor, the system Vulkan ICDs/layers, and the unchanged
`tests/lsan_suppressions.txt`. Leak detection remains enabled. Device inventory
still contains the NVIDIA GeForce GTX 1070 and system llvmpipe; Nemo's existing
device owner prefers the discrete GPU.

### Extended-cycle limitation, not hidden as a pass

With default glibc TLS settings, a 25/100-cycle stress run loses NVIDIA after
18 successful cycles: the loader reports `libnvidia-tls.so.580.173.02: cannot
allocate memory in static TLS block`, ignores that ICD, and only llvmpipe remains.
This happens with both unmodified and patched Mesa. The retained probe now fails
when the device count changes. The initial 100-cycle process's exit 0 is **not**
100-cycle native-GPU acceptance. The accepted growth comparison is ten cycles,
with two devices throughout.

An exploratory unmodified 100-cycle run with
`GLIBC_TUNABLES=glibc.rtld.optional_static_tls=32768` and slow sanitizer unwinding
timed out after 300 seconds; its partial stdout was not retained by that runner.
No result or resolution is claimed for that run, and the full native suite did
not use this tunable. Fixing the separate extended-load TLS limit is not included
in the Mesa device-list patch.

### Retained build script rerun

The [build script](assets/issue70-vulkan-lifetime/build-mesa.py) was executed
from a fresh work directory to prove the procedure, not only the resulting
library, reproduces the result. It was run twice: the first cold run completed
in 154.09 seconds, and the rerun after the script gained explicit pinned-tool
verification took 68.98 seconds with warm package caches. Both runs produced
byte-identical stages, which is the expected result for a fixed source,
configuration and toolchain. The runs produced
`stage-unmodified/libEGL_mesa.so.0.0.0` (SHA256
`1bd630ce2a6a03695bff5314607b68390d38dbea9e1d234442326b437563835b`, build ID
`e52ba0d0232f789f379406368e91dccf9a6026e3`) and
`stage-fixed/libEGL_mesa.so.0.0.0` (SHA256
`c87333f2725f14ec0679478dc251a26fbed2ee6be1896689762ccecb2c1658cd`, build ID
`d0967b2ba78076ee96f2e3b12b835c42ff8291e2`). Hashes differ from the initially
acceptance-tested library because DWARF records build and source paths; the
patch, source version, Meson configuration and installed runtime dependency set
are the same, and the emitted `DT_NEEDED` set is byte-identical.

Against those artifacts, the unmodified library leaked 248 bytes, the patched
library passed ten create/query/destroy cycles with two devices throughout, the
EGL-only probe exited 0 with two devices, and the patched runtime passed the
native `Gpu.` subset 11/11 in 17.25 seconds. The full 505-test native suite was
run against the earlier library, which shares the same sources and configuration;
it was not rerun because no behavior changed.

## Minimal upstream reproduction without Vulkan

The [EGL-only probe](assets/issue70-vulkan-lifetime/egl-query-probe.cpp) loads
GLVND EGL, calls public `eglQueryDevicesEXT`, and closes the library without
initializing a display. On a machine with a DRM render node:

```bash
g++-13 -std=c++20 -fsanitize=address,undefined -g \
  docs/evidence/assets/issue70-vulkan-lifetime/egl-query-probe.cpp -ldl \
  -o /tmp/mesa-egl-query-probe
__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json \
  ASAN_OPTIONS=detect_leaks=1 /tmp/mesa-egl-query-probe
```

This reproduces the 104+144-byte leak without loading the NVIDIA EGL vendor,
linking Vulkan, or using any suppression file. The EGL probe accepts no arguments
and exits 3 when libEGL or the extension is unavailable, so a missing vendor
manifest is not read as a leak result. Selecting the patched Mesa
manifest instead exits 0 with the same two EGL devices. This query/unload case
is the proposed upstream regression seam; the Vulkan probe retains Nemo's
original external-stack reproduction.

## Reproduce the isolated repair

The exact tested machine is Pop!_OS 24.04 amd64, GCC 13.3.0, libasan8/libubsan1
14.2.0, glibc 2.39, Vulkan loader 1.3.280, NVIDIA 580.173.02, Mesa
`26.1.6-1pop0~1787580452~24.04~a5619ea`, and libdrm 2.4.134.
[Build inputs](assets/issue70-vulkan-lifetime/build-inputs.json) pin source,
extracted development packages, Meson wheel, and installed Mesa/gallium identity.
The script requires the existing Nemo development environment, Python 3.11+
with `venv`, system Mako/PyYAML, C/C++ compilers, Ninja, pkg-config, binutils,
patch, file, Debian package tools, configured matching Pop deb/deb-src repositories
and network access. It is not a general distro installer.

From the repository root, using an absent or empty scratch directory:

```bash
python3 docs/evidence/assets/issue70-vulkan-lifetime/build-mesa.py \
  build/issue70-reproduction --jobs 4

g++-13 -std=c++20 -Wall -Wextra -Wpedantic -fsanitize=address,undefined -g \
  docs/evidence/assets/issue70-vulkan-lifetime/probe.cpp -lvulkan \
  -o build/issue70-reproduction/probe

# Expected red: enumeration succeeds, LeakSanitizer reports 248 bytes and exits 1.
# Exit 3 means no usable Vulkan ICD; exit 4 means enumeration failed or the
# device count changed, so a red run cannot be mistaken for a broken setup.
__EGL_VENDOR_LIBRARY_FILENAMES="/usr/share/glvnd/egl_vendor.d/10_nvidia.json:$PWD/build/issue70-reproduction/stage-unmodified/10_mesa_unmodified.json" \
  ASAN_OPTIONS=detect_leaks=1 \
  LSAN_OPTIONS="suppressions=$PWD/tests/lsan_suppressions.txt" \
  build/issue70-reproduction/probe

# Expected green, preserving both EGL vendors and native Vulkan discovery.
__EGL_VENDOR_LIBRARY_FILENAMES="/usr/share/glvnd/egl_vendor.d/10_nvidia.json:$PWD/build/issue70-reproduction/stage-fixed/10_mesa_fixed.json" \
  ASAN_OPTIONS=detect_leaks=1 \
  LSAN_OPTIONS="suppressions=$PWD/tests/lsan_suppressions.txt" \
  build/issue70-reproduction/probe 10 1

__EGL_VENDOR_LIBRARY_FILENAMES="/usr/share/glvnd/egl_vendor.d/10_nvidia.json:$PWD/build/issue70-reproduction/stage-fixed/10_mesa_fixed.json" \
  ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --preset asan -j 2
```

The build compiles a minimal in-tree Gallium target solely to link EGL, stages
**only** `libEGL_mesa`, and removes build-tree RUNPATH. At runtime it requires the
installed, version-matched full Gallium library and existing GBM/DRM/NVIDIA
libraries. No in-tree software renderer or replacement Vulkan driver is loaded.
The experimental EGL uses debug symbols/assertions rather than distro release
optimization; this difference is identical on both sides of the red/green test.
No packages are installed and no global environment variables are written.

## Review boundaries

Production correctness is established by isolated red/green allocation evidence,
real native GPU checks and the full sanitizer run, without a Nemo lifetime change.
Prototype conformance is not applicable: no UI, interaction, public API, shader,
image baseline or document behavior changed. Human approval covered only the
isolated dependency experiment, not a system package installation.
