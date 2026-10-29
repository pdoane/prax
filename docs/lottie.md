# Lottie

`lib/lottie` reads animations into draw lists. What it draws is decided from the Lottie specification, After Effects,
the players that files are made for, and the issues, as [behavior.md](behavior.md) describes. Comments state each
choice.

## Reading

`prLottieAnimationCreate` parses JSON once into a read-only model: compositions, layers, shape items, and properties
that share one pool of floats. Everything that needs the host happens here: images are decoded and loaded through the
host's decoders and loader, text is shaped per paragraph and then split into letters at grapheme cluster boundaries,
and corrections for older exporters are applied by file version.

## Drawing

- A layer's properties are evaluated at its composition's frame. A precomp's time is its time remap, or else the
  parent's time minus the start time, scaled by the stretch.
- Layers draw from last to first, within their in and out points. Each layer is nested as follows, from the outside
  in: a blend or matte group, then its transforms, composed in 3D and drawn flat, then an opacity group, then the
  precomp's size clip, then the masks. Effects become a filter graph.
- A composition's active camera is its topmost camera layer in range. Under a camera, a 3D layer is a projected group
  (`PrDrawGroup::projection`) of its plane: its transforms, then the camera's view, then a perspective of the camera's
  zoom about the center of the composition as it plays. The group holds the layer's opacity and its content's bounds,
  and the clip, masks and effects stay in the plane. A run of consecutive 3D layers draws farthest first, by the
  depth of each anchor, without splitting planes that intersect. 2D layers, and 3D layers without a camera, draw
  flat.
- Shape items are taken from last to first. Each style paints every path that comes after it in its group and in
  nested groups, as one path in the style's group space. Each path is first transformed by the modifiers that come
  before it, nearest first.
- Masks that only add at full opacity become a clip. Any other masks become a luminance mask, one mask at a time.

## Expressions

With `PR_JS`, QuickJS evaluates expressions once per frame, with After Effects' globals and an object tree that can be
looked up by name, match name or index. If an expression throws, the property keeps its keyframed value. Without the
option, keyframes are used.

## dotLottie

The animations in a `.lottie` ZIP archive read their files from the archive.

## Testing

Each corpus of Lottie animations is a suite of its own, compared with the player that its tests were written for
(docs/testing.md). A suite's errata record where that player is wrong, as decided from After Effects and the
specification, and its cut list the cases that need a player's own extension. The viewer shows each animation beside
its suite's reference.
