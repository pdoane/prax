// Studio's icons: Lucide's glyphs, as SVG path data on a grid of 24 units, which any library draws, such as Studio
// Kit's icons and the paths of lib/ui's elements. The data is the set's, and drawing it is the library's: a path that a
// cache parses once (prPathCacheFind), stroked at the set's stroke width with round joins and caps, or filled.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The icons of the set.
enum {
    PrStudioIconKind_None,
    PrStudioIconKind_ChevronUp,
    PrStudioIconKind_ChevronDown,
    PrStudioIconKind_ChevronLeft,
    PrStudioIconKind_ChevronRight,
    PrStudioIconKind_CaretRight,
    PrStudioIconKind_ArrowLeft,
    PrStudioIconKind_ArrowRight,
    PrStudioIconKind_Swap,
    PrStudioIconKind_Undo,
    PrStudioIconKind_Redo,
    PrStudioIconKind_Cursor,
    PrStudioIconKind_Move,
    PrStudioIconKind_Rotate,
    PrStudioIconKind_Scale,
    PrStudioIconKind_Magnet,
    PrStudioIconKind_Play,
    PrStudioIconKind_Pause,
    PrStudioIconKind_Stop,
    PrStudioIconKind_Step,
    PrStudioIconKind_StepOver,
    PrStudioIconKind_StepInto,
    PrStudioIconKind_StepOut,
    PrStudioIconKind_Restart,
    PrStudioIconKind_Detach,
    PrStudioIconKind_Close,
    PrStudioIconKind_Plus,
    PrStudioIconKind_Minus,
    PrStudioIconKind_Search,
    PrStudioIconKind_Filter,
    PrStudioIconKind_Settings,
    PrStudioIconKind_Trash,
    PrStudioIconKind_More,
    PrStudioIconKind_DotsV,
    PrStudioIconKind_Grid,
    PrStudioIconKind_Eye,
    PrStudioIconKind_EyeOff,
    PrStudioIconKind_Lock,
    PrStudioIconKind_Unlock,
    PrStudioIconKind_Star,
    PrStudioIconKind_Quote,
    PrStudioIconKind_Hash,
    PrStudioIconKind_Ratio,
    PrStudioIconKind_Asterisk,
    PrStudioIconKind_Folder,
    PrStudioIconKind_FolderOpen,
    PrStudioIconKind_Repository,
    PrStudioIconKind_Worktree,
    PrStudioIconKind_Branch,
    PrStudioIconKind_Cube,
    PrStudioIconKind_Sphere,
    PrStudioIconKind_Light,
    PrStudioIconKind_Camera,
    PrStudioIconKind_Script,
    PrStudioIconKind_Audio,
    PrStudioIconKind_Image,
    PrStudioIconKind_Terrain,
    PrStudioIconKind_Link,
    PrStudioIconKind_Target,
    PrStudioIconKind_Check,
    PrStudioIconKind_Alert,
    PrStudioIconKind_Info,
    PrStudioIconKind_Spinner,
    PrStudioIconKind_Atlas,
    PrStudioIconKind_Calendar,
    PrStudioIconKind_Chart,
    PrStudioIconKind_Palette,
    PrStudioIconKind_Flame,
    PrStudioIconKind_Sparkles,
    PrStudioIconKind_Timer,
    PrStudioIconKind_Copy,
    PrStudioIconKind_Lightbulb,
    PrStudioIconKind_WindowMinimize,       // a window's buttons, as a custom title bar draws them
    PrStudioIconKind_WindowMaximize,
    PrStudioIconKind_WindowRestore,
    PrStudioIconKind_Count,
};
typedef uint16_t PrStudioIconKind;

// The grid that the set's glyphs are drawn on, in units, and the width of their strokes on that grid.
PR_CONSTANT float kPrStudioIconGrid = 24.0f;
PR_CONSTANT float kPrStudioIconStrokeWidth = 2.0f;

// The icon's name, as Lucide names it, and its path data on the set's grid. An icon out of the set has no name and no
// data.
PrStringView prStudioIconName(PrStudioIconKind icon);
PrStringView prStudioIconPathData(PrStudioIconKind icon);

// Returns true if the icon is filled rather than stroked.
bool prStudioIconIsFilled(PrStudioIconKind icon);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
