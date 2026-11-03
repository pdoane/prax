// Regular expressions: a parser to a tree of nodes, a compiler of the tree to the steps of a program, and a simulation
// that runs every thread of the program at once over the text, in the order of the pattern's preference.

#include "pr/regex.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/raw_array.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

// What a step of a program does.
enum {
    Op_Rune,                        // takes the code point `rune`
    Op_Any,                         // takes any code point but a line feed
    Op_Class,                       // takes a code point of the class `x`
    Op_Match,
    Op_Jump,                        // goes on at `x`
    Op_Split,                       // goes on at `x`, which it prefers, and at `y`
    Op_Save,                        // records the offset in slot `x`
    Op_Start,                       // goes on only at the start of the text
    Op_End,                         // goes on only at the end of the text
};
typedef uint8_t Op;

struct Step {
    Op op;
    uint32_t rune;
    uint32_t x;
    uint32_t y;
};

// A range of code points, both ends included.
struct RuneRange {
    uint32_t low;
    uint32_t high;
};

// A class: a range of the program's rune ranges, and whether it takes the code points outside them.
struct RuneClass {
    uint32_t first;
    uint32_t count;
    bool negated;
};

// What a pattern compiles to.
struct Program {
    RawArray<Step> steps;
    RawArray<RuneRange> ranges;
    RawArray<RuneClass> classes;
    uint32_t groups;                // with group 0
};

// The most that a repetition counts, the deepest that groups nest, and the most steps of a program.
constexpr int32_t kMaxRepeat = 1000;
constexpr uint32_t kMaxDepth = 256;
constexpr uint32_t kMaxSteps = 1u << 20;

// An offset of a slot that no step has recorded.
constexpr uint32_t kSlotNone = 0xffffffffu;

constexpr RuneRange kDigitRanges[] = {{'0', '9'}};
constexpr RuneRange kWordRanges[] = {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}};
constexpr RuneRange kSpaceRanges[] = {{0x09, 0x0a}, {0x0c, 0x0d}, {0x20, 0x20}};

// The ranges of the class that a letter of `\d`, `\w` or `\s` names, in either case.
struct PerlClass {
    const RuneRange* ranges;
    uint32_t count;
};

bool isPerlClass(char c) {
    return c == 'd' || c == 'D' || c == 'w' || c == 'W' || c == 's' || c == 'S';
}

PerlClass perlClassOf(char c) {
    switch (prAsciiLower(c)) {
        case 'd': return PerlClass{kDigitRanges, 1};
        case 'w': return PerlClass{kWordRanges, 4};
        default: return PerlClass{kSpaceRanges, 3};
    }
}

// What a node of the tree is.
enum {
    NodeKind_Empty,
    NodeKind_Rune,
    NodeKind_Any,
    NodeKind_Class,
    NodeKind_Concat,                    // `a` then `b`
    NodeKind_Alternate,                 // `a` or `b`, preferring `a`
    NodeKind_Star,
    NodeKind_Plus,
    NodeKind_Quest,
    NodeKind_Repeat,                    // `a` from `minimum` to `maximum` times, or without bound for a maximum of -1
    NodeKind_Group,                     // `a`, recording what it matches as group `group`
    NodeKind_Start,
    NodeKind_End,
};
typedef uint8_t NodeKind;

struct Node {
    NodeKind kind;
    bool greedy;
    uint32_t rune;
    uint32_t classIndex;
    uint32_t group;
    int32_t minimum;
    int32_t maximum;
    uint32_t a;
    uint32_t b;
};

// A parser by recursive descent, whose recursion follows the nesting of groups:
//
//   alternate := concat ('|' concat)*
//   concat    := repeat*
//   repeat    := atom ('*' | '+' | '?' | '{n}' | '{n,}' | '{n,m}') '?'?
//   atom      := '(' alternate ')' | '[' class ']' | '.' | '^' | '$' | '\' escape | literal
//
// A class's ranges go straight into the program, and its node names the class.
struct Parser {
    PrStringView pattern;
    size_t at;
    uint32_t depth;
    Program* program;
    RawArray<Node> nodes;
    PrStatus status;

    void fail(PrStatus failure) {
        if (status == PrStatus_Ok)
            status = failure;
    }

    bool done() const { return at >= pattern.size; }
    char peek() const { return done() ? '\0' : pattern.data[at]; }
    char peekAt(size_t offset) const { return at + offset < pattern.size ? pattern.data[at + offset] : '\0'; }

    uint32_t decodeRune() {
        uint32_t rune = 0;
        at += prUtf8Decode(pattern, at, &rune);
        return rune;
    }

    uint32_t node(NodeKind kind) {
        Node n{};
        n.kind = kind;
        n.greedy = true;
        nodes.push(n);
        return nodes.count - 1;
    }

    uint32_t binary(NodeKind kind, uint32_t a, uint32_t b) {
        uint32_t n = node(kind);
        nodes.data[n].a = a;
        nodes.data[n].b = b;
        return n;
    }

    uint32_t classNode(uint32_t first, bool negated) {
        program->classes.push(RuneClass{first, program->ranges.count - first, negated});
        uint32_t n = node(NodeKind_Class);
        nodes.data[n].classIndex = program->classes.count - 1;
        return n;
    }

    // Adds the ranges of a Perl class, or of its complement over all code points.
    void addPerlClass(char letter) {
        PerlClass c = perlClassOf(letter);
        if (!prAsciiIsUpper(letter)) {
            program->ranges.append(c.ranges, c.count);
            return;
        }
        uint32_t next = 0;
        for (uint32_t i = 0; i < c.count; ++i) {
            if (c.ranges[i].low > next)
                program->ranges.push(RuneRange{next, c.ranges[i].low - 1});
            next = c.ranges[i].high + 1;
        }
        program->ranges.push(RuneRange{next, kPrUtf8MaxCodePoint});
    }

    uint32_t parse() {
        uint32_t root = parseAlternate();
        if (status == PrStatus_Ok && !done())
            fail(PrStatus_Syntax);
        return root;
    }

    uint32_t parseAlternate() {
        uint32_t left = parseConcat();
        while (status == PrStatus_Ok && peek() == '|') {
            ++at;
            uint32_t right = parseConcat();
            left = binary(NodeKind_Alternate, left, right);
        }
        return left;
    }

    uint32_t parseConcat() {
        uint32_t result = node(NodeKind_Empty);
        bool any = false;
        while (status == PrStatus_Ok && !done() && peek() != '|' && peek() != ')') {
            uint32_t atom = parseRepeat();
            result = any ? binary(NodeKind_Concat, result, atom) : atom;
            any = true;
        }
        return result;
    }

    // Consumes the `?` after a quantifier, which prefers fewer repetitions, and rejects a second quantifier.
    bool parseGreedy() {
        bool greedy = peek() != '?';
        if (!greedy)
            ++at;
        if (peek() == '*' || peek() == '+' || peek() == '?')
            fail(PrStatus_Syntax);
        return greedy;
    }

    uint32_t parseRepeat() {
        uint32_t atom = parseAtom();
        if (status != PrStatus_Ok)
            return atom;
        char c = peek();
        if (c == '*' || c == '+' || c == '?') {
            ++at;
            uint32_t n = node(c == '*' ? NodeKind_Star : c == '+' ? NodeKind_Plus : NodeKind_Quest);
            nodes.data[n].a = atom;
            nodes.data[n].greedy = parseGreedy();
            return n;
        }
        int32_t minimum = 0;
        int32_t maximum = 0;
        // A `{` that does not begin an interval is a literal, which the next atom takes.
        if (c == '{' && parseInterval(&minimum, &maximum)) {
            if (maximum != -1 && maximum < minimum) {
                fail(PrStatus_Syntax);
                return atom;
            }
            if (minimum > kMaxRepeat || maximum > kMaxRepeat) {
                fail(PrStatus_Limit);
                return atom;
            }
            uint32_t n = node(NodeKind_Repeat);
            nodes.data[n].a = atom;
            nodes.data[n].minimum = minimum;
            nodes.data[n].maximum = maximum;
            nodes.data[n].greedy = parseGreedy();
            return n;
        }
        return atom;
    }

    bool parseDecimal(int32_t* out) {
        if (!prAsciiIsDigit(peek()))
            return false;
        int32_t value = 0;
        while (prAsciiIsDigit(peek())) {
            value = value * 10 + (peek() - '0');
            value = value > kMaxRepeat ? kMaxRepeat + 1 : value;
            ++at;
        }
        *out = value;
        return true;
    }

    // Parses `{n}`, `{n,}` or `{n,m}`, with -1 for no upper bound. Leaves the position where it was if the text is no
    // interval.
    bool parseInterval(int32_t* minimum, int32_t* maximum) {
        size_t start = at;
        ++at;
        int32_t low = 0;
        if (parseDecimal(&low)) {
            if (peek() == '}') {
                ++at;
                *minimum = *maximum = low;
                return true;
            }
            if (peek() == ',') {
                ++at;
                int32_t high = 0;
                if (peek() == '}') {
                    ++at;
                    *minimum = low;
                    *maximum = -1;
                    return true;
                }
                if (parseDecimal(&high) && peek() == '}') {
                    ++at;
                    *minimum = low;
                    *maximum = high;
                    return true;
                }
            }
        }
        at = start;
        return false;
    }

    uint32_t parseAtom() {
        switch (peek()) {
            case '(': return parseGroup();
            case '[': return parseClass();
            case '.': ++at; return node(NodeKind_Any);
            case '^': ++at; return node(NodeKind_Start);
            case '$': ++at; return node(NodeKind_End);
            case '\\': return parseEscape();
            case '*':
            case '+':
            case '?': fail(PrStatus_Syntax); return node(NodeKind_Empty);
            default: {
                uint32_t n = node(NodeKind_Rune);
                nodes.data[n].rune = decodeRune();
                return n;
            }
        }
    }

    uint32_t parseGroup() {
        ++at;
        bool recorded = true;
        if (peek() == '?') {
            if (peekAt(1) != ':') {
                fail(PrStatus_Syntax);
                return node(NodeKind_Empty);
            }
            recorded = false;
            at += 2;
        }
        if (++depth > kMaxDepth) {
            fail(PrStatus_Limit);
            return node(NodeKind_Empty);
        }
        uint32_t group = recorded ? program->groups++ : 0;
        uint32_t child = parseAlternate();
        --depth;
        if (status != PrStatus_Ok)
            return child;
        if (peek() != ')') {
            fail(PrStatus_Syntax);
            return child;
        }
        ++at;
        if (!recorded)
            return child;
        uint32_t n = node(NodeKind_Group);
        nodes.data[n].a = child;
        nodes.data[n].group = group;
        return n;
    }

    uint32_t parseEscape() {
        ++at;
        if (done()) {
            fail(PrStatus_Syntax);
            return node(NodeKind_Empty);
        }
        if (isPerlClass(peek())) {
            char letter = peek();
            ++at;
            uint32_t first = program->ranges.count;
            addPerlClass(letter);
            return classNode(first, false);
        }
        uint32_t rune = 0;
        if (!parseEscapedRune(&rune))
            return node(NodeKind_Empty);
        uint32_t n = node(NodeKind_Rune);
        nodes.data[n].rune = rune;
        return n;
    }

    // Decodes an escape after its backslash, which names no Perl class. A letter or digit names no rune, except those
    // of the control characters.
    bool parseEscapedRune(uint32_t* out) {
        char c = peek();
        uint32_t control = 0xffffffffu;
        switch (c) {
            case 'n': control = '\n'; break;
            case 't': control = '\t'; break;
            case 'r': control = '\r'; break;
            case 'f': control = '\f'; break;
            case 'v': control = '\v'; break;
            case '0': control = 0; break;
            default: break;
        }
        if (control != 0xffffffffu) {
            ++at;
            *out = control;
            return true;
        }
        if (prAsciiIsDigit(c) || prAsciiIsLetter(c)) {
            fail(PrStatus_Syntax);
            return false;
        }
        *out = decodeRune();
        return true;
    }

    uint32_t parseClass() {
        ++at;
        bool negated = peek() == '^';
        if (negated)
            ++at;
        uint32_t first = program->ranges.count;
        // A `]` that comes first in a class is a member.
        for (bool member = false;; member = true) {
            if (done()) {
                fail(PrStatus_Syntax);
                return node(NodeKind_Empty);
            }
            if (peek() == ']' && member) {
                ++at;
                break;
            }
            if (peek() == '\\' && isPerlClass(peekAt(1))) {
                addPerlClass(peekAt(1));
                at += 2;
                continue;
            }
            uint32_t low = 0;
            if (!parseClassRune(&low))
                return node(NodeKind_Empty);
            uint32_t high = low;
            if (peek() == '-' && peekAt(1) != ']' && at + 1 < pattern.size) {
                ++at;
                if ((peek() == '\\' && isPerlClass(peekAt(1))) || !parseClassRune(&high) || high < low) {
                    fail(PrStatus_Syntax);
                    return node(NodeKind_Empty);
                }
            }
            program->ranges.push(RuneRange{low, high});
        }
        return classNode(first, negated);
    }

    bool parseClassRune(uint32_t* out) {
        if (peek() != '\\') {
            *out = decodeRune();
            return true;
        }
        ++at;
        if (done()) {
            fail(PrStatus_Syntax);
            return false;
        }
        return parseEscapedRune(out);
    }
};

// A compiler of the tree to steps, by the constructions of Thompson. Which way a split prefers is what makes a
// quantifier greedy or not. A run of concatenations or alternatives compiles in a loop, so that only the nesting of
// groups and quantifiers recurses.
struct Compiler {
    Program* program;
    const RawArray<Node>* nodes;
    RawArray<uint32_t> chain;
    PrStatus status;

    uint32_t here() const { return program->steps.count; }

    uint32_t emit(Step step) {
        if (program->steps.count >= kMaxSteps) {
            status = PrStatus_Limit;
            return here();
        }
        program->steps.push(step);
        return here() - 1;
    }

    void preferring(uint32_t split, uint32_t preferred, uint32_t other, bool greedy) {
        if (status != PrStatus_Ok)
            return;
        program->steps.data[split].x = greedy ? preferred : other;
        program->steps.data[split].y = greedy ? other : preferred;
    }

    void compile(uint32_t root) {
        emit(Step{.op = Op_Save, .x = 0});
        compileNode(root);
        emit(Step{.op = Op_Save, .x = 1});
        emit(Step{.op = Op_Match});
    }

    // Writes the operands of a left-leaning run of `kind` under `index` to `chain`, from `base`, in order.
    void flatten(uint32_t index, NodeKind kind, uint32_t base) {
        uint32_t at = index;
        while (nodes->data[at].kind == kind) {
            chain.push(nodes->data[at].b);
            at = nodes->data[at].a;
        }
        chain.push(at);
        for (uint32_t i = base, j = chain.count - 1; i < j; ++i, --j) {
            uint32_t swap = chain.data[i];
            chain.data[i] = chain.data[j];
            chain.data[j] = swap;
        }
    }

    void compileNode(uint32_t index) {
        if (status != PrStatus_Ok)
            return;
        Node n = nodes->data[index];
        switch (n.kind) {
            case NodeKind_Empty: break;
            case NodeKind_Rune: emit(Step{.op = Op_Rune, .rune = n.rune}); break;
            case NodeKind_Any: emit(Step{.op = Op_Any}); break;
            case NodeKind_Class: emit(Step{.op = Op_Class, .x = n.classIndex}); break;
            case NodeKind_Start: emit(Step{.op = Op_Start}); break;
            case NodeKind_End: emit(Step{.op = Op_End}); break;
            case NodeKind_Concat: {
                uint32_t base = chain.count;
                flatten(index, NodeKind_Concat, base);
                for (uint32_t i = base; i < chain.count; ++i)
                    compileNode(chain.data[i]);
                chain.count = base;
                break;
            }
            case NodeKind_Alternate: {
                // Each alternative but the last splits to itself, preferred, and to the alternatives after it, and all
                // jump to the end.
                uint32_t base = chain.count;
                flatten(index, NodeKind_Alternate, base);
                uint32_t count = chain.count - base;
                uint32_t jumps = chain.count;
                for (uint32_t i = 0; i < count; ++i) {
                    uint32_t split = 0;
                    if (i + 1 < count)
                        split = emit(Step{.op = Op_Split, .x = here() + 1});
                    compileNode(chain.data[base + i]);
                    if (i + 1 < count) {
                        chain.push(emit(Step{.op = Op_Jump}));
                        if (status == PrStatus_Ok)
                            program->steps.data[split].y = here();
                    }
                }
                for (uint32_t j = jumps; status == PrStatus_Ok && j < chain.count; ++j)
                    program->steps.data[chain.data[j]].x = here();
                chain.count = base;
                break;
            }
            case NodeKind_Star: compileStar(n.a, n.greedy); break;
            case NodeKind_Plus: {
                uint32_t enter = here();
                compileNode(n.a);
                uint32_t split = emit(Step{.op = Op_Split});
                preferring(split, enter, here(), n.greedy);
                break;
            }
            case NodeKind_Quest: compileQuest(n.a, n.greedy); break;
            case NodeKind_Repeat:
                // The required copies, then a star for no upper bound, or the optional copies.
                for (int32_t i = 0; i < n.minimum && status == PrStatus_Ok; ++i)
                    compileNode(n.a);
                if (n.maximum == -1) {
                    compileStar(n.a, n.greedy);
                } else {
                    for (int32_t i = n.minimum; i < n.maximum && status == PrStatus_Ok; ++i)
                        compileQuest(n.a, n.greedy);
                }
                break;
            case NodeKind_Group:
                emit(Step{.op = Op_Save, .x = 2 * n.group});
                compileNode(n.a);
                emit(Step{.op = Op_Save, .x = 2 * n.group + 1});
                break;
            default: PR_ASSERT(false); break;
        }
    }

    void compileStar(uint32_t child, bool greedy) {
        uint32_t split = emit(Step{.op = Op_Split});
        uint32_t enter = here();
        compileNode(child);
        emit(Step{.op = Op_Jump, .x = split});
        preferring(split, enter, here(), greedy);
    }

    void compileQuest(uint32_t child, bool greedy) {
        uint32_t split = emit(Step{.op = Op_Split});
        uint32_t enter = here();
        compileNode(child);
        preferring(split, enter, here(), greedy);
    }
};

// The threads at one offset of the text, in the order of preference, each with its slots, and the generation that last
// reached each step, so that a step takes one thread at an offset: the one that the pattern prefers.
struct Threads {
    RawArray<uint32_t> steps;
    RawArray<uint32_t> slots;          // `slotCount` for each thread, in turn
    RawArray<uint32_t> seen;           // by index of the program's steps
    uint32_t generation;
};

// An entry of the stack that follows the steps that take no code point: a step to visit, or a slot to restore once what
// follows a Save has been visited.
struct Visit {
    uint32_t step;
    uint32_t slot;                  // kSlotNone for a step to visit
    uint32_t value;
};

// A simulation of every thread of a program over a text at once.
struct Matcher {
    const Program* program;
    PrStringView text;
    bool whole;                     // the match starts at the start of the text and ends at its end
    uint32_t slotCount;
    Threads lists[2];
    RawArray<uint32_t> work;
    RawArray<uint32_t> best;
    RawArray<Visit> stack;
    bool matched;

    // Adds the threads that the steps from `start` reach at `offset` without taking a code point, with `slots`, in the
    // order of preference.
    void addThread(Threads& list, uint32_t start, uint32_t offset, uint32_t* slots) {
        stack.clear();
        stack.push(Visit{start, kSlotNone, 0});
        while (stack.count != 0) {
            Visit visit = stack.last();
            stack.pop();
            if (visit.slot != kSlotNone) {
                slots[visit.slot] = visit.value;
                continue;
            }
            uint32_t at = visit.step;
            if (list.seen.data[at] == list.generation)
                continue;
            list.seen.data[at] = list.generation;
            const Step& step = program->steps.data[at];
            switch (step.op) {
                case Op_Jump: stack.push(Visit{step.x, kSlotNone, 0}); break;
                case Op_Split:
                    // The preferred way is visited first, and the slots that it records are restored before the other.
                    stack.push(Visit{step.y, kSlotNone, 0});
                    stack.push(Visit{step.x, kSlotNone, 0});
                    break;
                case Op_Save:
                    stack.push(Visit{0, step.x, slots[step.x]});
                    slots[step.x] = offset;
                    stack.push(Visit{at + 1, kSlotNone, 0});
                    break;
                case Op_Start:
                    if (offset == 0)
                        stack.push(Visit{at + 1, kSlotNone, 0});
                    break;
                case Op_End:
                    if (offset == text.size)
                        stack.push(Visit{at + 1, kSlotNone, 0});
                    break;
                default:
                    list.steps.push(at);
                    list.slots.append(slots, slotCount);
                    break;
            }
        }
    }

    void addFresh(Threads& list, uint32_t offset) {
        for (uint32_t i = 0; i < slotCount; ++i)
            work.data[i] = kSlotNone;
        addThread(list, 0, offset, work.data);
    }

    bool classTakes(uint32_t index, uint32_t rune) const {
        const RuneClass& c = program->classes.data[index];
        bool inside = false;
        for (uint32_t i = 0; i < c.count && !inside; ++i) {
            const RuneRange& r = program->ranges.data[c.first + i];
            inside = rune >= r.low && rune <= r.high;
        }
        return inside != c.negated;
    }

    bool run() {
        uint32_t steps = program->steps.count;
        for (Threads& list : lists) {
            list.seen.resize(steps);
            for (uint32_t i = 0; i < steps; ++i)
                list.seen.data[i] = 0;
            list.generation = 0;
        }
        work.resize(slotCount);
        best.resize(slotCount);
        Threads* current = &lists[0];
        Threads* next = &lists[1];
        current->steps.clear();
        current->slots.clear();
        ++current->generation;
        addFresh(*current, 0);
        uint32_t offset = 0;
        uint32_t size = uint32_t(text.size);
        for (;;) {
            uint32_t rune = 0;
            uint32_t width = offset < size ? prUtf8Decode(text, offset, &rune) : 0;
            uint32_t after = offset + width;
            next->steps.clear();
            next->slots.clear();
            ++next->generation;
            for (uint32_t i = 0; i < current->steps.count; ++i) {
                const uint32_t* slots = current->slots.data + i * slotCount;
                const Step& step = program->steps.data[current->steps.data[i]];
                bool takes = false;
                if (step.op == Op_Match) {
                    if (!whole || offset == size) {
                        for (uint32_t s = 0; s < slotCount; ++s)
                            best.data[s] = slots[s];
                        matched = true;
                        // The threads after this one are less preferred, so none of them can win.
                        break;
                    }
                } else if (offset < size) {
                    if (step.op == Op_Rune)
                        takes = rune == step.rune;
                    else if (step.op == Op_Any)
                        takes = rune != '\n';
                    else if (step.op == Op_Class)
                        takes = classTakes(step.x, rune);
                }
                if (takes) {
                    for (uint32_t s = 0; s < slotCount; ++s)
                        work.data[s] = slots[s];
                    addThread(*next, current->steps.data[i] + 1, after, work.data);
                }
            }
            // A search may start at any offset, as the least preferred thread, until a match is found.
            if (!whole && !matched && offset < size)
                addFresh(*next, after);
            Threads* swap = current;
            current = next;
            next = swap;
            if (offset >= size || (current->steps.count == 0 && (matched || whole)))
                break;
            offset = after;
        }
        return matched;
    }
};

// Runs `program` over `text`, and writes the groups of a match to `out`.
bool matchProgram(const Program* program, PrStringView text, bool whole, PrRegexGroup* PR_NULLABLE out,
        uint32_t capacity) {
    PR_ASSERT(text.size < 0xffffffffu);
    Matcher m{};
    m.program = program;
    m.text = text;
    m.whole = whole;
    m.slotCount = 2 * program->groups;
    bool matched = m.run();
    for (uint32_t g = 0; matched && g < program->groups && g < capacity; ++g) {
        uint32_t start = m.best.data[2 * g];
        uint32_t end = m.best.data[2 * g + 1];
        bool set = start != kSlotNone && end != kSlotNone;
        out[g] = set ? PrRegexGroup{start, end} : PrRegexGroup{kPrRegexGroupNone, kPrRegexGroupNone};
    }
    for (Threads& list : m.lists) {
        list.steps.reset();
        list.slots.reset();
        list.seen.reset();
    }
    m.work.reset();
    m.best.reset();
    m.stack.reset();
    return matched;
}

void resetProgram(Program* program) {
    program->steps.reset();
    program->ranges.reset();
    program->classes.reset();
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrRegex : Program {};

PrStatus prRegexCreate(PrStringView pattern, PrRegex* PR_NULLABLE* PR_NONNULL out) {
    PrRegex* regex = allocZeroed<PrRegex>();
    regex->groups = 1;
    Parser parser{};
    parser.pattern = pattern;
    parser.program = regex;
    uint32_t root = parser.parse();
    PrStatus status = parser.status;
    if (status == PrStatus_Ok) {
        Compiler compiler{};
        compiler.program = regex;
        compiler.nodes = &parser.nodes;
        compiler.compile(root);
        status = compiler.status;
        compiler.chain.reset();
    }
    parser.nodes.reset();
    if (status != PrStatus_Ok) {
        resetProgram(regex);
        prMemoryFree(regex);
        *out = nullptr;
        return status;
    }
    *out = regex;
    return PrStatus_Ok;
}

void prRegexDestroy(PrRegex* PR_NULLABLE regex) {
    if (regex == nullptr)
        return;
    resetProgram(regex);
    prMemoryFree(regex);
}

uint32_t prRegexGroupCount(const PrRegex* regex) {
    return regex->groups;
}

bool prRegexSearch(const PrRegex* regex, PrStringView text, PrRegexGroup* PR_NULLABLE out, uint32_t capacity) {
    return matchProgram(regex, text, false, out, capacity);
}

bool prRegexMatch(const PrRegex* regex, PrStringView text, PrRegexGroup* PR_NULLABLE out, uint32_t capacity) {
    return matchProgram(regex, text, true, out, capacity);
}
