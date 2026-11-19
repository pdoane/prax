#include "pr/mime_sniff.h"
#include "pr/string.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

using namespace pr;

// Returns the name of the format that `bytes` sniff as, with `name`.
PrStringView sniffed(PrStringView bytes, PrStringView name = "") {
    PrByteView view{reinterpret_cast<const uint8_t*>(bytes.data), bytes.size};
    return prMimeFormatInfo(prMimeSniff(view, name)).name;
}

// The local header of a ZIP archive whose first entry is named `name`.
String zipWith(PrStringView name) {
    String header({"PK\x03\x04", 4});
    for (int i = 4; i < 26; ++i)
        header.append(1, '\0');
    header.append(1, char(name.size));
    header.append(1, '\0');
    header.append(1, '\0');
    header.append(1, '\0');
    header.append(name);
    return header;
}

// The start of an ISO base media file whose `ftyp` box has the major brand `brand`.
String ftyp(PrStringView brand) {
    String box({"\0\0\0\x14" "ftyp", 8});
    box.append(brand);
    box.append({"\0\0\0\0", 4});
    box.append(brand);
    return box;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("signatures identify images") {
    CHECK(sniffed({"\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR", 16}) == "png");
    CHECK(sniffed({"\x89PNG\r\n\x1a\n\0\0\0\x08" "acTL", 16}) == "apng");
    CHECK(sniffed({"\xff\xd8\xff\xe0", 4}) == "jpeg");
    CHECK(sniffed("GIF89a....") == "gif");
    CHECK(sniffed({"RIFF\x10\0\0\0WEBPVP8 ", 16}) == "webp");
    CHECK(sniffed({"\xabKTX 20\xbb\r\n\x1a\n", 12}) == "ktx2");
    CHECK(sniffed({"\xabKTX 11\xbb\r\n\x1a\n", 12}) == "ktx");
    CHECK(sniffed("#?RADIANCE\n") == "hdr");
    CHECK(sniffed({"II*\0", 4}) == "tiff");
    CHECK(sniffed("DDS |") == "dds");
    CHECK(sniffed({"\0\0\x01\0\x01\0\x10\x10\0\0\x01\0\x20\0\0\x01\0\0\x16\0\0\0", 22}) == "ico");
    CHECK(sniffed({"BM\0\0\0\0\0\0\0\0\x36\0\0\0\x28\0\0\0", 18}) == "bmp");
    CHECK(sniffed(ftyp("avif")) == "avif");
    CHECK(sniffed(ftyp("heic")) == "heif");
    CHECK(sniffed("<svg xmlns=\"http://www.w3.org/2000/svg\"/>") == "svg");
    CHECK(sniffed("<?xml version=\"1.0\"?>\n<!-- drawn -->\n<!DOCTYPE svg>\n<svg>") == "svg");
}

TEST_CASE("signatures identify fonts, archives and compressed streams") {
    CHECK(sniffed({"\0\x01\0\0\0\x0c\0\0\0\0\0\0", 12}) == "ttf");
    CHECK(sniffed("OTTO....") == "otf");
    CHECK(sniffed("wOF2....") == "woff2");
    CHECK(sniffed(zipWith("readme.txt")) == "zip");
    CHECK(sniffed(zipWith("data.json"), "data.json") == "zip");
    CHECK(sniffed({"\x1f\x8b\x08\0", 4}) == "gzip");
    CHECK(sniffed({"\x28\xb5\x2f\xfd", 4}) == "zstd");
    CHECK(sniffed("BZh91AY") == "bzip2");
    String tar;
    tar.append(512, '\0');
    tar.replace(257, 5, "ustar");
    CHECK(sniffed(tar) == "tar");
}

TEST_CASE("signatures identify 3D assets") {
    CHECK(sniffed({"glTF\x02\0\0\0", 8}) == "glb");
    CHECK(sniffed({"glTF\x01\0\0\0", 8}) != "glb");
    CHECK(sniffed("#usda 1.0\n(\n)\n") == "usda");
    CHECK(sniffed({"PXR-USDC\0\x08", 10}) == "usdc");
    // A package is an archive whose first file is a layer.
    CHECK(sniffed(zipWith("scene.usdc")) == "usdz");
    CHECK(sniffed(zipWith("Scene.USDA")) == "usdz");
    CHECK(sniffed(zipWith("textures/a.png")) == "zip");
    CHECK(sniffed("{\n  \"asset\": {\"version\": \"2.0\"}\n}") == "gltf");
    CHECK(sniffed("DRACO\x02\x02") == "draco");
}

TEST_CASE("signatures identify documents, media and executables") {
    CHECK(sniffed("%PDF-1.7") == "pdf");
    CHECK(sniffed("  <!DOCTYPE html><html>") == "html");
    CHECK(sniffed("<p>Hello</p>") == "html");
    CHECK(sniffed("<?xml version=\"1.0\"?><root/>") == "xml");
    CHECK(sniffed("[1, 2, 3]") == "json");
    CHECK(sniffed("{\"a\": 1}") == "json");
    CHECK(sniffed({"RIFF\x10\0\0\0WAVEfmt ", 16}) == "wav");
    CHECK(sniffed({"OggS\0\x02", 6}) == "ogg");
    CHECK(sniffed("ID3\x04") == "mp3");
    CHECK(sniffed({"\xff\xfb\x90\x64", 4}) == "mp3");
    CHECK(sniffed(ftyp("isom")) == "mp4");
    CHECK(sniffed(ftyp("qt  ")) == "quicktime");
    CHECK(sniffed({"\x1a\x45\xdf\xa3\x9f\x42\x82\x84webm", 12}) == "webm");
    CHECK(sniffed({"\0asm\x01\0\0\0", 8}) == "wasm");
    CHECK(sniffed({"\x7f" "ELF\x02\x01", 6}) == "elf");
    CHECK(sniffed({"\xcf\xfa\xed\xfe", 4}) == "macho");
}

TEST_CASE("text is plain unless its content or its name identifies it") {
    CHECK(sniffed("# A heading\n\nSome text.\n") == "text");
    CHECK(sniffed("# A heading\n\nSome text.\n", "notes/README.md") == "markdown");
    CHECK(sniffed("plain words\n", "notes.txt") == "text");
    CHECK(sniffed("hello", "page.HTML") == "html");
    CHECK(sniffed("{\"scene\": 0}", "box.gltf") == "gltf");
    CHECK(sniffed("{\"scene\": 0}", "box.json") == "json");
    // An INI file's section is not JSON.
    CHECK(sniffed("[section]\nkey=value\n") == "text");
    CHECK(sniffed("[section]\nkey=value\n", "setup.cfg") == "ini");
    // A name that implies no format leaves the guess from the content.
    CHECK(sniffed("<?xml version=\"1.0\"?>\n<materialx version=\"1.39\"/>\n", "look.mtlx") == "xml");
    CHECK(sniffed("{\"data\": [1, 2]}", "archive/entry") == "json");
}

TEST_CASE("a name replaces what the content of text suggests") {
    // HTML's patterns begin many formats of text.
    CHECK(sniffed("<!DOCTYPE html>\n<p><?php echo $name; ?></p>\n") == "html");
    CHECK(sniffed("<!DOCTYPE html>\n<p><?php echo $name; ?></p>\n", "index.php") == "php");
    CHECK(sniffed("<!-- generated -->\n# A heading\n") == "html");
    CHECK(sniffed("<!-- generated -->\n# A heading\n", "README.md") == "markdown");
    CHECK(sniffed("<p align=\"center\">logo</p>\n\n# A heading\n", "README.md") == "markdown");
    CHECK(sniffed("<?xml version=\"1.0\"?>\n<svg/>", "icon.SVG") == "svg");
    CHECK(sniffed("<?xml version=\"1.0\"?>\n<html/>", "page.xhtml") == "html");
    // Text whose content suggests nothing takes the language of its name.
    CHECK(sniffed("let x: number = 1;\n", "main.ts") == "typescript");
    CHECK(sniffed("const v = <div>{x}</div>;\n", "view.tsx") == "tsx");
    CHECK(sniffed("fn main() {}\n", "src/main.rs") == "rust");
    CHECK(sniffed("#include <stdio.h>\n", "point.H") == "cpp");
    CHECK(sniffed("#usda 1.0\n", "cube.usda") == "usda");
    CHECK(sniffed("def Xform \"World\" {}\n", "cube.usda") == "usda");
    // A hidden file's name is a whole name, with no extension.
    CHECK(sniffed("export PATH=~/bin:$PATH\n", "/home/a/.zshrc") == "shell");
    CHECK(sniffed("a\n", ".md") == "text");
}

TEST_CASE("a conventional file name implies its format") {
    CHECK(sniffed("all: main.o\n", "Makefile") == "make");
    CHECK(sniffed("all: main.o\n", "src/GNUmakefile") == "make");
    CHECK(sniffed("FROM alpine:3\n", "Dockerfile") == "dockerfile");
    CHECK(sniffed("FROM alpine:3\n", "api.dockerfile") == "dockerfile");
    CHECK(sniffed("cc_library(name = \"core\")\n", "lib/BUILD") == "starlark");
    CHECK(sniffed("cc_library(name = \"core\")\n", "BUILD.bazel") == "starlark");
    CHECK(sniffed("add_library(core a.c)\n", "CMakeLists.txt") == "cmake");
    // A whole name has its conventional case, and an extension any case.
    CHECK(sniffed("all: main.o\n", "MAKEFILE") == "text");
    CHECK(sniffed("all: main.o\n", "rules.MK") == "make");
}

TEST_CASE("a shebang line names the interpreter of a script") {
    CHECK(sniffed("#!/bin/sh\necho hi\n", "configure") == "shell");
    CHECK(sniffed("#!/usr/bin/env python3\nprint(1)\n", "tools/run") == "python");
    CHECK(sniffed("#!/usr/bin/env -S python3.12 -u\nprint(1)\n") == "python");
    CHECK(sniffed("#!/usr/bin/env LANG=C ruby\n") == "ruby");
    CHECK(sniffed("#! /usr/local/bin/node\r\nconsole.log(1)\r\n") == "javascript");
    CHECK(sniffed("#!/usr/bin/make -f\nall:\n") == "make");
    // A shebang is a signature, which a name does not override.
    CHECK(sniffed("#!/usr/bin/env python3\nprint(1)\n", "build.sh") == "python");
    // An interpreter that no format names leaves the name, and then the content, to decide.
    CHECK(sniffed("#!/usr/bin/perl\nprint 1;\n") == "text");
    CHECK(sniffed("#!/usr/bin/perl\nprint 1;\n", "run.sh") == "shell");
    CHECK(sniffed("#!/usr/bin/env\n") == "text");
}

TEST_CASE("a name never overrides a signature, nor makes binary bytes text") {
    CHECK(sniffed({"\x89PNG\r\n\x1a\n", 8}, "image.md") == "png");
    CHECK(sniffed("#usda 1.0\n", "cube.json") == "usda");
    // Binary bytes are no format at all.
    CHECK(sniffed({"\x01\x02\x03\x04", 4}, "notes.md") == "data");
    CHECK(sniffed({"\x47\x40\x00\x10\x00", 5}, "clip.ts") == "data");
}

TEST_CASE("text that is empty or cut short is text") {
    // Empty bytes are empty text.
    CHECK(sniffed("") == "text");
    CHECK(sniffed("", "empty.md") == "markdown");
    // A UTF-8 sequence that the end of the bytes cuts short is still text.
    CHECK(sniffed("caf\xc3") == "text");
    CHECK(sniffed("caf\xc3(") == "data");
}

TEST_CASE("only the first bytes are read") {
    String text;
    text.append(kPrMimeSniffBytes, 'a');
    text.append(1, '\x01');
    CHECK(sniffed(text) == "text");
}

TEST_CASE("every format has a name of its own that finds it") {
    for (uint32_t i = 0; i < PrMimeFormat_Count; ++i) {
        PrMimeFormatInfo info = prMimeFormatInfo(PrMimeFormat(i));
        CHECK(!PrStringView(info.type).isEmpty());
        CHECK(prMimeFormatFromName(info.name) == i);
    }
    CHECK(prMimeFormatFromName("nonesuch") == PrMimeFormat_Count);
    CHECK(prMimeFormatInfo(PrMimeFormat_Usdz).kind == PrMimeKind_Model);
    CHECK(prMimeFormatInfo(PrMimeFormat_Usda).text);
    CHECK(!prMimeFormatInfo(PrMimeFormat_Usdc).text);
}
