# Textures

`lib/texture` turns images into textures that a shader samples. It has four parts:

- **Sources** produce a texture's bits on demand. A source keeps the means to produce the bits, such as a file's path,
  and never the bits.
- **Format sets** open encoded bytes as sources. The library itself depends on no file format.
- **The catalog** names sources by path, so that one file has one source.
- **The cache** holds the textures that sources realize on one device. Every consumer on that device shares it:
  lib/draw's renderers, the layout tree, Studio's views, and a 3D renderer.

The headers state each call's contract: `pr/texture.h`, `pr/texture_catalog.h` and `pr/texture_cache.h`. This file
explains what the parts are for and how they fit together.

## Dependencies

```
lib/core (PrBlobSource)   lib/color   lib/gpu   lib/mime   lib/task
          \                   |          |        /           /
           +------------- lib/texture ---+-------+-----------+
                    /         |          \
   lib/ktx (with lib/basis)   lib/dds      lib/image         lib/draw, lib/layout, lib/ui, Studio
   KTX 2.0's format           DDS's        the format of     ask the cache for what they sample
                              format       every image codec
```

- **The containers of GPU textures share the block formats.** `lib/ktx` and `lib/dds` describe and decode their
  blocks with `lib/bc` and `lib/etc2`, which depend on no GPU interface.
- **Only a format's library reaches the format.** `lib/ktx` makes the sources of KTX 2.0 files, `lib/dds` those of DDS
  files, and `lib/image` makes the sources of every image that an image codec decodes. `lib/texture` includes no
  format's header (`lint.py`, `format-dependency`).
- **A library that loads textures takes a format set.** The program builds the set from the formats that it wants
  (`prTextureFormatSetCreate`), or takes the selector's set of every format (`prTextureFormatSetCreateDefault`).
- **A library that draws takes a cache.** It asks the cache for a source's texture each time it draws the source. It
  never creates a GPU texture for an image itself.

## Sources

A source (`PrTextureSource`) describes a texture and produces its bits when asked. It is immutable and reference
counted.

- **A description** (`PrTextureDesc`) gives the texture's kind, extent, layers and mip levels, one GPU format, its
  alpha, and its orientation.
- **The simplest source is a function that fills the bits** (`prTextureSourceCreate`). A format's library reads a file
  and decodes it. An application fills the bits from data that it keeps, computes them, or renders them.
- **A source can wrap another source and change its format.** A format's library decodes or transcodes to another
  format (`prTextureSourceReformat`). This library converts pixels (`prTextureSourceCreateConvert`), makes mip levels
  (`prTextureSourceCreateMips`), and arranges tiles in a grid (`prTextureSourceCreateTiles`). A chain of such sources
  is a load pipeline. The consumer of the chain sees one source.
- **A source keeps nothing between reads.** Each read of a file's source reads the file and decodes it again. This is
  what lets a texture be made again after a device is lost.
- **A source of pixels that nothing can produce again keeps a copy of them** (`prTextureSourceCreatePixels`). Use it
  for pixels that exist only in memory, such as a canvas's snapshot. Do not use it for a decoded file.

Color and alpha follow two rules:

- **The format says how a sampler reads the values.** A format's Srgb form holds values encoded with sRGB's transfer
  function, and a sampler decodes them. A plain form's values are read as stored. A texture carries no primaries.
- **A texture keeps its alpha.** A source is opaque, straight or premultiplied, as its bits are. The cache converts to
  the alpha that an ask reads.

## Reads

A read (`PrTextureRead`) asks a source for a range of its levels and layers. It completes once, during the call or
later, on any thread.

- **A read goes into memory or into a texture.** A read into memory fills storage that the caller gives or that the
  read allocates. A read into a texture names a device and a GPU texture, and fills the texture.
- **A source can fill a texture itself** (`PrTextureSourceFlag_FillsTextures`). Such a source reaches the device by a
  path of its own. Examples are a loader that reads a file's blocks into GPU memory through the device's I/O, and a
  source that renders its texture. No copy of the bits passes through memory.
- **A source that fills textures states the uses that its texture needs** (`PrTextureSourceDesc::usage`). A compute
  shader writes a texture with the Storage use, and a render pass draws into one with the RenderTarget use. A consumer
  creates the texture with those uses, in the source's own format, and does not pack it into a page. A procedural
  texture is such a source: it keeps its parameters, and a read dispatches its shader into the texture.
- **The library fills the texture for every other source.** It reads the bits into memory, uploads them, and frees
  them before the read completes. A consumer on a device always reads into a texture, so it gets the best path that
  the source has, and no bits outlive the read.
- **A CPU consumer reads into memory.** A writer that embeds an image's pixels, or a filter that runs on the CPU, reads
  the source directly. No device is involved.

## Previews

A source may have previews (`prTextureSourcePreview`): smaller versions of the same picture, each a source of its own.
A preview costs much less to read than the picture, and its texture is smaller.

- **Previews are what the file offers.** A camera's JPEG embeds a thumbnail in its Exif data. A JPEG also decodes at a
  quarter and at an eighth of its extent for a fraction of the cost of the whole image. `lib/image` lists both. It
  does not list a decode at half the extent, which saves too little. An image whose format offers nothing, such as a
  PNG, has no previews and is read whole.
- **An embedded thumbnail must show the same picture.** `lib/image` uses it only if it has the image's aspect ratio
  and components, and the image has no ICC profile. Cameras letterbox some thumbnails, and those are skipped.
- **A consumer that shows a picture small reads a preview.** A gallery of JPEG thumbnails reads a preview for each
  file. It never decodes a whole image.
- **A consumer that waits for a picture shows a preview meanwhile.** The preview arrives first because it costs less.

The cache applies both rules from the detail that an ask states. A consumer does not choose a preview itself.

## Sources that change

A source may show a picture that changes: an animated image, an image whose bytes are still arriving, or a video. Such
a source has a function that says what it shows at a time on a timeline (`PrTextureSourceDesc::show`,
`prTextureSourceShow`). The answer is a picture, which is a still source of its own with the same description, a serial
that names the picture, whether the picture is complete or an approximation, and when the source shows another.

- **Time is an input.** The time is seconds since the start of a timeline that the consumer owns
  (`PrTextureTimeline`). The source keeps no start time and nothing of any playback, so one source shows on any number
  of timelines at once, each paused, seeked or played at its own rate.
- **A change is a time, a notification, or never.** The answer gives the time at which the picture changes, says
  whether the source notifies its notifier (`PrTextureSourceDesc::notifier`) when it shows another, or both, as a video
  does whose frame at a time has not decoded yet. A source that says neither is never asked again.
- **The source itself is the picture of a consumer that does not play it.** That is an animation's first frame, and the
  picture of all of an image's bytes.
- **The provider keeps the timing of its file.** An animated image starts at time zero and plays its frames for the
  durations that its file states, as often as the file says. A frame that the file gives less than 20 ms shows for
  100 ms.
- **Pictures that are read in order read fastest.** A GIF and an animated PNG store a frame as its difference from the
  frames before it. The frames of an image keep a few decoders open, and a read takes the one that last decoded the
  nearest frame before its own, so that playbacks at different times each keep a decoder moving forward. No two reads
  share a decoder at once.

A consumer does not call the function itself. The cache plays such sources for every consumer ([Playing](#playing)).

## Bytes that arrive

A resource's bytes may still be arriving: from the network, or from a host that feeds them (`PrBlobFeed`, in
`pr/blob.h`). A blob source of such bytes says how many have arrived and whether they are complete or failed
(`prBlobSourceProgress`), and notifies its notifier as that changes. The bytes that have arrived never change, and stay
readable if the rest fail.

- **A source opens as soon as its bytes describe it.** A format set tries the bytes that have arrived each time they
  have doubled, and every byte at the end (`prTextureFormatSetOpen`). A format that needs every byte, as KTX and DDS do,
  waits for them.
- **An image shows revisions as its bytes arrive.** The source of an image that opened early changes
  ([Sources that change](#sources-that-change)). Its bytes go to a decoder that keeps its state from one piece to
  the next (`prImageDecoderAppend`): each byte is decoded once, so the work of an image is proportional to its bytes
  and its pixels, however small the pieces. A revision is a snapshot of what the decoder holds, taken when a consumer
  asks after the decoder changed, so revisions follow the consumer's frames, not the arrival of the bytes. A revision
  is not complete, so the cache answers it as Partial. Once the bytes are complete, the source shows itself, the image
  of every byte, and never changes again.
- **Each revision is a picture of its own.** No texels change under a held region. The cache frees superseded
  revisions as it frees an animation's past frames, and there is at most one revision for each frame that a consumer
  draws.
- **An ask that does not play sees the image arriving too.** `prTextureCacheRealize` answers a source that notifies
  with what it shows at the start of a timeline, Partial until it is complete, and no realization waits for bytes.
- **A failure keeps the last revision.** Bytes that fail, or that end before the image does, leave the last revision
  as the picture that stays, with the failure (`PrTextureShow::status`, `prTextureCacheStatus`).
- **What a revision shows is the codec's.** PNG decodes the rows that have arrived, and an interlaced PNG fills each
  block from the passes so far. A sequential JPEG decodes the rows of blocks that have arrived, and a progressive one
  the whole picture at the precision of its scans so far. GIF draws the pixels of the frame that is arriving, and an
  animation plays the frames that have begun; an animated PNG plays its frames as each becomes whole. WebP, and every
  format without a streaming decoder, shows nothing until its bytes are complete.

## Formats

A format set (`PrTextureFormatSet`) is an ordered list of formats. A format (`PrTextureFormat`) makes sources of the
encoded bytes of one file format.

- **Opening reads the bytes once to describe the texture** (`prTextureFormatSetOpen`). The set classifies the first
  bytes and the name with lib/mime. The first format that reads that media type and accepts the bytes makes the source.
  The source keeps the byte provider (`PrBlobSource`), not the bytes.
- **The order is the priority.** KTX 2.0 goes before the image format, because an image codec set may decode a KTX
  file's pixels and lose its blocks.

| format | made by | what its sources produce |
| --- | --- | --- |
| KTX 2.0 | `lib/ktx`, `prKtxTextureFormatCreate` | blocks of BC, ETC2, EAC and ASTC as stored; Basis Universal's ETC1S and UASTC transcoded to the blocks that the device samples; pixels where the blocks decode; levels, layers and cube faces |
| any image that a `PrImageCodecSet` decodes | `lib/image`, `prImageTextureFormatCreate` | pixels as Rgba8, Rgba16F, Rgba32F or R8; the image's own mip levels; previews; the image's alpha and EXIF orientation |
| an application's | `prTextureSourceCreate` | the levels that its function fills |

An image's source reads its values in one of two ways (`PrImageTextureColor`). `Stored` keeps the values as the file
has them, for a library that decides by its own rules whether they are color, as a material does. `Srgb` converts the
image to sRGB for display, through its ICC profile or its named color space.

## The catalog

A catalog (`PrTextureCatalog`) gives the source of a named resource. The name is a file's path, or a name that the
caller chooses for bytes from an archive or the network.

- **One name has one source.** Two consumers that ask for the same path get the same source, so the cache holds one
  texture for both.
- **A consumer asks every frame** (`prTextureCatalogSource`). The first ask starts opening the source on the catalog's
  task pool. Later asks return the source once it has opened, or the failure.
- **The catalog does not keep sources alive.** A source lives while a consumer or a cache holds it. When the last
  holder releases it, the catalog forgets the name, and a later ask opens the resource again.
- **A changed resource is forgotten explicitly** (`prTextureCatalogForget`). The next ask opens it as a new source.
- **Bytes that arrive open as they arrive** ([Bytes that arrive](#bytes-that-arrive)). A name is Pending until the bytes
  describe the source, and the open completes on the thread that delivers them.

## The cache

A cache (`PrTextureCache`) holds the textures that sources realize on one device. A host creates one cache for each
device and gives it to every consumer that samples on that device.

### Asks

A consumer asks for a source each time it samples it (`prTextureCacheRealize`). The ask (`PrTextureCacheAsk`) states
what the consumer reads:

- **Values**: color that a shader blends as encoded (`Encoded`, which lib/draw reads), color in linear light
  (`Color`), or data (`Data`). The values decide the texture's format and how made mip levels are filtered.
- **Alpha**: straight or premultiplied.
- **Channels**: the channels that the consumer reads. An ask for one channel of data may realize in one channel.
- **Levels**: whether the consumer samples mip levels where it minifies.
- **Detail**: how many texels the consumer shows along the longer side.
- **Priority**: how soon the consumer needs the texture.

The cache chooses the texture from the source, the ask and the device. A consumer does not repeat those choices.

- **Format.** Blocks stay blocks if the device samples them. Basis Universal transcodes to the best format that the
  device has.
- **Mip levels.** The cache makes mip levels only if the consumer samples them and the version that it realizes has
  more texels than the consumer shows. The region says how many levels the texture has (`PrTextureRegion::levelCount`),
  and the consumer samples accordingly.
- **Sharing.** Asks that need the same texture share it, so a material and a UI that read one image alike use one
  texture.

### Detail and partial results

The cache realizes the smallest version of the source that has the detail that an ask states. A version is the source
or one of its previews.

- An ask returns `Ready` with the region of that version once it has arrived. A larger version that the cache already
  holds also serves the ask.
- An ask returns `Partial` if the needed version has not arrived and a smaller one has. The region holds the smaller
  version. The consumer draws it and asks again when the cache reports an arrival.
- An ask returns `Pending` if nothing of the source has arrived, and `Failed` if the source does not read.

A region (`PrTextureRegion`) holds the whole picture in the texels of the version that was realized. A consumer that
addresses the picture in the source's texels converts with `prTextureRegionRect`. lib/draw does this when it makes a
source from a region ([draw.md](draw.md#images)), so a drawing looks the same whichever version it draws.

### Order and cancellation

Realizations wait in a queue. The cache keeps at most as many in flight as its task pool is wide.

- **The best waiting realization starts first.** The order is priority, then the latest frame that asked, then the
  smaller version.
- **High priority starts at once.** It does not wait for a place. Use it for what the user is looking at.
- **A trim cancels what no frame asked for** (`prTextureCacheTrim`). A consumer that scrolls past a thousand images
  reads only the ones that it still shows.
- **A cache with a pool reads the smallest preview first** if nothing of the source has arrived, so that the consumer
  has something to draw.

A realization that has started is not cancelled. Its read runs to completion.

### Lifetime and placement

- **The cache keeps a region while anything else holds its source.** A drawing that holds the source may keep the
  region. A producer that records a region into a draw list makes the list hold the source ([draw.md](draw.md)).
- **The cache has a budget for all of its textures.** While it is over budget, a trim evicts textures that no frame
  asked for and whose sources only the cache holds, least recently asked first.
- **Small sources share pages where textures bind to slots.** A source of 8 bits a channel, at most 256 texels on a
  side, with one level, goes into a slot of a shared page on a device that draws through a GPU without bindless. This
  saves a binding and a draw call for each. A device with bindless reaches every texture by handle, so the cache does
  not pack there, and every source keeps hardware sampling and its mip levels. A memory device reads texels in place,
  so the cache does not pack there either.

### Playing

The host gives the cache the time of each frame that it draws (`prTextureCacheSetTime`). A consumer that plays asks
with `prTextureCachePlay` and the timeline that it plays on. The answer for a source whose picture changes is the region
of the picture that the source shows at the timeline's time then, marked as playing (`PrTextureRegion::playing`), and
the source of that picture.

- **The consumer owns the timeline.** A widget pauses or seeks its own, and asks with it as it is now. A consumer that
  keeps none passes null and plays on the cache's own timeline of the source, which starts at the first such ask.
- **The source is asked only when its answer may have changed.** That is when the timeline reaches the time that the
  last answer gave, when the timeline moves back before it, or when the source has notified. A notification is an
  arrival, so the consumers ask again. A source whose answer says it never changes costs an ask nothing but the
  lookup, and adds nothing to the host's frames.
- **Timelines share pictures.** Two timelines that show the same picture of a source, by its serial, share one picture
  and one texture.
- **An approximation is Partial.** A picture that the source says is not complete is answered as Partial, as a smaller
  version is, so a consumer treats an image that is arriving as it treats one whose preview shows.

- **A consumer that plays has no code for animation.** Layout asks this way for an image element and a CSS background,
  and draws the region with its own sampling, fit and tiling, as it draws any image.
- **A drawing holds the picture that it draws.** The answer names the picture's source. A draw list that draws the
  region holds it, so the cache keeps the texture for as long as the list draws it.
- **A picture shows until a newer one has arrived.** The cache realizes the picture that is due, on its pool or inside
  the ask, and answers with the last picture meanwhile. A source that moves on before a picture arrives skips it.
- **The host draws when a picture is due.** The cache reports an arrival when a playing source shows another picture,
  which makes a consumer that keeps a drawing ask and draw again, and it reports the time of the next picture
  (`prTextureCacheNextFrame`), at which the host draws a frame.
- **A picture that is due starts at once.** Its realization is not held back by the pool's limit, so the end of a
  frame cannot cancel it.
- **A source that nothing asks for stops.** A source plays while consumers ask for it, and for a second after the last
  ask: a consumer that records on another thread asks less often than the host's frames end.
- **Pictures that are past are freed at once.** They are not asked for again, so a trim frees their textures whatever
  the budget, once no drawing holds them.
- **`prTextureCacheRealize` does not play.** It answers with the source's own picture, which is what a row of
  thumbnails shows.
- **Device loss keeps the playback.** What the cache learned of the sources that it plays stays when it moves to
  another device, so each shows the same picture there.

A cache whose time was never set is at time zero.

### Cost

The cache is built for a scene with many thousands of textures. No operation's cost grows with the number of textures
that the cache holds.

- An ask is one lookup by key.
- Starting a realization takes the head of a priority queue.
- A trim does work only for what it cancels and evicts. A texture that could not be evicted because its source was
  held is set aside, and each trim rechecks a fixed number of those.

A consumer that kept a drawing while an image was on its way does not poll every image. It compares one counter
(`prTextureCacheArrivals`) with the value that it saw when it drew.

### Device loss

The host moves the cache to the new device (`prTextureCacheSetDevice`). The cache frees its textures, cancels its
queue and advances its generation. Each source realizes again when a consumer next asks for it. The read comes from
the source, so a file's texture comes from the file. The cache keeps no copy of any texture in memory.

A host that keeps drawings of regions compares the generation with the one that it drew in, and draws again if they
differ.

A host that has no new device yet moves the cache to no device (a null device), and then frees the lost device. The
cache answers every ask with Pending and starts nothing until the host gives it a device.

## CPU and GPU renderers

Both kinds of renderer use the same sources, catalog and cache.

- **A GPU renderer** samples the cache's textures on its device. The bits pass through memory only during a read, or
  not at all for a source that fills textures.
- **A CPU renderer** draws on a memory device (`prGpuMemoryCreate`). A texture on that device is pixels in memory, and
  the renderer reads them in place. The one copy of the pixels is the texture itself.

## Threads

Sources, reads, format sets and catalogs are safe to use from any thread. A source is immutable, and any number of
reads may run at once. A read completes on the thread of its innermost provider.

Any thread may ask a cache. The cache realizes on the host's task pool. Without a pool, a realization runs inside the
ask that starts it, which suits a test or a capture that renders one frame. The cache calls the host's `arrived`
function when a realization that an ask was waiting for arrives or fails, so that the host draws again.
