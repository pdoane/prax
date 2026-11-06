// Studio Kit's menus, popovers, the menu bar, context menus, tooltips, modals and the command palette.
//
// A floating widget is created under the round's overlay (prUiOverlay). In place of a parent, it takes the node that it
// floats from: the trigger that opens it, the host of its context menu, or the anchor of its tooltip. Its key must be
// unique among the widgets that float from that node. A floating widget is placed against its node's box at the last
// layout, and kept inside the overlay.
//
// A trigger's open state is retained on the trigger's node as long as every round asks for it. A widget that floats
// from a trigger asks for the state in each round that creates the widget. It closes the state when the round's input
// dismisses the widget: by Escape, by a primary press outside the widget and its trigger, or by the activation of one
// of its items.

#pragma once

#include "pr/kit.h"
#include "pr/kit_choice.h"
#include "pr/kit_icon.h"
#include "pr/kit_input.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Open state

// Returns true if the floating widget of `trigger` is open. `trigger` is a node that the current round created.
bool prKitIsOpen(PrKit* kit, PrLayoutNode trigger);

// Opens or closes the floating widget of `trigger`. `trigger` is a node that the current round created.
void prKitSetOpen(PrKit* kit, PrLayoutNode trigger, bool open);

// Opens the floating widget of `trigger` if it is closed and closes it otherwise. Returns true if it is now open.
bool prKitToggleOpen(PrKit* kit, PrLayoutNode trigger);

// ---------------------------------------------------------------------------------------------------------------------
// Popovers

// The content of a popover.
typedef struct PrKitPopover {
    PrUiKey key;
} PrKitPopover;

// The placement of a popover against its trigger. Zero places it flush with the trigger's bottom left corner, as wide
// as its content.
typedef struct PrKitPopoverStyle {
    PrVec2 offset;                  // px from the trigger's bottom left corner
    float minWidth;                 // px
} PrKitPopoverStyle;

// Creates the raised surface that `trigger` opens, and returns it. The surface is a column that holds the popover's
// content. Returns none while the popover is closed. The popover sits below its trigger, or above it if it does not fit
// below. The round that dismisses the popover closes it and returns none.
PrLayoutNode prKitPopover(PrKit* kit, PrLayoutNode trigger, PrKitPopover popover, PrKitPopoverStyle style);

// ---------------------------------------------------------------------------------------------------------------------
// Menus

// The content of a menu.
typedef struct PrKitMenu {
    PrUiKey key;
} PrKitMenu;

// The width of a menu and how it takes the keyboard. Zero makes the menu as wide as its widest item, and makes it hold
// the keyboard while it is open.
typedef struct PrKitMenuStyle {
    float minWidth;                 // px

    // True to leave focus where it is, as a field's list of suggestions leaves it in the field.
    bool leavesFocus;
} PrKitMenuStyle;

// Creates a menu: a column of items that is a focus scope. A menu in a popover or a context menu holds the keyboard
// while it is open, unless its style leaves focus where it is. Focus moves onto the menu's first item once the menu has
// been laid out, and Up and Down move focus between items.
PrLayoutNode prKitMenu(PrKit* kit, PrLayoutNode parent, PrKitMenu menu, PrKitMenuStyle style);

// The content of a caption above a group of a menu's items.
typedef struct PrKitMenuLabel {
    PrUiKey key;
    PrStringView text;
} PrKitMenuLabel;

// Creates a caption above a group of a menu's items.
PrLayoutNode prKitMenuLabel(PrKit* kit, PrLayoutNode menu, PrKitMenuLabel label);

// The content of a menu item. Every item has a check column, so that the text of a menu's items lines up.
typedef struct PrKitMenuItem {
    PrUiKey key;
    PrStringView text;
    PrStringView shortcut;          // shown in a key cap at the item's end; empty for none
    PrKitIconKind icon;             // shown before the text
    bool checked;
    bool disabled;
} PrKitMenuItem;

// The kind of command that a menu item runs, which sets the color of its text.
enum {
    PrKitMenuItemVariant_Default,
    PrKitMenuItemVariant_Danger,    // the command destroys something
};
typedef uint8_t PrKitMenuItemVariant;

// The style of a menu item.
typedef struct PrKitMenuItemStyle {
    PrKitMenuItemVariant variant;
} PrKitMenuItemStyle;

// Creates a menu item. A click activates the item (prKitActivated), and so does Enter or Space while it has focus. Its
// activation closes the popover or context menu that the menu is in. A disabled item is shown faint and takes no input.
PrLayoutNode prKitMenuItem(PrKit* kit, PrLayoutNode menu, PrKitMenuItem item, PrKitMenuItemStyle style);

// The content of a rule between groups of a menu's items.
typedef struct PrKitMenuSeparator {
    PrUiKey key;
} PrKitMenuSeparator;

// Creates a rule between groups of a menu's items.
PrLayoutNode prKitMenuSeparator(PrKit* kit, PrLayoutNode menu, PrKitMenuSeparator separator);

// ---------------------------------------------------------------------------------------------------------------------
// Selects

// The content of a select: the label and icon of the chosen option. An empty label shows no option chosen.
typedef struct PrKitSelect {
    PrUiKey key;
    PrStringView label;
    PrKitIconKind icon;
    bool disabled;
} PrKitSelect;

// Creates a select: a field that shows the chosen option and opens a menu of options. In a row, the select is as wide
// as its content. A caller sets its width with prKitPlace.
PrLayoutNode prKitSelect(PrKit* kit, PrLayoutNode parent, PrKitSelect select, PrKitChoiceStyle style);

// Returns the menu of the select's options while it is open, or none. The caller creates each option as a menu item
// (prKitMenuItem) that is checked if it is the chosen option. Activating an option closes the menu and gives the select
// focus. The chosen option is scrolled into view when the menu opens. A menu taller than the room it is given scrolls.
PrLayoutNode prKitSelectMenu(PrKit* kit, PrLayoutNode select);

// Returns true if Up or Down was pressed in the current round while the select had focus and its menu was closed.
// Writes -1 to `step` for Up and 1 for Down. The caller chooses the option before or after the chosen one.
bool prKitStepped(const PrKit* kit, PrLayoutNode select, int32_t* step);

// ---------------------------------------------------------------------------------------------------------------------
// Menu bars

// The maximum number of items in one menu bar that can pass the open menu between them.
enum { kPrKitMenuBarMaxItems = 32 };

// The content of an application's menu bar.
typedef struct PrKitMenuBar {
    PrUiKey key;
} PrKitMenuBar;

// Creates an application's menu bar: a row on the header background with a rule below it. While one of its items is
// open, moving the pointer onto another item closes the open item and opens the other.
PrLayoutNode prKitMenuBar(PrKit* kit, PrLayoutNode parent, PrKitMenuBar bar);

// The content of an item of a menu bar.
typedef struct PrKitMenuBarItem {
    PrUiKey key;
    PrStringView text;
    bool disabled;
} PrKitMenuBarItem;

// Creates an item of a menu bar. The item is a trigger, and its activation opens or closes it. Its menu is a popover of
// the item.
PrLayoutNode prKitMenuBarItem(PrKit* kit, PrLayoutNode bar, PrKitMenuBarItem item);

// ---------------------------------------------------------------------------------------------------------------------
// Context menus

// Opens the context menu of `host` at `at`, in viewport px, or closes it.
void prKitOpenContextMenu(PrKit* kit, PrLayoutNode host, PrVec2 at);
void prKitCloseContextMenu(PrKit* kit, PrLayoutNode host);

// The content of a context menu.
typedef struct PrKitContextMenu {
    PrUiKey key;
} PrKitContextMenu;

// Creates the context menu of `host`, which is a node that the current round created, and returns its menu. Returns
// none while the menu is closed. The host takes presses in the round. A secondary click opens the menu at the pointer
// if the click is on the host itself, or on a part of it that takes no presses. Escape, a press outside the menu, or
// the activation of an item closes the menu.
PrLayoutNode prKitContextMenu(PrKit* kit, PrLayoutNode host, PrKitContextMenu menu, PrKitMenuStyle style);

// ---------------------------------------------------------------------------------------------------------------------
// Tooltips

// The number of seconds that the pointer rests on an anchor before its tooltip shows.
static const double kPrKitTooltipDelay = 0.5;

// The content of a tooltip.
typedef struct PrKitTooltip {
    PrUiKey key;
    PrStringView text;
    PrStringView shortcut;          // shown in a key cap after the text; empty for none
} PrKitTooltip;

// Creates the tooltip of `anchor`, below the anchor, once the pointer has rested on it for kPrKitTooltipDelay seconds.
// Returns none until then. A tooltip lets the pointer through to what is under it. A tooltip that is waiting for its
// delay requests frames.
PrLayoutNode prKitTooltip(PrKit* kit, PrLayoutNode anchor, PrKitTooltip tooltip);

// ---------------------------------------------------------------------------------------------------------------------
// Drag ghosts

// The content of the ghost of a dragged payload. `key` is unique among the ghosts of the payload's source.
typedef struct PrKitDragGhost {
    PrUiKey key;
    PrStringView text;
    PrKitIconKind icon;
} PrKitDragGhost;

// Creates the ghost of the payload in flight (prUiDragging), which follows the pointer, a little below and right of it,
// and lets the pointer through to the targets under it. Returns none if no payload is in flight.
PrLayoutNode prKitDragGhost(PrKit* kit, PrKitDragGhost ghost);

// ---------------------------------------------------------------------------------------------------------------------
// Modals and dialogs

// Where a modal puts its content over the viewport.
enum {
    PrKitModalPlacement_Center,
    PrKitModalPlacement_Top,        // centered across the viewport, `inset` px below its top edge
};
typedef uint8_t PrKitModalPlacement;

// The content of a modal.
typedef struct PrKitModal {
    PrUiKey key;
} PrKitModal;

// The placement of a modal. Zero is centered.
typedef struct PrKitModalStyle {
    PrKitModalPlacement placement;
    float inset;                    // px; 0 means 120
} PrKitModalStyle;

// Creates the modal that `trigger` opens and returns its content, which is a column that is a focus scope. Returns none
// while the modal is closed. A scrim covers the viewport under the content and takes the pointer from what is under it.
// Once the content has been laid out, focus moves to the node that prKitModalAutofocus names, or else to the content's
// first focusable node. Tab keeps focus within the content. Escape, or a primary press outside the content, closes the
// modal, and the call returns none.
PrLayoutNode prKitModal(PrKit* kit, PrLayoutNode trigger, PrKitModal modal, PrKitModalStyle style);

// Makes `node` the node that receives focus when the modal opens, like the autofocus attribute in an HTML dialog.
// `node` is focusable, created in the current round, and inside an open modal's content. The call does nothing if focus
// is already in the modal.
void prKitModalAutofocus(PrKit* kit, PrLayoutNode node);

// The content of a dialog.
typedef struct PrKitDialog {
    PrUiKey key;
    PrStringView title;
} PrKitDialog;

// The placement and width of a dialog. Zero is centered, 420 px wide.
typedef struct PrKitDialogStyle {
    PrKitModalStyle modal;
    float width;                    // px; 0 means 420
} PrKitDialogStyle;

// Creates the dialog that `trigger` opens: a modal of a card with a header that holds the title and a close button.
// Returns the card's body, which is a padded column, or none while the dialog is closed. The close button closes the
// dialog, and so do the modal's dismissals.
PrLayoutNode prKitDialog(PrKit* kit, PrLayoutNode trigger, PrKitDialog dialog, PrKitDialogStyle style);

// Creates a row at the end of a dialog's body that puts its buttons at its end.
PrLayoutNode prKitDialogFooter(PrKit* kit, PrLayoutNode body);

// ---------------------------------------------------------------------------------------------------------------------
// Command palettes

// The content of a command palette.
typedef struct PrKitCommandPalette {
    PrUiKey key;
    PrStringView prompt;            // faint text in the search row while the query is empty
    uint32_t count;                 // number of items that the caller creates in the palette's list

    // The query that the search row edits while the palette is searchable, by which the caller filters the items it
    // creates. The caller takes a new query from prKitTextChanged on the palette's list. A palette that is not
    // searchable shows only its prompt in the search row.
    PrStringView query;
    bool searchable;
} PrKitCommandPalette;

// Creates the command palette that `trigger` opens: a modal at the top of the viewport, over a card of a search row and
// a list. Returns the list, in which the caller creates the items, or none while the palette is closed. The palette
// holds focus while it is open, and keeps its selected item while it stays open. Up and Down move the selection, and
// Enter activates the selected item.
PrLayoutNode prKitCommandPalette(PrKit* kit, PrLayoutNode trigger, PrKitCommandPalette palette);

// The content of an item in a command palette.
typedef struct PrKitCommandItem {
    PrUiKey key;
    PrStringView text;
    PrStringView category;          // shown faint after the text; empty for none
    PrStringView shortcut;          // shown in a key cap at the item's end; empty for none
    PrKitIconKind icon;
    bool disabled;
} PrKitCommandItem;

// Creates an item in a command palette's list. A click activates the item (prKitActivated), and so does Enter while it
// is the palette's selected item, in the next round. Its activation closes the palette. The selected item is scrolled
// into view as the selection moves.
PrLayoutNode prKitCommandItem(PrKit* kit, PrLayoutNode list, PrKitCommandItem item);

// Creates the row of key hints at the end of a command palette's card, after the list's items.
PrLayoutNode prKitCommandPaletteFooter(PrKit* kit, PrLayoutNode list);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
