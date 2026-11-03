#include "dom/src/dom_xml.h"

#include "pr/charset.h"
#include "pr/raw_array.h"

#include "dom/src/dom_element_state.h"
#include "dom/src/dom_hooks.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_use.h"

namespace pr_impl {
namespace {

using Builder = DomXmlBuilder;

PrDomNode* parentNode(Builder* b) {
    return b->open.count != 0 ? static_cast<PrDomNode*>(b->open.last()) : b->document;
}

bool isScript(const PrDomElement* element) {
    return (element->space == PrDomNamespace_Html || element->space == PrDomNamespace_Svg) &&
        element->tag == PrDomTag_Script;
}

PrStatus startElement(void* context, PrXmlParser* parser, const PrXmlName* name,
        const PrXmlAttribute* PR_NULLABLE attributes, uint32_t attributeCount) {
    Builder* b = static_cast<Builder*>(context);
    DomNamespaceId space = PrDomNamespace_None;
    if (!domNamespaceFromUrl(b->document, name->space, &space))
        return prXmlParserFail(parser, "the document names too many namespaces");
    PrDomElement* element = domElementCreateNs(b->document, space, name->prefix, name->local);
    for (uint32_t i = 0; i < attributeCount; ++i) {
        const PrXmlAttribute& attribute = attributes[i];
        DomNamespaceId attributeSpace = PrDomNamespace_None;
        if (!domNamespaceFromUrl(b->document, attribute.name.space, &attributeSpace)) {
            domNodeRelease(element);
            return prXmlParserFail(parser, "the document names too many namespaces");
        }
        domSetAttributeNs(element, attributeSpace, attribute.name.prefix, attribute.name.local, attribute.value);
    }
    domScriptParserCreated(element, false);
    domAttach(parentNode(b), element, nullptr);
    // The insertion steps of a link obtain its sheet, and those of a meta set the document's default language. Embedded
    // content loads, and a use element joins its document's uses.
    domNotifyLinkUpdate(element, DomInsertedBy_Parser);
    domMetaConnected(element);
    domEmbedChanged(element, {});
    domUseConnectionChanged(element);
    domNodeRetain(element);
    b->open.push(element);
    return PrStatus_Ok;
}

// Runs the steps for closing an element: updates the sheet of a style element, and runs a script
// (html/#parsing-xhtml-documents).
PrStatus endElement(void* context, PrXmlParser* parser, const PrXmlName*) {
    Builder* b = static_cast<Builder*>(context);
    PrDomElement* element = b->open.data[--b->open.count];
    domNotifyStyleBlockUpdate(element, DomInsertedBy_Parser);
    PrStatus status = PrStatus_Ok;
    if (isScript(element) && b->scripting.run != nullptr && !prXmlParserIsInEntity(parser)) {
        PrXmlPosition at = prXmlParserContentStart(parser);
        status = b->scripting.run(b->scripting.context, element, at.line, at.column);
    }
    domNodeRelease(element);
    return status;
}

PrStatus text(void* context, PrXmlParser*, PrStringView data) {
    Builder* b = static_cast<Builder*>(context);
    domAttach(parentNode(b), domTextCreate(b->document, data), nullptr);
    return PrStatus_Ok;
}

PrStatus cdataSection(void* context, PrXmlParser*, PrStringView data) {
    Builder* b = static_cast<Builder*>(context);
    domAttach(parentNode(b), domTextCreateCdataSection(b->document, data), nullptr);
    return PrStatus_Ok;
}

PrStatus comment(void* context, PrXmlParser*, PrStringView data) {
    Builder* b = static_cast<Builder*>(context);
    domAttach(parentNode(b), domCommentCreate(b->document, data), nullptr);
    return PrStatus_Ok;
}

PrStatus processingInstruction(void* context, PrXmlParser*, PrStringView target, PrStringView data) {
    Builder* b = static_cast<Builder*>(context);
    PrDomProcessingInstruction* instruction = domProcessingInstructionCreate(b->document, target, data);
    domAttach(parentNode(b), instruction, nullptr);
    domNotifyInstructionInserted(instruction);
    return PrStatus_Ok;
}

PrStatus doctype(void* context, PrXmlParser*, PrStringView name, PrStringView publicId, PrStringView systemId) {
    Builder* b = static_cast<Builder*>(context);
    domAttach(b->document, domDocumentTypeCreate(b->document, name, publicId, systemId), nullptr);
    return PrStatus_Ok;
}

PrStringView entity(void* context, PrStringView publicId, PrStringView name) {
    const DomXmlEntities* entities = static_cast<Builder*>(context)->entities;
    return entities != nullptr ? entities->resolve(entities->context, publicId, name) : PrStringView{};
}

} // namespace

void domXmlBuilderInit(DomXmlBuilder* builder, PrDomDocument* document, DomXmlScripting scripting,
        const DomXmlEntities* PR_NULLABLE entities) {
    *builder = DomXmlBuilder{.document = document, .scripting = scripting, .entities = entities};
    builder->handler = PrXmlHandler{
        .context = builder,
        .startElement = startElement,
        .endElement = endElement,
        .text = text,
        .cdataSection = cdataSection,
        .comment = comment,
        .processingInstruction = processingInstruction,
        .doctype = doctype,
        .entity = entity,
    };
    builder->parser = prXmlParserCreate(&builder->handler);
}

void domXmlBuilderReset(DomXmlBuilder* builder) {
    prXmlParserDestroy(builder->parser);
    for (uint32_t i = 0; i < builder->open.count; ++i)
        domNodeRelease(builder->open.data[i]);
    builder->open.reset();
    *builder = DomXmlBuilder{};
}

PrStatus domXmlParse(PrDomDocument* document, PrStringView source, DomXmlScripting scripting,
        const DomXmlEntities* PR_NULLABLE entities, PrXmlError* PR_NULLABLE error) {
    DomXmlBuilder builder;
    domXmlBuilderInit(&builder, document, scripting, entities);
    prXmlParserAppend(builder.parser, source);
    prXmlParserEnd(builder.parser);
    PrStatus status = prXmlParserRun(builder.parser);
    while (status == PrStatus_Ok && !prXmlParserIsEnded(builder.parser))
        status = prXmlParserRun(builder.parser);
    if (error != nullptr)
        *error = prXmlParserError(builder.parser);
    domXmlBuilderReset(&builder);
    return status;
}

bool domXmlCharset(PrStringView bytes, PrCharset transport, bool complete, PrCharset* out) {
    if (!complete && !prCharsetIsBomDecided(bytes))
        return false;
    PrCharset charset = prCharsetSniffBom(bytes);
    if (charset == PrCharset_None)
        charset = transport;
    if (charset == PrCharset_None && !prXmlDeclaredCharset(bytes, complete, &charset))
        return false;
    *out = charset != PrCharset_None ? charset : PrCharset_Utf8;
    return true;
}

PrStringView domXmlDecode(PrStringView bytes, PrCharset transport, PrRawString* out, PrCharset* outCharset) {
    domXmlCharset(bytes, transport, true, outCharset);
    PrCharsetDecoder decoder = prCharsetDecoderMake(*outCharset, 0);
    return prCharsetDecodeView(&decoder, bytes, PrCharsetInput_Final, out);
}

} // namespace pr_impl

using namespace pr_impl;

PrDomDocument* prDomParseXml(PrStringView source, PrStringView contentType, PrXmlError* PR_NULLABLE error) {
    PrDomDocument* document = domDocumentCreate(DomDocumentKind_Xml);
    prRawStringReplace(&document->contentType, contentType);
    PrXmlError found = {};
    domXmlParse(document, source, {}, nullptr, &found);
    if (error != nullptr)
        *error = found;
    return document;
}
