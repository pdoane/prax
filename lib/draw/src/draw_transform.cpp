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
        if (!isFiniteF32(value))
            return true;
    }
    double a = double(t.a);
    double b = double(t.b);
    double c = double(t.c);
    double d = double(t.d);
    double determinant = absF64(a * d - c * b);
    double squares = a * a + b * b + c * c + d * d;
    double largest = (squares + sqrtF64(maxF64(squares * squares - 4 * determinant * determinant, 0))) / 2;
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
    return floorF64(at + 0.5);
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
    next.degenerate = current->degenerate || !isFiniteF32(next.affine.e) || !isFiniteF32(next.affine.f);
    return next;
}

const PrDrawCommand* prDrawTransformSnap(const PrDrawTransform* transform, const PrDrawCommand* command,
                                         PrDrawSnapped* storage) {
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
    PrRect inner{};
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
            // Each side is as wide as its snapped outer and inner edges leave it.
            const PrDrawBorder& b = *command->border;
            PrRect exact = b.shape.rect;
            PrRect within{exact.left + b.sides[PrRectSide_Left].width, exact.top + b.sides[PrRectSide_Top].width,
                          exact.right - b.sides[PrRectSide_Right].width,
                          exact.bottom - b.sides[PrRectSide_Bottom].width};
            outer = snapRect(s, a, exact, axes);
            inner = snapRect(s, a, within, axes);
            moved = !sameRect(outer, exact) || !sameRect(inner, within);
            break;
        }
        default:
            return command;
    }
    // An image brush's tile in the command's space snaps its start, and its far edge where it does not repeat.
    const PrDrawBrush* brush = command->brush;
    PrRect tile{};
    bool tiled = brush != nullptr && brush->kind == PrDrawBrushKind_Image && brush->space == PrDrawBrushSpace_User
                 && brush->transform.a == 1 && brush->transform.b == 0 && brush->transform.c == 0
                 && brush->transform.d == 1 && brush->transform.e == 0 && brush->transform.f == 0;
    if (tiled) {
        const PrRect& exact = brush->image.tiles.tile;
        PrRect snapped = snapRect(s, a, exact, axes);
        bool padX = brush->image.tiles.x == PrDrawExtend_Pad;
        bool padY = brush->image.tiles.y == PrDrawExtend_Pad;
        tile = PrRect{snapped.left, snapped.top, padX ? snapped.right : snapped.left + (exact.right - exact.left),
                      padY ? snapped.bottom : snapped.top + (exact.bottom - exact.top)};
        tiled = !sameRect(tile, exact);
        moved = moved || tiled;
    }
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
            storage->border.sides[PrRectSide_Top].width = inner.top - outer.top;
            storage->border.sides[PrRectSide_Right].width = outer.right - inner.right;
            storage->border.sides[PrRectSide_Bottom].width = outer.bottom - inner.bottom;
            storage->border.sides[PrRectSide_Left].width = inner.left - outer.left;
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
            return false;
        default:
            return true;
    }
}
