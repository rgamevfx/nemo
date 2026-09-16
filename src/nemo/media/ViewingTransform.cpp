#include "nemo/media/ViewingTransform.hpp"

#include <OpenColorIO/OpenColorIO.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>

#include "nemo/media/InputColor.hpp"

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
        // The built-in-config URI form names a config compiled into OCIO
        // (the pinned ACES Studio config for new projects), so it is loaded
        // from the built-in registry rather than from disk.
        if (isBuiltinConfigUri(configPath)) {
            return OCIO::Config::CreateFromBuiltinConfig(configPath.c_str());
        }
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
    glsl += "\nlayout(set = 1, binding = 0) readonly buffer InPixels { vec4 in_pixels[]; };\n";
    glsl += "layout(set = 1, binding = 1) writeonly buffer OutPixels { vec4 out_pixels[]; };\n";
    glsl += "void main()\n{\n";
    glsl += "    uint idx = gl_GlobalInvocationID.x;\n";
    glsl += "    if (idx >= in_pixels.length() || idx >= out_pixels.length()) return;\n";
    glsl += "    out_pixels[idx] = " + functionName + "(in_pixels[idx]);\n";
    glsl += "}\n";
    return glsl;
}

// Compute wrapper over the native packed four-channel image itself (issue
// #98): the image is the read/write surface, so a source input transform runs
// IN PLACE — one float4 load and one float4 store per logical pixel, with no
// staging buffer, no copy and no second image per frame. The OCIO transform
// sees the packed texel as its RGBA input, and the texel's own alpha is stored
// back unchanged (alpha is never part of the conversion). The dispatch extent
// comes from the image itself (`imageSize`), so no per-frame uniform is needed.
[[nodiscard]] std::string wrapImageEntry(const std::string& ocioText, const std::string& functionName) {
    std::string glsl;
    glsl += "#version 450\n";
    glsl += "layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;\n";
    glsl += makeComputeCompatible(ocioText);
    glsl += "\nlayout(set = 1, binding = 0, rgba32f) uniform image2D pixelImage;\n";
    glsl += "void main()\n{\n";
    glsl += "    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n";
    glsl += "    const ivec2 size = imageSize(pixelImage);\n";
    glsl += "    if (p.x >= size.x || p.y >= size.y) return;\n";
    glsl += "    const vec4 inColor = imageLoad(pixelImage, p);\n";
    glsl += "    const vec4 outColor = " + functionName + "(inColor);\n";
    glsl += "    imageStore(pixelImage, p, vec4(outColor.rgb, inColor.a));\n";
    glsl += "}\n";
    return glsl;
}

[[nodiscard]] std::vector<float> expandRgbLut(const float* values, std::size_t texels) {
    // OCIO stores RGB triplets, but RGB32F images are not sampleable on
    // common desktop drivers. The generated shader reads only .rgb.
    std::vector<float> rgba;
    rgba.reserve(texels * 4);
    for (std::size_t t = 0; t < texels; ++t) {
        rgba.push_back(values[t * 3 + 0]);
        rgba.push_back(values[t * 3 + 1]);
        rgba.push_back(values[t * 3 + 2]);
        rgba.push_back(0.0F);
    }
    return rgba;
}

[[nodiscard]] OcioGpuProgram
buildProgram(const std::string& configPath, const OCIO::ConstProcessorRcPtr& processor, const std::string& description,
             const std::string& functionName,
             const OcioGpuProgram::PixelInterface pixels = OcioGpuProgram::PixelInterface::Rgba32fBuffers) {
    const OCIO::ConstGPUProcessorRcPtr gpu = processor->getOptimizedGPUProcessor(OCIO::OPTIMIZATION_DEFAULT);

    OCIO::GpuShaderDescRcPtr desc = OCIO::GpuShaderDesc::CreateShaderDesc();
    desc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_VK_4_6);
    desc->setFunctionName(functionName.c_str());
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
        const std::size_t texels = static_cast<std::size_t>(width) * height;
        if (texture.channels == 1) {
            texture.values.assign(values, values + texels);
        } else {
            texture.values = expandRgbLut(values, texels);
        }
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
        texture.channels = 4;
        texture.values = expandRgbLut(values, static_cast<std::size_t>(edge) * edge * edge);
        program.textures.push_back(std::move(texture));
    }

    const bool packedImage = pixels == OcioGpuProgram::PixelInterface::PackedImage;
    program.glsl = packedImage ? wrapImageEntry(desc->getShaderText(), desc->getFunctionName())
                               : wrapComputeEntry(desc->getShaderText(), desc->getFunctionName());
    program.pixelInterface = pixels;
    program.description = description;
    return program;
}

}  // namespace

std::string resolveConfigPath(const std::string& configPath) {
    std::string resolved = configPath.empty() ? envOrDefault("OCIO") : configPath;
    if (resolved.empty()) {
        throw OcioException("color config: no config given and the OCIO environment variable is not set; set policy "
                            "configPath or $OCIO");
    }
    // The registered built-in-config reference is not a file: it names a
    // config compiled into the OCIO library. Only this scheme is treated
    // specially; any other URI is an ordinary path and is reported missing.
    if (isBuiltinConfigUri(resolved)) {
        return resolved;
    }
    if (!std::filesystem::exists(resolved)) {
        throw OcioException("color config: " + resolved + ": file does not exist");
    }
    return resolved;
}

bool isBuiltinConfigUri(const std::string_view path) {
    // A built-in config is only "present" when the core registry recognizes the
    // reference; any other URI-looking string stays an ordinary path so an
    // unsupported reference is reported as missing rather than claimed builtin.
    return isRegisteredColorConfigReference(path);
}

NewProjectColorDefault newProjectColorDefault() {
    // Owner-approved pinned default: the OCIO-embedded ACES Studio config, its
    // real scene-linear Rec.709 working space, and ACES 2.0 SDR viewing on
    // sRGB. Creation-time only — never a migration of existing documents.
    NewProjectColorDefault defaulted;
    defaulted.configUri = std::string{kBuiltinColorConfigUri};
    defaulted.policy.workingSpace = "Linear Rec.709 (sRGB)";
    defaulted.policy.viewerTransform = "sRGB - Display/ACES 2.0 - SDR 100 nits (Rec.709)";
    defaulted.policy.deliveryTransform = defaulted.policy.viewerTransform;
    // An explicitly configured OCIO environment is an owner override: the new
    // project then authors no reference and resolves through that environment
    // (the working/view literals stay the approved ones so the new project is
    // still coherently configured, and a config that lacks them reports the
    // offending relationship).
    if (!envOrDefault("OCIO").empty()) {
        defaulted.configUri.clear();
    }
    return defaulted;
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
    const OCIO::ConstProcessorRcPtr processor = buildProcessor(config, resolvedPath, workingSpace, display, view);
    return buildProgram(resolvedPath, processor,
                        "working '" + workingSpace + "' -> display '" + display + "' view '" + view + "'",
                        "OCIODisplay");
}

// ---------------------------------------------------------------------------
// Input color (issue #81)
// ---------------------------------------------------------------------------

namespace {

// Published Rec.709/sRGB (D65) primaries -> ACES AP0 matrix, exactly as the
// pinned OCIO 2.5.2 `studio-config-v4.0.0_aces-v2.0_ocio-v2.5` defines its
// `Linear Rec.709 (sRGB)` color space (read from the config's own processor).
// A working space is only accepted as scene-linear Rec.709 when its measured
// gamut matches this known reference — a name or a role is never trusted (the
// pinned ACES config's `scene_linear` role is ACEScg).
constexpr std::array<double, 9> kRec709ToAcesAp0{0.439632982,  0.382988691, 0.177378327,  //
                                                 0.0897764415, 0.813439429, 0.0967841297,
                                                 0.0175411701, 0.111546554, 0.870912254};
constexpr double kGamutTolerance = 2e-4;
constexpr double kLinearityTolerance = 2e-4;

// Applies one CPU processor to a single probe pixel.
[[nodiscard]] std::array<double, 3> probeRgb(const OCIO::ConstCPUProcessorRcPtr& cpu, const std::array<float, 3>& in) {
    float pixel[4] = {in[0], in[1], in[2], 1.0F};
    const OCIO::PackedImageDesc desc(pixel, 1, 1, OCIO::CHANNEL_ORDERING_RGBA, OCIO::BIT_DEPTH_F32, sizeof(float),
                                     4 * sizeof(float), 4 * sizeof(float));
    cpu->apply(desc);
    return {pixel[0], pixel[1], pixel[2]};
}

// Validates a working target against ONE retained config snapshot: name
// resolution, data-space rejection, measured linearity, and the published
// Rec.709 gamut when the config declares the ACES interchange role.
void requireSceneLinearRec709Impl(const OCIO::ConstConfigRcPtr& config, const std::string& reference,
                                  const std::string& workingSpace, const std::string& context) {
    const std::string suffix = workingTargetContext(context, workingSpace);
    const OCIO::ConstColorSpaceRcPtr space = config->getColorSpace(workingSpace.c_str());
    if (space == nullptr) {
        fail(reference, "working space '" + workingSpace + "' is not a colorspace in this config" + suffix);
    }
    if (space->isData()) {
        fail(reference,
             "working space '" + workingSpace + "' is a data space, not a scene-linear color space" + suffix);
    }
    // Meaning, not name: measure the config's own processor for this space. An
    // unset role reports an empty name, which is the same as absent.
    const char* interchange = config->getRoleColorSpace("aces_interchange");
    if (interchange != nullptr && interchange[0] == '\0') {
        interchange = nullptr;
    }
    const char* sceneLinear = config->getRoleColorSpace("scene_linear");
    if (sceneLinear != nullptr && sceneLinear[0] == '\0') {
        sceneLinear = nullptr;
    }
    const char* target = interchange != nullptr ? interchange : sceneLinear;
    if (target == nullptr) {
        return;  // no scene-linear reference in this config to measure against
    }
    OCIO::ConstProcessorRcPtr toTarget;
    try {
        toTarget = config->getProcessor(workingSpace.c_str(), target);
    } catch (const OCIO::Exception& e) {
        fail(reference, "cannot evaluate working space '" + workingSpace + "' against '" + std::string{target} +
                            "': " + e.what() + suffix);
    }
    const OCIO::ConstCPUProcessorRcPtr cpu = toTarget->getDefaultCPUProcessor();
    for (const double value : probeRgb(cpu, {0.0F, 0.0F, 0.0F})) {
        if (std::abs(value) > kLinearityTolerance) {
            fail(reference,
                 "working space '" + workingSpace + "' is not scene-linear: black does not map to black" + suffix);
        }
    }
    // Midpoint test: an affine (matrix) map satisfies f((a+b)/2) = (f(a)+f(b))/2;
    // any transfer function breaks it, so a display-encoded space cannot pass as
    // the working target.
    const std::array<std::array<float, 3>, 3> basis{{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}};
    const std::array<std::array<double, 3>, 3> mapped{probeRgb(cpu, basis[0]), probeRgb(cpu, basis[1]),
                                                      probeRgb(cpu, basis[2])};
    for (std::size_t a = 0; a < 3; ++a) {
        for (std::size_t b = a + 1; b < 3; ++b) {
            const std::array<double, 3> midpoint =
                probeRgb(cpu, {static_cast<float>((basis[a][0] + basis[b][0]) / 2.0),
                               static_cast<float>((basis[a][1] + basis[b][1]) / 2.0),
                               static_cast<float>((basis[a][2] + basis[b][2]) / 2.0)});
            for (int channel = 0; channel < 3; ++channel) {
                if (std::abs(midpoint[channel] - (mapped[a][channel] + mapped[b][channel]) / 2.0) >
                    kLinearityTolerance) {
                    fail(reference, "working space '" + workingSpace +
                                        "' is not scene-linear: its response is not linear" + suffix);
                }
            }
        }
    }
    if (interchange == nullptr) {
        return;
    }
    // Gamut: the measured matrix must be the published Rec.709 -> ACES AP0
    // matrix. A matrix-only gamut transform to another primary set (ACEScg, for
    // example) is a real conversion, not an identity, so it cannot pass.
    for (std::size_t column = 0; column < 3; ++column) {
        for (std::size_t row = 0; row < 3; ++row) {
            const double expected = kRec709ToAcesAp0[row * 3 + column];
            if (std::abs(mapped[column][row] - expected) > kGamutTolerance) {
                fail(reference, "working space '" + workingSpace + "' does not have Rec.709 primaries (measured " +
                                    std::to_string(mapped[column][row]) + " at matrix element " + std::to_string(row) +
                                    "," + std::to_string(column) + ", expected " + std::to_string(expected) +
                                    " against '" + std::string{interchange} + ")" + suffix);
            }
        }
    }
}

// The config's own file rule for `filePath`, on ONE retained snapshot.
[[nodiscard]] ConfigFileRule configFileRuleImpl(const OCIO::ConstConfigRcPtr& config, const std::string& reference,
                                                const std::string& filePath) {
    std::size_t ruleIndex = 0;
    const char* colorSpace = nullptr;
    try {
        colorSpace = config->getColorSpaceFromFilepath(filePath.c_str(), ruleIndex);
    } catch (const OCIO::Exception& e) {
        fail(reference, std::string("cannot resolve a file rule for '") + filePath + "': " + e.what());
    }
    if (colorSpace == nullptr || colorSpace[0] == '\0') {
        return ConfigFileRule{};
    }
    const OCIO::ConstFileRulesRcPtr rules = config->getFileRules();
    // OCIO appends the config's Default rule last; a resolution through it is a
    // configured fallback, not a declaration about this file.
    const bool defaultRule = rules != nullptr && rules->getNumEntries() > 0 && ruleIndex + 1 == rules->getNumEntries();
    return ConfigFileRule{true, colorSpace, defaultRule};
}

[[nodiscard]] OCIO::ConstProcessorRcPtr buildInputProcessorImpl(const OCIO::ConstConfigRcPtr& config,
                                                                const std::string& reference,
                                                                const std::string& workingSpace,
                                                                const std::string& inputColorSpace) {
    if (config->getColorSpace(inputColorSpace.c_str()) == nullptr) {
        fail(reference, "input color space '" + inputColorSpace + "' is not a colorspace in this config");
    }
    try {
        return config->getProcessor(inputColorSpace.c_str(), workingSpace.c_str());
    } catch (const OCIO::Exception& e) {
        fail(reference, std::string("cannot build input transform '") + inputColorSpace + "' -> '" + workingSpace +
                            "': " + e.what());
    } catch (const std::exception& e) {
        fail(reference, std::string("cannot build input transform '") + inputColorSpace + "' -> '" + workingSpace +
                            "': " + e.what());
    }
}

}  // namespace

struct OcioConfigSnapshot::Impl {
    std::string reference;
    OCIO::ConstConfigRcPtr config;
    std::string identity;
};

OcioConfigSnapshot::OcioConfigSnapshot(std::string configPath) : impl_(std::make_unique<Impl>()) {
    impl_->reference = resolveConfigPath(configPath);
    impl_->config = loadConfig(impl_->reference);
    // Identity from the SAME loaded bytes the processors, rules and space list
    // are taken from: nothing can pair new config content with an old identity.
    impl_->identity = impl_->reference + "|" + impl_->config->getCacheID(impl_->config->getCurrentContext());
}

OcioConfigSnapshot::~OcioConfigSnapshot() = default;
OcioConfigSnapshot::OcioConfigSnapshot(OcioConfigSnapshot&&) noexcept = default;
OcioConfigSnapshot& OcioConfigSnapshot::operator=(OcioConfigSnapshot&&) noexcept = default;

const std::string& OcioConfigSnapshot::reference() const noexcept {
    return impl_->reference;
}
const std::string& OcioConfigSnapshot::identity() const noexcept {
    return impl_->identity;
}

std::vector<std::string> OcioConfigSnapshot::colorSpaces() const {
    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(impl_->config->getNumColorSpaces()));
    for (int i = 0; i < impl_->config->getNumColorSpaces(); ++i) {
        const char* name = impl_->config->getColorSpaceNameByIndex(i);
        if (name != nullptr && name[0] != '\0') {
            names.emplace_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

void OcioConfigSnapshot::requireSceneLinearRec709(const std::string& workingSpace, const std::string& context) const {
    requireSceneLinearRec709Impl(impl_->config, impl_->reference, workingSpace, context);
}

ConfigFileRule OcioConfigSnapshot::fileRuleFor(const std::string& filePath) const {
    return configFileRuleImpl(impl_->config, impl_->reference, filePath);
}

std::shared_ptr<const OcioInputTransform> OcioConfigSnapshot::inputTransform(const std::string& workingSpace,
                                                                             const std::string& inputColorSpace) const {
    return std::make_shared<const OcioInputTransform>(*this, workingSpace, inputColorSpace);
}

OcioGpuProgram OcioConfigSnapshot::inputTransformImageGpu(const std::string& workingSpace,
                                                          const std::string& inputColorSpace) const {
    requireSceneLinearRec709Impl(impl_->config, impl_->reference, workingSpace,
                                 "input transform '" + inputColorSpace + "'");
    const OCIO::ConstProcessorRcPtr processor =
        buildInputProcessorImpl(impl_->config, impl_->reference, workingSpace, inputColorSpace);
    return buildProgram(impl_->reference, processor,
                        "input '" + inputColorSpace + "' -> working '" + workingSpace +
                            "' over the packed four-channel image",
                        "OCIOInput", OcioGpuProgram::PixelInterface::PackedImage);
}

OcioGpuProgram OcioConfigSnapshot::inputTransformGpu(const std::string& workingSpace,
                                                     const std::string& inputColorSpace) const {
    requireSceneLinearRec709Impl(impl_->config, impl_->reference, workingSpace,
                                 "input transform '" + inputColorSpace + "'");
    const OCIO::ConstProcessorRcPtr processor =
        buildInputProcessorImpl(impl_->config, impl_->reference, workingSpace, inputColorSpace);
    return buildProgram(impl_->reference, processor,
                        "input '" + inputColorSpace + "' -> working '" + workingSpace + "'", "OCIOInput");
}

// The free functions are one-shot wrappers: the same code path through a single
// snapshot, for callers that resolve a configuration once (tests, tooling, the
// inspector's enumeration query).
std::vector<std::string> configInputColorSpaces(const std::string& configPath) {
    const OcioConfigSnapshot snapshot(configPath);
    return snapshot.colorSpaces();
}

std::string colorConfigIdentity(const std::string& configPath) {
    const OcioConfigSnapshot snapshot(configPath);
    return snapshot.identity();
}

ConfigFileRule configFileRuleFor(const std::string& configPath, const std::string& filePath) {
    const OcioConfigSnapshot snapshot(configPath);
    return snapshot.fileRuleFor(filePath);
}

void requireSceneLinearRec709(const std::string& configPath, const std::string& workingSpace,
                              const std::string& context) {
    std::string resolvedPath;
    try {
        resolvedPath = resolveConfigPath(configPath);
    } catch (const OcioException& error) {
        throw OcioException(std::string(error.what()) + workingTargetContext(context, workingSpace));
    }
    const OcioConfigSnapshot snapshot(resolvedPath);
    snapshot.requireSceneLinearRec709(workingSpace, context);
}

std::string workingTargetContext(const std::string& context, const std::string& workingSpace) {
    return " (context: " + context + "; working space '" + workingSpace +
           "'; supported working spaces: the built-in 'linear' scene-linear Rec.709 or a project configuration whose "
           "working space is scene-linear Rec.709)";
}

struct OcioInputTransform::Impl {
    std::string inputColorSpace;
    std::string identity;
    // Keeps the configuration the processor was built from alive for the
    // processor's whole life.
    OCIO::ConstConfigRcPtr config;
    OCIO::ConstCPUProcessorRcPtr cpu;
};

OcioInputTransform::OcioInputTransform(const OcioConfigSnapshot& snapshot, std::string workingSpace,
                                       std::string inputColorSpace)
    : impl_(std::make_unique<Impl>()) {
    const std::string context = "input transform '" + inputColorSpace + "' -> working '" + workingSpace + "'";
    requireSceneLinearRec709Impl(snapshot.impl_->config, snapshot.impl_->reference, workingSpace, context);
    const OCIO::ConstProcessorRcPtr processor =
        buildInputProcessorImpl(snapshot.impl_->config, snapshot.impl_->reference, workingSpace, inputColorSpace);
    impl_->inputColorSpace = std::move(inputColorSpace);
    impl_->config = snapshot.impl_->config;
    impl_->cpu = processor->getDefaultCPUProcessor();
    // Snapshot content identity + the resolved processor: a config reload can
    // never reuse this conversion, and this conversion can never be attributed
    // to different config bytes.
    impl_->identity =
        snapshot.identity() + "|" + workingSpace + "|" + impl_->inputColorSpace + "|" + processor->getCacheID();
}

OcioInputTransform::OcioInputTransform(std::string configPath, std::string workingSpace, std::string inputColorSpace)
    : OcioInputTransform(OcioConfigSnapshot(std::move(configPath)), std::move(workingSpace),
                         std::move(inputColorSpace)) {}

OcioInputTransform::~OcioInputTransform() = default;
OcioInputTransform::OcioInputTransform(OcioInputTransform&&) noexcept = default;
OcioInputTransform& OcioInputTransform::operator=(OcioInputTransform&&) noexcept = default;

void OcioInputTransform::apply(CpuImage& image) const {
    if (image.width() <= 0 || image.height() <= 0) {
        return;
    }
    // Only the identified root RGB channels are colour. The processor reads and
    // writes exactly those three planes and every other declared channel —
    // alpha, an auxiliary pass — is left bit-for-bit as the decoder produced
    // it: an RGBA descriptor would push alpha through OCIO's op chain, which
    // perturbs even a pass-through at the 1e-6 level, and a fixed three-channel
    // packed descriptor would convert whatever channels happen to sit first.
    // The planes are addressed through the raster's own channel stride, so a
    // multi-channel interleaved image needs no repacking, no allocation and no
    // per-pixel name lookup. Alpha is deliberately not described: the internal
    // straight-alpha contract is exact.
    const std::array<int, 4> indices = image.rgbaIndices();
    if (indices[0] < 0 || indices[1] < 0 || indices[2] < 0) {
        return;  // no complete root RGB: this image carries no colour to convert
    }
    float* base = image.data();
    const auto channelStride = static_cast<std::ptrdiff_t>(image.channelCount() * sizeof(float));
    const auto rowStride = channelStride * image.width();
    const OCIO::PlanarImageDesc desc(base + indices[0], base + indices[1], base + indices[2], nullptr, image.width(),
                                     image.height(), OCIO::BIT_DEPTH_F32, channelStride, rowStride);
    impl_->cpu->apply(desc);
    image.setColorInterpretation(ColorInterpretation::SceneLinear);
}

const std::string& OcioInputTransform::inputColorSpace() const noexcept {
    return impl_->inputColorSpace;
}
const std::string& OcioInputTransform::identity() const noexcept {
    return impl_->identity;
}

OcioGpuProgram buildInputTransformImageGpu(const std::string& configPath, const std::string& workingSpace,
                                           const std::string& inputColorSpace) {
    const OcioConfigSnapshot snapshot(configPath);
    return snapshot.inputTransformImageGpu(workingSpace, inputColorSpace);
}

OcioGpuProgram buildInputTransformGpu(const std::string& configPath, const std::string& workingSpace,
                                      const std::string& inputColorSpace) {
    const OcioConfigSnapshot snapshot(configPath);
    return snapshot.inputTransformGpu(workingSpace, inputColorSpace);
}

}  // namespace nemo::media
