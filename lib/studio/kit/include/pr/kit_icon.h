// Studio Kit's icons: Lucide's glyphs, drawn as paths at any size.

#pragma once

#include "pr/kit.h"
#include "pr/kit_text.h"
#include "pr/path.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The icons of the set.
enum {
    PrKitIconKind_None,
    PrKitIconKind_ChevronUp,
    PrKitIconKind_ChevronDown,
    PrKitIconKind_ChevronLeft,
    PrKitIconKind_ChevronRight,
    PrKitIconKind_CaretRight,
    PrKitIconKind_ArrowLeft,
    PrKitIconKind_ArrowRight,
    PrKitIconKind_Swap,
    PrKitIconKind_Undo,
    PrKitIconKind_Redo,
    PrKitIconKind_Cursor,
    PrKitIconKind_Move,
    PrKitIconKind_Rotate,
    PrKitIconKind_Scale,
    PrKitIconKind_Magnet,
    PrKitIconKind_Play,
    PrKitIconKind_Pause,
    PrKitIconKind_Stop,
    PrKitIconKind_Step,
    PrKitIconKind_StepOver,
    PrKitIconKind_StepInto,
    PrKitIconKind_StepOut,
    PrKitIconKind_Restart,
    PrKitIconKind_Detach,
    PrKitIconKind_Close,
    PrKitIconKind_Plus,
    PrKitIconKind_Minus,
    PrKitIconKind_Search,
    PrKitIconKind_Filter,
    PrKitIconKind_Settings,
    PrKitIconKind_Trash,
    PrKitIconKind_More,
    PrKitIconKind_DotsV,
    PrKitIconKind_Grid,
    PrKitIconKind_Eye,
    PrKitIconKind_EyeOff,
    PrKitIconKind_Lock,
    PrKitIconKind_Unlock,
    PrKitIconKind_Star,
    PrKitIconKind_Quote,
    PrKitIconKind_Hash,
    PrKitIconKind_Ratio,
    PrKitIconKind_Asterisk,
    PrKitIconKind_Folder,
    PrKitIconKind_FolderOpen,
    PrKitIconKind_Repository,
    PrKitIconKind_Worktree,
    PrKitIconKind_Branch,
    PrKitIconKind_Cube,
    PrKitIconKind_Sphere,
    PrKitIconKind_Light,
    PrKitIconKind_Camera,
    PrKitIconKind_Script,
    PrKitIconKind_Audio,
    PrKitIconKind_Image,
    PrKitIconKind_Terrain,
    PrKitIconKind_Link,
    PrKitIconKind_Target,
    PrKitIconKind_Check,
    PrKitIconKind_Alert,
    PrKitIconKind_Info,
    PrKitIconKind_Spinner,
    PrKitIconKind_Atlas,
    PrKitIconKind_Count,
};
typedef uint16_t PrKitIconKind;

// Returns the icon's name, as Lucide names it.
PrStringView prKitIconName(PrKitIconKind icon);

// Returns true if the icon is filled rather than stroked.
bool prKitIconFilled(PrKitIconKind icon);

// How an icon is drawn: a square of `size` px, or 16 px for zero, in `tone`.
PR_NO_PADDING_BEGIN
typedef struct PrKitIconStyle {
    float size;
    PrKitTone tone;
    uint8_t _pad[3];
} PrKitIconStyle;
PR_NO_PADDING_END

// An icon of the set.
typedef struct PrKitIcon {
    uint64_t key;
    PrKitIconKind icon;
} PrKitIcon;

// Creates an icon under `parent`.
PrLayoutNode prKitIcon(PrKit* kit, PrLayoutNode parent, PrKitIcon icon, PrKitIconStyle style);

// An icon of the caller's: `path`, on the set's grid of 24 units, in `color`, stroked at the set's stroke width if
// `stroked` is true and filled otherwise. `path` must outlive every round that draws it.
typedef struct PrKitPathIcon {
    uint64_t key;
    const PrPath* path;
    PrColor color;
    bool stroked;
} PrKitPathIcon;

// Creates a path icon under `parent`. Its style's tone is not used.
PrLayoutNode prKitPathIcon(PrKit* kit, PrLayoutNode parent, PrKitPathIcon icon, PrKitIconStyle style);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
