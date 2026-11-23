// SVG animation: the time containers of a document's outermost svg elements, the animation elements that they time, and
// the values that those elements give their targets' attributes

#include "svg/src/svg_animate.h"

#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/float_convert.h"
#include "pr/memory.h"
#include "pr/min_max.h"
#include "pr/path.h"
#include "pr/path_data.h"
#include "pr/path_measure.h"
#include "pr/raw_array.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/svg.h"
#include "pr/svg_element.h"
#include "pr/text_buffer.h"

#include "css/src/css_animations.h"
#include "css/src/css_color_value.h"
#include "css/src/css_parser.h"
#include "css/src/css_path.h"
#include "css/src/css_presentation.h"
#include "css/src/css_presentation_hooks.h"
#include "css/src/css_property.h"
#include "css/src/css_root.h"
#include "css/src/css_rule.h"
#include "css/src/css_value.h"
#include "dom/src/dom_event.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_range.h"
#include "dom/src/dom_use.h"
#include "svg/src/svg_animate_value.h"
#include "svg/src/svg_document.h"
#include "svg/src/svg_draw.h"
#include "svg/src/svg_hints.h"
#include "svg/src/svg_render.h"
#include "svg/src/svg_tree.h"

namespace pr_impl {

// The kinds of value of a begin or end list (svg-animations/#BeginValueSyntax).
enum {
    SvgTimeKind_Offset,
    SvgTimeKind_Syncbase,
    SvgTimeKind_Event,
    SvgTimeKind_Repeat,
    SvgTimeKind_AccessKey,
    SvgTimeKind_Wallclock,
    SvgTimeKind_Indefinite,
};
typedef uint8_t SvgTimeKind;

// One value of a begin or end list.
struct SvgTimeSpec {
    PrRawString base;           // the id of the element that it names, or null for none
    PrRawString name;           // a syncbase's begin or end, or an event's type
    double offset;              // in seconds
    int32_t iteration;          // of a repeat value
    SvgTimeKind kind;
};

// An interval of an element, which the elements that depend on it resolve against. Its end is infinite if unresolved.
struct SvgInterval {
    double begin;
    double end;
};

// What a begin time does to an element that already ran (svg-animations/#RestartAttribute).
enum {
    SvgRestart_Always,
    SvgRestart_WhenNotActive,
    SvgRestart_Never,
};
typedef uint8_t SvgRestart;

// What an element does after its active duration (svg-animations/#FillAttribute).
enum {
    SvgFill_Remove,
    SvgFill_Freeze,
};
typedef uint8_t SvgFill;

// The time container of an outermost svg element, whose time begins once it is made and its document is complete, as
// the document's load event fires (svg-animations/#DocumentBegin). Until then its time is zero, and a seek is kept for
// its begin.
struct SvgTimeContainer {
    PrDomElement* root;
    double origin;              // the timeline's time, in milliseconds, at the container's time zero, once begun
    double held;                // the container's time while paused, in seconds, or the time it begins at
    double sampled;             // the container's time at the last sample
    bool paused;
    bool begun;
    bool seeked;                // a seek before the container began, whose begin samples as a seek does
};

// A listener for an event value of a begin or end list, which adds an instance time when the event fires
// (svg-animations/#EventValueSyntax).
struct SvgEventHook {
    SvgAnimations* owner;
    PrDomElement* animation;
    PrDomElement* base;         // the element that the event fires at
    PrRawString type;
    double offset;
    bool endList;
};

// An animation element that a container times.
struct SvgTimed {
    PrDomElement* element;
    PrDomElement* PR_NULLABLE target;
    PrRawString attributeName;
    PrCssProperty property;     // the property that the attribute is a presentation attribute of, or Count
    RawArray<SvgTimeSpec> beginSpecs;
    RawArray<SvgTimeSpec> endSpecs;
    RawArray<double> beginInstances;    // added by events and scripts, in seconds
    RawArray<double> endInstances;
    RawArray<SvgInterval> intervals;       // oldest first
    RawArray<SvgEventHook*> hooks;
    SvgAnimateFunction function;    // of its values, once they are read
    SvgAnimateValue base;           // the target's own value of the attribute, as the function's type reads it
    uint32_t baseReads;             // the times that `base` was read
    uint32_t container;
    double begin;               // of the current interval, or infinite if there is none
    double end;
    double duration;            // the simple duration, or infinite if indefinite
    double repeatDuration;      // negative if absent, infinite if indefinite
    double repeatCount;         // 0 if absent, infinite if indefinite
    double minimum;
    double maximum;
    double lastBegin;           // of the previous interval
    double lastEnd;
    double sampledBegin;        // of the interval at the last sample
    double joined;              // the container time at which the element joined the timeline
    int32_t iteration;          // at the last sample
    SvgRestart restart;
    SvgFill fill;
    bool durationStated;
    bool endStated;
    bool endEventual;           // the end list has an event, repeat or accessKey value
    bool inert;                 // attributeType="CSS" names no property
    bool motion;                // an animateMotion, which writes a transform rather than an attribute
    bool begun;                 // the current interval has begun
    bool ran;                   // an interval has begun
    bool sampledActive;         // the element was active at the last sample
    bool present;               // found by the last scan
    bool compiled;              // `function` is read and compiles; one in error is read again at each sample
    bool compiles;
    bool baseRead;              // `base` is read
};

// A value that a sample gives an attribute.
struct SvgAnimatedValue {
    PrDomElement* element;
    PrRawString name;
    SvgAnimateValue value;
    PrRawString text;           // the value's text, written when a reader first asks for it, or null
    double begin;               // of the interval that wrote it, which orders the animations of one attribute
    PrCssProperty property;
};

// The transform that animateMotion gives an element, outside its own: a, b, c, d, e, f of a matrix.
struct SvgMotion {
    PrDomElement* element;
    double matrix[6];
    const SvgAnimateValue* PR_NULLABLE own; // the element's own transform, which the motion precedes
    uint32_t ownRead;                       // the times that the animation read the element's own transform
};

// What one sample computes, which the next replaces.
struct SvgOverlay {
    RawArray<SvgAnimatedValue> values;
    RawArray<SvgMotion> motions;
};

// An event that waits for dispatch.
struct SvgQueuedEvent {
    PrDomElement* target;
    PrStringView type;          // a literal
    int32_t detail;
};

// Where a simple duration has reached, and whether the element writes a value.
struct SvgSimpleTime {
    double progress;
    int32_t iteration;
    bool writes;                // the element is active or frozen
    bool inside;                // the time is within the current interval
    bool running;               // a later time changes what it writes
    bool moving;                // what it writes changes as time passes within its interval
};

struct SvgAnimations {
    PrDomDocument* document;
    CssParser* PR_NULLABLE parser; // declares the animated values of presentation attributes
    SvgOverlay overlays[2];        // the current sample's and the one being built
    uint32_t front;
    RawArray<SvgTimeContainer> containers;
    RawArray<SvgTimed> animations;
    RawArray<SvgQueuedEvent> events;
    uint64_t revision;          // changes when an interval or an instance time changes
    bool scan;                  // the animation elements must be found again
    bool scanQueued;            // a task that finds them is queued
    bool active;                // a container runs, and something it times changes
    bool moving;                // a container runs, and a value changes as time passes, between the events ahead
    bool seek;                  // the next sample follows a jump of a container's time
    bool unsampled;             // an instance time changed since the last sample
    bool sampling;
    bool styled;                // an element has been given animated declarations (cssElementAnimatedStyle)
};

namespace {

// The times that one interval search reads from a list, the intervals that an element keeps for the elements that
// depend on it, and the times that events and scripts add to a list, each dropping the oldest past its limit.
constexpr uint32_t kMaxIntervals = 32;
constexpr uint32_t kMaxInstanceTimes = 64;

// How far one sample walks an element's intervals, which bounds a cyclic dependency that makes a new interval on every
// step, and how many repeat events it queues for one element.
constexpr uint32_t kMaxIntervalSteps = 256;
constexpr int32_t kMaxRepeatEvents = 8;

// Returns the document's animations, or null if SVG does not follow it.
SvgAnimations* PR_NULLABLE animationsOf(const PrDomDocument* document) {
    return svgDocumentAnimations(document);
}

double documentTime(const SvgAnimations* a) {
    return cssAnimations(a->document).time;
}

PrStringView viewOf(const PrRawString& text) {
    return prRawStringView(&text);
}

void specsReset(RawArray<SvgTimeSpec>* specs) {
    for (uint32_t i = 0; i < specs->count; ++i) {
        prRawStringReset(&specs->data[i].base);
        prRawStringReset(&specs->data[i].name);
    }
    specs->reset();
}

void heldReset(SvgAnimatedValue* held) {
    prRawStringReset(&held->name);
    svgAnimateValueReset(&held->value);
    prRawStringReset(&held->text);
}

void overlayClear(SvgOverlay* overlay) {
    for (uint32_t i = 0; i < overlay->values.count; ++i)
        heldReset(&overlay->values.data[i]);
    overlay->values.clear();
    overlay->motions.clear();
}

PrStringView attributeOf(const PrDomElement* element, PrStringView name) {
    return domAttributeValue(element, name);
}

// Reads the digits at `*at`: exactly `wanted` of them, or one or more if `wanted` is zero.
bool clockDigits(PrStringView text, size_t* at, uint32_t wanted, double* out) {
    size_t read = 0;
    double value = 0;
    while (*at + read < text.size && prAsciiIsDigit(text.data[*at + read])) {
        value = value * 10 + (text.data[*at + read] - '0');
        ++read;
    }
    if (read == 0 || (wanted != 0 && read != wanted))
        return false;
    *at += read;
    *out = value;
    return true;
}

// Reads a clock value in its full, partial or timecount form (svg-animations/#ClockValueSyntax). Minutes and seconds
// take two digits and stay below 60, and a fraction takes at least one digit.
bool clockValue(PrStringView text, double* out) {
    PrStringView value = prStringTrim(text);
    uint32_t colons = 0;
    for (size_t i = 0; i < value.size; ++i)
        colons += value.data[i] == ':' ? 1 : 0;
    if (colons > 2)
        return false;
    size_t at = 0;
    double seconds = 0;
    double field = 0;
    if (colons == 2) {
        if (!clockDigits(value, &at, 0, &field) || at == value.size || value.data[at] != ':')
            return false;
        ++at;
        seconds += field * 3600;
    }
    if (colons != 0) {
        if (!clockDigits(value, &at, 2, &field) || field > 59 || at == value.size || value.data[at] != ':')
            return false;
        ++at;
        seconds += field * 60;
        if (!clockDigits(value, &at, 2, &field) || field > 59)
            return false;
    } else if (!clockDigits(value, &at, 0, &field)) {
        return false;
    }
    seconds += field;
    if (at < value.size && value.data[at] == '.') {
        ++at;
        size_t start = at;
        double scale = 0.1;
        while (at < value.size && prAsciiIsDigit(value.data[at])) {
            seconds += (value.data[at] - '0') * scale;
            scale /= 10;
            ++at;
        }
        if (at == start)
            return false;
    }
    PrStringView metric{value.data + at, value.size - at};
    if (colons != 0) {
        *out = seconds;
        return metric.size == 0;
    }
    double scale = 0;
    if (metric.size == 0 || metric == "s")
        scale = 1;
    else if (metric == "ms")
        scale = 0.001;
    else if (metric == "min")
        scale = 60;
    else if (metric == "h")
        scale = 3600;
    else
        return false;
    *out = seconds * scale;
    return true;
}

// Returns the id with its backslash escapes removed, as a timing value escapes a full stop in an id.
PrRawString unescapeId(PrStringView text) {
    PrTextBuffer out = {};
    for (size_t i = 0; i < text.size; ++i) {
        if (text.data[i] == '\\' && i + 1 < text.size)
            ++i;
        prTextBufferAppendChar(&out, text.data[i]);
    }
    PrRawString result = prRawStringCopy(prTextBufferView(&out));
    prTextBufferReset(&out);
    return result;
}

// Returns where the offset at the end of a timing value starts: the last sign whose remainder is a clock value. An
// event type may hold a hyphen, which is why the clock value decides.
size_t splitOffset(PrStringView text, double* out) {
    for (size_t i = text.size; i > 0; --i) {
        char c = text.data[i - 1];
        double offset = 0;
        if ((c != '+' && c != '-') || !clockValue(PrStringView{text.data + i, text.size - i}, &offset))
            continue;
        *out = c == '-' ? -offset : offset;
        return i - 1;
    }
    *out = 0;
    return text.size;
}

// Reads one value of a begin or end list (svg-animations/#BeginValueSyntax). Returns false for a value that the grammar
// rejects.
bool parseTimeSpec(PrStringView text, SvgTimeSpec* out) {
    PrStringView value = prStringTrim(text);
    *out = SvgTimeSpec{};
    if (value.size == 0)
        return false;
    if (value == "indefinite") {
        out->kind = SvgTimeKind_Indefinite;
        return true;
    }
    if (prStringStartsWith(value, "wallclock(")) {
        out->kind = SvgTimeKind_Wallclock;
        return true;
    }
    double offset = 0;
    size_t nameSize = splitOffset(value, &offset);
    PrStringView name = prStringTrim(PrStringView{value.data, nameSize});
    out->offset = offset;
    if (name.size == 0 || clockValue(name, &out->offset)) {
        out->kind = SvgTimeKind_Offset;
        return true;
    }
    size_t dot = name.size;
    for (size_t i = 0; i < name.size; ++i) {
        if (name.data[i] == '\\') {
            ++i;
        } else if (name.data[i] == '.') {
            dot = i;
            break;
        }
    }
    PrStringView token = name;
    if (dot != name.size) {
        out->base = unescapeId(PrStringView{name.data, dot});
        token = PrStringView{name.data + dot + 1, name.size - dot - 1};
    }
    if (token == "begin" || token == "end") {
        out->kind = SvgTimeKind_Syncbase;
        out->name = prRawStringCopy(token);
        return true;
    }
    if (prStringStartsWith(token, "repeat(") && token.data[token.size - 1] == ')') {
        double count = 0;
        PrStringView inside{token.data + 7, token.size - 8};
        if (prF64ParsePrefix(inside, &count) != inside.size || count < 0 || count > 1e9) {
            prRawStringReset(&out->base);
            return false;
        }
        out->iteration = int32_t(count);
        out->kind = SvgTimeKind_Repeat;
        return true;
    }
    if (prStringStartsWith(token, "accessKey(")) {
        out->kind = SvgTimeKind_AccessKey;
        return true;
    }
    if (token.size == 0) {
        prRawStringReset(&out->base);
        return false;
    }
    out->kind = SvgTimeKind_Event;
    out->name = prRawStringCopy(token);
    return true;
}

// Reads a begin or end list, whose values are separated by semicolons.
void parseTimeList(PrStringView text, RawArray<SvgTimeSpec>* out) {
    size_t start = 0;
    for (size_t i = 0; i <= text.size; ++i) {
        if (i != text.size && text.data[i] != ';')
            continue;
        SvgTimeSpec spec{};
        if (parseTimeSpec(PrStringView{text.data + start, i - start}, &spec))
            out->push(spec);
        start = i + 1;
    }
}

// Returns the outermost svg ancestor of the element, or null if it has none.
PrDomElement* PR_NULLABLE containerRootOf(PrDomElement* element) {
    PrDomElement* root = nullptr;
    for (PrDomNode* at = element; at != nullptr; at = at->parent) {
        if (at->type == PrDomNodeType_Element && svgIs(static_cast<PrDomElement*>(at), PrDomTag_Svg))
            root = static_cast<PrDomElement*>(at);
    }
    return root;
}

// Returns the element that the animation animates: the one that its href names, or else its parent
// (svg-animations/#TargetElement).
PrDomElement* PR_NULLABLE targetOf(PrDomElement* animation) {
    PrStringView href = svgHref(animation);
    if (href.data == nullptr) {
        PrDomNode* parent = animation->parent;
        return parent != nullptr && parent->type == PrDomNodeType_Element ? static_cast<PrDomElement*>(parent) : nullptr;
    }
    if (href.size == 0 || href.data[0] != '#')
        return nullptr;
    return domElementById(animation->document, PrStringView{href.data + 1, href.size - 1});
}

// Returns the seconds that a duration attribute names, `fallback` if it names none, and infinity for indefinite.
double durationOf(const PrDomElement* element, PrStringView name, double fallback) {
    PrStringView text = prStringTrim(attributeOf(element, name));
    if (text.size == 0)
        return fallback;
    if (text == "indefinite" || text == "media")
        return kPrF64Infinity;
    double seconds = 0;
    if (!clockValue(text, &seconds) || seconds < 0)
        return fallback;
    return seconds;
}

// Returns true if the element writes its value after an interval ends (svg-animations/#FillAttribute).
bool freezes(const SvgTimed& t) {
    return t.fill == SvgFill_Freeze;
}

void timedReset(SvgAnimations* a, SvgTimed* t);
double containerTime(const SvgAnimations* a, uint32_t container);

// Returns a container for `root`, begun if its document is complete (SvgTimeContainer).
SvgTimeContainer newContainer(const SvgAnimations* a, PrDomElement* root) {
    bool begun = a->document->readiness == DomReadiness_Complete;
    return SvgTimeContainer{root, begun ? documentTime(a) : 0, 0, 0, false, begun, false};
}

// Returns the local name of the attribute that an animation's attributeName names, a qualified name whose prefix the
// namespaces in scope of the animation element resolve (svg-animations/#AttributeNameAttribute). XLink's href is the
// href that SVG reads in its place, and a prefix of xlink that no declaration resolves, as HTML's parser leaves it, is
// XLink's. Writes false to `known` for a name in another namespace, which no attribute that SVG reads has.
PrStringView attributeNameOf(PrDomElement* element, PrStringView name, bool* known) {
    *known = true;
    size_t colon = prStringFindByte(name, ':');
    if (colon == name.size)
        return name;
    PrStringView prefix{name.data, colon};
    PrStringView local{name.data + colon + 1, name.size - colon - 1};
    PrStringView space = prDomNodeLookupNamespaceUri(prDomElementAsNode(element), prefix);
    bool xlink = space == domNamedNamespaceUrl(PrDomNamespace_XLink) || (space.data == nullptr && prefix == "xlink");
    *known = xlink && local == "href";
    return local;
}

// Reads the timing of an animation element into `out`, which is empty.
void readTimed(SvgAnimations* a, PrDomElement* element, SvgTimed* out) {
    out->element = element;
    out->target = targetOf(element);
    out->motion = element->tag == PrDomTag_AnimateMotion;
    bool known = true;
    out->attributeName = prRawStringCopy(attributeNameOf(element, prStringTrim(attributeOf(element, "attributeName")), &known));
    PrStringView attributeType = prStringTrim(attributeOf(element, "attributeType"));
    PrCssProperty property = PrCssProperty_Count;
    if (out->target != nullptr && known)
        property = svgPresentationOf(out->target, viewOf(out->attributeName));
    // An attributeType of CSS animates a property, so a name that is not a presentation attribute animates nothing
    // (svg-animations/#TargetAttributes). Any presentation attribute reaches the style.
    out->inert = !known || (attributeType == "CSS" && property == PrCssProperty_Count);
    out->property = property;
    PrDomElement* root = containerRootOf(element);
    out->container = kArrayMissing;
    for (uint32_t i = 0; i < a->containers.count; ++i) {
        if (a->containers.data[i].root == root)
            out->container = i;
    }
    if (out->container == kArrayMissing) {
        out->container = a->containers.count;
        a->containers.push(newContainer(a, root));
    }
    parseTimeList(attributeOf(element, "begin"), &out->beginSpecs);
    parseTimeList(attributeOf(element, "end"), &out->endSpecs);
    if (attributeOf(element, "begin").data == nullptr)
        out->beginSpecs.push(SvgTimeSpec{});
    out->begin = kPrF64Infinity;
    out->end = kPrF64Infinity;
    out->lastBegin = kPrF64Infinity;
    out->lastEnd = kPrF64Infinity;
    out->sampledBegin = kPrF64Infinity;
    out->joined = containerTime(a, out->container);
    out->durationStated = prStringTrim(attributeOf(element, "dur")).size != 0;
    out->duration = durationOf(element, "dur", kPrF64Infinity);
    if (out->duration <= 0)
        out->duration = kPrF64Infinity;
    // A repeatDur of zero is not a duration, and is ignored as one that does not parse is.
    out->repeatDuration = durationOf(element, "repeatDur", -1);
    if (out->repeatDuration == 0)
        out->repeatDuration = -1;
    PrStringView repeat = prStringTrim(attributeOf(element, "repeatCount"));
    if (repeat == "indefinite") {
        out->repeatCount = kPrF64Infinity;
    } else if (repeat.size != 0) {
        double count = 0;
        if (prF64ParsePrefix(repeat, &count) == repeat.size && count > 0 && prF64IsFinite(count))
            out->repeatCount = count;
    }
    out->minimum = durationOf(element, "min", 0);
    if (!prF64IsFinite(out->minimum))
        out->minimum = 0;
    out->maximum = durationOf(element, "max", kPrF64Infinity);
    PrStringView restart = prStringTrim(attributeOf(element, "restart"));
    out->restart = SvgRestart_Always;
    if (restart == "never")
        out->restart = SvgRestart_Never;
    else if (restart == "whenNotActive")
        out->restart = SvgRestart_WhenNotActive;
    out->fill = SvgFill_Remove;
    if (prStringTrim(attributeOf(element, "fill")) == "freeze")
        out->fill = SvgFill_Freeze;
    out->endStated = out->endSpecs.count != 0;
    for (uint32_t i = 0; i < out->endSpecs.count; ++i) {
        SvgTimeKind kind = out->endSpecs.data[i].kind;
        out->endEventual = out->endEventual || kind == SvgTimeKind_Event || kind == SvgTimeKind_Repeat ||
            kind == SvgTimeKind_AccessKey;
    }
}

// Returns the element that a timing value names: the element of its id, or the animation's target for an event value
// that names none (svg-animations/#EventValueSyntax).
PrDomElement* PR_NULLABLE specBase(const SvgTimed& t, const SvgTimeSpec& spec) {
    if (prRawStringIsNull(&spec.base))
        return t.target;
    return domElementById(t.element->document, viewOf(spec.base));
}

SvgTimed* PR_NULLABLE timedOf(SvgAnimations* a, const PrDomElement* PR_NULLABLE element) {
    for (uint32_t i = 0; element != nullptr && i < a->animations.count; ++i) {
        if (a->animations.data[i].element == element)
            return &a->animations.data[i];
    }
    return nullptr;
}

// Appends to `out`, sorted, the times that a list resolves to now: its offsets, the intervals of its syncbases, and the
// times that events and scripts added (svg-animations/#BeginValueSyntax).
void resolveTimes(SvgAnimations* a, const SvgTimed& t, bool endList, RawArray<double>* out) {
    out->clear();
    const RawArray<SvgTimeSpec>& specs = endList ? t.endSpecs : t.beginSpecs;
    for (uint32_t i = 0; i < specs.count; ++i) {
        const SvgTimeSpec& spec = specs.data[i];
        if (spec.kind == SvgTimeKind_Offset) {
            out->push(spec.offset);
        } else if (spec.kind == SvgTimeKind_Indefinite && endList) {
            out->push(kPrF64Infinity);
        } else if (spec.kind == SvgTimeKind_Syncbase) {
            const SvgTimed* base = timedOf(a, specBase(t, spec));
            if (base == nullptr)
                continue;
            bool begins = viewOf(spec.name) == "begin";
            for (uint32_t j = 0; j < base->intervals.count; ++j) {
                double time = begins ? base->intervals.data[j].begin : base->intervals.data[j].end;
                if (prF64IsFinite(time))
                    out->push(time + spec.offset);
            }
        }
    }
    const RawArray<double>& instances = endList ? t.endInstances : t.beginInstances;
    out->append(instances.span());
    insertionSortItems(out->span(), [](double x, double y) { return x < y; });
}

void addInstanceTime(SvgAnimations* a, SvgTimed* t, bool endList, double time) {
    RawArray<double>& instances = endList ? t->endInstances : t->beginInstances;
    if (instances.count == kMaxInstanceTimes) {
        for (uint32_t i = 1; i < instances.count; ++i)
            instances.data[i - 1] = instances.data[i];
        --instances.count;
    }
    instances.push(time);
    ++a->revision;
    a->unsampled = true;
}

// Adds the instance times that event values of the event type `type`, fired at `element`, resolve to, so that
// begin="other.beginEvent" and begin="other.repeat(2)" follow the events that the engine queues.
void feedTimeEvent(SvgAnimations* a, const PrDomElement* element, PrStringView type, int32_t detail, double time) {
    for (uint32_t i = 0; i < a->animations.count; ++i) {
        SvgTimed& t = a->animations.data[i];
        for (uint32_t list = 0; list < 2; ++list) {
            const RawArray<SvgTimeSpec>& specs = list == 0 ? t.beginSpecs : t.endSpecs;
            for (uint32_t j = 0; j < specs.count; ++j) {
                const SvgTimeSpec& spec = specs.data[j];
                bool named = spec.kind == SvgTimeKind_Event && viewOf(spec.name) == type;
                bool repeated = spec.kind == SvgTimeKind_Repeat && type == "repeatEvent" && spec.iteration == detail;
                if ((!named && !repeated) || prRawStringIsNull(&spec.base) || specBase(t, spec) != element)
                    continue;
                addInstanceTime(a, &t, list == 1, time + spec.offset);
            }
        }
    }
}

void queueTimeEvent(SvgAnimations* a, SvgTimed& t, PrStringView type, int32_t detail, double time) {
    a->events.push(SvgQueuedEvent{t.element, type, detail});
    feedTimeEvent(a, t.element, type, detail, time);
}

// Returns the intermediate active duration: how long the simple duration runs with its repeats
// (svg-animations/#RepeatCountAttribute).
double repeatSpan(const SvgTimed& t) {
    bool counted = t.repeatCount != 0;
    bool repeated = t.repeatDuration >= 0;
    if (!counted && !repeated)
        return t.duration;
    double byCount = counted ? t.repeatCount * t.duration : kPrF64Infinity;
    double byDuration = repeated ? t.repeatDuration : kPrF64Infinity;
    return byCount < byDuration ? byCount : byDuration;
}

// Returns the end of the active duration of an interval that begins at `begin` and takes its end from the instance time
// `end`, within min and max (svg-animations/#MinAttribute).
double activeEnd(const SvgTimed& t, double begin, double end) {
    bool counted = t.repeatCount != 0;
    bool repeated = t.repeatDuration >= 0;
    double preliminary;
    if (t.endStated && prF64IsFinite(end) && !t.durationStated && !counted && !repeated) {
        preliminary = end - begin;
    } else {
        preliminary = repeatSpan(t);
        if (prF64IsFinite(end) && end - begin < preliminary)
            preliminary = end - begin;
    }
    // A min past the max ignores both (svg-animations/#MinAttribute).
    if (t.minimum <= t.maximum) {
        preliminary = prF64Max(preliminary, t.minimum);
        preliminary = prF64Min(preliminary, t.maximum);
    }
    return begin + preliminary;
}

// Returns the active end of the interval that begins at `begin`, and writes whether the end list allows one.
double endOfInterval(SvgAnimations* a, const SvgTimed& t, double begin, double skipEnd, bool* valid) {
    RawArray<double> ends;
    resolveTimes(a, t, true, &ends);
    bool stated = t.endStated || t.endInstances.count != 0;
    double end = kPrF64Infinity;
    *valid = true;
    if (stated) {
        bool resolved = false;
        for (uint32_t i = 0; i < ends.count && !resolved; ++i) {
            if (ends.data[i] < begin || (ends.data[i] == skipEnd && ends.data[i] == begin))
                continue;
            end = ends.data[i];
            resolved = true;
        }
        // An event value leaves the end open. A list of times that all fall before the begin allows no interval.
        *valid = resolved || t.endEventual || ends.count == 0;
    }
    ends.reset();
    return activeEnd(t, begin, end);
}

// Finds the first interval that begins after `beginAfter`, or at it if `inclusive`, and ends after the container began.
// Returns false if the lists allow none.
bool findInterval(SvgAnimations* a, const SvgTimed& t, double beginAfter, bool inclusive, double skipEnd, SvgInterval* out) {
    RawArray<double> begins;
    resolveTimes(a, t, false, &begins);
    double after = beginAfter;
    bool found = false;
    for (uint32_t step = 0; step < kMaxIntervalSteps; ++step) {
        double begin = kPrF64Infinity;
        bool resolved = false;
        for (uint32_t i = 0; i < begins.count && !resolved; ++i) {
            if (begins.data[i] > after || (inclusive && begins.data[i] == after)) {
                begin = begins.data[i];
                resolved = true;
            }
        }
        if (!resolved || !prF64IsFinite(begin))
            break;
        bool valid = false;
        double activeAt = endOfInterval(a, t, begin, skipEnd, &valid);
        if (!valid)
            break;
        if (activeAt > 0 || (begin == 0 && activeAt == 0)) {
            *out = SvgInterval{begin, activeAt};
            found = true;
            break;
        }
        inclusive = activeAt > begin;
        after = activeAt;
    }
    begins.reset();
    return found;
}

// Drops the times of a list before `begin`, as the reset of a restarting element drops its past events and script calls
// (svg-animations/#RestartAttribute).
void dropPastInstances(RawArray<double>* instances, double begin) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < instances->count; ++i) {
        if (instances->data[i] >= begin)
            instances->data[kept++] = instances->data[i];
    }
    instances->count = kept;
}

void recordInterval(SvgAnimations* a, SvgTimed* t, SvgInterval interval) {
    dropPastInstances(&t->beginInstances, interval.begin);
    dropPastInstances(&t->endInstances, interval.begin);
    if (t->intervals.count == kMaxIntervals) {
        for (uint32_t i = 1; i < t->intervals.count; ++i)
            t->intervals.data[i - 1] = t->intervals.data[i];
        --t->intervals.count;
    }
    t->intervals.push(interval);
    ++a->revision;
}

// Queues the events of an interval that began and ended between two samples, which neither sample shows.
void reportInterval(SvgAnimations* a, SvgTimed* t, double begin, double end) {
    if (!t->sampledActive || t->sampledBegin != begin)
        queueTimeEvent(a, *t, "beginEvent", 0, begin);
    queueTimeEvent(a, *t, "endEvent", 0, end);
    t->sampledActive = false;
    t->sampledBegin = kPrF64Infinity;
}

// Walks the element's intervals forward to `time`: makes the interval that the lists define, follows it while it runs,
// and moves to the next once it has passed.
void advance(SvgAnimations* a, SvgTimed* t, double time, bool seek) {
    RawArray<double> begins;
    for (uint32_t step = 0; step < kMaxIntervalSteps; ++step) {
        bool first = !prF64IsFinite(t->lastEnd);
        double after = first ? -kPrF64Infinity : t->lastEnd;
        bool inclusive = first || t->lastEnd > t->lastBegin;
        if (!prF64IsFinite(t->begin)) {
            SvgInterval interval{};
            if ((t->ran && t->restart == SvgRestart_Never) ||
                    !findInterval(a, *t, after, inclusive, t->lastEnd, &interval))
                break;
            t->begin = interval.begin;
            t->end = interval.end;
            t->begun = false;
            recordInterval(a, t, interval);
        } else if (!t->begun) {
            // An interval that has not begun follows its lists as they change.
            SvgInterval interval{};
            if (findInterval(a, *t, after, inclusive, t->lastEnd, &interval) &&
                    (interval.begin != t->begin || interval.end != t->end)) {
                t->begin = interval.begin;
                t->end = interval.end;
                t->intervals.last() = interval;
                ++a->revision;
            }
        }
        if (time >= t->begin && !t->begun) {
            t->begun = true;
            t->ran = true;
        }
        if (t->begun) {
            bool valid = false;
            double end = endOfInterval(a, *t, t->begin, t->lastEnd, &valid);
            // A later begin time ends an interval of an element that always restarts.
            if (t->restart == SvgRestart_Always) {
                resolveTimes(a, *t, false, &begins);
                for (uint32_t i = 0; i < begins.count; ++i) {
                    if (begins.data[i] > t->begin && begins.data[i] < end) {
                        end = begins.data[i];
                        break;
                    }
                }
            }
            if (end != t->end) {
                t->end = end;
                t->intervals.last().end = end;
                ++a->revision;
            }
        }
        if (!t->begun || !prF64IsFinite(t->end) || time < t->end)
            break;
        // An element that joins the timeline late does not report the intervals that ended before it joined.
        if (!seek && t->end > t->joined)
            reportInterval(a, t, t->begin, t->end);
        t->lastBegin = t->begin;
        t->lastEnd = t->end;
        t->begin = kPrF64Infinity;
        t->end = kPrF64Infinity;
        t->begun = false;
        ++a->revision;
    }
    begins.reset();
}

// Returns where the element's simple duration has reached at `time`, and whether it writes a value.
SvgSimpleTime simpleTimeOf(const SvgTimed& t, double time) {
    SvgSimpleTime out{};
    double elapsed = 0;
    // An interval still to begin changes a later frame, whatever the element writes now.
    out.running = prF64IsFinite(t.begin) && time < t.begin;
    if (t.begun && time >= t.begin && time < t.end) {
        elapsed = time - t.begin;
        out.inside = true;
        // An interval that never ends and whose simple duration is indefinite holds one value.
        out.running = prF64IsFinite(t.end) || (prF64IsFinite(t.duration) && t.duration > 0);
        out.moving = prF64IsFinite(t.duration) && t.duration > 0;
    } else if (t.ran && freezes(t) && prF64IsFinite(t.lastEnd) && time >= t.lastBegin) {
        elapsed = t.lastEnd - t.lastBegin;
    } else {
        return out;
    }
    out.writes = true;
    // An indefinite simple duration holds the value at its start.
    if (!prF64IsFinite(t.duration) || t.duration <= 0)
        return out;
    // The repeats cover part of an active duration that min extends, and the fill decides the rest: a freeze holds
    // their last value, and a remove writes nothing (svg-animations/#MinAttribute).
    double repeated = repeatSpan(t);
    bool held = elapsed >= repeated;
    if (held && out.inside && !freezes(t)) {
        out.writes = false;
        return out;
    }
    if (held)
        elapsed = repeated;
    double iterations = elapsed / t.duration;
    out.iteration = int32_t(prF64Floor(iterations));
    out.progress = iterations - out.iteration;
    if ((held || !out.inside) && out.progress == 0 && out.iteration > 0) {
        --out.iteration;
        out.progress = 1;
    }
    return out;
}

// The specifications of the values of an animation, read from its element.
SvgAnimateSpec specOf(const SvgTimed& t) {
    const PrDomElement* e = t.element;
    SvgAnimateSpec spec{};
    spec.values = attributeOf(e, "values");
    spec.from = attributeOf(e, "from");
    spec.to = attributeOf(e, "to");
    spec.by = attributeOf(e, "by");
    spec.keyTimes = attributeOf(e, "keyTimes");
    spec.keySplines = attributeOf(e, "keySplines");
    spec.calcMode = attributeOf(e, "calcMode");
    spec.additive = attributeOf(e, "additive");
    spec.accumulate = attributeOf(e, "accumulate");
    spec.type = svgAnimateTypeOf(viewOf(t.attributeName));
    spec.set = e->tag == PrDomTag_Set;
    if (e->tag == PrDomTag_AnimateTransform) {
        PrStringView type = attributeOf(e, "type");
        spec.transformType = type.data != nullptr ? type : PrStringView("");
    }
    return spec;
}

// Returns what the units of the target's values resolve against, as the presentation last drew and styled it, which a
// sample reads from inside the presentation's update.
SvgAnimateContext contextOf(const SvgTimed& t) {
    SvgAnimateContext context{};
    SvgLengthContext basis{};
    svgElementLengthBasisAsDrawn(t.target, &basis);
    context.width = double(basis.viewport.x);
    context.height = double(basis.viewport.y);
    context.fontSize = double(basis.fontSize);
    context.axis = svgAnimateAxisOf(viewOf(t.attributeName));
    context.target = t.target;
    context.property = t.property;
    return context;
}

// Returns the value that the overlay gives the attribute, or null if it gives none.
const SvgAnimatedValue* PR_NULLABLE overlayHeld(const SvgOverlay& overlay, const PrDomElement* element, PrStringView name) {
    for (uint32_t i = 0; i < overlay.values.count; ++i) {
        const SvgAnimatedValue& held = overlay.values.data[i];
        if (held.element == element && viewOf(held.name) == name)
            return &held;
    }
    return nullptr;
}

// Returns the text of a value that the overlay holds, which is written the first time that a reader asks for it.
PrStringView heldText(const SvgAnimatedValue& held) {
    if (prRawStringIsNull(&held.text)) {
        PrTextBuffer text{};
        svgAnimateValueWrite(held.value, &text);
        PrRawString written = prTextBufferCopy(&text);
        const_cast<SvgAnimatedValue&>(held).text = prRawStringIsNull(&written) ? prRawStringEmpty() : written;
        prTextBufferReset(&text);
    }
    return viewOf(held.text);
}

const SvgMotion* PR_NULLABLE overlayMotion(const SvgOverlay& overlay, const PrDomElement* element) {
    for (uint32_t i = 0; i < overlay.motions.count; ++i) {
        if (overlay.motions.data[i].element == element)
            return &overlay.motions.data[i];
    }
    return nullptr;
}

// Reads a point of two numbers.
bool readPoint(PrStringView text, PrVec2* out) {
    RawArray<double> numbers;
    bool ok = svgAnimateNumbers(text, &numbers) && numbers.count == 2;
    if (ok)
        *out = PrVec2{float(numbers.data[0]), float(numbers.data[1])};
    numbers.reset();
    return ok;
}

// Appends the path of `data` to `path`, and the vertex of each of its commands to `vertices`.
void appendCommands(const RawArray<PrPathDataCommand>& commands, PrPath* path, RawArray<PrVec2>* vertices) {
    uint32_t count = commands.count;
    RawArray<PrPathDataVertex> ends;
    ends.resize(count);
    prPathDataVertices(commands.data, count, ends.data);
    if (count != 0)
        prPathAppendCommands(path, commands.data, count);
    for (uint32_t i = 0; i < count; ++i)
        vertices->push(ends.data[i].point);
    ends.reset();
}

void appendPathData(PrStringView data, PrPath* path, RawArray<PrVec2>* vertices) {
    RawArray<PrPathDataCommand> commands;
    uint32_t count = prPathDataParse(data, nullptr, 0);
    commands.resize(count);
    prPathDataParse(data, commands.data, count);
    appendCommands(commands, path, vertices);
    commands.reset();
}

// Appends the equivalent path of the path or basic shape `shape` (SVG2/shapes.html), as the presentation last styled
// it, in its user space. Returns false for another element, or a shape that has no path.
bool appendShapePath(const PrDomElement* shape, PrPath* path, RawArray<PrVec2>* vertices) {
    if (svgIs(shape, PrDomTag_Path)) {
        PrStringView data = svgAttribute(shape, "d");
        if (data.size == 0)
            return false;
        appendPathData(data, path, vertices);
        return prPathVerbCount(path) != 0;
    }
    PrCssPresentation* presentation = domIsConnected(shape) ? cssPresentationOf(shape->document) : nullptr;
    const CssComputedStyle* style = presentation != nullptr ? cssPresentationStyle(presentation, shape) : nullptr;
    if (style == nullptr)
        return false;
    SvgNode node{};
    RawArray<PrPathDataCommand> commands;
    bool shaped = svgReadShape(shape, style, &node, &commands);
    if (shaped && commands.count == 0) {
        SvgLengthContext basis{};
        svgElementLengthBasisAsDrawn(shape, &basis);
        svgShapeCommands(node, basis.viewport, &commands);
    }
    if (shaped)
        appendCommands(commands, path, vertices);
    commands.reset();
    return shaped && prPathVerbCount(path) != 0;
}

// Builds the path that an animateMotion moves along (svg-animations/#AnimateMotionElement): the path of the element
// that an mpath child names, or the path attribute, or a line through the points of values, from, to and by. Appends
// the vertex of each segment to `vertices`, which a discrete motion jumps between. Writes true to `stated` for the
// points, which are then the keys.
bool motionPath(const PrDomElement* animation, PrPath* path, RawArray<PrVec2>* vertices, bool* stated) {
    for (const PrDomNode* child = animation->first; child != nullptr; child = child->next) {
        if (child->type != PrDomNodeType_Element || !svgIs(static_cast<const PrDomElement*>(child), PrDomTag_Mpath))
            continue;
        PrStringView href = svgHref(static_cast<const PrDomElement*>(child));
        if (href.size < 2 || href.data[0] != '#')
            break;
        const PrDomElement* shape = domElementById(animation->document, PrStringView{href.data + 1, href.size - 1});
        if (shape == nullptr || !appendShapePath(shape, path, vertices))
            break;
        return true;
    }
    PrStringView data = attributeOf(animation, "path");
    if (prStringTrim(data).size != 0) {
        appendPathData(data, path, vertices);
        return prPathVerbCount(path) != 0;
    }
    RawArray<PrVec2> points;
    PrStringView values = attributeOf(animation, "values");
    bool ok = true;
    if (prStringTrim(values).size != 0) {
        RawArray<PrStringView> items;
        svgAnimateSplit(values, &items);
        for (uint32_t i = 0; i < items.count && ok; ++i) {
            PrVec2 point{};
            ok = readPoint(items.data[i], &point);
            points.push(point);
        }
        items.reset();
    } else {
        PrVec2 from{};
        PrVec2 to{};
        PrVec2 by{};
        bool hasFrom = readPoint(attributeOf(animation, "from"), &from);
        bool hasTo = readPoint(attributeOf(animation, "to"), &to);
        bool hasBy = readPoint(attributeOf(animation, "by"), &by);
        ok = hasTo || hasBy;
        // A to or by animation starts at the element's own position, the origin of the motion.
        points.push(hasFrom ? from : PrVec2{0, 0});
        points.push(hasTo ? to : PrVec2{points.data[0].x + by.x, points.data[0].y + by.y});
    }
    ok = ok && points.count != 0;
    if (ok) {
        prPathMoveTo(path, points.data[0]);
        for (uint32_t i = 1; i < points.count; ++i)
            prPathLineTo(path, points.data[i]);
        vertices->append(points.span());
        *stated = true;
    }
    points.reset();
    return ok;
}

// Writes the transform that an animateMotion gives at `at` (svg-animations/#AnimateMotionElement): a translation to the
// point on its path, turned by its rotate attribute. An additive motion follows `underlying`.
bool motionAt(const SvgTimed& t, const SvgSimpleTime& at, const SvgMotion* PR_NULLABLE underlying, double* matrix) {
    const PrDomElement* e = t.element;
    PrPath* path = prPathCreate();
    bool stated = false;
    RawArray<PrVec2> vertices;
    RawArray<double> fractions;
    bool built = motionPath(e, path, &vertices, &stated) && vertices.count != 0;
    PrPathMeasure* measure = prPathMeasureCreate();
    if (built)
        prPathMeasureSetPath(measure, path, prAffine2Identity(), 0.001f);
    double total = double(prPathMeasureLength(measure));
    PrStringView mode = prStringTrim(attributeOf(e, "calcMode"));
    // A motion paces its values unless it names another mode (svg-animations/#AnimateMotionElementCalcModeAttribute).
    SvgCalcMode calcMode = SvgCalcMode_Paced;
    if (mode == "discrete")
        calcMode = SvgCalcMode_Discrete;
    else if (mode == "linear")
        calcMode = SvgCalcMode_Linear;
    else if (mode == "spline")
        calcMode = SvgCalcMode_Spline;
    bool discrete = calcMode == SvgCalcMode_Discrete;
    PrStringView keyPointsText = attributeOf(e, "keyPoints");
    bool hasKeyPoints = prStringTrim(keyPointsText).size != 0;
    bool vertexKeys = !hasKeyPoints && (discrete || stated);
    if (built && hasKeyPoints) {
        RawArray<PrStringView> items;
        svgAnimateSplit(keyPointsText, &items);
        for (uint32_t i = 0; i < items.count && built; ++i) {
            RawArray<double> one;
            built = svgAnimateNumbers(items.data[i], &one) && one.count == 1 && one.data[0] >= 0 && one.data[0] <= 1;
            if (built)
                fractions.push(one.data[0]);
            one.reset();
        }
        items.reset();
    } else if (built && vertexKeys) {
        double walked = 0;
        for (uint32_t i = 0; i < vertices.count; ++i) {
            if (i != 0)
                walked += prF64Hypot(double(vertices.data[i].x - vertices.data[i - 1].x),
                    double(vertices.data[i].y - vertices.data[i - 1].y));
            fractions.push(walked);
        }
        for (uint32_t i = 0; i < fractions.count; ++i)
            fractions.data[i] = walked > 0 ? fractions.data[i] / walked : 0;
    } else {
        fractions.push(0);
        fractions.push(1);
    }
    uint32_t keys = fractions.count;
    RawArray<double> times;
    RawArray<double> splines;
    PrStringView statedTimes = attributeOf(e, "keyTimes");
    if (built && prStringTrim(statedTimes).size != 0) {
        built = svgAnimateKeyTimes(statedTimes, keys, discrete, &times);
    } else if (calcMode == SvgCalcMode_Paced && !hasKeyPoints) {
        times.append(fractions.span());
    } else {
        for (uint32_t i = 0; i < keys; ++i)
            times.push(discrete ? double(i) / keys : keys == 1 ? 0 : double(i) / (keys - 1));
    }
    if (built && calcMode == SvgCalcMode_Spline)
        built = svgAnimateKeySplines(attributeOf(e, "keySplines"), keys - 1, &splines);
    if (built) {
        double progress = clamp(at.progress, 0.0, 1.0);
        double local = 0;
        uint32_t index = 0;
        if (discrete || keys == 1) {
            while (index + 1 < times.count && times.data[index + 1] <= progress)
                ++index;
        } else {
            while (index + 2 < times.count && times.data[index + 1] <= progress)
                ++index;
            double span = times.data[index + 1] - times.data[index];
            local = span <= 0 ? 0 : (progress - times.data[index]) / span;
        }
        PrPathMeasurePosition position{};
        if (total <= 0) {
            // A path of no length, as a lone moveto is, stays at its first point with a heading of 0.
            position.point = vertices.data[0];
        } else if (discrete && vertexKeys) {
            position = prPathMeasurePosition(measure, float(fractions.data[index] * total));
            position.point = vertices.data[index];
        } else {
            if (calcMode == SvgCalcMode_Spline && index * 4 + 3 < splines.count)
                local = svgAnimateSpline(splines.data + index * 4, local);
            double fraction = fractions.data[index];
            if (index + 1 < keys)
                fraction += (fractions.data[index + 1] - fraction) * clamp(local, 0.0, 1.0);
            position = prPathMeasurePosition(measure, float(fraction * total));
        }
        PrVec2 point = position.point;
        // A cumulative motion adds the point that it ends at once for each repeat that ran.
        if (at.iteration > 0 && prStringTrim(attributeOf(e, "accumulate")) == "sum") {
            PrVec2 end = vertices.last();
            if (total > 0 && (!discrete || !vertexKeys))
                end = prPathMeasurePosition(measure, float(fractions.data[keys - 1] * total)).point;
            point = PrVec2{point.x + end.x * at.iteration, point.y + end.y * at.iteration};
        }
        double heading = total > 0 ? prF64Atan2(double(position.tangent.y), double(position.tangent.x)) * 180 / kPrF64Pi : 0;
        PrStringView rotate = prStringTrim(attributeOf(e, "rotate"));
        double angle = 0;
        if (rotate == "auto")
            angle = heading;
        else if (rotate == "auto-reverse")
            angle = heading + 180;
        else
            prF64ParsePrefix(rotate, &angle);
        double radians = angle * kPrF64Pi / 180;
        double c = prF64Cos(radians);
        double s = prF64Sin(radians);
        double result[6] = {c, s, -s, c, double(point.x), double(point.y)};
        if (underlying != nullptr && prStringTrim(attributeOf(e, "additive")) == "sum") {
            const double* u = underlying->matrix;
            double product[6] = {
                u[0] * result[0] + u[2] * result[1], u[1] * result[0] + u[3] * result[1],
                u[0] * result[2] + u[2] * result[3], u[1] * result[2] + u[3] * result[3],
                u[0] * result[4] + u[2] * result[5] + u[4], u[1] * result[4] + u[3] * result[5] + u[5],
            };
            for (uint32_t i = 0; i < 6; ++i)
                result[i] = product[i];
        }
        for (uint32_t i = 0; i < 6; ++i)
            matrix[i] = result[i];
    }
    times.reset();
    splines.reset();
    vertices.reset();
    fractions.reset();
    prPathMeasureDestroy(measure);
    prPathDestroy(path);
    return built;
}

void observeEvent(void* PR_NULLABLE context, PrDomEvent*) {
    SvgEventHook* hook = static_cast<SvgEventHook*>(context);
    SvgAnimations* a = hook->owner;
    SvgTimed* t = timedOf(a, hook->animation);
    if (t == nullptr)
        return;
    addInstanceTime(a, t, hook->endList, containerTime(a, t->container) + hook->offset);
}

PrDomEventListener listenerOf(SvgEventHook* hook) {
    PrDomEventListener listener{};
    listener.invoke = observeEvent;
    listener.context = hook;
    listener.identity = uint64_t(uintptr_t(hook));
    return listener;
}

void removeHooks(SvgTimed* t) {
    for (uint32_t i = 0; i < t->hooks.count; ++i) {
        SvgEventHook* hook = t->hooks.data[i];
        PrDomEventListener listener = listenerOf(hook);
        prDomEventTargetRemoveEventListener(hook->base, viewOf(hook->type), &listener, nullptr);
        prRawStringReset(&hook->type);
        prMemoryFree(hook);
    }
    t->hooks.reset();
}

// Listens for each event value of the element's lists, which add instance times when they fire.
void addHooks(SvgAnimations* a, SvgTimed* t) {
    for (uint32_t list = 0; list < 2; ++list) {
        const RawArray<SvgTimeSpec>& specs = list == 0 ? t->beginSpecs : t->endSpecs;
        for (uint32_t i = 0; i < specs.count; ++i) {
            const SvgTimeSpec& spec = specs.data[i];
            PrDomElement* base = spec.kind == SvgTimeKind_Event ? specBase(*t, spec) : nullptr;
            if (base == nullptr)
                continue;
            SvgEventHook* hook = allocZeroed<SvgEventHook>();
            *hook = SvgEventHook{a, t->element, base, prRawStringCopy(viewOf(spec.name)), spec.offset, list == 1};
            t->hooks.push(hook);
            PrDomEventListener listener = listenerOf(hook);
            prDomEventTargetAddEventListener(base, viewOf(hook->type), &listener, nullptr);
        }
    }
}

void timedReset(SvgAnimations*, SvgTimed* t) {
    removeHooks(t);
    prRawStringReset(&t->attributeName);
    specsReset(&t->beginSpecs);
    specsReset(&t->endSpecs);
    t->beginInstances.reset();
    t->endInstances.reset();
    t->intervals.reset();
    svgAnimateFunctionReset(&t->function);
    svgAnimateValueReset(&t->base);
    t->compiled = false;
    t->baseRead = false;
}

// Returns true if no element from the animation to the root fails its conditional processing, which leaves an animation
// untimed (svg-animations/#ConditionalProcessing).
bool timedInTree(const PrDomElement* element) {
    for (const PrDomNode* at = element; at != nullptr && at->type == PrDomNodeType_Element; at = at->parent) {
        if (!svgConditionsHold(static_cast<const PrDomElement*>(at)))
            return false;
    }
    return true;
}

// Finds the document's animation elements again, keeping the timing state of those that it had and dropping the others.
void scanDocument(SvgAnimations* a) {
    for (uint32_t i = 0; i < a->animations.count; ++i)
        a->animations.data[i].present = false;
    PrDomNode* root = a->document;
    for (PrDomNode* node = root; node != nullptr; node = domNodeNext(root, node)) {
        if (node->type != PrDomNodeType_Element)
            continue;
        PrDomElement* element = static_cast<PrDomElement*>(node);
        if (!domIsSvgAnimation(element) || !timedInTree(element))
            continue;
        SvgTimed read{};
        readTimed(a, element, &read);
        read.present = true;
        SvgTimed* was = timedOf(a, element);
        if (was == nullptr) {
            a->animations.push(read);
            continue;
        }
        // The state of its intervals carries over.
        read.beginInstances.append(was->beginInstances.span());
        read.endInstances.append(was->endInstances.span());
        read.intervals.append(was->intervals.span());
        read.begin = was->begin;
        read.end = was->end;
        read.lastBegin = was->lastBegin;
        read.lastEnd = was->lastEnd;
        read.sampledBegin = was->sampledBegin;
        read.sampledActive = was->sampledActive;
        read.joined = was->joined;
        read.iteration = was->iteration;
        read.begun = was->begun;
        read.ran = was->ran;
        timedReset(a, was);
        *was = read;
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < a->animations.count; ++i) {
        SvgTimed& t = a->animations.data[i];
        if (!t.present) {
            timedReset(a, &t);
            continue;
        }
        a->animations.data[kept++] = t;
    }
    a->animations.count = kept;
    // The elements are in document order, which orders the animations of one attribute that begin together.
    stableSortItems(a->animations.span(), [](const SvgTimed& x, const SvgTimed& y) {
        return domNodePrecedes(x.element, y.element);
    });
    for (uint32_t i = 0; i < a->animations.count; ++i)
        addHooks(a, &a->animations.data[i]);
    a->scan = false;
}

// Queues the events that the element's interval crossed since the last sample. A seek queues no repeat event.
void queueIntervalEvents(SvgAnimations* a, SvgTimed* t, double time, bool seek) {
    SvgSimpleTime at = simpleTimeOf(*t, time);
    if (at.inside && !seek && at.iteration > t->iteration && t->begin == t->sampledBegin) {
        int32_t from = t->iteration + kMaxRepeatEvents < at.iteration ? at.iteration - kMaxRepeatEvents : t->iteration;
        for (int32_t iteration = from + 1; iteration <= at.iteration; ++iteration)
            queueTimeEvent(a, *t, "repeatEvent", iteration, time);
    }
    t->iteration = at.inside ? at.iteration : 0;
    if (t->sampledActive && (!at.inside || t->begin != t->sampledBegin))
        queueTimeEvent(a, *t, "endEvent", 0, time);
    if (at.inside && (!t->sampledActive || t->begin != t->sampledBegin))
        queueTimeEvent(a, *t, "beginEvent", 0, time);
    t->sampledActive = at.inside;
    t->sampledBegin = t->begin;
}

// Drops the intervals of a container's elements, so that a seek back through them runs them again.
void rewindContainer(SvgAnimations* a, uint32_t container) {
    for (uint32_t i = 0; i < a->animations.count; ++i) {
        SvgTimed& t = a->animations.data[i];
        if (t.container != container)
            continue;
        t.begin = kPrF64Infinity;
        t.end = kPrF64Infinity;
        t.lastBegin = kPrF64Infinity;
        t.lastEnd = kPrF64Infinity;
        t.intervals.clear();
        t.iteration = 0;
        t.begun = false;
        t.ran = false;
        t.joined = -kPrF64Infinity;
        ++a->revision;
    }
}

double containerTime(const SvgAnimations* a, uint32_t container) {
    const SvgTimeContainer& c = a->containers.data[container];
    if (!c.begun)
        return 0;
    return c.paused ? c.held : (documentTime(a) - c.origin) / 1000;
}

// Begins the time of each container that has not begun, at the time that a seek kept for it, or else at zero.
void beginContainers(SvgAnimations* a) {
    for (uint32_t i = 0; i < a->containers.count; ++i) {
        SvgTimeContainer& c = a->containers.data[i];
        if (c.begun)
            continue;
        c.begun = true;
        c.origin = documentTime(a) - c.held * 1000;
        a->seek = a->seek || c.seeked;
        c.seeked = false;
    }
}

// Adds a length as a CSS length: a percentage, or a dimension, a number being pixels.
void addLength(CssValuePool* pool, double number, PrCssUnit unit) {
    if (unit == PrCssUnit_Percent)
        cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Percentage, PrCssUnit_Percent, number));
    else if (unit == PrCssUnit_Number)
        cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Dimension, PrCssUnit_Px, number));
    else
        cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Dimension, unit, number));
}

// Adds the arguments of a CSS function, separated by commas, each a dimension of `unit` or a number, and closes the
// function that starts at `at`.
void addArguments(CssValuePool* pool, uint32_t at, const double* numbers, uint32_t count, PrCssUnit unit) {
    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0)
            cssValuePoolAdd(pool, cssLiteralValue(','));
        PrCssValueKind kind = unit == PrCssUnit_Number ? PrCssValueKind_Number : PrCssValueKind_Dimension;
        cssValuePoolAdd(pool, cssNumberValue(kind, unit, numbers[i]));
    }
    pool->values.data[at].next = pool->values.count - at;
}

void addFunction(CssValuePool* pool, PrCssKeyword name, const double* numbers, uint32_t count, PrCssUnit unit) {
    uint32_t at = cssValuePoolAdd(pool, cssFunctionValue(name));
    addArguments(pool, at, numbers, count, unit);
}

// Adds an SVG transform function as the CSS transform functions that do the same (css-transforms-1/#svg-syntax): a
// translation in pixels, angles in degrees, and a rotation about a center as a rotation between two translations.
void addTransform(CssValuePool* pool, const PrSvgTransform& transform) {
    double v[6];
    for (uint32_t i = 0; i < 6; ++i)
        v[i] = double(transform.values[i]);
    switch (transform.type) {
        case PrSvgTransformType_Matrix: addFunction(pool, PrCssKeyword_Matrix, v, 6, PrCssUnit_Number); break;
        case PrSvgTransformType_Translate: addFunction(pool, PrCssKeyword_Translate, v, 2, PrCssUnit_Px); break;
        case PrSvgTransformType_Scale: addFunction(pool, PrCssKeyword_Scale, v, 2, PrCssUnit_Number); break;
        case PrSvgTransformType_Rotate: {
            double to[2] = {v[1], v[2]};
            double back[2] = {-v[1], -v[2]};
            bool centered = v[1] != 0 || v[2] != 0;
            if (centered)
                addFunction(pool, PrCssKeyword_Translate, to, 2, PrCssUnit_Px);
            addFunction(pool, PrCssKeyword_Rotate, v, 1, PrCssUnit_Deg);
            if (centered)
                addFunction(pool, PrCssKeyword_Translate, back, 2, PrCssUnit_Px);
            break;
        }
        case PrSvgTransformType_SkewX: addFunction(pool, PrCssKeyword_Skewx, v, 1, PrCssUnit_Deg); break;
        default: addFunction(pool, PrCssKeyword_Skewy, v, 1, PrCssUnit_Deg); break;
    }
}

// Adds the value of a presentation attribute as the CSS value of its property. Returns false for a value that only its
// text gives, as one that does not interpolate does.
bool addValue(CssValuePool* pool, PrCssProperty property, const SvgAnimateValue& value) {
    if (!value.valid)
        return false;
    switch (value.type) {
        case PrSvgValueType_Length:
            addLength(pool, value.numbers.data[0], value.units.data[0]);
            return true;
        case PrSvgValueType_LengthList:
            for (uint32_t i = 0; i < value.numbers.count; ++i) {
                if (i != 0)
                    cssValuePoolAdd(pool, cssLiteralValue(','));
                addLength(pool, value.numbers.data[i], value.units.data[i]);
            }
            return value.numbers.count != 0;
        case PrSvgValueType_Number:
        case PrSvgValueType_Integer:
            // pathLength is a length in CSS, of user units.
            if (property == PrCssProperty_PathLength)
                cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Dimension, PrCssUnit_Px, value.numbers.data[0]));
            else
                cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, value.numbers.data[0]));
            return true;
        case PrSvgValueType_Color:
            cssValuePoolAddLegacyColor(pool, value.color);
            return true;
        case PrSvgValueType_Path: {
            // d takes path data in path().
            uint32_t at = cssValuePoolAdd(pool, cssFunctionValue(PrCssKeyword_Path));
            cssValuePoolAddPathData(pool, value.path.data, value.path.count);
            pool->values.data[at].next = pool->values.count - at;
            return property == PrCssProperty_D && value.path.count != 0;
        }
        case PrSvgValueType_TransformList: {
            RawArray<PrSvgTransform> list;
            svgAnimateTransforms(value, &list);
            for (uint32_t i = 0; i < list.count; ++i)
                addTransform(pool, list.data[i]);
            bool any = list.count != 0;
            list.reset();
            return any;
        }
        default:
            return false;
    }
}

// A property's value, as a span of a pool of values.
struct SvgDeclared {
    PrCssProperty property;
    CssValueSpan span;
};

// Gives the element the declarations of the overlay's values of its presentation attributes, and of its motion, which
// precedes its own transform. A value is given as the CSS value that it is, and a value that only text gives as its
// text.
void declare(SvgAnimations* a, const SvgOverlay& overlay, PrDomElement* element) {
    PrTextBuffer text = {};
    CssValuePool pool = {};
    RawArray<SvgDeclared> declared;
    const SvgMotion* motion = overlayMotion(overlay, element);
    const SvgAnimatedValue* transformed = nullptr;
    bool any = false;
    for (uint32_t i = 0; i < overlay.values.count; ++i) {
        const SvgAnimatedValue& held = overlay.values.data[i];
        if (held.element != element || held.property == PrCssProperty_Count)
            continue;
        any = true;
        // A motion writes the transform, with the element's own after it.
        if (held.property == PrCssProperty_Transform) {
            transformed = &held;
            if (motion != nullptr)
                continue;
        }
        uint32_t start = pool.values.count;
        if (addValue(&pool, held.property, held.value))
            declared.push(SvgDeclared{held.property, CssValueSpan{start, pool.values.count - start}});
        else
            svgPresentationDeclare(&text, held.property, heldText(held));
    }
    if (motion != nullptr) {
        // The motion is outside the element's transform (svg-animations/#AnimateMotionElement).
        any = true;
        uint32_t start = pool.values.count;
        addFunction(&pool, PrCssKeyword_Matrix, motion->matrix, 6, PrCssUnit_Number);
        const SvgAnimateValue* own = transformed != nullptr ? &transformed->value : motion->own;
        if (own != nullptr) {
            RawArray<PrSvgTransform> list;
            svgAnimateTransforms(*own, &list);
            for (uint32_t i = 0; i < list.count; ++i)
                addTransform(&pool, list.data[i]);
            list.reset();
        }
        declared.push(SvgDeclared{PrCssProperty_Transform, CssValueSpan{start, pool.values.count - start}});
    }
    if (!any) {
        cssElementDropAnimatedStyle(element);
    } else {
        a->styled = true;
        CssDeclarationBlock* block = cssElementAnimatedStyle(element);
        PrStringView written = prTextBufferView(&text);
        cssDeclarationBlockParse(block, written.data != nullptr ? written : PrStringView(""), &a->parser,
            CssOrigin_Author);
        // A value that its property does not take is in error, and declares nothing
        // (SVG2/styling.html#PresentationAttributes).
        for (uint32_t i = 0; i < declared.count; ++i) {
            CssValueRun run = cssValuePoolRun(&pool, declared.data[i].span);
            cssDeclarationBlockSetValue(block, cssPropertyDeclaredName(declared.data[i].property), run, false, false,
                &a->parser);
        }
        cssElementAnimated(element);
    }
    declared.reset();
    cssValuePoolReset(&pool);
    prTextBufferReset(&text);
}

bool sameMotion(const SvgMotion* PR_NULLABLE a, const SvgMotion* PR_NULLABLE b) {
    if ((a == nullptr) != (b == nullptr))
        return false;
    for (uint32_t i = 0; a != nullptr && i < 6; ++i) {
        if (a->matrix[i] != b->matrix[i])
            return false;
    }
    if (a != nullptr && (a->own != b->own || a->ownRead != b->ownRead))
        return false;
    return true;
}

// Gives each element whose animated class differs between two overlays its classes: the animated value, or its class
// attribute's again once no animation gives it one. Selectors match the classes, and the attribute keeps its value.
void changeClasses(SvgAnimations* a, const SvgOverlay& before, const SvgOverlay& after) {
    PrCssPresentation* presentation = cssPresentationOf(a->document);
    for (uint32_t pass = 0; pass < 2; ++pass) {
        const SvgOverlay& from = pass == 0 ? before : after;
        const SvgOverlay& to = pass == 0 ? after : before;
        for (uint32_t i = 0; i < from.values.count; ++i) {
            const SvgAnimatedValue& held = from.values.data[i];
            if (viewOf(held.name) != "class")
                continue;
            const SvgAnimatedValue* other = overlayHeld(to, held.element, "class");
            // An element in both is handled once, in the pass over the overlay after.
            if (other != nullptr && (pass == 0 || svgAnimateValueEqual(other->value, held.value)))
                continue;
            const SvgAnimatedValue* was = pass == 0 ? &held : other;
            const SvgAnimatedValue* now = pass == 0 ? nullptr : &held;
            PrStringView base = domAttributeValue(held.element, "class");
            PrStringView oldText = was != nullptr ? heldText(*was) : base;
            PrStringView newText = now != nullptr ? heldText(*now) : base;
            domElementSetClasses(held.element, now != nullptr ? newText : PrStringView{nullptr, 0});
            if (presentation != nullptr)
                cssPresentationClassesChanged(presentation, held.element, oldText, newText);
        }
    }
}

// Restyles the elements whose values or motion differ between two overlays.
void changeClasses(SvgAnimations* a, const SvgOverlay& before, const SvgOverlay& after);

void applyChanges(SvgAnimations* a, const SvgOverlay& before, const SvgOverlay& after) {
    RawArray<PrDomElement*> declared;
    RawArray<PrDomElement*> animated;
    // The attributes whose values changed and that are not CSS properties. The drawing reads them as it reads the
    // element's own attributes.
    RawArray<const SvgAnimatedValue*> attributes;
    auto note = [](RawArray<PrDomElement*>* list, PrDomElement* element) {
        if (list->find(element) == kArrayMissing)
            list->push(element);
    };
    for (uint32_t pass = 0; pass < 2; ++pass) {
        const SvgOverlay& from = pass == 0 ? before : after;
        const SvgOverlay& to = pass == 0 ? after : before;
        for (uint32_t i = 0; i < from.values.count; ++i) {
            const SvgAnimatedValue& held = from.values.data[i];
            const SvgAnimatedValue* other = overlayHeld(to, held.element, viewOf(held.name));
            if (other != nullptr && svgAnimateValueEqual(other->value, held.value))
                continue;
            note(held.property != PrCssProperty_Count ? &declared : &animated, held.element);
            if (held.property == PrCssProperty_Count)
                attributes.push(&held);
        }
        for (uint32_t i = 0; i < from.motions.count; ++i) {
            const SvgMotion* motion = &from.motions.data[i];
            if (!sameMotion(motion, overlayMotion(to, motion->element)))
                note(&declared, motion->element);
        }
    }
    for (uint32_t i = 0; i < declared.count; ++i)
        declare(a, after, declared.data[i]);
    changeClasses(a, before, after);
    for (uint32_t i = 0; i < animated.count; ++i) {
        if (declared.find(animated.data[i]) == kArrayMissing)
            cssElementAnimated(animated.data[i]);
    }
    for (uint32_t i = 0; i < attributes.count; ++i) {
        PrDomElement* element = attributes.data[i]->element;
        PrStringView name = viewOf(attributes.data[i]->name);
        // A use's shadow tree instances what its animated href names.
        if (name == "href")
            domUsesAttributeChanged(element, PrDomNamespace_None, name);
        svgContentAttributeAnimated(element, name);
    }
    declared.reset();
    animated.reset();
    attributes.reset();
}

// Returns the target's own value of the animated attribute, or its initial value if it has none, as the function's type
// reads it. A value that depends on its context is read again each time.
const SvgAnimateValue& baseOf(SvgTimed* t, const SvgAnimateContext& context) {
    if (t->baseRead)
        return t->base;
    PrStringView name = viewOf(t->attributeName);
    PrStringView text = svgBaseAttribute(t->target, name);
    if (text.size == 0)
        text = svgAnimateInitialOf(name);
    svgAnimateValueRead(context, t->function.type, text, &t->base);
    t->baseRead = !t->base.contextual;
    ++t->baseReads;
    return t->base;
}

// Returns the target's own transform, which a motion precedes, read once until its attribute changes.
const SvgAnimateValue& ownTransformOf(SvgTimed* t, const SvgAnimateContext& context) {
    if (!t->baseRead) {
        svgAnimateValueRead(context, PrSvgValueType_TransformList, svgBaseAttribute(t->target, "transform"), &t->base);
        t->baseRead = true;
        ++t->baseReads;
    }
    return t->base;
}

// Samples every animation at its container's time: walks the intervals, queues the events, computes the values, and
// restyles what changed.
void sample(SvgAnimations* a) {
    if (a->sampling)
        return;
    a->sampling = true;
    if (a->scan)
        scanDocument(a);
    bool seek = a->seek;
    a->seek = false;
    for (uint32_t i = 0; i < a->containers.count; ++i) {
        double time = containerTime(a, i);
        if (time < a->containers.data[i].sampled)
            rewindContainer(a, i);
        a->containers.data[i].sampled = time;
    }
    // A syncbase resolves against the intervals of its base, and an event value against the events that a begin or an
    // end queues, so the walk repeats until it settles.
    for (uint32_t pass = 0; pass < a->animations.count + 2; ++pass) {
        uint64_t was = a->revision;
        for (uint32_t i = 0; i < a->animations.count; ++i) {
            SvgTimed& t = a->animations.data[i];
            double time = containerTime(a, t.container);
            advance(a, &t, time, seek);
            queueIntervalEvents(a, &t, time, seek);
        }
        if (a->revision == was)
            break;
    }
    SvgOverlay& before = a->overlays[a->front];
    SvgOverlay& after = a->overlays[a->front ^ 1];
    overlayClear(&after);
    bool active = false;
    bool moving = false;
    for (uint32_t i = 0; i < a->animations.count; ++i) {
        SvgTimed& t = a->animations.data[i];
        double time = containerTime(a, t.container);
        SvgSimpleTime at = simpleTimeOf(t, time);
        active = active || (at.running && !a->containers.data[t.container].paused);
        moving = moving || (at.moving && !a->containers.data[t.container].paused);
        if (!at.writes || t.target == nullptr || t.inert)
            continue;
        double priority = at.inside ? t.begin : t.lastBegin;
        uint32_t repeat = at.iteration > 0 ? uint32_t(at.iteration) : 0;
        if (t.motion) {
            const SvgMotion* below = overlayMotion(after, t.target);
            SvgMotion motion{t.target, {}, nullptr, 0};
            if (!motionAt(t, at, below, motion.matrix))
                continue;
            motion.own = &ownTransformOf(&t, contextOf(t));
            motion.ownRead = t.baseReads;
            if (below != nullptr)
                *const_cast<SvgMotion*>(below) = motion;
            else
                after.motions.push(motion);
            continue;
        }
        PrStringView name = viewOf(t.attributeName);
        if (name.size == 0)
            continue;
        // The animations of one attribute compose in order, each over what those below it wrote.
        SvgAnimateContext context = contextOf(t);
        if (!t.compiled || (t.function.flags & SvgAnimateFlag_Contextual) != 0) {
            t.compiles = svgAnimateCompile(context, specOf(t), &t.function);
            // A function that does not compile may compile in a later context, as one that reads a style that is not
            // known yet does, so it is tried again at the next sample.
            t.compiled = t.compiles;
        }
        if (!t.compiles)
            continue;
        const SvgAnimatedValue* below = overlayHeld(after, t.target, name);
        const SvgAnimateValue& underlying = below != nullptr ? below->value : baseOf(&t, context);
        SvgAnimateValue value{};
        if (!svgAnimateSample(context, t.function, underlying, at.progress, repeat, &value)) {
            svgAnimateValueReset(&value);
            continue;
        }
        uint32_t slot = kArrayMissing;
        for (uint32_t j = 0; j < after.values.count && slot == kArrayMissing; ++j) {
            if (after.values.data[j].element == t.target && viewOf(after.values.data[j].name) == name)
                slot = j;
        }
        // The animation with the later begin writes over the one before it, and document order breaks a tie.
        if (slot != kArrayMissing && after.values.data[slot].begin > priority) {
            svgAnimateValueReset(&value);
            continue;
        }
        if (slot == kArrayMissing) {
            slot = after.values.count;
            after.values.push(SvgAnimatedValue{t.target, prRawStringCopy(name), {}, {}, priority, t.property});
        }
        SvgAnimatedValue& held = after.values.data[slot];
        svgAnimateValueReset(&held.value);
        prRawStringReset(&held.text);
        held.value = value;
        held.begin = priority;
    }
    a->front ^= 1;
    applyChanges(a, before, after);
    overlayClear(&before);
    a->unsampled = false;
    a->active = active;
    a->moving = moving;
    a->sampling = false;
}

// The driver of the document's animations on its timeline (CssAnimationDriver).
void driverUpdate(void* PR_NULLABLE context, double) {
    sample(static_cast<SvgAnimations*>(context));
}

double driverNextEventTime(void* PR_NULLABLE context, double time) {
    const SvgAnimations* a = static_cast<const SvgAnimations*>(context);
    if (a->unsampled || a->scan || a->events.count != 0)
        return time;
    if (!a->active)
        return -1;
    // The next event of an element, as CSS's animations count theirs (cssAnimateNextEventTime): the begin of an
    // interval still to come, the end of the current one, and the repeats of one that repeats a finite number of times.
    // The repeats of one that repeats indefinitely do not count, so a page with one is idle once it has begun.
    double next = -1;
    for (uint32_t i = 0; i < a->animations.count; ++i) {
        const SvgTimed& t = a->animations.data[i];
        const SvgTimeContainer& c = a->containers.data[t.container];
        if (c.paused)
            continue;
        double now = containerTime(a, t.container);
        double due = kPrF64Infinity;
        if (t.begun && now >= t.begin && now < t.end) {
            due = t.end;
            if (prF64IsFinite(t.end) && prF64IsFinite(t.duration) && t.duration > 0) {
                double repeat = t.begin + (prF64Floor((now - t.begin) / t.duration) + 1) * t.duration;
                due = prF64Min(due, repeat);
            }
        } else if (prF64IsFinite(t.begin) && now < t.begin) {
            due = t.begin;
        }
        if (!prF64IsFinite(due))
            continue;
        double at = prF64Max(c.origin + due * 1000, time);
        next = next < 0 ? at : prF64Min(next, at);
    }
    return next;
}

bool driverAnimating(void* PR_NULLABLE context) {
    const SvgAnimations* a = static_cast<const SvgAnimations*>(context);
    // A begin, an end or a repeat ahead is an event (driverNextEventTime), which wakes a frame at its time.
    return a->moving || a->unsampled || a->scan;
}

bool driverHasEvents(void* PR_NULLABLE context) {
    return static_cast<const SvgAnimations*>(context)->events.count != 0;
}

// Dispatches the events that wait, in the order that the samples queued them. Listeners may queue more, which wait for
// the next update.
void driverSendEvents(void* PR_NULLABLE context) {
    SvgAnimations* a = static_cast<SvgAnimations*>(context);
    RawArray<SvgQueuedEvent> events = a->events;
    a->events = {};
    for (uint32_t i = 0; i < events.count; ++i) {
        domNodeHold(events.data[i].target);
    }
    for (uint32_t i = 0; i < events.count; ++i) {
        const SvgQueuedEvent& e = events.data[i];
        PrSvgTimeEventInit init = prSvgTimeEventInitDefault();
        init.detail = e.detail;
        PrDomEvent* event = prSvgTimeEventAsEvent(prSvgTimeEventCreate(e.type, &init));
        event->trusted = true;
        domEventDispatch(e.target, event);
        domEventRelease(event);
    }
    for (uint32_t i = 0; i < events.count; ++i)
        domNodeUnhold(events.data[i].target);
    events.reset();
}

// Drops what the engine holds of the elements in the subtree of `node`, which leaves the document.
void dropSubtree(SvgAnimations* a, PrDomNode* node) {
    auto inside = [&](const PrDomNode* PR_NULLABLE element) {
        return element != nullptr && domIsInclusiveAncestor(node, element);
    };
    uint32_t kept = 0;
    for (uint32_t i = 0; i < a->animations.count; ++i) {
        SvgTimed& t = a->animations.data[i];
        bool hooked = false;
        for (uint32_t j = 0; j < t.hooks.count; ++j)
            hooked = hooked || inside(t.hooks.data[j]->base);
        if (inside(t.element) || inside(t.target) || hooked) {
            timedReset(a, &t);
            continue;
        }
        a->animations.data[kept++] = t;
    }
    a->animations.count = kept;
    for (uint32_t which = 0; which < 2; ++which) {
        SvgOverlay& overlay = a->overlays[which];
        uint32_t values = 0;
        for (uint32_t i = 0; i < overlay.values.count; ++i) {
            SvgAnimatedValue& held = overlay.values.data[i];
            if (inside(held.element)) {
                // Nor the classes that the animations gave them, if the overlay is the current one.
                if (which == a->front && viewOf(held.name) == "class")
                    domElementSetClasses(held.element, PrStringView{nullptr, 0});
                heldReset(&held);
                continue;
            }
            overlay.values.data[values++] = held;
        }
        overlay.values.count = values;
        uint32_t motions = 0;
        for (uint32_t i = 0; i < overlay.motions.count; ++i) {
            if (!inside(overlay.motions.data[i].element))
                overlay.motions.data[motions++] = overlay.motions.data[i];
        }
        overlay.motions.count = motions;
    }
    uint32_t events = 0;
    for (uint32_t i = 0; i < a->events.count; ++i) {
        if (!inside(a->events.data[i].target))
            a->events.data[events++] = a->events.data[i];
    }
    a->events.count = events;
    // A time container keeps its time while its svg element is out of the document, so that the element's animations
    // continue if it is inserted again. The element names it until it is freed (svgAnimationsElementDestroyed).
    for (uint32_t i = 0; i < a->containers.count; ++i) {
        if (inside(a->containers.data[i].root))
            cssElementFollow(a->containers.data[i].root);
    }
    // The elements keep no animated declarations outside the document.
    for (PrDomNode* at = a->styled ? node : nullptr; at != nullptr; at = domNodeNext(node, at)) {
        if (at->type == PrDomNodeType_Element && static_cast<PrDomElement*>(at)->style != nullptr &&
                static_cast<PrDomElement*>(at)->style->animated != nullptr)
            cssElementDropAnimatedStyle(static_cast<PrDomElement*>(at));
    }
    a->scan = true;
}

// Returns the container of the outermost svg element of `element`, created if there is none.
uint32_t containerOf(SvgAnimations* a, PrDomElement* element) {
    PrDomElement* root = containerRootOf(element);
    if (root == nullptr)
        root = element;
    for (uint32_t i = 0; i < a->containers.count; ++i) {
        if (a->containers.data[i].root == root)
            return i;
    }
    a->containers.push(newContainer(a, root));
    return a->containers.count - 1;
}

// Returns the animations of the element's document, or null if SVG does not follow it.
SvgAnimations* PR_NULLABLE animationsFor(const PrDomElement* element) {
    return svgDocumentAnimations(element->document);
}

// Returns the timing of the animation element, with the document's changes and instance times sampled first, or null.
SvgTimed* PR_NULLABLE settledTimed(SvgAnimations* a, const PrDomElement* element) {
    if (a->scan || a->unsampled)
        sample(a);
    return timedOf(a, element);
}

// Finds the animation elements again in a task of its own, if a sample has not found them first, so that the listeners
// of event values listen before a later task dispatches their events (svg-animations/#EventValueSyntax).
void scanTask(void* data) {
    PrDomDocument* document = static_cast<PrDomDocument*>(data);
    if (SvgAnimations* a = animationsOf(document)) {
        a->scanQueued = false;
        // The elements that the scan finds begin and end at the next sample.
        if (a->scan && !a->sampling) {
            scanDocument(a);
            a->unsampled = true;
        }
    }
    domNodeRelease(document);
}

void requestScan(SvgAnimations* a) {
    a->scan = true;
    const DomDocumentHost* host = a->document->host;
    if (a->scanQueued || host == nullptr)
        return;
    a->scanQueued = true;
    domNodeRetain(a->document);
    // A microtask runs before a script that the parser meets, which may dispatch an event that a value listens for.
    if (host->queueMicrotask(host->context, scanTask, a->document))
        return;
    host->queueTask(host->context, scanTask, a->document, [](void* data) {
        PrDomDocument* document = static_cast<PrDomDocument*>(data);
        if (SvgAnimations* a = animationsOf(document))
            a->scanQueued = false;
        domNodeRelease(document);
    });
}

// Returns the value that the animations give the attribute of `element`, or of its original if it is an instance in a
// use element's shadow tree (SVG2/struct.html#UseAnimations), or null.
const SvgAnimatedValue* PR_NULLABLE animatedHeld(const PrDomElement* element, PrStringView name) {
    const SvgAnimations* a = animationsOf(element->document);
    if (a == nullptr)
        return nullptr;
    const SvgAnimatedValue* held = overlayHeld(a->overlays[a->front], element, name);
    if (held == nullptr && a->overlays[a->front].values.count != 0 && domInUseShadowTree(element)) {
        if (const PrDomElement* original = domElementFromNode(domNodeUseOriginal(element)))
            held = overlayHeld(a->overlays[a->front], original, name);
    }
    return held;
}

} // namespace

SvgAnimations* svgAnimationsCreate(PrDomDocument* document) {
    SvgAnimations* a = allocZeroed<SvgAnimations>();
    a->document = document;
    a->scan = true;
    CssAnimationDriver driver{a, driverUpdate, driverNextEventTime, driverAnimating, driverHasEvents, driverSendEvents};
    cssAnimationDriverAdd(document, driver);
    document->animatedHref = [](const PrDomElement* use) { return svgAnimatedAttribute(use, "href"); };
    return a;
}

void svgAnimationsDestroy(SvgAnimations* PR_NULLABLE animations) {
    SvgAnimations* a = animations;
    if (a == nullptr)
        return;
    cssAnimationDriverRemove(a->document, a);
    a->document->animatedHref = nullptr;
    if (a->parser != nullptr) {
        cssParserReset(a->parser);
        prMemoryFree(a->parser);
    }
    for (uint32_t i = 0; i < a->animations.count; ++i)
        timedReset(a, &a->animations.data[i]);
    a->animations.reset();
    for (SvgOverlay& overlay : a->overlays) {
        overlayClear(&overlay);
        overlay.values.reset();
        overlay.motions.reset();
    }
    a->containers.reset();
    a->events.reset();
    prMemoryFree(a);
}

void svgAnimationsReadinessChanged(SvgAnimations* animations) {
    SvgAnimations* a = animations;
    if (a->document->readiness != DomReadiness_Complete)
        return;
    beginContainers(a);
    // The containers' new times sample at once, so that the load event's listeners read their values.
    a->unsampled = true;
    sample(a);
}

void svgAnimationsTreeChanged(SvgAnimations* animations, const PrDomNode* node) {
    // A document that has never created an animation element inserts none.
    for (const PrDomNode* at = node->document->animations ? node : nullptr; at != nullptr; at = domNodeNext(node, at)) {
        if (at->type == PrDomNodeType_Element && domIsSvgAnimation(static_cast<const PrDomElement*>(at))) {
            requestScan(animations);
            return;
        }
    }
    // Another node may be what the animations reference.
    if (animations->animations.count != 0)
        animations->scan = true;
}

void svgAnimationsAttributeChanged(SvgAnimations* animations, PrDomElement* element, PrStringView name) {
    // Without animations and the values that they gave, only a change to an animation element can matter.
    if (animations->animations.count == 0 && animations->overlays[animations->front].values.count == 0 &&
            !domIsSvgAnimation(element))
        return;
    // A change to an animation element, or to an id or an href that targets and timing values read, finds them again.
    if (domIsSvgAnimation(element)) {
        requestScan(animations);
    } else if (element->tag == PrDomTag_Mpath && element->space == PrDomNamespace_Svg && domIsConnected(element)) {
        // The path that a motion follows changed, which a script reads at once.
        animations->scan = true;
        if (!animations->sampling)
            sample(animations);
    } else if ((name == "id" || name == "href") && animations->animations.count != 0) {
        animations->scan = true;
    }
    // An element whose class the animations give keeps it when its attribute changes.
    if (name == "class") {
        const SvgAnimatedValue* held = overlayHeld(animations->overlays[animations->front], element, "class");
        if (held != nullptr)
            domElementSetClasses(element, heldText(*held));
    }
    // The animations of the attribute read its own value again.
    for (uint32_t i = 0; i < animations->animations.count; ++i) {
        SvgTimed& t = animations->animations.data[i];
        if (t.target == element && (viewOf(t.attributeName) == name || (t.motion && name == "transform"))) {
            // The values that the animation composes over it change, so the animations sample again.
            t.baseRead = false;
            animations->unsampled = true;
        }
    }
}

void svgAnimationsElementDestroyed(SvgAnimations* animations, PrDomElement* element) {
    for (uint32_t i = 0; i < animations->containers.count; ++i) {
        if (animations->containers.data[i].root == element)
            animations->containers.data[i].root = nullptr;
    }
}

void svgAnimationsRemoving(SvgAnimations* animations, PrDomNode* node) {
    dropSubtree(animations, node);
}

void svgAnimationsPause(PrDomElement* svg, bool paused) {
    SvgAnimations* a = animationsFor(svg);
    if (a == nullptr)
        return;
    // Finding the container may add one, which moves the others.
    uint32_t container = containerOf(a, svg);
    SvgTimeContainer& c = a->containers.data[container];
    if (c.paused == paused)
        return;
    // A container that has not begun keeps the time it begins at, paused or not.
    if (paused && c.begun)
        c.held = (documentTime(a) - c.origin) / 1000;
    else if (c.begun)
        c.origin = documentTime(a) - c.held * 1000;
    c.paused = paused;
    a->unsampled = true;
}

bool svgAnimationsPaused(PrDomElement* svg) {
    SvgAnimations* a = animationsFor(svg);
    if (a == nullptr)
        return false;
    uint32_t container = containerOf(a, svg);
    return a->containers.data[container].paused;
}

double svgAnimationsTime(PrDomElement* svg) {
    SvgAnimations* a = animationsFor(svg);
    return a != nullptr ? containerTime(a, containerOf(a, svg)) : 0;
}

void svgAnimationsSeek(PrDomElement* svg, double seconds) {
    SvgAnimations* a = animationsFor(svg);
    if (a == nullptr || !prF64IsFinite(seconds))
        return;
    // Finding the container may add one, which moves the others.
    uint32_t container = containerOf(a, svg);
    SvgTimeContainer& c = a->containers.data[container];
    double time = prF64Max(seconds, 0);
    // A container that has not begun will begin at that time: its begin seeks there.
    if (!c.begun) {
        c.held = time;
        c.seeked = true;
        return;
    }
    if (c.paused)
        c.held = time;
    else
        c.origin = documentTime(a) - time * 1000;
    a->seek = true;
    sample(a);
}

PrDomElement* PR_NULLABLE svgAnimationTarget(PrDomElement* animation) {
    SvgAnimations* a = animationsFor(animation);
    SvgTimed* t = a != nullptr ? settledTimed(a, animation) : nullptr;
    return t != nullptr ? t->target : nullptr;
}

bool svgAnimationStartTime(PrDomElement* animation, double* out) {
    SvgAnimations* a = animationsFor(animation);
    SvgTimed* t = a != nullptr ? settledTimed(a, animation) : nullptr;
    if (t == nullptr || !prF64IsFinite(t->begin))
        return false;
    *out = t->begin;
    return true;
}

double svgAnimationTime(PrDomElement* animation) {
    SvgAnimations* a = animationsFor(animation);
    SvgTimed* t = a != nullptr ? settledTimed(a, animation) : nullptr;
    return t != nullptr ? containerTime(a, t->container) : 0;
}

bool svgAnimationSimpleDuration(PrDomElement* animation, double* out) {
    SvgAnimations* a = animationsFor(animation);
    SvgTimed* t = a != nullptr ? settledTimed(a, animation) : nullptr;
    if (t == nullptr || !prF64IsFinite(t->duration))
        return false;
    *out = t->duration;
    return true;
}

void svgAnimationAddTime(PrDomElement* animation, bool end, double offset) {
    SvgAnimations* a = animationsFor(animation);
    SvgTimed* t = a != nullptr ? settledTimed(a, animation) : nullptr;
    if (t == nullptr || !prF64IsFinite(offset))
        return;
    double time = containerTime(a, t->container);
    // Ending an element that is not active now has no effect (svg-animations/#__svg__SVGAnimationElement__endElement).
    if (end && offset <= 0 && !simpleTimeOf(*t, time).inside)
        return;
    addInstanceTime(a, t, end, time + offset);
    sample(a);
}

PrStringView svgAnimatedAttribute(const PrDomElement* element, PrStringView name) {
    const SvgAnimatedValue* held = animatedHeld(element, name);
    return held != nullptr ? heldText(*held) : PrStringView{nullptr, 0};
}

const SvgAnimateValue* PR_NULLABLE svgAnimatedValue(const PrDomElement* element, PrStringView name) {
    const SvgAnimatedValue* held = animatedHeld(element, name);
    return held != nullptr ? &held->value : nullptr;
}

} // namespace pr_impl
