// TeX's spacing between atoms, laid out on a formula's MathML operators.

#include "tex/src/tex_spacing.h"

#include "pr/utf8.h"

namespace pr_impl {

namespace {

// An atom of a list: a node, or the node and the U+2061 that applies it as a function.
struct Atom {
    TexNodeIndex node;
    TexNodeIndex parent;            // the node's parent, which is the list or a declaration's <mstyle> in it
    TexNodeIndex applyFunction;     // the U+2061 that ends the atom, or kTexNodeNone
    TexClass texClass;
};

TexNode& nodeAt(TexTree* tree, TexNodeIndex node) {
    return tree->nodes.data[node];
}

// Returns true if the node lays out a list of atoms of its own. A row element does, unless it is a declaration's
// <mstyle>, whose children are atoms of the list around it.
bool isList(const TexTree* tree, TexNodeIndex node) {
    const TexNode& n = tree->nodes.data[node];
    return texElementIsRow(n.element) && !(n.flags & TexNodeFlag_Transparent);
}

// Appends the atoms of `container`'s children, and of the children of the declarations among them.
void collectAtoms(TexTree* tree, TexNodeIndex container, RawArray<Atom>* atoms) {
    for (TexNodeIndex child = nodeAt(tree, container).firstChild; child != kTexNodeNone;
            child = nodeAt(tree, child).next) {
        const TexNode& n = nodeAt(tree, child);
        if (n.flags & TexNodeFlag_Transparent) {
            collectAtoms(tree, child, atoms);
            continue;
        }
        if (n.element == TexElement_Semantics) {
            // The annotation is not laid out, and the presentation is the atom.
            TexNodeIndex presented = n.firstChild;
            if (presented != kTexNodeNone && nodeAt(tree, presented).texClass != TexClass_None)
                atoms->push(Atom{presented, child, kTexNodeNone, nodeAt(tree, presented).texClass});
            continue;
        }
        if (n.flags & TexNodeFlag_ApplyFunction) {
            if (atoms->count != 0 && atoms->last().applyFunction == kTexNodeNone)
                atoms->last().applyFunction = child;
            continue;
        }
        if (n.texClass == TexClass_None)
            continue;
        atoms->push(Atom{child, container, kTexNodeNone, n.texClass});
    }
}

// The class that an atom has toward the atom before it, and toward the atom after it.
TexClass leftClass(TexClass texClass) {
    return texClass == TexClass_Middle ? TexClass_Close : texClass;
}

TexClass rightClass(TexClass texClass) {
    return texClass == TexClass_Middle ? TexClass_Open : texClass;
}

// Changes the binary operators that TeX treats as ordinary to Ord (The TeXbook, Appendix G, rules 5 and 6). These are a
// Bin that starts the list or follows an Op, Bin, Rel, Open or Punct, a Bin that a Rel, Close or Punct follows, and a
// Bin that ends the list. A list that follows amsmath's empty Ord is treated as starting after an Ord.
void demoteBinaries(RawArray<Atom>* atoms, bool leadingOrd) {
    TexClass previous = leadingOrd ? TexClass_Ord : TexClass_None;
    for (uint32_t i = 0; i < atoms->count; ++i) {
        Atom& atom = atoms->data[i];
        if (atom.texClass == TexClass_Bin) {
            switch (previous) {
                case TexClass_None:
                case TexClass_Bin:
                case TexClass_Op:
                case TexClass_Rel:
                case TexClass_Open:
                case TexClass_Punct:
                    atom.texClass = TexClass_Ord;
                    break;
                default:
                    break;
            }
        }
        TexClass left = leftClass(atom.texClass);
        if (i > 0 && atoms->data[i - 1].texClass == TexClass_Bin &&
                (left == TexClass_Rel || left == TexClass_Close || left == TexClass_Punct))
            atoms->data[i - 1].texClass = TexClass_Ord;
        previous = rightClass(atom.texClass);
    }
    if (atoms->count != 0 && atoms->last().texClass == TexClass_Bin)
        atoms->last().texClass = TexClass_Ord;
}

// TeX's spaces between atoms in mu, by the classes of the left and the right atom (The TeXbook, chapter 18). 3 is
// \thinmuskip, 4 is \medmuskip and 5 is \thickmuskip. A negative space is one of TeX's parenthesized entries, which
// script styles omit.
constexpr int8_t kSpaces[8][8] = {
    //          Ord  Op  Bin  Rel Open Close Punct Inner
    /* Ord */   {0,  3,  -4,  -5,  0,    0,    0,   -3},
    /* Op */    {3,  3,   0,  -5,  0,    0,    0,   -3},
    /* Bin */   {-4, -4,  0,   0, -4,    0,    0,   -4},
    /* Rel */   {-5, -5,  0,   0, -5,    0,    0,   -5},
    /* Open */  {0,  0,   0,   0,  0,    0,    0,    0},
    /* Close */ {0,  3,  -4,  -5,  0,    0,    0,   -3},
    /* Punct */ {-3, -3,  0,  -3, -3,   -3,   -3,   -3},
    /* Inner */ {-3, 3,  -4,  -5, -3,    0,   -3,   -3},
};

uint8_t spaceBetween(TexClass left, TexClass right, TexStyle style) {
    if (left >= TexClass_Middle || right >= TexClass_Middle)
        return 0;
    int8_t space = kSpaces[left][right];
    if (space >= 0)
        return uint8_t(space);
    return style >= TexStyle_Script ? 0 : uint8_t(-space);
}

TexNodeIndex makeApplyFunction(TexTree* tree, TexStyle style) {
    TexNodeIndex node = texNodeCreate(tree, TexElement_Mo, TexClass_None, style);
    texNodeSetText(tree, node, "⁡");
    nodeAt(tree, node).flags |= TexNodeFlag_ApplyFunction;
    return node;
}

// Inserts U+2061 after each operator name that is followed by an atom that it applies to, which is an Ord, an Op, an
// Open or an Inner.
void applyFunctions(TexTree* tree, TexNodeIndex node, RawArray<Atom>* atoms) {
    if (isList(tree, node)) {
        atoms->clear();
        collectAtoms(tree, node, atoms);
        demoteBinaries(atoms, nodeAt(tree, node).flags & TexNodeFlag_LeadingOrd);
        for (uint32_t i = 0; i + 1 < atoms->count; ++i) {
            const Atom& atom = atoms->data[i];
            if (!(nodeAt(tree, atom.node).flags & TexNodeFlag_Function) || atom.applyFunction != kTexNodeNone)
                continue;
            TexClass next = atoms->data[i + 1].texClass;
            if (next != TexClass_Ord && next != TexClass_Op && next != TexClass_Open && next != TexClass_Inner)
                continue;
            TexNodeIndex apply = makeApplyFunction(tree, nodeAt(tree, atom.node).style);
            texNodeInsertBefore(tree, atom.parent, apply, nodeAt(tree, atom.node).next);
        }
    }
    for (TexNodeIndex child = nodeAt(tree, node).firstChild; child != kTexNodeNone; child = nodeAt(tree, child).next)
        applyFunctions(tree, child, atoms);
}

bool isScripted(TexElement element) {
    switch (element) {
        case TexElement_Msub:
        case TexElement_Msup:
        case TexElement_Msubsup:
        case TexElement_Munder:
        case TexElement_Mover:
        case TexElement_Munderover:
        case TexElement_Mmultiscripts:
            return true;
        default:
            return false;
    }
}

// Decides the form of each operator by its place in the element that holds its outermost embellished operator
// (mathml-core/#dfn-form). The first of several in a row is prefix, the last is postfix, a script is postfix, and any
// other is infix.
void decideForms(TexTree* tree, TexNodeIndex node) {
    TexNode& n = nodeAt(tree, node);
    bool row = texElementIsRow(n.element) || n.element == TexElement_Semantics;
    TexNodeIndex end = kTexNodeNone;
    if (n.element == TexElement_Semantics && n.firstChild != kTexNodeNone)
        end = nodeAt(tree, n.firstChild).next;
    if (row) {
        uint32_t count = 0;
        for (TexNodeIndex child = n.firstChild; child != end; child = nodeAt(tree, child).next) {
            if (!texNodeIsSpaceLike(tree, child))
                ++count;
        }
        uint32_t index = 0;
        for (TexNodeIndex child = n.firstChild; child != end; child = nodeAt(tree, child).next) {
            if (texNodeIsSpaceLike(tree, child))
                continue;
            if (texNodeIsEmbellished(tree, child)) {
                TexNode& core = nodeAt(tree, texNodeCore(tree, child));
                if (!(core.flags & TexNodeFlag_Formed)) {
                    core.flags |= TexNodeFlag_Formed;
                    core.form = TexForm_Infix;
                    if (count > 1 && index == 0)
                        core.form = TexForm_Prefix;
                    else if (count > 1 && index + 1 == count)
                        core.form = TexForm_Postfix;
                }
            }
            ++index;
        }
    } else if (isScripted(n.element)) {
        bool underOver = n.element == TexElement_Munder || n.element == TexElement_Mover ||
            n.element == TexElement_Munderover;
        uint32_t index = 0;
        for (TexNodeIndex child = n.firstChild; child != kTexNodeNone; child = nodeAt(tree, child).next, ++index) {
            if (!texNodeIsEmbellished(tree, child))
                continue;
            TexNode& core = nodeAt(tree, texNodeCore(tree, child));
            if (underOver)
                core.flags |= TexNodeFlag_UnderOver;
            if (index != 0 && !(core.flags & TexNodeFlag_Formed)) {
                core.flags |= TexNodeFlag_Formed;
                core.form = TexForm_Postfix;
            }
        }
    }
    for (TexNodeIndex child = nodeAt(tree, node).firstChild; child != end; child = nodeAt(tree, child).next)
        decideForms(tree, child);
}

// Returns the core operator of an atom, which takes the space on the atom's side, or kTexNodeNone if the atom is not an
// embellished operator.
TexNodeIndex coreOf(const TexTree* tree, TexNodeIndex node) {
    return texNodeIsEmbellished(tree, node) ? texNodeCore(tree, node) : kTexNodeNone;
}

TexCategoryValues propertiesOf(const TexTree* tree, TexNodeIndex core) {
    return texOperatorDictionary(texNodeText(tree, core), tree->nodes.data[core].form, false);
}

// The space in mu on each side of \bmod's operator in every style. amsmath cancels the binary operator's \medmuskip and
// adds 5mu of its own.
constexpr uint8_t kModSpace = 5;

TexNodeIndex makeSpaceNode(TexTree* tree, uint8_t mu, TexStyle style) {
    TexNodeIndex space = texNodeCreate(tree, TexElement_Mspace, TexClass_None, style);
    static const char* const kWidths[] = {"0em", "0.0556em", "0.1111em", "0.1667em", "0.2222em", "0.2778em"};
    texNodeSetAttribute(tree, space, "width", mu < 6 ? kWidths[mu] : "0.3333em");
    return space;
}

// Lays out the spaces between the atoms of the list `node`.
void spaceList(TexTree* tree, TexNodeIndex node, RawArray<Atom>* atoms) {
    // A row that is itself an embellished operator leaves its operator's spaces to the row around it
    // (mathml-core/#layout-of-mrow).
    const TexNode& list = nodeAt(tree, node);
    if (list.element != TexElement_Math && texNodeIsEmbellished(tree, node))
        return;
    atoms->clear();
    collectAtoms(tree, node, atoms);
    bool leadingOrd = list.flags & TexNodeFlag_LeadingOrd;
    demoteBinaries(atoms, leadingOrd);
    for (uint32_t i = 0; i < atoms->count; ++i) {
        const Atom& atom = atoms->data[i];
        TexNodeIndex left = coreOf(tree, atom.node);
        if (left != kTexNodeNone)
            nodeAt(tree, left).flags |= TexNodeFlag_Spaced;
        if (atom.applyFunction != kTexNodeNone)
            nodeAt(tree, atom.applyFunction).flags |= TexNodeFlag_Spaced;
    }
    for (uint32_t i = 0; i < atoms->count; ++i) {
        const Atom& atom = atoms->data[i];
        bool hasPrevious = i > 0 || leadingOrd;
        if (!hasPrevious)
            continue;
        TexNodeIndex b = coreOf(tree, atom.node);
        TexNodeIndex a = kTexNodeNone;
        TexClass previousClass = TexClass_Ord;
        bool previousMod = false;
        if (i > 0) {
            const Atom& previous = atoms->data[i - 1];
            a = previous.applyFunction != kTexNodeNone ? previous.applyFunction : coreOf(tree, previous.node);
            previousClass = previous.texClass;
            previousMod = nodeAt(tree, previous.node).flags & TexNodeFlag_ModSpacing;
        }
        bool mod = nodeAt(tree, atom.node).flags & TexNodeFlag_ModSpacing;
        uint8_t space =
            spaceBetween(rightClass(previousClass), leftClass(atom.texClass), nodeAt(tree, atom.node).style);
        // `onLeft` of the space goes to the rspace of the left atom's operator, and the rest to the lspace of the right
        // atom's operator.
        uint8_t onLeft = 0;
        if (previousMod) {
            space = kModSpace;
            onLeft = space;
        } else if (mod) {
            space = kModSpace;
            onLeft = 0;
        } else if (a != kTexNodeNone && b != kTexNodeNone) {
            uint8_t dictionaryRight = propertiesOf(tree, a).rspace;
            uint8_t dictionaryLeft = propertiesOf(tree, b).lspace;
            // Choose the split that differs from the dictionary on the fewest sides, so that the writer writes the
            // fewest attributes.
            uint8_t candidates[] = {dictionaryRight, uint8_t(space >= dictionaryLeft ? space - dictionaryLeft : 0), space, 0};
            uint32_t bestCost = 3;
            for (uint8_t candidate : candidates) {
                if (candidate > space)
                    continue;
                uint32_t cost = uint32_t(candidate != dictionaryRight) + uint32_t(space - candidate != dictionaryLeft);
                if (cost < bestCost) {
                    bestCost = cost;
                    onLeft = candidate;
                }
            }
        } else if (a != kTexNodeNone) {
            onLeft = space;
        }
        if (a == kTexNodeNone && b == kTexNodeNone) {
            if (space != 0) {
                TexNodeIndex spacer = makeSpaceNode(tree, space, nodeAt(tree, atom.node).style);
                texNodeInsertBefore(tree, atom.parent, spacer, atom.node);
            }
            continue;
        }
        if (a != kTexNodeNone)
            nodeAt(tree, a).rspace = onLeft;
        if (b != kTexNodeNone)
            nodeAt(tree, b).lspace = uint8_t(space - (a != kTexNodeNone ? onLeft : 0));
    }
    // \bmod's operator keeps its space at the ends of its list.
    if (atoms->count != 0) {
        const Atom& first = atoms->data[0];
        if ((nodeAt(tree, first.node).flags & TexNodeFlag_ModSpacing) && !leadingOrd)
            nodeAt(tree, coreOf(tree, first.node)).lspace = kModSpace;
        const Atom& last = atoms->last();
        if (nodeAt(tree, last.node).flags & TexNodeFlag_ModSpacing)
            nodeAt(tree, coreOf(tree, last.node)).rspace = kModSpace;
    }
}

void spaceLists(TexTree* tree, TexNodeIndex node, RawArray<Atom>* atoms) {
    if (isList(tree, node))
        spaceList(tree, node, atoms);
    for (TexNodeIndex child = nodeAt(tree, node).firstChild; child != kTexNodeNone; child = nodeAt(tree, child).next)
        spaceLists(tree, child, atoms);
}

} // namespace

void texTreeSpace(TexTree* tree) {
    if (tree->root == kTexNodeNone)
        return;
    RawArray<Atom> atoms{};
    applyFunctions(tree, tree->root, &atoms);
    decideForms(tree, tree->root);
    spaceLists(tree, tree->root, &atoms);
    atoms.reset();
}

} // namespace pr_impl
