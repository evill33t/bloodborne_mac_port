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
selectable capsules with a facing arrow; character geometry and animations are not loaded.
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
