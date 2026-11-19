// File classification: the format of a resource identified from its first bytes and its name, with the media type and
// the kind of each format.

#include "pr/mime_sniff.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/mem.h"
#include "pr/string_ops.h"
#include "pr/utf8.h"

namespace pr_impl {

namespace {

constexpr PrMimeFormatInfo kFormats[] = { // by PrMimeFormat
    {"data", "application/octet-stream", PrMimeKind_None, false},
    {"png", "image/png", PrMimeKind_Image, false},
    {"apng", "image/apng", PrMimeKind_Image, false},
    {"jpeg", "image/jpeg", PrMimeKind_Image, false},
    {"gif", "image/gif", PrMimeKind_Image, false},
    {"webp", "image/webp", PrMimeKind_Image, false},
    {"bmp", "image/bmp", PrMimeKind_Image, false},
    {"ico", "image/x-icon", PrMimeKind_Image, false},
    {"tiff", "image/tiff", PrMimeKind_Image, false},
    {"ktx", "image/ktx", PrMimeKind_Image, false},
    {"ktx2", "image/ktx2", PrMimeKind_Image, false},
    {"dds", "image/vnd.ms-dds", PrMimeKind_Image, false},
    {"hdr", "image/vnd.radiance", PrMimeKind_Image, false},
    {"exr", "image/x-exr", PrMimeKind_Image, false},
    {"psd", "image/vnd.adobe.photoshop", PrMimeKind_Image, false},
    {"avif", "image/avif", PrMimeKind_Image, false},
    {"heif", "image/heif", PrMimeKind_Image, false},
    {"jxl", "image/jxl", PrMimeKind_Image, false},
    {"qoi", "image/qoi", PrMimeKind_Image, false},
    {"astc", "image/x-astc", PrMimeKind_Image, false},
    {"pvr", "image/x-pvr", PrMimeKind_Image, false},
    {"emjc", "image/x-emjc", PrMimeKind_Image, false},
    {"pnm", "image/x-portable-anymap", PrMimeKind_Image, false},
    {"svg", "image/svg+xml", PrMimeKind_Image, true},
    {"ttf", "font/ttf", PrMimeKind_Font, false},
    {"otf", "font/otf", PrMimeKind_Font, false},
    {"ttc", "font/collection", PrMimeKind_Font, false},
    {"woff", "font/woff", PrMimeKind_Font, false},
    {"woff2", "font/woff2", PrMimeKind_Font, false},
    {"zip", "application/zip", PrMimeKind_Archive, false},
    {"tar", "application/x-tar", PrMimeKind_Archive, false},
    {"7z", "application/x-7z-compressed", PrMimeKind_Archive, false},
    {"rar", "application/vnd.rar", PrMimeKind_Archive, false},
    {"gzip", "application/gzip", PrMimeKind_Compressed, false},
    {"zstd", "application/zstd", PrMimeKind_Compressed, false},
    {"xz", "application/x-xz", PrMimeKind_Compressed, false},
    {"bzip2", "application/x-bzip2", PrMimeKind_Compressed, false},
    {"lz4", "application/x-lz4", PrMimeKind_Compressed, false},
    {"gltf", "model/gltf+json", PrMimeKind_Model, true},
    {"glb", "model/gltf-binary", PrMimeKind_Model, false},
    {"usda", "model/vnd.usda", PrMimeKind_Model, true},
    {"usdc", "model/x-usdc", PrMimeKind_Model, false},
    {"usdz", "model/vnd.usdz+zip", PrMimeKind_Model, false},
    {"draco", "model/x-draco", PrMimeKind_Model, false},
    {"ply", "model/x-ply", PrMimeKind_Model, false},
    {"fbx", "model/x-fbx", PrMimeKind_Model, false},
    {"pdf", "application/pdf", PrMimeKind_Document, false},
    {"postscript", "application/postscript", PrMimeKind_Document, true},
    {"rtf", "application/rtf", PrMimeKind_Document, true},
    {"html", "text/html", PrMimeKind_Document, true},
    {"xml", "application/xml", PrMimeKind_Document, true},
    {"json", "application/json", PrMimeKind_Document, true},
    {"markdown", "text/markdown", PrMimeKind_Document, true},
    {"yaml", "application/yaml", PrMimeKind_Document, true},
    {"toml", "application/toml", PrMimeKind_Document, true},
    {"ini", "text/x-ini", PrMimeKind_Document, true},
    {"diff", "text/x-diff", PrMimeKind_Document, true},
    {"text", "text/plain", PrMimeKind_Text, true},
    {"shell", "text/x-shellscript", PrMimeKind_Code, true},
    {"fish", "text/x-fish", PrMimeKind_Code, true},
    {"powershell", "text/x-powershell", PrMimeKind_Code, true},
    {"c", "text/x-csrc", PrMimeKind_Code, true},
    {"cpp", "text/x-c++src", PrMimeKind_Code, true},
    {"csharp", "text/x-csharp", PrMimeKind_Code, true},
    {"objc", "text/x-objcsrc", PrMimeKind_Code, true},
    {"cuda", "text/x-cuda", PrMimeKind_Code, true},
    {"java", "text/x-java", PrMimeKind_Code, true},
    {"kotlin", "text/x-kotlin", PrMimeKind_Code, true},
    {"swift", "text/x-swift", PrMimeKind_Code, true},
    {"dart", "application/vnd.dart", PrMimeKind_Code, true},
    {"go", "text/x-go", PrMimeKind_Code, true},
    {"rust", "text/x-rust", PrMimeKind_Code, true},
    {"zig", "text/x-zig", PrMimeKind_Code, true},
    {"python", "text/x-python", PrMimeKind_Code, true},
    {"ruby", "text/x-ruby", PrMimeKind_Code, true},
    {"php", "text/x-php", PrMimeKind_Code, true},
    {"lua", "text/x-lua", PrMimeKind_Code, true},
    {"javascript", "text/javascript", PrMimeKind_Code, true},
    {"typescript", "text/x-typescript", PrMimeKind_Code, true},
    {"tsx", "text/x-tsx", PrMimeKind_Code, true},
    {"css", "text/css", PrMimeKind_Code, true},
    {"sql", "application/sql", PrMimeKind_Code, true},
    {"glsl", "text/x-glsl", PrMimeKind_Code, true},
    {"hlsl", "text/x-hlsl", PrMimeKind_Code, true},
    {"slang", "text/x-slang", PrMimeKind_Code, true},
    {"wgsl", "text/wgsl", PrMimeKind_Code, true},
    {"cmake", "text/x-cmake", PrMimeKind_Code, true},
    {"make", "text/x-makefile", PrMimeKind_Code, true},
    {"dockerfile", "text/x-dockerfile", PrMimeKind_Code, true},
    {"starlark", "text/x-starlark", PrMimeKind_Code, true},
    {"hcl", "text/x-hcl", PrMimeKind_Code, true},
    {"terraform", "text/x-terraform", PrMimeKind_Code, true},
    {"proto", "text/x-protobuf", PrMimeKind_Code, true},
    {"wav", "audio/wav", PrMimeKind_Audio, false},
    {"aiff", "audio/aiff", PrMimeKind_Audio, false},
    {"flac", "audio/flac", PrMimeKind_Audio, false},
    {"ogg", "application/ogg", PrMimeKind_Audio, false},
    {"mp3", "audio/mpeg", PrMimeKind_Audio, false},
    {"midi", "audio/midi", PrMimeKind_Audio, false},
    {"m4a", "audio/mp4", PrMimeKind_Audio, false},
    {"mp4", "video/mp4", PrMimeKind_Video, false},
    {"quicktime", "video/quicktime", PrMimeKind_Video, false},
    {"webm", "video/webm", PrMimeKind_Video, false},
    {"matroska", "video/x-matroska", PrMimeKind_Video, false},
    {"avi", "video/x-msvideo", PrMimeKind_Video, false},
    {"wasm", "application/wasm", PrMimeKind_Executable, false},
    {"elf", "application/x-elf", PrMimeKind_Executable, false},
    {"macho", "application/x-mach-binary", PrMimeKind_Executable, false},
    {"pe", "application/vnd.microsoft.portable-executable", PrMimeKind_Executable, false},
};
static_assert(countOf(kFormats) == PrMimeFormat_Count, "a format without its info");

// A format that a run of bytes at a fixed offset identifies.
struct Signature {
    uint32_t offset;
    PrStringView bytes;
    PrMimeFormat format;
};

// The signatures that identify a format alone, in the order that they are tried. A format whose signature needs a
// closer look is identified in prMimeSniff instead.
constexpr Signature kSignatures[] = {
    {0, {"\x89PNG\r\n\x1a\n", 8}, PrMimeFormat_Png},
    {0, {"\xff\xd8\xff", 3}, PrMimeFormat_Jpeg},
    {0, "GIF87a", PrMimeFormat_Gif},
    {0, "GIF89a", PrMimeFormat_Gif},
    {0, {"II*\0", 4}, PrMimeFormat_Tiff},
    {0, {"MM\0*", 4}, PrMimeFormat_Tiff},
    {0, {"II+\0", 4}, PrMimeFormat_Tiff},
    {0, {"\xabKTX 11\xbb\r\n\x1a\n", 12}, PrMimeFormat_Ktx},
    {0, {"\xabKTX 20\xbb\r\n\x1a\n", 12}, PrMimeFormat_Ktx2},
    {0, "DDS ", PrMimeFormat_Dds},
    {0, "#?RADIANCE", PrMimeFormat_Hdr},
    {0, "#?RGBE", PrMimeFormat_Hdr},
    {0, {"\x76\x2f\x31\x01", 4}, PrMimeFormat_Exr},
    {0, "8BPS", PrMimeFormat_Psd},
    {0, {"\xff\x0a", 2}, PrMimeFormat_Jxl},
    {0, {"\0\0\0\x0cJXL \r\n\x87\n", 12}, PrMimeFormat_Jxl},
    {0, "qoif", PrMimeFormat_Qoi},
    {0, {"\x13\xab\xa1\x5c", 4}, PrMimeFormat_Astc},
    {0, {"PVR\x03", 4}, PrMimeFormat_Pvr},
    {0, "emj1", PrMimeFormat_Emjc},
    {0, "OTTO", PrMimeFormat_Otf},
    {0, "ttcf", PrMimeFormat_Ttc},
    {0, "wOFF", PrMimeFormat_Woff},
    {0, "wOF2", PrMimeFormat_Woff2},
    {0, {"7z\xbc\xaf\x27\x1c", 6}, PrMimeFormat_SevenZip},
    {0, {"Rar!\x1a\x07", 6}, PrMimeFormat_Rar},
    {0, {"\x1f\x8b\x08", 3}, PrMimeFormat_Gzip},
    {0, {"\x28\xb5\x2f\xfd", 4}, PrMimeFormat_Zstd},
    {0, {"\xfd" "7zXZ\0", 6}, PrMimeFormat_Xz},
    {0, {"\x04\x22\x4d\x18", 4}, PrMimeFormat_Lz4},
    {0, "#usda ", PrMimeFormat_Usda},
    {0, "PXR-USDC", PrMimeFormat_Usdc},
    {0, "DRACO", PrMimeFormat_Draco},
    {0, "ply\n", PrMimeFormat_Ply},
    {0, "ply\r\n", PrMimeFormat_Ply},
    {0, {"Kaydara FBX Binary  \0", 21}, PrMimeFormat_Fbx},
    {0, "%PDF-", PrMimeFormat_Pdf},
    {0, "%!PS", PrMimeFormat_PostScript},
    {0, "{\\rtf", PrMimeFormat_Rtf},
    {0, "fLaC", PrMimeFormat_Flac},
    {0, "OggS", PrMimeFormat_Ogg},
    {0, "MThd", PrMimeFormat_Midi},
    {0, "ID3", PrMimeFormat_Mp3},
    {0, {"\0asm", 4}, PrMimeFormat_Wasm},
    {0, {"\x7f" "ELF", 4}, PrMimeFormat_Elf},
    {0, {"\xfe\xed\xfa\xce", 4}, PrMimeFormat_MachO},
    {0, {"\xfe\xed\xfa\xcf", 4}, PrMimeFormat_MachO},
    {0, {"\xce\xfa\xed\xfe", 4}, PrMimeFormat_MachO},
    {0, {"\xcf\xfa\xed\xfe", 4}, PrMimeFormat_MachO},
    {257, {"ustar", 5}, PrMimeFormat_Tar},
};

// A name, a whole file name or an extension, and the text format that it implies.
struct NamedFormat {
    PrStringView name;
    PrMimeFormat format;
};

// The file names that imply a format by convention, in their conventional case.
constexpr NamedFormat kFileNames[] = {
    {"Makefile", PrMimeFormat_Make},
    {"makefile", PrMimeFormat_Make},
    {"GNUmakefile", PrMimeFormat_Make},
    {"Dockerfile", PrMimeFormat_Dockerfile},
    {"Containerfile", PrMimeFormat_Dockerfile},
    {"CMakeLists.txt", PrMimeFormat_CMake},
    {"BUILD", PrMimeFormat_Starlark},
    {"WORKSPACE", PrMimeFormat_Starlark},
    {"BUCK", PrMimeFormat_Starlark},
    {"Rakefile", PrMimeFormat_Ruby},
    {"Gemfile", PrMimeFormat_Ruby},
    {".profile", PrMimeFormat_Shell},
    {".bashrc", PrMimeFormat_Shell},
    {".bash_profile", PrMimeFormat_Shell},
    {".bash_logout", PrMimeFormat_Shell},
    {".zshrc", PrMimeFormat_Shell},
    {".zprofile", PrMimeFormat_Shell},
    {".zshenv", PrMimeFormat_Shell},
    {".editorconfig", PrMimeFormat_Ini},
    {".gitconfig", PrMimeFormat_Ini},
    {".gitmodules", PrMimeFormat_Ini},
};

// The extensions that imply a format, without the dot.
constexpr NamedFormat kExtensions[] = {
    {"html", PrMimeFormat_Html},
    {"htm", PrMimeFormat_Html},
    {"xhtml", PrMimeFormat_Html},
    {"xml", PrMimeFormat_Xml},
    {"xsd", PrMimeFormat_Xml},
    {"xsl", PrMimeFormat_Xml},
    {"xslt", PrMimeFormat_Xml},
    {"rng", PrMimeFormat_Xml},
    {"svg", PrMimeFormat_Svg},
    {"json", PrMimeFormat_Json},
    {"jsonc", PrMimeFormat_Json},
    {"gltf", PrMimeFormat_Gltf},
    {"usda", PrMimeFormat_Usda},
    {"usd", PrMimeFormat_Usda},
    {"md", PrMimeFormat_Markdown},
    {"markdown", PrMimeFormat_Markdown},
    {"yaml", PrMimeFormat_Yaml},
    {"yml", PrMimeFormat_Yaml},
    {"toml", PrMimeFormat_Toml},
    {"ini", PrMimeFormat_Ini},
    {"cfg", PrMimeFormat_Ini},
    {"diff", PrMimeFormat_Diff},
    {"patch", PrMimeFormat_Diff},
    {"sh", PrMimeFormat_Shell},
    {"bash", PrMimeFormat_Shell},
    {"zsh", PrMimeFormat_Shell},
    {"fish", PrMimeFormat_Fish},
    {"ps1", PrMimeFormat_PowerShell},
    {"psm1", PrMimeFormat_PowerShell},
    {"psd1", PrMimeFormat_PowerShell},
    {"c", PrMimeFormat_C},
    {"cpp", PrMimeFormat_Cpp},
    {"cc", PrMimeFormat_Cpp},
    {"cxx", PrMimeFormat_Cpp},
    {"c++", PrMimeFormat_Cpp},
    {"h", PrMimeFormat_Cpp},
    {"hpp", PrMimeFormat_Cpp},
    {"hh", PrMimeFormat_Cpp},
    {"hxx", PrMimeFormat_Cpp},
    {"h++", PrMimeFormat_Cpp},
    {"inl", PrMimeFormat_Cpp},
    {"ipp", PrMimeFormat_Cpp},
    {"tpp", PrMimeFormat_Cpp},
    {"cppm", PrMimeFormat_Cpp},
    {"ixx", PrMimeFormat_Cpp},
    {"cs", PrMimeFormat_CSharp},
    {"csx", PrMimeFormat_CSharp},
    {"m", PrMimeFormat_ObjC},
    {"mm", PrMimeFormat_ObjC},
    {"cu", PrMimeFormat_Cuda},
    {"cuh", PrMimeFormat_Cuda},
    {"java", PrMimeFormat_Java},
    {"kt", PrMimeFormat_Kotlin},
    {"kts", PrMimeFormat_Kotlin},
    {"swift", PrMimeFormat_Swift},
    {"dart", PrMimeFormat_Dart},
    {"go", PrMimeFormat_Go},
    {"rs", PrMimeFormat_Rust},
    {"zig", PrMimeFormat_Zig},
    {"zon", PrMimeFormat_Zig},
    {"py", PrMimeFormat_Python},
    {"pyi", PrMimeFormat_Python},
    {"rb", PrMimeFormat_Ruby},
    {"rake", PrMimeFormat_Ruby},
    {"gemspec", PrMimeFormat_Ruby},
    {"php", PrMimeFormat_Php},
    {"phtml", PrMimeFormat_Php},
    {"lua", PrMimeFormat_Lua},
    {"js", PrMimeFormat_JavaScript},
    {"mjs", PrMimeFormat_JavaScript},
    {"cjs", PrMimeFormat_JavaScript},
    {"jsx", PrMimeFormat_JavaScript},
    {"ts", PrMimeFormat_TypeScript},
    {"mts", PrMimeFormat_TypeScript},
    {"cts", PrMimeFormat_TypeScript},
    {"tsx", PrMimeFormat_Tsx},
    {"css", PrMimeFormat_Css},
    {"sql", PrMimeFormat_Sql},
    {"glsl", PrMimeFormat_Glsl},
    {"vert", PrMimeFormat_Glsl},
    {"frag", PrMimeFormat_Glsl},
    {"geom", PrMimeFormat_Glsl},
    {"comp", PrMimeFormat_Glsl},
    {"tesc", PrMimeFormat_Glsl},
    {"tese", PrMimeFormat_Glsl},
    {"mesh", PrMimeFormat_Glsl},
    {"task", PrMimeFormat_Glsl},
    {"rgen", PrMimeFormat_Glsl},
    {"rchit", PrMimeFormat_Glsl},
    {"rmiss", PrMimeFormat_Glsl},
    {"vsh", PrMimeFormat_Glsl},
    {"fsh", PrMimeFormat_Glsl},
    {"hlsl", PrMimeFormat_Hlsl},
    {"hlsli", PrMimeFormat_Hlsl},
    {"fx", PrMimeFormat_Hlsl},
    {"fxh", PrMimeFormat_Hlsl},
    {"slang", PrMimeFormat_Slang},
    {"wgsl", PrMimeFormat_Wgsl},
    {"cmake", PrMimeFormat_CMake},
    {"mk", PrMimeFormat_Make},
    {"mak", PrMimeFormat_Make},
    {"make", PrMimeFormat_Make},
    {"dockerfile", PrMimeFormat_Dockerfile},
    {"containerfile", PrMimeFormat_Dockerfile},
    {"bzl", PrMimeFormat_Starlark},
    {"star", PrMimeFormat_Starlark},
    {"bazel", PrMimeFormat_Starlark},
    {"hcl", PrMimeFormat_Hcl},
    {"tf", PrMimeFormat_Terraform},
    {"tfvars", PrMimeFormat_Terraform},
    {"proto", PrMimeFormat_Proto},
};

// The interpreters that a shebang line names, without a version that follows the name, as `python3.12` names Python.
constexpr NamedFormat kInterpreters[] = {
    {"sh", PrMimeFormat_Shell},
    {"bash", PrMimeFormat_Shell},
    {"zsh", PrMimeFormat_Shell},
    {"dash", PrMimeFormat_Shell},
    {"ksh", PrMimeFormat_Shell},
    {"ash", PrMimeFormat_Shell},
    {"fish", PrMimeFormat_Fish},
    {"pwsh", PrMimeFormat_PowerShell},
    {"python", PrMimeFormat_Python},
    {"ruby", PrMimeFormat_Ruby},
    {"php", PrMimeFormat_Php},
    {"lua", PrMimeFormat_Lua},
    {"luajit", PrMimeFormat_Lua},
    {"node", PrMimeFormat_JavaScript},
    {"nodejs", PrMimeFormat_JavaScript},
    {"make", PrMimeFormat_Make},
    {"gmake", PrMimeFormat_Make},
};

bool startsWith(PrByteView bytes, uint32_t offset, PrStringView prefix) {
    return bytes.size >= offset && bytes.size - offset >= prefix.size &&
        prMemEquals(bytes.data + offset, prefix.data, prefix.size);
}

PrStringView textOf(PrByteView bytes) {
    return PrStringView{reinterpret_cast<const char*>(bytes.data), bytes.size};
}

// Returns true if a RIFF container's form type at offset 8 is `form`.
bool isRiff(PrByteView bytes, PrStringView form) {
    return startsWith(bytes, 0, "RIFF") && startsWith(bytes, 8, form);
}

// Identifies an ISO base media file (ISO/IEC 14496-12) by the brands of its `ftyp` box: the major brand, then each
// compatible brand.
PrMimeFormat sniffIsoMedia(PrByteView bytes) {
    if (!startsWith(bytes, 4, "ftyp") || bytes.size < 16)
        return PrMimeFormat_None;
    uint32_t size = prMemReadBeU32(bytes.data);
    size_t end = size < bytes.size ? size : bytes.size;
    struct Brand {
        PrStringView brand;
        PrMimeFormat format;
    };
    constexpr Brand kBrands[] = {
        {"avif", PrMimeFormat_Avif}, {"avis", PrMimeFormat_Avif}, {"heic", PrMimeFormat_Heif},
        {"heix", PrMimeFormat_Heif}, {"heim", PrMimeFormat_Heif}, {"heis", PrMimeFormat_Heif},
        {"hevc", PrMimeFormat_Heif}, {"hevx", PrMimeFormat_Heif}, {"mif1", PrMimeFormat_Heif},
        {"msf1", PrMimeFormat_Heif}, {"qt  ", PrMimeFormat_QuickTime}, {"M4A ", PrMimeFormat_M4a},
        {"M4B ", PrMimeFormat_M4a}, {"isom", PrMimeFormat_Mp4}, {"iso2", PrMimeFormat_Mp4},
        {"iso4", PrMimeFormat_Mp4}, {"iso5", PrMimeFormat_Mp4}, {"iso6", PrMimeFormat_Mp4},
        {"mp41", PrMimeFormat_Mp4}, {"mp42", PrMimeFormat_Mp4}, {"avc1", PrMimeFormat_Mp4},
        {"dash", PrMimeFormat_Mp4}, {"M4V ", PrMimeFormat_Mp4}, {"3gp4", PrMimeFormat_Mp4},
        {"3gp5", PrMimeFormat_Mp4}, {"3gp6", PrMimeFormat_Mp4},
    };
    // The major brand is at offset 8 and the compatible brands follow the minor version at offset 12.
    for (size_t at = 8; at + 4 <= end; at = at == 8 ? 16 : at + 4) {
        PrStringView brand{reinterpret_cast<const char*>(bytes.data + at), 4};
        for (const Brand& known : kBrands) {
            if (brand == known.brand)
                return known.format;
        }
    }
    return PrMimeFormat_None;
}

// Identifies a Matroska or WebM file by the DocType of its EBML header.
PrMimeFormat sniffEbml(PrByteView bytes) {
    if (!startsWith(bytes, 0, "\x1a\x45\xdf\xa3"))
        return PrMimeFormat_None;
    PrStringView head = prStringHead(textOf(bytes), 64);
    if (prStringContains(head, "webm"))
        return PrMimeFormat_Webm;
    if (prStringContains(head, "matroska"))
        return PrMimeFormat_Matroska;
    return PrMimeFormat_None;
}

// Identifies a ZIP archive, and a USD package among them: an archive whose first file is a USD layer.
PrMimeFormat sniffZip(PrByteView bytes) {
    if (startsWith(bytes, 0, {"PK\x05\x06", 4}))
        return PrMimeFormat_Zip;
    if (!startsWith(bytes, 0, {"PK\x03\x04", 4}) || bytes.size < 30)
        return PrMimeFormat_None;
    uint16_t nameSize = prMemReadLeU16(bytes.data + 26);
    PrStringView name = prStringSlice(textOf(bytes), 30, nameSize);
    if (prAsciiEndsWithFold(name, ".usda") || prAsciiEndsWithFold(name, ".usdc") || prAsciiEndsWithFold(name, ".usd"))
        return PrMimeFormat_Usdz;
    return PrMimeFormat_Zip;
}

// Identifies an animated PNG: an `acTL` chunk before the first `IDAT` (APNG specification).
PrMimeFormat sniffPng(PrByteView bytes) {
    for (size_t at = 8; at + 8 <= bytes.size;) {
        uint32_t length = prMemReadBeU32(bytes.data + at);
        PrStringView type{reinterpret_cast<const char*>(bytes.data + at + 4), 4};
        if (type == "acTL")
            return PrMimeFormat_Apng;
        if (type == "IDAT" || length > bytes.size)
            break;
        at += size_t(length) + 12;
    }
    return PrMimeFormat_Png;
}

// Returns true if the bytes are a TrueType font's table directory: version 1.0 or `true`, and a plausible number of
// tables.
bool isTrueType(PrByteView bytes) {
    if (bytes.size < 12 || !(startsWith(bytes, 0, {"\0\x01\0\0", 4}) || startsWith(bytes, 0, "true")))
        return false;
    uint16_t tables = prMemReadBeU16(bytes.data + 4);
    return tables > 0 && tables < 256;
}

// Returns true if the bytes begin as a BMP file: `BM`, reserved fields of zero, and the size of a known info header.
bool isBmp(PrByteView bytes) {
    if (bytes.size < 18 || !startsWith(bytes, 0, "BM") || prMemReadLeU32(bytes.data + 6) != 0)
        return false;
    uint32_t header = prMemReadLeU32(bytes.data + 14);
    return header == 12 || header == 40 || header == 52 || header == 56 || header == 64 || header == 108 || header == 124;
}

// Returns true if the bytes begin as an icon or cursor directory with at least one image, and each entry of the
// directory that the bytes hold is one: a reserved byte of zero, an icon's planes and bits per pixel, a size, and an
// offset past the directory, where the first image begins. The header alone is four bytes that many binary files, such
// as a buffer of 16-bit indices, begin with.
bool isIco(PrByteView bytes) {
    if (bytes.size < 22 || prMemReadLeU16(bytes.data) != 0)
        return false;
    uint16_t type = prMemReadLeU16(bytes.data + 2);
    uint32_t count = prMemReadLeU16(bytes.data + 4);
    if ((type != 1 && type != 2) || count == 0)
        return false;
    uint32_t directory = 6 + 16 * count;
    for (uint32_t i = 0; i < count && 6 + 16 * (i + 1) <= bytes.size; ++i) {
        const uint8_t* entry = bytes.data + 6 + 16 * i;
        uint16_t planes = prMemReadLeU16(entry + 4);
        uint16_t bits = prMemReadLeU16(entry + 6);
        bool depth = bits == 0 || bits == 1 || bits == 4 || bits == 8 || bits == 16 || bits == 24 || bits == 32;
        uint32_t offset = prMemReadLeU32(entry + 12);
        if (entry[3] != 0 || (type == 1 && (planes > 1 || !depth)) || prMemReadLeU32(entry + 8) == 0 ||
                offset < directory || (i == 0 && offset != directory))
            return false;
    }
    return true;
}

// Returns true if the bytes begin as an MPEG audio frame: a frame sync, a layer, a bitrate and a sampling rate that are
// not reserved (ISO/IEC 11172-3, 2.4.2.3).
bool isMpegAudio(PrByteView bytes) {
    if (bytes.size < 4 || bytes.data[0] != 0xff || (bytes.data[1] & 0xe0) != 0xe0)
        return false;
    uint8_t version = (bytes.data[1] >> 3) & 3;
    uint8_t layer = (bytes.data[1] >> 1) & 3;
    uint8_t bitrate = bytes.data[2] >> 4;
    uint8_t rate = (bytes.data[2] >> 2) & 3;
    return version != 1 && layer != 0 && bitrate != 0 && bitrate != 15 && rate != 3;
}

// Returns true if the bytes begin as a portable executable: an MS-DOS header whose `e_lfanew` points to the `PE`
// signature.
bool isPe(PrByteView bytes) {
    if (bytes.size < 64 || !startsWith(bytes, 0, "MZ"))
        return false;
    uint32_t at = prMemReadLeU32(bytes.data + 0x3c);
    return startsWith(bytes, at, {"PE\0\0", 4});
}

// Returns true if the bytes begin as a portable anymap: `P1` to `P7` and whitespace.
bool isPnm(PrByteView bytes) {
    return bytes.size >= 3 && bytes.data[0] == 'P' && bytes.data[1] >= '1' && bytes.data[1] <= '7' &&
        prAsciiIsSpace(char(bytes.data[2]));
}

// Returns true if the bytes are text: UTF-8 without the bytes that mimesniff counts as binary data
// (mimesniff/#binary-data-byte), or text that a UTF-16 byte order mark begins. A sequence that the end of the bytes
// cuts short is text.
bool isText(PrByteView bytes) {
    if (startsWith(bytes, 0, "\xfe\xff") || startsWith(bytes, 0, "\xff\xfe"))
        return true;
    for (size_t i = 0; i < bytes.size; ++i) {
        uint8_t b = bytes.data[i];
        if (b <= 0x08 || b == 0x0b || (b >= 0x0e && b <= 0x1a) || (b >= 0x1c && b <= 0x1f))
            return false;
    }
    PrStringView text = textOf(bytes);
    size_t valid = prUtf8Validate(text);
    if (valid == text.size)
        return true;
    // What follows the valid prefix must be the start of one sequence that needs more bytes than remain.
    uint8_t lead = uint8_t(text.data[valid]);
    size_t length = 0;
    if (lead >= 0xc2 && lead <= 0xdf)
        length = 2;
    else if (lead >= 0xe0 && lead <= 0xef)
        length = 3;
    else if (lead >= 0xf0 && lead <= 0xf4)
        length = 4;
    if (text.size - valid >= length)
        return false;
    for (size_t i = valid + 1; i < text.size; ++i) {
        if ((uint8_t(text.data[i]) & 0xc0) != 0x80)
            return false;
    }
    return true;
}

// Returns the text after a byte order mark and whitespace.
PrStringView skipSpace(PrStringView text) {
    if (prStringStartsWith(text, "\xef\xbb\xbf"))
        text = prStringFrom(text, 3);
    size_t at = 0;
    while (at < text.size && prAsciiIsSpace(text.data[at]))
        ++at;
    return prStringFrom(text, at);
}

// Returns true if `text` begins with `tag`, without regard to ASCII case, followed by a byte that ends a tag's name
// (mimesniff/#tag-terminating-byte).
bool startsWithTag(PrStringView text, PrStringView tag) {
    if (text.size <= tag.size || !prAsciiStartsWithFold(text, tag))
        return false;
    char after = text.data[tag.size];
    return after == ' ' || after == '>';
}

// Returns true if text is HTML by the patterns of mimesniff/#identifying-a-resource-with-an-unknown-mime-type.
bool isHtml(PrStringView text) {
    constexpr PrStringView kTags[] = {
        "<!DOCTYPE HTML", "<HTML", "<HEAD", "<SCRIPT", "<IFRAME", "<H1", "<DIV", "<FONT", "<TABLE", "<A", "<STYLE",
        "<TITLE", "<B", "<BODY", "<BR", "<P", "<!--",
    };
    for (PrStringView tag : kTags) {
        if (startsWithTag(text, tag))
            return true;
    }
    return false;
}

// Identifies XML, and SVG among it: an XML declaration, or an `<svg` root after any declaration, comments, processing
// instructions and doctype.
PrMimeFormat sniffXml(PrStringView text) {
    bool declared = prStringStartsWith(text, "<?xml");
    PrStringView rest = text;
    for (uint32_t i = 0; i < 64 && prStringStartsWith(rest, "<"); ++i) {
        if (prStringStartsWith(rest, "<svg") && rest.size > 4 &&
                (prAsciiIsSpace(rest.data[4]) || rest.data[4] == '>' || rest.data[4] == '/'))
            return PrMimeFormat_Svg;
        if (!prStringStartsWith(rest, "<?") && !prStringStartsWith(rest, "<!"))
            break;
        PrStringView close = ">";
        if (prStringStartsWith(rest, "<!--"))
            close = "-->";
        size_t end = prStringFind(rest, close);
        if (end == rest.size)
            break;
        rest = skipSpace(prStringFrom(rest, end + close.size));
    }
    return declared ? PrMimeFormat_Xml : PrMimeFormat_None;
}

// Returns true if `text` begins with `[` and then what begins a JSON value, so that a section header of an INI file is
// not taken for JSON.
bool startsJsonArray(PrStringView text) {
    if (!prStringStartsWith(text, "["))
        return false;
    PrStringView value = skipSpace(prStringFrom(text, 1));
    if (value.size == 0)
        return true;
    char c = value.data[0];
    return c == '{' || c == '[' || c == '"' || c == '-' || c == ']' || prAsciiIsDigit(c) ||
        prStringStartsWith(value, "true") || prStringStartsWith(value, "false") || prStringStartsWith(value, "null");
}

// Guesses the format of text from its content: XML, HTML, JSON, glTF JSON or plain text.
PrMimeFormat sniffText(PrByteView bytes) {
    PrStringView text = skipSpace(textOf(bytes));
    PrMimeFormat xml = sniffXml(text);
    if (xml != PrMimeFormat_None)
        return xml;
    if (isHtml(text))
        return PrMimeFormat_Html;
    if (prStringStartsWith(text, "{") || startsJsonArray(text)) {
        // A glTF asset is an object with an `asset` member (glTF-2.0/#asset), which writers put first.
        if (prStringStartsWith(text, "{") && prStringContains(text, "\"asset\""))
            return PrMimeFormat_Gltf;
        return PrMimeFormat_Json;
    }
    return PrMimeFormat_Text;
}

// Returns the format of the interpreter that a shebang line names (POSIX exec, `#!` and a path), or None. The program
// env runs the first of its arguments that is not an option or an assignment, as `#!/usr/bin/env python3` runs python3.
PrMimeFormat sniffShebang(PrStringView text) {
    if (!prStringStartsWith(text, "#!"))
        return PrMimeFormat_None;
    PrStringView rest = prStringFrom(text, 2);
    PrStringView line = prStringHead(rest, prStringFindByte(rest, '\n'));
    size_t at = 0;
    bool env = false;
    while (at < line.size) {
        while (at < line.size && (line.data[at] == ' ' || line.data[at] == '\t' || line.data[at] == '\r'))
            ++at;
        size_t end = at;
        while (end < line.size && line.data[end] != ' ' && line.data[end] != '\t' && line.data[end] != '\r')
            ++end;
        PrStringView word = prStringSlice(line, at, end - at);
        at = end;
        if (word.size == 0)
            break;
        if (env && (prStringStartsWith(word, "-") || prStringFindByte(word, '=') != word.size))
            continue;
        size_t slash = prStringFindLastByte(word, '/');
        PrStringView program = slash == word.size ? word : prStringFrom(word, slash + 1);
        if (!env && program == "env") {
            env = true;
            continue;
        }
        size_t named = program.size;
        while (named > 0 && (prAsciiIsDigit(program.data[named - 1]) || program.data[named - 1] == '.'))
            --named;
        for (const NamedFormat& interpreter : kInterpreters) {
            if (prStringHead(program, named) == interpreter.name)
                return interpreter.format;
        }
        break;
    }
    return PrMimeFormat_None;
}

// Returns the text format that `name`, a file name or a path, implies by its last part, or None. A last part that
// starts with its only dot, as a hidden file's does, has no extension.
PrMimeFormat formatOfName(PrStringView name) {
    size_t slash = prStringFindLastByte(name, '/');
    PrStringView last = slash == name.size ? name : prStringFrom(name, slash + 1);
    for (const NamedFormat& file : kFileNames) {
        if (last == file.name)
            return file.format;
    }
    size_t dot = prStringFindLastByte(last, '.');
    if (dot == last.size || dot == 0)
        return PrMimeFormat_None;
    PrStringView extension = prStringFrom(last, dot + 1);
    for (const NamedFormat& known : kExtensions) {
        if (prAsciiEqualsFold(extension, known.name))
            return known.format;
    }
    return PrMimeFormat_None;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrMimeFormat prMimeSniff(PrByteView bytes, PrStringView name) {
    if (bytes.size > kPrMimeSniffBytes)
        bytes.size = kPrMimeSniffBytes;
    // Empty bytes are empty text, which the name refines as it does any text.
    if (bytes.data == nullptr)
        bytes = PrByteView{reinterpret_cast<const uint8_t*>(""), 0};
    for (const Signature& signature : kSignatures) {
        if (!startsWith(bytes, signature.offset, signature.bytes))
            continue;
        if (signature.format == PrMimeFormat_Png)
            return sniffPng(bytes);
        return signature.format;
    }
    if (startsWith(bytes, 0, "glTF") && bytes.size >= 8 && prMemReadLeU32(bytes.data + 4) == 2)
        return PrMimeFormat_Glb;
    if (isRiff(bytes, "WEBP"))
        return PrMimeFormat_Webp;
    if (isRiff(bytes, "WAVE"))
        return PrMimeFormat_Wav;
    if (isRiff(bytes, "AVI "))
        return PrMimeFormat_Avi;
    if (startsWith(bytes, 0, "FORM") && (startsWith(bytes, 8, "AIFF") || startsWith(bytes, 8, "AIFC")))
        return PrMimeFormat_Aiff;
    if (startsWith(bytes, 0, "BZh") && bytes.size > 3 && bytes.data[3] >= '1' && bytes.data[3] <= '9')
        return PrMimeFormat_Bzip2;
    PrMimeFormat found = sniffZip(bytes);
    if (found == PrMimeFormat_None)
        found = sniffIsoMedia(bytes);
    if (found == PrMimeFormat_None)
        found = sniffEbml(bytes);
    if (found != PrMimeFormat_None)
        return found;
    if (isTrueType(bytes))
        return PrMimeFormat_Ttf;
    if (isBmp(bytes))
        return PrMimeFormat_Bmp;
    if (isIco(bytes))
        return PrMimeFormat_Ico;
    if (isPe(bytes))
        return PrMimeFormat_Pe;
    if (isMpegAudio(bytes))
        return PrMimeFormat_Mp3;
    if (isPnm(bytes))
        return PrMimeFormat_Pnm;
    if (!isText(bytes))
        return PrMimeFormat_None;
    PrMimeFormat declared = sniffShebang(textOf(bytes));
    if (declared != PrMimeFormat_None)
        return declared;
    PrMimeFormat named = formatOfName(name);
    if (named != PrMimeFormat_None)
        return named;
    return sniffText(bytes);
}

PrMimeFormatInfo prMimeFormatInfo(PrMimeFormat format) {
    PR_ASSERT(format < PrMimeFormat_Count);
    return kFormats[format];
}

PrMimeFormat prMimeFormatFromName(PrStringView name) {
    for (uint32_t i = 0; i < PrMimeFormat_Count; ++i) {
        if (kFormats[i].name == name)
            return PrMimeFormat(i);
    }
    return PrMimeFormat_Count;
}
