# Drawing

The drawing libraries are vector graphics libraries with no dependency on the web, layout or any producer. The list's
vocabulary covers web painting, SVG, canvas 2D and Lottie in full. Features of XAML, Illustrator, After Effects and
Figma are added on their own merits. Headers state each command's fields and contracts, and this file covers what
backends must agree on.

## Libraries

`lib/geom` (points, rects, transforms, shaped corners) → `lib/path` (paths and renderer algorithms, each in its own
public `pr/path_*` header) → `lib/draw` (the list, resources, lowering utilities, the renderer and writer interfaces,
the structure tree) → `lib/draw/backends/*`, with the comparison backends in `lib/draw/comparisons/*`. `lib/font` and
`lib/text` sit beside these libraries ([text.md](text.md)).

The device backends are written once, against `lib/gpu` ([gpu.md](gpu.md)), which runs them on Metal, Direct3D 12 and
Vulkan.
Their shaders are Slang, compiled for each API at build time.

**Backends stay simple, and complexity lives in the libraries.** A backend that is hard to write or port means that a
library is missing something. Every backend reads a list through the public API by switching on each command's kind,
with no private fast path. A `const` list may be rendered by several backends at once. A backend is used by one thread.

## The list

- **Commands keep what the producer meant**: a border, not its pieces, and a shadow, not a blur of a shape. Backends
  draw commands natively, recognize cheap cases, or lower commands through the shared utilities. A shape has its own
  kind only if it has a fast path, and every kind lowers to a path (`prDrawShapePath`).
- **A list keeps no recording state.** Every call passes what it draws, and every painting call passes its paint
  (`PrDrawPaint`). The list copies them into side storage that it owns, and consecutive commands in an equal brush or
  stroke share one copy. A producer that sets state and leaves it, as canvas 2D does, keeps that state itself, with
  `save` and `restore`.
- **Resources are referenced**: paths, images, fonts, lists and filters. They are immutable once shared, thread-safe,
  and have a process-unique id that backends key caches on. A list's id changes when the list changes. A path's id
  names its contents: the clone that a list records shares the id, so lists recorded every frame from the producer's
  paths hit the same cache entries.
- **Transforms, clips, groups and marks push and pop in one nesting order**, like platform state stacks.
- **Target space is pixels.** The producer puts its unit ratio in the root transform, so the stack gives the target
  scale everywhere (glyph rasterization, caching, mip selection).
- **What cannot paint is not recorded.**
- **Transform collapse** is detected during the walk by `prDrawTransformPush`, for every backend.
- **Embedding** plays another list as if it were recorded in place (SVG `<use>`, symbols, precomps, cached subtrees). An
  embedded list that misses the clip in effect draws nothing, and a consumer skips it without walking it
  (`prDrawListMisses`), so a list that embeds more than it shows, such as a page of a printed document, costs what it
  shows. A referenced list is frozen. Freezing is a state of the list, not a separate type: a list is recorded and then frozen
  in place, and a change after freezing is a contract violation that asserts. A backend may cache a frozen list as a
  texture keyed on its id and the target scale if the list reads no backdrop and draws only source-over and normal
  outside isolated groups.

## Paint

- **A paint is a color or a brush.** A brush carries its own opacity. A command stores the color that its brush
  multiplies, per component: the paint's color, or white at the brush's opacity. So a backend paints a brush times the
  command's color, and opacity without a group is that color's alpha.
- **Images are brushes.** An image is a shape filled with an image brush (`prDrawBrushImage`). A source is either
  texels or an area of a list rendered once at the target scale.
- **Every source samples the same way.** Its kind (an image, a host texture, a list, a projected group's layer)
  decides only where its texture comes from. Each is then a texture with its own mip levels, sampled by one path.
  Mipmap sampling is anisotropic: the level comes from the minor axis of the pixel's footprint, and up to 16 taps
  spread along its major axis, so a plane seen at a steep angle stays sharp across it. A sampling with
  `PrDrawSamplingFlag_Gutter` lets a backend sample the whole texture, so filtering reads the texels around a
  sub-rectangle; without the flag, a backend keeps every tap within the rectangle.
- **Gradients** are defined by `prDrawGradientParameter`, `prDrawGradientExtend` and the ramp's pieces
  (`prDrawGradientRamp`, `prDrawRampColor`), and every backend agrees with them. A piece spans the parameter
  between two stops, and a backend interpolates only within a piece, so a stop that changes color abruptly is exact
  at any resolution: a point on it takes the later color. A piece that mixes linearly in the target's space is just
  its two ends. Pad keeps which end a point lies past, so past the end is the last stop's color while 1 itself is the
  gradient's. Stops mix premultiplied, as CSS mixes them, or with color and alpha interpolated separately
  (`interpolationAlpha`), as SVG mixes them. In the second case, a stop next to transparent black mixes toward black.

## Coverage and compositing

A pixel's result is clip coverage × shape coverage × paint. Paint is evaluated only where both coverages are nonzero.

- Coverage is area, or is center-sampled without antialiasing. Clips cover the same way.
- Each command composites on its own, so shared edges show the background, as they do in browsers, and glyph runs
  draw the same however they are split.
- Producers decide what snaps to the pixel grid, and how, and backends round it where it is played. An edge marked
  `PrDrawEdges_Snapped` rounds to the nearest target pixel, half up, in the snap space: the composition of the
  transforms around it, without those pushed apart (`PrDrawSnapFlags`). So a list snaps wherever it is embedded, and
  its recording does not depend on where it lands. `prDrawTransformSnap` rounds a command for every backend's walk.
  Everything else is drawn exactly as given.
- Blending is `Cs' = (1 - αb) Cs + αb B(Cb, Cs)` (straight), compositing is `co = cs' Fa + cb Fb` (premultiplied), and
  the result is then mixed by coverage.
- Into a target of 8-bit channels, a solid color is rounded to what the target holds before coverage applies, so that
  colors that round to one color paint alike, antialiased edges included. Coverage and the blend are not rounded
  otherwise.
- A border whose sides are each one band of a color covers its ring as one area, and each pixel takes the color of the
  side that it lies on, blended across the line between two sides over about a pixel (`prDrawBorderSides`). The sides
  meet without the seam that two coverages leave where they share an edge.
- Operators are bounded to the covered shape. Canvas-style unbounded compositing is a group with that operator.

## Strokes and boxes

`pr/path_stroke.h` computes outlines. Under a similarity transform, the mapped geometry is stroked with a scaled width.
Otherwise the stroke is computed in shape space and then mapped. Non-scaling strokes are measured in target pixels, and
a hairline is a non-scaling stroke of width one, never of width zero. An aligned stroke is computed at double width and
then cut. Brush mapping is evaluated in a unit square along the stroke. Producers resolve markers, arrowheads and
corner-aligned dashes. Borders, shadows and decorations follow CSS through the lowering utilities. A drop shadow paints
only outside its caster, and shadows of anything other than a box are a group with a shadow filter. A box's shadow may
approximate its Gaussian blur for speed, as long as it stays plausible, so backends' shadows differ slightly in their
falloff. A wavy
decoration's phase is the origin's offset into the wave.

## Text

`Glyphs` draws one run of one font. A backend that draws from an atlas draws an upright glyph at a quarter of a target
pixel, from its own outline placed there (`prDrawGlyphPlace`), as the atlas holds it, so a glyph covers the same pixels
however layout splits its run and wherever within the quarter its run puts it. The cpu backend draws glyphs exactly
where their runs place them, or at those phases with `PrDrawCpuOptions::glyphPhases`. Per-glyph transforms serve text
on a path and animators, and per-glyph colors split runs. Color glyphs paint their own content × the paint's alpha (COLR
takes the paint's color if asked to). Picture glyphs go through the backend's glyph decoder, and `PrDrawGlyphCache`
lowers both kinds to lists. Hinting applies only under a plain scale, and shaping uses hinted advances at
`PrTextRun::pixelsPerEm`.

## Clips, groups and filters

- Clips only intersect. `PushClipTargetRect` is in target pixels regardless of the transform. CSS `overflow` clips to
  the border inset (`prCornerRectInset`). Clipping to content is a group with an alpha mask.
- Transforms are affine. 3D is a projected group, and producers sort and split planes.
- A projected group (`PrDrawGroup::projection`) is recorded flat in its plane, within bounds that it must have. Its
  layer holds the visible part of the plane. A plane whose projection is affine is drawn at its exact scale. Any other
  plane is drawn at its largest scale over the visible part, but no finer than the enclosing transform's scale to
  target pixels, and its layer holds at most twice as many pixels as the target pixels that the plane covers, so a steep
  plane's near part is magnified from the layer. The layer composites where the plane's bounds land, projected and cut
  behind the eye, and each target pixel samples the layer at its point of the plane. `pr/draw_projection.h` computes
  that placement for every backend, and the tangent through which a backend that cannot sample a layer projectively
  draws the plane. A projected group is not a backdrop root unless it has another effect: a backdrop on it, or inside
  it, reads what lies beneath the plane on the target, mapped back through the projection and filtered in the plane.
  The comparison backends draw no backdrops. A projected group inside another flattens into the outer plane.
- A group renders into a transparent layer and composites with its operator and blend mode across its bounds. A group
  with no effect is not a group.
- A group that composites in sRGB-linear (`PrDrawGroup::space`, SVG's color-interpolation) is recorded as groups that
  every backend draws. The outer group's filter blends the content onto what lies beneath in sRGB-linear and copies
  the result over it. An inner group holds the group's other effects, if it has any. Such a group reads what lies
  beneath, so it costs a backdrop read wherever it draws.
- A backdrop filters what was drawn before the group over what lies beneath the list. A group with an effect is a
  backdrop root, except a projected group with no other effect (`prDrawGroupIsBackdropRoot`).
- A filter is an immutable graph of SVG and CSS filter kinds, plus design tools' shadows, glows, feathering,
  progressive blur and tiling. There are no custom shaders.
- Each node of a filter produces its result within its region, inside the graph's. The region of an input is the
  region of the node that made it, or the graph's for the group's inputs. A node reads past that region by its edge
  mode: none, duplicate, wrap or mirror. Stitched turbulence tiles its node's region, whose copies join without a
  seam. A displacement map reads its map in its node's space and moves the pixels of its input without converting
  them. A convolution's default divisor counts a kernel sum within rounding of zero as zero.
- Blurs approximate Gaussians, and their cost never grows with sigma: wide blurs and far-reaching filters draw at a
  lower resolution (`draw_lower.h`).

## Color

- **Extended sRGB, encoded.** Colors in a list, images and targets are extended sRGB: sRGB's primaries, white and
  transfer function, mirrored below zero and extended past one, unclamped. Producers convert other spaces when they
  record a color or create an image. SDR white is 1.0, and absolute spaces convert with HDR Reference White at
  203 cd/m².
- **Alpha is stated where a color is held.** A color carries no alpha convention, so every field that holds one says
  whether it is straight or premultiplied. Targets and layers are premultiplied.
- **The target's format decides its range.** An 8-bit target holds [0, 1] and clips. A half-float target holds
  extended values, and so does every layer of its render.
- **Blending happens on encoded values**, as the web specifies it, whatever encoding the target's pixels hold.
- **The application maps to the display.** A list and a render know nothing of the display they are shown on. A
  render's target holds extended sRGB, encoded, in 8 bits or half floats (`PrDrawTarget`), and the renderer maps no
  colors. An application that shows a half-float render on a display tone maps the colors whose luminance is past the
  headroom, in linear light, with a knee that keeps their hue (`prColorToneMap`), maps colors into the display's gamut
  by reducing their chroma in OKLCH as CSS Color 4 specifies (`prColorGamutMap`), and encodes them as its swap chain
  holds colors, all in a last pass of its own over the composited render. The display shader does so on the device,
  in a pass of the application's, from a table of the most chroma at each lightness and hue (`prColorGamutMaxChroma`),
  which lies within a just noticeable difference of the CPU's search (`pr/draw_display.h`). The layer of a render that
  reads a backdrop composites source-over what the target held: a render owns what it draws, and what lies beneath
  belongs to whoever owns the render pass.
- **Headroom is linear light.** A headroom, a limit and a brightness are multiples of SDR white in linear light, and
  core's transfer functions (`PrColorTransfer`) convert between that and a space's encoding.
- **Producers resolve display-dependent colors.** A list holds no color that depends on the display. CSS's
  `color-hdr()` and `dynamic-range-limit` resolve where the page is projected, for the headroom that the host passes,
  and an image brush's `limit` caps its texels in the draw itself.
- **Swap chains report their encoding.** A `gpu` swap chain is created for SDR or HDR (`PrGpuSwapChainIntent`), and
  its device chooses the format and the color space that the platform presents: 8-bit sRGB for SDR, and for HDR,
  extended sRGB in half floats, scRGB, or HDR10's PQ on 10 bits (`PrGpuSwapChainInfo`). The swap chain converts no
  colors, so an SDR render draws into it directly, and an HDR render draws into a texture of its own that the
  application converts into the swap chain's. An application's frame reports the display (`PrAppDisplay`), as
  [rendering.md](rendering.md#the-display-and-the-users-preferences) describes.

## Backends

| backend | host provides | fidelity |
| --- | --- | --- |
| `triangles` | a renderer of textured, colored triangles (Dear ImGui-class) | limited, by design |
| `cpu` | memory for pixels | complete |
| `canvas` | a web page's canvases | the browser's Canvas 2D |
| `fragment` | a `lib/gpu` device | complete |
| `coregraphics`, `blend2d`, `imgui` | a platform renderer to compare with | the platform's |

- `triangles` tessellates with a one-pixel fringe, evaluates paint at vertices on meshes cut where gradients turn,
  draws glyphs from an atlas, and draws shadows as bands. Anything per-pixel belongs to `fragment`.
- `cpu` is tiled and multithreaded.
- `canvas` walks a list into calls on a canvas's 2D context, so the browser rasterizes paths, strokes, gradients,
  patterns, images and shadows. The walk lowers what the context cannot express into fills and stops: borders,
  decorations, strokes that the context does not stroke, and ramps that mix in other spaces. It evaluates a brush that
  the context has no paint for per pixel, and caches glyphs' outlines from one frame to the next. A group with an
  effect draws into a canvas of its own. The context cannot apply filters other than chains of blurs and drop shadows,
  and cannot draw projected groups.
- `fragment` draws instances (quads, parallelograms, triangles) with analytic coverage for common shapes. Glyphs and
  small paths, filled or stroked, come from the atlas if they are unrotated and unskewed, so unchanged frames
  rasterize nothing. An embedded list's instances and records are kept once two frames have embedded it in the same
  state (transform, clip, opacity, compositing), and a later frame that embeds it in that state, or moved by whole
  target pixels within the clip, copies them instead of walking the list, so unchanged content costs a copy. Other
  paths go through configurable CPU preparation (fringe tessellation, sparse strips, or binned tiles). Clips are one
  record. Renderers divide instances into draws as their API allows. Inputs are read-only (in GLSL ES, through
  `texelFetch`). Blends that the API does not support read the destination through framebuffer fetch or a copy.
  Filters are passes into half-float layers. Instance colors and ramps are half floats, so one record layout serves
  8-bit and half-float targets; a frame for an 8-bit target clamps them. Images, lists rendered as sources and projected
  groups' layers are textures with real mip levels that the device generates, and the hardware's samplers filter them:
  nearest, linear, trilinear and anisotropic for mipmaps, and cubic as nine bilinear samples. Tile modes are the
  samplers' address modes, and the shader places each point within its tile. Copies that are filtered on their own
  (`PrDrawTileFlag_Clamped`) are sampled as one tile that extends by Pad, at the point moved into its copy. A tiled
  sub-rectangle without a gutter,
  and an opaque host texture whose taps can land on no texel, read texels in the shader instead. Straight-alpha host
  textures are premultiplied after filtering, so the colors of their transparent texels bleed into their edges. A
  projected group composites as one quad that samples its layer through the projection.

A device backend is a GPU-free builder that turns a list into a frame of plain data, plus one renderer that draws the
frame on a `lib/gpu` device. The fragment renderer defines its records once, in a header that C++ and Slang share
(`draw_fragment_records.h`). Shared CPU preparation lives in the libraries, so a backend's choices are configuration.

**Comparison backends** (`coregraphics`, `blend2d`, `imgui`, in `lib/draw/comparisons/`) are not product backends.
They exist to compare the engine with platform renderers, in speed and in fidelity: each translates the list into a
platform's drawing API, so it shows what that platform does with it. A feature that the rules give no way to draw is
dropped rather than built for one platform. Four rules decide what they support, with no per-feature judgment:

1. A command goes to the platform's call for its kind, with the parameters that call takes, converted to its terms.
   Parameters it does not take are ignored, and the platform's behavior stands where it differs from `cpu`.
2. A command the platform has no call for is lowered by a shared utility into geometry or commands, which then follow
   rule 1. A command with no such lowering draws nothing.
3. Transforms, clips and groups are the platform's state stack. A scope the stack cannot express draws its contents
   into a layer of the platform's own images, which the platform composites at the pop. A layer lasts exactly as long
   as its scope.
4. The backend computes no pixels, colors or vertices of its own, and keeps nothing across commands but platform
   objects that wrap the list's resources one to one.

The departures listed below are what the rules produce. A comparison backend is also a performance reference, so work
done for one platform alone is a cost in its numbers, not only in its code.

| feature | `cpu`, `fragment` | `triangles` |
| --- | --- | --- |
| shapes, fill and stroke | yes | tessellated, with fringe coverage |
| brushes | exact | at vertices, on meshes cut where gradients turn |
| opacity of images and lists | yes | yes, as the vertex color |
| borders, shadows, decorations | yes | lowered; shadows as bands |
| images, nine-slices | yes | yes |
| the host's textures | `fragment` | yes |
| glyphs, color glyphs | yes (`fragment` from an atlas) | from an atlas |
| clips: rectangles, target rectangles | yes | scissor |
| clips: corners, paths | yes | convex ones cut what they clip, aliased; others clip to their bounds |
| groups: opacity | yes | multiplied into the contents, exact where nothing inside overlaps |
| groups: blending, filters, masks, backdrops | yes | no |
| projected groups | a layer, sampled through the projection | no |
| width profiles, stroke brush mapping | yes | yes |
| half-float targets | `cpu`, `fragment` | no |

`triangles` departures:

- Operators draw as source-over if they only add, and otherwise draw nothing. Blend modes draw as normal. Filters,
  backdrops, masks and knockout are absent.
- A list source plays clipped in place, at its opacity. A list brush on a stroke draws nothing.
- Shadow bands fall off linearly, drop shadows are not cut by the caster, and the opening edges of inner shadows are
  aliased.
- Image brushes tile by the sampler of the horizontal mode, without spacing, and filter across the copies' edges
  whatever their flags. Host textures are sampled as straight
  alpha.
- Sub-pixel strokes are a pixel wide, faded by their width.
- Unrotated, unskewed glyphs at moderate sizes come from the atlas at quarter-pixel phase, and other glyphs are filled
  as paths.
- A projected group and its contents draw nothing, because a projected group needs a layer.

`coregraphics` uses a Quartz bitmap context and transform stack. CoreText draws filled glyphs from the font bytes, and
COLR is lowered. Its departures:

- A projected group's layer holds the plane through the projection's tangent at the center of the plane's visible
  part, and is clipped to where the plane lands.
- Strokes are Quartz's: centered, of constant width, with the start cap at every end. Miter-clip and arcs joins are
  drawn as miters, and stroke brush mappings are absent.
- Filters other than a lone drop shadow, backdrops, knockout, brushes on images and hard mix are absent.
- A list source, as an image or a brush, draws nothing. A luminance mask is read as alpha.
- Diamond gradients are drawn as the ellipse through the corners. Clamped image
  tiles draw once.

`blend2d` uses its own transform stack, and layers of its images for scopes other than pixel-rect clips and plain
groups. Its departures:

- A projected group's layer holds the plane through the projection's tangent at the center of the plane's visible
  part, and composites through the coverage of where the plane lands.
- Strokes are Blend2D's: solid, centered and of constant width. Arcs joins are drawn as miter-clip, and stroke brush
  mappings are absent.
- Shadows, filters, backdrops, knockout, brushes on images, image tile spacing and aliased drawing are absent.
- Blend2D runs without its JIT. Its portable pipelines draw every blend mode, and every operator except source-over,
  copy, clear and destination-out, as source-over.
- A list source, as an image or a brush, draws nothing. A luminance mask is read as alpha.
- Diamond gradients are drawn as the ellipse through the corners, non-nesting two-point radial gradients use
  Blend2D's focal gradient, and extend None is drawn as Pad, for gradients and images. Cubic and mipmap sampling are
  bilinear. A layer clip applies once to the layer's contents.

`imgui` goes through `ImDrawList`'s calls, and draws Dear ImGui's draw data as its renderers do, on a `lib/gpu` device
in the host's pass. Its source documents the mapping. Glyphs load through Dear ImGui's FreeType loader, with a code
point lookup that reads glyph ids and with variable instances as separate fonts. They are hinted at whole-pixel sizes,
with single-strength synthetic bold, and in color only as FreeType draws them (COLRv1 and SVG glyphs as outlines, and
bitmap-only fonts absent). Stroked runs are absent. A group's contents draw in place without its effects. A projected
group's contents draw through the projection's tangent at the center of the plane's visible part, within the scissor of
where the plane lands.

**Atlases are general.** They are keyed on resource identity, part, transform scale and skew, and subpixel phase.
Packing is code that the GPU renderers share (`lib/draw/backends/shared`), not part of the drawing API. `PrDrawAtlas`
holds glyphs (font id, glyph id) and paths (path id, fill rule or stroke shape, measure and alignment) at a positive
scale without rotation, skew or mirroring, at quarter-pixel phase.
Each is rasterized once, with the coverage that the uncached fill would have. An entry found since the last
`prDrawAtlasNext` keeps its texels, and a full atlas empties a page that holds none of those entries.

## GPU integration

- The host owns the device and the passes. Preparing a list (outside any pass, given only the size and the target
  format) records uploads and offscreen passes into the host's command buffer. Rendering draws into a rectangle of a
  pass that the host opened, over what the pass holds. Several lists may share a pass.
- Every backend is created over a `lib/gpu` device. The target's `commands` and the render's pass are the device's
  (`PrGpuCommands`, `PrGpuPass`), which may wrap the host's native ones, and a host texture is a `PrDrawTextureId`
  that names a texture of the device (`prDrawGpuTexture`). A backend may destroy what it creates as soon
  as the work that uses it is recorded, and the device keeps it until that work is done.
- A backdrop is provided at prepare time as a texture or a redraw function. Otherwise, what lies beneath is
  transparent.
- The backends that draw on the CPU render on a memory device (`PrGpuNativeKind_Pixels`), such as the memory and
  Canvas 2D devices. A frame renders natively into the texels of the pass's attachment, over what they hold, while the
  pass runs (`prGpuPassNativeDraw`). It reads the textures that it draws in place (`prGpuTextureNative`). To draw into
  pixels of its own, an application renders over a texture made on them (`prGpuTextureCreateOverPixels`), as
  `prDrawRendererRenderPixels` does.
- The `canvas` backend renders on a device whose textures are a web page's canvases (`PrGpuNativeKind_Canvas2d`). A
  frame draws through the 2D context of the pass's attachment while the pass runs (`prGpuPassNativeDraw`). The device
  keeps each texture's canvas and texels in step, so `canvas` and `cpu` can share one device, its textures and its swap
  chains.
- There is no global device state, and a backend never assumes that it is the device's only user.
- A device renderer belongs to the native device that its driver was made on. When the device is lost (`pr/gpu.h`),
  the host destroys the renderer with the device and makes one on the new device, whose first prepare makes again what
  it draws with from the list's resources. Dropping resources (`prDrawRendererDropResources`) frees device memory and
  keeps the device.
- Shaders are written once, in Slang, and compiled for each API. Render tests against `cpu` keep each API's results
  in agreement.
- What a render draws belongs to the host.

## Documents

A list says what a page looks like. A document format also says what the page means, and wants text as text. Writers
(`pr/draw_writer.h`) turn lists into documents, one list for each page, and three pieces keep the list itself free of
any one format: marks in the list, a structure tree beside it, and the writer that joins the two.

### Writers

A writer is an interface of its own, beside the renderer. A renderer fills pixels; a writer emits the bytes of a
document, which the host receives through a function as they are written. A document is a sequence of pages, each a list
on a sheet of an extent in px, written between `prDrawWriterBegin` and `prDrawWriterEnd`. Pages may arrive one at a time,
as a paginator makes them. Writer backends follow the rules of every backend: they read the list through the public API,
switch on each command's kind, and keep their complexity in the libraries.

| backend | output | text |
| --- | --- | --- |
| `svg` | SVG 2, a document of its own for each page, since SVG has no pages | glyph outlines, shared between uses on the page |
| `pdf` | PDF 2.0 (ISO 32000-2), tagged when a structure is given | embedded subsets of the fonts, with ToUnicode maps |

- **Sheets and boxes.** A page is a sheet (`PrDrawPage`): its list is drawn on the sheet's extent, and it gives the
  boxes that print production reads, the bleed box that its content is clipped to and the trim box of the finished page,
  as css-page-3's bleed area and page box. The producer decides the sheet, and the writer records what its format can. A
  PDF page's MediaBox is the sheet, with a BleedBox and a TrimBox where each differs from it (ISO 32000-2, 14.11.2). An
  SVG page is the sheet, since SVG has no such boxes. A page whose sheet is its page box writes neither box.
- **Units.** A page's extent is in px. A PDF page is in points, 72 to the inch, and CSS defines 96 px to the inch
  (css-values-4/#absolute-lengths), so the page's transform scales by 3/4 and flips y. An SVG page keeps px.
- **Text is a font.** A PDF glyph run shows the subset of its font's glyphs by glyph id, so the glyphs are the font's
  own, which a reader can select and search. An SVG document draws the outlines of its glyphs (`prFontGlyphOutline`),
  each defined once and placed by `use`, because SVG has no way to show a glyph by its id: a `text` element shapes its
  characters again, and the shaping of another reader can differ. A run's text, when the run carries it, labels its
  group (`aria-label`), so the document still reads.
- **Text is compact.** A PDF shows the glyphs of a line as one `TJ` array at the run's size, whose numbers move the
  text position only where layout placed a glyph away from where the font's width leaves it. A gap that most glyphs of
  the line share, as letter spacing leaves, is the character spacing (`Tc`), which a reader that extracts text counts
  as part of each glyph. Runs that follow one another share a text object and move between lines by `Td`. The writer
  leaves out an operator that would select the color, alphas or font that the graphics state already holds.
- **Fonts are subset where they are written.** `lib/font` writes a subset of a font's file (`prFontSubset`): TrueType
  outlines in an sfnt, or CFF charstrings in a bare CFF table, in both cases with the glyph ids of the font kept, so
  that a run's ids name the same glyphs in the subset. A font whose outlines its file does not hold, as a variable
  font's instance away from its default, a font with synthetic bold, or a CFF2 font, is subset as CFF charstrings of
  the outlines that it draws. A font with no subset, of bitmap strikes only, draws its glyphs as filled outlines,
  marked with the run's text as their replacement text. Synthetic oblique is the text matrix's shear, and size-adjust
  scales the text's size.
- **Text maps back to characters.** A run that carries its text (`PrFontGlyphRun::text`) and the cluster of each glyph
  maps each glyph to the characters of its cluster in the font's ToUnicode map. Where a glyph's characters differ
  between its uses, or where a cluster's characters do not divide among its glyphs, as with a ligature or a reordered
  cluster, the run's text is its replacement text (ISO 32000-2, 14.9.4) instead. A run without its text maps each glyph
  through the font's character map, inverted.
- **Streams are compressed** with the writer's codec (`lib/compress`), and images are written as their pixels, with a
  soft mask for alpha, compressed the same way. A writer without a codec writes them as they are. With a codec, a
  PDF's objects that are not streams, such as its structure elements and annotations, are compressed in object
  streams, which a cross-reference stream finds (ISO 32000-2, 7.5.7 and 7.5.8). A writer given a task pool
  (`PrDrawWriterOptions::tasks`) compresses several streams at once on its threads, and writes them in order.
- **A PDF has an identifier**, which PDF 2.0 requires: a hash of the bytes before its trailer, so the same pages make
  the same file. Its XMP metadata gives its title and language, and a tagged PDF claims PDF/UA-2 there.

### Native, lowered and rasterized

Commands keep what the producer meant, so a writer draws each one as natively as its format allows, and lowers it
through the shared utilities where the format has no equivalent: a border into its pieces (`prDrawBorderLower`), a
decoration into its area, a stroke the format cannot express into fills (`prDrawLowerStroke`), and a gradient into stops
that sample its ramp (`prDrawGradientRamp`), which the format interpolates between.

What has no native form and no lowering is rasterized: the CPU renderer draws the affected commands, and the writer
places the pixels as an image where they lie. This covers filters, backdrops, shadows, masks, projected groups, gradient
forms, blend modes and operators that the format does not have, nine-slices, color glyphs, and brushes over lists.

A renderer renders a part of a list (`PrDrawPart`, `prDrawFramePreparePart`) as it renders a group's contents into a
layer: a range of the list's commands that closes every scope it opens, under the transform that the walk holds where
they lie, placed in the render. Every renderer takes a part, so the writer rasterizes the command or the whole group in
the list where it was recorded, and builds no list and copies no command. The region is the bounds of the commands
(`prDrawCommandsBounds`), mapped to the page and cut to the clips around them, and the writer renders it at its raster
scale (`PrDrawWriterOptions::rasterScale`), in target pixels per px. The clips around the commands do not play in the
render: the writer places the image inside the format's own clips, which cut it. A rasterized group is one image, so
what it holds is no longer text. The image is in the first mark that the group holds, so the structure still reaches
it.

### Marks

A list carries the semantics of its content only as marks: `PushMark` and `PopMark` commands, which nest with the
list's other pushes and label the commands between them with a producer's key, such as the DOM node behind a layout
box. A mark has no effect on pixels, and a renderer skips it. A key may have many marks, such as the lines of a
paragraph or the pages of a table, and the list is in paint order, which is not reading order.

A producer marks content only: text and replaced elements. Box decoration, such as backgrounds, borders and rules, is
left unmarked, so it is decoration by default, an artifact in PDF's terms. Layout marks only when its client asks, so a
tree painting for a screen records no marks.

### The structure tree

The structure (`pr/draw_structure.h`) is an object of its own, beside the lists, that says what the content means:
nodes in reading order, each with a type and children. A node's type is one of PDF's standard structure types (ISO
32000-2, 14.8.4), a closed set that names roles a reader recognizes: headings, paragraphs, lists, tables, figures,
links. A child is a node or a key, which stands for every mark of that key, in paint order. A node also holds what a
reader needs beyond the content: alternate text, replacement text, the expansion of an abbreviation, its language, a
title, and for a link its URI or the key whose marks it leads to. The tree knows nothing of the web. Its producer
decides reading order, and the tree's order is reading order, whatever the paint order was.

The tree serves more than PDF. A platform's accessibility tree is the same tree joined to the same keys, with bounds
from layout in place of marks. Its nodes leave room for what later producers add: the header cells of a table cell,
MathML, and inline SVG.

### The join

The writer joins the marks to the tree. As it writes each page, it records each mark: its key, its page, the id of
its marked content, and its bounds on the page. At the end it resolves the tree.

- A key child becomes the marked content of each of its marks, as content references, on whatever pages they fell.
  One node may span pages.
- Content in no mark, or in a mark whose key no node names, is an artifact.
- A link's annotations are the bounds of its content's marks, one rectangle for each mark, and an annotation for each
  page that they fall on, each a child of the link's node.
- A link to a key goes to the first mark of that key: its page, and the top left of its bounds.
- The outline (bookmarks) follows the headings, nested by level, each going to its first mark.
- A key without a mark, or a destination that has none, leaves out the reference. Nothing fails.

A writer given no structure writes no tags, and every mark is ignored.

## Producers

This table shows how producers' features map to commands.

| source feature | becomes |
| --- | --- |
| **CSS** | |
| `background-color`, `background-image` layers, `background-blend-mode` | `Rect` or `CornerRect` per layer, image or gradient brush with tiles, command blend mode |
| gradients, `repeating-*`, color hints, `in <colorspace>` | brushes: extend, stop hints, `interpolation` and `hue` |
| `border`, `border-radius`, `corner-shape` | `Border`, corners |
| `border-image`, `mask-border` | `NineSlice`, over a list for a gradient |
| `box-shadow`, `text-shadow` | `DropShadow`, `InnerShadow`; a group with a shadow filter for text |
| `outline` | `Border` outside the box |
| `overflow`, `clip-path`, `mask-*` | `PushClipCornerRect` of `prCornerRectInset`, `PushClipPath`; group mask over a list composing the layers |
| `filter`, `backdrop-filter` | group filter, backdrop |
| `opacity`, `mix-blend-mode`, `isolation` | group opacity, blend mode, isolate |
| `transform`, `perspective` | `PushTransformAffine2`; a projected group |
| `text-decoration`, `-webkit-text-stroke`, `background-clip: text` | `Decoration`; stroked `Glyphs`; group masked by the glyphs' list |
| `image-rendering`, `object-fit` | sampling; source and destination rects |
| **SVG** | |
| shapes, `d` | `Rect`, `Ellipse`, `Line`, `Polyline`, `Path` (arcs converted by the path builder) |
| `fill-opacity`, `stroke-opacity` | the color's alpha, or the brush's opacity |
| `opacity`, `mix-blend-mode`, `isolation` | group opacity, blend mode, isolate |
| `transform` | `PushTransformAffine2` |
| `paint-order` | the order fill, stroke and markers are recorded in |
| `vector-effect: non-scaling-stroke` | a non-scaling stroke |
| `marker` | an `Embed` of the marker at each vertex |
| gradients, `objectBoundingBox` | brushes, `ObjectBounds` space |
| `pattern` | image brush over a list |
| `clipPath` of shapes; of text or `use`; `mask` | `PushClipPath`; group with an alpha mask; group mask |
| `filter` | filter graph |
| `textPath`, `rotate` | `Glyphs` with per-glyph transforms |
| `shape-rendering`, `image-rendering` | antialias; sampling |
| **Canvas 2D** | |
| `fillRect`, `strokeRect`, `fill`, `stroke`, `fillText`, `strokeText` | `Rect`, `Path`, `Glyphs`, with or without a stroke |
| `clearRect` | `Rect` with clear |
| `globalAlpha`, `globalCompositeOperation` | the paint's alpha; operator or blend mode, around the draw in a group where the operator changes what lies beneath a transparent source (`prDrawCompositeIsUnbounded`) |
| `filter`, `shadow*` | the draw inside a group with a filter |
| `save`, `restore` | the producer's recorder, emitting pops |
| `drawImage`, `createPattern` | a rectangle filled with an image brush, of the pixels for a canvas |
| `putImageData`, `getImageData` | `WritePixels`; the caller's pixels |
| `isPointInPath`, `isPointInStroke` | the path toolkit |
| `roundRect`, `ellipse`, `arc` | `CornerRect`, `Ellipse`, `Path` |
| **Lottie** | |
| shapes, fills, strokes, gradients | shape commands; brushes, opacity stops merged at the union of their offsets |
| trim paths, repeaters, rounded corners | the importer's geometry |
| merge paths, offset paths, mask expansion | `prPathPolylineCombine`, `prPathPolylineOffset` |
| masks and their modes, track mattes | a clip, or group masks over lists composing the masks by operator |
| precomps, time remap | their commands in place, clipped; the importer evaluates time |
| layer blend modes (hard mix included), effects | group blend mode, filter graph |
| text and its animators | a `Glyphs` run a letter, under the letter's transform |
| **XAML** | |
| brushes (`MappingMode`, `SpreadMethod`, `ColorInterpolationMode`) | brushes: `ObjectBounds` or `User` space, extend, `srgb-linear` interpolation |
| `ImageBrush`, `VisualBrush`, `DrawingBrush`, `TileBrush` flips | image brushes over images and lists, Reflect tiles |
| `Pen` | `PrDrawStroke`, with per-end and dash caps |
| `RenderTransform` | `PushTransformAffine2` |
| `CombinedGeometry`, `GeometryGroup` | `prPathPolylineCombine`, fill rule |
| `OpacityMask`, `Effect` | group mask with a brush; group filter |
| `EdgeMode`, `BitmapScalingMode` | antialias, sampling |
| **Illustrator and After Effects** | |
| appearance stack (several fills and strokes) | several commands over one path |
| stroke alignment, width profiles, gradient on a stroke | stroke alignment, profile, mapping |
| arrowheads, dash alignment | producer lists and dash arrays |
| knockout and isolated groups, opacity masks, clipping groups | group knockout, isolation, mask; clips |
| gradient meshes, freeform gradients | mesh brushes |
| glows, feather | filter nodes |
| **Figma** | |
| several fills, image fill modes | several commands; image brush transform and tiles |
| angular and diamond gradients | Conic and Diamond brushes |
| per-side stroke weights | `Border` |
| corner smoothing | `CornerRect` with superellipse corners |
| drop and inner shadows | `DropShadow` and `InnerShadow` on rectangles; filters elsewhere |
| layer, background and progressive blur; noise | group filter and backdrop |
| masks, boolean groups | group mask; `prPathPolylineCombine` |

## Testing

- Unit tests check primitives exactly, and geometry tests measure area at 8× resolution. Lowering utilities are tested
  as geometry.
- Backends are measured against `cpu`, within a declared tolerance.
- Every list feature gets a captioned drawing when it lands, in `examples/draw_features`, a library of drawings and an
  application that shows them (`./dev.sh example draw_features`). The library also holds dense workloads that measure
  the renderers.
- `./dev.sh bench draw` renders every drawing on every backend, and reports CPU and device time (`PrDrawStats`).
  `--images <directory>` writes what each row rendered as a PNG, so that the renderers' renders of a drawing can be
  compared by eye.
