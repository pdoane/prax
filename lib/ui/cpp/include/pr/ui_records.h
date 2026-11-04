// Records in C++: group recipes declared as types, and the record of a part made from its recipes through the context's
// memo (pr/ui.h, Records).
//
// A group recipe is a small struct of plain data -- no padding, no pointers, flags as bool -- that holds only what its
// part varies in one group. Its type is its identity. It names its kind as `static constexpr PrLayoutGroup kKind`, and
// its member `void make(const Owner& owner, Value& value) const` writes its group. Owner is the library's object that
// the recipe reads, such as its theme. Value is the kind's value (UiGroupValue), which starts as layout's default. A
// recipe that derives its group from another group of its kind names that group in a field `base`.

#pragma once

#include "pr/mem.h"
#include "pr/type_traits.h"
#include "pr/ui.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// The value of a group of each kind, which a recipe's make writes.
template <PrLayoutGroup Kind>
struct UiGroupValue;
template <>
struct UiGroupValue<PrLayoutGroup_Font> {
    using Type = PrLayoutFontStyle;
};
template <>
struct UiGroupValue<PrLayoutGroup_Color> {
    using Type = PrLayoutColorStyle;
};
template <>
struct UiGroupValue<PrLayoutGroup_Box> {
    using Type = PrLayoutBoxStyle;
};
template <>
struct UiGroupValue<PrLayoutGroup_Decoration> {
    using Type = PrLayoutDecorationStyle;
};
template <>
struct UiGroupValue<PrLayoutGroup_Effect> {
    using Type = PrLayoutEffectStyle;
};
template <>
struct UiGroupValue<PrLayoutGroup_Flex> {
    using Type = PrLayoutFlexStyle;
};
template <>
struct UiGroupValue<PrLayoutGroup_Table> {
    using Type = PrLayoutTableStyle;
};
template <>
struct UiGroupValue<PrLayoutGroup_Grid> {
    using Type = PrLayoutGridStyle;
};
template <>
struct UiGroupValue<PrLayoutGroup_Math> {
    using Type = PrLayoutMathStyle;
};

// Returns one address for each type, whichever file names it: the identity of a recipe, and of a part.
template <typename T>
const void* uiTypeId() {
    static const char id = 0;
    return &id;
}

// Returns the number of bytes of a recipe of type `Recipe`. A recipe with no fields has none, because its type
// identifies it.
template <typename Recipe>
constexpr uint32_t uiRecipeSize() {
    return isEmpty<Recipe> ? 0 : uint32_t(sizeof(Recipe));
}

// Returns the description of `recipe`, owned by `owner`, that the context takes.
template <typename Owner, typename Recipe>
PrUiGroupRecipe uiGroupRecipe(const Owner& owner, const Recipe& recipe) {
    PrLayoutGroupIndex base = 0;
    if constexpr (requires { recipe.base; })
        base = recipe.base;
    using Value = typename UiGroupValue<Recipe::kKind>::Type;
    return PrUiGroupRecipe{
        .type = uiTypeId<Recipe>(),
        .bytes = &recipe,
        .size = uiRecipeSize<Recipe>(),
        .kind = Recipe::kKind,
        .base = base,
        .make = [](const void* PR_NULLABLE o, const void* PR_NULLABLE r, void* value) {
            static_cast<const Recipe*>(r)->make(*static_cast<const Owner*>(o), *static_cast<Value*>(value));
        },
        .owner = &owner,
    };
}

// Returns the group of `recipe`, owned by `owner` (prUiGroup).
template <typename Owner, typename Recipe>
PrLayoutGroupIndex uiGroup(PrUi* ui, const Owner& owner, const Recipe& recipe) {
    PrUiGroupRecipe described = uiGroupRecipe(owner, recipe);
    return prUiGroup(ui, &described);
}

// The type whose identity (uiTypeId) identifies a part of the recipe types `Groups`.
template <typename... Groups>
struct UiPartOf {};

// Returns true if no two of `Groups` are of one kind.
template <typename... Groups>
constexpr bool uiKindsDistinct() {
    if constexpr (sizeof...(Groups) < 2) {
        return true;
    } else {
        const PrLayoutGroup kinds[] = {Groups::kKind...};
        for (uint32_t i = 0; i < sizeof...(Groups); ++i) {
            for (uint32_t j = i + 1; j < sizeof...(Groups); ++j) {
                if (kinds[i] == kinds[j])
                    return false;
            }
        }
        return true;
    }
}

// Copies `recipe`'s bytes to `bytes`, and moves `bytes` past them.
template <typename Recipe>
void uiPack(uint8_t* PR_NONNULL& bytes, const Recipe& recipe) {
    constexpr uint32_t size = uiRecipeSize<Recipe>();
    prMemCopy(bytes, &recipe, size);
    bytes += size;
}

// Returns the record of the part that `groups` make, owned by `owner`. The record names the group of each recipe, which
// is of its own kind, and index 0 of each kind that no recipe gives. A part that is unchanged since the last frame
// costs one comparison.
template <typename Owner, typename... Groups>
PrLayoutStyleIndex uiStyle(PrUi* ui, const Owner& owner, const Groups&... groups) {
    static_assert(uiKindsDistinct<Groups...>(), "a part sets each kind of group once");
    if constexpr (sizeof...(Groups) == 0) {
        return 0;
    } else {
        // The part's bytes, packed, so that the memo compares them at once.
        constexpr uint32_t size = (uiRecipeSize<Groups>() + ...);
        uint8_t key[size > 0 ? size : 1];
        uint8_t* packed = key;
        (uiPack(packed, groups), ...);
        const void* type = uiTypeId<UiPartOf<Groups...>>();
        PrLayoutStyleIndex style = 0;
        const void* kept = prUiMemoFind(ui, type, &owner, &style);
        if (kept != nullptr && prMemEquals(kept, key, size))
            return style;
        PrUiPart part{.type = type, .owner = &owner, .bytes = key, .size = size};
        PrLayoutStyle record = prLayoutStyleDefault();
        ((record.groups[Groups::kKind] = uiGroup(ui, owner, groups)), ...);
        return prUiMemoKeep(ui, &part, prLayoutStyleIntern(prUiTree(ui), &record), 0);
    }
}

} // namespace pr

PR_ASSUME_NONNULL_END
