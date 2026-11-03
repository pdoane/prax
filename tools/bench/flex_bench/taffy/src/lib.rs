//! The flex benchmark's adapter for Taffy: a TaffyTree behind a C interface, built from the benchmark's styles
//! (tools/bench/flex_bench/flex_style.h, whose layout `FlexStyle` repeats).

use taffy::prelude::*;
use taffy::{compute_leaf_layout, AlignContent, AlignItems, LengthPercentage, LengthPercentageAuto, Overflow, Point};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FlexLength {
    kind: u8,
    _pad: [u8; 3],
    value: f32,
}

#[repr(C)]
pub struct FlexStyle {
    display: u8,
    direction: u8,
    wrap: u8,
    align_items: u8,
    align_self: u8,
    align_content: u8,
    justify_content: u8,
    margin_auto: u8,
    grow: f32,
    shrink: f32,
    basis: FlexLength,
    width: FlexLength,
    height: FlexLength,
    min_width: FlexLength,
    min_height: FlexLength,
    max_width: FlexLength,
    max_height: FlexLength,
    margin: [f32; 4],
    padding: [f32; 4],
    border: [f32; 4],
    gap: f32,
    aspect_ratio: f32,
    position: u8,
    overflow: u8,
    _pad2: [u8; 2],
    inset: [FlexLength; 4],
    _order: i32,
    _words: u16,
    _percent_word: u8,
    _pad3: u8,
}

const AUTO: u8 = 0;
const PX: u8 = 1;

fn dimension(length: FlexLength) -> Dimension {
    match length.kind {
        AUTO => Dimension::auto(),
        PX => Dimension::length(length.value),
        _ => Dimension::percent(length.value / 100.0),
    }
}

fn limit(length: FlexLength) -> LengthPercentageAuto {
    match length.kind {
        AUTO => LengthPercentageAuto::auto(),
        PX => LengthPercentageAuto::length(length.value),
        _ => LengthPercentageAuto::percent(length.value / 100.0),
    }
}

fn sides<T>(values: &[f32; 4], make: impl Fn(f32) -> T) -> Rect<T> {
    Rect { top: make(values[0]), right: make(values[1]), bottom: make(values[2]), left: make(values[3]) }
}

fn margin(flex: &FlexStyle, side: usize) -> LengthPercentageAuto {
    if flex.margin_auto & (1 << side) != 0 {
        LengthPercentageAuto::auto()
    } else {
        LengthPercentageAuto::length(flex.margin[side])
    }
}

fn items(align: u8) -> Option<AlignItems> {
    match align {
        1 => Some(AlignItems::FLEX_START),
        2 => Some(AlignItems::FLEX_END),
        3 => Some(AlignItems::CENTER),
        4 => Some(AlignItems::STRETCH),
        8 => Some(AlignItems::BASELINE),
        _ => None,
    }
}

fn content(align: u8) -> Option<AlignContent> {
    match align {
        1 => Some(AlignContent::FLEX_START),
        2 => Some(AlignContent::FLEX_END),
        3 => Some(AlignContent::CENTER),
        4 => Some(AlignContent::STRETCH),
        5 => Some(AlignContent::SPACE_BETWEEN),
        6 => Some(AlignContent::SPACE_AROUND),
        7 => Some(AlignContent::SPACE_EVENLY),
        _ => None,
    }
}

fn style_of(flex: &FlexStyle) -> Style {
    Style {
        display: if flex.display == 0 { Display::Flex } else { Display::None },
        flex_direction: match flex.direction {
            1 => FlexDirection::Column,
            2 => FlexDirection::RowReverse,
            3 => FlexDirection::ColumnReverse,
            _ => FlexDirection::Row,
        },
        flex_wrap: match flex.wrap {
            1 => FlexWrap::Wrap,
            2 => FlexWrap::WrapReverse,
            _ => FlexWrap::NoWrap,
        },
        align_items: items(flex.align_items),
        align_self: items(flex.align_self),
        align_content: content(flex.align_content),
        justify_content: content(flex.justify_content),
        flex_grow: flex.grow,
        flex_shrink: flex.shrink,
        flex_basis: dimension(flex.basis),
        size: Size { width: dimension(flex.width), height: dimension(flex.height) },
        min_size: Size { width: limit(flex.min_width), height: limit(flex.min_height) },
        max_size: Size { width: limit(flex.max_width), height: limit(flex.max_height) },
        margin: Rect {
            top: margin(flex, 0),
            right: margin(flex, 1),
            bottom: margin(flex, 2),
            left: margin(flex, 3),
        },
        padding: sides(&flex.padding, LengthPercentage::length),
        border: sides(&flex.border, LengthPercentage::length),
        gap: Size { width: LengthPercentage::length(flex.gap), height: LengthPercentage::length(flex.gap) },
        aspect_ratio: if flex.aspect_ratio > 0.0 { Some(flex.aspect_ratio) } else { None },
        position: if flex.position == 2 { Position::Absolute } else { Position::Relative },
        overflow: if flex.overflow == 1 {
            Point { x: Overflow::Scroll, y: Overflow::Scroll }
        } else {
            Point { x: Overflow::Visible, y: Overflow::Visible }
        },
        inset: Rect {
            top: limit(flex.inset[0]),
            right: limit(flex.inset[1]),
            bottom: limit(flex.inset[2]),
            left: limit(flex.inset[3]),
        },
        ..Default::default()
    }
}

/// A tree, the leaves its last counted layout measured, and the children of the node being made, kept so that making
/// a node allocates only what Taffy does.
pub struct Tree {
    taffy: TaffyTree<()>,
    measured: u32,
    children: Vec<NodeId>,
}

/// A tree whose layouts round nothing, as lib/layout does not; Taffy rounds to whole pixels by default.
#[export_name = "prFlexTaffyTreeNew"]
pub extern "C" fn taffy_tree_new() -> *mut Tree {
    let mut taffy = TaffyTree::new();
    taffy.disable_rounding();
    Box::into_raw(Box::new(Tree { taffy, measured: 0, children: Vec::new() }))
}

/// # Safety
/// `tree` is a tree taffy_tree_new made and not yet freed.
#[export_name = "prFlexTaffyTreeFree"]
pub unsafe extern "C" fn taffy_tree_free(tree: *mut Tree) {
    drop(Box::from_raw(tree));
}

/// A node of `style` holding the `count` nodes of `children`, in order.
///
/// # Safety
/// `tree` is live, `style` points to a style, and `children` to `count` nodes of the tree.
#[export_name = "prFlexTaffyNodeNew"]
pub unsafe extern "C" fn taffy_node_new(tree: *mut Tree, style: *const FlexStyle, children: *const u64,
                                        count: usize) -> u64 {
    let tree = &mut *tree;
    let style = style_of(&*style);
    let node = if count == 0 {
        tree.taffy.new_leaf(style)
    } else {
        tree.children.clear();
        tree.children.extend(std::slice::from_raw_parts(children, count).iter().map(|&id| NodeId::from(id)));
        tree.taffy.new_with_children(style, &tree.children)
    };
    node.expect("a node").into()
}

/// # Safety
/// `tree` is live, `node` is its node, and `style` points to a style.
#[export_name = "prFlexTaffyNodeSetStyle"]
pub unsafe extern "C" fn taffy_node_set_style(tree: *mut Tree, node: u64, style: *const FlexStyle) {
    (*tree).taffy.set_style(NodeId::from(node), style_of(&*style)).expect("a node");
}

/// Lays the tree out from `root` in a viewport of `width` by `height`.
///
/// # Safety
/// `tree` is live and `root` is its node.
#[export_name = "prFlexTaffyLayout"]
pub unsafe extern "C" fn taffy_layout(tree: *mut Tree, root: u64, width: f32, height: f32) {
    let space = Size { width: AvailableSpace::Definite(width), height: AvailableSpace::Definite(height) };
    (*tree).taffy.compute_layout(NodeId::from(root), space).expect("a layout");
}

/// As taffy_layout, counting the leaves it measures: each call of the measure function, which a leaf's cached result
/// does not reach.
///
/// # Safety
/// As taffy_layout.
#[export_name = "prFlexTaffyLayoutCounted"]
pub unsafe extern "C" fn taffy_layout_counted(tree: *mut Tree, root: u64, width: f32, height: f32) -> u32 {
    let tree = &mut *tree;
    let space = Size { width: AvailableSpace::Definite(width), height: AvailableSpace::Definite(height) };
    let mut measured = 0u32;
    tree.taffy
        .compute_layout_with_measure(NodeId::from(root), space, |inputs, _, _, style| {
            measured += 1;
            compute_leaf_layout(inputs, style, |_, _| 0.0, |_, _| Size::ZERO)
        })
        .expect("a layout");
    tree.measured = measured;
    measured
}

