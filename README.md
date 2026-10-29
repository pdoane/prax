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

Key libraries:

| layer | libraries |
| --- | --- |
| Platform | `core` (memory, strings, threads, files), `task`, `app` (windows, input), `gpu`, `net`, `tls`, `http`, `fetch`, `websocket` and `storage` |
| Formats | `json`, `xml`, `charset`, `locale`, `unicode`, `image`, `compress` and `color` |
| Engines | `font`, `text`, `layout`, `path` and `draw` |
| Documents | `html`, `svg`, `mathml`, `markdown` and `lottie` |
| Applications | `ui` (immediate-mode UI), `studio/kit` (widgets/component library) and `studio/command` (commands and key bindings) |

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
- The applications in `examples/` are built this way; `examples/draw` is the smallest.

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
with `/STACK:8388608`, which reserves 8 MB of stack for each thread that does not choose its own size. The library's
recursion limits assume that much, as macOS and Linux give a main thread. The options:

| option | default | what it does |
| --- | --- | --- |
| `PR_JS` | `ON` | builds the QuickJS bindings and Lottie expressions |
| `PR_<MODULE>_<BACKEND>` | see below | builds a backend of a module's interface |
| `PR_INSTALL` | `ON` when Prax is the top-level project | generates the install rules and the CMake package |
| `PR_PLATFORM_MIMALLOC` | `OFF` | allocates with mimalloc instead of the C library |
| `PR_USER_CONFIG`, `PR_PLATFORM_NO_DEFAULT` | | bind platform facilities at compile time from a header |
| `PR_SLANGC` | | the Slang compiler to use instead of the release the build fetches |
| `PR_DOWNLOAD_DIR` | | a download cache that several builds can share |

A backend is on by default when it uses the platform's own API or Prax's own code, and off when it depends on a
third-party library. A development build (`PR_DEVELOP`) or an examples build (`PR_EXAMPLES`) enables every backend the
platform supports.

| module | backends: on by default | off by default |
| --- | --- | --- |
| app | `COCOA` on Apple platforms, `WIN32` on Windows | |
| gpu | `METAL` on Apple platforms, `D3D12` on Windows, `VULKAN` on Linux and Android | `VULKAN` elsewhere |
| net | `SOCKETS`, `NULL` | |
| tls | `APPLE` (Network.framework) on Apple platforms, `SCHANNEL` on Windows, `OPENSSL` on other Unix-like systems | `OPENSSL` elsewhere |
| fetch | `BUILTIN`, `INTERCEPT` | |
| http, websocket, task, compress, image | `BUILTIN` | |
| font | `CORETEXT` on Apple platforms, `DIRECTWRITE` on Windows | `FREETYPE` |
| text | `SIMPLE`, `CORETEXT` on Apple platforms | `HARFBUZZ`, `KB` |
| draw | `CPU`, `TRIANGLES`, `FRAGMENT`, `GLYPH_BITMAP` | |

`prax::font_bundle` maps the generic font families and font fallback to font files that the application provides.
`pr_font_bundle_fetch(<directory> Noto)` downloads a bundle's files into a directory at configure time. It is available
after `find_package(prax)` or when the source tree is added to the project.

## Development

`./dev.sh` runs the development workflow: building, testing, running the conformance suites, benchmarking, regenerating
generated sources, linting, and checking the package the way a consumer uses it. `./dev.sh help` lists its commands.
It and its tools are written in Python 3, which development also needs. Its builds use Ninja and enable `PR_EXAMPLES`
and `PR_DEVELOP`, which builds the tests, tools and comparisons with warnings as errors. The suites' test corpora are
fetched into `third_party` at pinned revisions.

## License

Prax is licensed under either of the [MIT License](LICENSE-MIT) or the [Apache License, Version 2.0](LICENSE-APACHE),
at your option.

Some of its sources are copied from or derived from other works, such as the Unicode data, the tables generated from
W3C and WHATWG specifications, the specification text that Prax's suites quote, the Brotli dictionary and the float
conversions of Ryu and fast_float. Those parts stay under their own licenses, which
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists with their notices.

Unless you explicitly state otherwise, any contribution intentionally submitted for inclusion in Prax by you, as
defined in the Apache License, Version 2.0, is licensed as above, without any additional terms or conditions.
