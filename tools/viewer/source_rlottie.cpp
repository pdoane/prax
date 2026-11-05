// rlottie, Samsung's Lottie player, as a source (source_rlottie.h), against which the viewer compares lib/lottie.

#include "tools/viewer/source_rlottie.h"

#include "tools/viewer/documents.h"

#include "rlottie.h"

#include <algorithm>
#include <cmath>

namespace viewer {

struct RlottieSource {
    std::unique_ptr<rlottie::Animation> animation;
};

std::shared_ptr<RlottieSource> loadRlottie(const std::filesystem::path& path) {
    std::shared_ptr<RlottieSource> rlottie = std::make_shared<RlottieSource>();
    rlottie->animation = rlottie::Animation::loadFromFile(path.string(), false);
    return rlottie->animation != nullptr ? rlottie : nullptr;
}

PrVec2 rlottieSize(const RlottieSource& rlottie) {
    size_t width = 0;
    size_t height = 0;
    rlottie.animation->size(width, height);
    return PrVec2{float(width), float(height)};
}

// Returns the frame that the engine draws at the time, looping over the animation's length, taken whole because rlottie
// draws only whole frames. The length is the out point minus the in point. rlottie's frame count rounds the length up.
double rlottieFrameAt(const RlottieSource& rlottie, double time) {
    double frames = rlottie.animation->duration() * rlottie.animation->frameRate();
    return std::floor(frames > 0 ? std::fmod(time * rlottie.animation->frameRate(), frames) : 0);
}

void rlottiePixels(RlottieSource& rlottie, PrVec2 viewport, uint32_t width, uint32_t height, float pixelRatio,
        double frame, std::vector<uint8_t>& bgra) {
    // rlottie scales the animation to its surface, so it renders the whole frame at the size that the viewport fits it
    // to, and the part that the pixels hold is copied to where the frame lies.
    PrRect fitted = fittedIn(rlottieSize(rlottie), viewport);
    long left = std::lround(fitted.left * pixelRatio);
    long top = std::lround(fitted.top * pixelRatio);
    size_t drawnWidth = size_t(std::max(0L, std::lround(fitted.right * pixelRatio) - left));
    size_t drawnHeight = size_t(std::max(0L, std::lround(fitted.bottom * pixelRatio) - top));
    bgra.assign(size_t(width) * height * 4, 0);
    if (drawnWidth > 0 && drawnHeight > 0) {
        std::vector<uint8_t> drawn(drawnWidth * drawnHeight * 4);
        rlottie::Surface surface(reinterpret_cast<uint32_t*>(drawn.data()), drawnWidth, drawnHeight, drawnWidth * 4);
        rlottie.animation->renderSync(size_t(frame), surface);
        size_t x = size_t(std::max(0L, left));
        size_t columns = x < width ? std::min(drawnWidth, size_t(width) - x) : 0;
        for (size_t y = 0; y < drawnHeight && size_t(top) + y < height && columns > 0; ++y)
            std::copy_n(drawn.data() + y * drawnWidth * 4, columns * 4, bgra.data() + ((size_t(top) + y) * width + x) * 4);
    }
    // Over white, as the player pages show it.
    for (size_t i = 0; i < bgra.size(); i += 4) {
        uint8_t cover = uint8_t(255 - bgra[i + 3]);
        for (size_t c = 0; c < 3; ++c)
            bgra[i + c] = uint8_t(bgra[i + c] + cover);
        bgra[i + 3] = 255;
    }
}

} // namespace viewer
