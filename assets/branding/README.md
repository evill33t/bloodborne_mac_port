# Windows launcher branding

Requested assets imported byte-for-byte from **Supermedo/bloodborne_pc**, branch `windows`,
pinned at [`5adea35dbd38851339b63773c1245c02a314f094`](https://github.com/Supermedo/bloodborne_pc/tree/5adea35dbd38851339b63773c1245c02a314f094).
The source repository's [GPLv2 LICENSE](https://github.com/Supermedo/bloodborne_pc/blob/5adea35dbd38851339b63773c1245c02a314f094/LICENSE)
is retained alongside these files as [LICENSE](LICENSE). Original image/artwork authorship
is not claimed by this Mac adaptation.

| Local file | Original source | Source change / author | SHA-256 |
| --- | --- | --- | --- |
| `bloodborne.ico` | [`launcher/bloodborne.ico`](https://github.com/Supermedo/bloodborne_pc/blob/5adea35dbd38851339b63773c1245c02a314f094/launcher/bloodborne.ico) | [`89170ac3`](https://github.com/Supermedo/bloodborne_pc/commit/89170ac382d6e447016664662d9a1599fe6dec67), **nick24051998**: Windows launcher emblem on a dark tile | `86dea795d41fe911ec52d57230c588cf131e583b785105a5b72061141690fed0` |
| `hero.jpg` | [`launcher/assets/hero.jpg`](https://github.com/Supermedo/bloodborne_pc/blob/5adea35dbd38851339b63773c1245c02a314f094/launcher/assets/hero.jpg) | [`9d5a4b21`](https://github.com/Supermedo/bloodborne_pc/commit/9d5a4b210e086a714e3c0f1a6c14aaea41b38b9a), **nick24051998**: Windows launcher visual assets | `96b1f8f8ee6f49fd6d5da90ba18291730caaaebc4bdce28c52c6d56bdac56cdf` |

The icon's source generator is the Windows launcher's `make_icon.py`; that implementation
is not copied here. The Mac [`build_icon.sh`](../../scripts/macos/build_icon.sh) converts the
selected ICO/PNG to an ICNS icon with macOS tools, continuing **Mark Hall's** existing app-icon
packaging approach from [bloodborne_mac_port](https://github.com/Wooloomooloo2/bloodborne_mac_port).
Both checkout and self-contained apps get this fallback. Selecting a supported game folder
still replaces the running icon with the player's `sce_sys/icon0.png`; clearing the folder
or selecting a folder without readable icon artwork restores the bundled icon.

The original 1536×1024 JPEG is embedded in both GPU-library architectures. The team setup
panel and “Entering the hunt” screen draw it with a centered crop that preserves aspect ratio,
with darkening for text contrast. Decoding uses ImageIO and the existing native Metal overlay;
its resource survives completion of unretained command buffers. Other co-op dialogs keep
the existing game-menu styling. No network image fetch is needed at runtime.

Local adaptation: **Ronny Stiftel / evill33t**, building on **Mark Hall's** native Metal overlay
and **Kaniel / lKaniel's** co-op presentation. This provenance is included in both app bundles.
