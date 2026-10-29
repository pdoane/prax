#include "layout/src/layout_value.h"

#include "pr/float.h"
#include "pr/hash.h"
#include "pr/mem.h"
#include "pr/memory.h"

namespace pr_impl {

namespace {

// The seed that compounds are hashed with.
constexpr uint64_t kCompoundSeed = 0x6a09e667f3bcc909ull;

// The value of a calculation's node, in pixels raised to its power of length (css-values-4/#calc-type-checking). The
// power is 0 for a number, 1 for a length, which a percentage resolves to, and any other value that a product or an
// inversion makes. `none` is a bound that Clamp leaves out.
struct CalcValue {
    double value;
    int32_t power;
    bool none;
};

// A walk of a calculation's nodes that evaluates them against `base` pixels, and notes whether they read the base.
struct Calc {
    const PrLayoutCalcNode* nodes;
    uint32_t count;
    uint32_t at;                    // the next node
    double base;
    bool percent;                   // true if a percentage was read

    // Evaluates the node at `at` and its operands, and moves past them. Returns false if the calculation is malformed
    // or its types do not add up.
    bool next(CalcValue* out) {
        if (at >= count)
            return false;
        const PrLayoutCalcNode& node = nodes[at++];
        if (node.op == PrLayoutCalcOp_Value)
            return leaf(node, out);
        uint32_t operands = node.count;
        CalcValue v[3] = {};
        int32_t power = 0;          // the operands' power of length so far
        switch (node.op) {
            case PrLayoutCalcOp_Sum:
            case PrLayoutCalcOp_Min:
            case PrLayoutCalcOp_Max:
            case PrLayoutCalcOp_Hypot: {
                if (operands == 0)
                    return false;
                double acc = 0;
                for (uint32_t i = 0; i < operands; ++i) {
                    CalcValue x{};
                    if (!next(&x) || x.none || (i != 0 && x.power != power))
                        return false;
                    power = x.power;
                    switch (node.op) {
                        case PrLayoutCalcOp_Sum:    acc = i == 0 ? x.value : acc + x.value; break;
                        case PrLayoutCalcOp_Min:    acc = i == 0 ? x.value : minimumF64(acc, x.value); break;
                        case PrLayoutCalcOp_Max:    acc = i == 0 ? x.value : maximumF64(acc, x.value); break;
                        default:
                            // Any NaN makes NaN (css-values-4/#calc-ieee), whereas hypot would let an infinity win.
                            acc = i == 0 ? absF64(x.value)
                                  : isNanF64(acc) || isNanF64(x.value) ? kNanF64
                                                                       : hypotF64(acc, x.value);
                            break;
                    }
                }
                *out = CalcValue{acc, power, false};
                return true;
            }
            case PrLayoutCalcOp_Product: {
                if (operands == 0)
                    return false;
                double acc = 1;
                for (uint32_t i = 0; i < operands; ++i) {
                    CalcValue x{};
                    if (!next(&x) || x.none)
                        return false;
                    power += x.power;
                    acc *= x.value;
                }
                *out = CalcValue{acc, power, false};
                return true;
            }
            case PrLayoutCalcOp_Clamp: {
                if (operands != 3)
                    return false;
                for (CalcValue& x : v) {
                    if (!next(&x))
                        return false;
                }
                if (v[1].none || (!v[0].none && v[0].power != v[1].power) || (!v[2].none && v[2].power != v[1].power))
                    return false;
                double x = v[1].value;
                if (!v[2].none)
                    x = minimumF64(x, v[2].value);
                if (!v[0].none)
                    x = maximumF64(v[0].value, x);
                *out = CalcValue{x, v[1].power, false};
                return true;
            }
            case PrLayoutCalcOp_RoundNearest:
            case PrLayoutCalcOp_RoundUp:
            case PrLayoutCalcOp_RoundDown:
            case PrLayoutCalcOp_RoundToZero:
            case PrLayoutCalcOp_Mod:
            case PrLayoutCalcOp_Rem: {
                if (operands != 2 || !next(&v[0]) || !next(&v[1]) || v[0].none || v[1].none
                    || v[0].power != v[1].power)
                    return false;
                double a = v[0].value;
                double b = v[1].value;
                double x = 0;
                switch (node.op) {
                    case PrLayoutCalcOp_RoundUp:     x = roundToMultipleF64<RoundMode_Up>(a, b); break;
                    case PrLayoutCalcOp_RoundDown:   x = roundToMultipleF64<RoundMode_Down>(a, b); break;
                    case PrLayoutCalcOp_RoundToZero: x = roundToMultipleF64<RoundMode_TowardZero>(a, b); break;
                    case PrLayoutCalcOp_Mod:         x = floorModF64(a, b); break;
                    case PrLayoutCalcOp_Rem:         x = fmodF64(a, b); break;
                    default:                         x = roundToMultipleF64<RoundMode_Nearest>(a, b); break;
                }
                *out = CalcValue{x, v[0].power, false};
                return true;
            }
            case PrLayoutCalcOp_Negate:
            case PrLayoutCalcOp_Invert:
            case PrLayoutCalcOp_Abs:
            case PrLayoutCalcOp_Sign: {
                if (operands != 1 || !next(&v[0]) || v[0].none)
                    return false;
                double x = v[0].value;
                switch (node.op) {
                    case PrLayoutCalcOp_Negate:
                        *out = CalcValue{-x, v[0].power, false};
                        return true;
                    case PrLayoutCalcOp_Invert:
                        *out = CalcValue{1 / x, -v[0].power, false};
                        return true;
                    case PrLayoutCalcOp_Abs:
                        *out = CalcValue{absF64(x), v[0].power, false};
                        return true;
                    default:
                        *out = CalcValue{x > 0 ? 1.0 : x < 0 ? -1.0 : x, 0, false};
                        return true;
                }
            }
            default:
                return false;
        }
    }

    bool leaf(const PrLayoutCalcNode& node, CalcValue* out) {
        switch (node.leaf) {
            case PrLayoutCalcLeaf_Px:
                *out = CalcValue{node.value, 1, false};
                return true;
            case PrLayoutCalcLeaf_Percent:
                percent = true;
                *out = CalcValue{base * node.value / 100, 1, false};
                return true;
            case PrLayoutCalcLeaf_Number:
                *out = CalcValue{node.value, 0, false};
                return true;
            case PrLayoutCalcLeaf_None:
                *out = CalcValue{0, 0, true};
                return true;
            default:
                return false;
        }
    }
};

// Evaluates a whole calculation against `base`. Returns false if it is not a length.
bool evaluate(const PrLayoutCalcNode* nodes, uint32_t count, double base, double* out, bool* percent) {
    Calc calc{.nodes = nodes, .count = count, .at = 0, .base = base, .percent = false};
    CalcValue value{};
    if (!calc.next(&value) || calc.at != count || value.power != 1 || value.none)
        return false;
    *out = value.value;
    *percent = calc.percent;
    return true;
}

// Returns true if `percent` is a percentage that a word holds exactly.
bool fitsPercent(double percent) {
    double fixed = percent * 65536;
    return fixed >= double(kPrLayoutWordPercentMin) && fixed <= double(kPrLayoutWordPercentMax)
           && fixed == floorF64(fixed);
}

uint32_t hashNodes(const PrLayoutCalcNode* nodes, uint32_t count) {
    return uint32_t(hashBytes(nodes, sizeof(PrLayoutCalcNode) * count, kCompoundSeed));
}

} // namespace

PrLayoutLength layoutCompoundsIntern(LayoutCompounds* compounds, const PrLayoutCalcNode* nodes, uint32_t count) {
    double px = 0;
    bool percent = false;
    if (count == 0 || !evaluate(nodes, count, 0, &px, &percent))
        return prLayoutLengthAuto();
    // NaN propagates through every operation, so a calculation with a NaN leaf is NaN against any base, and a NaN
    // result is 0 (css-values-4/#calc-ieee). A compound holds no NaN. Another calculation can make NaN at some bases,
    // as an infinite percentage does at 0, and resolves to 0 at those bases.
    for (uint32_t i = 0; i < count; ++i) {
        if (nodes[i].op == PrLayoutCalcOp_Value && isNanF64(nodes[i].value))
            return prLayoutLengthUnits(PrLayoutUnit{0});
    }
    // Pixels alone compute now, and a lone percentage becomes its word if the word can hold it exactly.
    if (!percent)
        return prLayoutLengthUnits(prLayoutUnitFromPxDouble(px));
    if (count == 1 && fitsPercent(nodes[0].value))
        return prLayoutLengthPercent(float(nodes[0].value));
#if !defined(NDEBUG)
    for (uint32_t i = 0; i < count; ++i) {
        const PrLayoutCalcNode& node = nodes[i];
        PrLayoutCalcNode made = node.op == PrLayoutCalcOp_Value ? prLayoutCalcLeaf(node.leaf, node.value)
                                                                : prLayoutCalcOperation(node.op, node.count);
        PR_ASSERT(memEquals(&made, &node, sizeof(PrLayoutCalcNode)));
    }
#endif
    uint32_t hash = hashNodes(nodes, count);
    uint32_t found = compounds->index.find(hash, [&](uint32_t i) {
        const LayoutCompound& c = compounds->entries.data[i];
        return c.count == count && memEquals(c.nodes, nodes, sizeof(PrLayoutCalcNode) * count);
    });
    if (found != kHashMissing)
        return PrLayoutLength{prLayoutWordCompound(found)};
    uint32_t at = 0;
    if (compounds->free.count != 0) {
        at = compounds->free.last();
        compounds->free.pop();
    } else {
        at = compounds->entries.count;
        compounds->entries.push(LayoutCompound{});
    }
    PrLayoutCalcNode* copy = static_cast<PrLayoutCalcNode*>(prMemoryAlloc(sizeof(PrLayoutCalcNode) * count));
    memCopy(copy, nodes, sizeof(PrLayoutCalcNode) * count);
    compounds->entries.data[at] = LayoutCompound{copy, count, hash, 0};
    compounds->index.insert(hash, at);
    compounds->candidates.push(at);
    return PrLayoutLength{prLayoutWordCompound(at)};
}

PrLayoutUnit layoutCompoundResolve(const LayoutCompounds* compounds, uint32_t index, PrLayoutUnit base) {
    const LayoutCompound& c = compounds->entries.data[index];
    double px = 0;
    bool percent = false;
    if (!evaluate(c.nodes, c.count, prLayoutUnitToPxDouble(base), &px, &percent))
        return PrLayoutUnit{0};
    return prLayoutUnitFromPxDouble(px);
}

void layoutCompoundsCount(LayoutCompounds* compounds, uint32_t word, uint32_t delta) {
    if (prLayoutWordKind(word) != PrLayoutWordKind_Compound)
        return;
    uint32_t at = prLayoutWordCompoundIndex(word);
    LayoutCompound& c = compounds->entries.data[at];
    c.references += delta;
    if (c.references == 0)
        compounds->candidates.push(at);
}

void layoutCompoundsCollect(LayoutCompounds* compounds) {
    for (uint32_t k = 0; k < compounds->candidates.count; ++k) {
        uint32_t i = compounds->candidates.data[k];
        LayoutCompound& c = compounds->entries.data[i];
        if (c.nodes == nullptr || c.references != 0)
            continue;
        compounds->index.erase(c.hash, i);
        prMemoryFree(c.nodes);
        c = LayoutCompound{};
        compounds->free.push(i);
    }
    compounds->candidates.clear();
}

bool layoutCompoundsVerify(const LayoutCompounds* compounds) {
    return layoutCandidatesCover(compounds->candidates, compounds->entries.count, [&](uint32_t i) {
        const LayoutCompound& c = compounds->entries.data[i];
        return c.nodes != nullptr && c.references == 0;
    });
}

void layoutCompoundsReset(LayoutCompounds* compounds) {
    for (uint32_t i = 0; i < compounds->entries.count; ++i)
        prMemoryFree(compounds->entries.data[i].nodes);
    compounds->entries.reset();
    compounds->free.reset();
    compounds->candidates.reset();
    compounds->index.reset();
}

} // namespace pr_impl
