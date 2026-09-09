#include "nemo/gpu/Compile.hpp"
#include "nemo/gpu/Error.hpp"
#include <fstream>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <string>

namespace nemo::gpu {

namespace {

// One-time glslang process initialization. The finalizer runs at process
// exit (static storage duration) after every compile is done.
struct GlslangProcess {
    GlslangProcess() { glslang::InitializeProcess(); }
    ~GlslangProcess() { glslang::FinalizeProcess(); }
};

void ensureProcess() {
    static GlslangProcess process;
    (void)process;
}

}  // namespace

std::vector<std::uint32_t> loadSpirv(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        throw GpuException(GpuError::InvalidRequest, "cannot open compiled shader: " + path.string());
    const auto bytes = stream.tellg();
    if (bytes < 4 || bytes % 4 != 0)
        throw GpuException(GpuError::InvalidRequest, "not a SPIR-V module: " + path.string());
    std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes) / 4);
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(words.data()), bytes) || words.front() != 0x07230203)
        throw GpuException(GpuError::InvalidRequest, "not a SPIR-V module: " + path.string());
    return words;
}

std::vector<std::uint32_t> compileGlslToSpirv(const std::string& glsl) {
    ensureProcess();

    glslang::TShader shader(EShLangCompute);
    const char* source = glsl.c_str();
    shader.setStrings(&source, 1);
    shader.setEntryPoint("main");
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangCompute, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);

    if (!shader.parse(GetDefaultResources(), 450, ENoProfile, false, false, EShMsgDefault)) {
        throw CompileException(std::string("glslang parse failed: ") + shader.getInfoLog() + shader.getInfoDebugLog());
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) {
        throw CompileException(std::string("glslang link failed: ") + program.getInfoLog() + program.getInfoDebugLog());
    }

    std::vector<std::uint32_t> spirv;
    spv::SpvBuildLogger spvLog;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), spirv, &spvLog);
    if (spirv.empty()) {
        throw CompileException("glslang produced no SPIR-V");
    }
    return spirv;
}

}  // namespace nemo::gpu
