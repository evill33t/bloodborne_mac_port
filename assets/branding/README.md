# App icon and co-op branding

## Default app icon

The `dev` branch uses the logo published by **Yharnam Together / Bloodborne Seamless Co-op**
at [`bloodborne.seamlesscoop.dev/logo.webp`](https://bloodborne.seamlesscoop.dev/logo.webp),
downloaded on **30 October 2026**. The publisher's source project is
**Kaniel / lKaniel and the bloodborne-server contributors**:
[`lKaniel/bloodborne-server`](https://github.com/lKaniel/bloodborne-server).
The website's [credits and GPL-2.0-or-later notice](https://bloodborne.seamlesscoop.dev/docs/credits)
identify its project and license; they do not name an individual artist for this logo.
Artwork authorship is not claimed by the Mac adaptation.

| Local file | Purpose | SHA-256 |
| --- | --- | --- |
| `seamless-coop-logo.webp` | Exact, unmodified downloaded source; 512×512 with transparency | `726f033100f445f4dbedc11ba4ee1f60de1e8c0fa3e836fb07762532b9e5951a` |
| `seamless-coop-logo.png` | PNG conversion preserving the source dimensions and alpha channel | `e6cbf13be98894963696782577ac5f5f666ef5330718492affb5621543f98dd1` |

The PNG was converted with macOS ImageIO through `sips`:

```sh
sips -s format png assets/branding/seamless-coop-logo.webp \
  --out assets/branding/seamless-coop-logo.png
```

Both checkout and self-contained app packaging pass this PNG to
[`build_icon.sh`](../../scripts/macos/build_icon.sh), which generates `Bloodborne.icns`
with the standard 16, 32, 128, 256 and 512 point entries at 1× and 2×. It preserves the
artwork's composition and transparent background. The source is bundled locally; the app
does not fetch the icon from the network.

Selecting a supported game folder still replaces the running icon with the player's
`sce_sys/icon0.png`; clearing the folder or selecting a folder without readable icon artwork
restores the bundled logo.

## Windows launcher assets

Requested assets imported byte-for-byte from **Supermedo/bloodborne_pc**, branch `windows`,
pinned at [`5adea35dbd38851339b63773c1245c02a314f094`](https://github.com/Supermedo/bloodborne_pc/tree/5adea35dbd38851339b63773c1245c02a314f094).
The source repository's [GPLv2 LICENSE](https://github.com/Supermedo/bloodborne_pc/blob/5adea35dbd38851339b63773c1245c02a314f094/LICENSE)
is retained alongside these files as [LICENSE](LICENSE). Original image/artwork authorship
is not claimed by this Mac adaptation.

| Local file | Original source | Source change / author | SHA-256 |
| --- | --- | --- | --- |
| `bloodborne.ico` (previous fallback; retained as a source reference) | [`launcher/bloodborne.ico`](https://github.com/Supermedo/bloodborne_pc/blob/5adea35dbd38851339b63773c1245c02a314f094/launcher/bloodborne.ico) | [`89170ac3`](https://github.com/Supermedo/bloodborne_pc/commit/89170ac382d6e447016664662d9a1599fe6dec67), **nick24051998**: Windows launcher emblem on a dark tile | `86dea795d41fe911ec52d57230c588cf131e583b785105a5b72061141690fed0` |
| `hero.jpg` | [`launcher/assets/hero.jpg`](https://github.com/Supermedo/bloodborne_pc/blob/5adea35dbd38851339b63773c1245c02a314f094/launcher/assets/hero.jpg) | [`9d5a4b21`](https://github.com/Supermedo/bloodborne_pc/commit/9d5a4b210e086a714e3c0f1a6c14aaea41b38b9a), **nick24051998**: Windows launcher visual assets | `96b1f8f8ee6f49fd6d5da90ba18291730caaaebc4bdce28c52c6d56bdac56cdf` |

The previous icon's source generator is the Windows launcher's `make_icon.py`; that
implementation is not copied here. The Mac icon conversion continues **Mark Hall's**
existing app-icon packaging approach from
[bloodborne_mac_port](https://github.com/Wooloomooloo2/bloodborne_mac_port).

The original 1536×1024 JPEG is embedded in both GPU-library architectures. It fills the screen
behind the team setup window, replacing the gray surrounding area; the setup window retains
the game's menu styling. It also backs the “Entering the hunt” screen. A centered crop preserves
aspect ratio, with darkening for text contrast. Decoding uses ImageIO and the existing native Metal overlay;
its resource survives completion of unretained command buffers. Other co-op dialogs keep
the existing game-menu styling. No network image fetch is needed at runtime.

Local adaptation: **Ronny Stiftel / evill33t**, building on **Mark Hall's** native Metal overlay
and **Kaniel / lKaniel's** co-op presentation. This provenance is included in both app bundles.
