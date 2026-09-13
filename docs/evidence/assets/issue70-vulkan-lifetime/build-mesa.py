#!/usr/bin/env python3
"""Issue 70: rebuild the installed Mesa libEGL_mesa in an isolated work directory.

    build-mesa.py <workdir>

Downloads (never installs) the pinned inputs into <workdir>, builds the exact installed Mesa
release once with debug symbols and stages it unmodified, then applies
./mesa-egl-query-cleanup.patch to the same source and relinks into:

    <work>/stage-unmodified/  libEGL_mesa.so.0.0.0 + symlinks + absolute-path glvnd manifest
    <work>/stage-fixed/       same, after `patch -p1 --fuzz=0` + incremental ninja

Both stages keep debug symbols and have DT_RPATH/DT_RUNPATH removed, so they load the
installed libgallium-<version>.so / libgbm / libdrm / libX11 instead of the in-tree stub
gallium they were linked against.  Nothing is installed system-wide and no driver, layer or
vendor override is set.  Any mismatch (installed Mesa/gallium version or hash, pinned download
hash, pristine eglapi.c, patch, leftover RUNPATH, missing debug info/SONAME, unresolved
linkage or libgallium resolving outside /lib and /usr/lib) aborts FAIL-CLOSED.
Transcript: <work>/logs/build.log.
"""

import argparse, hashlib, json, os, re, shutil, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PINS = json.loads((HERE / "build-inputs.json").read_text())
PATCH = HERE / "mesa-egl-query-cleanup.patch"
LIB = PINS["stages"]["lib"]


def fail(message):
    raise RuntimeError("FAIL-CLOSED: " + message)


def sha256(path):
    with open(path, "rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def verify(path, expected, what):
    got = sha256(path) if Path(path).is_file() else fail(f"{what}: missing {path}")
    if got != expected:
        fail(f"{what}: sha256 {got}, expected {expected}")


def run(cmd, log, cwd=None, env=None):
    """Run one command, append its output to log, abort on non-zero exit."""
    cmd = [str(part) for part in cmd]
    done = subprocess.run(cmd, cwd=None if cwd is None else str(cwd), env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    with open(log, "a") as handle:
        handle.write(f"$ {' '.join(cmd)}\n{done.stdout}\n[exit {done.returncode}]\n\n")
    if done.returncode:
        fail(f"{' '.join(cmd)} failed (exit {done.returncode}):\n"
             + "\n".join(done.stdout.strip().splitlines()[-30:]))
    return done.stdout


def preflight(log):
    """Refuse to build unless the installed Mesa/gallium is the pinned one."""
    for package in PINS["installed"]["packages"]:
        version = run(["dpkg-query", "-W", "-f=${Version}", package], log).strip()
        if version != PINS["mesa_version"]:
            fail(f"installed {package} is {version}, pinned {PINS['mesa_version']}")
    egl, gallium = PINS["installed"]["libEGL_mesa"], PINS["installed"]["libgallium"]
    verify(egl["path"], egl["sha256"], "installed libEGL_mesa")
    verify(gallium["path"], gallium["sha256"], "installed libgallium")
    dynamic = run(["readelf", "-d", egl["path"]], log)
    if egl["soname"] not in dynamic or gallium["soname"] not in dynamic:
        fail(f"installed libEGL_mesa does not reference {egl['soname']} / {gallium['soname']}")


def fetch_debs(work, log):
    """apt-get download the pinned .debs, verify them, extract them into <work>/local.

    There is no resume mode: main rejects a non-empty workdir, so the `is_file`
    guards only catch an apt-get that exits 0 without writing the pinned file.
    """
    deps, local = work / "deps", work / "local"
    for directory in (deps, local):
        directory.mkdir(parents=True, exist_ok=True)
    for entry in PINS["debs"]:
        deb = deps / entry["filename"]
        if not deb.is_file():
            run(["apt-get", "download", f"{entry['name']}={entry['version']}"], log, cwd=deps)
        verify(deb, entry["sha256"], f"deb {entry['name']}")
        version = run(["dpkg-deb", "-f", deb, "Version"], log).strip()
        if version != entry["version"]:
            fail(f"deb {deb.name}: version {version}, pinned {entry['version']}")
        run(["dpkg-deb", "-x", deb, local], log)
    for pc in local.rglob("*.pc"):  # extracted .pc files must point into the extracted tree
        pc.write_text(re.sub(r"^prefix=/usr$", f"prefix={local}/usr", pc.read_text(), flags=re.MULTILINE))
    return local


def fetch_source(work, log):
    """Fetch and verify the pinned release, extract it fresh, write VERSION."""
    source = PINS["source"]
    archive = work / "archive"
    archive.mkdir(parents=True, exist_ok=True)
    dsc, tarball = archive / source["dsc"]["filename"], archive / source["tar"]["filename"]
    if not (dsc.is_file() and tarball.is_file()):
        run(["apt-get", "source", "--download-only", f"{source['package']}={source['version']}"], log, cwd=archive)
    verify(dsc, source["dsc"]["sha256"], "source .dsc")
    verify(tarball, source["tar"]["sha256"], "source tarball")
    src = archive / source["extracted_dir"]
    run(["dpkg-source", "-x", dsc.name], log, cwd=archive)
    verify(src / source["eglapi"], source["eglapi_sha256"], "pristine eglapi.c")
    # debian/rules writes VERSION from debian/changelog so meson's project_version() (and
    # therefore the libgallium SONAME) matches the installed libgallium.
    (src / "VERSION").write_text(source["version"] + "\n")
    return src


def toolchain(work, local, log):
    """Create the venv with the hash-pinned Meson wheel; return (environment, tool versions)."""
    venv, wheels = work / "venv", work / "wheels"
    wheels.mkdir(parents=True, exist_ok=True)
    if not (venv / "bin" / "meson").is_file():
        if not (venv / "bin" / "python").is_file():
            run([sys.executable, "-m", "venv", "--system-site-packages", venv], log)
        wheel = wheels / PINS["tooling"]["meson_wheel"]["filename"]
        if not wheel.is_file():
            run([venv / "bin/pip", "download", "--no-deps", "--only-binary=:all:",
                 f"meson=={PINS['tooling']['meson']}", "--dest", wheels], log)
        verify(wheel, PINS["tooling"]["meson_wheel"]["sha256"], "meson wheel")
        run([venv / "bin/pip", "install", "--no-index", "--find-links", wheels,
             f"meson=={PINS['tooling']['meson']}"], log)

    env = dict(os.environ, LC_ALL="C")
    env["PATH"] = os.pathsep.join([str(local / "usr/bin"), str(venv / "bin"), env.get("PATH", "")])
    env["PKG_CONFIG_PATH"] = os.pathsep.join([str(local / "usr/lib/x86_64-linux-gnu/pkgconfig"),
                                              str(local / "usr/share/pkgconfig")])
    env["BISON_PKGDATADIR"], env["M4"] = str(local / "usr/share/bison"), str(local / "usr/bin/m4")
    env["MESON"], env["PATCHELF"] = str(venv / "bin/meson"), str(local / "usr/bin/patchelf")
    env["NINJA"] = shutil.which("ninja", path=env["PATH"]) or fail("ninja not found in PATH")
    env["PATCH"] = shutil.which("patch", path=env["PATH"]) or fail("patch not found in PATH")
    for tool in ("MESON", "PATCHELF"):
        if not Path(env[tool]).is_file(): fail(f"{tool} missing: {env[tool]}")
    modules = run([venv / "bin/python", "-c",
                   "import mako, yaml; print(mako.__version__, yaml.__version__)"], log).split()
    versions = {"meson": run([env["MESON"], "--version"], log).strip(), "mako": modules[0],
                "pyyaml": modules[1], "ninja": run([env["NINJA"], "--version"], log).strip(),
                "patch": run([env["PATCH"], "--version"], log).splitlines()[0]}
    for name in ("meson", "mako", "pyyaml", "ninja", "patch"):
        if PINS["tooling"][name] not in versions[name]:
            fail(f"{name} is {versions[name]}, pinned {PINS['tooling'][name]}")
    return env, versions


def stage(work, env, label, log):
    """Copy the built library into its own EGL-only stage, drop RUNPATH, check linkage."""
    stages = PINS["stages"]
    stage_dir = work / stages["dirs"][label]
    stage_dir.mkdir(parents=True)
    staged = stage_dir / LIB
    shutil.copy2(work / "build" / "src" / "egl" / LIB, staged)
    for link in stages["symlinks"]:
        os.symlink(LIB, stage_dir / link)
    run([env["PATCHELF"], "--remove-rpath", staged], log)
    manifest = stage_dir / stages["manifests"][label]
    manifest.write_text(json.dumps({"file_format_version": "1.0.0", "ICD": {"library_path": str(staged)}}, indent=4) + "\n")

    dynamic = run(["readelf", "-d", staged], log)
    gallium = PINS["installed"]["libgallium"]["soname"]
    if "RPATH" in dynamic or "RUNPATH" in dynamic:
        fail(f"{label}: staged library keeps RPATH/RUNPATH")
    if gallium not in dynamic:
        fail(f"{label}: staged library does not DT_NEED {gallium}")
    description = run(["file", "-b", staged], log).strip()
    if "debug_info" not in description:
        fail(f"{label}: no debug symbols: {description}")

    ldd = run(["ldd", "-r", staged], log, env=dict(env, LD_LIBRARY_PATH=str(stage_dir)))
    if "not found" in ldd or "undefined symbol" in ldd:
        fail(f"{label}: unresolved linkage:\n{ldd}")
    match = re.search(rf"{re.escape(gallium)}\s+=>\s+(\S+)", ldd)
    if not match or not match.group(1).startswith(("/lib/", "/usr/lib/")):
        fail(f"{label}: {gallium} is not resolved from the installed paths")
    build_id = re.search(r"Build ID:\s*([0-9a-f]+)", run(["readelf", "-n", staged], log))
    return {"library": str(staged), "manifest": str(manifest), "sha256": sha256(staged),
            "build_id": build_id.group(1) if build_id else "", "libgallium": match.group(1)}


def apply_patch(src, env, log):
    """Apply the sibling patch with --fuzz=0 to src/egl/main/eglapi.c only."""
    if not PATCH.is_file():
        fail(f"patch missing: {PATCH}")
    touched = re.findall(r"^\+\+\+ b/(\S+)", PATCH.read_text(), flags=re.MULTILINE)
    if touched != [PINS["source"]["eglapi"]]:
        fail(f"patch touches {touched}, expected exactly ['{PINS['source']['eglapi']}']")
    target = src / PINS["source"]["eglapi"]
    before = sha256(target)
    run([env["PATCH"], "--batch", "-p1", "--fuzz=0", "-i", PATCH], log, cwd=src)
    if sha256(target) == before:
        fail("patch applied but src/egl/main/eglapi.c is unchanged")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("workdir", type=Path, help="scratch directory for downloads, source, build and stages")
    parser.add_argument("--jobs", type=int, default=2, help="parallel compiler jobs (default: 2)")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    work = args.workdir.expanduser().resolve()
    if work.exists() and (not work.is_dir() or any(work.iterdir())):
        parser.error("workdir must be absent or empty; existing files are never removed")
    log = work / "logs" / "build.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    try:
        print(f"work directory: {work}", flush=True)
        preflight(log)
        local = fetch_debs(work, log)
        src = fetch_source(work, log)
        env, tools = toolchain(work, local, log)
        build_dir = work / "build"
        run([env["MESON"], "setup", build_dir, src, "--prefix", work / "prefix",
             "--libdir", PINS["meson"]["libdir"], *PINS["meson"]["flags"]], log, env=env)
        run([env["NINJA"], "-j", args.jobs, "-C", build_dir, PINS["meson"]["target"]], log, env=env)
        unmodified = stage(work, env, "unmodified", log)
        print(f"unmodified: {unmodified['sha256']}", flush=True)

        apply_patch(src, env, log)
        run([env["NINJA"], "-j", args.jobs, "-C", build_dir, PINS["meson"]["target"]], log, env=env)
        fixed = stage(work, env, "fixed", log)
        print(f"fixed:      {fixed['sha256']}", flush=True)
        if fixed["sha256"] == unmodified["sha256"]:
            fail("fixed library is byte-identical to the unmodified one")
    except RuntimeError as error:
        print(str(error), file=sys.stderr, flush=True)
        return 1

    summary = {"mesa_version": PINS["mesa_version"], "workdir": str(work),
               "patch": {"file": str(PATCH), "sha256": sha256(PATCH), "fuzz": 0},
               "tool_versions": tools, "meson_flags": PINS["meson"]["flags"],
               "stages": {"unmodified": unmodified, "fixed": fixed},
               "fixed_differs_from_unmodified": fixed["sha256"] != unmodified["sha256"]}
    (work / "build-summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2), flush=True)
    print("load with " + PINS["stages"]["example_load"].replace("<work>", str(work)), flush=True)
    return 0

if __name__ == "__main__":
    sys.exit(main())
