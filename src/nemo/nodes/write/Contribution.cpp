// Write: the delivery sink (issue #94, stories 72-73).
//
// A Write node NEVER performs file I/O during evaluation. Its authored settings
// describe a delivery REQUEST — which file, which frames, which still or movie
// format, and how its color is output — and the image it carries through is the
// connected input unchanged. An explicit delivery job (eval::DeliveryQueue)
// reads that authored state and performs the write; evaluating the node itself, as the
// viewer, the CLI render path or a dependency of another node does, must be a
// pure read of authored document state. That is what keeps a render
// reproducible, undo/redo independent of disk contents, and a delivery failure
// from becoming an evaluation failure.
//
// The role is therefore Delivery, not Output: a Write branches from any
// compatible point of the graph instead of defining the network's result, and
// it has no output ports — nothing downstream consumes a delivery request.

#include "nemo/nodes/Builtins.hpp"

#include <cstdint>
#include <string>

#include "nemo/core/evaluation/EffectCpu.hpp"
#include "nemo/nodes/Common.hpp"

namespace nemo::nodes {
namespace {

NodeDescriptor writeDescriptor() {
    return NodeDescriptor{.type = "write",
                          .displayName = "Write",
                          .group = "I/O",
                          .isOutput = false,
                          .isDeliverySink = true,
                          .implementationVersion = 1,
                          .inputs = {{PortKind::Image, "image", false}},
                          .outputs = {},
                          .parameters =
                              {
                                  // Which channels the delivery writes (issue
                                  // #102). `all` — the default — delivers every
                                  // channel the connected image carries, which
                                  // is exactly what a Write delivered before the
                                  // selection existed; the named sets deliver
                                  // the target's own primary channels for those
                                  // roles. The delivery seam resolves the names
                                  // from the image's own description (never a
                                  // spelling this schema invents) and refuses a
                                  // selection the image carries no channel for.
                                  {.name = "channels",
                                   .type = ParameterType::Choice,
                                   .defaultValue = ParameterValue{ChoiceValue{"all"}},
                                   .choices = {"all", "rgb", "rgba", "alpha"},
                                   .label = "Channels",
                                   .section = "File",
                                   .editor = {}},
                                  // The delivery editor mounts on the `file` row: the host selects an editor from a
                                  // parameter's `editor` metadata and never consumes the key of the row that
                                  // carries the id, so exactly one row names it (issue #94).
                                  {.name = "file",
                                   .type = ParameterType::String,
                                   .defaultValue = ParameterValue{std::string{}},
                                   .label = "File",
                                   .section = "File",
                                   .editor = "nemo.write.delivery"},
                                  // A still sequence (OpenEXR) or one movie file
                                  // (MOV/MP4). The choice is authored verbatim
                                  // and the delivery seam refuses a
                                  // format/extension mismatch instead of
                                  // rewriting the path (issue #94).
                                  {.name = "fileType",
                                   .type = ParameterType::Choice,
                                   .defaultValue = ParameterValue{ChoiceValue{"exr"}},
                                   .choices = {"exr", "mov", "mp4"},
                                   .label = "File Type",
                                   .section = "File",
                                   .editor = {}},
                                  {.name = "createDirectories",
                                   .type = ParameterType::Boolean,
                                   .defaultValue = ParameterValue{true},
                                   .label = "Create Directories",
                                   .section = "File",
                                   .editor = {}},
                                  {.name = "overwrite",
                                   .type = ParameterType::Boolean,
                                   .defaultValue = ParameterValue{false},
                                   .label = "Overwrite Files",
                                   .section = "File",
                                   .editor = {}},
                                  {.name = "frameFirst",
                                   .type = ParameterType::Integer,
                                   .defaultValue = ParameterValue{std::int64_t{1}},
                                   .label = "First",
                                   .section = "Frames",
                                   .editor = {},
                                   .row = "Range"},
                                  {.name = "frameLast",
                                   .type = ParameterType::Integer,
                                   .defaultValue = ParameterValue{std::int64_t{1}},
                                   .label = "Last",
                                   .section = "Frames",
                                   .editor = {},
                                   .row = "Range"},
                                  {.name = "frameOffset",
                                   .type = ParameterType::Integer,
                                   .defaultValue = ParameterValue{std::int64_t{0}},
                                   .label = "File Offset",
                                   .section = "Frames",
                                   .editor = {},
                                   .row = "Numbering"},
                                  {.name = "precision",
                                   .type = ParameterType::Choice,
                                   .defaultValue = ParameterValue{ChoiceValue{"half"}},
                                   .choices = {"half", "float"},
                                   .label = "Precision",
                                   .section = "Format",
                                   .editor = {}},
                                  {.name = "compression",
                                   .type = ParameterType::Choice,
                                   .defaultValue = ParameterValue{ChoiceValue{"zip"}},
                                   // The frozen EXR inventory. `dwaa` is the one
                                   // LOSSY option (DWA quantizes RGB at the
                                   // encoder's default level): it is a storage
                                   // choice on a full-quality job, never a
                                   // reduced-quality evaluation.
                                   .choices = {"zip", "piz", "rle", "none", "dwaa"},
                                   .label = "Compression",
                                   .section = "Format",
                                   .editor = {}},
                                  // --------------------------------------------------------
                                  // Movie and output-color settings (issue #94).
                                  // Every key below is a plain authored parameter:
                                  // the node states the request and the delivery
                                  // seam validates it (media::validateDeliveryOutput),
                                  // so nothing here touches a file, a config or a
                                  // directory.
                                  // --------------------------------------------------------
                                  // MOV codec profile: ProRes 422, 4444 or
                                  // 4444 XQ. One value per movie, authored beside
                                  // the movie's own frame rate.
                                  {.name = "profile",
                                   .type = ParameterType::Choice,
                                   .defaultValue = ParameterValue{ChoiceValue{"422"}},
                                   .choices = {"422", "4444", "4444xq"},
                                   .label = "Profile",
                                   .section = "Format",
                                   .editor = {},
                                   .row = "Movie"},
                                  // MOV/MP4 frame rate. The delivered sequence
                                  // keeps its authored frames; this is how many
                                  // of them one movie second carries.
                                  {.name = "frameRate",
                                   .type = ParameterType::Float,
                                   .defaultValue = ParameterValue{24.0},
                                   .minimum = 0.0,
                                   .step = 1.0,
                                   .label = "Frame Rate",
                                   .section = "Format",
                                   .editor = {},
                                   .row = "Movie"},
                                  // MP4 encoding bitrate in kbit/s.
                                  {.name = "bitrateKbps",
                                   .type = ParameterType::Integer,
                                   .defaultValue = ParameterValue{std::int64_t{20000}},
                                   .minimum = 0.0,
                                   .step = 1000.0,
                                   .label = "Bitrate (kbps)",
                                   .section = "Format",
                                   .editor = {},
                                   .row = "Movie"},
                                  // Output color: `raw` writes the working
                                  // pixels unchanged (the existing behavior),
                                  // and only an explicit choice runs a delivery
                                  // transform.
                                  {.name = "colorMode",
                                   .type = ParameterType::Choice,
                                   .defaultValue = ParameterValue{ChoiceValue{"raw"}},
                                   .choices = {"raw", "project", "colorspace", "display"},
                                   .label = "Color Mode",
                                   .section = "Color",
                                   .editor = {},
                                   .row = "Transform"},
                                  // A colorspace name (`colorspace`) or a
                                  // "display/view" pair (`display`); empty for
                                  // `raw`/`project`, which resolve elsewhere.
                                  {.name = "outputTransform",
                                   .type = ParameterType::String,
                                   .defaultValue = ParameterValue{std::string{}},
                                   .label = "Output Transform",
                                   .section = "Color",
                                   .editor = {},
                                   .row = "Transform"},
                                  // An optional cube LUT applied after the selected
                                  // output transform, to primary RGB only.
                                  {.name = "lutFile",
                                   .type = ParameterType::String,
                                   .defaultValue = ParameterValue{std::string{}},
                                   .label = "LUT",
                                   .section = "Color",
                                   .editor = {}},
                              },
                          .capabilities = builtinCapabilities()};
}

// Delivery adds no pixels of its own: the delivered image IS the connected
// image, which is what makes a Write a legal evaluation target (the planner
// describes it as its input's image, and the shared description default already
// states that inheritance). A missing required input fails naming the node,
// through the shared declared-port accessor every effect reads.
CpuImage executeWrite(const CpuNodeContext& context) {
    return requiredImageInput(context, 0, "write requires a connected image input");
}

}  // namespace

NodeContribution writeContribution() {
    NodeContribution contribution;
    contribution.descriptor = writeDescriptor();
    // Delivery, not Output: the network's result is still defined solely by an
    // Output node, and not Viewer: a Write carries real pixels.
    contribution.role = NodeRole::Delivery;
    contribution.cpu = CpuImplementation{contribution.descriptor.implementationVersion, &executeWrite};
    // No `describe` rule and no `ownsChannelLayout`: the delivered image is the
    // inherited image, so every named channel travels through untouched and the
    // shared auxiliary preservation is a no-op for this pass-through.
    contribution.editors = {NodeEditorContribution{
        .id = "nemo.write.delivery",
        .source = "qrc:/qt/qml/Nemo/qml/WriteDeliveryEditor.qml",
        // Every authored delivery key renders exactly once: the grouped editor
        // presents all of them itself (channels / file / frames / format / color /
        // flags), so the generic fallback rows stay free for an unavailable editor.
        .consumes = {"channels", "file", "fileType", "createDirectories", "overwrite", "frameFirst", "frameLast",
                     "frameOffset", "precision", "compression", "profile", "frameRate", "bitrateKbps", "colorMode",
                     "outputTransform", "lutFile"},
        .presentation = "section"}};
    return contribution;
}

}  // namespace nemo::nodes
