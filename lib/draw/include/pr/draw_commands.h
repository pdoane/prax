// Draw commands: the contents of a recorded list, in the form that a backend reads.
//
// A backend walks a list's commands in order and switches on the kind of each one. draw_list.h records the commands and
// says what they mean.

#pragma once

#include "pr/base.h"
#include "pr/draw_list.h"
#include "pr/string.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The kinds of command. Each recording function of draw_list.h records the kind that matches its name.
enum {
    PrDrawCommandKind_Clear,        // replaces the target within the clip with the color
    PrDrawCommandKind_Rect,
    PrDrawCommandKind_CornerRect,
    PrDrawCommandKind_Ellipse,
    PrDrawCommandKind_Sector,
    PrDrawCommandKind_Capsule,
    PrDrawCommandKind_Line,         // stroked only
    PrDrawCommandKind_Polyline,
    PrDrawCommandKind_Path,
    PrDrawCommandKind_Border,
    PrDrawCommandKind_DropShadow,
    PrDrawCommandKind_InnerShadow,
    PrDrawCommandKind_Decoration,
    PrDrawCommandKind_NineSlice,
    PrDrawCommandKind_WritePixels,
    PrDrawCommandKind_Glyphs,
    PrDrawCommandKind_Embed,
    PrDrawCommandKind_PushTransformAffine2,
    PrDrawCommandKind_PopTransform,
    PrDrawCommandKind_PushClipRect,
    PrDrawCommandKind_PushClipTargetRect,
    PrDrawCommandKind_PushClipCornerRect,
    PrDrawCommandKind_PushClipPath,
    PrDrawCommandKind_PopClip,
    PrDrawCommandKind_PushGroup,
    PrDrawCommandKind_PopGroup,
    PrDrawCommandKind_Count,
};
typedef uint8_t PrDrawCommandKind;

// A command. Its kind decides which fields it uses. A painting command draws geometry of its own, and paints its brush
// times its color, component by component, or its color alone if it has no brush. The color of a command recorded with
// a brush is white at the brush's opacity, so a backend reads the opacity there. The source of a nine-slice takes the
// place of its brush. A command with a `stroke` field fills if the stroke is null, and strokes otherwise. The data that
// a command points to stays valid until the list changes.
typedef struct PrDrawCommand {
    PrDrawCommandKind kind;
    PrDrawComposite composite;                  // painting commands
    PrDrawBlendMode blendMode;                  // painting commands
    PrPathFillRule rule;                        // Polyline, Path, PushClipPath
    bool antialias;                             // painting commands and clips
    PrDrawSnapFlags snap;                       // painting commands and clips: the axes whose edges snap. Transform
                                                // pushes: how the transform snaps (PrDrawSnapFlags)
    PrDrawSampling sampling;                    // NineSlice
    PrColor16 color;                            // painting commands and Clear: straight alpha
    const PrDrawStroke* PR_NULLABLE stroke;     // shapes, Glyphs: null to fill
    const PrDrawBrush* PR_NULLABLE brush;       // painting commands except NineSlice: null for the color alone
    union {
        PrRect rect;                            // Rect, PushClipRect, PushClipTargetRect
        const PrCornerRect* cornerRect;         // CornerRect, PushClipCornerRect
        struct {
            PrVec2 center;
            PrVec2 radii;
            float rotation;                     // radians, clockwise where y points down
        } ellipse;                              // Ellipse
        struct {
            PrVec2 center;
            float innerRadius;
            float outerRadius;
            float startAngle;                   // radians clockwise from +x
            float sweep;
        } sector;                               // Sector
        struct {
            PrVec2 start;
            PrVec2 end;
            float radius;
        } capsule;                              // Capsule
        struct {
            PrVec2 start;
            PrVec2 end;
        } line;                                 // Line
        const PrPathPolyline* polyline;         // Polyline
        const PrPath* path;                     // Path, PushClipPath
        const PrDrawBorder* border;             // Border
        const PrDrawShadow* shadow;             // DropShadow, InnerShadow
        const PrDrawDecoration* decoration;     // Decoration
        struct {
            PrDrawSource source;
            PrRect destination;
            const PrDrawNineSlice* slices;
        } nineSlice;                            // NineSlice
        struct {
            const PrDrawImage* image;
            PrIVec2 origin;                  // the target point of the image's top left
        } writePixels;                          // WritePixels
        const PrFontGlyphRun* glyphs;           // Glyphs
        struct {
            const PrDrawList* list;
            PrDVec2 offset;                     // applied before the current transform
        } embed;                                // Embed
        struct {
            PrDAffine2 affine;                  // applied before the current transform
            PrVec2 snapExtent;                  // the rectangle that a PrDrawSnapFlag_Rect push snaps, from the origin
        };                                      // PushTransformAffine2
        const PrDrawGroup* group;               // PushGroup
    };
} PrDrawCommand;

// Returns the commands in recording order. They stay valid until the list changes.
uint32_t prDrawListCommandCount(const PrDrawList* list);
const PrDrawCommand* PR_NULLABLE prDrawListCommands(const PrDrawList* list);

// Returns the index of the PopGroup that closes the PushGroup at `push` in `commands`, or `count` if none closes it.
uint32_t prDrawCommandGroupEnd(const PrDrawCommand* commands, uint32_t count, uint32_t push);

// Returns the commands as text, one per line, for tests and diagnostics. The commands of an embedded list follow the
// Embed command, indented one level deeper.
PrString prDrawListDump(const PrDrawList* list);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
