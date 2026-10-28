# Native Metal map editor development

Requested scope: a usable Bloodborne editor for props, enemies, items, NPCs and routes;
quest/event authoring where the formats and runtime permit it; importing content from
Smithbox-supported games, prioritizing Demon's Souls, Dark Souls 1, Dark Souls 2,
Dark Souls 3 and Elden Ring. Continue on `codex/metal-map-editor`.

## Acceptance sequence

1. Complete and validate prop/enemy duplication: unique identifiers, native selection and
   transforms, creation/removal undo/redo, safe mod export, original plus duplicate enemies
   in combat and reset, and a copied prop's loaded world transform. Preserve original saves
   and assets. Exported files must also pass an independent SoulsFormats reference check.
2. Add region and route editing: visible waypoints, patrol references and ordered routes,
   transform/history/export support, and a bounded ordinary-enemy patrol test.
3. Add NPC configuration: identify friendly/scripted characters, edit supported NPC/AI,
   talk, initialization and animation references with parameter lookup and validation.
4. Add item placement: inspect treasure events and item lots, duplicate a pickup with its
   supporting part/event references, choose its contents and verify collecting it in-game.
5. Add quest/event tools: inspect relevant compiled events and their entity/flag references,
   expose supported edits with format checks, and validate an isolated event change.
6. Build source-format import adapters and a conversion report for each supported game.
   Verify available content in the Metal preview, then validate converted content in-game.
   Track geometry, textures, skeletons, animations, AI, collision, navigation and events
   separately. A scene preview or file parse does not establish playable enemy/map conversion.

## Format and test constraints

Use Smithbox/SoulsFormats as format reference and independent verification. Keep native
Metal rendering and document ownership in the editor. Reuse the port's loose-file mod
overlay; never overwrite original game data. Keep temporary gameplay observers and player
test controls under ignored `out/`, with private save copies and bounded runs.

Preserve existing record payloads where fields are not edited. Structural exports must
rebuild valid offsets, counts and references, keep retail type grouping, and reopen before
writing. Initial duplication testing demonstrated that an append-only, interleaved part
table accepted by SoulsFormats can still fault the retail loader. The grouped writer and
reference-target tests replace that approach.

Cross-game assets require source dumps for real-asset validation. A format adapter alone
does not provide animation, AI, collision or event compatibility with Bloodborne.

## Current evidence

Completed checkpoints cover prop/enemy duplication, regions/patrols, native NPC/AI and
area-talk lookup, independently collectible item pickups, and typed EMEVD editing plus a
pickup-reward quest template. Patrols, NPC stats, item collection/reload and quest
execution/reload have bounded in-game evidence. Optional character bind-pose previews and a native source-map browser now use a separate
SoulsFormats adapter. DS2 Scholar archive import and source placement transfer have real-asset
checks; foreign models, animations, collision, navigation and behavior still need conversion
before they can become playable Bloodborne content. Broader quest/dialogue authoring remains work.

See [MAP_EDITOR.md](MAP_EDITOR.md) for shipped controls and validation boundaries.
Ignored per-milestone evidence lives under `out/map-editor/`. The first duplication attempt
and its loader fault are retained separately from the corrected grouped exports and
subsequent gameplay checks. No cross-game conversion is claimed by those checks.
