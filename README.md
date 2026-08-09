# kilix-mask

Region maps: which parts of an image mean something, and what.

The same problem turns up in places that look unrelated. A camera needs to know
which parts of its view to ignore — the tree that moves in every wind, the road,
the burnt-in timestamp. A top-down game needs to know which parts of a room can
be walked on, and which stand in front of the character. Both are a person
painting regions over a picture and something later reading them back.

Two libraries. `libkilix-mask` is the model and its file format, and needs
only C11 and zlib. `libkilix-mask-edit` is the painting on top, and adds
soft-raster. A consumer that only reads masks links the first and never pays
for the second.

## Build

```sh
git submodule update --init --recursive
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

## Rectangles

A map is convenient to paint and inconvenient to ship: a game storing rooms as
JSON wants a handful of rectangles, not a bitmap. `kilix_mask_rects.h` converts
between the two, in a separate header because most consumers never need it — a
motion mask is read as a bitmap and never becomes rectangles at all.

```c
kmask_rect bounds, holes[256];
size_t needed;
kmask_decompose(mask, region, &bounds, holes, 256, &needed);
kmask_apply(other, region, &bounds, holes, needed);   /* exact inverse */
```

`kmask_cover()` gives rectangles covering a region directly. `kmask_decompose()`
gives the shape a room model usually wants — one bounding rectangle plus the
holes inside it — because a room is mostly floor with furniture in it rather
than the reverse, and "this box minus these obstacles" is far smaller than a
cover of the floor.

**The round trip is a fixpoint**, and that is the property worth caring about:
decomposing a map and painting the result back reproduces it cell for cell.
Without it every edit-save-reload cycle would erode a shape slightly, and the
drift would only surface after enough cycles that nobody could say which edit
caused it. The tests pin it over hand-built room shapes and over forty random
ones at varying cell sizes.

Two details that only matter on real painted shapes:

- **Both sweep orientations are tried** and the smaller kept. Painted shapes are
  run-heavy in one direction — 50 horizontal stripes cost 50 rectangles
  row-major and 5000 column-major.
- **Runs are grown before they are thickened**, and only while the whole run
  stays inside the shape. Thickening cell by cell leaves ragged single-cell
  rectangles along every diagonal edge.

Over capacity, the count needed is reported rather than the list truncated: a
silently short list of holes decomposes into a different room.

## The editor

`kilix_mask_edit.h` is the painting: a viewport, brush / rectangle / flood-fill
/ pick tools, undo, and a compositor that draws the image, the region tints,
the grid and the cursor into a soft-raster canvas.

There is no terminal anywhere in it. That is not tidiness — a painting tool is
close to untestable once its logic lives inside an event loop, because the two
bugs that actually matter are invisible from outside a running session:

```c
kmaskedit_create(&editor, mask, &plate);
kmaskedit_set_view(editor, 1280, 720);

kmaskedit_press(editor, 200, 140, KMASKEDIT_BUTTON_PAINT);
kmaskedit_drag(editor, 260, 190);
kmaskedit_release(editor, 260, 190);

kmaskedit_compose(editor, &frame, 0, 0);
count = kmaskedit_take_damage(editor, rects, KMASKEDIT_DAMAGE_MAX_RECTS);
```

**The pointer and the picture agree by construction.** Composition samples the
image through the same lookup tables `kmaskedit_to_source()` reads, so the cell
drawn at a view pixel is always the cell that painting there will change. The
alternative — deriving the mapping once to draw and once to hit-test — puts
paint one cell away from the cursor at some zoom levels and not others, which
is close to undiagnosable from a screenshot. There is one mapping, so the two
cannot drift apart. The test composes an image whose pixels are all
distinguishable and reads back every view pixel at five scales, panned off
centre.

**Damage is never under-reported.** Every change records the view rectangle it
could have touched, rounded outwards. Too large costs a few bytes on the wire;
too small leaves a pixel stale until something unrelated repaints it. The test
composes, acts, composes again, and requires every moved pixel to fall inside a
reported rectangle — over four hundred random operations, because the damage
bugs that survive are in combinations nobody thinks to try. It caught one:
shrinking the brush reported the new cursor footprint and left the old, larger
outline on screen.

Whether a damaged area is still worth patching is not decided here.
`kittyfb_present_damage()` already measures that and falls back on its own;
two thresholds that could disagree would make the cheaper path depend on
whichever was stricter.

Some smaller decisions that are easy to get wrong:

- **A stroke decides paint-or-erase once, at press.** If the cell under the
  pointer already holds the active region the whole stroke erases. Deciding per
  cell makes a drag across a boundary flicker on and off under the pointer.
- **Drags interpolate.** A terminal reports motion at whatever rate it manages,
  so a quick drag arrives as two distant positions; without this a stroke is a
  row of dots. Walking in cells rather than pixels keeps the work proportional
  to what is painted.
- **Brush sizes are odd.** An even footprint has no centre cell to sit under
  the pointer, so it paints off to one side.
- **A background whose size differs from the mask is refused**, not scaled.
  Scaling puts every region over the wrong pixels while looking plausible.
- **History is bounded and drops the oldest**, subject to a byte budget as well
  as a count, because one flood fill can be millions of cells. The newest entry
  is never dropped, even if it alone exceeds the budget.

`kmaskedit_compose()` draws at an offset and honours the canvas' existing clip,
so reserving a status bar takes no cooperation from the editor.

## The command

```sh
kilix-mask --image plate.ppm --cell 6 room.mask.png
```

Drag to paint, right-drag to erase, wheel to zoom at the pointer, `?` for keys.
An existing mask is loaded and **its** geometry used — `--cell` and `--size`
describe a new mask only, because re-gridding a loaded one would move every
cell that was ever painted.

Two ways to run it without a terminal:

```sh
kilix-mask --image plate.ppm --render frame.ppm room.mask.png
kilix-mask --selftest
```

`--render` composes exactly what the editor would show, chrome included, so a
picture of the tool cannot drift from the tool. `--selftest` exercises the
assembled binary — create, paint, encode, decode, decompose, apply, compose —
and is the only check available on a machine that has the program installed but
not this source tree. `make test` runs it alongside the suites.

Pictures are binary PPM. Decoding arbitrary image formats is not this module's
job and the error message says so:

```sh
ffmpeg -i snapshot.jpg -pix_fmt rgb24 plate.ppm
```

The terminal side is deliberately thin — read the pointer, place it in the
frame, hand back pixels — because it is the one part no test reaches. Two
things it does need to get right:

- **A pixel mouse report is relative to the terminal**, and the frame is
  centred inside it. `kittyts_origin_x()` / `_y()` give the difference. That
  accessor did not exist; it was added for this, since the alternative was
  re-deriving the centering and letting it drift from the real placement.
- **Frames and patches are different presentation calls.** Editor damage plus
  the status strip goes to `kittyts_present_damage()`; a resize or the help
  overlay goes to a full `kittyts_present()`.

## License

MIT. See `LICENSE`.
