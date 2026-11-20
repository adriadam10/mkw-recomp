#!/usr/bin/env python3
"""Build the production game-field shader or the diagnostic FSR 3.1.6 provider.

The default build compiles only the source-fixed game-motion-vector-field shader
for the official provider layout. --provider keeps the full diagnostic DLL
available. The builder keeps all generated work below build/ffx and never writes
into the repository or the player's game folder.

Usage:
  uv run tools/ffx/builder/build.py
  uv run tools/ffx/builder/build.py --provider
  uv run tools/ffx/builder/build.py --help
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from urllib.request import urlopen
import zipfile


ROOT = Path(__file__).resolve().parents[3]
SUPPORT = Path(__file__).resolve().parents[1] / "provider"
SHADER_TOOLS = Path(__file__).resolve().parents[1] / "provider_shaders"
PATCHES = Path(__file__).resolve().parents[1] / "patches"

SDK_URL = "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git"
SDK_COMMIT = "7539ae8b6568ad29ed78a753404351daf3b5cca7"
SDK_PATHS = (
    "Kits/FidelityFX/api/include",
    "Kits/FidelityFX/api/internal",
    "Kits/FidelityFX/backend/vk",
    "Kits/FidelityFX/framegeneration/include",
    "Kits/FidelityFX/framegeneration/fsr3/include",
    "Kits/FidelityFX/framegeneration/fsr3/internal",
    "Kits/FidelityFX/upscalers/fsr3/include",
)

DXC_VERSION = "1.9.2609.5"
DXC_URL = (
    "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.dxc/"
    f"{DXC_VERSION}/microsoft.direct3d.dxc.{DXC_VERSION}.nupkg"
)
DXC_SHA256 = "73cfa082245918c2cc1ec6740250eb7377e83a757b3b3a3e16e0d71535bfbfc3"

MINIZ_VERSION = "3.1.2"
MINIZ_COMMIT = "77d0dce8627735138c51770d1799a1ef48f2117d"
MINIZ_URL = f"https://github.com/richgel999/miniz/releases/download/{MINIZ_VERSION}/miniz-{MINIZ_VERSION}.zip"
MINIZ_SHA256 = "f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a"

GAME_FIELD_SHADER = "ffx_frameinterpolation_game_motion_vector_field_pass.hlsl"
GAME_FIELD_OUTPUT = "mkw_fsr_game_field.spv"
GAME_FIELD_HEADER = "ffx_game_field_spv.h"
GAME_FIELD_SOURCE_ARCHIVE = "mkw_fsr_game_field_source.zip"
GAME_FIELD_SHA256 = "1d7e3e8d3b7046764a2a9f98f5e04147befaf4b0ea4f9fa017242f9824af5c6e"
GAME_FIELD_BINDINGS = {
    "cbFI": 256,
    "s_LinearClamp": 320,
    "r_previous_interpolation_source": 2,
    "r_current_interpolation_source": 3,
    "r_dilated_motion_vectors": 0,
    "r_dilated_depth": 1,
    "r_input_distortion_field": 4,
    "rw_game_motion_vector_field_x": 128,
    "rw_game_motion_vector_field_y": 129,
}


def run(command: list[str | Path]) -> None:
    text = [str(part) for part in command]
    print("+", subprocess.list2cmdline(text), flush=True)
    subprocess.run(text, check=True)


def output(command: list[str | Path]) -> str:
    text = [str(part) for part in command]
    result = subprocess.run(text, check=True, text=True, encoding="utf-8", errors="replace", capture_output=True)
    return result.stdout.strip()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def download(url: str, destination: Path, expected_sha256: str) -> None:
    if destination.is_file() and sha256(destination) == expected_sha256:
        print(f"reuse {destination}", flush=True)
        return

    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=destination.parent, prefix=destination.name + ".", delete=False
        ) as temporary:
            temporary_path = Path(temporary.name)
            print(f"download {url}", flush=True)
            with urlopen(url) as response:
                shutil.copyfileobj(response, temporary)
            temporary.flush()
            os.fsync(temporary.fileno())
        actual = sha256(temporary_path)
        if actual != expected_sha256:
            raise RuntimeError(f"SHA-256 mismatch for {url}: expected {expected_sha256}, got {actual}")
        temporary_path.replace(destination)
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def reset_build_directory(destination: Path) -> None:
    destination = destination.resolve()
    generated_root = (ROOT / "build").resolve()
    if generated_root not in destination.parents:
        raise RuntimeError(f"cleanup must stay below {generated_root}: {destination}")
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True, exist_ok=True)


def safe_extract(archive: Path, destination: Path, *, replace: bool = False) -> None:
    destination = destination.resolve()
    generated_root = (ROOT / "build").resolve()
    if generated_root not in destination.parents:
        raise RuntimeError(f"package extraction must stay below {generated_root}: {destination}")
    with zipfile.ZipFile(archive) as source:
        for member in source.infolist():
            target = (destination / member.filename).resolve()
            try:
                target.relative_to(destination.resolve())
            except ValueError as error:
                raise RuntimeError(f"unsafe archive member {member.filename}") from error
        if replace:
            reset_build_directory(destination)
        else:
            destination.mkdir(parents=True, exist_ok=True)
        source.extractall(destination)


def ensure_sdk(source: Path, commit: str) -> None:
    if source.exists():
        try:
            current = output(["git", "-C", source, "rev-parse", "HEAD"])
        except subprocess.CalledProcessError as error:
            raise RuntimeError(f"existing SDK tree is not a Git checkout: {source}") from error
        if current.lower() == commit:
            patch_paths = set(patched_paths())
            modified = {
                Path(path).as_posix()
                for path in output(["git", "-C", source, "diff", "--name-only", "HEAD"]).splitlines()
                if path
            }
            untracked = {
                Path(path).as_posix()
                for path in output(["git", "-C", source, "ls-files", "--others", "--exclude-standard"]).splitlines()
                if path
            }
            unexpected = sorted((modified - patch_paths) | untracked)
            if unexpected:
                raise RuntimeError("SDK checkout has changes unrelated to provider patches: " + ", ".join(unexpected))
            print(f"reuse FidelityFX-SDK {commit}", flush=True)
            return
        raise RuntimeError(f"existing SDK checkout does not match {commit}: {source}; use a new build work directory")

    source.parent.mkdir(parents=True, exist_ok=True)
    run(["git", "init", source])
    run(["git", "-C", source, "remote", "add", "origin", SDK_URL])
    run(["git", "-C", source, "config", "core.autocrlf", "false"])
    run(["git", "-C", source, "sparse-checkout", "init", "--cone"])
    run(["git", "-C", source, "sparse-checkout", "set", *SDK_PATHS])
    run(
        [
            "git",
            "-C",
            source,
            "fetch",
            "--depth",
            "1",
            "--filter=blob:none",
            "origin",
            commit,
        ]
    )
    run(["git", "-C", source, "checkout", "--detach", "FETCH_HEAD"])
    current = output(["git", "-C", source, "rev-parse", "HEAD"])
    if current.lower() != commit:
        raise RuntimeError(f"FidelityFX-SDK is {current}, expected {commit}")


def patched_paths() -> list[str]:
    paths: set[str] = set()
    for patch in sorted(PATCHES.glob("*.patch")):
        for line in patch.read_text(encoding="utf-8").splitlines():
            if line.startswith("+++ b/"):
                path = line.removeprefix("+++ b/")
                if path != "/dev/null":
                    paths.add(path)
    return sorted(paths)


def apply_patches(source: Path) -> None:
    patches = sorted(PATCHES.glob("*.patch"))
    if not patches:
        raise RuntimeError(f"no provider patches found in {PATCHES}")
    for patch in patches:
        check = subprocess.run(
            ["git", "-C", str(source), "apply", "--check", str(patch)],
            text=True,
            capture_output=True,
        )
        if check.returncode == 0:
            run(["git", "-C", source, "apply", str(patch)])
            print(f"applied {patch.name}", flush=True)
            continue
        reverse = subprocess.run(
            ["git", "-C", str(source), "apply", "--reverse", "--check", str(patch)],
            text=True,
            capture_output=True,
        )
        if reverse.returncode == 0:
            print(f"already applied {patch.name}", flush=True)
            continue
        raise RuntimeError(
            f"cannot apply {patch.name}\nforward check:\n{check.stderr}\nreverse check:\n{reverse.stderr}"
        )


def find_tool(name: str, *, extra: tuple[Path, ...] = ()) -> Path:
    for candidate in extra:
        if candidate.is_file():
            return candidate.resolve()
    found = shutil.which(name)
    if found:
        return Path(found).resolve()
    raise RuntimeError(f"{name} was not found; pass its path or put it on PATH")


def find_spirv_tool(name: str) -> Path:
    common = (
        Path(r"C:\msys64\clang64\bin") / f"{name}.exe",
        Path(r"C:\VulkanSDK\Bin") / f"{name}.exe",
    )
    return find_tool(name, extra=common)


def download_dxc(work_root: Path) -> tuple[Path, Path]:
    archive = work_root / "downloads" / f"microsoft.direct3d.dxc.{DXC_VERSION}.nupkg"
    download(DXC_URL, archive, DXC_SHA256)
    package = work_root / f"tools/dxc/{DXC_VERSION}"
    safe_extract(archive, package, replace=True)
    return archive, package


def copy_dxc_notices(destination: Path, dxc_package: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    for name in ("LICENSE-LLVM.txt", "LICENCE-MIT.txt", "LICENSE-MS.txt"):
        shutil.copy2(dxc_package / name, destination / f"DXC-{name}")


def sdk_license(sdk: Path) -> str:
    try:
        return output(["git", "-C", sdk, "show", f"{SDK_COMMIT}:Kits/FidelityFX/docs/license.md"]) + "\n"
    except subprocess.CalledProcessError as error:
        raise RuntimeError("the pinned FidelityFX license could not be read") from error


def compile_game_field(
    work_root: Path,
    sdk: Path,
    dxc: Path,
    spirv_val: Path,
    spirv_dis: Path,
) -> tuple[Path, Path]:
    shader = sdk / "Kits/FidelityFX/framegeneration/fsr3/internal/shaders" / GAME_FIELD_SHADER
    if not shader.is_file():
        raise RuntimeError(f"game-field shader is missing from the pinned SDK: {shader}")
    tie_source = (
        sdk
        / "Kits/FidelityFX/framegeneration/fsr3/include/gpu/frameinterpolation"
        / "ffx_frameinterpolation_game_motion_vector_field.h"
    )
    patched = tie_source.read_text(encoding="utf-8")
    tie_cap = "uLowPriorityFactor = ffxMin(uLowPriorityFactor, PRIORITY_LOW_MAX - 1);"
    if tie_cap not in patched:
        raise RuntimeError("the StillTie production patch is not present in the game-field shader")

    generated_root = work_root / "shaders/game-field"
    reset_build_directory(generated_root)
    spv = generated_root / GAME_FIELD_OUTPUT
    disassembly = generated_root / (GAME_FIELD_OUTPUT + ".spvasm")
    common_defines = ("-DFFX_GPU=1", "-DFFX_HLSL=1", "-DFFX_HALF=0")
    fi_defines = (
        "-DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=1",
        "-DFFX_FRAMEINTERPOLATION_OPTION_JITTERED_MOTION_VECTORS=0",
        "-DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=0",
    )
    include_root = sdk / "Kits/FidelityFX"
    run(
        [
            dxc,
            "-spirv",
            "-fvk-use-dx-layout",
            "-fspv-use-unknown-image-format",
            "-fvk-t-shift",
            "0",
            "0",
            "-fvk-u-shift",
            "128",
            "0",
            "-fvk-b-shift",
            "256",
            "0",
            "-fvk-s-shift",
            "320",
            "0",
            "-T",
            "cs_6_6",
            "-E",
            "CS",
            "-WX",
            "-O3",
            "-fspv-target-env=vulkan1.2",
            *common_defines,
            *fi_defines,
            "-I",
            include_root / "api/internal/gpu",
            "-I",
            include_root / "framegeneration/fsr3/include",
            "-I",
            include_root / "framegeneration/fsr3/include/gpu",
            "-I",
            shader.parent,
            "-Fo",
            spv,
            shader,
        ]
    )
    run([spirv_val, "--target-env", "vulkan1.2", spv])
    run([spirv_dis, "--raw-id", spv, "-o", disassembly])
    validate_game_field_bindings(disassembly)
    return spv, disassembly


def validate_game_field_bindings(disassembly: Path) -> None:
    names: dict[int, str] = {}
    bindings: dict[int, int] = {}
    name_re = re.compile(r'^\s*OpName %(\d+) "(.*)"$')
    binding_re = re.compile(r"^\s*OpDecorate %(\d+) Binding (\d+)$")
    for line in disassembly.read_text(encoding="utf-8").splitlines():
        if match := name_re.match(line):
            names[int(match.group(1))] = match.group(2)
        elif match := binding_re.match(line):
            bindings[int(match.group(1))] = int(match.group(2))
    actual = {names.get(identifier, f"%{identifier}"): binding for identifier, binding in bindings.items()}
    if actual != GAME_FIELD_BINDINGS:
        expected = ", ".join(f"{name}={binding}" for name, binding in sorted(GAME_FIELD_BINDINGS.items()))
        found = ", ".join(f"{name}={binding}" for name, binding in sorted(actual.items()))
        raise RuntimeError(f"game-field bindings differ\nexpected: {expected}\nfound: {found}")


def write_game_field_header(work_root: Path, spv: Path) -> Path:
    data = spv.read_bytes()
    if len(data) % 4:
        raise RuntimeError(f"SPIR-V byte size is not a multiple of four: {len(data)}")
    words = [int.from_bytes(data[offset : offset + 4], "little") for offset in range(0, len(data), 4)]
    if b"".join(word.to_bytes(4, "little") for word in words) != data:
        raise RuntimeError("generated C words do not reconstruct the reviewed SPIR-V")

    include_dir = work_root / "out/include"
    reset_build_directory(include_dir)
    header = include_dir / GAME_FIELD_HEADER
    rows = [
        "    " + ", ".join(f"0x{word:08x}u" for word in words[offset : offset + 8]) + ","
        for offset in range(0, len(words), 8)
    ]
    header.write_text(
        f"""\
// SPDX-License-Identifier: MIT
//
// Copyright (C) 2026 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Generated from AMD FidelityFX-SDK v2.4.0
// source: {SDK_URL}
// commit: {SDK_COMMIT}
// shader: {GAME_FIELD_SHADER}
// builder: uv run tools/ffx/builder/build.py
// spv sha256: {GAME_FIELD_SHA256}
// spv bytes: {len(data)}
// official bindings: UAV 128/129, CB 256, SRV 0..4, sampler 320
#pragma once
#include <stdint.h>

static const uint32_t ffx_game_field_spv[] = {{
{chr(10).join(rows)}
}};
""",
        encoding="utf-8",
        newline="\n",
    )
    return header


def write_source_archive(destination: Path, sdk: Path, fidelityfx_license: str) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    entries: list[tuple[Path, str]] = []
    for directory, owner in ((sdk, "sdk"), (SUPPORT.parent, "builder")):
        for path in directory.rglob("*"):
            if not path.is_file():
                continue
            relative = path.relative_to(directory)
            if ".git" in relative.parts or "__pycache__" in relative.parts:
                continue
            entries.append((path, f"{owner}/{relative.as_posix()}"))

    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path, name in sorted(entries, key=lambda item: item[1].lower()):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, path.read_bytes())
        license_info = zipfile.ZipInfo("LICENSES/FidelityFX-LICENSE.md", date_time=(1980, 1, 1, 0, 0, 0))
        license_info.compress_type = zipfile.ZIP_DEFLATED
        license_info.external_attr = 0o100644 << 16
        archive.writestr(license_info, fidelityfx_license.encode("utf-8"))
        builder_license = zipfile.ZipInfo("LICENSES/mkw-recomp-LICENSE", date_time=(1980, 1, 1, 0, 0, 0))
        builder_license.compress_type = zipfile.ZIP_DEFLATED
        builder_license.external_attr = 0o100644 << 16
        archive.writestr(builder_license, (ROOT / "LICENSE").read_bytes())


def write_game_field_notices(
    work_root: Path,
    sdk: Path,
    dxc_package: Path,
    spv: Path,
    header: Path,
    *,
    package_source: bool,
) -> tuple[Path | None, Path]:
    out = work_root / "out"
    licenses = out / "licenses"
    reset_build_directory(licenses)
    fidelityfx_license = sdk_license(sdk)
    (licenses / "FidelityFX-LICENSE.md").write_text(fidelityfx_license, encoding="utf-8", newline="\n")
    shutil.copy2(ROOT / "LICENSE", licenses / "mkw-recomp-LICENSE")
    copy_dxc_notices(licenses, dxc_package)

    source_archive: Path | None = None
    if package_source:
        source_dir = out / "source"
        reset_build_directory(source_dir)
        source_archive = source_dir / GAME_FIELD_SOURCE_ARCHIVE
        write_source_archive(source_archive, sdk, fidelityfx_license)

    patch_hashes = "\n".join(f"  {patch.name}: {sha256(patch)}" for patch in sorted(PATCHES.glob("*.patch")))
    source_archive_text = (
        f"\nsource archive: {source_archive}\nsource archive sha256: {sha256(source_archive)}\n"
        if source_archive is not None
        else "\n"
    )
    notice = out / "SOURCE-NOTICES.txt"
    output_sha = sha256(spv)
    notice.write_text(
        f"""FidelityFX-SDK v2.4.0
  source: {SDK_URL}
  commit: {SDK_COMMIT}
  license: MIT, see licenses/FidelityFX-LICENSE.md
  shader: {GAME_FIELD_SHADER}
  patches:
{patch_hashes}

DirectXShaderCompiler {DXC_VERSION}
  package: {DXC_URL}
  sha256: {DXC_SHA256}
  license: LLVM and MIT notices in licenses/DXC-*

game-field shader
  official layout: UAV {GAME_FIELD_BINDINGS["rw_game_motion_vector_field_x"]}/{GAME_FIELD_BINDINGS["rw_game_motion_vector_field_y"]}, CB {GAME_FIELD_BINDINGS["cbFI"]}, SRV 0..4, sampler {GAME_FIELD_BINDINGS["s_LinearClamp"]}
  output: {spv}
  output sha256: {output_sha}
  C header: {header}
  C header sha256: {sha256(header)}
{source_archive_text}""",
        encoding="utf-8",
        newline="\n",
    )
    return source_archive, notice


def build_game_field(args: argparse.Namespace) -> int:
    if os.name != "nt":
        raise SystemExit("the game-field shader build currently requires Windows")
    work_root = args.work_dir.resolve()
    generated_root = (ROOT / "build").resolve()
    if generated_root not in work_root.parents:
        raise SystemExit(f"--work-dir must be below {generated_root}")
    work_root.mkdir(parents=True, exist_ok=True)

    sdk = work_root / "src"
    ensure_sdk(sdk, SDK_COMMIT)
    apply_patches(sdk)
    _, dxc_package = download_dxc(work_root)
    dxc = args.dxc or dxc_package / "build/native/bin/x64/dxc.exe"
    if not dxc.is_file():
        raise RuntimeError(f"DXC executable was not found: {dxc}")
    spirv_val = args.spirv_val or find_spirv_tool("spirv-val")
    spirv_dis = args.spirv_dis or find_spirv_tool("spirv-dis")
    spv, _ = compile_game_field(work_root, sdk, dxc, spirv_val, spirv_dis)
    actual_sha = sha256(spv)
    if actual_sha != GAME_FIELD_SHA256:
        raise RuntimeError(
            f"game-field artifact changed without review: expected {GAME_FIELD_SHA256}, built {actual_sha}"
        )

    output_dir = work_root / "out/bin"
    reset_build_directory(output_dir)
    output = output_dir / GAME_FIELD_OUTPUT
    shutil.copy2(spv, output)
    header = write_game_field_header(work_root, output)
    source_archive, notice = write_game_field_notices(
        work_root, sdk, dxc_package, output, header, package_source=args.source_package
    )
    print(f"built {output}", flush=True)
    print(f"sha256 {actual_sha}", flush=True)
    print(f"header {header}", flush=True)
    print(f"notices {notice}", flush=True)
    if source_archive is not None:
        print(f"source {source_archive}", flush=True)
    return 0


def visual_studio_install() -> tuple[Path, str]:
    vswhere = (
        Path(
            os.environ.get(
                "ProgramFiles(x86)",
                r"C:\Program Files (x86)",
            )
        )
        / "Microsoft Visual Studio/Installer/vswhere.exe"
    )
    if not vswhere.is_file():
        raise RuntimeError("vswhere.exe was not found; install the MSVC C++ workload")
    raw = output(
        [
            vswhere,
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-format",
            "json",
            "-utf8",
        ]
    )
    records = json.loads(raw)
    if not records:
        raise RuntimeError("Visual Studio with the MSVC x64 C++ workload was not found")
    record = records[0]
    return Path(record["installationPath"]), record["installationVersion"]


def cmake_generator(requested: str | None, dll: Path) -> tuple[str, Path]:
    install, version = visual_studio_install()
    major = int(version.split(".", 1)[0])
    generator = requested or {
        18: "Visual Studio 18 2026",
        17: "Visual Studio 17 2022",
        16: "Visual Studio 16 2019",
    }.get(major)
    if generator is None:
        raise RuntimeError(f"unknown CMake generator for Visual Studio {version}; pass --generator")
    return generator, install


def make_vulkan_import_library(work_root: Path, visual_studio: Path) -> Path:
    deps = ROOT / "build/win/deps/lib"
    official = next(
        (
            candidate
            for candidate in (
                deps / "vulkan-1.lib",
                deps / "libvulkan-1.lib",
            )
            if candidate.is_file()
        ),
        None,
    )
    if official is not None:
        print(f"use Vulkan import library {official}", flush=True)
        return official

    binaries = sorted(
        visual_studio.glob("VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe"),
        key=lambda path: path.parents[3].name,
    )
    if not binaries:
        raise RuntimeError("dumpbin.exe was not found in the Visual Studio C++ toolset")
    dumpbin = binaries[-1]
    lib = dumpbin.with_name("lib.exe")
    if not lib.is_file():
        raise RuntimeError("lib.exe was not found in the Visual Studio C++ toolset")

    vulkan_dll = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/vulkan-1.dll"
    if not vulkan_dll.is_file():
        raise RuntimeError(f"Vulkan loader was not found: {vulkan_dll}")
    exports = output([dumpbin, "/nologo", "/exports", vulkan_dll])
    names = re.findall(
        r"^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)",
        exports,
        re.MULTILINE,
    )
    names = sorted({name for name in names if name != "[NONAME]"})
    if "vkGetInstanceProcAddr" not in names:
        raise RuntimeError(f"no Vulkan exports were parsed from {vulkan_dll}")

    target = work_root / "vendor/vulkan/vulkan-1.lib"
    definition = target.with_suffix(".def")
    target.parent.mkdir(parents=True, exist_ok=True)
    definition.write_text(
        "LIBRARY vulkan-1.dll\nEXPORTS\n" + "".join(f"{name}\n" for name in names),
        encoding="ascii",
        newline="\n",
    )
    run([lib, "/nologo", f"/def:{definition}", f"/out:{target}", "/machine:x64"])
    return target


def compile_and_link(
    work_root: Path,
    miniz_root: Path,
    generated: Path,
    generator: str,
    vulkan_lib: Path,
) -> Path:
    for item in SUPPORT.iterdir():
        target = work_root / item.name
        if item.is_dir():
            shutil.copytree(item, target, dirs_exist_ok=True)
        else:
            shutil.copy2(item, target)

    cmake_build = work_root / "cmake-build"
    run(
        [
            "cmake",
            "-S",
            work_root,
            "-B",
            cmake_build,
            "-G",
            generator,
            "-A",
            "x64",
            "-DMKW_FFX_SDK_ROOT=" + str(work_root / "src"),
            "-DMKW_FFX_MINIZ_ROOT=" + str(miniz_root),
            "-DMKW_FFX_GENERATED=" + str(generated),
            "-DMKW_FFX_VULKAN_INCLUDE=" + str(ROOT / "build/win/deps/include"),
            "-DMKW_FFX_VULKAN_LIBRARY=" + str(vulkan_lib),
        ]
    )
    run(
        [
            "cmake",
            "--build",
            cmake_build,
            "--config",
            "Release",
            "--parallel",
        ]
    )
    dll = next((work_root / "out/bin").glob("mkw_fsr_fg_vk.dll"), None)
    if dll is None:
        raise RuntimeError("CMake finished without producing mkw_fsr_fg_vk.dll")
    return dll


def copy_notices(work_root: Path, sdk: Path, dxc_package: Path, miniz_package: Path) -> None:
    notices = work_root / "out/licenses"
    notices.mkdir(parents=True, exist_ok=True)

    try:
        sdk_license = output(["git", "-C", sdk, "show", f"{SDK_COMMIT}:Kits/FidelityFX/docs/license.md"])
    except subprocess.CalledProcessError as error:
        raise RuntimeError("the pinned FidelityFX license could not be read") from error
    (notices / "FidelityFX-LICENSE.md").write_text(sdk_license + "\n", encoding="utf-8", newline="\n")

    for name in ("LICENSE", "readme.md", "ChangeLog.md"):
        shutil.copy2(miniz_package / name, notices / f"miniz-{name}")
    for name in ("LICENSE-LLVM.txt", "LICENCE-MIT.txt", "LICENSE-MS.txt"):
        shutil.copy2(dxc_package / name, notices / f"DXC-{name}")

    (work_root / "out/SOURCE-NOTICES.txt").write_text(
        f"""FidelityFX-SDK v2.4.0
  source: {SDK_URL}
  commit: {SDK_COMMIT}
  license: MIT, see licenses/FidelityFX-LICENSE.md

DirectXShaderCompiler {DXC_VERSION}
  package: {DXC_URL}
  sha256: {DXC_SHA256}
  license: LLVM and MIT notices in licenses/DXC-*

miniz {MINIZ_VERSION}
  source: {MINIZ_URL}
  commit: {MINIZ_COMMIT}
  sha256: {MINIZ_SHA256}
  license: MIT, see licenses/miniz-LICENSE
""",
        encoding="utf-8",
        newline="\n",
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build the pinned game-field shader or the full diagnostic FSR provider."
    )
    parser.add_argument(
        "--provider",
        action="store_true",
        help="build the full diagnostic provider DLL instead of the game-field SPIR-V",
    )
    parser.add_argument(
        "--source-package",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="include pinned SDK and builder sources in the game-field output",
    )
    parser.add_argument(
        "--work-dir",
        type=Path,
        default=ROOT / "build/ffx",
        help="generated checkouts, tools, shader and provider output (default: build/ffx)",
    )
    parser.add_argument("--generator", help="override the Visual Studio CMake generator")
    parser.add_argument("--dxc", type=Path, help="DXC executable (default: downloaded package)")
    parser.add_argument("--spirv-val", type=Path, help="spirv-val executable")
    parser.add_argument("--spirv-dis", type=Path, help="spirv-dis executable")
    args = parser.parse_args()

    if not args.provider:
        return build_game_field(args)

    if os.name != "nt":
        raise SystemExit("this provider build currently requires Windows and MSVC")
    work_root = args.work_dir.resolve()
    generated_root = (ROOT / "build").resolve()
    if generated_root not in work_root.parents:
        raise SystemExit(f"--work-dir must be below {generated_root}")
    work_root.mkdir(parents=True, exist_ok=True)

    sdk = work_root / "src"
    ensure_sdk(sdk, SDK_COMMIT)
    apply_patches(sdk)

    downloads = work_root / "downloads"
    _dxc_archive, dxc_package = download_dxc(work_root)
    miniz_archive = downloads / f"miniz-{MINIZ_VERSION}.zip"
    download(MINIZ_URL, miniz_archive, MINIZ_SHA256)

    miniz_package = work_root / f"vendor/miniz/{MINIZ_VERSION}"
    safe_extract(miniz_archive, miniz_package, replace=True)

    dxc = args.dxc or dxc_package / "build/native/bin/x64/dxc.exe"
    spirv_val = args.spirv_val or find_spirv_tool("spirv-val")
    spirv_dis = args.spirv_dis or find_spirv_tool("spirv-dis")
    pwsh = find_tool("pwsh")
    shader_script = SHADER_TOOLS / "compile_shaders.ps1"
    generator_script = SHADER_TOOLS / "gen_shader_blobs.py"
    generated = work_root / "generated/fp32_wave32_shaderblobs.cpp"

    run(
        [
            pwsh,
            "-NoProfile",
            "-File",
            shader_script,
            "-BuildRoot",
            work_root,
            "-Dxc",
            dxc,
            "-SpirvVal",
            spirv_val,
            "-SpirvDis",
            spirv_dis,
        ]
    )
    run(
        [
            sys.executable,
            generator_script,
            "--spv-dir",
            work_root / "shaders/spv/fp32-wave32",
            "--hdr-spv-dir",
            work_root / "shaders/spv/fp32-wave32-hdr",
            "--output",
            generated,
        ]
    )

    dll = work_root / "out/bin/mkw_fsr_fg_vk.dll"
    generator, visual_studio = cmake_generator(args.generator, dll)
    vulkan_lib = make_vulkan_import_library(work_root, visual_studio)
    dll = compile_and_link(
        work_root,
        miniz_package,
        generated,
        generator,
        vulkan_lib,
    )
    copy_notices(work_root, sdk, dxc_package, miniz_package)
    print(f"built {dll}", flush=True)
    print(f"licenses {work_root / 'out/licenses'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
