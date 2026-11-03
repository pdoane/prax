// The style classes of the editor (text_editor_style.h): the words of category names that give each class, and the
// theme's colors.

#include "studio/text_editor/src/text_editor_style.h"

#include "pr/count_of.h"

namespace pr_impl {

namespace {

// A word of a category's name, and the class that it gives.
struct ClassWord {
    const char* word;
    TextEditorStyleClass style;
};

// The classes by the first word of a category's name. The name of a markup element, such as a tag, reads as its
// structure, as a keyword does.
constexpr ClassWord kClassOfFirstWord[] = {
    {"attribute", TextEditorStyleClass_Attribute},
    {"boolean", TextEditorStyleClass_Constant},
    {"character", TextEditorStyleClass_Constant},
    {"comment", TextEditorStyleClass_Comment},
    {"constant", TextEditorStyleClass_Constant},
    {"constructor", TextEditorStyleClass_Type},
    {"delimiter", TextEditorStyleClass_Punctuation},
    {"escape", TextEditorStyleClass_Escape},
    {"field", TextEditorStyleClass_Member},
    {"float", TextEditorStyleClass_Number},
    {"function", TextEditorStyleClass_Function},
    {"keyword", TextEditorStyleClass_Keyword},
    {"label", TextEditorStyleClass_Variable},
    {"member", TextEditorStyleClass_Member},
    {"method", TextEditorStyleClass_Function},
    {"module", TextEditorStyleClass_Type},
    {"namespace", TextEditorStyleClass_Type},
    {"number", TextEditorStyleClass_Number},
    {"operator", TextEditorStyleClass_Operator},
    {"parameter", TextEditorStyleClass_Variable},
    {"preproc", TextEditorStyleClass_Keyword},
    {"property", TextEditorStyleClass_Member},
    {"punctuation", TextEditorStyleClass_Punctuation},
    {"string", TextEditorStyleClass_String},
    {"tag", TextEditorStyleClass_Keyword},
    {"type", TextEditorStyleClass_Type},
    {"variable", TextEditorStyleClass_Variable},
};

// The classes of markup, by the second word of a name whose first is `text` or `markup`.
constexpr ClassWord kClassOfMarkupWord[] = {
    {"bold", TextEditorStyleClass_Strong},
    {"emphasis", TextEditorStyleClass_Emphasis},
    {"heading", TextEditorStyleClass_Heading},
    {"italic", TextEditorStyleClass_Emphasis},
    {"link", TextEditorStyleClass_LinkTarget},
    {"literal", TextEditorStyleClass_String},
    {"raw", TextEditorStyleClass_String},
    {"reference", TextEditorStyleClass_Link},
    {"strong", TextEditorStyleClass_Strong},
    {"title", TextEditorStyleClass_Heading},
    {"uri", TextEditorStyleClass_LinkTarget},
};

// The color of a class's text in a dark theme and in a light one, or zero alpha for the text's own.
struct ClassColor {
    PrColor8 dark;
    PrColor8 light;
};

constexpr ClassColor kClassColors[TextEditorStyleClass_Count] = {
    {},                                                 // None
    {{197, 134, 192, 255}, {175, 0, 219, 255}},         // Keyword
    {{78, 201, 176, 255}, {38, 127, 153, 255}},         // Type
    {{220, 220, 170, 255}, {121, 94, 38, 255}},         // Function
    {{156, 220, 254, 255}, {0, 16, 128, 255}},          // Member
    {},                                                 // Variable
    {{206, 145, 120, 255}, {163, 21, 21, 255}},         // String
    {{215, 186, 125, 255}, {238, 0, 0, 255}},           // Escape
    {{181, 206, 168, 255}, {9, 134, 88, 255}},          // Number
    {{79, 193, 255, 255}, {0, 112, 193, 255}},          // Constant
    {{106, 153, 85, 255}, {0, 128, 0, 255}},            // Comment
    {{156, 220, 254, 255}, {229, 0, 0, 255}},           // Attribute
    {},                                                 // Operator
    {{150, 150, 150, 255}, {110, 110, 110, 255}},       // Punctuation
    {{86, 156, 214, 255}, {128, 0, 0, 255}},            // Heading
    {{197, 134, 192, 255}, {175, 0, 219, 255}},         // Strong
    {{206, 145, 120, 255}, {163, 21, 21, 255}},         // Emphasis
    {{86, 156, 214, 255}, {0, 0, 255, 255}},            // Link
    {{156, 220, 254, 255}, {0, 16, 128, 255}},          // LinkTarget
};

// Returns the word of `name` that starts at `from`, up to the next dot.
PrStringView wordAt(PrStringView name, size_t from) {
    size_t end = from;
    while (end < name.size && name.data[end] != '.')
        ++end;
    return PrStringView{name.data + from, end - from};
}

TextEditorStyleClass classOfWord(const ClassWord* words, size_t count, PrStringView word) {
    for (size_t i = 0; i < count; ++i) {
        if (word == words[i].word)
            return words[i].style;
    }
    return TextEditorStyleClass_None;
}

} // namespace

TextEditorStyleClass textEditorStyleClassFromCategory(PrStringView name) {
    PrStringView first = wordAt(name, 0);
    if (first == "text" || first == "markup") {
        if (first.size >= name.size)
            return TextEditorStyleClass_None;
        return classOfWord(kClassOfMarkupWord, countOf(kClassOfMarkupWord), wordAt(name, first.size + 1));
    }
    return classOfWord(kClassOfFirstWord, countOf(kClassOfFirstWord), first);
}

PrColor16 textEditorStyleClassColor(TextEditorStyleClass style, bool dark) {
    const ClassColor& color = kClassColors[style];
    PrColor8 shown = dark ? color.dark : color.light;
    return shown.a != 0 ? prColor16FromColor(prColorFromColor8(shown)) : PrColor16{};
}

} // namespace pr_impl
