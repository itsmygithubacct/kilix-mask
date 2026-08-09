# kilix-mask

Region maps: which parts of an image mean something, and what.

The same problem turns up in places that look unrelated. A camera needs to know
which parts of its view to ignore — the tree that moves in every wind, the road,
the burnt-in timestamp. A top-down game needs to know which parts of a room can
be walked on, and which stand in front of the character. Both are a person
painting regions over a picture and something later reading them back.

This is the model and its file format. Painting interactively is a separate
concern; a consumer that only reads masks never links the editor.

Dependencies are a C11 compiler and zlib.

## Build

```sh
make
make test
make sanitize
```

## Use

```c
kmask *mask;
kmask_create(&mask, 1280, 720, /* cell */ 6);

kmask_fill_rect(mask, 100, 80, 600, 400, 1);
kmask_region_set_name(mask, 1, "walkable");
kmask_region_set_attr(mask, 2, "baseline", "640");

kmask_save(mask, "room.mask.png");
```

A region is an id from 1 to 255; 0 means unset. Masking uses ids 0 and 1 and
never notices the rest. A game gets walkable, water and blocked as separate ids.

## Cells

`cell` is how many source pixels one map cell covers. **1** stores a value per
pixel — what a motion mask or a walk-behind mask wants. Larger stores a grid,
which is what a tile game wants and which keeps the file proportionally smaller.

The grid rounds *up*, so a cell that does not divide the image evenly still
covers all of it. Painting a rectangle covers every cell it touches rather than
only the cells wholly inside it — filling a source rect should cover that rect.

Coordinates work in either space: `kmask_set()` takes grid cells,
`kmask_set_at()` takes source pixels. Out-of-range reads return 0 and writes are
ignored, so a brush sweeping off the edge needs no clamping by the caller.

## Attributes

Regions carry a name and free-form key/value attributes, because the consumers
disagree about what a region needs to hold: a walk-behind region has a baseline,
a terrain type has a movement cost, a camera zone has a label. Fixing those as
struct fields would mean changing this header for every new consumer.

Keys and values are ASCII without newlines or `=`. Both are structural in the
file, and refusing beats writing something that comes back mangled.

## Expansion, and one polarity worth reading twice

```c
kmask_expand(mask, out, size);                 /* region ids */
kmask_expand_exclude(mask, region, out, size); /* 0 where painted, 255 elsewhere */
```

The second is the one that matters and the one easy to get backwards. A motion
mask names what to **ignore** — the operator paints the tree — while the
detector consuming it treats **0 as ignore**. Producing that directly means
neither side inverts anything, and nobody discovers the mistake as a camera that
detects only the tree.

## The file is a real PNG

Not a private format wearing a familiar extension. Verified against PIL,
ffprobe and `file(1)`:

```
PNG image data, 214 x 120, 8-bit colormap, non-interlaced
```

- **Pixels are region ids**, written through a palette so a map appears as
  distinct colours rather than a near-black field of 0..3. Regions without a
  chosen colour get a generated hue, so a file is legible without anyone having
  picked any.
- **Region 0 is transparent**, so a map laid over its source image shows through
  where nothing is painted.
- **Names and attributes ride in a `tEXt` chunk**, which is the point of
  choosing PNG: the metadata cannot drift away from the bitmap it describes. A
  sidecar has to be reconciled on every load.
- **The image is the grid, not the source.** A 6-pixel grid over 1280×720 is a
  214×120 PNG — 1123 bytes — with the source size and cell in the metadata.

The metadata is plain text, so it reads cleanly in any tool that shows PNG
comments:

```
version 1
source 1280 720
cell 6
region 1 name=walkable
region 2 name=water cost=3
region 3 name=behind baseline=640
```

Chunks written by other tools are skipped rather than rejected — an image editor
that re-saved the file will have added some. A file that is a PNG but not one of
ours is refused, because the geometry lives in the metadata and there is nothing
to reconstruct without it.

## License

MIT. See `LICENSE`.
