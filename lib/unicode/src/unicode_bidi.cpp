// The Unicode Bidirectional Algorithm, with the rule numbers of UAX #9. bidi_data.cpp holds the classes and brackets it
// reads.

#include "pr/unicode_bidi.h"

#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// The maximum depth of the stack of paired brackets (BD16).
constexpr uint32_t kMaxBrackets = 63;

// An entry of the directional status stack (X1).
struct DirectionalStatus {
    uint8_t level;
    PrUnicodeBidiClass override;         // L or R, or ON for none
    bool isolate;
};

bool isIsolateInitiator(PrUnicodeBidiClass c) {
    return c == PrUnicodeBidiClass_LRI || c == PrUnicodeBidiClass_RLI || c == PrUnicodeBidiClass_FSI;
}

bool isRemoved(PrUnicodeBidiClass c) {
    return c == PrUnicodeBidiClass_BN || c == PrUnicodeBidiClass_RLE || c == PrUnicodeBidiClass_LRE ||
        c == PrUnicodeBidiClass_RLO || c == PrUnicodeBidiClass_LRO || c == PrUnicodeBidiClass_PDF;
}

// Returns true if `c` is a neutral or isolate formatting character (NI, of N1).
bool isNeutral(PrUnicodeBidiClass c) {
    return c == PrUnicodeBidiClass_B || c == PrUnicodeBidiClass_S || c == PrUnicodeBidiClass_WS ||
        c == PrUnicodeBidiClass_ON || isIsolateInitiator(c) || c == PrUnicodeBidiClass_PDI;
}

// Returns the direction that a strong type or number contributes to rules N0 and N1. Numbers count as R.
PrUnicodeBidiClass strongOf(PrUnicodeBidiClass c) {
    if (c == PrUnicodeBidiClass_L)
        return PrUnicodeBidiClass_L;
    if (c == PrUnicodeBidiClass_R || c == PrUnicodeBidiClass_AL || c == PrUnicodeBidiClass_EN ||
            c == PrUnicodeBidiClass_AN)
        return PrUnicodeBidiClass_R;
    return PrUnicodeBidiClass_ON;
}

PrUnicodeBidiClass directionOf(uint8_t level) {
    return (level & 1) != 0 ? PrUnicodeBidiClass_R : PrUnicodeBidiClass_L;
}

// Returns the level given by the first strong character of [start, end) (P2, P3), or `fallback` if none is strong. The
// search skips characters between an isolate initiator and its matching PDI. If `isolate` is set, an unmatched PDI ends
// the search, because the text of an FSI ends there.
uint8_t firstStrongLevel(const PrUnicodeBidiClass* classes, uint32_t start, uint32_t end, bool isolate,
        uint8_t fallback) {
    uint32_t depth = 0;
    for (uint32_t i = start; i < end; ++i) {
        PrUnicodeBidiClass c = classes[i];
        if (isIsolateInitiator(c)) {
            ++depth;
        } else if (c == PrUnicodeBidiClass_PDI) {
            if (depth == 0 && isolate)
                break;
            if (depth != 0)
                --depth;
        } else if (c == PrUnicodeBidiClass_B) {
            break;
        } else if (depth == 0 && c == PrUnicodeBidiClass_L) {
            return 0;
        } else if (depth == 0 && (c == PrUnicodeBidiClass_R || c == PrUnicodeBidiClass_AL)) {
            return 1;
        }
    }
    return fallback;
}

// The characters of a paragraph as the rules after X9 see them, without the characters that X9 removes.
struct Paragraph {
    const PrUnicodeBidiClass* classes;               // the original classes
    const uint32_t* PR_NULLABLE codePoints;
    uint8_t* levels;
    uint8_t level;
    Array<PrUnicodeBidiClass> types;        // the type of each character, as the rules resolve it
    Array<uint32_t> kept;                   // the characters that X9 keeps, in order
    Array<uint8_t> embedding;               // the explicit level of each kept character

    // For each kept character, the index in `kept` of its matching PDI or isolate initiator, or kArrayMissing.
    Array<uint32_t> matching;

    Array<uint32_t> runStarts;              // the level runs, as the index in `kept` where each starts
    Array<uint32_t> runOf;                  // for each kept character, its level run
    Array<uint32_t> sequence;               // an isolating run sequence, as indices of `kept`
    Array<uint32_t> openers;                // N0's stack, as positions in `sequence`
    Array<uint32_t> openerKeys;
    Array<uint32_t> pairs;                  // N0's pairs, opener then closer, by opener
};

// Computes the explicit levels and directions (X1 through X8) into `p.levels` and `p.types`.
void explicitLevels(Paragraph& p, uint32_t start, uint32_t end) {
    DirectionalStatus stack[kPrUnicodeBidiMaxDepth + 2];
    uint32_t depth = 0;
    stack[depth++] = DirectionalStatus{p.level, PrUnicodeBidiClass_ON, false};
    uint32_t overflowIsolates = 0;
    uint32_t overflowEmbeddings = 0;
    uint32_t validIsolates = 0;
    for (uint32_t i = start; i < end; ++i) {
        PrUnicodeBidiClass c = p.classes[i];
        const DirectionalStatus& top = stack[depth - 1];
        switch (c) {
            case PrUnicodeBidiClass_RLE:
            case PrUnicodeBidiClass_LRE:
            case PrUnicodeBidiClass_RLO:
            case PrUnicodeBidiClass_LRO: {
                bool rtl = c == PrUnicodeBidiClass_RLE || c == PrUnicodeBidiClass_RLO;
                uint8_t level = rtl ? uint8_t((top.level + 1) | 1) : uint8_t((top.level + 2) & ~1);
                p.levels[i] = top.level;
                if (level <= kPrUnicodeBidiMaxDepth && overflowIsolates == 0 && overflowEmbeddings == 0) {
                    PrUnicodeBidiClass override = PrUnicodeBidiClass_ON;
                    if (c == PrUnicodeBidiClass_RLO)
                        override = PrUnicodeBidiClass_R;
                    else if (c == PrUnicodeBidiClass_LRO)
                        override = PrUnicodeBidiClass_L;
                    stack[depth++] = DirectionalStatus{level, override, false};
                } else if (overflowIsolates == 0) {
                    ++overflowEmbeddings;
                }
                break;
            }
            case PrUnicodeBidiClass_RLI:
            case PrUnicodeBidiClass_LRI:
            case PrUnicodeBidiClass_FSI: {
                p.levels[i] = top.level;
                if (top.override != PrUnicodeBidiClass_ON)
                    p.types.data[i - start] = top.override;
                bool rtl = c == PrUnicodeBidiClass_RLI || (c == PrUnicodeBidiClass_FSI &&
                    firstStrongLevel(p.classes, i + 1, end, true, 0) == 1);
                uint8_t level = rtl ? uint8_t((top.level + 1) | 1) : uint8_t((top.level + 2) & ~1);
                if (level <= kPrUnicodeBidiMaxDepth && overflowIsolates == 0 && overflowEmbeddings == 0) {
                    ++validIsolates;
                    stack[depth++] = DirectionalStatus{level, PrUnicodeBidiClass_ON, true};
                } else {
                    ++overflowIsolates;
                }
                break;
            }
            case PrUnicodeBidiClass_PDI:
                if (overflowIsolates != 0) {
                    --overflowIsolates;
                } else if (validIsolates != 0) {
                    overflowEmbeddings = 0;
                    while (!stack[depth - 1].isolate)
                        --depth;
                    --depth;
                    --validIsolates;
                }
                p.levels[i] = stack[depth - 1].level;
                if (stack[depth - 1].override != PrUnicodeBidiClass_ON)
                    p.types.data[i - start] = stack[depth - 1].override;
                break;
            case PrUnicodeBidiClass_PDF:
                p.levels[i] = top.level;
                if (overflowIsolates != 0) {
                } else if (overflowEmbeddings != 0) {
                    --overflowEmbeddings;
                } else if (!top.isolate && depth >= 2) {
                    --depth;
                }
                break;
            case PrUnicodeBidiClass_B:
                p.levels[i] = p.level;
                break;
            case PrUnicodeBidiClass_BN:
                p.levels[i] = top.level;
                break;
            default:
                p.levels[i] = top.level;
                if (top.override != PrUnicodeBidiClass_ON)
                    p.types.data[i - start] = top.override;
                break;
        }
    }
}

// Resolves the weak types of an isolating run sequence (W1 through W7). `t` reads the type at each position of the
// sequence.
void resolveWeak(Paragraph& p, uint32_t start, PrUnicodeBidiClass sos) {
    uint32_t n = p.sequence.count;
    auto t = [&](uint32_t i) -> PrUnicodeBidiClass& {
        return p.types.data[p.kept.data[p.sequence.data[i]] - start];
    };
    for (uint32_t i = 0; i < n; ++i) {
        if (t(i) != PrUnicodeBidiClass_NSM)
            continue;
        if (i == 0)
            t(i) = sos;
        else
            t(i) = isIsolateInitiator(t(i - 1)) || t(i - 1) == PrUnicodeBidiClass_PDI ? PrUnicodeBidiClass_ON : t(i - 1);
    }
    PrUnicodeBidiClass strong = sos;
    for (uint32_t i = 0; i < n; ++i) {
        PrUnicodeBidiClass c = t(i);
        if (c == PrUnicodeBidiClass_L || c == PrUnicodeBidiClass_R || c == PrUnicodeBidiClass_AL)
            strong = c;
        else if (c == PrUnicodeBidiClass_EN && strong == PrUnicodeBidiClass_AL)
            t(i) = PrUnicodeBidiClass_AN;
    }
    for (uint32_t i = 0; i < n; ++i)
        if (t(i) == PrUnicodeBidiClass_AL)
            t(i) = PrUnicodeBidiClass_R;
    for (uint32_t i = 1; i + 1 < n; ++i) {
        PrUnicodeBidiClass before = t(i - 1);
        PrUnicodeBidiClass after = t(i + 1);
        if (t(i) == PrUnicodeBidiClass_ES && before == PrUnicodeBidiClass_EN && after == PrUnicodeBidiClass_EN)
            t(i) = PrUnicodeBidiClass_EN;
        else if (t(i) == PrUnicodeBidiClass_CS && before == PrUnicodeBidiClass_EN && after == PrUnicodeBidiClass_EN)
            t(i) = PrUnicodeBidiClass_EN;
        else if (t(i) == PrUnicodeBidiClass_CS && before == PrUnicodeBidiClass_AN && after == PrUnicodeBidiClass_AN)
            t(i) = PrUnicodeBidiClass_AN;
    }
    for (uint32_t i = 0; i < n;) {
        if (t(i) != PrUnicodeBidiClass_ET) {
            ++i;
            continue;
        }
        uint32_t end = i;
        while (end < n && t(end) == PrUnicodeBidiClass_ET)
            ++end;
        bool number = (i > 0 && t(i - 1) == PrUnicodeBidiClass_EN) || (end < n && t(end) == PrUnicodeBidiClass_EN);
        for (uint32_t j = i; j < end && number; ++j)
            t(j) = PrUnicodeBidiClass_EN;
        i = end;
    }
    for (uint32_t i = 0; i < n; ++i)
        if (t(i) == PrUnicodeBidiClass_ES || t(i) == PrUnicodeBidiClass_ET || t(i) == PrUnicodeBidiClass_CS)
            t(i) = PrUnicodeBidiClass_ON;
    strong = sos;
    for (uint32_t i = 0; i < n; ++i) {
        PrUnicodeBidiClass c = t(i);
        if (c == PrUnicodeBidiClass_L || c == PrUnicodeBidiClass_R)
            strong = c;
        else if (c == PrUnicodeBidiClass_EN && strong == PrUnicodeBidiClass_L)
            t(i) = PrUnicodeBidiClass_L;
    }
}

// Resolves the paired brackets of an isolating run sequence (BD16, N0). A pair takes the embedding direction if a
// strong type of that direction is inside it. Otherwise it takes the opposite direction if that is inside it and also
// precedes it. Nonspacing marks after either bracket follow the bracket.
void resolveBrackets(Paragraph& p, uint32_t start, PrUnicodeBidiClass sos, PrUnicodeBidiClass embedding) {
    if (p.codePoints == nullptr)
        return;
    uint32_t n = p.sequence.count;
    auto index = [&](uint32_t i) { return p.kept.data[p.sequence.data[i]]; };
    auto t = [&](uint32_t i) -> PrUnicodeBidiClass& { return p.types.data[index(i) - start]; };
    p.openers.clear();
    p.openerKeys.clear();
    p.pairs.clear();
    for (uint32_t i = 0; i < n; ++i) {
        if (t(i) != PrUnicodeBidiClass_ON)
            continue;
        uint32_t key = 0;
        PrUnicodeBidiBracket bracket = prUnicodeBidiBracket(p.codePoints[index(i)], &key);
        if (bracket == PrUnicodeBidiBracket_Open) {
            if (p.openers.count == kMaxBrackets)
                break;
            p.openers.push(i);
            p.openerKeys.push(key);
        } else if (bracket == PrUnicodeBidiBracket_Close) {
            for (uint32_t s = p.openers.count; s-- > 0;) {
                if (p.openerKeys.data[s] != key)
                    continue;
                p.pairs.push(p.openers.data[s]);
                p.pairs.push(i);
                p.openers.count = s;
                p.openerKeys.count = s;
                break;
            }
        }
    }
    // Sort the pairs by opener.
    for (uint32_t a = 2; a < p.pairs.count; a += 2) {
        uint32_t opener = p.pairs.data[a];
        uint32_t closer = p.pairs.data[a + 1];
        uint32_t b = a;
        for (; b >= 2 && p.pairs.data[b - 2] > opener; b -= 2) {
            p.pairs.data[b] = p.pairs.data[b - 2];
            p.pairs.data[b + 1] = p.pairs.data[b - 1];
        }
        p.pairs.data[b] = opener;
        p.pairs.data[b + 1] = closer;
    }
    PrUnicodeBidiClass opposite = embedding == PrUnicodeBidiClass_L ? PrUnicodeBidiClass_R : PrUnicodeBidiClass_L;
    for (uint32_t a = 0; a < p.pairs.count; a += 2) {
        uint32_t opener = p.pairs.data[a];
        uint32_t closer = p.pairs.data[a + 1];
        bool matches = false;
        bool opposes = false;
        for (uint32_t i = opener + 1; i < closer; ++i) {
            PrUnicodeBidiClass s = strongOf(t(i));
            matches = matches || s == embedding;
            opposes = opposes || s == opposite;
        }
        PrUnicodeBidiClass direction = embedding;
        if (!matches) {
            if (!opposes)
                continue;
            PrUnicodeBidiClass before = sos;
            for (uint32_t i = opener; i-- > 0;) {
                PrUnicodeBidiClass s = strongOf(t(i));
                if (s != PrUnicodeBidiClass_ON) {
                    before = s;
                    break;
                }
            }
            direction = before == opposite ? opposite : embedding;
        }
        const uint32_t brackets[2] = {opener, closer};
        for (uint32_t bracket : brackets) {
            t(bracket) = direction;
            for (uint32_t i = bracket + 1; i < n && p.classes[index(i)] == PrUnicodeBidiClass_NSM; ++i)
                t(i) = direction;
        }
    }
}

// Resolves the neutral types of an isolating run sequence (N1, N2), then its levels (I1, I2).
void resolveNeutralsAndLevels(Paragraph& p, uint32_t start, PrUnicodeBidiClass sos, PrUnicodeBidiClass eos,
        PrUnicodeBidiClass embedding) {
    uint32_t n = p.sequence.count;
    auto index = [&](uint32_t i) { return p.kept.data[p.sequence.data[i]]; };
    auto t = [&](uint32_t i) -> PrUnicodeBidiClass& { return p.types.data[index(i) - start]; };
    for (uint32_t i = 0; i < n;) {
        if (!isNeutral(t(i))) {
            ++i;
            continue;
        }
        uint32_t end = i;
        while (end < n && isNeutral(t(end)))
            ++end;
        PrUnicodeBidiClass before = i == 0 ? sos : strongOf(t(i - 1));
        PrUnicodeBidiClass after = end == n ? eos : strongOf(t(end));
        PrUnicodeBidiClass direction = before == after && before != PrUnicodeBidiClass_ON ? before : embedding;
        for (uint32_t j = i; j < end; ++j)
            t(j) = direction;
        i = end;
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t& level = p.levels[index(i)];
        PrUnicodeBidiClass c = t(i);
        if ((level & 1) == 0) {
            if (c == PrUnicodeBidiClass_R)
                level += 1;
            else if (c == PrUnicodeBidiClass_AN || c == PrUnicodeBidiClass_EN)
                level += 2;
        } else if (c == PrUnicodeBidiClass_L || c == PrUnicodeBidiClass_EN || c == PrUnicodeBidiClass_AN) {
            level += 1;
        }
    }
}

// Resolves the paragraph [start, end), whose separator, if it has one, is its last character.
void resolveParagraph(Paragraph& p, uint32_t start, uint32_t end) {
    p.types.clear();
    for (uint32_t i = start; i < end; ++i)
        p.types.push(p.classes[i]);
    explicitLevels(p, start, end);

    // X9: find the characters that the later rules see, and the matching PDI of each isolate initiator (BD9).
    p.kept.clear();
    p.matching.clear();
    p.openers.clear();
    for (uint32_t i = start; i < end; ++i) {
        PrUnicodeBidiClass c = p.classes[i];
        if (isRemoved(c)) {
            p.levels[i] = kPrUnicodeBidiRemoved;
            continue;
        }
        uint32_t at = p.kept.count;
        p.kept.push(i);
        p.matching.push(kArrayMissing);
        if (isIsolateInitiator(c)) {
            p.openers.push(at);
        } else if (c == PrUnicodeBidiClass_PDI && p.openers.count != 0) {
            uint32_t opener = p.openers.data[--p.openers.count];
            p.matching.data[opener] = at;
            p.matching.data[at] = opener;
        }
    }
    uint32_t count = p.kept.count;
    if (count == 0)
        return;
    p.embedding.clear();
    for (uint32_t k = 0; k < count; ++k)
        p.embedding.push(p.levels[p.kept.data[k]]);

    // X10: find the level runs, and chain them through matched isolates into isolating run sequences.
    p.runStarts.clear();
    p.runOf.clear();
    for (uint32_t k = 0; k < count; ++k) {
        if (k == 0 || p.embedding.data[k] != p.embedding.data[k - 1])
            p.runStarts.push(k);
        p.runOf.push(p.runStarts.count - 1);
    }
    for (uint32_t r = 0; r < p.runStarts.count; ++r) {
        uint32_t first = p.runStarts.data[r];
        // A run that starts with a matched PDI continues the sequence of its initiator.
        if (p.classes[p.kept.data[first]] == PrUnicodeBidiClass_PDI && p.matching.data[first] != kArrayMissing)
            continue;
        p.sequence.clear();
        for (uint32_t run = r;;) {
            uint32_t runStart = p.runStarts.data[run];
            uint32_t runEnd = run + 1 < p.runStarts.count ? p.runStarts.data[run + 1] : count;
            for (uint32_t k = runStart; k < runEnd; ++k)
                p.sequence.push(k);
            uint32_t last = runEnd - 1;
            if (!isIsolateInitiator(p.classes[p.kept.data[last]]) || p.matching.data[last] == kArrayMissing)
                break;
            run = p.runOf.data[p.matching.data[last]];
        }
        uint32_t firstKept = p.sequence.data[0];
        uint32_t lastKept = p.sequence.last();
        uint8_t level = p.embedding.data[firstKept];
        uint8_t before = firstKept == 0 ? p.level : p.embedding.data[firstKept - 1];
        uint8_t after;
        if (lastKept + 1 == count || isIsolateInitiator(p.classes[p.kept.data[lastKept]]))
            after = p.level;
        else
            after = p.embedding.data[lastKept + 1];
        PrUnicodeBidiClass sos = directionOf(level > before ? level : before);
        PrUnicodeBidiClass eos = directionOf(level > after ? level : after);
        PrUnicodeBidiClass embedding = directionOf(level);
        resolveWeak(p, start, sos);
        resolveBrackets(p, start, sos, embedding);
        resolveNeutralsAndLevels(p, start, sos, eos, embedding);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrUnicodeBidiStrong prUnicodeBidiStrong(uint32_t codePoint) {
    PrUnicodeBidiClass c = prUnicodeBidiClass(codePoint);
    if (c == PrUnicodeBidiClass_L)
        return PrUnicodeBidiStrong_Left;
    if (c == PrUnicodeBidiClass_R || c == PrUnicodeBidiClass_AL)
        return PrUnicodeBidiStrong_Right;
    return PrUnicodeBidiStrong_None;
}

PrUnicodeBidiStrong prUnicodeBidiFirstStrong(PrStringView text) {
    for (size_t at = 0; at < text.size;) {
        char byte = text.data[at];
        if (uint8_t(byte) < 0x80) {
            if (prAsciiIsLetter(byte))
                return PrUnicodeBidiStrong_Left;
            ++at;
            continue;
        }
        uint32_t codePoint = 0;
        at += prUtf8Decode(text, at, &codePoint);
        PrUnicodeBidiStrong strong = prUnicodeBidiStrong(codePoint);
        if (strong != PrUnicodeBidiStrong_None)
            return strong;
    }
    return PrUnicodeBidiStrong_None;
}

uint8_t prUnicodeBidiResolve(const PrUnicodeBidiClass* classes, const uint32_t* PR_NULLABLE codePoints,
        uint32_t count, PrUnicodeBidiDirection direction, uint8_t* levels) {
    Paragraph p{};
    p.classes = classes;
    p.codePoints = codePoints;
    p.levels = levels;
    p.level = direction == PrUnicodeBidiDirection_RightToLeft ? 1 : 0;
    for (uint32_t start = 0; start < count;) {
        uint32_t end = start;
        while (end < count && classes[end] != PrUnicodeBidiClass_B)
            ++end;
        if (end < count)
            ++end;
        if (direction == PrUnicodeBidiDirection_Auto)
            p.level = firstStrongLevel(classes, start, end, false, 0);
        else
            p.level = direction == PrUnicodeBidiDirection_RightToLeft ? 1 : 0;
        resolveParagraph(p, start, end);
        start = end;
    }
    p.types.reset();
    p.kept.reset();
    p.embedding.reset();
    p.matching.reset();
    p.runStarts.reset();
    p.runOf.reset();
    p.sequence.reset();
    p.openers.reset();
    p.openerKeys.reset();
    p.pairs.reset();
    return p.level;
}

void prUnicodeBidiLineLevels(const PrUnicodeBidiClass* classes, uint32_t count, uint8_t paragraphLevel, uint8_t* levels) {
    bool trailing = true;
    for (uint32_t i = count; i-- > 0;) {
        PrUnicodeBidiClass c = classes[i];
        if (c == PrUnicodeBidiClass_S || c == PrUnicodeBidiClass_B) {
            levels[i] = paragraphLevel;
            trailing = true;
        } else if (isRemoved(c)) {
        } else if (trailing && (c == PrUnicodeBidiClass_WS || isIsolateInitiator(c) || c == PrUnicodeBidiClass_PDI)) {
            levels[i] = paragraphLevel;
        } else {
            trailing = false;
        }
    }
}

void prUnicodeBidiReorder(const uint8_t* levels, uint32_t count, uint32_t* order) {
    // A removed character takes the level of the character before it, or of the first kept character at the start.
    uint8_t first = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (levels[i] != kPrUnicodeBidiRemoved) {
            first = levels[i];
            break;
        }
    }
    Array<uint8_t> effective;
    uint8_t highest = 0;
    uint8_t lowestOdd = kPrUnicodeBidiMaxDepth + 2;
    uint8_t previous = first;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t level = levels[i] == kPrUnicodeBidiRemoved ? previous : levels[i];
        previous = level;
        effective.push(level);
        order[i] = i;
        highest = level > highest ? level : highest;
        if ((level & 1) != 0 && level < lowestOdd)
            lowestOdd = level;
    }
    for (uint32_t level = highest; level >= lowestOdd && level > 0; --level) {
        for (uint32_t i = 0; i < count;) {
            if (effective.data[i] < level) {
                ++i;
                continue;
            }
            uint32_t end = i;
            while (end < count && effective.data[end] >= level)
                ++end;
            for (uint32_t a = i, b = end - 1; a < b; ++a, --b) {
                uint32_t swap = order[a];
                order[a] = order[b];
                order[b] = swap;
                uint8_t swapLevel = effective.data[a];
                effective.data[a] = effective.data[b];
                effective.data[b] = swapLevel;
            }
            i = end;
        }
    }
    effective.reset();
}
