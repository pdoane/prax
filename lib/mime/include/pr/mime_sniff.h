// File classification: the format of a resource identified from its first bytes and its name, with the media type and
// the kind of each format.
//
// A classifier identifies, as the Unix `file` command does, and inspects nothing past what tells formats apart. Each
// format's own module reads its header. The formats include each language of text that a host treats as its own, such
// as a programming language that an editor highlights, so that a format's media type is all that a host needs to choose
// how to read the text.
//
// mimesniff - https://mimesniff.spec.whatwg.org/

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The formats that the classifier identifies. None is bytes that it does not identify.
enum {
    PrMimeFormat_None,

    // Images
    PrMimeFormat_Png,
    PrMimeFormat_Apng,              // a PNG with an animation control chunk before its first image data
    PrMimeFormat_Jpeg,
    PrMimeFormat_Gif,
    PrMimeFormat_Webp,
    PrMimeFormat_Bmp,
    PrMimeFormat_Ico,               // icons and cursors
    PrMimeFormat_Tiff,
    PrMimeFormat_Ktx,
    PrMimeFormat_Ktx2,
    PrMimeFormat_Dds,
    PrMimeFormat_Hdr,               // Radiance RGBE
    PrMimeFormat_Exr,
    PrMimeFormat_Psd,
    PrMimeFormat_Avif,
    PrMimeFormat_Heif,
    PrMimeFormat_Jxl,
    PrMimeFormat_Qoi,
    PrMimeFormat_Astc,
    PrMimeFormat_Pvr,
    PrMimeFormat_Emjc,
    PrMimeFormat_Pnm,
    PrMimeFormat_Tga,
    PrMimeFormat_Svg,

    // Fonts
    PrMimeFormat_Ttf,
    PrMimeFormat_Otf,
    PrMimeFormat_Ttc,
    PrMimeFormat_Woff,
    PrMimeFormat_Woff2,

    // Archives and compressed streams
    PrMimeFormat_Zip,
    PrMimeFormat_Tar,
    PrMimeFormat_SevenZip,
    PrMimeFormat_Rar,
    PrMimeFormat_Gzip,
    PrMimeFormat_Zstd,
    PrMimeFormat_Xz,
    PrMimeFormat_Bzip2,
    PrMimeFormat_Lz4,               // the LZ4 frame format

    // 3D assets
    PrMimeFormat_Gltf,              // glTF JSON
    PrMimeFormat_Glb,               // binary glTF
    PrMimeFormat_Usda,
    PrMimeFormat_Usdc,
    PrMimeFormat_Usdz,              // a ZIP archive whose first file is a USD layer
    PrMimeFormat_Draco,
    PrMimeFormat_Ply,
    PrMimeFormat_Fbx,               // binary FBX

    // Documents
    PrMimeFormat_Pdf,
    PrMimeFormat_PostScript,
    PrMimeFormat_Rtf,
    PrMimeFormat_Html,
    PrMimeFormat_Xml,
    PrMimeFormat_Json,
    PrMimeFormat_Markdown,
    PrMimeFormat_Yaml,
    PrMimeFormat_Toml,
    PrMimeFormat_Ini,
    PrMimeFormat_Diff,              // a unified or context diff, as a patch holds
    PrMimeFormat_Text,              // text that no other format identifies

    // Source code
    PrMimeFormat_Shell,             // POSIX shell, bash and zsh scripts
    PrMimeFormat_Fish,
    PrMimeFormat_PowerShell,
    PrMimeFormat_C,
    PrMimeFormat_Cpp,               // C++, and the headers that C and C++ share
    PrMimeFormat_CSharp,
    PrMimeFormat_ObjC,
    PrMimeFormat_Cuda,
    PrMimeFormat_Java,
    PrMimeFormat_Kotlin,
    PrMimeFormat_Swift,
    PrMimeFormat_Dart,
    PrMimeFormat_Go,
    PrMimeFormat_Rust,
    PrMimeFormat_Zig,
    PrMimeFormat_Python,
    PrMimeFormat_Ruby,
    PrMimeFormat_Php,
    PrMimeFormat_Lua,
    PrMimeFormat_JavaScript,
    PrMimeFormat_TypeScript,
    PrMimeFormat_Tsx,               // TypeScript with JSX
    PrMimeFormat_Css,
    PrMimeFormat_Sql,
    PrMimeFormat_Glsl,
    PrMimeFormat_Hlsl,
    PrMimeFormat_Slang,
    PrMimeFormat_Wgsl,
    PrMimeFormat_CMake,
    PrMimeFormat_Make,
    PrMimeFormat_Dockerfile,
    PrMimeFormat_Starlark,          // Bazel's and Buck's build files
    PrMimeFormat_Hcl,
    PrMimeFormat_Terraform,
    PrMimeFormat_Proto,             // Protocol Buffers schemas

    // Audio and video
    PrMimeFormat_Wav,
    PrMimeFormat_Aiff,
    PrMimeFormat_Flac,
    PrMimeFormat_Ogg,
    PrMimeFormat_Mp3,
    PrMimeFormat_Midi,
    PrMimeFormat_M4a,
    PrMimeFormat_Mp4,
    PrMimeFormat_QuickTime,
    PrMimeFormat_Webm,
    PrMimeFormat_Matroska,
    PrMimeFormat_Avi,

    // Executables
    PrMimeFormat_Wasm,
    PrMimeFormat_Elf,
    PrMimeFormat_MachO,
    PrMimeFormat_Pe,

    PrMimeFormat_Count,
};
typedef uint8_t PrMimeFormat;

// What a format holds.
enum {
    PrMimeKind_None,
    PrMimeKind_Image,
    PrMimeKind_Font,
    PrMimeKind_Archive,             // files, each with a name
    PrMimeKind_Compressed,          // one stream
    PrMimeKind_Model,
    PrMimeKind_Document,
    PrMimeKind_Text,
    PrMimeKind_Code,                // a program, a shader, a script or a build in a programming language
    PrMimeKind_Audio,
    PrMimeKind_Video,
    PrMimeKind_Executable,
};
typedef uint8_t PrMimeKind;

// A format's names and kind. A format's media type is the one that IANA registers for it if there is one, and otherwise
// the conventional unregistered type, such as `text/x-python`.
typedef struct PrMimeFormatInfo {
    PrStringView name;              // a short lowercase id, as a command line names the format: "png", "usdz"
    PrStringView type;              // the media type's essence: "image/png", or "application/octet-stream" for None
    PrMimeKind kind;
    bool text;                      // true if the format is text, which a text editor shows as its source
} PrMimeFormatInfo;

// The most bytes that prMimeSniff reads from the start of a resource.
enum { kPrMimeSniffBytes = 4096 };

// Identifies the format of the resource whose first bytes are `bytes`: the whole resource, or at least its first
// kPrMimeSniffBytes. `name` is the resource's file name or path, or empty.
//
// A signature decides: the magic bytes of a binary format, or the start of a text that declares its format, as `#usda`
// does, or a shebang line (`#!`) that names a known interpreter. Bytes that no signature identifies and that are not
// text are None. The format of other text is a guess from its content: HTML and XML by mimesniff's patterns
// (mimesniff/#identifying-a-resource-with-an-unknown-mime-type), JSON by its first byte, and glTF by an `asset` member.
// A name that implies a text format replaces that guess. The last part of the name implies a format by a conventional
// whole file name, such as `Makefile`, or else by its extension, without regard to ASCII case, such as `.tsx`. So a PHP
// page that begins with a doctype is PHP, and Markdown that begins with an HTML comment is Markdown. A name never
// overrides a signature, and text that neither its content nor its name identifies is Text.
//
// A Targa image has no signature. Binary bytes are Targa if they begin with a header that the format allows, and either
// the name has a Targa extension or the bytes are the whole resource and end with the TGA 2.0 footer.
PrMimeFormat prMimeSniff(PrByteView bytes, PrStringView name);

// Returns the names and kind of `format`.
PrMimeFormatInfo prMimeFormatInfo(PrMimeFormat format);

// Returns the format whose name is `name`, as PrMimeFormatInfo::name spells it, or Count if none is.
PrMimeFormat prMimeFormatFromName(PrStringView name);

// Returns the format that a file name or path implies by convention, without reading the file. The format comes from
// the extension of the last path component, compared without regard to ASCII case, such as `.JPG`. It can also come
// from a conventional whole file name, such as `Makefile`. Returns None if the name implies no format.
//
// A name can be wrong. If the bytes are available, prMimeSniff identifies them more reliably.
PrMimeFormat prMimeFormatOfFileName(PrStringView name);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
