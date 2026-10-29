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

## Save and reopen projects

**Save project** (Command-S) writes a `.bbmapproject` outside the game folder. **Open project**
(Command-O) resumes it; **Save as** (Command-Shift-S) creates another project file. The title
row marks edits since the last project save with `*`. Project saving and mod export have
separate states: saving retains your work, while **Export mod** produces the files used by
the game. A saved project can be closed without discarding work even when its latest changes
have not been exported.

Projects retain the original map, parameter and event baselines, newly created prop/enemy
and pickup ownership, private item lots, regions, routes, NPC settings, event arguments,
pickup reward quests, identifier reservations, full grouped undo/redo history including its
redo tail, the last export state, camera and selection. Imported character packages and their
bind-pose previews are embedded, including assets needed by undone or removed imports. Undo can still return to the exact
original MSB bytes after reopening. Source previews can be opened again separately; their
large mesh caches are not embedded in projects. Terrain, object models, textures and display
labels continue to load from your game folder. If the folder moves, the open dialog offers a
replacement folder, or use:

```sh
out/map-editor/bbmap-editor --project /path/to/session.bbmapproject --game /new/path/to/CUSA00900
```

Project files contain private game baselines and are intended for local editing. Share the
exported mod when distributing changes. Saves use a bounded, checksummed format and atomic
file replacement; an unrelated file, damaged existing project, directory symlink into the
game folder, or file symlink is refused as a save destination. Loading validates history
against its embedded source before replacing the document.
Version 2 projects support imported assets and a 512 MiB file limit (256 MiB of character
assets). Existing version 1 projects still open. Character imports are exported only while
their models are used by map placements.

`bash tests/test_project_file.sh /path/to/CUSA00900` checks ownership, allocator reservations,
source-baseline independence, grouped history and redo tails, malformed records, path guards,
and real pickup/quest/event export equality. Native replay under ignored
`out/map-editor/project-validation/final/` creates a project, closes the editor, reopens it in
a fresh process, removes and restores the new pickup/quest, exercises redo and export-state
transitions, and compares all four exported files byte for byte. A subsequent gameplay check
under `out/map-editor/project-validation/playable/` exports the pickup quest from a reopened
project. Ordinary walking/action input collects three vials plus the scripted five-vial
reward; the HUD shows eight, and a private-save reload stays at eight without repeating the
award. SoulsFormats independently verifies the exported quest and preserved original
instructions. All 124 tracked original asset/seed hashes remain unchanged. This bounded
scenario verifies the reopened-project export; it does not establish arbitrary quest or
campaign compatibility.

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
   Existing export folders are refused. Save a project to retain the editing session; the editor
   confirms discarding unsaved work on quit or when changing areas. Command-Z and
   Command-Shift-Z undo and redo.
5. Add the exported folder in the port's **Mods & Cheats**, enable it, and launch/reload the area.
   Test against a copied save. The game's scripts, active display groups, animation and physics
   still determine what happens to an object after it loads.

An export contains `README.txt` and the edited
`dvdroot_ps4/map/MapStudio/mAA_BB_00_00.msb.dcx`. Item-content changes also include
`dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx`. It neither copies models/textures nor modifies
source game files. Exports into the original game tree, including through a directory symlink,
are refused. Source maps are never overwritten. Reopen an exported result by selecting a
game view containing that mod; the port's existing mod overlay can produce such a view.

For a reproducible command-line edit or an unchanged round-trip export:

```sh
out/map-editor/bbmap-editor --game /path/to/game --map m24_01_00_00 --list
out/map-editor/bbmap-editor --game /path/to/game --map m24_01_00_00 \
  --batch --select o240083_1025 --translate 3 0 0 --export /path/to/new-mod
out/map-editor/bbmap-editor --game /path/to/game --map m24_01_00_01 \
  --batch --select c2630_0000 --duplicate --translate -1 0 2 --export /path/to/new-enemy-mod
out/map-editor/bbmap-editor --game /path/to/game --map m24_01_00_00 \
  --batch --export /path/to/unchanged-mod
```

## Items and pickups

Choose **Items / pickups** and select a treasure event. The editor selects its supporting
object for the usual transform controls. The pickup inspector shows its item-lot references
and collection flag. **Duplicate pickup** or **Command-D** creates an independent object,
treasure event and private item lot, and selects the copy. Move it, choose a weapon, armor
or consumable from the installed game's parameter rows, set the count, then **Apply item contents**.
Equipment pickers display English in-game names when the message binder is available and
search names/IDs without ASCII case sensitivity. Original parameter row names remain intact.
Apply replaces the lot's contents with one guaranteed stack: one equipment item or 1-99
consumables. Existing multi-slot/random lots remain intact until explicitly replaced.

Changing an existing pickup creates a private parameter row and retains its collection flag.
Previously collected pickups remain collected in that save. Duplicates receive a fresh flag
from the source pickup's existing retail flag group. Allocation reserves values found in
the game's parameter binder and event/talk files, and the current edit session. Computed
script flag ranges and other mods still require review when combining projects.

**Remove pickup** removes a newly created pickup's object, event and unused private lot as
one undoable action. Original pickups cannot be removed. Creation, transforms, contents and
removal participate in undo/redo; parameter-only changes also mark an exported document dirty.
Ground pickup duplication supports one source item lot with a persistent collection flag.
Chest duplication and complex scripted reward chains are not implemented. A dormant
`DummyObject` used as a pickup template becomes an active `Object` in the new copy; the
original remains dormant. Ordinary prop duplication preserves its original MSB type.

Item export includes a full gameparam binder with the private rows added. Existing row
payloads, names and duplicate IDs are preserved, as are all other binder files. Combine
parameter edits before enabling another mod that replaces the same binder. Original game
files and saves are never changed by editor export.

Validation is under ignored `out/map-editor/items-validation/`. All 24 maps pass structural
pickup duplication and reference/opaque-record preservation checks, including grouped
creation/removal undo and duplication of an already edited copy. Parameter tests preserve
every original item-row payload and every other binder entry. SoulsFormats independently
reads and rewrites the native map/PARAM output and verifies the unchanged originals.

In the Dream, an exported active copy of a dormant pickup awards **Blood Vial x3** through
ordinary walking and action-button input. A two-pickup export awards three, then five vials,
and the HUD reaches eight. Reloading that private save retains eight; revisiting both pickups
does not give more vials and their loot glows remain absent. No travel or player-placement
helper runs in these collection tests. Earlier Central Yharnam test-warp sessions loaded the
edited object and parameter binder but did not establish interaction; they are retained
separately and are not counted as collection proof.

For read-only inspection without the GUI:

```sh
out/map-editor/bbmap-editor --game /path/to/game --map m21_00_00_00 --list-treasures
out/map-editor/bbmap-editor --game /path/to/game --list-param EquipParamGoods
out/map-editor/bbmap-editor --game /path/to/game --map m24_01_00_00 --list-param TalkScripts
```

## Events and simple quests

Open **Events / quests…**. The modeless window reads the area's EMEVD and common script,
event names from EMELD, and instruction argument types/names/enumerations from your game's
`common.emedf.dcx`. Filter events by ID or name, or match the selected placement's entity ID
as an integer constant. Integer matches are navigation aids, not a complete reference graph.

Select an event and instruction to inspect its arguments. Supported constants have typed
number or enumeration controls and **Apply**. Parameter-fed arguments are protected; change
their caller's input instead. Unknown commands/layouts remain preserved. New item-lot
references must exist, and changed event-call targets must be loaded and accept the available
parameter bytes. These checks validate encoding and supported references; they do not prove
that every instruction change is sensible in the game's current quest state.

To create a simple quest, select a pickup in the map, choose a reward item/count in the
**Pickup reward** panel, and click **Create pickup reward quest**. The editor adds a new area
event and constructor call. It waits for the pickup's collection flag, awards a private item
lot once, then sets an independent persistent completion flag. The quest and its unused
reward lot can be removed together during the current session. Creation/removal and typed
argument edits use the main undo/redo history and participate in export/unsaved-state checks.

Export includes changed `dvdroot_ps4/event/*.emevd.dcx` files alongside the map and any item
parameters. Existing instructions, layers, parameters, linked files and string data are
preserved. Event-only edits leave the exported MSB byte-identical and include no parameter
binder. Event files are whole-file mod replacements; combine changes when another mod edits
the same script. This is a typed event editor with a pickup-reward quest template. ESD
dialogue/state-machine authoring and a general quest graph are not implemented.

Validation under ignored `out/map-editor/events-validation/` covers synthetic fixtures and
27 real root EMEVD files: 112,648 typed constants, 12,144 protected parameter-fed fields,
byte-identical unchanged round trips, bounded edits, shared history and new-event insertion.
SoulsFormats independently verifies all original instructions, layers, parameters, strings
and links, the new constructor call and completion guard, and a rewrite of the output.
Native replay checks quest creation/removal/history/export and an isolated float argument
change whose output differs only at that argument's bytes.

The gameplay test collects a three-vial pickup through ordinary walking and action input;
the newly authored event awards five more. The reward panel shows both stacks and the HUD
reaches eight. A private-save reload retains eight without awarding the quest again. No
travel, player-placement, flag-writing or inventory-writing helper runs in these tests.
All 124 tracked original asset/seed hashes remain unchanged. This bounded test establishes
the template's execution and persistence, not campaign-wide quest compatibility.

## Enemy placement

Choose **Enemies / characters** in the list filter. Existing MSBB Enemy and DummyEnemy parts appear as
selectable capsules with a facing arrow by default. The optional source adapter can load
untextured bind-pose character geometry; animations are not previewed.
The layer includes friendly and scripted characters stored as Enemy parts. **Show objects**
and **Show enemies** control viewport visibility independently of the list filter.

Enemy position and rotation use the same inspector, axis handles, reset and undo/redo as
objects. Enemy scale is preserved. The **Enemy references** section shows read-only entity
and instance IDs, NPC/AI parameters, initial animation, loading collision, patrol event and
move-point region names. Missing references are reported with their original index.

Transform edits preserve these references, event scripts, draw/backread groups and every byte outside
the edited transform fields. Moving an enemy leaves its patrol regions in place. Its loading
collision controls streaming, and game events or saved death flags can disable it or change its
placement. Keep the first moves local, test the exact map phase, and check behavior after
reloading using a copied save. The editor does not validate walkability or rebuild navigation.

The **Character settings** section edits NPC/AI, talk, initialization and animation IDs with
undo/redo. NPC/AI and character-init pickers search the installed game's parameter IDs and
row names. Talk choices come from the area's talk ESD binder, which is separate from
TalkParam. Changed references must exist, or use the supported none value. Existing unresolved
references remain intact when editing unrelated fields. Animation IDs are numeric and require
runtime testing for model compatibility.

The inspector shows the selected NPC row's base HP, NPC type and team for the supported retail
layout. Runtime effects and events can change those values. **NPCs with talk scripts** filters
characters with a positive Talk ID. A talk assignment does not establish friendliness or a
working quest: allegiance, interaction, animations and saved quest state still need testing.
The inspector identifies each enemy's MSB type; Player parts remain untouched.

Parameter validation is under ignored `out/map-editor/params-validation/`. Native tests read
all eight supported parameter tables and the area's 32 talk scripts. All 20,735 row IDs and
Unicode names match an independent SoulsFormats catalog, including duplicate IDs present in
retail data. Native replay tests NPC settings, undo/redo and export; SoulsFormats verifies the
edited NPC/AI references and existing placements. In `modified-npc-streamed-observed`, a copied
character using NPC/AI 263000 streams with 306 runtime HP while the original remains at 150.
The selected row has 120 base HP; this test includes the game's effects and does not equate
base HP with runtime HP. It does not establish a complete friendly-NPC dialogue or quest.

## Regions and patrols

Choose **Regions / waypoints** to inspect existing region origins, or **New waypoint** to
create a point at the current selection. Region markers support viewport picking, axis
dragging and position/rotation edits. Existing region shape dimensions are preserved; the
marker represents the origin rather than the full volume. Only newly created points can be
removed, and referenced points must be unlinked first.

Choose **Patrol routes** or **New patrol**. The active patrol panel shows its ordered points
and behavior. Select a waypoint and use **Append selected waypoint**; **Up**, **Down** and
**Unlink** edit the sequence. Selecting a patrol frames its points. The viewport draws numbered
straight connections; these lines do not represent a computed navigation path.

Select a character while keeping the patrol active and choose **Assign to selected character**.
Assignment replaces its direct move points with the patrol reference and behavior; undo restores
the previous assignment and direct points. **Show assigned patrol** opens a character's current
route. All region, patrol and character changes participate in undo/redo and mod export.
Only new patrols can be removed, after unlinking their characters.

Routes use the game's existing navigation and loading collisions. Keep points on connected
walkable ground within the character's loading area. Events can override a character's route.
There is no navigation rebuild or floor snapping yet.

Route validation is recorded under ignored `out/map-editor/route-validation/`. All 24 source
MSBs pass unchanged and fully undone byte-identical round trips, plus structural exports
with a copied character, two new regions and one patrol. The tests compare every old opaque
record after normalizing affected reference indices. SoulsFormats independently reads and
rewrites the native export, retaining existing named reference targets and placements.
Native Metal replay verifies waypoint picking, an axis drag, undo/redo and route export.

In `modified-patrol-return-observed`, the copied Central Yharnam enemy follows a new reverse
route through waypoint 1, waypoint 2 and back to waypoint 1. The player stays at least
23.88 metres away with full health; the enemy retains full health. Only the player is placed
by the temporary test helper; the observer does not write enemy AI, health or transforms.
An earlier observation reached both points before another enemy killed the test player;
that run is retained separately and is not counted as full-cycle proof. The final run exits
deliberately without a guest fault or Metal validation failure. All 68 tracked source/seed
hashes remain unchanged. This bounded patrol test does not establish navigation across areas.

## Duplicate enemies and props

Select an Object, Enemy or DummyEnemy and choose **Duplicate** or press **Command-D**.
The new copy is selected at the source's current transform. Move it with the axis handles or
inspector, rotate it, and export the mod. Props also retain editable scale. **Undo** and
**Redo** cover creating, transforming and removing copies, and restore their identifiers.

**Remove copy**, **Delete** or **Backspace** removes a copy created in the current session.
Undo restores it. Existing map records cannot be deleted; after reopening an export, its
placements are all treated as existing records. Removing every copy and undoing other edits
restores a byte-identical original export.

Each copy receives a fresh model-based name, entity ID, model instance ID and part type ID.
Allocation reserves identifiers from all parts, events and regions, including area variants
beside the opened map. Copies use the source entity's 9000-9999 ID range, or the opened map's
range when the source has no entity ID. Allocation stops if that range is full. Structural
exports keep part type groups contiguous and remap affected reference indices, preserving
collision, event, patrol and other reference targets.

Copies retain the source's model, groups, loading collision, NPC/AI settings and other
opaque part data. Event scripts, patrol regions and treasure events are not duplicated.
An ordinary enemy or prop is the useful starting point: scripted doors, bosses or quest
characters may need additional event work. Keep placements within the source collision's
area and check them in-game; there is no floor snapping or navigation validation yet.

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

The editor transforms and duplicates **Object, Enemy and DummyEnemy parts**. Map pieces,
collisions, navigation, event scripts, manual ID editing and deletion of existing parts are
outside this implementation.
There is no live injection or automatic game launch. Export changes are applied on an area
reload or a new game launch.

The map document retains the original inflated MSBB and compressed source. Unchanged exports
are byte-identical. Transform-only exports patch the selected position/rotation/scale fields
and preserve every other byte. Duplication rebuilds section offset tables and adds copies with
new identifiers, names and transforms. Part records remain in contiguous type groups. Existing
record payloads retain their opaque data and relative offsets; affected model instance counts
and full-part reference indices are updated. Collision references on objects/enemies and event
references in base events, treasure, generators, object actions and platoons retain their named
targets. ConnectCollision uses collision-subtype indices, whose order is preserved.
The supported Bloodborne DFLT DCX envelope is recompressed. An export
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
recompression, duplication of props/enemies/DummyEnemies, preserved reference targets and opaque
record contents, fresh identifiers across sibling variants, undo/remove/redo, malformed input
and export protection. All 24 supplied base/DLC MSBs
passed unchanged and edited/reopened round trips for both objects and enemies. This is format evidence, not gameplay coverage
for all those areas.

`--script file` runs local editor checks after the map loads. Commands include `select name`,
`deselect`, `duplicate`, `remove`, `move dx dy dz`, `rotate dx dy dz` (degrees), `undo`, `redo`,
`export path`, `capture path` and `quit`. `assert-count n` verifies the editable placement count.
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

### Enemy and prop duplication check (2026-10-28)

The grouped writer exports two new parts in `m24_01_00_01`, increasing the full part table
from 6172 to 6174. `c2630_0000` remains at its original position; its copy `c2630_0011`
(entity 2419000, instance 1007) spawns at `(-193.014999, -44.158001, 84.727997)` with yaw
168.998 degrees. Both ordinary enemies stream in, move toward the player during combat,
and reset to their separate authored positions at 150/150 HP after the player dies.

The prop `o240027_2005` remains at `(-191.452942, -43.963432, 77.411873)`. Its copy
`o240027_0000` (entity 2419001, instance 8005) loads at
`(-193.452942, -43.963432, 78.411873)`. A temporary read-only observer confirms both the
loaded placement and world-matrix translation for each named prop. The object registry
contains 1693 objects, including the new copy. Enemy and prop state are never written by
the observer; the player test controls use the same private-save/native-travel approach
as the earlier milestone.

Native Metal replay checks the Duplicate and Remove buttons, selection, copy transforms,
undo/redo and export for enemies and props. All 24 maps pass duplication/reopen checks for
Object, Enemy and DummyEnemy types, including reference-target and opaque record checks.
Smithbox's independent SoulsFormats reader resolves the final export's part and event
references to the same names, preserves all existing placements, and rewrites it successfully.

The initial append-only writer passed a library read but faulted the retail loader. It was
replaced with contiguous part type groups and full-part reference remapping. Corrected runs
load and complete the two-enemy combat/reset sequence. The final recorded-evidence check
also verifies both prop world transforms, 68 unchanged source/seed hashes and a deliberate
test shutdown with no guest fault or Metal validation assertion. The initial summary helper
tripped over its own diagnostic rows; its raw output is retained, and the final coordinate
and encounter checks use the recorded live data. These are bounded ordinary-enemy/prop
checks, not campaign, arbitrary scripted-object or killed-enemy/rest-cycle coverage.

Evidence and the usable mod are under ignored `out/map-editor/duplication-validation/`:
`validation.json`, `grouped-format-check.log`, `modified-final-observed/` and
`mods-grouped/two-enemies-and-prop/`. Original assets and saves remain unchanged.

## Source maps and character previews

The optional importer uses the SoulsFormats library from a local Smithbox build. Install
.NET 10 and build that adapter before rebuilding the editor app:

```sh
SMITHBOX_DIR=/path/to/Smithbox bash scripts/macos/build_souls_import.sh
bash scripts/macos/build_map_editor.sh
```

The adapter reads your own game files and writes an exclusive `.bbsource` preview outside
the source folder. The native editor validates this bounded scene format, owns its meshes,
and renders/picks them in Metal. The adapter is a separate process; it does not load any
foreign game's executable or run its scripts. No source-game assets are added to this repo.

Open **Source maps…** and choose the game profile. Steam installations are detected from
local manifests, including additional Steam libraries; detected profiles are marked
**installed** and their asset folder is filled automatically. Bloodborne uses the current
game folder. **Detect installed games** refreshes discovery after another game is installed.
For other dumps, use **Choose source folder…** or type the asset folder and press Enter.

The **Source map** menu lists readable maps with Smithbox's names and unused/test labels;
filter by map ID or name. The catalog reads and parses each available MSB without extracting
archives or decoding geometry. Choose a map and click **Prepare preview**. Manual **Map ID**
entry remains available, and **Refresh maps** rebuilds the list. Catalog and preview loading
run on workers. `SoulsImport.dll maps --profile DS3 --root /path/to/Game --smithbox /path/to/Smithbox`
also prints the map ID, name and MSB part count as tab-separated columns. DS2 generator
placements are joined during preview preparation, so the catalog's MSB-only count excludes
them.

DS2/Scholar's installed `Game` folder can be read directly: the reader decrypts the BHD index using its shipped public PEM,
reads individual BDT entries, and joins generator location/registration/enemy parameters.
DS3 installed archives are also read directly using Smithbox's archive key definitions.
Other profiles currently require **loose, unpacked game files**. Compression codecs
available in your SoulsFormats build determine which compressed assets can be read.
Unsupported files and placeholders appear in the conversion report. Source previews use
flat shading and bind-pose character geometry (terrain welded on a 35 cm grid, other
models on a 2 cm grid; sky and alpha-card materials are omitted), with capsule fallbacks for missing characters.
Collision and other nonvisual parts are retained as metadata rather than drawn.

| Profile | Adapter coverage |
| --- | --- |
| Bloodborne | Real maps and character bind poses checked against local assets |
| Dark Souls II Scholar | All 28 installed maps pass preparation and native scene validation; joined generator tables and FLVER previews checked |
| Dark Souls III | Installed archives, MSB3, FLVER map/prop/character preview checked on the local Cemetery of Ash map |
| Dark Souls Remastered | All 18 installed gameplay maps pass preparation and native scene validation; Firelink Shrine checked in Metal |
| Demon's Souls, original DS1, original DS2, Sekiro, Elden Ring, AC6, Nightreign | MSB reader selection and empty-format round trips tested; real asset validation awaits local dumps |
| Duskbloods / network test | Listed with current Smithbox profiles; the installed SoulsFormats library has no reader, so preparation reports unavailable |

The profile list follows [Smithbox's project types](https://github.com/vawser/Smithbox/blob/main/src/Smithbox.Program/Editors/Project%20Editor/Enums/ProjectType.cs).
Format fixtures do not establish real-game asset compatibility. Newer game readers and
platform-specific decompression libraries may require a newer compatible Smithbox build.

Catalog validation under ignored `out/map-editor/source-catalog-validation/` parses 35
installed DS1 Remastered MSBs (18 gameplay layouts and 17 developer/test layouts), 28 DS2
Scholar MSBs, 26 DS3 MSBs including DLC/test layouts, and 24 Bloodborne MSBs. This catalog
coverage does not establish complete geometry or gameplay conversion. Native replay detects
all three installed Steam games, prepares DS1 Firelink from the detected folder, checks
DS3's Cemetery and Ringed City entries, opens the Cemetery preview, and returns to Bloodborne
without modifying the document. `tests/test_source_catalog.sh` also covers escaped and
legacy library paths, duplicate library symlinks, incomplete/unsafe manifests and bounded
map-list parsing.

Source navigation uses the same orbit/pan/zoom controls. Select a part in the list or click
its geometry. **Frame map** fits the available geometry; **Cutaway** removes geometry above
the selected placement. **Isolate selected** fits one model without surrounding scenery. **Hide this model**
removes all instances of an obstructing model from the preview; **Restore hidden** shows them again.
Select **Characters only** when preparing a Bloodborne map to load
its characters without the terrain budget. For a source Bloodborne map matching the current
map variant, **Use character models in editor** replaces available capsules in the main
viewport with untextured bind-pose models. These previews are visual aids, not animation or
hitbox previews. They do not change mod exports.

To reuse a foreign spawn/prop arrangement:

1. Select its first placement and click **Use as source anchor**.
2. Choose a Bloodborne placement of the same kind as the template. **Target current
   selection** uses the current native placement and position as the destination anchor.
3. Adjust the target anchor and yaw offset. Select source placements and click
   **Create Bloodborne placement** to transfer their positions relative to those anchors.
4. The created copy is selected in the main editor and uses the normal transform,
   undo/redo, remove and export workflow. One Undo removes the entire transferred copy.

Transfer preserves the chosen Bloodborne template's model, AI, parameter, scale and
collision references; source positions and rotations supply the layout. It never treats
foreign IDs as Bloodborne IDs. Original source data remains unchanged. This button performs
**layout transfer**. Character appearance conversion uses the separate workflow below;
preparing a scene or transferring a layout does not convert foreign animations, AI,
collision, navigation, item tables or event/quest logic.

### Import a character appearance

Select a source character and an original Bloodborne character as the native template.
You can also enter a **Character ID** and click **Preview character** to load a model directly
from the chosen game folder, without finding a map that places it. Some humanoid models
assemble separate equipment parts and contain no mesh; those are reported as unavailable.
Set the source and target anchors as above, choose the source's **Render masks** (for example
`0` or `0,1`), then click **Import character as new model**. Conversion runs on a worker.
It matches named bones, preserves bind-pose offsets when falling back to an ancestor,
retargets geometry, maps material slots to a native shader and converts DDS images to PS4
textures. The report records bone mappings, direct weight coverage and texture fallbacks;
missing diffuse textures and incompatible skeletons fail without adding a placement.

The result is a separate character model with a fresh placement/entity ID. Original
Bloodborne characters keep their original mesh. The editor shows the converted bind pose;
move, rotate, duplicate, remove, undo, save/reopen a project and export it normally. The
mod contains the new mesh/embedded texture bank, copied native animation/behavior binder,
native action script and edited MSB. Native behavior paths are renamed without relocating
their Havok fixups, and model-table insertion remaps existing model references.

This imports **appearance onto a native rig**. Animations, AI, hitboxes and effects use the
chosen Bloodborne template. Cloth bindings for the old vertex buffers are removed, so cloth
uses ordinary skinning. Source-specific shaders are approximated by the native shader;
sound banks are not copied. Foreign animation/behavior conversion remains separate work.
The experimental map replacements below use a separate CLI workflow.

The managed converter can also produce an independent package:

```sh
dotnet out/map-editor/souls-import/SoulsImport.dll bake-skin \
  --profile DS3 --root /path/to/DS3/Game --model c1430 \
  --native-root /path/to/CUSA00900 --template c2630 --target c6900 \
  --mask 0 --output /path/to/new-character-package
```

Omitting `--target` creates a replacement-skin package for the native template; the editor
always allocates a separate unused model. CLI output folders are exclusive and must be
outside both game folders, including through symlinks. Game assets remain local and are
never added to the repository.

`SMITHBOX_DIR=... bash tests/test_skin_retarget.sh` checks bone palettes/global indices,
bind transforms and ancestor offsets, masks, image payload preservation, legacy texture
slots, malformed imports, behavior path renaming and source/output guards.
Native replay and gameplay evidence are under ignored `out/map-editor/asset-import-validation/`.
The DS3 Cemetery hollow (`c1430`, mask 0) imported as `c6900` beside original `c2630` in active
Central Yharnam phase `m24_01_00_01`: both entities had 150 HP, active loading state and moved
over five metres. Screenshots show the textured hollow and original enemy together using
native attack animations. The game check used a copied save and player-only placement;
no enemy memory was changed. Project reload produced six identical exported files, and
126 distinct original asset/seed hashes stayed unchanged. This is bounded coexistence and
attack evidence; it does not establish every source character or campaign compatibility.
The same native workflow also imports DS1 Remastered's Butcher (`c2660` → `c6901`,
100% direct bone weight) and DS2 Scholar's Kobold (`c1010` → `c6902`, 79.2%). Each loads
with 150 HP beside the original character and runs native attack behavior in a separate
private-save test. A project containing all three imports reopens and exports identical
assets; removing unused characters omits their asset packages from that mod.

### Majula as a Bloodborne mod

The optional managed adapter can convert DS2 Scholar's Majula (`m10_04_00_00`) into a
static replacement for Central Yharnam's `m24_01_00_01` variant. This produces a normal
mod folder; it runs in Bloodborne through the existing mod loader. It is currently a
CLI export, separate from the editor's placement-transfer and project-export controls.

```sh
SMITHBOX_DIR=/path/to/built/Smithbox bash scripts/macos/build_souls_import.sh
dotnet out/map-editor/souls-import/SoulsImport.dll bake-map \
  --profile DS2S --root '/path/to/Dark Souls II Scholar of the First Sin/Game' \
  --map m10_04_00_00 --native-root /path/to/CUSA00900 \
  --target-map m24_01_00_01 \
  --source-anchor '9,6,-16' --target-anchor '-195.6,-40.53,220.33' \
  --output /path/to/private-test-data/mods/Majula
```

Choose a new output directory outside both game installations. `BB_MODS_DIR` points to
its parent `mods` directory. Use a separate `BB_SAVE_DIR` containing a copied test save;
this replaces a campaign area and is not a campaign-compatible installation. Existing
saved positions in Yharnam may be over empty space after enabling the replacement.
The edited native player/spawn regions place a subsequent respawn near the Majula hub.
The converted lantern's interaction and travel from the Hunter's Dream are not verified.

The local export contains 143 static models, 524 placements and 227 converted textures.
Geometry uses native Bloodborne FLVER/material layouts, source texture tiling, converted
DS2 alpha/green normal channels, and native fallback surface textures. Source skies,
alpha cards and unsupported shaders are omitted or approximated; Bloodborne's sky and
lighting remain. Multi-layer terrain uses its first texture layer. Distant scenery,
water, lighting and material fidelity still need work.

Collision is rebuilt from opaque render triangles and divided among 99 distinct native
Havok bodies. Its BVHs use native fixups, shape keys, section codecs and the disabled
edge-welding sentinel. This supports a bounded walk on the hub terrain; it does not
establish complete traversability, correct physical materials or navigation. DS2 enemies,
NPCs, items, quests, scripts, audio and animated/breakable object behavior are not imported.

Unused Yharnam scenery is removed while referenced/scripted pieces and sky records remain.
Retained scenery and props are moved outside the play area. Original enemies become dummy
records retaining their names and IDs, preventing the old population from falling through
the replacement level and awarding echoes in the tested runs. Native campaign scripts
remain, so extended play and quest compatibility are not established.

`SMITHBOX_DIR=... bash tests/test_map_import.sh /path/to/CUSA00900` checks the independent
Smithbox collision codec, multi-section trees, welding sentinel, shape/body references,
scenery-reference preservation, parked actors, normal channels and mip preservation.
Local gameplay evidence is under ignored `out/map-editor/majula-validation/`: visible
Majula terrain/buildings, a 4.6-metre walk with unchanged HP, and native respawn onto the
hub without player-placement or travel requests. Original asset/seed hash checks cover
26 files. These are bounded checks, not a finished map port.

Imported texture loading also exposed a staging-buffer alignment error. Tile/detile
allocations now request shader storage alignment explicitly; the Metal host-kernel suite
and the mod run pass with Metal validation enabled.

### Firelink Shrine as a Bloodborne mod

`bake-map` also accepts `DS1` and `DS1R` for Firelink Shrine (`m10_02_00_00`), replacing
the same Central Yharnam variant. The installed source asset checks use **Dark Souls
Remastered**; original Dark Souls is an adapter profile, not a separately verified game run.
After building the optional adapter as above:

```sh
dotnet out/map-editor/souls-import/SoulsImport.dll bake-map \
  --profile DS1R --root '/path/to/DARK SOULS REMASTERED' \
  --map m10_02_00_00 --native-root /path/to/CUSA00900 \
  --target-map m24_01_00_01 \
  --source-anchor '-51.8,-60.6,55.7' --target-anchor '-195.6,-40.53,220.33' \
  --output /path/to/private-test-data/firelink-mods/Firelink
```

Point `BB_MODS_DIR` at `firelink-mods` and use an isolated copied save through
`BB_SAVE_DIR`. Firelink and Majula replace the same native area; enable one replacement
at a time. The source anchor is Firelink's bonfire. By default, the exported native player
and respawn regions use the source map's first player position, translated by the anchor
offset and raised one metre. `--spawn-offset 'x,y,z'` overrides that position relative to
the target anchor. Existing saves keep their saved coordinates until a native respawn.

DS1 rigid bone palettes and legacy texture slot names are converted to native Bloodborne
geometry/materials. Foliage and edge materials use the native alpha-test material while preserving diffuse
alpha. Only foliage cards are excluded from generated collision; opaque trunks and
physical alpha-tested props retain collision. The two source light-shaft meshes use a
verified native additive material with original texture and vertex alpha, and have no
collision. Source angular fading and texture scrolling are not reproduced.

The export adds an independent Bloodborne daytime sky, keeps the original scripted sky
records invisible, and removes unused distant city geometry. This approximates the
backdrop; DS1's sky, distant scenery, water surfaces and lighting are not transplanted.

This remains a static map replacement with the same NPC, item, quest, navigation and
campaign limitations as Majula. Local Firelink evidence under ignored
`out/map-editor/firelink-validation/` covers 72 models, 117 placements, 160 textures,
93 non-solid meshes (91 foliage and two light shafts), and 376,245 collision triangles
in both native collision archives. Independent readback checks native references, all
texture slots, preserved source ray alpha/mips, and original scripted sky identities.

A normal Hunter's Mark respawn landed on the imported floor with full HP, and the copied
save resumed above the floor with the production runtime. Native save-and-quit and later
reload were checked. The user confirmed foliage/sky and restored additive rays during
play. This is a bounded gameplay check; collision is still derived from render geometry
and is not yet equivalent to the source game's dedicated collision.

The DS2 regression also exposed a preexisting vertex-copy issue: SoulsFormats' vertex
copy omits `NormalW`, so rigid source bone selection now reads the original vertex. This
corrects 5,710 positions across nine Majula models; 27 triangles move across the existing
collision clipping bounds. Model/placement/texture counts, base triangle indices, the MSB
and texture archives remain unchanged. The previous local Majula demo is preserved.

The importer is also available from the command line:

```sh
dotnet out/map-editor/souls-import/SoulsImport.dll profiles
dotnet out/map-editor/souls-import/SoulsImport.dll import \
  --profile DS2S --root /path/to/DS2Scholar/Game --map m10_02_00_00 \
  --smithbox /path/to/Smithbox --output /path/to/new-preview.bbsource
out/map-editor/bbmap-editor --game /path/to/CUSA00900 --source /path/to/new-preview.bbsource
```

Checks: `tests/test_source_scene.sh` exercises malformed/truncated files and bounded geometry.
It optionally accepts a real `.bbsource` file and a Bloodborne `.msb.dcx` to verify native
placement transfer, yaw/anchor transforms, grouped history and exported readback.
`SMITHBOX_DIR=... bash tests/test_souls_import.sh` also builds the managed adapter and runs
its format fixtures. These tests do not replace gameplay checks.

An isolated runtime check loaded a DS2 generator layout transferred to a Bloodborne template: both
original entity 2410015 and new entity 2419000 were observed at their exported positions with
150 HP. The test used native travel to the Central Yharnam lamp and read-only enemy observers.
This proves the native placement export loaded; it does not prove foreign enemy asset conversion.

## Collision floors and snapping

The editor loads the selected variant's static collision placements and its area's high-detail
Havok archives, including patch entries. **Show collision floors** draws their floor/ramp
surfaces in green; turn off **Show terrain** to inspect them clearly. The display mesh is
thinned, while snapping queries the original triangles through a spatial index.

Select a prop, enemy/NPC, pickup object or waypoint and click **Snap down to floor**. It
chooses the highest loaded collision surface below that placement, within **Max drop**.
**Pivot offset** places the object's origin that distance above the floor. Position X/Z,
rotation, scale and parameter/event references stay unchanged. One Undo restores the old
height; no surface in range leaves the placement unchanged.

Snapping uses static collision surfaces with slopes within about 63 degrees of horizontal.
It does not test navigation connectivity, rebuild collision, account for moving/animated
object collision, or guarantee that a character can walk to that point. Unavailable collision
models are reported in the inspector; snapping then queries the loaded subset. Match the
object's pivot and the game's contact margin with **Pivot offset**, and test the exported mod.

`bash tests/test_collision_scene.sh /path/to/CUSA00900` covers stacked floors, ramps, edges,
range/offset limits, malformed input, the spatial index and real archive/placement decoding.
An optional second argument chooses another map variant. All 24 local Bloodborne MSBs pass
loading/query checks; maps with unavailable models retain that diagnostic.

In **Source maps**, **Far scenery** controls the shared map renderer's numeric backdrop
ranges. They are hidden by default to make the playable area easier to frame. Alphanumeric
model identifiers are left visible. This display filter does not change source data or exports.

A copied enemy snapped to static collision was exported and loaded in the game on an isolated
save. Original and copy remained at 150 HP and moved under game AI during a nine-second live
observation. The test used native lamp travel and player-only placement to bring them within loading
range; it never wrote enemy transforms, health or AI.
