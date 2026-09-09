#pragma once

// Viewer resolution policy (issue #11, spec section 8).
//
// Auto / Full / Half / Quarter resolve to a sampling scale (1/2/4) for the
// evaluation request. Auto picks the level from the image's physical
// display area — aspect-corrected, accounting for zoom — with stable
// hysteresis: one-pixel panel resizes must not continually rebuild
// representations. Explicit modes override Auto. ROI is a separate
// property of the request (EvaluationRequest.region stays in
// full-resolution coordinates); this policy only picks the sampling
// density.
//
// Not thread-safe by design: one policy object per viewer panel, driven
// from the render/UI thread that also forms requests.

#include <algorithm>
#include <cmath>

namespace nemo {

enum class ViewerResolution { Auto, Full, Half, Quarter };

[[nodiscard]] inline const char* viewerResolutionName(ViewerResolution mode) {
    switch (mode) {
    case ViewerResolution::Full:
        return "full";
    case ViewerResolution::Half:
        return "half";
    case ViewerResolution::Quarter:
        return "quarter";
    case ViewerResolution::Auto:
        break;
    }
    return "auto";
}

// Aspect-corrected physical size of an image fitted into the panel, in
// panel pixels: the display area the whole image covers at 1:1 sampling.
// Pixel aspect widens the image's display width (spec section 10.4 image
// contract). Exposed so the UI can reason about the fitted representation
// without duplicating the arithmetic.
struct ViewerFit {
    double width{0.0};
    double height{0.0};

    [[nodiscard]] double area() const { return width * height; }
};

[[nodiscard]] inline ViewerFit aspectFit(int imageWidth, int imageHeight, double pixelAspect, double physicalWidth,
                                         double physicalHeight) {
    ViewerFit fit;
    if (imageWidth <= 0 || imageHeight <= 0 || physicalWidth <= 0.0 || physicalHeight <= 0.0) {
        return fit;
    }
    const double pa = pixelAspect > 0.0 ? pixelAspect : 1.0;
    // Fitted size preserves the image's DISPLAY aspect
    // (imageWidth*pixelAspect)/imageHeight inside the panel's pixel area.
    const double scale = std::min(physicalWidth / (static_cast<double>(imageWidth) * pa),
                                  physicalHeight / static_cast<double>(imageHeight));
    fit.width = static_cast<double>(imageWidth) * pa * scale;
    fit.height = static_cast<double>(imageHeight) * scale;
    return fit;
}

class ViewerResolutionPolicy {
public:
    // Sampling scale (1 = Full, 2 = Half, 4 = Quarter) for the request.
    //
    // Decision model (spec section 8 "displayed image area in physical
    // pixels"): with the image aspect-fitted into the panel at zoom z, the
    // reduction factor between the shown image region and the pixels the
    // panel can display is
    //
    //     r = imageArea / (z² * fittedArea)
    //
    // r is the image pixels per panel pixel over the visible region: r <= 1
    // (image fits, or zoomed in) keeps Full; a scale-2 representation gives
    // the panel 1 sample per 4 image pixels, so Half only pays off when
    // r > 4 and Quarter when r > 16. Hysteresis holds the previous level
    // while r stays within the switching band (20% around each boundary),
    // so a one-pixel resize near a boundary cannot oscillate
    // representations.
    [[nodiscard]] int resolve(ViewerResolution mode, int imageWidth, int imageHeight, double pixelAspect,
                              double physicalWidth, double physicalHeight, double zoom = 1.0) const;

    // Last level Auto resolved (hysteresis state), for UI evidence of the
    // effective preview scale. 0 until the first Auto resolve.
    [[nodiscard]] int lastAutoScale() const { return lastAutoScale_; }

    // Forgets the hysteresis state (a new panel/geometry session).
    void reset() { lastAutoScale_ = 0; }

private:
    mutable int lastAutoScale_{0};
};

inline int ViewerResolutionPolicy::resolve(ViewerResolution mode, int imageWidth, int imageHeight, double pixelAspect,
                                           double physicalWidth, double physicalHeight, double zoom) const {
    if (mode == ViewerResolution::Full) {
        return 1;
    }
    if (mode == ViewerResolution::Half) {
        return 2;
    }
    if (mode == ViewerResolution::Quarter) {
        return 4;
    }

    const ViewerFit fit = aspectFit(imageWidth, imageHeight, pixelAspect, physicalWidth, physicalHeight);
    const double fittedArea = fit.area();
    if (fittedArea <= 0.0) {
        // No geometry yet: minimize work; the first real resolve sets the
        // hysteresis base.
        return 4;
    }
    const double z = zoom > 0.0 ? zoom : 1.0;
    const double imageArea = static_cast<double>(imageWidth) * static_cast<double>(imageHeight);
    // Image pixels shown per panel pixel over the visible region.
    const double reduction = imageArea / (z * z * fittedArea);

    auto baseLevel = [](double r) { return r > 16.0 ? 4 : (r > 4.0 ? 2 : 1); };
    int level = baseLevel(reduction);

    // Hysteresis: switching between adjacent levels requires crossing the
    // boundary (the larger level squared) with a 20% margin in the
    // direction of the switch; inside the band the previous level holds.
    constexpr double kBand = 0.2;
    if (lastAutoScale_ != 0 && level != lastAutoScale_) {
        const int larger = std::max(level, lastAutoScale_);
        const double boundary = static_cast<double>(larger) * static_cast<double>(larger);
        if (level > lastAutoScale_) {
            if (reduction < boundary * (1.0 + kBand)) {
                level = lastAutoScale_;
            }
        } else if (reduction >= boundary * (1.0 - kBand)) {
            level = lastAutoScale_;
        }
    }
    lastAutoScale_ = level;
    return level;
}

}  // namespace nemo
