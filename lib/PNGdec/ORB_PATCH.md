# PNGdec 1.1.6, patched

A copy of [bitbank2/PNGdec](https://github.com/bitbank2/PNGdec) 1.1.6 (Apache-2.0, see
LICENSE), vendored here instead of pulled from the registry because 1.1.6 (the newest) has
a decode bug that the Orb's own theme art hits.

## The bug

`src/inffast.c`, with `ALLOWS_UNALIGNED` (set on every ESP32 and every 64-bit host). When a
deflate match starts in the sliding window and runs on into fresh output, the tail is copied
4 bytes at a time from `out - dist`. With `dist` of 1, 2 or 3 that reads bytes not yet
written. Each `inflate()` call here decodes one image row, so every row starts a new call
and the case comes up whenever a short-distance match straddles a row start.

What it looks like: a run of pixels gets the wrong colour, and every row below inherits the
error through the PNG Up/Average/Paeth filters. Bright green on a near-black hand, smeared
rainbow rows across a menu plate. It depends on the exact compressed bytes, so re-saving
the same image with other settings moves or hides it, which makes it look random.

## The fix

One branch: when that gap is under 4, copy byte by byte, exactly as the neighbouring
direct-from-output branch already does for its own short overlaps. Marked `ORB PATCH`.

## Checking

`tools/pngcheck/pngcheck.cpp` decodes a PNG with this library and writes raw RGBA, and
`tools/bigorb_theme.py` uses it to prove each converted PNG decodes pixel-exact. Against
twelve re-encodings of one 800 px plate, stock 1.1.6 got 7 of 12 wrong (up to 53,113 bad
pixels); this copy gets all 12 right.
