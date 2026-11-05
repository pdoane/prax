// Draw structures: what the content of draw lists means, as a tree in reading order whose leaves are the producer's
// keys that the lists' marks carry (prDrawListPushMark).

#include "pr/draw_structure.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/raw_array.h"

namespace pr_impl {

namespace {

constexpr PrStringView kTypeNames[PrDrawStructureType_Count] = {
    "Document", "DocumentFragment", "Part", "Sect", "Div", "Aside", "NonStruct", "P", "H1", "H2", "H3", "H4", "H5", "H6",
    "Title", "FENote", "Sub", "Lbl", "Span", "Em", "Strong", "Link", "Annot", "Form", "Ruby", "RB", "RT", "RP", "L", "LI",
    "LBody", "Table", "TR", "TH", "TD", "THead", "TBody", "TFoot", "Caption", "Figure", "Formula", "Artifact",
    "BlockQuote", "Quote", "Code", "Note", "Reference", "BibEntry", "Index", "TOC", "TOCI",
};

// A node, with its strings in the structure's text and its children in the structure's items.
struct StructureNode {
    PrDrawStructureNodeDesc desc;   // its strings are offsets into the text until they are read
    PrDrawStructureNode parent;
    RawArray<PrDrawStructureItem> items;
};

// A string of a node, as an offset and a size in the structure's text.
struct TextRange {
    uint32_t start;
    uint32_t size;
};

struct Structure {
    RawArray<StructureNode> nodes;
    RawArray<char> text;                // every string of every node
    RawArray<TextRange> ranges;         // by node, five for each: alt, actual text, expansion, language and title
    RawArray<TextRange> uris;           // by node
};

constexpr uint32_t kRangesPerNode = 5;

TextRange store(Structure& s, PrStringView text) {
    TextRange range{s.text.count, uint32_t(text.size)};
    if (text.size != 0)
        s.text.append(text.data, uint32_t(text.size));
    return range;
}

PrStringView viewOf(const Structure& s, TextRange range) {
    if (range.size == 0)
        return PrStringView{};
    return PrStringView{s.text.data + range.start, range.size};
}

PrDrawStructureNode append(Structure& s, PrDrawStructureNode parent, const PrDrawStructureNodeDesc& desc) {
    PR_ASSERT(desc.type < PrDrawStructureType_Count);
    PrDrawStructureNode node = s.nodes.count;
    StructureNode& made = s.nodes.emplace();
    made = StructureNode{};
    made.desc = desc;
    made.desc.alt = made.desc.actualText = made.desc.expansion = made.desc.language = made.desc.title =
        made.desc.uri = PrStringView{};
    made.parent = parent;
    s.ranges.push(store(s, desc.alt));
    s.ranges.push(store(s, desc.actualText));
    s.ranges.push(store(s, desc.expansion));
    s.ranges.push(store(s, desc.language));
    s.ranges.push(store(s, desc.title));
    s.uris.push(store(s, desc.uri));
    if (parent != kPrDrawStructureNodeNone)
        s.nodes.data[parent].items.push(PrDrawStructureItem{.node = node});
    return node;
}

PrDrawStructureNodeDesc descOf(const Structure& s, PrDrawStructureNode node) {
    PR_ASSERT(node < s.nodes.count);
    PrDrawStructureNodeDesc desc = s.nodes.data[node].desc;
    const TextRange* ranges = s.ranges.data + node * kRangesPerNode;
    desc.alt = viewOf(s, ranges[0]);
    desc.actualText = viewOf(s, ranges[1]);
    desc.expansion = viewOf(s, ranges[2]);
    desc.language = viewOf(s, ranges[3]);
    desc.title = viewOf(s, ranges[4]);
    desc.uri = viewOf(s, s.uris.data[node]);
    return desc;
}

void dumpString(PrRawString* out, PrStringView name, PrStringView value) {
    if (value.size == 0)
        return;
    prRawStringAppend(out, " ");
    prRawStringAppend(out, name);
    prRawStringAppend(out, "=\"");
    prRawStringAppend(out, value);
    prRawStringAppend(out, "\"");
}

void indent(PrRawString* out, uint32_t depth) {
    for (uint32_t i = 0; i < depth; ++i)
        prRawStringAppend(out, "  ");
}

void dumpNode(const Structure& s, PrDrawStructureNode node, uint32_t depth, PrRawString* out) {
    indent(out, depth);
    PrDrawStructureNodeDesc desc = descOf(s, node);
    prRawStringAppend(out, kTypeNames[desc.type]);
    dumpString(out, "alt", desc.alt);
    dumpString(out, "actual", desc.actualText);
    dumpString(out, "expansion", desc.expansion);
    dumpString(out, "lang", desc.language);
    dumpString(out, "title", desc.title);
    dumpString(out, "uri", desc.uri);
    if (desc.destination != 0) {
        prRawStringAppend(out, " destination=");
        prRawStringAppendU64(out, desc.destination, 10);
    }
    prRawStringAppend(out, "\n");
    const RawArray<PrDrawStructureItem>& items = s.nodes.data[node].items;
    for (uint32_t i = 0; i < items.count; ++i) {
        if (items.data[i].node != kPrDrawStructureNodeNone) {
            dumpNode(s, items.data[i].node, depth + 1, out);
            continue;
        }
        indent(out, depth + 1);
        prRawStringAppend(out, "key ");
        prRawStringAppendU64(out, items.data[i].key, 10);
        prRawStringAppend(out, "\n");
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrDrawStructure : Structure {};

PrStringView prDrawStructureTypeName(PrDrawStructureType type) {
    return type < PrDrawStructureType_Count ? kTypeNames[type] : PrStringView{};
}

bool prDrawStructureTypeIsLegacy(PrDrawStructureType type) {
    return type >= PrDrawStructureType_BlockQuote && type < PrDrawStructureType_Count;
}

uint32_t prDrawStructureTypeHeadingLevel(PrDrawStructureType type) {
    if (type >= PrDrawStructureType_H1 && type <= PrDrawStructureType_H6)
        return uint32_t(type - PrDrawStructureType_H1) + 1;
    return 0;
}

PrDrawStructureNodeDesc prDrawStructureNodeDescDefault(PrDrawStructureType type) {
    PrDrawStructureNodeDesc desc{};
    desc.type = type;
    return desc;
}

PrDrawStructure* prDrawStructureCreate(const PrDrawStructureNodeDesc* PR_NULLABLE root) {
    PrDrawStructure* structure = allocZeroed<PrDrawStructure>();
    PrDrawStructureNodeDesc desc =
        root != nullptr ? *root : prDrawStructureNodeDescDefault(PrDrawStructureType_Document);
    append(*structure, kPrDrawStructureNodeNone, desc);
    return structure;
}

void prDrawStructureDestroy(PrDrawStructure* PR_NULLABLE structure) {
    if (structure == nullptr)
        return;
    for (uint32_t i = 0; i < structure->nodes.count; ++i)
        structure->nodes.data[i].items.reset();
    structure->nodes.reset();
    structure->text.reset();
    structure->ranges.reset();
    structure->uris.reset();
    prMemoryFree(structure);
}

PrDrawStructureNode prDrawStructureAppendNode(PrDrawStructure* structure, PrDrawStructureNode parent,
        const PrDrawStructureNodeDesc* desc) {
    PR_ASSERT(parent < structure->nodes.count);
    return append(*structure, parent, *desc);
}

void prDrawStructureAppendKey(PrDrawStructure* structure, PrDrawStructureNode parent, uint64_t key) {
    PR_ASSERT(parent < structure->nodes.count && key != 0);
    structure->nodes.data[parent].items.push(PrDrawStructureItem{.node = kPrDrawStructureNodeNone, .key = key});
}

uint32_t prDrawStructureNodeCount(const PrDrawStructure* structure) {
    return structure->nodes.count;
}

PrDrawStructureNodeDesc prDrawStructureNodeDesc(const PrDrawStructure* structure, PrDrawStructureNode node) {
    return descOf(*structure, node);
}

void prDrawStructureNodeSetDesc(PrDrawStructure* structure, PrDrawStructureNode node, const PrDrawStructureNodeDesc* desc) {
    PR_ASSERT(node < structure->nodes.count && desc->type < PrDrawStructureType_Count);
    Structure& s = *structure;
    s.nodes.data[node].desc = *desc;
    s.nodes.data[node].desc.alt = s.nodes.data[node].desc.actualText = s.nodes.data[node].desc.expansion =
        s.nodes.data[node].desc.language = s.nodes.data[node].desc.title = s.nodes.data[node].desc.uri = PrStringView{};
    TextRange* ranges = s.ranges.data + node * kRangesPerNode;
    ranges[0] = store(s, desc->alt);
    ranges[1] = store(s, desc->actualText);
    ranges[2] = store(s, desc->expansion);
    ranges[3] = store(s, desc->language);
    ranges[4] = store(s, desc->title);
    s.uris.data[node] = store(s, desc->uri);
}

PrDrawStructureNode prDrawStructureNodeParent(const PrDrawStructure* structure, PrDrawStructureNode node) {
    PR_ASSERT(node < structure->nodes.count);
    return structure->nodes.data[node].parent;
}

uint32_t prDrawStructureItemCount(const PrDrawStructure* structure, PrDrawStructureNode node) {
    PR_ASSERT(node < structure->nodes.count);
    return structure->nodes.data[node].items.count;
}

const PrDrawStructureItem* PR_NULLABLE prDrawStructureItems(const PrDrawStructure* structure, PrDrawStructureNode node) {
    PR_ASSERT(node < structure->nodes.count);
    return structure->nodes.data[node].items.data;
}

PrRawString prDrawStructureDump(const PrDrawStructure* structure) {
    PrRawString out = prRawStringEmpty();
    dumpNode(*structure, kPrDrawStructureRoot, 0, &out);
    return out;
}
