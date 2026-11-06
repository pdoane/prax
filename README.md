# Prax

**Theory put into practice.**

Prax, from praxis, is a collection of C libraries written in C++ for developing GUI applications and games. They
provide cross-platform abstractions, implementations of specifications, and opinionated layers on top that make
development easier and applications more consistent.

The libraries are layered so that applications can take the parts that they need. Consider text rendering:

 - lib/font provides the raw paths and metrics for a glyph
 - lib/draw rasterizes those paths
 - lib/text adds text shaping, bidirectional support, character transformation, and line breaking
 - lib/layout provides full CSS layout algorithms: inline, flexbox, grids, flow, and tables

The result is web quality layout and rendering made available without a browser. Then lib/ui adds
an immediate mode framework on top, improving ergonomics and performance.

Prax can be embedded in an existing application or be the base of a standalone one. For example, an application can
link the compression library it already uses or plug in its own GPU abstraction layer, and Prax has built-in
implementations so that it needs few external dependencies. The same application can run on desktop, mobile, and the
web; the Status section says what runs today.

## How it fits together

The libraries, by area. The first comment of each library's `CMakeLists.txt` says what it holds.

| area | libraries |
| --- | --- |
| Foundation | `core`, `task`, `cache`, `storage`, `webidl` |
| System | `app`, `gpu`, `audio`, `media`, `process` |
| Networking | `net`, `tls`, `http`, `websocket`, `fetch`, `xhr`, `url`, `mime` |
| Data | `json`, `xml`, `compress`, `image`, `color`, `geom`, `charset`, `locale`, `unicode`, `html_entity` |
| Text | `font`, `font_bundle`, `text`, `text_edit`, `text_view`, `syntax`, `regex`, `text_diff` |
| Graphics | `path`, `draw`, `canvas`, `lottie` |
| Documents | `css`, `dom`, `layout`, `html`, `svg`, `mathml`, `tex`, `markdown` |
| Developer tools | `lsp`, `dap`, `vterm` |
| Interface | `ui`, `studio/kit`, `studio/command`, `studio/text_editor`, `studio/terminal` |

**Verified.** WPT, the test suites of CommonMark, GFM, resvg and the Lottie players, and Prax's own suites check
conformance. Benchmarks measure immediate-mode frames and dynamic retained documents.

## Status

Prax is approaching pre-alpha on macOS and Windows. The suites' scoreboards (`suites/*/scoreboard.md`) record what
passes.

Coming soon:

- **Platforms.** iOS and the web, then Linux and Android.
- **3D.** A scene renderer, composed with 2D in one frame.
- **Audio and video.** Audio devices and the Web Audio API, synthesis, recording an application to video, reading
  video and audio files.
- **Accessibility.** The semantics of a UI and of a document (roles, names, states, focus and actions).
- **Studio.** The editors, documents, commands and undo that tools share.

## Usage

Prax builds with CMake 3.28 or newer, a C17 and C++20 compiler (MSVC on Windows), and Git. It targets arm64, and
x86-64 with x86-64-v3 (AVX2). It runs on macOS 26 or newer on Apple Silicon, and on Windows 10 version 1809 or newer;
configuring a build for anything older fails. The build fetches its dependencies at pinned releases and checks each
against its hash (`cmake/dependencies.cmake`). To use your own copy of a dependency, set
`FETCHCONTENT_SOURCE_DIR_<NAME>` to its source directory.

- Each library is a CMake target named for its module (`prax::draw`, `prax::layout`), or for its module and language
  binding (`prax::html_quickjs`).
- A target links the libraries it depends on, so a program names only the ones it calls.
- A module defines an interface, and its library contains every backend of that interface that the build enables (the
  `PR_<MODULE>_<BACKEND>` options below). A program chooses among them at run time, or disables them and provides its
  own implementation.
- The applications in `examples/` and `apps/` are built this way; `examples/draw_gallery` is the smallest.

Add the source tree to a project with FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(prax GIT_REPOSITORY <repository> GIT_TAG <commit>)
FetchContent_MakeAvailable(prax)

target_link_libraries(app PRIVATE prax::app prax::draw prax::text)
```

Or build and install the libraries, and find the package:

```sh
cmake -S prax -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /opt/prax
```

```cmake
find_package(prax 0.1 REQUIRED COMPONENTS app draw text)
```

The libraries are static. On Apple platforms, the linker warns when a link line repeats a static library;
`set(CMAKE_LINK_LIBRARIES_STRATEGY REORDER_FREELY)` in the program's project avoids it. On Windows, link the program
with `/STACK:8388608`, which reserves 8 MB of stack for each thread that does not choose its own size, and with
Emscripten, link it with `-sSTACK_SIZE=8388608`. The library's
recursion limits assume that much, as macOS and Linux give a main thread. The options:

| option | default | what it does |
| --- | --- | --- |
| `PR_JS` | `ON` | builds the QuickJS bindings and Lottie expressions |
| `PR_CHARSET_LEGACY` | `ON` | decodes and encodes the Encoding Standard's legacy single-byte and multi-byte charsets; if it is off, a program has only UTF-8, UTF-16, replacement and x-user-defined |
| `PR_COMPRESS_BROTLI` | `ON` | decodes Brotli in the built-in codec, which WOFF2 fonts and Brotli responses need |
| `PR_<MODULE>_<BACKEND>` | see below | builds a backend of a module's interface |
| `PR_INSTALL` | `ON` when Prax is the top-level project | generates the install rules and the CMake package |
| `PR_PLATFORM_MIMALLOC` | `OFF` | allocates with mimalloc instead of the C library |
| `PR_PLATFORM_THREADS` | `ON`, and `OFF` on the web | starts threads; without them, the program runs on one thread |
| `PR_USER_CONFIG`, `PR_PLATFORM_NO_DEFAULT` | | bind platform facilities at compile time from a header |
| `PR_SLANGC` | | the Slang compiler to use instead of the release the build fetches |
| `PR_DOWNLOAD_DIR` | the environment variable of that name, or else `prax/downloads` in the user's cache directory | the download cache that builds share; empty disables it |

A backend is on by default when it uses the platform's own API or Prax's own code, and off when it depends on a
third-party library. A development build (`PR_DEVELOP`), an examples build (`PR_EXAMPLES`) or an apps build
(`PR_APPS`) enables every backend the platform supports.

| interface | option | backend | builds on | default |
| --- | --- | --- | --- | --- |
| app | `PR_APP_COCOA` | Cocoa | Apple platforms | on |
| app | `PR_APP_WIN32` | Win32 | Windows | on |
| app | `PR_APP_WEB` | a web page's canvas | the web | on |
| gpu | `PR_GPU_METAL` | Metal 3 | Apple platforms | on |
| gpu | `PR_GPU_METAL4` | Metal 4, for experiments | Apple platforms | on |
| gpu | `PR_GPU_D3D12` | Direct3D 12 | Windows | on |
| gpu | `PR_GPU_VULKAN` | Vulkan | every platform but the web | on for Linux and Android |
| gpu | `PR_GPU_MEMORY` | memory, without shaders or presentation | every platform | on |
| gpu | `PR_GPU_CANVAS2D` | memory and a web page's canvases, without shaders | the web | on |
| net | `PR_NET_SOCKETS` | BSD sockets, or Winsock on Windows | every platform but the web | on |
| net | `PR_NET_NULL` | no connections, and an event loop that the caller runs | every platform | on |
| process launcher | `PR_PROCESS_POSIX` | posix_spawn | macOS and Linux | on |
| process launcher | `PR_PROCESS_WINDOWS` | CreateProcessW | Windows | on |
| tls | `PR_TLS_APPLE` | Network.framework | Apple platforms | on |
| tls | `PR_TLS_SCHANNEL` | SChannel | Windows | on |
| tls | `PR_TLS_OPENSSL` | OpenSSL 3 | where the build finds it | on for Unix-like systems other than Apple's |
| http | `PR_HTTP_BUILTIN` | HTTP/1.1 over `lib/net` and `lib/tls` | every platform | on |
| websocket | `PR_WEBSOCKET_BUILTIN` | the WebSocket protocol over `lib/net` and `lib/tls` | every platform | on |
| fetch | `PR_FETCH_BUILTIN` | the Fetch Standard over `lib/http` | every platform | on |
| fetch | `PR_FETCH_INTERCEPT` | requests that the host answers, or passes to the next fetcher | every platform | on |
| task pool | `PR_TASK_BUILTIN` | Prax | every platform | on |
| compression | `PR_COMPRESS_BUILTIN` | Prax | every platform | on |
| image codecs | `PR_IMAGE_BUILTIN` | Prax | every platform | on |
| audio | `PR_AUDIO_MEMORY` | output kept in memory, for a run without a device | every platform | on |
| audio | `PR_AUDIO_APPLE` | Core Audio | Apple platforms | on |
| audio | `PR_AUDIO_WINDOWS` | WASAPI | Windows | on |
| media | `PR_MEDIA_BUILTIN` | Prax | every platform | on |
| media | `PR_MEDIA_APPLE` | AVFoundation | Apple platforms | on |
| media | `PR_MEDIA_WEB` | a web page's video element | the web | on |
| media | `PR_MEDIA_WINDOWS` | Media Foundation | Windows | on |
| system fonts | `PR_FONT_CORETEXT` | CoreText | Apple platforms | on |
| system fonts | `PR_FONT_DIRECTWRITE` | DirectWrite | Windows | on |
| font hinter | `PR_FONT_FREETYPE` | FreeType | every platform | off |
| text shaper | `PR_TEXT_SIMPLE` | one glyph per character, with pair kerning | every platform | on |
| text shaper | `PR_TEXT_CORETEXT` | CoreText | Apple platforms | on |
| text shaper | `PR_TEXT_HARFBUZZ` | HarfBuzz | every platform | off |
| text shaper | `PR_TEXT_KB` | kb | every platform | off |
| syntax languages | `PR_SYNTAX_TREESITTER` | tree-sitter's grammars | every platform | off |
| draw renderer | `PR_DRAW_CPU` | CPU renderer | every platform | on |
| draw renderer | `PR_DRAW_CANVAS` | the browser's Canvas 2D | the web | on |
| draw renderer | `PR_DRAW_TRIANGLES` | GPU renderer: colored and textured triangles | every platform but the web | on |
| draw renderer | `PR_DRAW_FRAGMENT` | GPU renderer: per-pixel coverage and paint evaluation | every platform but the web | on |
| draw writer | `PR_DRAW_SVG` | SVG documents, which need `PR_DRAW_CPU` | every platform | on |
| draw writer | `PR_DRAW_PDF` | tagged PDF documents, which need `PR_DRAW_CPU` | every platform | on |

`prax::font_bundle` maps the generic font families and font fallback to font files that the application provides.
`pr_font_bundle_fetch(<directory> Noto)` downloads a bundle's files into a directory at configure time. It is available
after `find_package(prax)` or when the source tree is added to the project.

## Development

`./dev.sh` runs the development workflow: building, testing, running the conformance suites, benchmarking, regenerating
generated sources, linting, and checking the package the way a consumer uses it. `./dev.sh help` lists its commands.
It and its tools are written in Python 3, which development also needs. Its builds use Ninja and enable `PR_EXAMPLES`,
`PR_APPS` and `PR_DEVELOP`, which builds the tests, tools and comparisons with warnings as errors. They cache
downloads in `third_party/downloads`. The suites' test corpora are fetched into `third_party` at pinned revisions.

### CMake versions

| build | minimum | reason |
| --- | --- | --- |
| consumer | 3.28 | `FetchContent_Declare` needs 3.28 for `EXCLUDE_FROM_ALL`. Older versions build and install third-party libraries with Prax. Ubuntu 24.04 LTS ships 3.28. |
| development | 4.0 | Comparisons that predate CMake's current policies need `CMAKE_POLICY_VERSION_MINIMUM`, because deprecation warnings are errors. |

Raise the consumer minimum only for a feature that Prax needs. `CMakeLists.txt`, `cmake/shader_library.cmake` and
`tools/package/CMakeLists.txt` each state it. Test a change on the minimum version, because a newer CMake accepts what
an older one rejects. The `PR_DEVELOP` check in `CMakeLists.txt` states the development minimum.

## License

Prax is licensed under either of the [MIT License](LICENSE-MIT) or the [Apache License, Version 2.0](LICENSE-APACHE),
at your option.

Some of its sources are copied from or derived from other works, such as the Unicode data, the tables generated from
W3C and WHATWG specifications, the specification text that Prax's suites quote, the Brotli dictionary and the float
conversions of Ryu and fast_float. Those parts stay under their own licenses, which
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists with their notices.

Unless you explicitly state otherwise, any contribution intentionally submitted for inclusion in Prax by you, as
defined in the Apache License, Version 2.0, is licensed as above, without any additional terms or conditions.
