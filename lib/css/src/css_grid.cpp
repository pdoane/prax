// Grid templates in CSS: the strings of grid-template-areas, read as rows of cell tokens.

#include "css/src/css_grid.h"

#include "pr/raw_array.h"

namespace pr_impl {

namespace {

// The cells of one name in a template: the rectangle that bounds them, and how many there are.
struct NamedCells {
    PrStringView name;
    uint32_t rowStart;
    uint32_t rowEnd;
    uint32_t columnStart;
    uint32_t columnEnd;
    uint32_t count;
};

} // namespace

bool cssGridAreasFold(CssValuePool* pool, uint32_t first, uint32_t count) {
    RawArray<NamedCells> names;
    uint32_t rows = 0;
    uint32_t columns = 0;
    bool valid = true;
    for (uint32_t i = first; i < first + count && valid; i += pool->values.data[i].next) {
        const CssValueNode& value = pool->values.data[i];
        if (value.kind != PrCssValueKind_String)
            continue;
        PrStringView row = cssValuePoolText(pool, &value);
        uint32_t column = 0;
        valid = cssGridAreaCells(row, [&](PrStringView name) {
            if (name.size != 0) {
                uint32_t at = 0;
                while (at < names.count && names.data[at].name != name)
                    ++at;
                if (at == names.count)
                    names.push(NamedCells{name, rows, rows + 1, column, column + 1, 0});
                NamedCells& cells = names.data[at];
                cells.rowEnd = rows + 1;
                cells.columnStart = column < cells.columnStart ? column : cells.columnStart;
                cells.columnEnd = column + 1 > cells.columnEnd ? column + 1 : cells.columnEnd;
                ++cells.count;
            }
            ++column;
        });
        if (rows == 0)
            columns = column;
        valid = valid && column == columns && columns != 0;
        ++rows;
    }
    // A name's cells fill their rectangle if there are as many as it holds, since each cell is counted once.
    for (uint32_t k = 0; k < names.count && valid; ++k) {
        const NamedCells& cells = names.data[k];
        valid = cells.count == (cells.rowEnd - cells.rowStart) * (cells.columnEnd - cells.columnStart);
    }
    names.reset();
    if (!valid)
        return false;
    // The canonical text of each string goes onto the end of the pool's text, which its value then names.
    RawArray<char> text;
    for (uint32_t i = first; i < first + count; i += pool->values.data[i].next) {
        if (pool->values.data[i].kind != PrCssValueKind_String)
            continue;
        text.clear();
        cssGridAreaCells(cssValuePoolText(pool, &pool->values.data[i]), [&](PrStringView name) {
            if (text.count != 0)
                text.push(' ');
            if (name.size == 0)
                text.push('.');
            else
                text.append(name.data, uint32_t(name.size));
        });
        uint32_t start = cssValuePoolAddText(pool, PrStringView{text.data, text.count});
        pool->values.data[i].text.start = start;
        pool->values.data[i].text.length = text.count;
    }
    text.reset();
    return true;
}

} // namespace pr_impl
