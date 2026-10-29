// rlottie, Samsung's Lottie player, as a source (source_rlottie.h), against which the viewer compares lib/lottie.

#include "tools/viewer/source_rlottie.h"

#include "rlottie.h"

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

// Returns the frame that the engine draws at the time, looping over the animation's length, taken whole because rlottie
// draws only whole frames. The length is the out point minus the in point. rlottie's frame count rounds the length up.
double rlottieFrameAt(const RlottieSource& rlottie, double time) {
    double frames = rlottie.animation->duration() * rlottie.animation->frameRate();
    return std::floor(frames > 0 ? std::fmod(time * rlottie.animation->frameRate(), frames) : 0);
}

void rlottiePixels(RlottieSource& rlottie, uint32_t width, uint32_t height, double frame, std::vector<uint8_t>& bgra) {
    bgra.assign(size_t(width) * height * 4, 0);
    rlottie::Surface surface(reinterpret_cast<uint32_t*>(bgra.data()), width, height, size_t(width) * 4);
    rlottie.animation->renderSync(size_t(frame), surface);
    // Over white, as the player pages show it.
    for (size_t i = 0; i < bgra.size(); i += 4) {
        uint8_t cover = uint8_t(255 - bgra[i + 3]);
        for (size_t c = 0; c < 3; ++c)
            bgra[i + c] = uint8_t(bgra[i + c] + cover);
        bgra[i + 3] = 255;
    }
}

} // namespace viewer
