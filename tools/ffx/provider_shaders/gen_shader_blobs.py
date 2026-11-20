#!/usr/bin/env python3
"""Generate FfxShaderBlob metadata from validated SPIR-V disassembly.

The shaders are compiled with disjoint category shifts:
  SRV texture t0..       -> Vulkan binding 0..
  UAV texture/buffer u0..-> Vulkan binding 64..
  constant buffer b0..   -> Vulkan binding 128..
  sampler s0..           -> Vulkan binding 320..

This reads reflected bindings and names from spirv-dis output, embeds the
validated SPIR-V bytes, and emits the public provider lookup functions. It
deliberately fails on unknown resources or duplicate category bindings.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import argparse
import re


SHIFT_SRV_TEXTURE = 0
SHIFT_UAV = 64
SHIFT_CB = 128
SHIFT_SAMPLER = 320

FI_FILES = {
    "ffx_frameinterpolation_reconstruct_and_dilate_pass": "FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_AND_DILATE",
    "ffx_frameinterpolation_setup_pass": "FFX_FRAMEINTERPOLATION_PASS_SETUP",
    "ffx_frameinterpolation_reconstruct_previous_depth_pass": "FFX_FRAMEINTERPOLATION_PASS_RECONSTRUCT_PREV_DEPTH",
    "ffx_frameinterpolation_game_motion_vector_field_pass": "FFX_FRAMEINTERPOLATION_PASS_GAME_MOTION_VECTOR_FIELD",
    "ffx_frameinterpolation_optical_flow_vector_field_pass": "FFX_FRAMEINTERPOLATION_PASS_OPTICAL_FLOW_VECTOR_FIELD",
    "ffx_frameinterpolation_disocclusion_mask_pass": "FFX_FRAMEINTERPOLATION_PASS_DISOCCLUSION_MASK",
    "ffx_frameinterpolation_pass": "FFX_FRAMEINTERPOLATION_PASS_INTERPOLATION",
    "ffx_frameinterpolation_compute_inpainting_pyramid_pass": "FFX_FRAMEINTERPOLATION_PASS_INPAINTING_PYRAMID",
    "ffx_frameinterpolation_inpainting_pass": "FFX_FRAMEINTERPOLATION_PASS_INPAINTING",
    "ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass": "FFX_FRAMEINTERPOLATION_PASS_GAME_VECTOR_FIELD_INPAINTING_PYRAMID",
    "ffx_frameinterpolation_debug_view_pass": "FFX_FRAMEINTERPOLATION_PASS_DEBUG_VIEW",
}

OF_FILES = {
    "ffx_opticalflow_prepare_luma_pass": "FFX_OPTICALFLOW_PASS_PREPARE_LUMA",
    "ffx_opticalflow_compute_luminance_pyramid_pass": "FFX_OPTICALFLOW_PASS_GENERATE_OPTICAL_FLOW_INPUT_PYRAMID",
    "ffx_opticalflow_generate_scd_histogram_pass": "FFX_OPTICALFLOW_PASS_GENERATE_SCD_HISTOGRAM",
    "ffx_opticalflow_compute_scd_divergence_pass": "FFX_OPTICALFLOW_PASS_COMPUTE_SCD_DIVERGENCE",
    "ffx_opticalflow_compute_optical_flow_advanced_pass_v5": "FFX_OPTICALFLOW_PASS_COMPUTE_OPTICAL_FLOW_ADVANCED_V5",
    "ffx_opticalflow_filter_optical_flow_pass_v5": "FFX_OPTICALFLOW_PASS_FILTER_OPTICAL_FLOW_V5",
    "ffx_opticalflow_scale_optical_flow_advanced_pass_v5": "FFX_OPTICALFLOW_PASS_SCALE_OPTICAL_FLOW_ADVANCED_V5",
}

CATEGORIES = (
    "constant_buffers",
    "srv_textures",
    "uav_textures",
    "srv_buffers",
    "uav_buffers",
    "samplers",
)

ID = r"%([0-9]+)"
NAME_RE = re.compile(rf'^\s*OpName {ID} "(.*)"$')
DECORATE_RE = re.compile(rf"^\s*OpDecorate {ID} (Binding|DescriptorSet) ([0-9]+)$")
TYPE_POINTER_RE = re.compile(rf"^\s*{ID} = OpTypePointer (\w+) {ID}$")
TYPE_ARRAY_RE = re.compile(rf"^\s*{ID} = OpTypeArray {ID} {ID}$")
TYPE_RUNTIME_ARRAY_RE = re.compile(rf"^\s*{ID} = OpTypeRuntimeArray {ID}$")
TYPE_IMAGE_RE = re.compile(rf"^\s*{ID} = OpTypeImage ")
TYPE_SAMPLER_RE = re.compile(rf"^\s*{ID} = OpTypeSampler$")
TYPE_STRUCT_RE = re.compile(rf"^\s*{ID} = OpTypeStruct ")
OP_CONSTANT_RE = re.compile(rf"^\s*{ID} = OpConstant %[0-9]+ ([0-9]+)$")
VARIABLE_RE = re.compile(rf"^\s*{ID} = OpVariable {ID} (\w+)")


@dataclass(frozen=True)
class Resource:
    category: str
    name: str
    binding: int
    count: int
    space: int


def parse_disassembly(path: Path, reject_magic: bool) -> list[Resource]:
    text = path.read_text(encoding="utf-8")
    names: dict[int, str] = {}
    bindings: dict[int, int] = {}
    spaces: dict[int, int] = {}
    pointer_types: dict[int, tuple[str, int]] = {}
    arrays: dict[int, tuple[int, int]] = {}
    runtime_arrays: dict[int, int] = {}
    constants: dict[int, int] = {}
    images: set[int] = set()
    samplers: set[int] = set()
    structs: set[int] = set()
    variables: list[tuple[int, int, str]] = []

    for line in text.splitlines():
        if match := NAME_RE.match(line):
            names[int(match.group(1))] = match.group(2)
        elif match := DECORATE_RE.match(line):
            ident = int(match.group(1))
            value = int(match.group(3))
            (bindings if match.group(2) == "Binding" else spaces)[ident] = value
        elif match := TYPE_POINTER_RE.match(line):
            pointer_types[int(match.group(1))] = (match.group(2), int(match.group(3)))
        elif match := TYPE_ARRAY_RE.match(line):
            arrays[int(match.group(1))] = (int(match.group(2)), int(match.group(3)))
        elif match := TYPE_RUNTIME_ARRAY_RE.match(line):
            runtime_arrays[int(match.group(1))] = int(match.group(2))
        elif match := TYPE_IMAGE_RE.match(line):
            images.add(int(match.group(1)))
        elif match := TYPE_SAMPLER_RE.match(line):
            samplers.add(int(match.group(1)))
        elif match := TYPE_STRUCT_RE.match(line):
            structs.add(int(match.group(1)))
        elif match := OP_CONSTANT_RE.match(line):
            constants[int(match.group(1))] = int(match.group(2))
        elif match := VARIABLE_RE.match(line):
            variables.append((int(match.group(1)), int(match.group(2)), match.group(3)))

    resources: list[Resource] = []
    occupied: set[tuple[str, int, int]] = set()
    for variable_id, pointer_id, storage_class in variables:
        if variable_id not in bindings:
            continue
        if pointer_id not in pointer_types:
            raise RuntimeError(f"{path.name}: variable {variable_id} has unknown pointer type")

        _, pointee_id = pointer_types[pointer_id]
        count = 1
        base_id = pointee_id
        if pointee_id in arrays:
            base_id, count_id = arrays[pointee_id]
            if count_id not in constants:
                raise RuntimeError(f"{path.name}: array {pointee_id} has non-constant length")
            count = constants[count_id]

        binding = bindings[variable_id]
        space = spaces.get(variable_id, 0)
        name = names.get(variable_id)
        if not name:
            raise RuntimeError(f"{path.name}: resource {variable_id} has no OpName")

        if storage_class == "Uniform" and base_id in structs:
            category = "constant_buffers"
        elif storage_class == "StorageBuffer":
            category = "uav_buffers" if name.startswith("rw_") else "srv_buffers"
        elif storage_class == "UniformConstant" and base_id in samplers:
            category = "samplers"
            if binding < SHIFT_SAMPLER:
                raise RuntimeError(f"{path.name}: sampler {name} lacks sampler shift")
        elif storage_class == "UniformConstant" and base_id in images:
            category = "srv_textures" if binding < SHIFT_UAV else "uav_textures"
        elif storage_class == "UniformConstant" and (base_id in runtime_arrays or base_id in structs):
            category = "srv_buffers" if binding < SHIFT_UAV else "uav_buffers"
        else:
            raise RuntimeError(
                f"{path.name}: unsupported resource {name} "
                f"(storage={storage_class}, pointee={base_id}, binding={binding})"
            )

        if category == "constant_buffers" and binding < SHIFT_CB:
            raise RuntimeError(f"{path.name}: constant buffer {name} lacks buffer shift")
        if category in {"uav_textures", "uav_buffers"} and binding < SHIFT_UAV:
            raise RuntimeError(f"{path.name}: UAV {name} lacks UAV shift")
        if category in {"srv_textures", "srv_buffers"} and binding >= SHIFT_UAV:
            raise RuntimeError(f"{path.name}: SRV {name} has invalid binding {binding}")

        key = (category, binding, space)
        if key in occupied:
            raise RuntimeError(f"{path.name}: duplicate {category} binding {binding}:{space}")
        occupied.add(key)
        resources.append(Resource(category, name, binding, count, space))

    resources.sort(key=lambda res: (CATEGORIES.index(res.category), res.binding))
    return resources


def cpp_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit_resource_arrays(lines: list[str], prefix: str, category: str, resources: list[Resource]) -> None:
    if not resources:
        return
    names = [res.name for res in resources]
    lines.append(f"static const char* {prefix}_{category}_names[] = {{")
    lines.extend(f"    {cpp_string(name)}," for name in names)
    lines.append("};")
    for field, expression in (
        ("bindings", "binding"),
        ("counts", "count"),
        ("spaces", "space"),
    ):
        lines.append(f"static const uint32_t {prefix}_{category}_{field}[] = {{")
        lines.extend(f"    {getattr(res, expression)}u," for res in resources)
        lines.append("};")


def emit_shader(
    lines: list[str],
    spv_path: Path,
    resources: list[Resource],
    symbol_suffix: str = "",
) -> str:
    prefix = spv_path.stem.replace("-", "_") + symbol_suffix
    data = spv_path.read_bytes()
    lines.append(f"static const uint8_t {prefix}_data[] = {{")
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    lines.append("};")

    by_category = {category: [res for res in resources if res.category == category] for category in CATEGORIES}
    for category in CATEGORIES:
        emit_resource_arrays(lines, prefix, category, by_category[category])

    counts = {category: len(by_category[category]) for category in CATEGORIES}
    lines.append(f"static const FfxShaderBlobInfo {prefix}_info[] = {{")
    lines.extend(
        (
            f'    {prefix}_data, sizeof({prefix}_data), "CS",',
            f"    {counts['constant_buffers']}u, {counts['srv_textures']}u,",
            f"    {counts['uav_textures']}u, {counts['srv_buffers']}u,",
            f"    {counts['uav_buffers']}u, {counts['samplers']}u, 0u,",
        )
    )
    for category in CATEGORIES:
        if counts[category]:
            lines.extend(
                (
                    f"    {prefix}_{category}_names,",
                    f"    {prefix}_{category}_bindings,",
                    f"    {prefix}_{category}_counts,",
                    f"    {prefix}_{category}_spaces,",
                )
            )
        else:
            lines.append("    nullptr, nullptr, nullptr, nullptr,")
    lines.append("};")
    return f"{prefix}_info"


def emit_lookup(
    lines: list[str],
    function_name: str,
    enum_type: str,
    entries: dict[str, str],
    infos: dict[str, str],
) -> None:
    lines.append(f'extern "C" FfxErrorCode {function_name}(')
    lines.append(f"    {enum_type} passId, uint32_t, FfxShaderBlob* outBlob)")
    lines.append("{")
    lines.append("    if (outBlob == nullptr) {")
    lines.append("        return FFX_ERROR_INVALID_ARGUMENT;")
    lines.append("    }")
    lines.append("    switch (passId) {")
    for stem, enum_value in entries.items():
        lines.append(f"    case {enum_value}:")
        lines.append(f"        *outBlob = POPULATE_SHADER_BLOB_FFX({infos[stem]}, 0);")
        lines.append("        return FFX_OK;")
    lines.append("    default:")
    lines.append("        *outBlob = {};")
    lines.append("        return FFX_ERROR_INVALID_ARGUMENT;")
    lines.append("    }")
    lines.append("}")
    lines.append("")


def emit_opticalflow_lookup(
    lines: list[str],
    entries: dict[str, str],
    sdr_infos: dict[str, str],
    hdr_infos: dict[str, str],
) -> None:
    lines.append('extern "C" FfxErrorCode opticalflowGetPermutationBlobByIndex(')
    lines.append("    FfxOpticalflowPass passId, uint32_t permutationOptions, FfxShaderBlob* outBlob)")
    lines.append("{")
    lines.append("    if (outBlob == nullptr) {")
    lines.append("        return FFX_ERROR_INVALID_ARGUMENT;")
    lines.append("    }")
    lines.append("    const bool useHdr = (permutationOptions & 0x4u) != 0u;")
    lines.append("    switch (passId) {")
    for stem, enum_value in entries.items():
        lines.append(f"    case {enum_value}:")
        lines.append("        if (useHdr) {")
        lines.append(f"            *outBlob = POPULATE_SHADER_BLOB_FFX({hdr_infos[stem]}, 0);")
        lines.append("        } else {")
        lines.append(f"            *outBlob = POPULATE_SHADER_BLOB_FFX({sdr_infos[stem]}, 0);")
        lines.append("        }")
        lines.append("        return FFX_OK;")
    lines.append("    default:")
    lines.append("        *outBlob = {};")
    lines.append("        return FFX_ERROR_INVALID_ARGUMENT;")
    lines.append("    }")
    lines.append("}")
    lines.append("")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--spv-dir", type=Path, required=True)
    parser.add_argument(
        "--hdr-spv-dir",
        type=Path,
        required=True,
        help="directory containing the seven HDR optical-flow SPIR-V blobs",
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    all_entries = {**FI_FILES, **OF_FILES}
    available = {path.stem for path in args.spv_dir.glob("*.spv")}
    missing = sorted(set(all_entries) - available)
    if missing:
        raise SystemExit(f"missing SPIR-V blobs: {', '.join(missing)}")
    hdr_available = {path.stem for path in args.hdr_spv_dir.glob("*.spv")}
    hdr_missing = sorted(set(OF_FILES) - hdr_available)
    if hdr_missing:
        raise SystemExit(f"missing HDR optical-flow SPIR-V blobs: {', '.join(hdr_missing)}")

    lines = [
        "// Generated by gen_shader_blobs.py. Do not edit.",
        "// FidelityFX-SDK v2.4.0, commit 7539ae8b6568ad29ed78a753404351daf3b5cca7",
        '#include "Kits/FidelityFX/api/internal/ffx_internal_types.h"',
        '#include "Kits/FidelityFX/framegeneration/fsr3/internal/ffx_frameinterpolation_shaderblobs.h"',
        '#include "Kits/FidelityFX/framegeneration/fsr3/internal/ffx_opticalflow_shaderblobs.h"',
        "",
        "struct FfxShaderBlobInfo {",
        "    const uint8_t* blobData;",
        "    uint32_t blobSize;",
        "    const char* entryName;",
        "    uint32_t numConstantBuffers;",
        "    uint32_t numSRVTextures;",
        "    uint32_t numUAVTextures;",
        "    uint32_t numSRVBuffers;",
        "    uint32_t numUAVBuffers;",
        "    uint32_t numSamplers;",
        "    uint32_t numRTAccelerationStructures;",
        "    const char** constantBufferNames;",
        "    const uint32_t* constantBufferBindings;",
        "    const uint32_t* constantBufferCounts;",
        "    const uint32_t* constantBufferSpaces;",
        "    const char** srvTextureNames;",
        "    const uint32_t* srvTextureBindings;",
        "    const uint32_t* srvTextureCounts;",
        "    const uint32_t* srvTextureSpaces;",
        "    const char** uavTextureNames;",
        "    const uint32_t* uavTextureBindings;",
        "    const uint32_t* uavTextureCounts;",
        "    const uint32_t* uavTextureSpaces;",
        "    const char** srvBufferNames;",
        "    const uint32_t* srvBufferBindings;",
        "    const uint32_t* srvBufferCounts;",
        "    const uint32_t* srvBufferSpaces;",
        "    const char** uavBufferNames;",
        "    const uint32_t* uavBufferBindings;",
        "    const uint32_t* uavBufferCounts;",
        "    const uint32_t* uavBufferSpaces;",
        "    const char** samplerNames;",
        "    const uint32_t* samplerBindings;",
        "    const uint32_t* samplerCounts;",
        "    const uint32_t* samplerSpaces;",
        "    const char** rtAccelerationStructureNames;",
        "    const uint32_t* rtAccelerationStructureBindings;",
        "    const uint32_t* rtAccelerationStructureCounts;",
        "    const uint32_t* rtAccelerationStructureSpaces;",
        "};",
        "",
    ]

    infos: dict[str, str] = {}
    hdr_infos: dict[str, str] = {}
    for stem in all_entries:
        resources = parse_disassembly(args.spv_dir / f"{stem}.spv.spvasm", reject_magic=True)
        infos[stem] = emit_shader(lines, args.spv_dir / f"{stem}.spv", resources)
        lines.append("")

    for stem in OF_FILES:
        resources = parse_disassembly(args.hdr_spv_dir / f"{stem}.spv.spvasm", reject_magic=True)
        hdr_infos[stem] = emit_shader(
            lines,
            args.hdr_spv_dir / f"{stem}.spv",
            resources,
            symbol_suffix="_hdr",
        )
        lines.append("")

    emit_lookup(
        lines,
        "frameInterpolationGetPermutationBlobByIndex",
        "FfxFrameInterpolationPass",
        FI_FILES,
        infos,
    )
    emit_opticalflow_lookup(lines, OF_FILES, infos, hdr_infos)
    lines.extend(
        (
            'extern "C" FfxErrorCode frameInterpolationIsWave64(uint32_t, bool& isWave64) {',
            "    isWave64 = false; // This provider embeds FP32/Wave32 SPIR-V.",
            "    return FFX_OK;",
            "}",
            "",
            'extern "C" FfxErrorCode opticalflowIsWave64(uint32_t, bool& isWave64) {',
            "    isWave64 = false; // Wave size is a driver decision for SPIR-V.",
            "    return FFX_OK;",
            "}",
            "",
        )
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"Generated {args.output} with {len(infos) + len(hdr_infos)} real shader blobs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
