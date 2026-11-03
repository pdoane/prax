// Path data in CSS values: the commands of a path().

#include "css/src/css_path.h"

#include "css/src/css_keyword.h"
#include "css/src/css_serialize.h"

namespace pr_impl {

void cssValuePoolAddPathData(CssValuePool* pool, const PrPathDataCommand* PR_NULLABLE commands, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const PrPathDataCommand& command = commands[i];
        cssValuePoolAdd(pool, cssLiteralValue(command.letter));
        uint32_t arguments = prPathDataArgumentCount(command.letter);
        for (uint32_t k = 0; k < arguments; ++k) {
            double argument = double(command.arguments[k]);
            cssValuePoolAdd(pool, cssNumberValue(PrCssValueKind_Number, PrCssUnit_Number, argument));
        }
    }
}

uint32_t cssPathDataStart(CssValueRun run, uint32_t index) {
    uint32_t start = index + 1;
    uint32_t end = index + run.values[index].next;
    if (start < end && run.values[start].kind == PrCssValueKind_Keyword) {
        ++start;
        if (start < end && cssValueIsLiteral(&run.values[start], ','))
            ++start;
    }
    return start;
}

void cssPathCommands(CssValueRun run, uint32_t index, Array<PrPathDataCommand>* out) {
    uint32_t end = index + run.values[index].next;
    uint32_t argument = 0;
    for (uint32_t i = cssPathDataStart(run, index); i < end; ++i) {
        const CssValueNode* value = &run.values[i];
        if (value->kind == PrCssValueKind_Literal) {
            PrPathDataCommand command{};
            command.letter = cssValueLiteral(value);
            out->push(command);
            argument = 0;
        } else if (out->count != 0 && argument < kPrPathDataCommandMaxArguments) {
            out->last().arguments[argument++] = float(value->number);
        }
    }
}

void cssValuePoolAddAbsolutePath(CssValuePool* pool, CssValueRun run, uint32_t index) {
    Array<PrPathDataCommand> commands;
    cssPathCommands(run, index, &commands);
    prPathDataMakeAbsolute(commands.data, commands.count);
    uint32_t function = cssValuePoolAdd(pool, run.values[index]);
    uint32_t start = cssPathDataStart(run, index);
    cssValuePoolAddRun(pool, CssValueRun{run.values + index + 1, start - index - 1, run.text});
    cssValuePoolAddPathData(pool, commands.data, commands.count);
    pool->values.data[function].next = pool->values.count - function;
    commands.reset();
}

void cssSerializePath(PrTextBuffer* out, CssValueRun run, uint32_t index) {
    prTextBufferAppend(out, "path(");
    if (run.values[index].next > 1 && run.values[index + 1].kind == PrCssValueKind_Keyword) {
        prTextBufferAppend(out, cssKeywordName(cssValueKeyword(&run.values[index + 1])));
        prTextBufferAppend(out, ", ");
    }
    Array<PrPathDataCommand> commands;
    cssPathCommands(run, index, &commands);
    PrString data = prStringNull();
    prPathDataWrite(commands.data, commands.count, &data);
    cssSerializeString(out, prStringView(&data));
    prTextBufferAppendChar(out, ')');
    prStringReset(&data);
    commands.reset();
}

} // namespace pr_impl
