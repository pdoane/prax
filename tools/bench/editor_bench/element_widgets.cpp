// The widgets that the editor of elements is built with (element_widgets.h).

#include "tools/bench/editor_bench/element_widgets.h"

#include "pr/layout_query.h"
#include "pr/layout_tree.h"
#include "pr/layout_unit.h"

#include <stdio.h>

namespace widgets {

namespace {

// The rows that a list builds past each end of what it shows, and the rows that it guesses it shows before its first
// layout, as Studio Kit's lists do.
constexpr uint32_t kOverscan = 2;
constexpr uint32_t kUnmeasuredRows = 32;

// The ink of text and icons on the accent.
constexpr PrColor kOnAccent = prColorFromHex(0x0b1220);

// Creates a menu's card in the overlay, under `trigger`.
UiElement menuCard(UiBuilder& ui, PrLayoutNode trigger, float minWidth) {
    PrRect at = prLayoutRectToPx(prLayoutNodeBounds(ui.tree(), trigger, PrLayoutCoordinates_Viewport));
    return ui.div(prUiOverlay(ui.ui())).absolute().left(at.left).top(at.bottom + 2).minW(minWidth).flexCol().p(4)
        .rounded(6).bg(kHeader).border(1).borderColor(kLine).shadowLg().textColor(kText);
}

// An item of a menu. Returns true if it was activated.
bool menuItem(UiBuilder& ui, PrLayoutNode card, PrStringView text, PrStringView shortcut, bool checked,
        bool danger, bool disabled) {
    if (disabled) {
        UiElement row = ui.div(card).flexRow().itemsCenter().h(24).px(8).gap(8).textColor(kFaint);
        ui.div(row).w(12);
        body(ui.span(row).flex1()).text(text);
        return false;
    }
    UiElement row = ui.div(card).flexRow().itemsCenter().h(24).px(8).gap(8).rounded(4).cursorPointer()
        .hoverBg(kHover).textColor(danger ? kDanger : kText);
    UiElement mark = ui.div(row).w(12);
    if (checked)
        icon(ui, mark, PrStudioIconKind_Check, 12, kAccent);
    body(ui.span(row).flex1()).whitespaceNowrap().text(text);
    if (shortcut.size != 0)
        caption(ui.span(row)).textColor(kMuted).text(shortcut);
    return row.clicked();
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Look

UiElement icon(UiBuilder& ui, PrLayoutNode parent, PrStudioIconKind which, float size, PrColor color) {
    UiPath path{
        .path = ui.resources()->path(prStudioIconPathData(which)),
        .viewBox = PrVec2{kPrStudioIconGrid, kPrStudioIconGrid},
        .color = color,
        .strokeWidth = prStudioIconIsFilled(which) ? 0 : kPrStudioIconStrokeWidth,
    };
    return ui.path(parent, path).size(size).shrink0();
}

UiElement body(UiElement e) {
    return e.textSize(12).leading(16);
}

UiElement caption(UiElement e) {
    return e.textSize(11).leading(14);
}

UiElement bar(UiBuilder& ui, PrLayoutNode parent, PrColor background, float height, float gap) {
    return ui.div(parent).flexRow().itemsCenter().h(height).shrink0().px(8).gap(gap).bg(background).overflowHidden();
}

void divider(UiBuilder& ui, PrLayoutNode parent) {
    ui.div(parent).w(1).h(14).mx(4).bg(kLine).shrink0();
}

void spacer(UiBuilder& ui, PrLayoutNode parent) {
    ui.div(parent).flex1();
}

void label(UiBuilder& ui, PrLayoutNode parent, PrStringView text, PrColor color) {
    caption(ui.span(parent).textColor(color).whitespaceNowrap()).text(text);
}

void badge(UiBuilder& ui, PrLayoutNode parent, PrStringView text, PrColor color) {
    ui.span(parent).px(5).rounded(3).bg(PrColor{color.r, color.g, color.b, 0.16f}).textColor(color).textSize(9)
        .leading(14).bold().tracking(0.5f).text(text);
}

PrStringView number(char (&out)[32], float value) {
    int size = snprintf(out, sizeof out, "%.2f", double(value));
    return PrStringView{out, size_t(size > 0 ? size : 0)};
}

PrStringView count(char (&out)[32], uint32_t value) {
    int size = snprintf(out, sizeof out, "%u", value);
    return PrStringView{out, size_t(size > 0 ? size : 0)};
}

// ---------------------------------------------------------------------------------------------------------------------
// Controls

UiResult<Clicked> iconButton(UiBuilder& ui, PrLayoutNode parent, const IconButton& inputs, PrUiKey key) {
    struct Memo : IconButton {
        bool operator==(const Memo&) const = default;

        Clicked build(UiBuilder& ui, UiElement root) const {
            root.size(size).flexRow().itemsCenter().justifyCenter().rounded(4).shrink0().cursorPointer()
                .transition(0.12f) .bg(selected ? kSelect : kClear).hoverBg(selected ? kSelectHover : kHover);
            icon(ui, root, which, size > 20 ? 14 : 12, selected ? kAccent : kSecondary);
            return Clicked{root.clicked()};
        }
    };
    return ui.memo(parent, Memo{inputs}, key);
}

UiResult<Clicked> button(UiBuilder& ui, PrLayoutNode parent, const Button& inputs, PrUiKey key) {
    struct Memo : Button {
        bool operator==(const Memo&) const = default;

        Clicked build(UiBuilder& ui, UiElement root) const {
            PrColor fill = look == ButtonLook_Primary ? kAccent : look == ButtonLook_Solid ? kRaised : kClear;
            PrColor hover = look == ButtonLook_Primary ? kAccentHover : kHover;
            PrColor ink = look == ButtonLook_Primary ? kOnAccent : kText;
            root.flexRow().itemsCenter().gap(5).h(22).px(text.size != 0 ? 8 : 4).rounded(4).shrink0().cursorPointer()
                .transition(0.12f).bg(selected && look != ButtonLook_Primary ? kSelect : fill).hoverBg(hover)
                .textColor(ink);
            if (which != PrStudioIconKind_None)
                icon(ui, root, which, 13, ink);
            if (text.size != 0)
                body(ui.span(root).medium().whitespaceNowrap()).text(text);
            return Clicked{root.clicked()};
        }
    };
    return ui.memo(parent, Memo{inputs}, key);
}

UiResult<Chosen> segmented(UiBuilder& ui, PrLayoutNode parent, const Segmented& inputs, PrUiKey key) {
    struct Memo : Segmented {
        bool operator==(const Memo&) const = default;

        Chosen build(UiBuilder& ui, UiElement root) const {
            root.flexRow().h(kControl).p(2).gap(2).rounded(5).bg(kField).shrink0();
            Chosen out;
            for (uint32_t i = 0; i < count; ++i) {
                bool on = i == selected;
                UiElement s = ui.div(root).flexRow().itemsCenter().px(8).rounded(3).cursorPointer().transition(0.12f)
                    .bg(on ? kRaised : kClear).hoverBg(on ? kRaised : kHover).textColor(on ? kText : kMuted);
                caption(s).medium().text(items[i]);
                if (s.clicked())
                    out.index = int32_t(i);
            }
            return out;
        }
    };
    return ui.memo(parent, Memo{inputs}, key);
}

UiResult<Chosen> tabs(UiBuilder& ui, PrLayoutNode parent, const Tabs& inputs, PrUiKey key) {
    struct Memo : Tabs {
        bool operator==(const Memo&) const = default;

        Chosen build(UiBuilder& ui, UiElement root) const {
            root.flexRow().itemsCenter().gap(2);
            Chosen out;
            for (uint32_t i = 0; i < count; ++i) {
                bool on = i == selected;
                UiElement t = ui.div(root).flexRow().itemsCenter().h(22).px(10).rounded(4).cursorPointer()
                    .bg(on ? kRaised : kClear).hoverBg(on ? kRaised : kHover).textColor(on ? kText : kMuted);
                body(ui.span(t)).text(items[i]);
                if (t.clicked())
                    out.index = int32_t(i);
            }
            return out;
        }
    };
    return ui.memo(parent, Memo{inputs}, key);
}

UiResult<Clicked> checkbox(UiBuilder& ui, PrLayoutNode parent, const Checkbox& inputs, PrUiKey key) {
    struct Memo : Checkbox {
        bool operator==(const Memo&) const = default;

        Clicked build(UiBuilder& ui, UiElement root) const {
            root.flexRow().itemsCenter().gap(6).cursorPointer().shrink0();
            UiElement box = ui.div(root).size(14).rounded(3).flexRow().itemsCenter().justifyCenter().border(1)
                .borderColor(checked ? kAccent : kLine).bg(checked ? kAccent : kField);
            if (checked)
                icon(ui, box, PrStudioIconKind_Check, 10, kOnAccent);
            if (text.size != 0)
                label(ui, root, text, kSecondary);
            return Clicked{root.clicked()};
        }
    };
    return ui.memo(parent, Memo{inputs}, key);
}

UiResult<Clicked> toggle(UiBuilder& ui, PrLayoutNode parent, const Toggle& inputs, PrUiKey key) {
    struct Memo : Toggle {
        bool operator==(const Memo&) const = default;

        Clicked build(UiBuilder& ui, UiElement root) const {
            PrLayoutAlignment knob = on ? PrLayoutAlignment_FlexEnd : PrLayoutAlignment_FlexStart;
            root.w(28).h(16).p(2).rounded(8).bg(on ? kAccent : kRaised).flexRow().justify(knob).cursorPointer() .shrink0()
                .transition(0.15f);
            ui.div(root).size(12).roundedFull().bg(kText);
            return Clicked{root.clicked()};
        }
    };
    return ui.memo(parent, Memo{inputs}, key);
}

UiResult<Edited> numberDrag(UiBuilder& ui, PrLayoutNode parent, const NumberDrag& inputs, PrUiKey key) {
    struct Memo : NumberDrag {
        bool operator==(const Memo&) const = default;

        Edited build(UiBuilder& ui, UiElement root) const {
            root.flexRow().itemsCenter().h(kControl).flex1().minW(0).px(6).gap(4).rounded(3).bg(kField).border(1)
                .borderColor(kLine).hoverBorder(kFaint).cursor(PrAppCursor_ResizeX).pressable().mono().textSize(11)
                .leading(14).textColor(kText).overflowHidden();
            if (axis.a > 0)
                ui.div(root).w(2).h(10).rounded(1).bg(axis).shrink0();
            Edited out;
            out.value = value;
            // The value when the drag began, from which the drag measures.
            float& grab = ui.state<float>(root);
            PrUiDrag drag{};
            if (prUiDrag(ui.ui(), root, 0, 2, &drag)) {
                if (!drag.moved)
                    grab = value;
                float to = grab + (drag.position.x - drag.origin.x) * step;
                to = to < min ? min : to > max ? max : to;
                out.changed = drag.moved && to != value;
                out.committed = drag.released && drag.moved;
                out.value = to;
            }
            char text[32];
            ui.span(root).flex1().text(number(text, out.value));
            if (unit.size != 0)
                ui.span(root).textColor(kMuted).text(unit);
            return out;
        }
    };
    return ui.memo(parent, Memo{inputs}, key);
}

UiResult<VectorEdited> vectorField(UiBuilder& ui, PrLayoutNode parent, const VectorField& inputs, PrUiKey key) {
    struct Memo : VectorField {
        bool operator==(const Memo&) const = default;

        VectorEdited build(UiBuilder& ui, UiElement root) const {
            root.flexRow().gap(4).flex1().minW(0);
            VectorEdited out;
            for (uint32_t i = 0; i < 3; ++i) {
                UiResult<Edited> e =
                    numberDrag(ui, root, {.value = value[i], .step = step, .min = min, .axis = kAxis[i]});
                if (e.changed || e.committed) {
                    out.axis = int32_t(i);
                    out.value = e.value;
                    out.committed = e.committed;
                }
            }
            return out;
        }
    };
    return ui.memo(parent, Memo{inputs}, key);
}

UiResult<Edited> slider(UiBuilder& ui, PrLayoutNode parent, const Slider& inputs, PrUiKey key) {
    struct Memo : Slider {
        bool operator==(const Memo&) const = default;

        Edited build(UiBuilder& ui, UiElement root) const {
            root.flex1().h(kControl).flexRow().itemsCenter().relative().cursorPointer().pressable();
            Edited out;
            out.value = value;
            PrVec2 extent{};
            PrUiDrag drag{};
            if (prUiDrag(ui.ui(), root, 0, 0, &drag) && prUiExtent(ui.ui(), root, PrUiAxisFlag_X, &extent) && extent.x > 0) {
                PrRect box = prLayoutRectToPx(prLayoutNodeBounds(ui.tree(), root, PrLayoutCoordinates_Viewport));
                float t = (drag.position.x - box.left) / extent.x;
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                out.value = min + (max - min) * t;
                out.changed = out.value != value;
                out.committed = drag.released;
            }
            float share = (out.value - min) / (max - min) * 100;
            UiElement track = ui.div(root).wFull().h(4).rounded(2).bg(kRaised);
            ui.div(track).w(uiPct(share)).hFull().rounded(2).bg(kAccent);
            ui.div(root).absolute().left(uiPct(share)).size(12).ml(-6).roundedFull().bg(kText).shadowSm();
            return out;
        }
    };
    return ui.memo(parent, Memo{inputs}, key);
}

// ---------------------------------------------------------------------------------------------------------------------
// Menus

UiResult<Ran> menu(UiBuilder& ui, PrLayoutNode parent, const Menu& inputs, PrUiKey key) {
    struct Memo : Menu {
        bool operator==(const Memo&) const = default;

        Ran build(UiBuilder& ui, UiElement root) const {
            bool& open = ui.state<bool>(root);
            root.flexRow().itemsCenter().h(20).px(8).rounded(4).cursorPointer().bg(open ? kHover : kClear)
                .hoverBg(kHover);
            body(ui.span(root)).text(title);
            if (root.clicked())
                open = !open;
            Ran out;
            if (!open)
                return out;
            UiElement card = menuCard(ui, root, 220);
            for (uint32_t i = 0; i < count; ++i) {
                const MenuEntry& entry = entries[i];
                if (entry.separatorBefore)
                    ui.div(card).h(1).my(4).bg(kLine);
                bool isChecked = (checked >> i) & 1;
                bool isDisabled = entry.disabled || ((disabled >> i) & 1);
                if (menuItem(ui, card, entry.text, entry.shortcut, isChecked, entry.danger, isDisabled)) {
                    out = Ran{.ran = true, .command = entry.command};
                    open = false;
                }
            }
            return out;
        }
    };
    return ui.memo(parent, Memo{inputs}, key);
}

UiResult<Chosen> dropdown(UiBuilder& ui, PrLayoutNode parent, const Dropdown& inputs, PrUiKey key) {
    struct Memo : Dropdown {
        bool operator==(const Memo&) const = default;

        Chosen build(UiBuilder& ui, UiElement root) const {
            bool& open = ui.state<bool>(root);
            if (button(ui, root, {.text = items[selected], .which = which}).clicked)
                open = !open;
            Chosen out;
            if (!open)
                return out;
            UiElement card = menuCard(ui, root, 160);
            for (uint32_t i = 0; i < count; ++i) {
                if (menuItem(ui, card, items[i], {}, i == selected, false, false)) {
                    out.index = int32_t(i);
                    open = false;
                }
            }
            return out;
        }
    };
    return ui.memo(parent, Memo{inputs}, key);
}

void tooltip(UiBuilder& ui, PrLayoutNode anchor, PrStringView text, PrStringView shortcut) {
    if (!prUiIsHovered(ui.ui(), anchor) || prUiHoverSeconds(ui.ui(), anchor) <= 0.5)
        return;
    PrRect at = prLayoutRectToPx(prLayoutNodeBounds(ui.tree(), anchor, PrLayoutCoordinates_Viewport));
    UiElement tip = ui.div(prUiOverlay(ui.ui())).absolute().left(at.left).top(at.bottom + 4).flexRow().gap(8).px(8)
        .py(4).rounded(4).bg(kRaised).border(1).borderColor(kLine).textColor(kText);
    caption(ui.span(tip)).text(text);
    if (shortcut.size != 0)
        caption(ui.span(tip)).textColor(kMuted).text(shortcut);
}

// ---------------------------------------------------------------------------------------------------------------------
// Panels

UiElement panelHeader(UiBuilder& ui, PrLayoutNode parent, PrStringView title) {
    UiElement header = bar(ui, parent, kHeader, kPanelHeader, 4).borderB(1).borderColor(kLine);
    ui.span(header).textColor(kMuted).textSize(10).leading(14).bold().tracking(1).text(title);
    spacer(ui, header);
    return header;
}

UiResult<RowEvents> treeRow(UiBuilder& ui, PrLayoutNode parent, const TreeRow& inputs, PrUiKey key) {
    struct Memo : TreeRow {
        bool operator==(const Memo&) const = default;

        RowEvents build(UiBuilder& ui, UiElement root) const {
            bool hovered = root.hovered();
            root.flexRow().itemsCenter().h(kRow).shrink0().pl(6 + float(depth) * 12).pr(6).gap(4).cursorPointer()
                .bg(selected ? kSelect : kClear).hoverBg(selected ? kSelectHover : kHover);
            RowEvents out;
            UiElement chevron = ui.div(root).size(14).flexRow().itemsCenter().justifyCenter().shrink0();
            if (hasChildren) {
                icon(ui, chevron, PrStudioIconKind(open ? PrStudioIconKind_ChevronDown : PrStudioIconKind_ChevronRight),
                    10, kMuted);
                out.folded = chevron.clicked();
            }
            icon(ui, root, which, 12, hidden ? kFaint : selected ? kAccent : kSecondary);
            body(ui.span(root).flex1().minW(0).whitespaceNowrap().overflowHidden()).textColor(hidden ? kFaint : kText)
                .text(name);
            if (type.view().size != 0)
                caption(ui.span(root)).textColor(kFaint).whitespaceNowrap().text(type);
            UiElement eye = ui.div(root).size(16).flexRow().itemsCenter().justifyCenter().rounded(3).shrink0();
            if (hovered || hidden) {
                PrStudioIconKind seen = PrStudioIconKind(hidden ? PrStudioIconKind_EyeOff : PrStudioIconKind_Eye);
                icon(ui, eye, seen, 11, hidden ? kFaint : kSecondary);
                out.hid = eye.clicked();
            }
            out.clicked = root.clicked();
            return out;
        }
    };
    return ui.memo(parent, Memo{inputs}, key);
}

ListWindow listWindow(UiBuilder& ui, UiElement root, uint32_t count, float rowHeight) {
    root.flexCol().flex1().minH(0).overflowYScroll().py(2);
    // The scrollport at the last layout, or a guess before the first.
    PrVec2 port{};
    if (!prUiExtent(ui.ui(), root, PrUiAxisFlag_Y, &port))
        port.y = rowHeight * float(kUnmeasuredRows);
    float total = rowHeight * float(count);
    float offset = prLayoutUnitToPx(prLayoutNodeScroll(ui.tree(), root).y);
    float most = total > port.y ? total - port.y : 0;
    offset = offset < 0 ? 0 : offset > most ? most : offset;
    uint32_t top = uint32_t(offset / rowHeight);
    uint32_t bottom = uint32_t((offset + port.y) / rowHeight) + 1;
    uint32_t first = top > kOverscan ? top - kOverscan : 0;
    uint32_t end = bottom + kOverscan < count ? bottom + kOverscan : count;
    first = first < end ? first : end;
    ui.div(root).h(rowHeight * float(first)).shrink0();
    return ListWindow{.first = first, .end = end};
}

void listRest(UiBuilder& ui, PrLayoutNode root, ListWindow window, uint32_t count, float rowHeight) {
    ui.div(root).h(rowHeight * float(count - window.end)).shrink0();
}

UiResult<Clicked> tile(UiBuilder& ui, PrLayoutNode parent, const Tile& inputs, PrUiKey key) {
    struct Memo : Tile {
        bool operator==(const Memo&) const = default;

        Clicked build(UiBuilder& ui, UiElement root) const {
            root.w(84).h(88).flexCol().itemsCenter().justifyCenter().gap(6).rounded(6).cursorPointer()
                .bg(selected ? kSelect : kClear).hoverBg(selected ? kSelectHover : kHover);
            PrColor tone = selected ? kAccent : kMuted;
            icon(ui, root, which, 24, tone);
            caption(ui.span(root)).textColor(tone).text(name);
            return Clicked{root.clicked()};
        }
    };
    return ui.memo(parent, Memo{inputs}, key);
}

UiElement logLine(UiBuilder& ui, PrLayoutNode parent, const LogLine& inputs, PrUiKey key) {
    struct Memo : LogLine {
        bool operator==(const Memo&) const = default;

        void build(UiBuilder& ui, UiElement root) const {
            PrColor tone =
                level == PrKitLogLevel_Error ? kDanger : level == PrKitLogLevel_Warning ? kWarning : kSecondary;
            root.flexRow().itemsCenter().h(kRow).shrink0().gap(8).px(4).mono().textSize(11).leading(14).hoverBg(kHover);
            ui.span(root).textColor(kFaint).text(time);
            ui.div(root).size(6).roundedFull().bg(tone).shrink0();
            PrColor ink = level == PrKitLogLevel_Info ? kText : tone;
            ui.span(root).flex1().minW(0).whitespaceNowrap().overflowHidden().textColor(ink).text(message);
            ui.span(root).textColor(kMuted).text(source);
        }
    };
    return ui.memo(parent, Memo{inputs}, key);
}

UiResult<Clicked> groupHeader(UiBuilder& ui, PrLayoutNode parent, const GroupHeader& inputs, PrUiKey key) {
    struct Memo : GroupHeader {
        bool operator==(const Memo&) const = default;

        Clicked build(UiBuilder& ui, UiElement root) const {
            root.flexRow().itemsCenter().h(kRow + 4).shrink0().px(8).gap(6).bg(kHeader).borderB(1).borderColor(kLine)
                .cursorPointer().hoverBg(kHover);
            icon(ui, root, PrStudioIconKind(open ? PrStudioIconKind_ChevronDown : PrStudioIconKind_ChevronRight), 10, kMuted);
            body(ui.span(root).flex1()).semibold().textColor(kText).text(name);
            if (badge.size != 0)
                widgets::badge(ui, root, badge, badgeColor);
            return Clicked{root.clicked()};
        }
    };
    return ui.memo(parent, Memo{inputs}, key);
}

UiResult<FieldEdited> propertyRow(UiBuilder& ui, PrLayoutNode parent, const PropertyRow& inputs, PrUiKey key) {
    struct Memo : PropertyRow {
        bool operator==(const Memo&) const = default;

        FieldEdited build(UiBuilder& ui, UiElement root) const {
            root.flexRow().itemsCenter().minH(kRow + 4).shrink0().px(8).gap(8);
            caption(ui.span(root).w(96).shrink0().whitespaceNowrap()).textColor(kMuted).text(name);
            UiElement content = ui.div(root).flexRow().itemsCenter().gap(8).flex1().minW(0);
            FieldEdited out;
            out.value = value;
            auto number = [&](const Edited& e) {
                if (!e.changed && !e.committed)
                    return;
                out.changed = true;
                out.committed = e.committed;
                out.value.numbers[0] = e.value;
            };
            switch (kind) {
                case FieldKind_Number:
                    number(numberDrag(ui, content, {
                        .value = value.numbers[0],
                        .step = step,
                        .min = min,
                        .max = max,
                        .unit = unit,
                    }));
                    break;
                case FieldKind_Slider: {
                    number(slider(ui, content, {.value = value.numbers[0], .min = min, .max = max}));
                    UiResult<Edited> field = numberDrag(ui, content, {
                        .value = value.numbers[0],
                        .step = step,
                        .min = min,
                        .max = max,
                    });
                    field.root.grow(0).shrink0().basis(56);
                    number(field);
                    break;
                }
                case FieldKind_Vector: {
                    VectorField vector{
                        .value = {value.numbers[0], value.numbers[1], value.numbers[2]},
                        .step = step,
                        .min = min,
                    };
                    UiResult<VectorEdited> e = vectorField(ui, content, vector);
                    if (e.axis >= 0) {
                        out.changed = true;
                        out.committed = e.committed;
                        out.value.numbers[e.axis] = e.value;
                    }
                    break;
                }
                case FieldKind_Switch:
                case FieldKind_Check: {
                    bool clicked = false;
                    if (kind == FieldKind_Switch)
                        clicked = toggle(ui, content, {.on = value.flag}).clicked;
                    else
                        clicked = checkbox(ui, content, {.checked = value.flag}).clicked;
                    if (clicked) {
                        out.changed = out.committed = true;
                        out.value.flag = !value.flag;
                    }
                    break;
                }
                case FieldKind_Choice: {
                    UiResult<Chosen> chosen = segmented(ui, content, {
                        .items = choices,
                        .count = choiceCount,
                        .selected = value.choice,
                    });
                    if (chosen.index >= 0) {
                        out.changed = out.committed = true;
                        out.value.choice = uint32_t(chosen.index);
                    }
                    break;
                }
                case FieldKind_Color: {
                    PrColor c = value.color;
                    ui.div(content).size(16).rounded(3).bg(c).border(1).borderColor(kLine);
                    char hex[32];
                    int size = snprintf(hex, sizeof hex, "#%02X%02X%02X", unsigned(c.r * 255), unsigned(c.g * 255),
                        unsigned(c.b * 255));
                    caption(ui.span(content).mono()).text(PrStringView{hex, size_t(size > 0 ? size : 0)});
                    break;
                }
                case FieldKind_Asset: {
                    UiElement field = ui.div(content).flexRow().itemsCenter().gap(6).h(kControl).flex1().minW(0).px(6)
                        .rounded(3).bg(kField).border(1).borderColor(kLine);
                    icon(ui, field, PrStudioIconKind_Cube, 12, kSecondary);
                    caption(ui.span(field).flex1().whitespaceNowrap()).text(value.text);
                    break;
                }
                default: break;
            }
            return out;
        }
    };
    return ui.memo(parent, Memo{inputs}, key);
}

UiElement hudPanel(UiBuilder& ui, PrLayoutNode parent, bool right, bool bottom, bool row) {
    UiElement panel = ui.div(parent).absolute().flexRow().itemsCenter().gap(10).px(8).py(6).rounded(6)
        .bg(PrColor{0.06f, 0.07f, 0.09f, 0.82f}).border(1).borderColor(kLine).mono().textSize(10).leading(14);
    if (!row)
        panel.flexCol().itemsStart().gap(2);
    if (right)
        panel.right(10);
    else
        panel.left(10);
    if (bottom)
        panel.bottom(10);
    else
        panel.top(10);
    return panel;
}

void readout(UiBuilder& ui, PrLayoutNode parent, PrStringView name, PrStringView value) {
    UiElement line = ui.div(parent).flexRow().gap(6);
    ui.span(line).textColor(kMuted).text(name);
    if (value.size != 0)
        ui.span(line).textColor(kText).text(value);
}

// ---------------------------------------------------------------------------------------------------------------------
// Docks

PrLayoutNode pane(UiBuilder& ui, PrLayoutNode parent, PrStringView title, float share) {
    UiElement box = ui.div(parent).flexCol().minH(0).overflowHidden();
    if (share > 0)
        box.h(uiPct(share * 100)).shrink0();
    else
        box.flex1();
    UiElement strip = ui.div(box).flexRow().itemsEnd().h(kTabStrip).shrink0().px(4).bg(kApp);
    UiElement tab = ui.div(strip).flexRow().itemsCenter().h(kTabStrip - 2).px(10).roundedT(4).bg(kHeader)
        .textColor(kText);
    caption(ui.span(tab)).medium().text(title);
    return ui.div(box).flexCol().flex1().minH(0);
}

void splitter(UiBuilder& ui, PrLayoutNode parent, PrLayoutNode container, bool vertical, float* share, bool after) {
    UiElement handle = ui.div(parent).shrink0().bg(kApp).hoverBg(kAccent).pressable()
        .cursor(vertical ? PrAppCursor_ResizeX : PrAppCursor_ResizeY);
    if (vertical)
        handle.w(4);
    else
        handle.h(4);
    PrUiDrag drag{};
    PrVec2 extent{};
    PrUiAxisFlags axis = vertical ? PrUiAxisFlag_X : PrUiAxisFlag_Y;
    if (prUiDrag(ui.ui(), handle, 0, 0, &drag) && drag.moved && prUiExtent(ui.ui(), container, axis, &extent)) {
        float along = vertical ? extent.x : extent.y;
        float delta = (vertical ? drag.delta.x : drag.delta.y) * (after ? -1.0f : 1.0f);
        if (along > 0) {
            float to = *share + delta / along;
            *share = to < 0.1f ? 0.1f : to > 0.9f ? 0.9f : to;
        }
    }
}

} // namespace widgets
