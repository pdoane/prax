// Estimates of how text sets, from a table of the advances of code points.

#include "pr/text_estimate.h"

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/raw_array.h"
#include "pr/unicode.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// The code points of a page of the table of advances, and the pages that all of Unicode takes.
constexpr uint32_t kPageBits = 8;
constexpr uint32_t kPageCodePoints = 1u << kPageBits;
constexpr uint32_t kPageCount = 0x110000u >> kPageBits;

// An advance that has not been asked of the font.
constexpr float kAdvanceUnknown = -1;

// The distance between tab stops, in spaces.
constexpr float kTabSpaces = 8;

// The first code point that estimates treat as wide, which a line may break beside. Below it are the scripts that
// separate words.
constexpr uint32_t kWideFirst = 0x2e80;

// The advances of a run of code points, or kAdvanceUnknown.
struct AdvancePage {
    float advances[kPageCodePoints];    // by the code point's low bits
};

// The advances of the code points that have been asked for.
struct TextEstimator {
    PrTextFont* font;
    PrTextAxis axis;
    float size;                     // of the font, in px per em
    uint64_t revision;              // of the font's table of faces, which the advances were read from
    RawArray<AdvancePage*> pages;   // by code point, shifted by kPageBits: null for a page that nothing has asked of
};

void clearPages(TextEstimator* e) {
    for (uint32_t i = 0; i < e->pages.count; ++i) {
        prMemoryFree(e->pages.data[i]);
        e->pages.data[i] = nullptr;
    }
}

float advanceOf(TextEstimator* e, uint32_t codePoint) {
    uint32_t pageIndex = codePoint >> kPageBits;
    if (pageIndex >= kPageCount)
        return 0;
    AdvancePage* page = e->pages.data[pageIndex];
    if (page == nullptr) {
        page = alloc<AdvancePage>();
        for (uint32_t i = 0; i < kPageCodePoints; ++i)
            page->advances[i] = kAdvanceUnknown;
        e->pages.data[pageIndex] = page;
    }
    float& kept = page->advances[codePoint & (kPageCodePoints - 1)];
    if (kept == kAdvanceUnknown) {
        float ems = 0;
        if (!prTextFontAdvance(e->font, codePoint, prUnicodeScript(codePoint), e->axis, &ems) &&
                !prTextFontAdvance(e->font, kPrUtf8Replacement, prUnicodeScript(kPrUtf8Replacement), e->axis, &ems))
            ems = 0;
        kept = ems * e->size;
    }
    return kept;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTextEstimator : TextEstimator {};

PrTextEstimator* prTextEstimatorCreate(PrTextFont* font, PrTextAxis axis) {
    PrTextEstimator* e = allocZeroed<PrTextEstimator>();
    prTextFontRetain(font);
    e->font = font;
    e->axis = axis;
    e->size = prTextFontStyle(font).size;
    e->revision = prTextFontRevision(font);
    e->pages.resize(kPageCount);
    for (uint32_t i = 0; i < kPageCount; ++i)
        e->pages.data[i] = nullptr;
    return e;
}

void prTextEstimatorDestroy(PrTextEstimator* PR_NULLABLE estimator) {
    if (estimator == nullptr)
        return;
    clearPages(estimator);
    estimator->pages.reset();
    prTextFontRelease(estimator->font);
    prMemoryFree(estimator);
}

float prTextEstimatorAdvance(PrTextEstimator* estimator, uint32_t codePoint) {
    return advanceOf(estimator, codePoint);
}

float prTextEstimatorByteAdvance(PrTextEstimator* estimator) {
    // The letters and the space, each once, stand for running text and for code alike.
    float sum = advanceOf(estimator, ' ');
    for (uint32_t c = 'a'; c <= 'z'; ++c)
        sum += advanceOf(estimator, c);
    return sum / 27;
}

uint32_t prTextEstimatorLineCount(PrTextEstimator* estimator, PrStringView text, float width) {
    PrTextEstimator* e = estimator;
    if (e->revision != prTextFontRevision(e->font)) {
        e->revision = prTextFontRevision(e->font);
        clearPages(e);
    }
    if (!(width > 0))
        return 1;
    float tabStop = kTabSpaces * advanceOf(e, ' ');
    uint32_t lines = 1;
    float x = 0;                    // the end of the line so far
    float word = 0;                 // how much of the line follows its last opportunity to break
    bool canBreak = false;          // the line has an opportunity
    size_t at = 0;
    while (at < text.size) {
        uint32_t codePoint = uint8_t(text.data[at]);
        if (codePoint < 0x80)
            at += 1;
        else
            at += prUtf8Decode(text, at, &codePoint);
        bool space = codePoint == ' ' || codePoint == '\t';
        float advance = advanceOf(e, codePoint);
        if (codePoint == '\t' && tabStop > 0)
            advance = tabStop - prF32Fmod(x, tabStop);
        // White space hangs past the end of a line, so only what follows it wraps.
        if (x + advance > width && x > 0 && !space) {
            lines += 1;
            x = canBreak ? word : 0;
            canBreak = false;
            word = x;
            // The word that moved down may fill lines of its own.
            while (x > width) {
                lines += 1;
                x -= width;
                word = x;
            }
        }
        x += advance;
        word += advance;
        if (space || codePoint == '-' || codePoint >= kWideFirst) {
            canBreak = true;
            word = 0;
        }
    }
    return lines;
}
