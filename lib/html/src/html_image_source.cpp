// The image source of an img element (html_image_source.h).
//
// html/#images-processing-model - https://html.spec.whatwg.org/multipage/images.html#images-processing-model

#include "html/src/html_image_source.h"

#include "pr/ascii.h"
#include "pr/css.h"

#include "css/src/css_media.h"
#include "css/src/css_syntax.h"
#include "css/src/css_unit.h"
#include "dom/src/dom_microsyntax.h"
#include "dom/src/dom_mime.h"
#include "dom/src/dom_node.h"

namespace pr_impl {

namespace {

// Returns true for ASCII whitespace (infra/#ascii-whitespace).
bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

// Returns true if `text` is a valid non-negative integer (html/#valid-non-negative-integer): one or more ASCII digits.
bool isNonNegativeInteger(PrStringView text) {
    if (text.size == 0)
        return false;
    for (size_t i = 0; i < text.size; ++i) {
        if (text.data[i] < '0' || text.data[i] > '9')
            return false;
    }
    return true;
}

// Appends the candidate of `url` and `descriptors` to `out`, unless the descriptors are in error
// (html/#parse-a-srcset-attribute, the descriptor parser).
void addCandidate(PrStringView url, const Array<PrStringView>& descriptors, Array<HtmlImageCandidate>* out) {
    bool error = false;
    uint64_t width = 0;
    double density = -1;
    uint64_t height = 0;
    for (uint32_t i = 0; i < descriptors.count && !error; ++i) {
        PrStringView descriptor = descriptors.data[i];
        char last = descriptor.data[descriptor.size - 1];
        PrStringView value{descriptor.data, descriptor.size - 1};
        if (last == 'w') {
            error = width != 0 || density >= 0 || !isNonNegativeInteger(value)
                    || !domParseNonNegativeInteger(value, &width) || width == 0;
        } else if (last == 'x') {
            error = width != 0 || density >= 0 || height != 0 || !domIsValidFloat(value)
                    || !domParseFloat(value, &density) || density < 0;
        } else if (last == 'h') {
            error = height != 0 || density >= 0 || !isNonNegativeInteger(value)
                    || !domParseNonNegativeInteger(value, &height) || height == 0;
        } else {
            error = true;
        }
    }
    // A height is only for a future version, and is an error without a width.
    if (error || (height != 0 && width == 0) || width > UINT32_MAX)
        return;
    out->push(HtmlImageCandidate{.url = url, .density = density >= 0 ? density : 0, .width = uint32_t(width)});
}

// Returns the length that a <source-size-value> gives, or a negative value if the component is not a non-negative
// length that the environment resolves (html/#sizes-attributes). A math function is not resolved.
double sourceSizeOf(const CssTokenArray* tokens, uint32_t index, const HtmlImageEnvironment& environment) {
    CssTokenKind kind = cssTokenKind(tokens, index);
    double number = kind == CssTokenKind_Number || kind == CssTokenKind_Dimension ? cssTokenNumber(tokens, index) : -1;
    if (number < 0)
        return -1;
    if (kind == CssTokenKind_Number)
        return number == 0 ? 0 : -1;
    double height = environment.media != nullptr
                        ? environment.media->features[PrCssDescriptor_MediaHeight - kCssDescriptorMediaFirst].number
                        : 0;
    double vw = environment.viewportWidth / 100;
    double vh = height == height ? height / 100 : vw;
    PrCssUnit unit = cssTokenUnit(tokens, index);
    switch (unit) {
        case PrCssUnit_Em:
        case PrCssUnit_Rem:
            return number * environment.fontSize;
        case PrCssUnit_Vw:
            return number * vw;
        case PrCssUnit_Vh:
            return number * vh;
        case PrCssUnit_Vmin:
            return number * (vw < vh ? vw : vh);
        case PrCssUnit_Vmax:
            return number * (vw > vh ? vw : vh);
        default:
            break;
    }
    double scale = cssUnitCategory(unit) == CssUnitCategory_Length ? cssUnitScale(unit) : 0;
    return scale != 0 ? number * scale : -1;
}

// Returns true if the media query list `query` matches the environment, as a source element's media attribute or a
// sizes attribute's condition asks. Every query matches where no environment evaluates it.
bool matches(PrStringView query, const HtmlImageEnvironment& environment) {
    if (environment.media == nullptr)
        return true;
    PrCssMediaQueryList* list = cssMediaQueryListCreate(environment.media, query);
    bool matched = prCssMediaQueryListMatches(list);
    cssMediaQueryListRelease(list);
    return matched;
}

// Returns true if the environment decodes images of the MIME type `type`, as a source element's type must say
// (html/#update-the-source-set). SVG images are drawn whatever the codec.
bool decodes(PrStringView type, const HtmlImageEnvironment& environment) {
    PrStringView essence = domMimeTypeEssence(type);
    char lower[64];
    if (essence.size == 0 || essence.size > sizeof(lower))
        return false;
    for (size_t i = 0; i < essence.size; ++i)
        lower[i] = asciiLower(essence.data[i]);
    PrStringView folded{lower, essence.size};
    if (folded == "image/svg+xml")
        return true;
    return environment.images != nullptr && prImageCodecSupportsType(environment.images, folded);
}

// Selects the candidate of the source set whose density is the lowest at least the pixel ratio, or else the highest. A
// candidate's density is its density descriptor, its width over the source size, or 1 without either
// (html/#normalise-the-source-densities).
HtmlImageSource selectCandidate(const Array<HtmlImageCandidate>& set, double sourceSize, double pixelRatio) {
    HtmlImageSource chosen{PrStringView{}, 1};
    bool above = false;
    for (uint32_t i = 0; i < set.count; ++i) {
        const HtmlImageCandidate& candidate = set.data[i];
        double density = candidate.density > 0                    ? candidate.density
                         : candidate.width != 0 && sourceSize > 0 ? double(candidate.width) / sourceSize
                                                                  : 1;
        bool enough = density >= pixelRatio;
        bool better = chosen.url.data == nullptr || (enough && !above)
                      || (enough && density < chosen.density) || (!enough && !above && density > chosen.density);
        if (better) {
            chosen = HtmlImageSource{candidate.url, density};
            above = enough;
        }
    }
    return chosen;
}

} // namespace

void htmlParseSrcset(PrStringView value, Array<HtmlImageCandidate>* out) {
    const char* s = value.data;
    size_t size = value.size;
    size_t at = 0;
    Array<PrStringView> descriptors;
    for (;;) {
        // The splitting loop: whitespace and commas separate the candidates.
        while (at < size && (isSpace(s[at]) || s[at] == ','))
            ++at;
        if (at >= size)
            break;
        size_t start = at;
        while (at < size && !isSpace(s[at]))
            ++at;
        PrStringView url{s + start, at - start};
        descriptors.clear();
        if (url.data[url.size - 1] == ',') {
            // A URL that ends in commas has no descriptors.
            while (url.size != 0 && url.data[url.size - 1] == ',')
                --url.size;
        } else {
            // The descriptor tokenizer: whitespace separates descriptors, a comma ends them, and parentheses hold
            // whatever they enclose.
            while (at < size && isSpace(s[at]))
                ++at;
            enum { InDescriptor, InParens, AfterDescriptor } state = InDescriptor;
            size_t token = at;
            for (; at <= size; ++at) {
                char c = at < size ? s[at] : '\0';
                bool end = at == size;
                if (state == InDescriptor) {
                    if (!end && isSpace(c)) {
                        if (at > token)
                            descriptors.push(PrStringView{s + token, at - token});
                        state = AfterDescriptor;
                    } else if (!end && c == ',') {
                        if (at > token)
                            descriptors.push(PrStringView{s + token, at - token});
                        ++at;
                        break;
                    } else if (!end && c == '(') {
                        state = InParens;
                    } else if (end) {
                        if (at > token)
                            descriptors.push(PrStringView{s + token, at - token});
                        break;
                    }
                } else if (state == InParens) {
                    if (end) {
                        descriptors.push(PrStringView{s + token, at - token});
                        break;
                    }
                    if (c == ')')
                        state = InDescriptor;
                } else if (end) {
                    break;
                } else if (!isSpace(c)) {
                    state = InDescriptor;
                    token = at;
                    --at;
                }
            }
        }
        if (url.size != 0)
            addCandidate(url, descriptors, out);
    }
    descriptors.reset();
}

double htmlSourceSize(PrStringView value, const HtmlImageEnvironment& environment) {
    double fallback = environment.viewportWidth;
    if (value.size == 0)
        return fallback;
    CssTokenArray tokens = {};
    CssTokenScratch scratch = {};
    cssTokenize(&tokens, &scratch, value);
    uint32_t count = cssTokenCount(&tokens);
    double size = -1;
    uint32_t entry = 0;
    while (size < 0 && entry < count) {
        // The entry's component values, up to the next comma outside blocks: the first and the last that are not
        // whitespace.
        uint32_t first = kHashMissing;
        uint32_t last = kHashMissing;
        uint32_t at = entry;
        while (at < count && cssTokenKind(&tokens, at) != CssTokenKind_Comma) {
            CssTokenKind kind = cssTokenKind(&tokens, at);
            uint32_t next = at + 1;
            if (kind == CssTokenKind_Function || kind == CssTokenKind_OpenParen || kind == CssTokenKind_OpenBracket
                || kind == CssTokenKind_OpenBrace)
                next = cssTokenCloser(&tokens, at) + 1;
            if (kind != CssTokenKind_Whitespace) {
                if (first == kHashMissing)
                    first = at;
                last = at;
            }
            at = next;
        }
        uint32_t following = at + 1;
        if (last != kHashMissing) {
            double length = sourceSizeOf(&tokens, last, environment);
            if (length >= 0) {
                // What comes before the length is a media condition, which must match.
                bool unconditional = first == last;
                PrStringView condition =
                    unconditional ? PrStringView{}
                                  : cssSpanText(&tokens, CssTokenSpan{first, last - first});
                if (unconditional || matches(condition, environment))
                    size = length;
            }
        }
        entry = following;
    }
    cssTokenArrayReset(&tokens);
    cssTokenScratchReset(&scratch);
    return size >= 0 ? size : fallback;
}

HtmlImageSource htmlSelectImageSource(const PrDomElement* img, const HtmlImageEnvironment& environment) {
    Array<HtmlImageCandidate> set;
    HtmlImageSource selected{PrStringView{}, 1};
    // A picture's source elements before the img offer their sets first (html/#update-the-source-set).
    const PrDomElement* picture = domElementFromNode(img->parent);
    if (picture != nullptr && domIsHtml(picture, PrDomTag_Picture)) {
        for (const PrDomNode* child = picture->first; child != img && child != nullptr; child = child->next) {
            const PrDomElement* source = domElementFromNode(child);
            if (source == nullptr || !domIsHtml(source, PrDomTag_Source))
                continue;
            PrStringView srcset = domAttributeValue(source, "srcset");
            set.clear();
            htmlParseSrcset(srcset, &set);
            if (set.count == 0)
                continue;
            PrStringView media = domAttributeValue(source, "media");
            if (media.data != nullptr && !matches(media, environment))
                continue;
            PrStringView type = domAttributeValue(source, "type");
            if (type.data != nullptr && !decodes(type, environment))
                continue;
            double sourceSize = htmlSourceSize(domAttributeValue(source, "sizes"), environment);
            selected = selectCandidate(set, sourceSize, environment.pixelRatio);
            set.reset();
            return selected;
        }
    }
    // The img's own srcset, with its src as a candidate of density 1 if no candidate has that density or a width.
    set.clear();
    htmlParseSrcset(domAttributeValue(img, "srcset"), &set);
    PrStringView src = domAttributeValue(img, "src");
    bool described = false;
    for (uint32_t i = 0; i < set.count; ++i)
        described = described || set.data[i].width != 0 || set.data[i].density == 1;
    if (src.size != 0 && !described)
        set.push(HtmlImageCandidate{.url = src, .density = 1, .width = 0});
    if (set.count == 0) {
        set.reset();
        return HtmlImageSource{src, 1};
    }
    selected = selectCandidate(set, htmlSourceSize(domAttributeValue(img, "sizes"), environment),
                               environment.pixelRatio);
    set.reset();
    return selected;
}

} // namespace pr_impl
