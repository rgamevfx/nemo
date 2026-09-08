#include "nemo/media/ViewingTransform.hpp"

#include <OpenColorIO/OpenColorIO.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

namespace OCIO = OCIO_NAMESPACE;

namespace nemo::media {

namespace {

[[noreturn]] void fail(const std::string& configPath, const std::string& message) {
    throw OcioException("color config: " + configPath + ": " + message);
}

[[nodiscard]] std::string envOrDefault(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
}

// "display/view" (the ColorPolicy convention) -> (display, view). A bare
// name means the same string names both (documented in the header).
[[nodiscard]] std::pair<std::string, std::string> splitDisplayView(const std::string& name,
                                                                   const std::string& configPath, TransformKind kind) {
    const char* what = kind == TransformKind::Viewer ? "viewer transform" : "delivery transform";
    const auto slash = name.find('/');
    if (slash == std::string::npos) {
        return {name, name};
    }
    const std::string display = name.substr(0, slash);
    const std::string view = name.substr(slash + 1);
    if (display.empty() || view.empty()) {
        fail(configPath,
             std::string(what) + " '" + name + "' must be 'display/view' with a non-empty display and view");
    }
    return {display, view};
}

[[nodiscard]] OCIO::ConstConfigRcPtr loadConfig(const std::string& configPath) {
    try {
        return OCIO::Config::CreateFromFile(configPath.c_str());
    } catch (const OCIO::Exception& e) {
        fail(configPath, std::string("cannot load config: ") + e.what());
    } catch (const std::exception& e) {
        fail(configPath, std::string("cannot load config: ") + e.what());
    }
}

// Resolves the transform names against the config, throwing with the exact
// offending name; returns the display/view pair.
[[nodiscard]] std::pair<std::string, std::string> resolveNames(const OCIO::ConstConfigRcPtr& config,
                                                               const std::string& configPath,
                                                               const std::string& workingSpace,
                                                               const std::string& transformName, TransformKind kind) {
    if (config->getColorSpace(workingSpace.c_str()) == nullptr) {
        fail(configPath, "working space '" + workingSpace + "' is not a colorspace in this config");
    }
    const auto [display, view] = splitDisplayView(transformName, configPath, kind);
    bool displayExists = false;
    for (int i = 0; i < config->getNumDisplays(); ++i) {
        if (config->getDisplay(i) == display) {
            displayExists = true;
            break;
        }
    }
    if (!displayExists) {
        fail(configPath, (kind == TransformKind::Viewer ? "viewer transform '" : "delivery transform '") +
                             transformName + "': display '" + display + "' is not registered in this config");
    }
    if (!config->hasView(display.c_str(), view.c_str())) {
        fail(configPath, (kind == TransformKind::Viewer ? "viewer transform '" : "delivery transform '") +
                             transformName + "': display '" + display + "' has no view '" + view + "'");
    }
    return {display, view};
}

[[nodiscard]] OCIO::ConstProcessorRcPtr buildProcessor(const OCIO::ConstConfigRcPtr& config,
                                                       const std::string& configPath, const std::string& workingSpace,
                                                       const std::string& display, const std::string& view) {
    try {
        return config->getProcessor(workingSpace.c_str(), display.c_str(), view.c_str(), OCIO::TRANSFORM_DIR_FORWARD);
    } catch (const OCIO::Exception& e) {
        fail(configPath, std::string("cannot build viewing transform '") + workingSpace + "' -> '" + display + "/" +
                             view + "': " + e.what());
    } catch (const std::exception& e) {
        fail(configPath, std::string("cannot build viewing transform '") + workingSpace + "' -> '" + display + "/" +
                             view + "': " + e.what());
    }
}

// OCIO's generated Vulkan-GLSL samples its LUT textures with implicit-LOD
// `texture(...)` calls. The compute stage cannot use implicit LOD, and these
// textures are single-mip with pixel-independent coordinates, so LOD 0 is
// exactly what OCIO's fragment-stage model computes. Rewrite the calls to
// `textureLod(..., 0.0)` rather than reimplementing OCIO's transform code.
[[nodiscard]] std::string makeComputeCompatible(const std::string& ocioText) {
    std::string out;
    out.reserve(ocioText.size() + 64);
    std::size_t pos = 0;
    while (pos < ocioText.size()) {
        const auto hit = ocioText.find("texture(", pos);
        if (hit == std::string::npos) {
            out.append(ocioText, pos, std::string::npos);
            break;
        }
        // Do not rewrite identifiers ending in "...texture(" (e.g. a helper
        // named sampleTexture) and do not recurse into an existing Lod call.
        const bool isCall =
            hit == 0 || !(std::isalnum(static_cast<unsigned char>(ocioText[hit - 1])) || ocioText[hit - 1] == '_');
        out.append(ocioText, pos, hit - pos);
        if (!isCall) {
            out.append("texture(");
            pos = hit + 8;
            continue;
        }
        // Balance from the call's own '(' so nested calls close correctly.
        std::size_t depth = 0;
        std::size_t close = hit + 7;
        for (; close < ocioText.size(); ++close) {
            if (ocioText[close] == '(') {
                ++depth;
            } else if (ocioText[close] == ')') {
                --depth;
                if (depth == 0) {
                    break;
                }
            }
        }
        if (close >= ocioText.size()) {
            fail("generated shader", "unbalanced parentheses in OCIO shader text");
        }
        out.append("textureLod(").append(ocioText, hit + 8, close - hit - 8).append(", 0.0)");
        pos = close + 1;
    }
    return out;
}

void fillUniformBuffer(OCIO::GpuShaderDesc& desc, std::vector<std::byte>& buffer) {
    const auto size = desc.getUniformBufferSize();
    buffer.assign(size, std::byte{0});

    auto putFloat = [&](std::size_t offset, float value) {
        std::memcpy(buffer.data() + offset, &value, sizeof(float));
    };
    auto putInt = [&](std::size_t offset, int value) { std::memcpy(buffer.data() + offset, &value, sizeof(int)); };
    for (unsigned i = 0; i < desc.getNumUniforms(); ++i) {
        OCIO::GpuShaderDesc::UniformData data;
        desc.getUniform(i, data);
        switch (data.m_type) {
        case OCIO::UNIFORM_DOUBLE:
            putFloat(data.m_bufferOffset, static_cast<float>(data.m_getDouble()));
            break;
        case OCIO::UNIFORM_BOOL:
            putInt(data.m_bufferOffset, data.m_getBool() ? 1 : 0);
            break;
        case OCIO::UNIFORM_FLOAT3: {
            const OCIO::Float3& v = data.m_getFloat3();
            putFloat(data.m_bufferOffset + 0, static_cast<float>(v[0]));
            putFloat(data.m_bufferOffset + 4, static_cast<float>(v[1]));
            putFloat(data.m_bufferOffset + 8, static_cast<float>(v[2]));
            break;
        }
        case OCIO::UNIFORM_VECTOR_FLOAT: {
            // std140: one float per 16-byte array slot (OCIO computes the
            // offsets with GPU_ARRAY_STRIDE = 16).
            const float* values = data.m_vectorFloat.m_getVector();
            const int count = data.m_vectorFloat.m_getSize();
            for (int j = 0; j < count; ++j) {
                putFloat(data.m_bufferOffset + static_cast<std::size_t>(j) * 16, values[j]);
            }
            break;
        }
        case OCIO::UNIFORM_VECTOR_INT: {
            const int* values = data.m_vectorInt.m_getVector();
            const int count = data.m_vectorInt.m_getSize();
            for (int j = 0; j < count; ++j) {
                putInt(data.m_bufferOffset + static_cast<std::size_t>(j) * 16, values[j]);
            }
            break;
        }
        case OCIO::UNIFORM_UNKNOWN:
            fail("generated shader", "unknown uniform type in OCIO shader program");
        }
    }
}

// Compute wrapper: linear pixel SSBOs at set 1, bindings 0/1. The OCIO
// program (declarations, helpers, transform function) sits before it.
[[nodiscard]] std::string wrapComputeEntry(const std::string& ocioText, const std::string& functionName) {
    std::string glsl;
    glsl += "#version 450\n";
    glsl += "layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;\n";
    glsl += makeComputeCompatible(ocioText);
    glsl += "\nlayout(set = 1, binding = 0) buffer InPixels { vec4 in_pixels[]; };\n";
    glsl += "layout(set = 1, binding = 1) buffer OutPixels { vec4 out_pixels[]; };\n";
    glsl += "void main()\n{\n";
    glsl += "    uint idx = gl_GlobalInvocationID.x;\n";
    glsl += "    out_pixels[idx] = " + functionName + "(in_pixels[idx]);\n";
    glsl += "}\n";
    return glsl;
}

[[nodiscard]] OcioGpuProgram buildProgram(const OCIO::ConstConfigRcPtr& config, const std::string& configPath,
                                          const std::string& workingSpace, const std::string& display,
                                          const std::string& view) {
    const OCIO::ConstProcessorRcPtr processor = buildProcessor(config, configPath, workingSpace, display, view);
    const OCIO::ConstGPUProcessorRcPtr gpu = processor->getOptimizedGPUProcessor(OCIO::OPTIMIZATION_DEFAULT);

    OCIO::GpuShaderDescRcPtr desc = OCIO::GpuShaderDesc::CreateShaderDesc();
    desc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_VK_4_6);
    desc->setFunctionName("OCIODisplay");
    desc->setPixelName("outColor");
    // OCIO's UBO lands at set 0 binding 0; LUT textures take set 0 bindings
    // starting at 1. The adapter's pixel SSBOs live on set 1.
    desc->setDescriptorSetIndex(0, 1);
    desc->setResourcePrefix("ocio");
    try {
        gpu->extractGpuShaderInfo(desc);
    } catch (const OCIO::Exception& e) {
        fail(configPath, std::string("cannot extract GPU viewing transform: ") + e.what());
    }

    OcioGpuProgram program;
    program.descriptorSet = desc->getDescriptorSetIndex();
    program.textureBindingStart = desc->getTextureBindingStart();
    fillUniformBuffer(*desc, program.uniformBytes);

    program.textures.reserve(static_cast<std::size_t>(desc->getNumTextures()) + desc->getNum3DTextures());
    for (unsigned i = 0; i < desc->getNumTextures(); ++i) {
        const char* textureName = nullptr;
        const char* samplerName = nullptr;
        unsigned width = 0;
        unsigned height = 0;
        OCIO::GpuShaderCreator::TextureType channel{};
        OCIO::GpuShaderDesc::TextureDimensions dimensions{};
        OCIO::Interpolation interpolation{};
        desc->getTexture(i, textureName, samplerName, width, height, channel, dimensions, interpolation);
        const float* values = nullptr;
        desc->getTextureValues(i, values);
        OcioGpuProgram::Texture texture;
        texture.width = width;
        texture.height = height;
        texture.dimensions = dimensions == OCIO::GpuShaderDesc::TEXTURE_1D ? 1 : 2;
        texture.channels = channel == OCIO::GpuShaderCreator::TEXTURE_RED_CHANNEL ? 1u : 4u;
        texture.binding = desc->getTextureShaderBindingIndex(i);
        texture.values.assign(values, values + static_cast<std::size_t>(width) * height * texture.channels);
        program.textures.push_back(std::move(texture));
    }
    for (unsigned i = 0; i < desc->getNum3DTextures(); ++i) {
        const char* textureName = nullptr;
        const char* samplerName = nullptr;
        unsigned edge = 0;
        OCIO::Interpolation interpolation{};
        desc->get3DTexture(i, textureName, samplerName, edge, interpolation);
        const float* values = nullptr;
        desc->get3DTextureValues(i, values);
        OcioGpuProgram::Texture texture;
        texture.width = edge;
        texture.height = edge;
        texture.dimensions = 3;
        texture.binding = desc->get3DTextureShaderBindingIndex(i);
        // OCIO's 3D LUT arrays are RGB triplets (TEXTURE_RGB_CHANNEL, 3
        // components — see GenericGpuShaderDesc::add3DTexture). Expand them
        // to RGBA: 3-component float images are not sampleable on common
        // desktop drivers (NVIDIA exposes only HOST_IMAGE_TRANSFER for
        // VK_FORMAT_R32G32B32_SFLOAT); the shader code only ever reads .rgb.
        texture.channels = 4;
        const std::size_t texels = static_cast<std::size_t>(edge) * edge * edge;
        texture.values.resize(texels * 4);
        for (std::size_t t = 0; t < texels; ++t) {
            texture.values[t * 4 + 0] = values[t * 3 + 0];
            texture.values[t * 4 + 1] = values[t * 3 + 1];
            texture.values[t * 4 + 2] = values[t * 3 + 2];
            texture.values[t * 4 + 3] = 0.0F;
        }
        program.textures.push_back(std::move(texture));
    }

    program.glsl = wrapComputeEntry(desc->getShaderText(), desc->getFunctionName());
    program.description = "working '" + workingSpace + "' -> display '" + display + "' view '" + view + "'";
    return program;
}

}  // namespace

std::string resolveConfigPath(const std::string& configPath) {
    std::string resolved = configPath.empty() ? envOrDefault("OCIO") : configPath;
    if (resolved.empty()) {
        throw OcioException("color config: no config given and the OCIO environment variable is not set; set policy "
                            "configPath or $OCIO");
    }
    if (!std::filesystem::exists(resolved)) {
        throw OcioException("color config: " + resolved + ": file does not exist");
    }
    return resolved;
}

void applyViewingTransformCpu(CpuImage& image, const std::string& configPath, const std::string& workingSpace,
                              const std::string& transformName, TransformKind kind) {
    const std::string resolvedPath = resolveConfigPath(configPath);
    const OCIO::ConstConfigRcPtr config = loadConfig(resolvedPath);
    const auto [display, view] = resolveNames(config, resolvedPath, workingSpace, transformName, kind);
    const OCIO::ConstProcessorRcPtr processor = buildProcessor(config, resolvedPath, workingSpace, display, view);

    const OCIO::ConstCPUProcessorRcPtr cpu = processor->getDefaultCPUProcessor();
    const OCIO::PackedImageDesc desc(image.data(), image.width(), image.height(), OCIO::CHANNEL_ORDERING_RGBA,
                                     OCIO::BIT_DEPTH_F32, sizeof(float), 4 * sizeof(float),
                                     static_cast<ptrdiff_t>(4 * sizeof(float)) * image.width());
    cpu->apply(desc);
    image.setColorInterpretation(ColorInterpretation::DisplayReferred);
}

void applyViewingTransformCpu(CpuImage& image, const std::string& configPath, const ColorPolicy& policy,
                              TransformKind kind) {
    applyViewingTransformCpu(image, configPath, policy.workingSpace,
                             kind == TransformKind::Viewer ? policy.viewerTransform : policy.deliveryTransform, kind);
}

OcioGpuProgram buildViewingTransformGpu(const std::string& configPath, const std::string& workingSpace,
                                        const std::string& transformName, TransformKind kind) {
    const std::string resolvedPath = resolveConfigPath(configPath);
    const OCIO::ConstConfigRcPtr config = loadConfig(resolvedPath);
    const auto [display, view] = resolveNames(config, resolvedPath, workingSpace, transformName, kind);
    return buildProgram(config, resolvedPath, workingSpace, display, view);
}

}  // namespace nemo::media
