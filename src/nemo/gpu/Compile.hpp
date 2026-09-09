#pragma once

// Runtime GLSL -> SPIR-V compilation (issue #6). OCIO's generated
// Vulkan-GLSL viewing-transform program is only known when a project config
// is loaded, so it must be compiled at runtime. glslang is the reference
// Khronos front end and the compiler OCIO's Vulkan output targets; Nemo's
// own effect shaders stay Slang (issue #3/#8) — see docs/decisions/
// 0006-ocio-viewing-transform.md.
//
// Process-wide state: glslang::InitializeProcess must run exactly once and
// FinalizeProcess exactly once; this module owns both (thread-safe one-time
// init, finalization at process exit).

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace nemo::gpu {

// Shader compilation failures carry the glslang info log (repo rule: errors
// identify the offending relationship).
struct CompileException : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Compiles a Vulkan-GLSL compute shader (entry point "main") to SPIR-V
// words. Throws CompileException with the full info log on parse or link
// failure.
[[nodiscard]] std::vector<std::uint32_t> compileGlslToSpirv(const std::string& glsl);

// Validated, aligned binary loading shared by native effect/presentation
// consumers. Failures identify the file; no shader substitution.
[[nodiscard]] std::vector<std::uint32_t> loadSpirv(const std::filesystem::path& path);

}  // namespace nemo::gpu
