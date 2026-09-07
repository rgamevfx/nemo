#include "nemo/gpu/Compile.hpp"

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
