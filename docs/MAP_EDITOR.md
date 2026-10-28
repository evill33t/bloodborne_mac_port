# Native Metal map editor

The standalone editor opens an existing Bloodborne map from your own game files, renders its
terrain and available object models in Metal, edits existing object and enemy placements, and exports a
loose-file mod for the port. It runs as a native arm64 macOS application without starting the
game or loading the x86-64 runtime.

## Build and open

With the repository's arm64 dependencies and ImGui submodule initialized:

```sh
bash scripts/macos/build_map_editor.sh
open "out/map-editor/Bloodborne Map Editor.app"
```

Choose the game folder containing `eboot.bin` and `dvdroot_ps4`. Alternatively:

```sh
out/map-editor/bbmap-editor --game /path/to/CUSA00900 --map m24_01_00_00
```

The ordinary GPU CMake project also has `bbmap-editor` and `map-editor-test` targets on macOS.
Asset loading happens on a worker; the editor shows progress while reading terrain and models.

## Edit and export

1. Choose the **map variant**. Game phases can use different MSBs: Central Yharnam has
   `m24_01_00_00`, `m24_01_00_01` and `m24_01_00_11`, for example. The edited/exported filename
   must match the active placement used by your test save. An area can load several variants
   together; a file being opened does not prove that every placement in it is active.
   The viewport uses the selected variant's
   placements and the area's shared terrain models. Filter the placement list by type, part or model
   name, or click an object's geometry or an enemy marker.
2. Drag an X/Y/Z handle to move it. The inspector also accepts position in metres, rotation in
   degrees, and object scale; click **Apply transform** after typing. **Reset** restores that placement's
   original transform. Undo/redo covers both inspector changes and complete handle drags.
3. Right-drag to orbit, middle-drag to pan, and use the wheel to zoom. **F** frames the selection.
   Selecting an object enables a roof cutaway at its height; adjust the ceiling or turn the
   cutaway off in the inspector.
4. Click **Export mod…** (Command-Shift-E) and choose a **new folder outside the game folder**.
   Existing export folders are refused. The editor confirms discarding unexported changes on
   quit or when changing areas. Command-Z and Command-Shift-Z undo and redo.
5. Add the exported folder in the port's **Mods & Cheats**, enable it, and launch/reload the area.
   Test against a copied save. The game's scripts, active display groups, animation and physics
   still determine what happens to an object after it loads.

An export contains `README.txt` and only the edited
`dvdroot_ps4/map/MapStudio/mAA_BB_00_00.msb.dcx`. It neither copies models/textures nor modifies
source game files. Exports into the original game tree, including through a directory symlink,
are refused. Source maps are never overwritten. Reopen an exported result by selecting a
game view containing that mod; the port's existing mod overlay can produce such a view.

For a reproducible command-line edit or an unchanged round-trip export:

```sh
out/map-editor/bbmap-editor --game /path/to/game --map m24_01_00_00 --list
out/map-editor/bbmap-editor --game /path/to/game --map m24_01_00_00 \
  --batch --select o240083_1025 --translate 3 0 0 --export /path/to/new-mod
out/map-editor/bbmap-editor --game /path/to/game --map m24_01_00_00 \
  --batch --export /path/to/unchanged-mod
```

## Enemy placement

Choose **Enemies / characters** in the list filter. Existing MSBB Enemy and DummyEnemy parts appear as
selectable capsules with a facing arrow; character geometry and animations are not loaded.
The layer includes friendly and scripted characters stored as Enemy parts. **Show objects**
and **Show enemies** control viewport visibility independently of the list filter.

Enemy position and rotation use the same inspector, axis handles, reset and undo/redo as
objects. Enemy scale is preserved. The **Enemy references** section shows read-only entity
and instance IDs, NPC/AI parameters, initial animation, loading collision, patrol event and
move-point region names. Missing references are reported with their original index.

Exports preserve these references, event scripts, draw/backread groups and every byte outside
the edited transform fields. Moving an enemy leaves its patrol regions in place. Its loading
collision controls streaming, and game events or saved death flags can disable it or change its
placement. Keep the first moves local, test the exact map phase, and check behavior after
reloading using a copied save. The editor does not validate walkability or rebuild navigation.

This milestone does not insert, duplicate or delete enemies, edit NPC/AI parameters, move
patrol regions or modify scripted encounters. The inspector identifies each enemy's MSB type;
Player parts remain untouched.

## Implementation and boundaries

The editor shares `BbMapArt`'s MSB/FLVER/TPF/BC1/BC7 decoding, object transforms, terrain
generation and mesh representation with `dev`'s in-game map. Its native Metal viewport uses
the same orthographic orbit convention, with a depth buffer, a separate integer attachment
for visible-surface picking, selection shading and a height cutaway. It uses retained Metal
command buffers; resources remain alive while submitted frames use them.

The viewport is an inspection view: terrain is simplified and textured colors are baked into
vertices. Object models use their first FLVER state, and unavailable models are reported.
It does not reproduce retail material shaders, object animations, active draw groups, or
game physics. Central Yharnam currently has 35 object model identifiers this loader cannot
display. These parts remain editable by their names, with an explicit missing-model message.

The editor edits **existing Object, Enemy and DummyEnemy parts**. Map pieces, collisions,
navigation, event scripts, IDs and placement creation/deletion are outside this implementation.
There is no live injection or automatic game launch. Export changes are applied on an area
reload or a new game launch.

The map document retains the original inflated MSBB and compressed source. Unchanged exports
are byte-identical. Changed exports patch only the selected position/rotation/scale fields,
preserve every other byte, and recompress the supported Bloodborne DFLT DCX envelope. An export
is reopened and compared before writing. Unexpected headers, bounds, cycles, duplicate names,
invalid model references and non-finite transforms are rejected.

The record layout was checked against
[Smithbox/SoulsFormats MSBB PartsParam](https://github.com/vawser/Smithbox/blob/fbf7e18dfe5c423da50fd9edc5f6affe8294d851/src/Andre/SoulsFormats/SoulsFormats/Formats/MSB/MSBB/PartsParam.cs).
Smithbox's repository carries its [MIT notice](https://github.com/vawser/Smithbox/blob/fbf7e18dfe5c423da50fd9edc5f6affe8294d851/LICENSE).
The existing port/reference asset readers keep their original provenance and GPL notices.
No C# runtime, Smithbox UI/backend, game geometry or textures are bundled with the editor.

## Checks

```sh
bash tests/test_map_editor.sh
# Optional: also verify real maps from your own dump.
bash tests/test_map_editor.sh /path/to/game/dvdroot_ps4/map/MapStudio/*.msb.dcx
bash tests/test_map.sh arm64
```

Document fixtures cover byte preservation, object/enemy transform-only edits, enemy scale
protection, collision/patrol/region resolution, unresolved references, UTF-16 names,
recompression, undo/redo, malformed input and export protection. All 24 supplied base/DLC MSBs
passed unchanged and edited/reopened round trips for both objects and enemies. This is format evidence, not gameplay coverage
for all those areas.

`--script file` runs local editor checks after the map loads. Commands include `select name`,
`deselect`, `move dx dy dz`, `rotate dx dy dz` (degrees), `undo`, `redo`, `export path`, `capture path` and `quit`.
`click x y` and `drag x y end-x end-y` replay input in the editor's own logical coordinates;
`assert-selected name`, `assert-position x y z tolerance` and `assert-rotation x y z tolerance`
verify the resulting document.
The viewport input test exercised real Metal picking, a Y handle drag, undo and redo. Metal
API validation was enabled during real-asset captures. Test artifacts and exported mods live
under the worktree's ignored `out/map-editor/validation/` directory.

### In-game check (2026-10-28)

The editor exported a 3-metre +X move of `o240080_1020` in `m24_01_00_01`. An offline game
loaded the export through the existing loose-file mod overlay. File-open tracing identified
that exact phase file; changing only `m24_01_00_00` had left this save's active layout unchanged.

A temporary read-only observer in an isolated test runtime compared the named object's loaded
MSB position and world-matrix translation against an unmodified control using the same frozen
runtime binaries. X changed from -200.476074 to -197.476074; Y (-40.5836182) and Z (220.234894)
were unchanged. The observer only read game memory and is absent from the editor and normal
runtime. The compressed export reopened successfully; its inflated MSB differed at one byte
inside that object's X field, with every other byte preserved.

Metal API validation was enabled, with no validation assertion or GPU fault in the successful
phase test. Original assets and seed saves matched all 42 tracked hashes after testing. Tests
used private save copies and were stopped after evidence capture. This verifies the bounded
clinic placement/export/reload workflow, not every object type, area, time phase or game script.
Evidence is in `out/map-editor/validation/validation.json`, the per-session logs/coordinate
records and native screenshots. The usable single-file mod is `mods-phase/phase-01/` there.

### Enemy milestone check (2026-10-28)

The final enemy export moves Central Yharnam's `c2630_0000` (entity `2410015`) one metre in -X
and rotates it 30 degrees in `m24_01_00_01`. Its MSB position changes from
`(-192.014999, -44.158001, 82.727997)` to `(-193.014999, -44.158001, 82.727997)`;
yaw changes from 138.998 to 168.998 degrees. Exactly two inflated bytes change, within that
part's position and rotation fields. All other map bytes, including AI/NPC parameters,
loading collision and references, remain identical.

A live offline game loaded the exported file and reported that named entity at the edited
position. The enemy streamed in, pursued the disposable test character, attacked it and killed
it; the game then completed the player's transition to the Hunter's Dream. These checks use
private save copies and a temporary source-based test runtime: its observer reads character
state, and its player controller uses the game's existing placement/travel/checkpoint
sequence. It never writes enemy positions, HP, AI or events. These helpers are absent from
the editor and the normal runtime.

The final automated check loaded a copy of that post-death save, requested the game's native
travel back to Yharnam, and verified the entity at the edited position with heading 2.94957161
radians (168.998 degrees). The test selected the clinic respawn point using the same player
checkpoint sequence as the port's autoplay rig. After the enemy killed the player again, the
player respawned at that checkpoint and the enemy reset to the edited position and heading at
150/150 HP. Enemy state was only observed; no enemy was moved, damaged or revived by the test
helper. This verifies reset after player death, not a killed-enemy/rest cycle or a full campaign.
The final check completed successfully and the test process was deliberately stopped.

The clinic beast's observed placement did not follow its edited MSB transform in the initial
tests. Scripted placement is a possible explanation, not an established diagnosis. Likewise,
testing a single NPC does not validate arbitrary moves across collision or navigation areas.
An extended test reached the runtime watchdog and subsequently produced a guest fault during
the diagnostic dump; this work does not establish long-session runtime stability.

The final mod and evidence live in the ignored `out/map-editor/enemy-validation/` directory:
`mods-normal-safe/yharnam-enemy/`, `final-export-check.json`, `validation.json`, native editor
captures and per-session character-state logs. Viewport replay verifies real Metal enemy
picking, handle dragging, position/rotation undo and redo. Format tests cover Object, Enemy
and DummyEnemy transforms across all 24 available base/DLC maps; they are distinct from the
bounded gameplay checks above.
