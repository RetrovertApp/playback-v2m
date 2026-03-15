# V2M Playback Plugin

Plays V2 Synthesizer Music files (.v2m) using jgilje's v2m-player fork.
V2M is the music format used by Farbrausch's V2 synthesizer, widely used
in demoscene productions.

## Supported Formats

| Format | Extension | Description |
|--------|-----------|-------------|
| V2M | .v2m | V2 Synthesizer module (multiple format versions, auto-converted) |

## Library Source

| Library | Source | Commit |
|---------|--------|--------|
| v2m-player | https://github.com/jgilje/v2m-player | 385ad6956202d09a2912ef91df9c44e13f9e4a84 |

## Patches

- `v2m-sound-fixes.patch` - Two sound quality fixes to `synth_core.cpp`:
  1. **Moog filter DC offset** (line 331): Removes double subtraction of
     `fcdcoffset` that caused incorrect DC bias in the Moog filter output.
  2. **Noise filter state** (line 853): Fixes direction of noise filter
     state assignment so the computed filter result is saved back to the
     persistent state instead of being discarded.

## License

See [LICENSE](LICENSE) (Artistic License 2.0).
