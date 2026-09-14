#include "nemo/nodes/Builtins.hpp"

#include <cmath>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/core/evaluation/Params.hpp"
#include "nemo/core/evaluation/SourceRequest.hpp"
#include "nemo/nodes/Common.hpp"

namespace nemo::nodes {
namespace {

NodeDescriptor sourceDescriptor() {
    // Node-scoped Read settings. Names/defaults are the resolver's frozen
    // semantic owner (evaluation/SourceRequest.hpp): the catalog declares the
    // same keys so authoring/validation/presentation cannot drift from the one
    // effective-request owner.
    const auto choice = [](std::string_view name, std::string_view label, std::string_view section, std::string value,
                           std::vector<std::string> choices) {
        return ParameterSpec{.name = std::string{name},
                             .type = ParameterType::Choice,
                             .defaultValue = ParameterValue{ChoiceValue{std::string{value}}},
                             .choices = std::move(choices),
                             .label = std::string{label},
                             .section = std::string{section},
                             .editor = {}};
    };
    const auto integer = [](std::string_view name, std::string_view label, std::string_view section, std::int64_t value,
                            bool nonzero = false) {
        return ParameterSpec{.name = std::string{name},
                             .type = ParameterType::Integer,
                             .defaultValue = ParameterValue{value},
                             .label = std::string{label},
                             .section = std::string{section},
                             .editor = {},
                             .nonzero = nonzero};
    };
    return NodeDescriptor{
        .type = "source",
        .displayName = "Read",
        .group = "I/O",
        .implementationVersion = 2,
        .inputs = {},
        .outputs = {{PortKind::Image, "color"}},
        .parameters =
            {{.name = "source",
              .type = ParameterType::String,
              .defaultValue = ParameterValue{std::string{}},
              .label = "File",
              .section = "Source",
              .editor = "nemo.read.source"},
             choice(kReadParamRangeMode, "Range Mode", "Timing", "auto", {"auto", "custom"}),
             integer(kReadParamRangeFirst, "First Frame", "Timing", 0),
             integer(kReadParamRangeLast, "Last Frame", "Timing", 0),
             integer(kReadParamFrameOffset, "Offset", "Timing", 0),
             // Step is a nonzero signed integer; the catalog's nonzero
             // constraint is the generic-edit validator, and no bounds are
             // declared so a typed value is never clamped.
             integer(kReadParamFrameStep, "Step", "Timing", 1, /*nonzero=*/true),
             choice(kReadParamBeforePolicy, "Before", "Policies", "error", {"error", "hold", "black"}),
             choice(kReadParamAfterPolicy, "After", "Policies", "error", {"error", "hold", "black"}),
             choice(kReadParamMissingPolicy, "Missing Frames", "Policies", "error", {"error", "black"}),
             choice(kReadParamInputTransform, "Input Transform", "Color", "auto", {"auto", "explicit", "raw"}),
             {.name = std::string{kReadParamInputColorSpace},
              .type = ParameterType::String,
              .defaultValue = ParameterValue{std::string{}},
              .label = "Input Color Space",
              .section = "Color",
              .editor = {}},
             choice(kReadParamAlphaMode, "Alpha Mode", "Color", "auto", {"auto", "straight", "premultiplied"}),
             choice(kReadParamSourceTransfer, "Transfer", "Encoding Hints", "auto",
                    {"auto", "bt709", "srgb", "gamma22", "gamma28", "linear"}),
             choice(kReadParamSourcePrimaries, "Primaries", "Encoding Hints", "auto", {"auto", "bt709"}),
             choice(kReadParamSourceMatrix, "Matrix", "Encoding Hints", "auto", {"auto", "bt709", "bt601"}),
             choice(kReadParamSourceRange, "Range", "Encoding Hints", "auto", {"auto", "limited", "full"}),
             choice(kReadParamSourceChromaLocation, "Chroma Location", "Encoding Hints", "auto", {"auto", "left"})},
        .capabilities = builtinCapabilities(true)};
}

// Real source media (issue #11). The reference lives in the Document; the pixels
// come from the provider. There is NO synthetic fallback: an unresolved or
// unprovided source is an explicit evaluation error that identifies the node.
CpuImage executeSource(const CpuNodeContext& context) {
    const NodeInstance& node = context.node;
    const EvaluationRequest& request = context.request;
    // One resolution owns mapping, coverage and policy (issue #75): the node's
    // own mapping replaces the shared reference's, never composes with it, so
    // offset/step apply exactly once.
    const EffectiveSourceRequest source = resolveSourceRequest(context.document, node, request.localTime);
    const std::string& key = source.sourceKey;
    context.effectiveParams["source"] = std::string(key);
    context.effectiveParams["sourcePath"] = source.path;
    context.effectiveParams["frame"] = source.sourceFrame;
    if (source.policyError) {
        // The resolver never throws for a policy decision; the executor raises
        // the node-identifying error here, before any frame is opened, using the
        // one core-owned diagnostic so every executor reports the same source
        // relationship (before/after range vs a missing member file).
        failNode(node, sourcePolicyProblem(source));
    }
    if (context.sources == nullptr) {
        failNode(node, "source '" + key + "' (" + source.path +
                           ") requires a decode provider; this "
                           "executor cannot evaluate real media and never substitutes synthetic content");
    }
    CpuImage decoded;
    try {
        // Transparent black stays the provider's job: it owns the raster layout,
        // pixel aspect and any retained-resource path, and the request carries
        // the decision rather than the pixels.
        decoded = context.sources->frame(context.document, source, request);
    } catch (const EvaluationException&) {
        throw;
    } catch (const std::exception& error) {
        failNode(node, "source provider failed for '" + key + "' at frame " + std::to_string(source.readFrame) + ": " +
                           error.what());
    }
    const int expectedWidth = scaledDimension(request.region.width, request.samplingScale);
    const int expectedHeight = scaledDimension(request.region.height, request.samplingScale);
    if (decoded.width() != expectedWidth || decoded.height() != expectedHeight) {
        failNode(node, "source '" + key + "' decoded raster " + std::to_string(decoded.width()) + "x" +
                           std::to_string(decoded.height()) + " does not cover the requested raster " +
                           std::to_string(expectedWidth) + "x" + std::to_string(expectedHeight) +
                           " (full-resolution region at sampling scale " + std::to_string(request.samplingScale) + ")");
    }
    // Real source pixel aspect travels with the decoded frame so downstream
    // coordinate math honors anamorphic media. A non-finite or non-positive
    // value is a node error, never silently replaced by square pixels (mirrors
    // the GPU source session's validation). Returning the decoded image by value
    // moves its storage, so the provider's layout and pixel aspect are adopted
    // without a pixel copy.
    const float pixelAspect = decoded.layout().pixelAspect;
    if (!std::isfinite(pixelAspect) || pixelAspect <= 0.0F) {
        failNode(node, "source '" + key + "' reports an invalid pixel aspect (" + std::to_string(pixelAspect) + ")");
    }
    return decoded;
}

}  // namespace

NodeContribution sourceContribution() {
    NodeContribution contribution;
    contribution.descriptor = sourceDescriptor();
    contribution.role = NodeRole::Source;
    contribution.cpu = CpuImplementation{contribution.descriptor.implementationVersion, &executeSource};
    // The Read node's node-local file control is a registered namespaced editor,
    // selected from catalog `editor` metadata; the inspector host owns its
    // placement. It is an aggregate editor: it declares the node parameters it
    // owns so exactly one control renders per setting, and it presents the file,
    // summary, timing and color groups itself (full-width "section").
    contribution.editors = {NodeEditorContribution{
        .id = "nemo.read.source",
        .source = "qrc:/qt/qml/Nemo/qml/ReadSourceEditor.qml",
        .consumes = {"source", "rangeMode", "rangeFirst", "rangeLast", "frameOffset", "frameStep", "beforePolicy",
                     "afterPolicy", "missingPolicy", "inputTransform", "inputColorSpace", "alphaMode", "sourceTransfer",
                     "sourcePrimaries", "sourceMatrix", "sourceRange", "sourceChromaLocation"},
        .presentation = "section"}};
    return contribution;
}

}  // namespace nemo::nodes
