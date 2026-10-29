#include "pr/byte_view.h"

#include <array>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

// Bytes convert, from a run or from a whole array. Text does not.
static_assert(std::is_convertible_v<std::vector<uint8_t>, PrByteView>);
static_assert(std::is_convertible_v<std::array<uint8_t, 4>, PrByteView>);
static_assert(std::is_convertible_v<const uint8_t (&)[4], PrByteView>);
static_assert(!std::is_convertible_v<std::string, PrByteView>);
static_assert(!std::is_convertible_v<const char*, PrByteView>);

constexpr uint8_t kBytes[] = {1, 0, 2};
static_assert(PrByteView(kBytes).size == 3);
static_assert(PrByteView(kBytes) == PrByteView(kBytes, 3));
static_assert(PrByteView{} == PrByteView(kBytes, 0));
