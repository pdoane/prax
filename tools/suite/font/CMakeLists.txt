# The harness of the font-freetype suite (tools/suite/font_freetype.py): lib/font's readers beside FreeType.
pr_harness(font_compare prax::font font_compare.cpp)
target_link_libraries(font_compare PRIVATE freetype prax::path prax::core_cpp)
