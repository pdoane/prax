// Transform Functions
//
// css-transforms-2 - https://drafts.csswg.org/css-transforms-2/

#include "css/src/css_transform.h"

#include "pr/float.h"

#include "css/src/css_keyword.h"
#include "css/src/css_math.h"
#include "dom/src/dom_geometry.h"

namespace pr_impl {
namespace {

// Reads an argument of a transform function as an absolute value: a number, a length in pixels or an angle in degrees.
// Returns false for a percentage, a length that is not a fixed number of pixels, and a calculation that is none of
// those.
bool absolute(CssValueRun run, uint32_t index, double* out) {
    const CssValueNode* value = &run.values[index];
    switch (value->kind) {
        case PrCssValueKind_Number:
        case PrCssValueKind_Integer:
            *out = value->number;
            return true;
        case PrCssValueKind_Dimension: {
            double scale = cssUnitScale(value->unit);
            *out = value->number * scale;
            return scale != 0;
        }
        case PrCssValueKind_Math:
            return cssMathTo(run, index, PrCssUnit_Number, out) || cssMathTo(run, index, PrCssUnit_Px, out) ||
                cssMathTo(run, index, PrCssUnit_Deg, out);
        default:
            return false;
    }
}

} // namespace

bool cssTransformMatrix(CssValueRun function, PrDomMatrix* out) {
    const CssValueNode* node = function.values;
    if (node->kind != PrCssValueKind_Function)
        return false;
    double args[16] = {};
    uint32_t count = 0;
    for (uint32_t i = 1; i < node->next; i += function.values[i].next) {
        if (function.values[i].kind == PrCssValueKind_Literal)
            continue;
        // perspective(none) is the identity.
        if (cssValueIsKeyword(&function.values[i], PrCssKeyword_None) &&
                cssValueKeyword(node) == PrCssKeyword_Perspective) {
            *out = prDomMatrixIdentity();
            out->is2D = false;
            return true;
        }
        if (count == 16 || !absolute(function, i, &args[count]))
            return false;
        ++count;
    }
    PrDomMatrix next = prDomMatrixIdentity();
    switch (cssValueKeyword(node)) {
        case PrCssKeyword_Matrix:
        case PrCssKeyword_Matrix3d:
            if (prDomMatrixFromValues(args, count, &next) != PrStatus_Ok)
                return false;
            break;
        case PrCssKeyword_Translate:
        case PrCssKeyword_Translate3d:
            next.m[12] = args[0];
            next.m[13] = count > 1 ? args[1] : 0;
            next.m[14] = count > 2 ? args[2] : 0;
            next.is2D = cssValueKeyword(node) != PrCssKeyword_Translate3d;
            break;
        case PrCssKeyword_Translatex: next.m[12] = args[0]; break;
        case PrCssKeyword_Translatey: next.m[13] = args[0]; break;
        case PrCssKeyword_Translatez:
            next.m[14] = args[0];
            next.is2D = false;
            break;
        case PrCssKeyword_Scale:
        case PrCssKeyword_Scale3d:
            next.m[0] = args[0];
            next.m[5] = count > 1 ? args[1] : args[0];
            next.m[10] = count > 2 ? args[2] : 1;
            next.is2D = cssValueKeyword(node) != PrCssKeyword_Scale3d;
            break;
        case PrCssKeyword_Scalex: next.m[0] = args[0]; break;
        case PrCssKeyword_Scaley: next.m[5] = args[0]; break;
        case PrCssKeyword_Scalez:
            next.m[10] = args[0];
            next.is2D = false;
            break;
        case PrCssKeyword_Rotate:
        case PrCssKeyword_Rotatez: next = domMatrixRotation(0, 0, 1, degreesToRadiansF64(args[0])); break;
        case PrCssKeyword_Rotatex: next = domMatrixRotation(1, 0, 0, degreesToRadiansF64(args[0])); break;
        case PrCssKeyword_Rotatey: next = domMatrixRotation(0, 1, 0, degreesToRadiansF64(args[0])); break;
        case PrCssKeyword_Rotate3d:
            next = domMatrixRotation(args[0], args[1], args[2], degreesToRadiansF64(args[3]));
            next.is2D = false;
            break;
        case PrCssKeyword_Skew:
            next.m[4] = tanF64(degreesToRadiansF64(args[0]));
            next.m[1] = tanF64(degreesToRadiansF64(args[1]));
            break;
        case PrCssKeyword_Skewx: next.m[4] = tanF64(degreesToRadiansF64(args[0])); break;
        case PrCssKeyword_Skewy: next.m[1] = tanF64(degreesToRadiansF64(args[0])); break;
        case PrCssKeyword_Perspective:
            next.m[11] = -1 / (args[0] < 1 ? 1 : args[0]);
            next.is2D = false;
            break;
        default: return false;
    }
    *out = next;
    return true;
}

} // namespace pr_impl
