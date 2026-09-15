# Build-time shader compilation (issue #3): Slang -> SPIR-V binaries.
# Execution and image-tolerance validation of these shaders is the native
# execution gate (issue #8), not this directory.
#
# Issue #83: each built-in node owns its kernels next to its contribution
# (src/nemo/nodes/<slug>/<name>.slang). Only the module-wide shaders that are
# not node effects stay here. The output basenames are unchanged (<name>.spv),
# so EffectLibrary's shader->spv mapping and every existing consumer of the
# compiled-kernel directory keep working without a second inventory.
nemo_add_slang_shaders(nemo_shaders ALL
    SHADERS
        passthrough.slang
        mediaConvert.slang
        viewerPresentation.slang
        ../../nodes/testpattern/testpattern.slang
        ../../nodes/constcolor/constcolor.slang
        ../../nodes/merge/merge.slang
        ../../nodes/output/output.slang
        ../../nodes/source/source.slang
        ../../nodes/grade/grade.slang
        ../../nodes/blur/blur.slang
        ../../nodes/blur/blurHorizontal.slang
        ../../nodes/transform/transform.slang
    # Every node effect includes the shared per-input geometry block (issue
    # #85); the compiled SPIR-V must be rebuilt when that contract changes.
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../nodes/GpuGeometry.slang"
)

if(DEFINED NEMO_SLANG_SPV_DIR)
    # Surface the compiled-kernel directory to sibling consumers (nemo-cli
    # evaluate-gpu, tests); the function sets it in this directory's scope.
    set(NEMO_SLANG_SPV_DIR "${NEMO_SLANG_SPV_DIR}" PARENT_SCOPE)
endif()
