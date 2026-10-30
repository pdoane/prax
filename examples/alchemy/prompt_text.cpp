// Prompt text: attention weights, lines and embedding tokens.

#include "examples/alchemy/prompt_text.h"

#include "pr/ascii.h"
#include "pr/string_ops.h"

#include "examples/alchemy/text.h"

#include <algorithm>
#include <cmath>
#include <stdio.h>
#include <stdlib.h>

namespace alchemy {

using namespace pr;

namespace {

constexpr uint32_t kNoOffset = 0xffffffffu;

// The attention of a group that names no weight of its own.
constexpr double kImplicitWeight = 1.1;

// Returns true if a word continues through `c`. A word ends at whitespace, at the punctuation that a prompt separates
// its terms with, and at the backslash that takes a parenthesis out of the grammar.
bool isWordChar(char c) {
    PrStringView separators = ",.:;!?'\"()[]{}\\";
    return !asciiIsSpace(c) && stringFind(separators, c) == separators.size;
}

// Returns true if the parenthesis at `pos` is text rather than a delimiter: a backslash before it removes it from the
// grammar.
bool isEscapedParen(PrStringView text, uint32_t pos) {
    return pos > 0 && text.data[pos - 1] == '\\';
}

bool isDelimiter(PrStringView text, uint32_t pos) {
    char c = text.data[pos];
    return (c == '(' || c == ')') && !isEscapedParen(text, pos);
}

// Appends `weight` with two decimals and without trailing zeros: 1.15, 1.1, 1.
void appendWeight(std::string& out, double weight) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%.2f", weight);
    std::string text = buffer;
    while (!text.empty() && text.back() == '0')
        text.pop_back();
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    out += text;
}

double roundWeight(double weight) {
    return std::round(weight * 100.0) / 100.0;
}

void appendGroup(std::string& out, PrStringView inner, double weight) {
    out += '(';
    out.append(inner.begin(), inner.end());
    out += ':';
    appendWeight(out, weight);
    out += ')';
}

// Splits a group's content into its text and its weight. Only the last colon can name the weight, and content whose
// tail past it is not a number names none.
bool splitWeight(PrStringView content, PrStringView* inner, double* weight) {
    size_t colon = stringFindLast(content, ':');
    if (colon == content.size || colon == 0)
        return false;
    std::string tail = stringOf(stringTrim(stringSlice(content, colon + 1, content.size - colon - 1)));
    if (tail.empty())
        return false;
    char* end = nullptr;
    double parsed = strtod(tail.c_str(), &end);
    if (end != tail.c_str() + tail.size() || !std::isfinite(parsed))
        return false;
    *inner = stringSlice(content, 0, colon);
    *weight = parsed;
    return true;
}

// A parenthesized run: the bytes that its parentheses cover, the bytes of the text that they weight, and the attention
// that the text is read at.
struct Group {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t innerStart = 0;
    uint32_t innerEnd = 0;
    double weight = 0.0;
};

Group readGroup(PrStringView text, uint32_t start, uint32_t end) {
    Group group{.start = start, .end = end, .innerStart = start + 1, .innerEnd = end - 1, .weight = kImplicitWeight};
    PrStringView inner;
    double weight = 0.0;
    if (splitWeight(stringSlice(text, group.innerStart, group.innerEnd - group.innerStart), &inner, &weight)) {
        group.innerEnd = group.innerStart + uint32_t(inner.size);
        group.weight = weight;
    }
    return group;
}

PrStringView groupInner(PrStringView text, const Group& group) {
    return stringSlice(text, group.innerStart, group.innerEnd - group.innerStart);
}

// Returns the parenthesis that closes the group opened at `open`, or kNoOffset if the text leaves it open.
uint32_t findClose(PrStringView text, uint32_t open) {
    uint32_t depth = 0;
    for (uint32_t i = open; i < text.size; ++i) {
        if (!isDelimiter(text, i))
            continue;
        if (text.data[i] == '(')
            ++depth;
        else if (--depth == 0)
            return i;
    }
    return kNoOffset;
}

// Finds the group that `pos` stands inside: the nearest parenthesis to its left that the text has not closed again
// before `pos`.
bool findEnclosingGroup(PrStringView text, uint32_t pos, Group* out) {
    uint32_t depth = 0;
    uint32_t open = kNoOffset;
    for (uint32_t i = pos; i > 0; --i) {
        if (!isDelimiter(text, i - 1))
            continue;
        if (text.data[i - 1] == ')') {
            ++depth;
        } else if (depth == 0) {
            open = i - 1;
            break;
        } else {
            --depth;
        }
    }
    if (open == kNoOffset)
        return false;
    uint32_t close = findClose(text, open);
    if (close == kNoOffset || close == open + 1)
        return false;
    *out = readGroup(text, open, close + 1);
    return true;
}

// Reads the bytes from `start` to `end` as one whole group.
bool parseGroup(PrStringView text, uint32_t start, uint32_t end, Group* out) {
    if (start >= text.size || end - start < 3)
        return false;
    if (text.data[start] != '(' || isEscapedParen(text, start) || findClose(text, start) != end - 1)
        return false;
    *out = readGroup(text, start, end);
    return true;
}

void expandToWord(PrStringView text, uint32_t* start, uint32_t* end) {
    while (*start > 0 && isWordChar(text.data[*start - 1]))
        --*start;
    while (*end < text.size && isWordChar(text.data[*end]))
        ++*end;
}

} // namespace

bool promptWeightAdjust(PrStringView text, uint32_t selectionStart, uint32_t selectionEnd, double delta, WeightEdit* out) {
    uint32_t start = std::min(selectionStart, uint32_t(text.size));
    uint32_t end = std::min(selectionEnd, uint32_t(text.size));
    if (start == end) {
        Group enclosing;
        if (findEnclosingGroup(text, start, &enclosing)) {
            start = enclosing.start;
            end = enclosing.end;
        } else {
            expandToWord(text, &start, &end);
        }
    }
    if (start == end)
        return false;

    Group group;
    bool weighted = parseGroup(text, start, end, &group);
    if (!weighted) {
        Group enclosing;
        if (findEnclosingGroup(text, start, &enclosing) && enclosing.innerStart == start && enclosing.innerEnd == end) {
            group = enclosing;
            weighted = true;
        }
    }

    out->text.clear();
    if (weighted) {
        out->begin = group.start;
        out->end = group.end;
        // Zero is a stop rather than a wall: a step that would cross it lands on it, and a further step continues into
        // the negative weights that the server takes.
        double adjusted = roundWeight(group.weight + delta);
        if (group.weight > 0.0 && adjusted < 0.0)
            adjusted = 0.0;
        if (adjusted == 1.0)
            out->text = stringOf(groupInner(text, group));
        else
            appendGroup(out->text, groupInner(text, group), adjusted);
        return true;
    }

    double adjusted = roundWeight(1.0 + delta);
    if (adjusted == 1.0)
        return false;
    out->begin = start;
    out->end = end;
    appendGroup(out->text, stringSlice(text, start, end - start), adjusted);
    return true;
}

std::vector<PrStringView> promptLines(PrStringView prompt) {
    std::vector<PrStringView> lines;
    PrStringView text = stringTrim(prompt);
    while (text.size != 0) {
        size_t newline = stringFind(text, '\n');
        lines.push_back(stringSlice(text, 0, newline));
        if (newline == text.size)
            break;
        text = stringSlice(text, newline + 1, text.size - newline - 1);
    }
    return lines;
}

std::string promptAppendLine(PrStringView prompt, PrStringView line) {
    std::string out = stringOf(prompt);
    if (!out.empty())
        out += '\n';
    out.append(line.begin(), line.end());
    return out;
}

PrStringView embeddingName(PrStringView filename) {
    for (PrStringView extension : {".safetensors", ".ckpt", ".pt", ".pth", ".bin"}) {
        if (asciiEndsWithFold(filename, extension))
            return stringSlice(filename, 0, filename.size - extension.size);
    }
    return filename;
}

std::string embeddingInsertText(PrStringView text, uint32_t selectionStart, uint32_t selectionEnd,
        PrStringView filename) {
    uint32_t begin = std::min(selectionStart, uint32_t(text.size));
    uint32_t end = std::min(selectionEnd, uint32_t(text.size));
    std::string out;
    if (begin > 0 && !asciiIsSpace(text.data[begin - 1]))
        out += ' ';
    out += "embedding:";
    PrStringView name = embeddingName(filename);
    out.append(name.begin(), name.end());
    if (end < text.size && !asciiIsSpace(text.data[end]))
        out += ' ';
    return out;
}

} // namespace alchemy
