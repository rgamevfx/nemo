# Build-time shader compilation (issue #3): Slang -> SPIR-V binaries.
# Execution and image-tolerance validation of these shaders is the native
# execution gate (issue #8), not this directory.
nemo_add_slang_shaders(nemo_shaders ALL
    SHADERS
        passthrough.slang
        testpattern.slang
        constcolor.slang
        merge.slang
        output.slang
        source.slang
        mediaConvert.slang
        viewerPresentation.slang
)

if(DEFINED NEMO_SLANG_SPV_DIR)
    # Surface the compiled-kernel directory to sibling consumers (nemo-cli
    # evaluate-gpu, tests); the function sets it in this directory's scope.
    set(NEMO_SLANG_SPV_DIR "${NEMO_SLANG_SPV_DIR}" PARENT_SCOPE)
endif()
