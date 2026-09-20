#pragma once

// Delivery output adapter (issue #94): what a Write node's authored settings
// mean as real output, and the encoders that produce it.
//
// Ownership, deliberately split:
//   - the COLOR of a delivered frame is the OpenColorIO adapter's job
//     (ViewingTransform.hpp). A delivery's output color is one of: the evaluated
//     values verbatim, the project's own delivery transform, a named color
//     space of the project config, or a named display/view — plus an optional
//     LUT file applied after that base. All of it resolves through the same
//     retained OCIO snapshot, so a frame range opens the configuration once,
//     and none of it is applied per frame by a second interpretation policy.
//     `DeliveryColorProcessor` is the retained, job-scoped handle to that
//     conversion.
//   - the STILL encoder is the OpenImageIO adapter's job (ImageIO.hpp).
//     `writeDeliveryImage` states the delivered geometry (data window, format,
//     pixel aspect, declared channels) through that one writer; there is no
//     second EXR implementation.
//   - the MOVIE encoder is FFmpeg's job, owned here: MOV as Apple ProRes 422 /
//     4444 / 4444 XQ, MP4 as H.264. `DeliveryMovieWriter` is ONE streaming
//     session per file — frames are converted, submitted and muxed as they
//     arrive, so memory stays bounded by a single frame and no whole-movie
//     buffer exists. No audio is written and none is claimed: a delivery movie
//     is a video track and nothing else.
//
// Nothing here runs during graph evaluation, and nothing here is a viewer
// path: a delivery never consumes the viewer's compact cached pixels (its lossy
// representation would silently reduce the delivered image) and never writes
// through the viewer's chunk encoder.
//
// Alpha: every conversion in this module leaves alpha and auxiliary channels
// bit-for-bit as they were and never scales RGB by alpha — there is no
// premultiply and no unpremultiply anywhere, so a straight-alpha source keeps
// the RGB hidden under zero alpha exactly as authored. Movie formats that carry
// no alpha plane store none (an opaque alpha is synthesized for them).

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "nemo/core/document/Document.hpp"
#include "nemo/core/evaluation/Image.hpp"
#include "nemo/media/ImageIO.hpp"

namespace nemo::media {

// Movie failures identify the offending output path and setting (repo rule:
// errors identify the offending relationship). Declared here rather than
// borrowed from the viewer encoder's error type: this adapter is part of the
// always-built module and must not pull the GPU/FFmpeg encode header in.
struct DeliveryMovieError : std::runtime_error {
    DeliveryMovieError(std::string path, std::string message)
        : std::runtime_error("delivery movie: " + path + ": " + std::move(message)), path(std::move(path)) {}

    std::string path;
};

// The authored output settings of one Write node (issue #94). Every field is a
// plain value: a delivery job freezes this struct at submit, so later edits
// cannot change what an accepted job writes.
struct DeliveryOutputOptions {
    // "exr" (one still per frame), "mov" (ProRes), or "mp4" (H.264).
    // Format-specific options are ignored by the other formats.
    std::string fileType{"exr"};
    // EXR storage precision ("half" or "float").
    OutputPrecision precision{OutputPrecision::Half};
    // EXR compression by OIIO's own name; the closed inventory of
    // `isSupportedExrCompression` is the only accepted set.
    std::string compression{"zip"};
    // ProRes profile of a MOV: "422", "4444" or "4444xq". The two 4444
    // profiles carry the alpha channel; 422 stores none.
    std::string profile{"422"};
    // Movie frames per second; FFmpeg derives a rational time base. Ignored by EXR.
    double frameRate{24.0};
    // MP4 quality: the H.264 encoder's target bit rate in kilobits per second.
    int bitrateKbps{20000};
    // What the delivered samples mean:
    //   "raw"        the evaluated values verbatim (the EXR default: no color
    //                transform at all, which is what an existing document's
    //                delivery did before this choice existed),
    //   "project"    the project's own delivery transform (ColorPolicy),
    //   "colorspace" working space -> the color space named by outputTransform,
    //   "display"    working space -> the display/view named by outputTransform.
    std::string colorMode{"raw"};
    // The color space (mode "colorspace") or "display/view" (mode "display").
    std::string outputTransform;
    // Optional LUT applied AFTER the base transform above, to the primary RGB
    // channels only. Empty means no LUT. A "raw" delivery with a LUT therefore
    // writes the evaluated values through the LUT, which is the only transform
    // such a delivery asks for.
    std::string lutFile;
};

// Settings-only validation; no filesystem access or configuration loading.
// Returns an empty string on success. DeliveryColorProcessor resolves the
// config, named transform and LUT on the job worker.
[[nodiscard]] std::string validateDeliveryOutput(const DeliveryOutputOptions& options);

// Whether validated output settings store an alpha plane. Uses the encoder's
// own layout, so an explicit RGBA selection cannot promise discarded alpha.
[[nodiscard]] bool deliveryStoresAlpha(const DeliveryOutputOptions& options);

// The choices one delivery color mode offers, straight from the project
// config: the config's own color spaces for "colorspace", its own "display/view"
// pairs for "display". "raw" and "project" have nothing to choose (raw applies
// no transform; project takes the policy's delivery transform), so they return
// an empty list. Throws OcioException naming the config or the mode otherwise.
[[nodiscard]] std::vector<std::string> deliveryTransformChoices(const std::string& configPath, const std::string& mode);

// The retained color conversion of ONE delivery job: the base transform and the
// optional LUT, resolved once from one OCIO snapshot and reused for every frame
// (a range never re-opens the config, never re-resolves a name and never builds
// a processor per frame). Move-only; the processors live as long as this object.
class DeliveryColorProcessor {
public:
    // Raw without a LUT loads nothing; other combinations resolve configPath
    // (empty means the OCIO environment, as for source input transforms).
    // Throws OcioException naming the config, the transform or the LUT when the
    // settings cannot be resolved.
    DeliveryColorProcessor(const DeliveryOutputOptions& options, const ColorPolicy& policy,
                           const std::string& configPath);
    ~DeliveryColorProcessor();
    DeliveryColorProcessor(DeliveryColorProcessor&&) noexcept;
    DeliveryColorProcessor& operator=(DeliveryColorProcessor&&) noexcept;
    DeliveryColorProcessor(const DeliveryColorProcessor&) = delete;
    DeliveryColorProcessor& operator=(const DeliveryColorProcessor&) = delete;

    // Applies the delivery's color in place: the identified primary RGB
    // channels only. Alpha and every auxiliary channel are untouched, and no
    // value is ever scaled by alpha (see the alpha note above). A raw delivery
    // without a LUT changes nothing at all.
    void apply(CpuImage& image) const;

    // The resolved transform chain ("working 'x' -> view 'y' then LUT 'z'"),
    // for job diagnostics.
    [[nodiscard]] const std::string& description() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Writes one delivered still through the shared image writer: the image's
// declared channels, the description's data window origin, format (display
// window) and pixel aspect, with the requested storage precision and
// compression. `fileType` must be "exr"; a movie is written by
// DeliveryMovieWriter, and a single still is not one. Throws ImageIoException
// naming the path (an unsupported compression names the compression), or
// DeliveryMovieError naming the path when a still write was asked for a movie
// format.
void writeDeliveryImage(const std::string& path, const CpuImage& image, const ImageDescription& description,
                        const DeliveryOutputOptions& options);

// ONE streaming delivery movie, written to `temporaryPath` (the caller owns the
// final path and its atomic publication: this writer never renames and never
// touches a path other than the one it was given).
//
// Frames are appended as they arrive and are encoded from a single reusable
// conversion buffer, so peak memory is a bounded number of frames regardless of
// the movie's length — a whole-movie buffer is never built. `finish()` flushes
// the encoder, drains the delayed frames and writes the container trailer;
// only then is `temporaryPath` a complete, readable movie. Destroying the
// writer without `finish()` abandons the output and removes the partial file,
// which is what a cancelled delivery needs: no partial movie is ever left where
// a finished one would be.
//
// The construction-time `description` is the frame contract of the movie — its
// format (the delivered raster geometry) and its channel naming. Every appended
// image must match it; a mismatch is refused by name rather than encoded.
class DeliveryMovieWriter {
public:
    // Validates the settings and the description, resolves the encoder (ProRes
    // for MOV, H.264 for MP4) and opens the container. Throws
    // DeliveryMovieError naming the path, the format, the profile or the codec
    // when the requested movie cannot be produced by this build (including a
    // raster dimension the chosen chroma layout cannot store). Nothing is
    // written to a final path.
    DeliveryMovieWriter(const std::string& temporaryPath, const ImageDescription& description,
                        const DeliveryOutputOptions& options);
    ~DeliveryMovieWriter();
    DeliveryMovieWriter(DeliveryMovieWriter&&) noexcept;
    DeliveryMovieWriter& operator=(DeliveryMovieWriter&&) noexcept;
    DeliveryMovieWriter(const DeliveryMovieWriter&) = delete;
    DeliveryMovieWriter& operator=(const DeliveryMovieWriter&) = delete;

    // Encodes one frame. Expected: the delivered raster for the frame, already
    // color-processed by DeliveryColorProcessor (this writer applies no color
    // transform of its own — it converts RGB to the codec's YUV layout and
    // nothing else). Throws DeliveryMovieError when the frame cannot be
    // encoded.
    void append(const CpuImage& image);

    // Completes the movie: flushes the codec, writes the trailer and closes the
    // container. Throws DeliveryMovieError when the movie could not be
    // completed (the temporary output is removed in that case). Calling
    // `finish()` twice is a no-op.
    void finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nemo::media
