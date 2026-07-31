# V2M Playback Plugin

Plays V2 Synthesizer Music files (.v2m) using [v2redux](https://github.com/spheenik/v2redux),
a clean-room C++17 port of the V2 engine. V2M is the music format used by
Farbrausch's V2 synthesizer, widely used in demoscene productions.

v2redux plays every v2m format version (0..6) with that version's own engine
behaviour, so the plugin hands the original file bytes straight to the player
instead of converting them up to the newest format first.

## Supported Formats

| Format | Extension | Description |
|--------|-----------|-------------|
| V2M | .v2m | V2 Synthesizer module (format versions 0-6, played version-natively) |

## Visualization

| Feature | Source |
|---------|--------|
| Pattern cells | Extracted from the file's event streams (see below) |
| Scope | Per-channel post-FX capture (patched into v2redux) |
| VU | `Player::getChannelLevels` |

A .v2m stores 16 channels of delta-coded MIDI-ish event streams plus a global
tempo track, not a tracker pattern grid. `v2m_pattern.h` parses those streams
and lays them out as a grid of 32nd-note rows with Note / Vel / Pgm columns, and
integrates the tempo track into a row-to-sample table that drives the playhead.
The event section sits at the start of the file and has the same layout in every
format version, so no conversion is needed to read it.

`test_pattern.cpp` is the self-check: it asserts the grid parses and that the
tempo integration agrees with v2redux's own song length across the corpus.
Build with `-DV2M_BUILD_TESTS=ON` and run `ctest`.

## Library Source

| Library | Source | Commit |
|---------|--------|--------|
| v2redux | https://github.com/spheenik/v2redux | 5d3157b7c312fbfe2f82430dd3e819594cf3205b |

## Patches

- `v2redux-scope-capture.patch` - Adds per-channel oscilloscope capture, needed
  for the scope visualization. A ring buffer per channel is filled from the same
  post-FX tap the existing peak meter reads, gated behind an off-by-default
  enable flag, and exposed as `Player::setScopeEnabled` / `getScopeSamples`.
  Display-only: it reads the channel buffers and writes a side buffer, so
  v2redux's bit-exact render is unchanged.

## License

See [LICENSE](LICENSE) (public domain / CC0).
This plugin's integration code is licensed under the MIT License; see
[LICENSES](LICENSES).
