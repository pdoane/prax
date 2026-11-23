// Transforms during a backend's walk of a list.

#include "pr/draw_transform.h"

#include "pr/float.h"
#include "pr/vec.h"

namespace pr_impl {

namespace {

// The minimum ratio of the smallest to the largest factor by which a transform scales a length near a point. Below it,
// the transform collapses. A rotation or uniform scale keeps the ratio at 1.
constexpr double kCollapse = 1e-6;

// Returns true if the transform collapses the plane or is not finite. It collapses if its smallest scale is a vanishing
// fraction of its largest. The test is of the transform in floats, which maps geometry.
bool collapses(const PrAffine2& t) {
    const float values[6] = {t.a, t.b, t.c, t.d, t.e, t.f};
    for (float value : values) {
        if (!prF32IsFinite(value))
            return true;
    }
    double a = double(t.a);
    double b = double(t.b);
    double c = double(t.c);
    double d = double(t.d);
    double determinant = prF64Abs(a * d - c * b);
    double squares = a * a + b * b + c * c + d * d;
    double largest = (squares + prF64Sqrt(prF64Max(squares * squares - 4 * determinant * determinant, 0))) / 2;
    return !(largest > 0) || determinant <= kCollapse * largest;
}

// How a snap space maps the axes of its space onto the target's: each onto one of the target's axes, either way round,
// or not.
enum {
    Alignment_None,
    Alignment_Straight,         // x onto x and y onto y
    Alignment_Swapped,          // x onto y and y onto x
};
typedef uint8_t Alignment;

Alignment alignmentOf(const PrDAffine2& s) {
    if (s.b == 0 && s.c == 0 && s.a != 0 && s.d != 0)
        return Alignment_Straight;
    if (s.a == 0 && s.d == 0 && s.b != 0 && s.c != 0)
        return Alignment_Swapped;
    return Alignment_None;
}

// Returns `at` rounded to the nearest whole number, half up.
double roundHalfUp(double at) {
    return prF64Floor(at + 0.5);
}

// Returns the coordinate `x` along x, or `y` along y, of the snap space `s` with alignment `a`, moved to where it lies
// on the nearest target pixel.
double snapX(const PrDAffine2& s, Alignment a, double x) {
    if (a == Alignment_Straight)
        return (roundHalfUp(s.a * x + s.e) - s.e) / s.a;
    return (roundHalfUp(s.b * x + s.f) - s.f) / s.b;
}
double snapY(const PrDAffine2& s, Alignment a, double y) {
    if (a == Alignment_Straight)
        return (roundHalfUp(s.d * y + s.f) - s.f) / s.d;
    return (roundHalfUp(s.c * y + s.e) - s.e) / s.c;
}

// Returns true if two rects have the same edges.
bool sameRect(PrRect a, PrRect b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// Returns `width` rounded to a whole number of target pixels, and to at least one target pixel if `width` is positive.
// `scale` is the number of target pixels per unit along the axis of `width`. The result is in units.
float snapWidth(float width, double scale) {
    if (width <= 0 || scale <= 0)
        return width;
    double pixels = roundHalfUp(double(width) * scale);
    return float((pixels < 1 ? 1 : pixels) / scale);
}

PrRect snapRect(const PrDAffine2& s, Alignment a, PrRect r, PrDrawSnapFlags axes) {
    if ((axes & PrDrawSnapFlag_X) != 0) {
        r.left = float(snapX(s, a, double(r.left)));
        r.right = float(snapX(s, a, double(r.right)));
    }
    if ((axes & PrDrawSnapFlag_Y) != 0) {
        r.top = float(snapY(s, a, double(r.top)));
        r.bottom = float(snapY(s, a, double(r.bottom)));
    }
    return r;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrDrawTransform prDrawTransformFromAffine2(PrAffine2 transform) {
    return prDrawTransformFromDAffine2(prDAffine2FromAffine2(transform));
}

PrDrawTransform prDrawTransformFromDAffine2(PrDAffine2 transform) {
    PrAffine2 affine = prAffine2FromDAffine2(transform);
    return PrDrawTransform{.daffine = transform, .affine = affine, .snap = transform, .degenerate = collapses(affine)};
}

PrDrawTransform prDrawTransformPush(const PrDrawTransform* current, const PrDrawCommand* push) {
    PrDAffine2 affine = push->affine;
    // A snapped origin moves to where the snap space puts it on a target pixel, and a snapped rectangle stretches to
    // its rounded edges.
    Alignment alignment = alignmentOf(current->snap);
    if (alignment != Alignment_None && (push->snap & PrDrawSnapFlag_Rect) != 0 && affine.b == 0 && affine.c == 0) {
        double left = snapX(current->snap, alignment, affine.e);
        double top = snapY(current->snap, alignment, affine.f);
        double right = snapX(current->snap, alignment, affine.e + affine.a * double(push->snapExtent.x));
        double bottom = snapY(current->snap, alignment, affine.f + affine.d * double(push->snapExtent.y));
        if (push->snapExtent.x > 0)
            affine.a = (right - left) / double(push->snapExtent.x);
        if (push->snapExtent.y > 0)
            affine.d = (bottom - top) / double(push->snapExtent.y);
        affine.e = left;
        affine.f = top;
    } else if (alignment != Alignment_None) {
        if ((push->snap & PrDrawSnapFlag_X) != 0)
            affine.e = snapX(current->snap, alignment, affine.e);
        if ((push->snap & PrDrawSnapFlag_Y) != 0)
            affine.f = snapY(current->snap, alignment, affine.f);
    }
    bool apart = (push->snap & PrDrawSnapFlag_Apart) != 0;
    if (affine.a == 1 && affine.b == 0 && affine.c == 0 && affine.d == 1) {
        PrDrawTransform next = prDrawTransformMoved(current, PrDVec2{affine.e, affine.f});
        if (apart)
            next.snap = current->snap;
        return next;
    }
    PrDrawTransform next = prDrawTransformFromDAffine2(prDAffine2Concat(affine, current->daffine));
    next.snap = apart ? current->snap : prDAffine2Concat(affine, current->snap);
    next.degenerate = next.degenerate || current->degenerate;
    return next;
}

PrDrawTransform prDrawTransformMoved(const PrDrawTransform* current, PrDVec2 offset) {
    // A translation moves the plane without changing how it collapses, unless it is not finite.
    PrDAffine2 m = current->daffine;
    PrDrawTransform next = *current;
    next.daffine.e = m.a * offset.x + m.c * offset.y + m.e;
    next.daffine.f = m.b * offset.x + m.d * offset.y + m.f;
    next.affine.e = float(next.daffine.e);
    next.affine.f = float(next.daffine.f);
    PrDAffine2 s = current->snap;
    next.snap.e = s.a * offset.x + s.c * offset.y + s.e;
    next.snap.f = s.b * offset.x + s.d * offset.y + s.f;
    next.degenerate = current->degenerate || !prF32IsFinite(next.affine.e) || !prF32IsFinite(next.affine.f);
    return next;
}

const PrDrawCommand* prDrawTransformSnap(const PrDrawTransform* transform, const PrDrawCommand* command, PrDrawSnapped* storage) {
    if (command->snap == 0 || command->kind == PrDrawCommandKind_PushTransformAffine2)
        return command;
    const PrDAffine2& s = transform->snap;
    Alignment a = alignmentOf(s);
    if (a == Alignment_None)
        return command;
    PrDrawSnapFlags axes = command->snap;
    // Geometry already on target pixels, as in a UI of whole pixels, is played as it is, with no copy.
    bool moved = false;
    PrRect rect{};
    PrRect outer{};
    float widths[PrRectSide_Count] = {};
    switch (command->kind) {
        case PrDrawCommandKind_Rect:
        case PrDrawCommandKind_PushClipRect:
            rect = snapRect(s, a, command->rect, axes);
            moved = !sameRect(rect, command->rect);
            break;
        case PrDrawCommandKind_CornerRect:
        case PrDrawCommandKind_PushClipCornerRect:
            rect = snapRect(s, a, command->cornerRect->rect, axes);
            moved = !sameRect(rect, command->cornerRect->rect);
            break;
        case PrDrawCommandKind_Border: {
            // The outer edges snap, and each side keeps its width from its edge in whole target pixels, so that a side
            // as wide as a target pixel stays one wherever the border lands. An inner edge snapped on its own could
            // round to the same target pixel as its outer edge, and leave the side no width.
            const PrDrawBorder& b = *command->border;
            outer = snapRect(s, a, b.shape.rect, axes);
            moved = !sameRect(outer, b.shape.rect);
            double scaleX = prF64Abs(a == Alignment_Straight ? s.a : s.b);
            double scaleY = prF64Abs(a == Alignment_Straight ? s.d : s.c);
            for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
                bool horizontal = side == PrRectSide_Left || side == PrRectSide_Right;
                float width = b.sides[side].width;
                if ((axes & (horizontal ? PrDrawSnapFlag_X : PrDrawSnapFlag_Y)) != 0)
                    width = snapWidth(width, horizontal ? scaleX : scaleY);
                widths[side] = width;
                moved = moved || width != b.sides[side].width;
            }
            break;
        }
        default:
            return command;
    }
    // The tile of an image brush in the command's space snaps at its start. Its far edge snaps too along an axis that
    // pads, and otherwise the tile keeps its size. The tile snaps where the brush's transform places it, if the
    // transform only moves or mirrors the tile or turns it by quarter turns. The brush of an upright image has such a
    // transform (prDrawBrushImageUpright).
    const PrDrawBrush* brush = command->brush;
    PrRect tile{};
    bool tiled = brush != nullptr && brush->kind == PrDrawBrushKind_Image && brush->space == PrDrawBrushSpace_User;
    const PrAffine2 t = tiled ? brush->transform : prAffine2Identity();
    bool straight = t.b == 0 && t.c == 0 && prF32Abs(t.a) == 1 && prF32Abs(t.d) == 1;
    bool turned = t.a == 0 && t.d == 0 && prF32Abs(t.b) == 1 && prF32Abs(t.c) == 1;
    PrAffine2 back{};
    tiled = tiled && (straight || turned) && prAffine2Invert(t, &back);
    if (tiled) {
        const PrRect& exact = brush->image.tiles.tile;
        PrRect placed = prAffine2MapRect(t, exact);
        PrRect snapped = snapRect(s, a, placed, axes);
        bool padX = (turned ? brush->image.tiles.y : brush->image.tiles.x) == PrDrawExtend_Pad;
        bool padY = (turned ? brush->image.tiles.x : brush->image.tiles.y) == PrDrawExtend_Pad;
        PrRect shifted{
            snapped.left, snapped.top, padX ? snapped.right : snapped.left + (placed.right - placed.left),
            padY ? snapped.bottom : snapped.top + (placed.bottom - placed.top),
        };
        tile = sameRect(shifted, placed) ? exact : prAffine2MapRect(back, shifted);
        tiled = !sameRect(tile, exact);
    }
    moved = moved || tiled;
    if (!moved)
        return command;
    storage->command = *command;
    PrDrawCommand& c = storage->command;
    switch (command->kind) {
        case PrDrawCommandKind_Rect:
        case PrDrawCommandKind_PushClipRect:
            c.rect = rect;
            break;
        case PrDrawCommandKind_CornerRect:
        case PrDrawCommandKind_PushClipCornerRect:
            storage->cornerRect = *command->cornerRect;
            storage->cornerRect.rect = rect;
            c.cornerRect = &storage->cornerRect;
            break;
        default:
            storage->border = *command->border;
            storage->border.shape.rect = outer;
            for (uint32_t side = 0; side < PrRectSide_Count; ++side)
                storage->border.sides[side].width = widths[side];
            c.border = &storage->border;
            break;
    }
    if (tiled) {
        storage->brush = *brush;
        storage->brush.image.tiles.tile = tile;
        c.brush = &storage->brush;
    }
    return &c;
}

bool prDrawTransformDrawsNothing(const PrDrawTransform* transform, const PrDrawCommand* command) {
    return transform->degenerate && prDrawCommandSkipsCollapsed(command);
}

bool prDrawAffine2Collapses(PrAffine2 transform) {
    return prDrawTransformFromAffine2(transform).degenerate;
}

bool prDrawCommandSkipsCollapsed(const PrDrawCommand* command) {
    switch (command->kind) {
        case PrDrawCommandKind_PushTransformAffine2:
        case PrDrawCommandKind_PopTransform:
        case PrDrawCommandKind_PopClip:
        case PrDrawCommandKind_PopGroup:
        case PrDrawCommandKind_PushMark:
        case PrDrawCommandKind_PopMark:
            return false;
        default:
            return true;
    }
}
