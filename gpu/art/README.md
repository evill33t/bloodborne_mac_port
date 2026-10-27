# Co-op artwork provenance

Original author and art direction: **Kaniel / lKaniel**
([GitHub](https://github.com/lKaniel)). Imported unchanged from
[`lKaniel/bloodborne-server`, revision `8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1`](https://github.com/lKaniel/bloodborne-server/tree/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1/bbport/gpu/art).
The reference retains GPL-2.0-or-later notices in the artwork decoders and packers.

`coop/` contains the reference's custom co-op PNG sources: sigils, medallions, frames, marks,
dividers and the loading backdrop. Its packer documents creation with Nano Banana.
`coop_art.bin` is the packed atlas. `party_icons.svg` and `party_icons.bin` are the reference's
status icon source and atlas. Original file hashes are in
[`gpu/bbseamless/source_files.json`](../bbseamless/source_files.json).

The original packers are retained as `scripts/coop_art.py` and
`scripts/party_icons_atlas.py`; these are developer tools, not required to run the game.
The Mac build embeds both atlases in the GPU library. Its local adapter uses the reference's
silver color treatment and native Metal resource ownership.

The player's menu textures, HP gauge and game font are loaded separately at runtime.
They are not part of these artwork files or the application distribution.

The Mac team setup and hunt-loading screens now use the separately imported Windows launcher
hero image instead of this atlas's backdrop. See [branding provenance](../../assets/branding/README.md)
for Supermedo/bloodborne_pc, nick24051998, the exact source commits and file hashes.
