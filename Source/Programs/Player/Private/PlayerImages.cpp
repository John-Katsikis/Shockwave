// --image-stats and --image-diff: numbers for screenshots, so renderer changes can be
// compared with a recorded baseline from the command line (docs/GraphicsVerification.md).

#include "Player.h"

#include <Assets/Image.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

namespace player {

namespace {

struct Stats {
    uint32_t width = 0, height = 0;
    double mean[3] = {0, 0, 0};   // 0..255, encoded values as stored
    double luma = 0;   // Rec. 709 weights on the encoded values
};

shockwave::Image load(const fs::path& path) {
    shockwave::Image image;
    std::string error;
    if (!shockwave::loadImage(path, image, error)) throw std::runtime_error(path.string() + ": " + error);
    return image;
}

Stats stats(const shockwave::Image& image) {
    Stats s;
    s.width = image.width;
    s.height = image.height;
    const size_t pixels = size_t(image.width) * image.height;
    for (size_t i = 0; i < pixels; ++i)
        for (int c = 0; c < 3; ++c) s.mean[c] += image.rgba[i * 4 + c];
    for (double& m : s.mean) m /= double(std::max<size_t>(pixels, 1));
    s.luma = 0.2126 * s.mean[0] + 0.7152 * s.mean[1] + 0.0722 * s.mean[2];
    return s;
}

std::string statsJson(const Stats& s) {
    char text[256];
    std::snprintf(
        text, sizeof(text), "{\"width\": %u, \"height\": %u, \"mean_rgb\": [%.3f, %.3f, %.3f], \"mean_luma\": %.3f}",
        s.width, s.height, s.mean[0], s.mean[1], s.mean[2], s.luma);
    return text;
}

void write(const fs::path& json, const std::string& text) {
    if (json.empty()) {
        std::fputs(text.c_str(), stdout);
        return;
    }
    std::ofstream out(json);
    out << text;
    out.close();
    if (!out) throw std::runtime_error("can't write " + json.string());
    std::printf("Wrote %s\n", json.string().c_str());
}

// Only the region's pixels: whole pixels from its top-left corner, at least one.
shockwave::Image crop(const shockwave::Image& whole, const std::array<double, 4>& region) {
    if (region == std::array<double, 4>{0, 0, 1, 1}) return whole;
    const auto x0 = std::min(uint32_t(region[0] * whole.width), whole.width - 1);
    const auto y0 = std::min(uint32_t(region[1] * whole.height), whole.height - 1);
    const auto w = std::clamp(uint32_t(std::lround(region[2] * whole.width)), 1u, whole.width - x0);
    const auto h = std::clamp(uint32_t(std::lround(region[3] * whole.height)), 1u, whole.height - y0);
    shockwave::Image part;
    part.width = w;
    part.height = h;
    part.rgba.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        std::copy_n(
            whole.rgba.data() + (size_t(y0 + y) * whole.width + x0) * 4, size_t(w) * 4,
            part.rgba.data() + size_t(y) * w * 4);
    return part;
}

}   // namespace

int imageStats(const fs::path& image, const fs::path& json, const std::array<double, 4>& region) {
    const Stats s = stats(crop(load(image), region));
    write(json, statsJson(s) + "\n");
    return 0;
}

int imageDiff(
    const fs::path& reference, const fs::path& candidate, double tolerance, const fs::path& json,
    const std::array<double, 4>& region) {
    const shockwave::Image a = crop(load(reference), region), b = crop(load(candidate), region);
    const Stats sa = stats(a), sb = stats(b);
    const bool sameSize = a.width == b.width && a.height == b.height;
    double squared = 0;
    int maxDiff = 0;
    size_t changed = 0;   // pixels with any channel off by more than 8 of 255
    const size_t pixels = sameSize ? size_t(a.width) * a.height : 0;
    for (size_t i = 0; i < pixels; ++i) {
        int worst = 0;
        for (int c = 0; c < 3; ++c) {
            const int d = std::abs(int(a.rgba[i * 4 + c]) - int(b.rgba[i * 4 + c]));
            squared += double(d) * d;
            worst = std::max(worst, d);
        }
        maxDiff = std::max(maxDiff, worst);
        changed += worst > 8;
    }
    const double rmse = pixels ? std::sqrt(squared / double(pixels * 3)) : 0.0;
    const double psnr = rmse > 0 ? 20.0 * std::log10(255.0 / rmse) : 999.0;
    const double changedPercent = pixels ? 100.0 * double(changed) / double(pixels) : 0.0;
    const bool pass = sameSize && rmse <= tolerance;
    char numbers[256];
    std::snprintf(
        numbers, sizeof(numbers),
        "\"rmse\": %.4f, \"psnr_db\": %.2f, \"max_diff\": %d, \"changed_percent\": %.4f, \"tolerance\": %.4f", rmse,
        psnr, maxDiff, changedPercent, tolerance);
    const std::string text = std::string("{\"reference\": ") + statsJson(sa) + ",\n \"candidate\": " + statsJson(sb) +
                             ",\n \"same_size\": " + (sameSize ? "true" : "false") + ", " + numbers +
                             ", \"pass\": " + (pass ? "true" : "false") + "}\n";
    write(json, text);
    if (!json.empty()) {
        if (!sameSize)
            std::printf("FAIL: %ux%u against %ux%u\n", b.width, b.height, a.width, a.height);
        else
            std::printf(
                "%s: rmse %.4f (tolerance %.4f), max %d, %.3f%% of pixels changed\n", pass ? "PASS" : "FAIL", rmse,
                tolerance, maxDiff, changedPercent);
    }
    return pass ? 0 : 1;
}

}   // namespace player
