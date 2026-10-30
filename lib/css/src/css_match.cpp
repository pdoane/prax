// CSS grammar matching and production checks.
//
// css-syntax-3 - https://drafts.csswg.org/css-syntax/
// css-values-5 - https://drafts.csswg.org/css-values-5/
// css-mixins-1 - https://drafts.csswg.org/css-mixins-1/

#include "css/src/css_match.h"

#include "pr/ascii.h"
#include "pr/color_space.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/search.h"
#include "pr/string_ops.h"

#include "css/src/css_color.h"
#include "css/src/css_color_value.h"
#include "css/src/css_parse_terminal.h"
#include "css/src/css_rule.h"

namespace pr_impl {

namespace {

// Returns the argument kind of `pseudo`, which determines what its selectors may contain according to its grammar.
CssMatchArgument argumentOf(CssPseudoClass pseudo) {
    return CssMatchArgument{pseudo, CssPseudoElement_None, false};
}

void holdFeatureValues(CssMatcher* m, uint32_t later, uint32_t root, const CssMatchEdit* run);
const CssMatchFrame& frameAt(const CssMatcher* m, uint32_t index);

// ---------------------------------------------------------------------------------------------------------------------
// The Matcher (css-values-4/#value-defs)
//
// One walk of the compiled grammar, adding what it matches to the pool. Matching uses continuation passing. A frame
// holds what remains to match after the node being tried, so a node that matched one way is retried another way when
// the rest does not fit.
//
// The loop below runs the frames, which live in the CssMatchState, so a deep match uses an array rather than the C
// stack.

// The maximum number of nodes one match may enter. Beyond it, the match fails.
constexpr uint32_t kMaxSteps = 1000000;

// The maximum number of frames one match may hold. Beyond it, the match fails.
constexpr uint32_t kMaxFrames = 1u << 16;

CssGrammarIndex childAt(CssGrammarIndex index, uint32_t ordinal) {
    CssGrammarIndex end = cssGrammarAfter(index);
    CssGrammarIndex child = CssGrammarIndex(index + 1);
    for (; ordinal != 0 && child < end; --ordinal)
        child = cssGrammarAfter(child);
    return child < end ? child : end;
}

uint32_t childCount(CssGrammarIndex index) {
    CssGrammarIndex end = cssGrammarAfter(index);
    uint32_t count = 0;
    for (CssGrammarIndex i = CssGrammarIndex(index + 1); i < end; i = cssGrammarAfter(i))
        ++count;
    return count;
}

// Returns true if `number` is within the bracketed bounds on a reference, in the type's canonical unit.
bool inRange(const CssGrammarRange& range, double number) {
    if ((range.flags & CssGrammarRangeFlag_MinUnbounded) == 0 && number < range.min)
        return false;
    if ((range.flags & CssGrammarRangeFlag_MaxUnbounded) == 0 && number > range.max)
        return false;
    return true;
}

bool isCommaNode(CssGrammarIndex index) {
    CssGrammarNode node = kCssGrammarNodes[index];
    return cssGrammarType(node) == CssGrammarType_Literal && cssGrammarValue(node) == ',';
}

// Returns true if the stream has read the whole input, apart from whitespace. The end of the input closes every block
// and function still open (css-syntax-3/#consume-simple-block, css-syntax-3/#consume-function).
bool atInputEnd(const CssTokenStream* ts) {
    if (ts->pos < ts->end)
        return false;
    for (uint32_t i = ts->end; i < cssTokenCount(ts->tokens); ++i) {
        if (cssTokenKind(ts->tokens, i) != CssTokenKind_Whitespace)
            return false;
    }
    return true;
}

bool isCloser(CssTokenKind kind) {
    return kind == CssTokenKind_CloseParen || kind == CssTokenKind_CloseBracket || kind == CssTokenKind_CloseBrace;
}

// Matches the token that a one-character literal names: a punctuation token of its own kind, or a delim for everything
// else. A closer that is missing because the input ended is matched at the position where it would be.
bool matchLiteral(CssMatcher* m, uint32_t character) {
    CssTokenKind kind = character < 0x80 ? kCssPunctuation.kindOf[character] : CssTokenKind_Delim;
    if (cssPeek(m->ts) != kind)
        return isCloser(kind) && atInputEnd(m->ts);
    if (kind == CssTokenKind_Delim && cssTokenDelim(m->ts->tokens, m->ts->pos) != character)
        return false;
    ++m->ts->pos;
    return true;
}

// Returns the first token at or after `pos` that is not whitespace.
uint32_t significant(const CssTokenStream* ts, uint32_t pos) {
    while (pos < ts->end && cssTokenKind(ts->tokens, pos) == CssTokenKind_Whitespace)
        ++pos;
    return pos;
}

CssTokenKind kindAt(const CssTokenStream* ts, uint32_t pos) {
    return pos < ts->end ? cssTokenKind(ts->tokens, pos) : CssTokenKind_Eof;
}

// A token in the form that a first set tests: the bit of its kind, and the bit of its literal character for a delim or
// a punctuation token.
struct Upcoming {
    uint32_t kind;
    uint64_t literal;
};

// Returns the kinds of token that a given value may be written as.
uint32_t givenKinds(const CssValueNode* value) {
    switch (value->kind) {
        case PrCssValueKind_Integer:
        case PrCssValueKind_Number:       return 1u << CssTokenKind_Number;
        case PrCssValueKind_Percentage:   return 1u << CssTokenKind_Percentage;
        case PrCssValueKind_Dimension:    return 1u << CssTokenKind_Dimension;
        case PrCssValueKind_Math:         return 1u << CssTokenKind_Function;
        case PrCssValueKind_String:       return 1u << CssTokenKind_String;
        case PrCssValueKind_Url:          return 1u << CssTokenKind_Url;
        case PrCssValueKind_UnicodeRange: return 1u << CssTokenKind_UnicodeRange;
        default:                        return ~0u;
    }
}

Upcoming upcomingAt(const CssTokenStream* ts, uint32_t pos) {
    CssTokenKind kind = kindAt(ts, pos);
    if (kind == CssTokenKind_Value)
        return Upcoming{givenKinds(cssTokenValue(ts->tokens, pos).values), 0};
    uint32_t character;
    if (kind == CssTokenKind_Delim)
        character = cssTokenDelim(ts->tokens, pos);
    else
        character = uint8_t(kCssPunctuation.byteOf[kind]);
    return Upcoming{1u << kind, character < 128 ? kCssGrammarLiteralBit[character] : 0};
}

// Returns true if a single match of the node at `index` could begin at `next`: its first set includes the token's kind
// or character, or the node can match nothing.
bool mayBegin(CssGrammarIndex index, Upcoming next) {
    const CssGrammarFirst& first = kCssGrammarFirsts[kCssGrammarFirstOf[index]];
    return (first.kinds & (kCssGrammarFirstEmpty | next.kind)) != 0 || (first.literals & next.literal) != 0;
}

// Returns true if the node under its multiplier could begin at `next`. This is mayBegin, except that it is also true if
// the multiplier may take the node zero times.
bool mayMatch(CssGrammarIndex index, Upcoming next) {
    return cssGrammarModifier(kCssGrammarNodes[index])->min == 0 || mayBegin(index, next);
}

// ---------------------------------------------------------------------------------------------------------------------
// The loop's moving parts

CssMatchFrame& frameAt(CssMatcher* m, uint32_t index) {
    return m->match->frames.data[index];
}

const CssMatchFrame& frameAt(const CssMatcher* m, uint32_t index) {
    return m->match->frames.data[index];
}

// Copies to `frame`, which is pushed to continue at `outer`, whether the frame at `outer` is inside a structural
// production, and which one.
void inherit(const CssMatcher* m, uint32_t outer, CssMatchFrame* frame) {
    const CssMatchFrame& from = frameAt(m, outer);
    frame->flags |= from.flags & CssMatchFlag_Structural;
    frame->production = from.production;
}

// Returns a frame of `kind` for the node at `index`, continuing at `outer`.
CssMatchFrame nodeFrame(const CssMatcher* m, CssMatchFrameKind kind, CssGrammarIndex index, uint32_t outer) {
    CssMatchFrame frame = {};
    frame.kind = kind;
    inherit(m, outer, &frame);
    frame.node = index;
    frame.outer = outer;
    return frame;
}

// Pushes a frame and makes it the frame that the loop resumes.
bool enterFrame(CssMatcher* m, CssMatchFrame frame) {
    if (m->match->frames.count == kMaxFrames)
        return false;
    frame.pos = m->ts->pos;
    m->frame = m->match->frames.count;
    m->match->frames.push(frame);
    m->action = CssMatchAction_Resume;
    return true;
}

// Returns true if the frame at `index`, and every frame above it, may change: no recorded choice depends on them.
bool settled(const CssMatcher* m, uint32_t index) {
    return m->floor <= index;
}

// Continues at `outer`. The frames above it are now dead ends. If no choice needs them, they are removed, so a match
// that does not backtrack keeps no more frames than a call stack would.
void continueAt(CssMatcher* m, uint32_t outer) {
    if (settled(m, outer + 1))
        m->match->frames.count = outer + 1;
    m->frame = outer;
    m->action = CssMatchAction_Resume;
}

// Replaces the frame at `index` with `frame` if nothing needs it unchanged. Otherwise pushes `frame` as a separate
// frame.
bool advanceFrame(CssMatcher* m, uint32_t index, CssMatchFrame frame) {
    if (!settled(m, index))
        return enterFrame(m, frame);
    frame.pos = m->ts->pos;
    m->match->frames.count = index + 1;
    frameAt(m, index) = frame;
    m->frame = index;
    m->action = CssMatchAction_Resume;
    return true;
}

// Records where to return, and which alternative to try then, if what the frame is about to try fails.
void pushChoice(CssMatcher* m, uint32_t frame, uint32_t state) {
    CssMatchChoice choice = {
        frame, state, m->match->frames.count, m->ts->pos, m->ts->end,
        m->pool->values.count, m->pool->text.count, m->match->edits.count,
    };
    m->match->choices.push(choice);
    m->floor = choice.frames;
}

// Drops the choices from `count` on. The match does not return to them.
void dropChoices(CssMatcher* m, uint32_t count) {
    m->match->choices.count = count;
    m->floor = count == m->choices ? 0 : m->match->choices.data[count - 1].frames;
}

// Undoes back to the last choice. Returns false if there is none, which fails the match.
bool takeChoice(CssMatcher* m) {
    if (m->match->choices.count == m->choices)
        return false;

    CssMatchChoice choice = m->match->choices.data[--m->match->choices.count];
    uint32_t below = m->match->choices.count;
    m->floor = below == m->choices ? 0 : m->match->choices.data[below - 1].frames;
    m->ts->pos = choice.pos;
    m->ts->end = choice.count;
    m->pool->values.count = choice.values;
    m->pool->text.count = choice.text;
    m->match->edits.count = choice.edits;
    m->match->frames.count = choice.frames;
    m->frame = choice.frame;
    m->state = choice.state;
    m->action = CssMatchAction_Resume;
    return true;
}

// Returns true if whitespace in a structural production may come before the token at the read point, for a leaf
// continuing at `index`. Between a function's parentheses it may, as it may anywhere in a value
// (css-values-4/#functional-notations). Otherwise, before a production's first component, the enclosing production
// decides, as the production's frame recorded on entry. After the first component, the production itself decides
// (selectors-4/#grammar).
bool spaceAllowed(const CssMatcher* m, uint32_t index) {
    uint32_t production = frameAt(m, index).production;
    for (uint32_t at = index; at != production; at = frameAt(m, at).outer) {
        CssMatchFrameKind kind = frameAt(m, at).kind;
        if (kind == CssMatchFrameKind_Function)
            return true;
        if (kind == CssMatchFrameKind_End)
            break;
    }
    const CssMatchFrame& f = frameAt(m, production);
    if (f.kind == CssMatchFrameKind_End)
        return true;
    if (f.pos == m->ts->pos)
        return (f.flags & CssMatchFlag_SpaceFirst) != 0;
    return !bitsetTest(kCssValueDefNoWhitespace, f.node);
}

// Returns true if whitespace may come before what the frame at `index` matches next.
bool spaceBefore(const CssMatcher* m, uint32_t index) {
    return (frameAt(m, index).flags & CssMatchFlag_Structural) == 0 || spaceAllowed(m, index);
}

// Skips the whitespace before a leaf continuing at `outer`, if whitespace is allowed there.
void skipSpace(CssMatcher* m, uint32_t outer) {
    if ((frameAt(m, outer).flags & CssMatchFlag_Structural) == 0 || spaceAllowed(m, outer))
        cssSkipWhitespace(m->ts);
}

// Returns the token that a node continuing at `outer` begins with, in the form that first sets test: the next token, or
// the next non-whitespace token if whitespace may come first.
Upcoming upcoming(const CssMatcher* m, uint32_t outer) {
    uint32_t pos = spaceBefore(m, outer) ? significant(m->ts, m->ts->pos) : m->ts->pos;
    return upcomingAt(m->ts, pos);
}

// Matches one node under its multiplier, then everything after it. A node taken once with no bounds needs no frame to
// count its repetitions. An optional node that could not begin here is taken zero times.
bool enterNode(CssMatcher* m, CssGrammarIndex index, uint32_t outer) {
    if (index == CssGrammarRoot_None)
        return false;
    CssGrammarShape shape = kCssGrammarShapes[index];
    if ((shape & CssGrammarShape_Once) != 0) {
        m->node = index;
        m->frame = outer;
        m->action = CssMatchAction_Once;
        return true;
    }
    if ((shape & CssGrammarShape_Optional) != 0 && !mayBegin(index, upcoming(m, outer))) {
        continueAt(m, outer);
        return true;
    }

    CssMatchFrame frame = {};
    frame.kind = CssMatchFrameKind_Repeat;
    inherit(m, outer, &frame);
    frame.node = index;
    frame.anchor = m->pool->values.count;
    frame.outer = outer;
    return enterFrame(m, frame);
}

// Matches a structural production, then everything after it. The production's node is added first, and its children are
// the values that its grammar matches.
bool enterProduction(CssMatcher* m, CssValueDef production, uint32_t outer) {
    CssValueNode value = {};
    value.kind = PrCssValueKind_Production;
    value.aux = production;
    value.tokens.start = m->ts->pos;

    CssMatchFrame frame = {};
    frame.kind = CssMatchFrameKind_Production;
    frame.flags = CssMatchFlag_Structural | (spaceBefore(m, outer) ? CssMatchFlag_SpaceFirst : 0);
    frame.node = production;
    frame.anchor = cssValuePoolAdd(m->pool, value);
    frame.outer = outer;
    frame.production = m->match->frames.count;
    if (!enterFrame(m, frame))
        return false;
    return enterNode(m, kCssValueDefGrammarRoot[production], m->frame);
}

// Matches a reference to a production, then everything after it. Inside a structural production, a production that
// keeps a node gets one.
bool enterType(CssMatcher* m, CssValueDef production, uint32_t outer) {
    if ((frameAt(m, outer).flags & CssMatchFlag_Structural) != 0 && bitsetTest(kCssValueDefKeepsNode, production))
        return enterProduction(m, production, outer);
    return enterNode(m, kCssValueDefGrammarRoot[production], outer);
}

bool checkProduction(CssMatcher* m, uint32_t index);
bool checkFunction(const CssMatcher* m, uint32_t at);

bool folds(const CssValueNode* values, uint32_t at);

void foldFunction(CssValuePool* pool, uint32_t at);

// ---------------------------------------------------------------------------------------------------------------------
// Canonical form (cssom-1/#serialize-a-css-value)
//
// A value is stored in canonical form:
// - the children of a || or && are in the order that its grammar writes them, which is the canonical order that webref
// records for a property as "per grammar";
// - components that the prose lets a value omit are omitted when they have their implied value (kCssGrammarOmissions);
// - a production whose prose defines its form is stored in that form (CssGrammarFlag_Folds).
//
// The matcher records in the match state the runs of values that it may change. It changes them once no choice can undo
// them: when the whole match is done, or when a function folds its arguments. The serializer writes a value as it is
// stored.

// Records the children that a || or && took. Its frames hold them from the last child taken, at `index`, down to the
// first.
void recordGroup(CssMatcher* m, uint32_t index) {
    const CssMatchFrame f = frameAt(m, index);
    Array<CssMatchEdit>* edits = &m->match->edits;
    uint32_t group = edits->count;
    edits->reserve(group + 1 + f.done);
    edits->count = group + 1 + f.done;

    uint32_t end = m->pool->values.count;
    for (uint32_t i = index, part = f.done; part != 0; --i) {
        const CssMatchFrame& taken = frameAt(m, i);
        if (taken.kind == CssMatchFrameKind_AnyOrder && taken.node == f.node && taken.outer == f.outer && taken.done == part) {
            edits->data[group + part] = CssMatchEdit{taken.anchor, end, taken.end, 0};
            end = taken.anchor;
            --part;
        }
    }
    edits->data[group] = CssMatchEdit{end, m->pool->values.count, f.node, f.done};
}

// Returns the entry in kCssGrammarOmissions for `node`, or null.
const CssGrammarOmission* PR_NULLABLE omissionOf(CssGrammarIndex node) {
    uint32_t at = lowerBound(kCssGrammarOmissions, kCssGrammarOmissionCount, node);
    return at < kCssGrammarOmissionCount && kCssGrammarOmissions[at].node == node ? &kCssGrammarOmissions[at] : nullptr;
}

// Returns true if a value is the one that `implied` gives.
bool isImpliedValue(const CssValueNode* value, const CssGrammarImpliedValue& implied) {
    switch (implied.kind) {
        case CssGrammarImpliedKind_Keyword:
            return cssValueIsKeyword(value, implied.keyword);
        case CssGrammarImpliedKind_Number:
            return (value->kind == PrCssValueKind_Integer || value->kind == PrCssValueKind_Number) &&
                value->number == implied.number;
        case CssGrammarImpliedKind_Percentage:
            return value->kind == PrCssValueKind_Percentage && value->number == implied.number;
        default:
            return (value->kind == PrCssValueKind_Dimension || value->kind == PrCssValueKind_Integer ||
                value->kind == PrCssValueKind_Number) && value->number == 0.0;
    }
}

// Returns true if the values in [start, end) are the implied value of an omitted component.
bool isImplied(const CssValueNode* values, uint32_t start, uint32_t end, const CssGrammarImplied& implied) {
    const CssGrammarImpliedValue* run = kCssGrammarImpliedValues + implied.first;
    if (implied.alternatives) {
        bool one = start != end && values[start].next == end - start;
        for (uint32_t i = 0; i < implied.count && one; ++i) {
            if (isImpliedValue(&values[start], run[i]))
                return true;
        }
        return false;
    }
    uint32_t i = 0;
    for (uint32_t at = start; at < end; at += values[at].next, ++i) {
        if (i == implied.count || !isImpliedValue(&values[at], run[i]))
            return false;
    }
    return i == implied.count && i != 0;
}

// Sorts the parts that follow the record of a || or && into the order of their nodes. Returns true if any part moved.
bool sortParts(CssMatchEdit* group) {
    CssMatchEdit* parts = group + 1;
    bool moved = false;
    for (uint32_t i = 1; i < group->parts; ++i) {
        CssMatchEdit part = parts[i];
        uint32_t j = i;
        for (; j != 0 && parts[j - 1].node > part.node; --j)
            parts[j] = parts[j - 1];
        parts[j] = part;
        moved = moved || j != i;
    }
    return moved;
}

// Rewrites a || or && over its values in canonical form: its parts in the order of their nodes, without the parts that
// a value omits. The first part of a required || is kept if the group would otherwise hold nothing. Returns the number
// of values that the group now holds.
uint32_t canonicalGroup(CssValuePool* pool, CssMatchEdit* group) {
    bool moved = sortParts(group);
    const CssMatchEdit* parts = group + 1;
    uint64_t omitted = 0;
    if ((kCssGrammarShapes[group->node] & CssGrammarShape_OmitsChild) != 0) {
        for (uint32_t i = 0; i < group->parts; ++i) {
            const CssGrammarOmission* omission = omissionOf(parts[i].node);
            if (omission != nullptr && isImplied(pool->values.data, parts[i].start, parts[i].end, omission->whole)) {
                omitted |= uint64_t(1) << i;
            }
        }
        bool required = cssGrammarModifier(kCssGrammarNodes[group->node])->min != 0;
        if (required && omitted == (uint64_t(2) << (group->parts - 1)) - 1)
            omitted &= ~uint64_t(1);
    }
    if (!moved && omitted == 0)
        return group->end - group->start;

    Array<CssValueNode>* values = &pool->values;
    values->reserve(values->count + (group->end - group->start));
    CssValueNode* scratch = values->data + values->count;
    uint32_t written = 0;
    for (uint32_t i = 0; i < group->parts; ++i) {
        if ((omitted & (uint64_t(1) << i)) != 0)
            continue;
        memCopy(scratch + written, values->data + parts[i].start,
            (parts[i].end - parts[i].start) * sizeof(CssValueNode));
        written += parts[i].end - parts[i].start;
    }
    memCopy(values->data + group->start, scratch, written * sizeof(CssValueNode));
    return written;
}

// Omits what a value may omit from the match of a node under its multiplier: all of it, if it has its implied value, or
// its trailing repetitions, as long as each is what would be implied. Each repetition is one value. Returns the number
// of values that the match now holds.
uint32_t canonicalRepeat(const CssValuePool* pool, const CssMatchEdit* run) {
    const CssValueNode* values = pool->values.data;
    const CssGrammarOmission* omission = omissionOf(run->node);
    if (omission->whole.count != 0)
        return isImplied(values, run->start, run->end, omission->whole) ? 0 : run->end - run->start;

    uint32_t starts[kCssGrammarOmissionMaxRepeats];
    uint32_t count = 0;
    for (uint32_t at = run->start; at < run->end; at += values[at].next)
        starts[count++] = at;
    for (; count > 1; --count) {
        const CssGrammarImplied& implied = omission->repetitions[count - 2];
        uint32_t last = starts[count - 1];
        uint32_t end = last + values[last].next;
        if (implied.copies == kCssGrammarImpliedOwn) {
            if (!isImplied(values, last, end, implied))
                break;
            continue;
        }
        uint32_t copied = starts[implied.copies];
        CssValueRun a = {values + last, end - last, pool->text.data};
        CssValueRun b = {values + copied, values[copied].next, pool->text.data};
        if (!cssValueRunEquals(a, b))
            break;
    }
    return starts[count - 1] + values[starts[count - 1]].next - run->start;
}

// Returns true if a one-component <position> is on the vertical or inline axis, which the two-component forms write
// second.
bool isSecondAxis(const CssValueNode* one) {
    if (one->kind != PrCssValueKind_Keyword)
        return false;
    switch (cssValueKeyword(one)) {
        case PrCssKeyword_Top:
        case PrCssKeyword_Bottom:
        case PrCssKeyword_YStart:
        case PrCssKeyword_YEnd:
        case PrCssKeyword_InlineStart:
        case PrCssKeyword_InlineEnd:
            return true;
        default:
            return false;
    }
}

// Resizes the run [start, end) of the tree whose first value is `root` to `length` values, by removing values from its
// end or adding room there. The values above it adjust their spans by the difference, and the edits from `later` on
// move with the values they refer to. A comma that an empty run leaves first or last among its siblings is also removed
// (css-values-4/#comb-comma).
void resizeRun(CssMatcher* m, uint32_t later, uint32_t root, uint32_t start, uint32_t end, uint32_t length) {
    Array<CssValueNode>* values = &m->pool->values;
    int32_t delta = int32_t(length) - int32_t(end - start);
    if (delta < 0) {
        values->erase(start + length, uint32_t(-delta));
    } else {
        values->reserve(values->count + uint32_t(delta));
        memMove(values->data + end + delta, values->data + end, (values->count - end) * sizeof(CssValueNode));
        values->count += uint32_t(delta);
    }

    uint32_t first = root;
    uint32_t last = values->count;
    for (uint32_t i = root; i < start;) {
        CssValueNode* value = &values->data[i];
        if (i + value->next <= start) {
            i += value->next;
            continue;
        }
        value->next = uint32_t(int32_t(value->next) + delta);
        first = i + 1;
        last = i + value->next;
        ++i;
    }

    Array<CssMatchEdit>* edits = &m->match->edits;
    for (uint32_t i = later; i < edits->count; ++i) {
        CssMatchEdit* edit = &edits->data[i];
        edit->start = edit->start >= end ? uint32_t(int32_t(edit->start) + delta) : edit->start;
        edit->end = edit->end >= end ? uint32_t(int32_t(edit->end) + delta) : edit->end;
    }

    if (length != 0)
        return;
    if (start < last && start == first && cssValueIsLiteral(&values->data[start], ','))
        resizeRun(m, later, root, start, start + 1, 0);
    else if (start == last && start > first && cssValueIsLiteral(&values->data[start - 1], ','))
        resizeRun(m, later, root, start - 1, start, 0);
}

// Folds the match of a production that a reference folds (CssGrammarFlag_Folds), which is one value, into the form that
// its prose defines.
void foldProduction(CssMatcher* m, uint32_t later, uint32_t root, const CssMatchEdit* run) {
    const CssValueNode* one = &m->pool->values.data[run->start];
    uint32_t size = run->end - run->start;
    if (one->next != size)
        return;
    switch (cssGrammarValue(kCssGrammarNodes[run->node])) {
        // css-values-4/#position-serialization: "If only one component is specified: The implied center keyword is
        // added, and a 2-component value is serialized."
        case CssValueDef_PositionOne: {
            bool second = isSecondAxis(one);
            resizeRun(m, later, root, run->start, run->end, size + 1);
            CssValueNode* at = m->pool->values.data + run->start;
            if (second)
                memMove(at + 1, at, size * sizeof(CssValueNode));
            at[second ? 0 : size] = cssKeywordValue(PrCssKeyword_Center);
            break;
        }
        // css-values-4/#ratios: "<ratio> is always serialized with both components", and the second "is optional,
        // defaulting to 1".
        case CssValueDef_Ratio: {
            resizeRun(m, later, root, run->start, run->end, size + 2);
            CssValueNode* at = m->pool->values.data + run->start;
            at[size] = cssLiteralValue('/');
            at[size + 1] = cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, 1.0);
            break;
        }
        case CssValueDef_MfPlain:
        case CssValueDef_MfRange:
            holdFeatureValues(m, later, root, run);
            break;
        default:
            break;
    }
}

// Applies the edits from `first` on, in the order recorded, to the tree whose first value is `root`, and drops them.
void applyEdits(CssMatcher* m, uint32_t first, uint32_t root) {
    Array<CssMatchEdit>* edits = &m->match->edits;
    for (uint32_t i = first; i < edits->count; i += edits->data[i].parts + 1) {
        CssMatchEdit edit = edits->data[i];
        uint32_t later = i + edit.parts + 1;
        if (edit.node == kCssMatchCalculate) {
            CssValueNode math[2];
            cssMathWrap(m->pool->values.data[edit.start], math);
            resizeRun(m, later, root, edit.start, edit.end, 2);
            m->pool->values.data[edit.start] = math[0];
            m->pool->values.data[edit.start + 1] = math[1];
            continue;
        }
        if ((cssGrammarModifier(kCssGrammarNodes[edit.node])->flags & CssGrammarFlag_Folds) != 0) {
            foldProduction(m, later, root, &edit);
            continue;
        }
        uint32_t held = edit.parts != 0 ? canonicalGroup(m->pool, &edits->data[i]) : canonicalRepeat(m->pool, &edit);
        if (held != edit.end - edit.start)
            resizeRun(m, later, root, edit.start + held, edit.end, 0);
    }
    edits->count = first;
}

// Returns the first of the edits from `first` on that is inside the function whose value is at `at`.
uint32_t editsInside(const CssMatcher* m, uint32_t first, uint32_t at) {
    const Array<CssMatchEdit>* edits = &m->match->edits;
    uint32_t i = first;
    while (i < edits->count && edits->data[i].start < at)
        i += edits->data[i].parts + 1;
    return i;
}

// ---------------------------------------------------------------------------------------------------------------------
// Resuming a frame
//
// A frame is read by value and written back before anything is pushed. Growing the array moves the elements, so a
// pointer into it does not survive a push.

// Returns true if the pool ends in a comma followed by the empty sequence that a free-form production passes when
// nothing follows the comma (css-values-5/#typedef-inherit-args). The empty sequence is present even though it consumed
// no tokens.
bool emptyAfterComma(const CssValuePool* pool) {
    uint32_t count = pool->values.count;
    return count >= 2 && cssValueIsLiteral(&pool->values.data[count - 2], ',') &&
        pool->values.data[count - 1].kind == PrCssValueKind_Tokens && pool->values.data[count - 1].text.length == 0;
}

// Resumes the children of a sequence, one at a time. Advancing pushes a frame rather than moving this one forward, so
// that backtracking into a child returns to the child after it.
bool resumeSiblings(CssMatcher* m, uint32_t index) {
    CssMatchFrame f = frameAt(m, index);

    // css-values-4/#component-types: a comma separates two items only if both are present.
    if (m->ts->pos > f.pos) {
        if ((f.flags & CssMatchFlag_CommaOmitted) != 0)
            return false;
        f.flags |= CssMatchFlag_Produced | CssMatchFlag_SinceComma;
    }

    if (f.node >= f.end) {
        CssMatchFlags taken = CssMatchFlag_CommaTaken | CssMatchFlag_SinceComma;
        if ((f.flags & taken) == CssMatchFlag_CommaTaken && !emptyAfterComma(m->pool))
            return false;
        if ((f.flags & CssMatchFlag_Required) != 0 && m->pool->values.count == f.anchor)
            return false;
        continueAt(m, f.outer);
        return true;
    }

    CssGrammarIndex child = f.node;
    CssMatchFrame next = f;
    next.node = cssGrammarAfter(child);

    // A comma at the end of the sequence separates the sequence from what follows, as in `[ <a> , ]? <b>#`. The comma
    // is written if the sequence produced something.
    if (isCommaNode(child)) {
        CssMatchFlags both = CssMatchFlag_Produced | CssMatchFlag_SinceComma;
        bool closing = next.node >= f.end;
        if ((f.flags & both) == both) {
            uint32_t mark = m->ts->pos;
            cssSkipWhitespace(m->ts);
            if (cssPeek(m->ts) == CssTokenKind_Comma) {
                ++m->ts->pos;
                cssValuePoolAdd(m->pool, cssLiteralValue(','));
                if (!closing) {
                    next.flags &= ~CssMatchFlag_SinceComma;
                    next.flags |= CssMatchFlag_CommaTaken;
                }
            } else if (closing) {
                return false;
            } else {
                m->ts->pos = mark;
                next.flags |= CssMatchFlag_CommaOmitted;
            }
        }
        return advanceFrame(m, index, next);
    }

    if (!advanceFrame(m, index, next))
        return false;
    return enterNode(m, child, m->frame);
}

// Resumes a repeated node. The match is greedy: it tries one more repetition before stopping at this count.
bool resumeRepeat(CssMatcher* m, uint32_t index, uint32_t state) {
    CssMatchFrame f = frameAt(m, index);
    const CssGrammarModifier* modifier = cssGrammarModifier(kCssGrammarNodes[f.node]);

    // The bracketed range bounds the value of each repetition, if the value is a number. If a given number, rather than
    // a written one, is outside its range, it is wrapped in a calculation, which is clamped when used
    // (css-typed-om-1/#create-an-internal-representation).
    if (state == 0 && modifier->range != CssGrammarRange_None && f.done != 0 && m->pool->values.count > f.taken) {
        const CssValueNode* last = &m->pool->values.data[f.taken];
        if (cssValueIsNumeric(last->kind) && !inRange(kCssGrammarRanges[modifier->range], last->number)) {
            bool given = m->ts->pos != 0 && cssTokenKind(m->ts->tokens, m->ts->pos - 1) == CssTokenKind_Value;
            if (!m->match->calculate || !given || m->pool->values.count != f.taken + 1)
                return false;
            m->match->edits.push(CssMatchEdit{uint32_t(f.taken), uint32_t(f.taken + 1), kCssMatchCalculate, 0});
        }
    }

    // A repetition that reads nothing must not repeat. A repetition that could not begin at the next token is not
    // tried. In a # list, that token comes after the comma that the list writes before every item but the first.
    bool progressed = f.done == 0 || m->ts->pos > f.pos;
    bool begins = false;
    if (state == 0 && progressed && f.done < modifier->max) {
        if (f.done != 0 && (modifier->flags & CssGrammarFlag_Comma) != 0) {
            uint32_t at = significant(m->ts, m->ts->pos);
            at = kindAt(m->ts, at) == CssTokenKind_Comma ? significant(m->ts, at + 1) : m->ts->end;
            begins = mayBegin(f.node, upcomingAt(m->ts, at));
        } else {
            begins = mayBegin(f.node, upcoming(m, index));
        }
    }
    if (begins) {
        // Stopping short of the minimum could not match. If the next token decides, stopping here could not match
        // either, so there is nothing to return to. A # list's comma decides every item except the first, which has no
        // comma before it.
        bool list = (modifier->flags & CssGrammarFlag_Comma) != 0;
        bool decided = (kCssGrammarShapes[f.node] & CssGrammarShape_Decided) != 0 && (f.done != 0 || !list);
        bool stops = f.done >= modifier->min && !decided;
        if (stops)
            pushChoice(m, index, 1);

        if (f.done != 0 && list) {
            cssSkipWhitespace(m->ts);
            if (cssPeek(m->ts) != CssTokenKind_Comma)
                return false;
            ++m->ts->pos;
            cssValuePoolAdd(m->pool, cssLiteralValue(','));
        }

        CssMatchFrame more = f;
        more.done = uint16_t(f.done + 1);
        more.taken = m->pool->values.count;
        if (!(stops ? enterFrame(m, more) : advanceFrame(m, index, more)))
            return false;

        // Exactly one item lies between two commas, so an item followed by another matches that run and no more. The
        // last item is left unbounded, because its extent depends on what follows the list.
        uint32_t comma;
        if ((modifier->flags & CssGrammarFlag_Delimited) != 0 && cssNextComma(m->ts, &comma)) {
            CssMatchFrame item = {};
            item.kind = CssMatchFrameKind_Item;
            inherit(m, m->frame, &item);
            item.anchor = m->ts->end;
            item.outer = m->frame;
            item.taken = m->match->choices.count;
            if (!enterFrame(m, item))
                return false;
            m->ts->end = comma;
        }

        m->node = f.node;
        m->action = CssMatchAction_Once;
        return true;
    }

    if (f.done < modifier->min)
        return false;
    if ((modifier->flags & CssGrammarFlag_Required) != 0 && m->pool->values.count == f.anchor)
        return false;
    if ((kCssGrammarShapes[f.node] & CssGrammarShape_Omissible) != 0 &&
            (modifier->max == 1 || f.done > 1) && m->pool->values.count != f.anchor) {
        m->match->edits.push(CssMatchEdit{f.anchor, m->pool->values.count, f.node, 0});
    }

    continueAt(m, f.outer);
    return true;
}

// Resumes the alternatives of a |, in the order that the specs write them, and skips the alternatives that could not
// begin at the next token.
bool resumeOneOf(CssMatcher* m, uint32_t index, uint32_t state) {
    CssMatchFrame f = frameAt(m, index);
    Upcoming next = upcoming(m, index);
    CssGrammarIndex end = cssGrammarAfter(f.node);
    CssGrammarIndex child = childAt(f.node, state);
    while (child < end && !mayMatch(child, next)) {
        child = cssGrammarAfter(child);
        ++state;
    }
    if (child >= end)
        return false;

    // If no later alternative could begin here either, there is nothing to return to.
    CssGrammarIndex later = cssGrammarAfter(child);
    uint32_t after = state + 1;
    while (later < end && !mayMatch(later, next)) {
        later = cssGrammarAfter(later);
        ++after;
    }
    if (later < end)
        pushChoice(m, index, after);
    return enterNode(m, child, f.outer);
}

// Resumes the children of a || or &&. Takes a child that is not yet taken before continuing. Each child taken pushes
// its own frame, which holds where the child's values start.
bool resumeAnyOrder(CssMatcher* m, uint32_t index, uint32_t state) {
    CssMatchFrame f = frameAt(m, index);
    uint32_t count = childCount(f.node);

    Upcoming next = upcoming(m, index);
    bool all = cssGrammarType(kCssGrammarNodes[f.node]) == CssGrammarType_AllOf;
    for (uint32_t slot = state; slot < count; ++slot) {
        if ((f.taken & (uint64_t(1) << slot)) != 0)
            continue;
        CssGrammarIndex child = childAt(f.node, slot);
        if (!mayMatch(child, next))
            continue;

        // Returning here is worthwhile if a later child could begin here, or if a || could end without this child.
        bool later = !all && f.done != 0;
        for (uint32_t other = slot + 1; other < count && !later; ++other) {
            later = (f.taken & (uint64_t(1) << other)) == 0 && mayMatch(childAt(f.node, other), next);
        }
        if (later)
            pushChoice(m, index, slot + 1);

        CssMatchFrame more = f;
        more.taken = f.taken | (uint64_t(1) << slot);
        more.done = uint16_t(f.done + 1);
        more.end = child;
        more.anchor = m->pool->values.count;
        if ((f.taken >> slot) != 0)
            more.flags |= CssMatchFlag_Unordered;
        if (!enterFrame(m, more))
            return false;
        return enterNode(m, child, m->frame);
    }

    if (all ? f.done != count : f.done == 0)
        return false;
    CssGrammarShape shape = kCssGrammarShapes[f.node];
    if (((f.flags & CssMatchFlag_Unordered) != 0 && (shape & CssGrammarShape_Written) == 0) ||
            (shape & CssGrammarShape_OmitsChild) != 0) {
        recordGroup(m, index);
    }
    continueAt(m, f.outer);
    return true;
}

bool resumeFrame(CssMatcher* m, uint32_t index, uint32_t state) {
    switch (frameAt(m, index).kind) {
        case CssMatchFrameKind_Siblings:
            return resumeSiblings(m, index);
        case CssMatchFrameKind_Repeat:
            return resumeRepeat(m, index, state);
        case CssMatchFrameKind_OneOf:
            return resumeOneOf(m, index, state);
        case CssMatchFrameKind_AnyOrder:
            return resumeAnyOrder(m, index, state);

        case CssMatchFrameKind_Item: {
            // The item's stream was narrowed to its own run, and it must have read all of it.
            CssMatchFrame f = frameAt(m, index);
            cssSkipWhitespace(m->ts);
            if (cssPeek(m->ts) != CssTokenKind_Eof)
                return false;
            m->ts->end = f.anchor;

            // Every way of matching the item ends at the same token, so everything it pushed is finished: both the
            // frames it left behind and the choices it could be retried from are dropped.
            dropChoices(m, uint32_t(f.taken));
            m->match->frames.count = index;
            continueAt(m, f.outer);
            return true;
        }

        case CssMatchFrameKind_Function: {
            CssMatchFrame f = frameAt(m, index);
            cssSkipWhitespace(m->ts);
            if (cssPeek(m->ts) == CssTokenKind_CloseParen)
                ++m->ts->pos;
            else if (!atInputEnd(m->ts))
                return false;
            m->pool->values.data[f.anchor].next = m->pool->values.count - f.anchor;
            if (!checkFunction(m, f.anchor))
                return false;

            // Every way of matching the arguments ends at this ')'. For a function that folds into its value, the
            // choices inside it are dropped, and the edits inside it are applied before it folds.
            if ((f.flags & CssMatchFlag_Structural) == 0 && folds(m->pool->values.data, f.anchor)) {
                dropChoices(m, uint32_t(f.taken));
                applyEdits(m, editsInside(m, m->edits, f.anchor), f.anchor);
                foldFunction(m->pool, f.anchor);
            }
            continueAt(m, f.outer);
            return true;
        }

        case CssMatchFrameKind_Fold: {
            CssMatchFrame f = frameAt(m, index);
            if (m->pool->values.count != f.anchor)
                m->match->edits.push(CssMatchEdit{f.anchor, m->pool->values.count, f.node, 0});
            continueAt(m, f.outer);
            return true;
        }

        case CssMatchFrameKind_Longhand: {
            CssMatchFrame f = frameAt(m, index);
            m->pool->values.data[f.anchor].next = m->pool->values.count - f.anchor;
            continueAt(m, f.outer);
            return true;
        }

        case CssMatchFrameKind_Production: {
            CssMatchFrame f = frameAt(m, index);
            if (!checkProduction(m, index))
                return false;
            CssValueNode* value = &m->pool->values.data[f.anchor];
            value->tokens.count = m->ts->pos - value->tokens.start;
            value->next = m->pool->values.count - f.anchor;
            continueAt(m, f.outer);
            return true;
        }

        default:
            // The End frame: the whole stream must have been read.
            cssSkipWhitespace(m->ts);
            if (cssPeek(m->ts) != CssTokenKind_Eof)
                return false;
            m->action = CssMatchAction_Done;
            return true;
    }
}

// Matches one leaf (a keyword, a literal, an at-keyword or a terminal) after any whitespace that the enclosing
// productions allow before it. A literal inside a structural production keeps a value.
bool matchLeaf(CssMatcher* m, CssGrammarIndex index, uint32_t outer) {
    CssGrammarNode node = kCssGrammarNodes[index];
    uint32_t value = cssGrammarValue(node);
    switch (cssGrammarType(node)) {
        case CssGrammarType_Keyword: {
            skipSpace(m, outer);
            if (cssPeek(m->ts) != CssTokenKind_Ident || cssTokenKeyword(m->ts->tokens, m->ts->pos) != value)
                return false;
            ++m->ts->pos;
            cssValuePoolAdd(m->pool, cssKeywordValue(PrCssKeyword(value)));
            return true;
        }

        // Every alternative is a keyword and no two are the same, so the ident names at most one of them, and there is
        // nothing to return to.
        case CssGrammarType_KeywordSet: {
            skipSpace(m, outer);
            if (cssPeek(m->ts) != CssTokenKind_Ident)
                return false;

            PrCssKeyword keyword = cssTokenKeyword(m->ts->tokens, m->ts->pos);
            CssGrammarIndex end = cssGrammarAfter(index);
            CssGrammarIndex child = CssGrammarIndex(index + 1);
            while (child < end && cssGrammarValue(kCssGrammarNodes[child]) != keyword)
                ++child;
            if (child == end)
                return false;
            ++m->ts->pos;
            cssValuePoolAdd(m->pool, cssKeywordValue(keyword));
            return true;
        }

        case CssGrammarType_Number:
            skipSpace(m, outer);
            return cssParseNumberLiteral(m, kCssGrammarNumbers[value]);

        case CssGrammarType_Literal:
            skipSpace(m, outer);
            if (!matchLiteral(m, value))
                return false;
            cssValuePoolAdd(m->pool, cssLiteralValue(char(value)));
            return true;

        case CssGrammarType_AtKeyword:
            skipSpace(m, outer);
            if (cssPeek(m->ts) != CssTokenKind_AtKeyword || cssTokenKeyword(m->ts->tokens, m->ts->pos) != value) {
                return false;
            }
            ++m->ts->pos;
            return true;

        case CssGrammarType_Terminal: {
            skipSpace(m, outer);
            uint32_t at = m->pool->values.count;
            if (!cssParseTerminal(m, CssTerminal(value)))
                return false;
            // A percentage written for a number (CssGrammarShape_Fraction) is stored as the number.
            CssValueNode* matched = &m->pool->values.data[at];
            if ((kCssGrammarShapes[index] & CssGrammarShape_Fraction) != 0 &&
                    matched->kind == PrCssValueKind_Percentage) {
                matched->kind = PrCssValueKind_Number;
                matched->number /= 100.0;
            }
            return true;
        }

        default:
            return false;
    }
}

// Matches one node, without its multiplier, then everything after it.
bool matchOnce(CssMatcher* m, CssGrammarIndex index, uint32_t outer) {
    if (++m->steps > kMaxSteps)
        return false;

    CssGrammarNode node = kCssGrammarNodes[index];
    uint32_t value = cssGrammarValue(node);

    switch (cssGrammarType(node)) {
        case CssGrammarType_Keyword:
        case CssGrammarType_KeywordSet:
        case CssGrammarType_Number:
        case CssGrammarType_Literal:
        case CssGrammarType_AtKeyword:
        case CssGrammarType_Terminal:
            if (!matchLeaf(m, index, outer))
                return false;
            break;

        case CssGrammarType_Type:
            if ((cssGrammarModifier(node)->flags & CssGrammarFlag_Folds) != 0) {
                CssMatchFrame frame = nodeFrame(m, CssMatchFrameKind_Fold, index, outer);
                frame.anchor = m->pool->values.count;
                if (!enterFrame(m, frame))
                    return false;
                outer = m->frame;
            }
            return enterType(m, CssValueDef(value), outer);

        case CssGrammarType_Property:
            return enterNode(m, kCssPropertyGrammarRoot[kCssPropertyAlias[value]], outer);

        case CssGrammarType_Function: {
            skipSpace(m, outer);
            if (cssPeek(m->ts) != CssTokenKind_Function || cssTokenKeyword(m->ts->tokens, m->ts->pos) != value) {
                return false;
            }
            ++m->ts->pos;

            CssValueNode matched = {};
            matched.kind = PrCssValueKind_Function;
            matched.aux = uint16_t(value);
            CssMatchFrame frame = nodeFrame(m, CssMatchFrameKind_Function, index, outer);
            frame.anchor = cssValuePoolAdd(m->pool, matched);
            frame.taken = m->match->choices.count;
            if (!enterFrame(m, frame))
                return false;
            return enterNode(m, CssGrammarIndex(index + 1), m->frame);
        }

        case CssGrammarType_OneOf: {
            // An alternative that is the only one that could begin here needs no frame to return to.
            Upcoming next = upcoming(m, outer);
            CssGrammarIndex end = cssGrammarAfter(index);
            CssGrammarIndex only = CssGrammarRoot_None;
            uint32_t viable = 0;
            for (CssGrammarIndex child = CssGrammarIndex(index + 1); child < end && viable < 2;
                    child = cssGrammarAfter(child)) {
                if (mayMatch(child, next)) {
                    only = child;
                    ++viable;
                }
            }
            if (viable == 0)
                return false;
            if (viable == 1)
                return enterNode(m, only, outer);
            return enterFrame(m, nodeFrame(m, CssMatchFrameKind_OneOf, index, outer));
        }

        case CssGrammarType_AnyOf:
        case CssGrammarType_AllOf:
            return enterFrame(m, nodeFrame(m, CssMatchFrameKind_AnyOrder, index, outer));

        case CssGrammarType_Longhand: {
            CssValueNode component = {};
            component.kind = PrCssValueKind_Longhand;
            component.aux = uint16_t(value);
            CssMatchFrame frame = nodeFrame(m, CssMatchFrameKind_Longhand, index, outer);
            frame.anchor = cssValuePoolAdd(m->pool, component);
            if (!enterFrame(m, frame))
                return false;
            return enterNode(m, CssGrammarIndex(index + 1), m->frame);
        }

        case CssGrammarType_Sequence: {
            // A run of leaves, each taken once, needs no frame to walk it.
            if ((kCssGrammarShapes[index] & CssGrammarShape_Leaves) != 0) {
                for (CssGrammarIndex child = CssGrammarIndex(index + 1); child < cssGrammarAfter(index);
                        child = cssGrammarAfter(child)) {
                    if (!matchLeaf(m, child, outer))
                        return false;
                }
                break;
            }
            CssMatchFrame frame = nodeFrame(m, CssMatchFrameKind_Siblings, CssGrammarIndex(index + 1), outer);
            frame.end = cssGrammarAfter(index);
            if ((cssGrammarModifier(node)->flags & CssGrammarFlag_Required) != 0) {
                frame.flags |= CssMatchFlag_Required;
                frame.anchor = m->pool->values.count;
            }
            return enterFrame(m, frame);
        }

        default:
            return false;
    }

    continueAt(m, outer);
    return true;
}

// Runs the match loop. Every step either advances the match or backtracks it to the last choice.
bool run(CssMatcher* m) {
    for (;;) {
        switch (m->action) {
            case CssMatchAction_Resume: {
                uint32_t state = m->state;
                m->state = 0;
                if (!resumeFrame(m, m->frame, state))
                    m->action = CssMatchAction_Fail;
                break;
            }

            case CssMatchAction_Once:
                if (!matchOnce(m, m->node, m->frame))
                    m->action = CssMatchAction_Fail;
                break;

            case CssMatchAction_Fail:
                if (!takeChoice(m))
                    return false;
                break;

            default:
                return true;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Structural Productions (selectors-4/#grammar)
//
// The requirements that the prose places on a production's match beyond its grammar. A check runs when the production
// has matched. Failing it fails that way of matching, and the matcher continues with the next.

// Returns the frame of the production that encloses the frame at `index`, or the End frame if none encloses it.
const CssMatchFrame& enclosing(const CssMatcher* m, uint32_t index) {
    for (;;) {
        const CssMatchFrame& f = frameAt(m, index);
        if (f.kind == CssMatchFrameKind_End || f.kind == CssMatchFrameKind_Production)
            return f;
        index = f.outer;
    }
}

bool isProduction(const CssMatchFrame& f, CssValueDef production) {
    return f.kind == CssMatchFrameKind_Production && f.node == production;
}

// Checks that the value of an <id-selector>'s <hash-token> is an identifier (selectors-4/#grammar).
bool checkId(const CssMatcher* m, const CssMatchFrame& f) {
    uint32_t hash = significant(m->ts, m->pool->values.data[f.anchor].tokens.start);
    return (cssTokenFlags(m->ts->tokens, hash) & CssTokenFlag_Id) != 0;
}

// Checks the namespace prefix of a simple selector. A prefix that no @namespace rule declares makes the selector
// invalid (selectors-4/#invalid). `*|` and `|` declare nothing.
bool checkNamespacePrefix(const CssMatcher* m, const CssMatchFrame& f) {
    const CssValueNode* prefix = &m->pool->values.data[f.anchor + 1];
    if (prefix->kind != PrCssValueKind_Ident)
        return true;

    PrStringView url;
    return m->context != nullptr && m->context->sheet != nullptr &&
        cssSheetNamespace(m->context->sheet, cssValuePoolText(m->pool, prefix), &url);
}

// Checks that a subclass selector is allowed here. After a pseudo-element, a compound holds only pseudo-classes, and so
// does the argument of a logical combination pseudo-class there (selectors-4/#pseudo-element-states).
bool checkSubclass(const CssMatcher* m) {
    return m->argument.after == CssPseudoElement_None;
}

// Checks a complex selector. Whitespace separates two compound units that no combinator joins, and is the descendant
// combinator there (selectors-4/#grammar). A pseudo-element may appear only in the last unit
// (selectors-4/#pseudo-element-attachment). An argument restricted to a compound, or to what may follow a
// pseudo-element, holds one unit.
bool checkComplex(const CssMatcher* m, const CssMatchFrame& f) {
    const CssValueNode* values = m->pool->values.data;
    bool joined = true;       // true if the next unit follows a combinator, or is the first
    bool pseudo = false;      // true if a unit before the next one holds a pseudo-element
    uint32_t units = 0;
    for (uint32_t i = f.anchor + 1; i < m->pool->values.count; i += values[i].next) {
        if (cssValueProduction(&values[i]) == CssValueDef_Combinator) {
            joined = true;
            continue;
        }
        if (pseudo)
            return false;
        if (!joined && cssTokenKind(m->ts->tokens, values[i].tokens.start) != CssTokenKind_Whitespace)
            return false;
        joined = false;
        ++units;

        uint32_t end = i + values[i].next;
        for (uint32_t j = i + 1; j < end; j += values[j].next)
            pseudo = pseudo || cssValueProduction(&values[j]) == CssValueDef_PseudoCompoundSelector;
    }
    return units == 1 || (m->argument.after == CssPseudoElement_None && !m->argument.compound);
}

// ---------------------------------------------------------------------------------------------------------------------
// Query and Rule Productions (mediaqueries-5/#mq-syntax, css-cascade-5/#layer-names)

// Returns the keyword that an ident's value names, compared ASCII case-insensitively, or PrCssKeyword_Unknown for any
// other value.
PrCssKeyword identKeyword(const CssValuePool* pool, const CssValueNode* value) {
    if (value->kind == PrCssValueKind_Ident)
        return cssKeywordFromText(cssValuePoolText(pool, value));
    return PrCssKeyword_Unknown;
}

// Checks that a <media-type> is not one of the keywords that join media queries in the grammar, and is not `layer`
// (mediaqueries-5/#mq-syntax).
bool checkMediaType(const CssMatcher* m, const CssMatchFrame& f) {
    switch (identKeyword(m->pool, &m->pool->values.data[f.anchor + 1])) {
        case PrCssKeyword_Only:
        case PrCssKeyword_Not:
        case PrCssKeyword_And:
        case PrCssKeyword_Or:
        case PrCssKeyword_Layer:
            return false;
        default:
            return true;
    }
}

// Checks an <mf-name>, which names a descriptor of the at-rule whose features the query tests. The ident stores that
// PrCssDescriptor in `aux`, or PrCssDescriptor_None for an unknown name. Inside an <mf-plain>, a range feature may be
// written with a min- or max- prefix (mediaqueries-5/#mq-min-max), which the ident's flags record. The name is part of
// the query either way, and the match stands.
bool checkFeatureName(CssMatcher* m, const CssMatchFrame& f) {
    static_assert(uint32_t(PrCssDescriptor_None) <= uint32_t(CssValueAuxFlag_Feature));
    CssValueNode* name = &m->pool->values.data[f.anchor + 1];
    PrStringView text = cssValuePoolText(m->pool, name);
    PrCssDescriptor descriptor = cssDescriptorFromText(m->features, text);
    CssValueAuxFlags prefix = 0;
    if (descriptor == PrCssDescriptor_None && isProduction(enclosing(m, f.outer), CssValueDef_MfPlain)) {
        if (asciiStartsWithFold(text, "min-"))
            prefix = CssValueAuxFlag_FeatureMin;
        else if (asciiStartsWithFold(text, "max-"))
            prefix = CssValueAuxFlag_FeatureMax;
        if (prefix != 0)
            descriptor = cssDescriptorFromText(m->features, stringSlice(text, 4, text.size - 4));
        if (descriptor != PrCssDescriptor_None && !bitsetTest(kCssDescriptorRange, descriptor))
            descriptor = PrCssDescriptor_None;
    }
    if (descriptor == PrCssDescriptor_None)
        name->aux = CssValueAuxFlags(PrCssDescriptor_None);
    else
        name->aux = CssValueAuxFlags(descriptor | prefix);
    return true;
}

// Returns the feature that an <mf-plain> or <mf-range> names, whose values are the pool's values from `first` to `end`.
// Writes the index of the name's ident to `name`.
PrCssDescriptor featureOf(const CssValuePool* pool, uint32_t first, uint32_t end, uint32_t* name) {
    *name = 0;
    for (uint32_t i = first; i < end; i += pool->values.data[i].next) {
        if (cssValueIsProduction(&pool->values.data[i], CssValueDef_MfName))
            *name = i + 1;
    }
    return cssValueFeature(&pool->values.data[*name]);
}

// Checks an <mf-plain> or <mf-range>. Its values must be values that its feature accepts, and a range must use a
// feature that queries compare (mediaqueries-5/#mq-range-context). If not, the name's ident gets PrCssDescriptor_None,
// and the match stands unchanged.
bool checkFeatureValues(const CssMatcher* m, const CssMatchFrame& f, bool range) {
    CssValuePool* pool = m->pool;
    uint32_t end = pool->values.count;
    uint32_t text = pool->text.count;
    uint32_t name;
    PrCssDescriptor descriptor = featureOf(pool, f.anchor + 1, end, &name);
    if (descriptor == PrCssDescriptor_None)
        return true;
    if (range && !bitsetTest(kCssDescriptorRange, descriptor)) {
        pool->values.data[name].aux = PrCssDescriptor_None;
        return true;
    }
    for (uint32_t i = f.anchor + 1; i < end; i += pool->values.data[i].next) {
        CssValueNode value = pool->values.data[i];
        if (!cssValueIsProduction(&value, CssValueDef_MfValue))
            continue;
        CssTokenStream tokens = cssSubStream(m->ts, CssTokenSpan{value.tokens.start, value.tokens.count});
        CssValueSpan matched;
        bool takes = cssParseDescriptorValue(descriptor, &tokens, pool, m->match, &matched);
        pool->values.count = end;
        pool->text.count = text;
        if (!takes) {
            pool->values.data[name].aux = PrCssDescriptor_None;
            return true;
        }
    }
    return true;
}

// Folds an <mf-plain> or <mf-range> whose feature checkFeatureValues found. Each <mf-value> holds its value as the
// feature's grammar matches it (mediaqueries-5/#mq-features). The production is rewritten after the pool's end and
// moved over the run.
void holdFeatureValues(CssMatcher* m, uint32_t later, uint32_t root, const CssMatchEdit* run) {
    CssValuePool* pool = m->pool;
    uint32_t name;
    PrCssDescriptor descriptor = featureOf(pool, run->start + 1, run->end, &name);
    if (descriptor == PrCssDescriptor_None)
        return;
    uint32_t built = pool->values.count;
    CssValueNode production = pool->values.data[run->start];
    pool->values.push(production);
    for (uint32_t i = run->start + 1; i < run->end; i += pool->values.data[i].next) {
        CssValueNode value = pool->values.data[i];
        if (!cssValueIsProduction(&value, CssValueDef_MfValue)) {
            for (uint32_t j = i; j < i + value.next; ++j) {
                CssValueNode copy = pool->values.data[j];
                pool->values.push(copy);
            }
            continue;
        }
        uint32_t node = pool->values.count;
        pool->values.push(value);
        CssTokenStream tokens = cssSubStream(m->ts, CssTokenSpan{value.tokens.start, value.tokens.count});
        CssValueSpan matched;
        if (!cssParseDescriptorValue(descriptor, &tokens, pool, m->match, &matched)) {
            pool->values.count = built;
            return;
        }
        pool->values.data[node].next = pool->values.count - node;
    }
    uint32_t length = pool->values.count - built;
    pool->values.data[built].next = length;
    int32_t delta = int32_t(length) - int32_t(run->end - run->start);
    resizeRun(m, later, root, run->start, run->end, length);
    built = uint32_t(int32_t(built) + delta);
    memCopy(pool->values.data + run->start, pool->values.data + built, length * sizeof(CssValueNode));
    pool->values.count = built;
}

// Checks that no ident of a layer name is a CSS-wide keyword (css-cascade-5/#layer-names).
bool checkLayerName(const CssMatcher* m, const CssMatchFrame& f) {
    const CssValueNode* values = m->pool->values.data;
    for (uint32_t i = f.anchor + 1; i < m->pool->values.count; i += values[i].next) {
        if (cssIsCssWideKeyword(identKeyword(m->pool, &values[i])))
            return false;
    }
    return true;
}

// Checks that a <keyframes-name> is not the <custom-ident> `none`, and not an empty string
// (css-animations-1/#keyframes).
bool checkKeyframesName(const CssMatcher* m, const CssMatchFrame& f) {
    const CssValueNode* name = &m->pool->values.data[f.anchor + 1];
    if (name->kind == PrCssValueKind_String)
        return name->text.length != 0;
    return identKeyword(m->pool, name) != PrCssKeyword_None;
}

// Checks that a <container-name> is not `none`, `and`, `not` or `or` (css-conditional-5/#container-rule).
bool checkContainerName(const CssMatcher* m, const CssMatchFrame& f) {
    switch (identKeyword(m->pool, &m->pool->values.data[f.anchor + 1])) {
        case PrCssKeyword_None:
        case PrCssKeyword_And:
        case PrCssKeyword_Not:
        case PrCssKeyword_Or:
            return false;
        default:
            return true;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Pseudo-class Arguments (selectors-4/#pseudo-classes)
//
// The arguments that the specs define in prose. Each adds what it matched to the pool after the pseudo-class's name.

// Returns true if a match is inside a <supports-selector-fn>, in its own frames or in the frames of the match whose
// check started it. A selector there is valid only if every part of it is supported: nothing that a forgiving list
// omits, and no unknown -webkit- pseudo-element (css-conditional-4/#dfn-support-selector).
bool insideSupportsSelector(const CssMatcher* m) {
    for (const CssMatcher* at = m; at != nullptr; at = at->parent) {
        for (uint32_t index = at->frame;; index = frameAt(at, index).outer) {
            const CssMatchFrame& f = frameAt(at, index);
            if (f.kind == CssMatchFrameKind_End)
                break;
            if (f.kind == CssMatchFrameKind_Production && f.node == CssValueDef_SupportsSelectorFn)
                return true;
        }
    }
    return false;
}

// Parses the argument of :is() or :where(), a <forgiving-selector-list>. Each item should be a <complex-real-selector>.
// An item that is not is kept as tokens, as written, and matches nothing (selectors-4/#forgiving-selector, as
// csswg-drafts#8356 resolved). Inside a <supports-selector-fn>, such an item fails the list. A ',' is kept between two
// items.
bool parseForgiving(const CssMatcher* m, CssTokenSpan span, CssMatchArgument argument) {
    bool whole = insideSupportsSelector(m);
    CssTokenStream ts = cssSubStream(m->ts, span);
    for (;;) {
        uint32_t comma = ts.end;
        bool more = cssNextComma(&ts, &comma);
        CssTokenSpan item = {ts.pos, comma - ts.pos};
        if (!cssMatchArgument(m, item, CssGrammarRoot_None, CssValueDef_ComplexRealSelector, argument)) {
            if (whole)
                return false;
            cssValuePoolAddTokens(m->pool, m->ts, cssTrimWhitespace(m->ts->tokens, item));
        }
        if (!more)
            return true;
        cssValuePoolAdd(m->pool, cssLiteralValue(','));
        ts.pos = comma + 1;
    }
}

// Returns true if a match is inside the argument of a :has(), where a nested :has() is not valid
// (selectors-4/#relational).
bool insideHas(const CssMatcher* m) {
    for (const CssMatcher* at = m; at != nullptr; at = at->parent) {
        if (at->argument.of == CssPseudoClass_HasFn)
            return true;
    }
    return false;
}

// Parses <an+b>, followed by [of <complex-real-selector-list>]? if `takesOf` is set
// (selectors-4/#the-nth-child-pseudo).
bool parseNth(const CssMatcher* m, CssPseudoClass pseudo, CssTokenSpan span, bool takesOf) {
    CssTokenStream ts = cssSubStream(m->ts, span);
    cssSkipWhitespace(&ts);
    CssMatcher argument = cssMatchNested(m, &ts);
    if (!cssParseANPlusB(&argument))
        return false;

    cssSkipWhitespace(&ts);
    if (ts.pos == ts.end)
        return true;
    if (!takesOf || cssPeek(&ts) != CssTokenKind_Ident || cssTokenKeyword(ts.tokens, ts.pos) != PrCssKeyword_Of)
        return false;
    ++ts.pos;
    cssValuePoolAdd(m->pool, cssKeywordValue(PrCssKeyword_Of));
    return cssMatchArgument(m, CssTokenSpan{ts.pos, ts.end - ts.pos}, CssGrammarRoot_None,
        CssValueDef_ComplexRealSelectorList, argumentOf(pseudo));
}

// Parses the argument of :lang(): a list of language ranges, each an <ident> or a <string>
// (selectors-4/#the-lang-pseudo), with a ',' kept between two ranges.
bool parseLang(const CssMatcher* m, CssTokenSpan span) {
    CssTokenStream ts = cssSubStream(m->ts, span);
    CssMatcher argument = cssMatchNested(m, &ts);
    for (;;) {
        cssSkipWhitespace(&ts);
        if (!cssParseIdentToken(&argument) && !cssParseStringToken(&argument)) {
            return false;
        }
        cssSkipWhitespace(&ts);
        if (ts.pos == ts.end)
            return true;
        if (cssPeek(&ts) != CssTokenKind_Comma)
            return false;
        ++ts.pos;
        cssValuePoolAdd(m->pool, cssLiteralValue(','));
    }
}

// Parses the argument of :active-view-transition-type(): <custom-ident>#
// (css-view-transitions-2/#active-view-transition-type-pseudo), with a ',' kept between two idents.
bool parseActiveViewTransitionTypeArgument(const CssMatcher* m, CssTokenSpan span) {
    CssTokenStream ts = cssSubStream(m->ts, span);
    CssMatcher items = cssMatchNested(m, &ts);
    for (;;) {
        cssSkipWhitespace(&ts);
        if (!cssParseCustomIdent(&items))
            return false;
        cssSkipWhitespace(&ts);
        if (ts.pos == ts.end)
            return true;
        if (cssPeek(&ts) != CssTokenKind_Comma)
            return false;
        ++ts.pos;
        cssValuePoolAdd(m->pool, cssLiteralValue(','));
    }
}

// Parses the argument of a functional pseudo-class: by its grammar if the spec writes one, and by its prose otherwise.
// A logical combination pseudo-class passes to its argument whatever its own position allows
// (selectors-4/#logical-combination): what may follow `after`, and a compound if the enclosing argument requires one.
bool parseClassArgument(const CssMatcher* m, CssPseudoClass pseudo, CssTokenSpan span, CssPseudoElement after) {
    CssGrammarIndex root = kCssPseudoClassGrammarRoot[pseudo];
    if (root != CssGrammarRoot_None)
        return cssMatchArgument(m, span, root, CssValueDef_Count, argumentOf(pseudo));

    CssMatchArgument logical = {pseudo, after, m->argument.compound};
    switch (pseudo) {
        case CssPseudoClass_IsFn:
        case CssPseudoClass_WhereFn:
            return parseForgiving(m, span, logical);
        case CssPseudoClass_NotFn:
            return cssMatchArgument(m, span, CssGrammarRoot_None, CssValueDef_ComplexRealSelectorList, logical);
        case CssPseudoClass_HasFn:
            return !insideHas(m) && cssMatchArgument(m, span, CssGrammarRoot_None,
                CssValueDef_RelativeRealSelectorList, argumentOf(pseudo));
        case CssPseudoClass_NthChildFn:
        case CssPseudoClass_NthLastChildFn:
            return parseNth(m, pseudo, span, true);
        case CssPseudoClass_NthOfTypeFn:
        case CssPseudoClass_NthLastOfTypeFn:
        case CssPseudoClass_NthColFn:
        case CssPseudoClass_NthLastColFn:
            return parseNth(m, pseudo, span, false);
        case CssPseudoClass_DirFn:
            // selectors-4/#the-dir-pseudo: any identifier, of which only ltr and rtl match.
            return cssMatchArgument(m, span, kCssValueDefGrammarRoot[CssValueDef_Ident],
                CssValueDef_Count, argumentOf(pseudo));
        case CssPseudoClass_LangFn:
            return parseLang(m, span);
        case CssPseudoClass_HostFn:
        case CssPseudoClass_HostContextFn:
            return cssMatchArgument(m, span, CssGrammarRoot_None, CssValueDef_CompoundSelector,
                {pseudo, CssPseudoElement_None, true});
        case CssPseudoClass_ActiveViewTransitionTypeFn:
            return parseActiveViewTransitionTypeArgument(m, span);
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Pseudo-element Arguments (selectors-4/#pseudo-elements)

// Parses the argument of ::view-transition-group(), ::view-transition-group-children(), ::view-transition-image-pair(),
// ::view-transition-new() or ::view-transition-old(), which is a <pt-name-and-class-selector>
// (css-view-transitions-2/#pseudo-element-class-additions).
bool parseViewTransitionArgument(const CssMatcher* m, CssTokenSpan span) {
    return cssMatchArgument(m, span, CssGrammarRoot_None, CssValueDef_PtNameAndClassSelector,
        argumentOf(CssPseudoClass_None));
}

// Parses the argument of a functional pseudo-element: by its grammar if the spec writes one, and by its prose
// otherwise.
bool parseElementArgument(const CssMatcher* m, CssPseudoElement pseudo, CssTokenSpan span) {
    CssGrammarIndex root = kCssPseudoElementGrammarRoot[pseudo];
    if (root != CssGrammarRoot_None)
        return cssMatchArgument(m, span, root, CssValueDef_Count, argumentOf(CssPseudoClass_None));

    switch (pseudo) {
        case CssPseudoElement_SlottedFn:
            return cssMatchArgument(m, span, CssGrammarRoot_None, CssValueDef_CompoundSelector,
                {CssPseudoClass_None, CssPseudoElement_None, true});
        case CssPseudoElement_ViewTransitionGroupFn:
        case CssPseudoElement_ViewTransitionGroupChildrenFn:
        case CssPseudoElement_ViewTransitionImagePairFn:
        case CssPseudoElement_ViewTransitionNewFn:
        case CssPseudoElement_ViewTransitionOldFn:
            return parseViewTransitionArgument(m, span);
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Pseudo-classes and Pseudo-elements

// Returns true if `name` is an unknown -webkit- pseudo-element. Any other pseudo-element named with a -webkit- prefix,
// written without parentheses, is valid and matches nothing (selectors-4/#unknown--webkit--pseudo-elements).
bool isUnknownWebkit(PrStringView name, bool function) {
    return !function && name.size > 8 && asciiStartsWithFold(name, "-webkit-");
}

// Returns the pseudo-element that the pseudo-compound whose frame is `compound` follows in its unit, or
// CssPseudoElement_None if it follows the unit's compound (selectors-4/#sub-pseudo-elements).
CssPseudoElement pseudoElementBefore(const CssMatcher* m, const CssMatchFrame& compound) {
    const CssMatchFrame& unit = enclosing(m, compound.outer);
    const CssValueNode* values = m->pool->values.data;
    CssPseudoElement before = CssPseudoElement_None;
    for (uint32_t i = unit.anchor + 1; i < compound.anchor; i += values[i].next) {
        if (cssValueIsProduction(&values[i], CssValueDef_PseudoCompoundSelector))
            before = cssMatchedPseudoElement(m->pool, i + 1);
    }
    return before;
}

// Checks a <pseudo-class-selector>, which inside a <pseudo-element-selector> names a pseudo-element. The name must
// exist (selectors-4/#invalid), must be allowed after the preceding pseudo-element (selectors-4/#pseudo-element-states,
// selectors-4/#sub-pseudo-elements), and a function's argument must satisfy its grammar or prose. The name's value
// stores the pseudo it names in `aux`. The argument replaces the tokens that <any-value> took between the '(' and a
// ')'.
bool checkPseudo(CssMatcher* m, uint32_t index) {
    CssMatchFrame f = frameAt(m, index);
    CssMatchFrame outer = enclosing(m, f.outer);
    bool element = isProduction(outer, CssValueDef_PseudoElementSelector);

    uint32_t nameAt = f.anchor + 2;       // index after the node and its ':'
    uint32_t nameToken = significant(m->ts, f.pos) + 1;
    bool function = cssTokenKind(m->ts->tokens, nameToken) == CssTokenKind_Function;
    PrStringView name = cssValuePoolText(m->pool, &m->pool->values.data[nameAt]);

    uint32_t pseudo;
    CssPseudoElement after;
    if (element) {
        CssPseudoElement found = cssPseudoElement(name, function);
        if (found == CssPseudoElement_None && isUnknownWebkit(name, function) && !insideSupportsSelector(m))
            found = CssPseudoElement_UnknownWebkit;
        after = pseudoElementBefore(m, enclosing(m, outer.outer));
        if (found == CssPseudoElement_None || !cssPseudoElementFollows(after, found))
            return false;
        pseudo = found;
    } else {
        CssPseudoClass found = cssPseudoClass(name, function);
        if (found >= CssPseudoClass_FirstPrivate && !m->match->userAgent)
            found = CssPseudoClass_None;
        if (isProduction(outer, CssValueDef_PseudoCompoundSelector))
            after = cssMatchedPseudoElement(m->pool, outer.anchor + 1);
        else
            after = m->argument.after;
        if (found == CssPseudoClass_None || !cssPseudoClassFollows(after, found))
            return false;
        pseudo = found;
    }
    m->pool->values.data[nameAt].aux = uint16_t(pseudo);
    if (!function)
        return true;

    // The argument replaces what <any-value> took after the name and its '(', and the ')' after it.
    uint32_t argumentAt = nameAt + 2;
    if (m->pool->values.count > argumentAt && m->pool->values.data[argumentAt].kind == PrCssValueKind_Tokens)
        m->pool->text.count = m->pool->values.data[argumentAt].text.start;
    m->pool->values.count = argumentAt;

    // The argument runs to the ')', or to the end of the input if that closed the function.
    CssTokenStream argument = {m->ts->tokens, nameToken + 1, m->ts->pos};
    uint32_t close = cssBlockEnd(&argument);
    CssTokenSpan span = {nameToken + 1, close - nameToken - 1};
    bool parsed;
    if (element)
        parsed = parseElementArgument(m, CssPseudoElement(pseudo), span);
    else
        parsed = parseClassArgument(m, CssPseudoClass(pseudo), span, after);
    if (parsed)
        cssValuePoolAdd(m->pool, cssLiteralValue(')'));
    return parsed;
}

bool checkProduction(CssMatcher* m, uint32_t index) {
    const CssMatchFrame& f = frameAt(m, index);
    switch (CssValueDef(f.node)) {
        case CssValueDef_MediaType:
            return checkMediaType(m, f);
        case CssValueDef_MfName:
            return checkFeatureName(m, f);
        case CssValueDef_MfPlain:
            return checkFeatureValues(m, f, false);
        case CssValueDef_MfRange:
            return checkFeatureValues(m, f, true);
        case CssValueDef_LayerName:
            return checkLayerName(m, f);
        case CssValueDef_KeyframesName:
            return checkKeyframesName(m, f);
        case CssValueDef_ContainerName:
            return checkContainerName(m, f);
        case CssValueDef_IdSelector:
            return checkId(m, f) && checkSubclass(m);
        case CssValueDef_TypeSelector:
        case CssValueDef_ClassSelector:
        case CssValueDef_AttributeSelector:
            return checkSubclass(m);
        case CssValueDef_NsPrefix:
            return checkNamespacePrefix(m, f);
        case CssValueDef_ComplexSelector:
        case CssValueDef_ComplexRealSelector:
            return checkComplex(m, f);
        case CssValueDef_PseudoClassSelector:
            return checkPseudo(m, index);
        default:
            return true;
    }
}

// Checks the requirements that the prose places on the arguments of the function at `at` beyond its grammar. symbols()
// with an alphabetic or a numeric system needs at least two symbols (css-counter-styles-3/#symbols-function).
bool checkFunction(const CssMatcher* m, uint32_t at) {
    const CssValueNode* function = &m->pool->values.data[at];
    if (!cssValueIsFunction(function, PrCssKeyword_Symbols))
        return true;
    uint32_t symbols = 0;
    bool twoNeeded = false;
    for (uint32_t i = at + 1; i < at + function->next; i += m->pool->values.data[i].next) {
        const CssValueNode* argument = &m->pool->values.data[i];
        if (argument->kind == PrCssValueKind_Keyword)
            twoNeeded = twoNeeded || cssValueKeyword(argument) == PrCssKeyword_Alphabetic ||
                cssValueKeyword(argument) == PrCssKeyword_Numeric;
        else if (argument->kind != PrCssValueKind_Literal)
            ++symbols;
    }
    return !twoNeeded || symbols >= 2;
}

// ---------------------------------------------------------------------------------------------------------------------
// Color Functions (css-color-4/#color-syntax, css-color-5/#relative-colors, css-color-5/#color-mix)
//
// A color function's match folds into the value it represents. A function written with its components becomes a color
// holding them in its color space. A relative color keeps its arguments. color-mix() and palette-mix() keep theirs, in
// the canonical order of css-color-5/#serial-color-mix.

// Folds the arguments of the color function at `at`, from `first` on, into a color in `space`: its channels as written,
// and an alpha of 1 if none is written.
void foldColor(CssValuePool* pool, uint32_t at, uint32_t first, PrColorSpace space, CssValueAuxFlags form) {
    uint32_t end = pool->values.count;
    uint32_t write = at + 1;
    uint32_t channels = 0;
    for (uint32_t read = first; read < end;) {
        CssValueNode* value = &pool->values.data[read];
        uint32_t size = value->next;
        if (value->kind != PrCssValueKind_Literal) {
            memMove(pool->values.data + write, value, size * sizeof(CssValueNode));
            write += size;
            ++channels;
        }
        read += size;
    }
    pool->values.count = write;
    if (channels == 3)
        cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, 1.0));

    CssValueNode* color = &pool->values.data[at];
    color->kind = PrCssValueKind_Color;
    color->aux = CssValueAuxFlags(space | form);
    color->next = pool->values.count - at;
}

// Folds the relative color at `at`. The color keeps its arguments, under the function that
// css-color-5/#serial-relative-color writes: rgb() for rgba(), hsl() for hsla(), and xyz-d65 for color()'s xyz. An
// origin color written with rgb(), hsl() or hwb() is converted to sRGB, whose missing components are zero
// (css-color-4/#missing), so that it serializes in the legacy form.
void foldRelativeColor(CssValuePool* pool, uint32_t at) {
    CssValueNode* origin = &pool->values.data[at + 2];
    PrColorSpace originSpace = cssValueColorSpace(origin);
    bool written = (originSpace == PrColorSpace_Srgb && cssValueColorLegacy(origin)) ||
        originSpace == PrColorSpace_Hsl || originSpace == PrColorSpace_Hwb;
    if (origin->kind == PrCssValueKind_Color && written) {
        for (uint32_t i = at + 3; i < at + 2 + origin->next; i += pool->values.data[i].next) {
            if (cssValueIsKeyword(&pool->values.data[i], PrCssKeyword_None))
                pool->values.data[i] = cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, 0);
        }
    }
    CssValueNode* function = &pool->values.data[at];
    if (cssValueKeyword(function) == PrCssKeyword_Rgba) {
        function->aux = PrCssKeyword_Rgb;
    } else if (cssValueKeyword(function) == PrCssKeyword_Hsla) {
        function->aux = PrCssKeyword_Hsl;
    } else if (cssValueKeyword(function) == PrCssKeyword_Color) {
        CssValueNode* space = function + 2 + function[2].next;
        if (cssValueIsKeyword(space, PrCssKeyword_Xyz))
            space->aux = PrCssKeyword_XyzD65;
    }
}

// Folds the <color-interpolation-method> whose `in` is at `at` into the form that css-color-5/#serial-color-mix writes:
// xyz becomes xyz-d65, and a hue interpolation method of shorter is removed. Returns its length in values, and writes
// the color space that it names to `space`.
uint32_t foldInterpolationMethod(Array<CssValueNode>* values, uint32_t at, PrCssKeyword* space) {
    CssValueNode* named = &values->data[at + 1];
    if (cssValueIsKeyword(named, PrCssKeyword_Xyz))
        named->aux = PrCssKeyword_XyzD65;
    *space = cssValueKeyword(named);
    if (at + 3 >= values->count || !cssValueIsKeyword(&values->data[at + 3], PrCssKeyword_Hue))
        return 2;
    if (!cssValueIsKeyword(&values->data[at + 2], PrCssKeyword_Shorter))
        return 4;
    values->erase(at + 2, 2);
    return 2;
}

// Returns true if the mix item whose first value is at `index` writes a second value, its percentage.
bool mixHasSecond(const Array<CssValueNode>* values, uint32_t index) {
    uint32_t second = index + values->data[index].next;
    return second < values->count && !cssValueIsLiteral(&values->data[second], ',');
}

// Folds color-mix() or palette-mix() at `at` into the form that css-color-5/#serial-color-mix writes. The interpolation
// method is removed if it is the default, oklab, and so is a hue interpolation method of shorter. xyz becomes xyz-d65.
// The percentages are all removed if each is an equal share of 100%. Otherwise each omitted percentage becomes the
// share of 100% that the others leave, unless one is a math function.
void foldMix(CssValuePool* pool, uint32_t at) {
    Array<CssValueNode>* values = &pool->values;
    uint32_t first = at + 1;
    if (cssValueIsKeyword(&values->data[first], PrCssKeyword_In)) {
        PrCssKeyword space;
        uint32_t length = foldInterpolationMethod(values, first, &space);
        if (space == PrCssKeyword_Oklab)
            values->erase(first, length + 1);
        else
            first += length + 1;
    }

    uint32_t items = 0;
    uint32_t omitted = 0;
    uint32_t calculated = 0;
    double sum = 0.0;
    double same = -1.0;
    bool equal = true;
    for (uint32_t i = first; i < values->count; ++i) {
        ++items;
        uint32_t second = i + values->data[i].next;
        if (!mixHasSecond(values, i)) {
            ++omitted;
            i = second;
            continue;
        }
        const CssValueNode* percentage = &values->data[second];
        if (percentage->kind == PrCssValueKind_Percentage) {
            sum += percentage->number;
            equal = equal && (same < 0.0 || same == percentage->number);
            same = percentage->number;
        } else {
            ++calculated;
        }
        i = second + percentage->next;
    }

    double share = omitted != 0 ? (100.0 - sum) / omitted : same;
    bool even = equal && (same < 0.0 || (same == share && same * items == 100.0));
    if (calculated == 0 && (even || omitted != 0)) {
        CssValueNode percentage = cssNumberValue(PrCssValueKind_Percentage, PrCssUnit_Number, share);
        for (uint32_t i = first; i < values->count; ++i) {
            uint32_t second = i + values->data[i].next;
            bool written = mixHasSecond(values, i);
            if (written && even)
                values->erase(second);
            else if (!written && !even)
                values->insert(second, percentage);
            i = even ? second : second + 1;
        }
    }
    values->data[at].next = values->count - at;
}

// Removes the shape from the radial gradient at `at` if its size implies the shape, along with a comma that would then
// come first. css-images-3/#valdef-radial-gradient-radial-shape: "If <radial-shape> is omitted, the ending shape
// defaults to a circle if the <radial-size> is a single <length>, and to an ellipse otherwise."
void omitImpliedShape(Array<CssValueNode>* values, uint32_t at) {
    const CssValueNode* shape = &values->data[at + 1];
    PrCssValueKind size = shape[1].kind;
    bool implied = cssValueIsKeyword(shape, PrCssKeyword_Ellipse) || (cssValueIsKeyword(shape, PrCssKeyword_Circle) &&
        (size == PrCssValueKind_Dimension || size == PrCssValueKind_Math));
    if (!implied)
        return;
    values->erase(at + 1);
    if (cssValueIsLiteral(&values->data[at + 1], ','))
        values->erase(at + 1);
}

// Folds a gradient at `at` into the form that css-images-4/#serialization writes. A radial gradient's shape is removed
// if its size implies it. Its color interpolation method, which is the last item before its stops, is removed if it is
// the default: srgb for legacy stops and oklab otherwise (css-images-4/#color-interpolation).
void foldGradient(CssValuePool* pool, uint32_t at) {
    Array<CssValueNode>* values = &pool->values;
    PrCssKeyword keyword = cssValueKeyword(&values->data[at]);
    if (keyword == PrCssKeyword_RadialGradient || keyword == PrCssKeyword_RepeatingRadialGradient)
        omitImpliedShape(values, at);

    uint32_t method = values->count;
    for (uint32_t i = at + 1; i < values->count && !cssValueIsLiteral(&values->data[i], ','); i += values->data[i].next) {
        if (cssValueIsKeyword(&values->data[i], PrCssKeyword_In))
            method = i;
    }
    if (method != values->count) {
        PrCssKeyword space;
        uint32_t length = foldInterpolationMethod(values, method, &space);
        bool legacy = cssColorsAreLegacy(values->data, method + length, values->count);
        if (space == (legacy ? PrCssKeyword_Srgb : PrCssKeyword_Oklab)) {
            values->erase(method, length);
            if (method == at + 1)
                values->erase(method);
        }
    }
    values->data[at].next = values->count - at;
}

// Returns true if the function whose value is at `at` folds its match into the value it represents, as its prose
// specifies.
bool folds(const CssValueNode* values, uint32_t at) {
    PrCssKeyword keyword = cssValueKeyword(&values[at]);
    const CssValueNode* argument = &values[at + 1];
    switch (keyword) {
        case PrCssKeyword_ColorMix:
        case PrCssKeyword_PaletteMix:
        case PrCssKeyword_LinearGradient:
        case PrCssKeyword_RepeatingLinearGradient:
        case PrCssKeyword_RadialGradient:
        case PrCssKeyword_RepeatingRadialGradient:
        case PrCssKeyword_ConicGradient:
        case PrCssKeyword_RepeatingConicGradient:
        case PrCssKeyword_ColorLayers:
        case PrCssKeyword_Alpha:
            return true;
        case PrCssKeyword_Color:
            return cssValueIsKeyword(argument, PrCssKeyword_From) || (argument->kind == PrCssValueKind_Keyword &&
                cssColorSpaceFromKeyword(cssValueKeyword(argument)) != PrColorSpace_Count);
        default:
            return cssColorSpaceFromKeyword(keyword) != PrColorSpace_Count;
    }
}

// Folds the match of a function that folds into the value it represents.
void foldFunction(CssValuePool* pool, uint32_t at) {
    PrCssKeyword keyword = cssValueKeyword(&pool->values.data[at]);
    switch (keyword) {
        case PrCssKeyword_ColorMix:
        case PrCssKeyword_PaletteMix:
            foldMix(pool, at);
            return;
        case PrCssKeyword_LinearGradient:
        case PrCssKeyword_RepeatingLinearGradient:
        case PrCssKeyword_RadialGradient:
        case PrCssKeyword_RepeatingRadialGradient:
        case PrCssKeyword_ConicGradient:
        case PrCssKeyword_RepeatingConicGradient:
            foldGradient(pool, at);
            return;
        case PrCssKeyword_ColorLayers:
            // css-color-6/#color-layers: the blend mode is normal if none is given.
            if (cssValueIsKeyword(&pool->values.data[at + 1], PrCssKeyword_Normal)) {
                pool->values.erase(at + 1, 2);
                pool->values.data[at].next -= 2;
            }
            return;
        default:
            break;
    }

    const CssValueNode* argument = &pool->values.data[at + 1];
    PrColorSpace space = cssColorSpaceFromKeyword(keyword);
    if (cssValueIsKeyword(argument, PrCssKeyword_From)) {
        foldRelativeColor(pool, at);
    } else if (keyword == PrCssKeyword_Color) {
        space = cssColorSpaceFromKeyword(cssValueKeyword(argument));
        foldColor(pool, at, at + 2, space, 0);
    } else {
        foldColor(pool, at, at + 1, space, space == PrColorSpace_Srgb ? CssValueAuxFlag_ColorLegacy : 0);
    }
}

} // namespace

// Matches the whole of the matcher's stream, from `production`, or from `root` if `production` is CssValueDef_Count.
// The match is structural if `structural` is set. It runs above any frames and choices that the current match already
// holds, because a check in the middle of a match can start it, and it leaves them unchanged. On failure, the stream
// and pool are unmodified.
bool cssMatchWhole(CssMatcher* m, CssGrammarIndex root, CssValueDef production, bool structural) {
    uint32_t frames = m->match->frames.count;
    uint32_t pos = m->ts->pos;
    uint32_t end = m->ts->end;
    uint32_t values = m->pool->values.count;
    uint32_t text = m->pool->text.count;
    m->choices = m->match->choices.count;
    m->edits = m->match->edits.count;
    m->floor = 0;

    CssMatchFrame last = {};
    last.kind = CssMatchFrameKind_End;
    last.flags = structural ? CssMatchFlag_Structural : 0;
    last.production = frames;
    bool matched = enterFrame(m, last);
    if (matched) {
        matched = production != CssValueDef_Count ? enterType(m, production, m->frame) : enterNode(m, root, m->frame);
    }
    matched = matched && run(m);

    m->match->frames.count = frames;
    m->match->choices.count = m->choices;
    if (matched) {
        applyEdits(m, m->edits, values);
        return true;
    }
    m->match->edits.count = m->edits;
    m->ts->pos = pos;
    m->ts->end = end;
    m->pool->values.count = values;
    m->pool->text.count = text;
    return false;
}

// Returns a matcher over `ts` for a match nested inside the one that `m` is running. The new matcher takes its context
// and the features of its queries from `m`.
CssMatcher cssMatchNested(const CssMatcher* m, CssTokenStream* ts) {
    CssMatcher nested = {};
    nested.ts = ts;
    nested.pool = m->pool;
    nested.match = m->match;
    nested.context = m->context;
    nested.parent = m;
    nested.argument = argumentOf(CssPseudoClass_None);
    nested.features = m->features;
    nested.anchors = m->anchors;
    nested.property = m->property;
    nested.start = m->start;
    nested.depth = m->depth;
    return nested;
}

// Matches the whole of `span`, from `production` or `root` as in cssMatchWhole, for the argument of a pseudo-class or
// pseudo-element whose check `m` is running.
bool cssMatchArgument(const CssMatcher* m, CssTokenSpan span, CssGrammarIndex root,
        CssValueDef production, CssMatchArgument argument) {
    CssTokenStream ts = cssSubStream(m->ts, span);
    CssMatcher nested = cssMatchNested(m, &ts);
    nested.argument = argument;
    return cssMatchWhole(&nested, root, production, production != CssValueDef_Count);
}

// Matches the whole of `span` as `production`, a query whose features are descriptors of `features`.
bool cssMatchQuery(const CssMatcher* m, CssTokenSpan span, CssValueDef production, CssAtRule features) {
    CssTokenStream ts = cssSubStream(m->ts, span);
    CssMatcher nested = cssMatchNested(m, &ts);
    nested.features = features;
    return cssMatchWhole(&nested, CssGrammarRoot_None, production, true);
}

// Returns the component keywords of the relative color whose arguments contain the match, as kCssColorComponents lists
// them. For alpha(), which accepts only `alpha`, each keyword is PrCssKeyword_Unknown. Returns null outside a relative
// color.
const PrCssKeyword* PR_NULLABLE cssMatchRelativeComponents(const CssMatcher* m) {
    static constexpr PrCssKeyword kAlphaAlone[3] = {PrCssKeyword_Unknown, PrCssKeyword_Unknown, PrCssKeyword_Unknown};
    const CssValueNode* values = m->pool->values.data;
    uint32_t count = m->pool->values.count;
    for (uint32_t index = m->frame;; index = frameAt(m, index).outer) {
        const CssMatchFrame& f = frameAt(m, index);
        // A match nested in another, such as a math function's arguments, is inside the function that the outer match
        // is in.
        if (f.kind == CssMatchFrameKind_End)
            return m->parent != nullptr ? cssMatchRelativeComponents(m->parent) : nullptr;
        if (f.kind != CssMatchFrameKind_Function)
            continue;

        const CssValueNode* function = &values[f.anchor];
        PrColorSpace space = cssColorSpaceFromKeyword(cssValueKeyword(function));
        if (space == PrColorSpace_Count && cssValueKeyword(function) != PrCssKeyword_Color &&
                cssValueKeyword(function) != PrCssKeyword_Alpha) {
            continue;
        }
        if (f.anchor + 2 >= count || !cssValueIsKeyword(&function[1], PrCssKeyword_From))
            return nullptr;
        if (cssValueKeyword(function) == PrCssKeyword_Alpha)
            return kAlphaAlone;
        if (cssValueKeyword(function) == PrCssKeyword_Color) {
            uint32_t named = f.anchor + 2 + function[2].next;
            if (named >= count)
                return nullptr;
            space = cssColorSpaceFromKeyword(cssValueKeyword(&values[named]));
        }
        return kCssColorComponents[prColorSpaceModel(space)];
    }
}

} // namespace pr_impl
