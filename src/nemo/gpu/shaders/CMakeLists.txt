# Build-time shader compilation (issue #3): Slang -> SPIR-V binaries.
# Execution and image-tolerance validation of these shaders is the native
# execution gate (issue #8), not this directory.

nemo_add_slang_shaders(nemo_shaders ALL
    SHADERS
        passthrough.slang
)
