# Souls map importer checkpoint

This experimental branch imports locally installed game data into additional
Bloodborne maps. Keep it isolated from `dev`. Game assets, generated packages,
test saves and private machine paths belong in ignored output folders.

Build the helper with a built Smithbox checkout:

```sh
SMITHBOX_DIR=/path/to/Smithbox bash scripts/macos/build_souls_import.sh
python3 scripts/import_souls_map.py --help
```

Export to a new output directory. The importer rejects output inside either
source game folder. Connected exports compose the previous area's parameters,
materials, textures, effects, scripts and environment maps before adding the
next area. Do not rebuild the shared helper during an export. Use copied saves
for native tests; test-only observer/placement controls are separate artifacts.

## Current additions

- Darkroot Basin: original reflective lake/river meshes and rigid transforms,
  source water normals and three scrolling wave layers through a native water
  shader with the native lake's reflection response; lossless source HDR capture;
  authored reflection-camera visibility and offset reflection-plane heights;
  private source-derived fog and authored ground/foliage diffuse gains.
- Basin Hydra: complete source visual skeleton and animated contacts,
  source attack/projectile parameters and AI, seven persistent severed-head
  states and detached-head packages, anchored native physics and defeat reward.
- Dusk: source golden golem package and first rescue encounter, real NPC face
  and outfit, source rescue poses/voices, interruptible dialogue and choice.
- Havel: upper/lower keyed doors, source moving leaves and collision,
  persistent open state and converted Watchtower Basement/Master Key checks.
- Catacombs: 63 skeleton/giant/necromancer/bonewheel actors with source AI,
  collapse/reassembly while their linked necromancer survives; 12 Wisps,
  Titanite Demon, axe Black Knight and two Crystal Lizards with source
  request/lifecycle/reward handling.
- Catacombs environment: separate indoor/outdoor fog, source material gains,
  animated insect overlays through native texture scrolling. Explicit texture
  references between source object archives are resolved with bounded traversal,
  including shared statue textures.
- Patches: source bridge/lever geometry and keyframed collision, trap and manual
  controls, saved orientation, real NPC face/outfit, first meeting and
  forgiveness dialogue, one-time Humanity gift converted to Madman's Knowledge.

Each stage exports its supported behavior and limitations in the import report.
Source contracts and original archive hashes are checked by the fixture runners.
Fixture success establishes resource/event structure, not native gameplay.

## Explicit remaining boundaries

Water uses Bloodborne's material/vertex controls. Source shoreline vertex alpha,
Fresnel curve, wave amplitude, depth absorption, waterfalls and interaction SFX
are not reproduced exactly. The HDR cube is lossless, but separate source diffuse
capture/tints and independent diffuse/specular environment powers remain unmapped.
Source exponential/linear fog equivalence is limited to the chosen midpoint.

Native locomotion, controller routing, cloth and some physical anchors remain
approximations. Final Catacombs enemy ragdolls are not fixed by the animated
temporary collapse adapter. Native effects replace incompatible source FXR1
resources, and some sounds/transient effects remain absent. Titanite Demon's
paired impale currently uses an ordinary source strike.

Pinwheel and his 14 illusion helpers remain disabled: their source requests,
regions, flags, relocation and phased spawning require an encounter adapter.
Network/Gravelord-only populations and dummy placements are not fresh enemies.
Later Dusk/Patches services, hostile NPC combat and full quest chains remain
incomplete. Patches' equipment-driven armed idle is approximated by a verified
source unarmed idle. The importer does not claim campaign completion.

## Focused checks

The runners use `SMITHBOX_DIR` and accept the source DS1R folder followed by
the native Bloodborne `dvdroot_ps4` folder. Optional output paths are documented
by each runner's arguments.

```sh
bash tests/test_basin_water.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_basin_environment.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_source_reflections.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_source_object_textures.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_hydra_characters.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_hydra_heads.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_hydra_actions.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_hydra_events.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_golden_golem.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_dusk_quest.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_map_doors.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_catacombs_environment.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_catacombs_creatures.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_catacombs_special.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_catacombs_bridges.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_patches_quest.sh "$DS1R_ROOT" "$BB_ASSETS"
```

Native acceptance includes visual water/fog comparison, attacks and head cuts,
golem defeat and rescue, key collection and door interaction from both sides,
necromancer-linked reassembly and permanent deaths, insect motion, bridge
collision, dialogue interruption and save/reload persistence. Keep each live
result separate from fixture/build evidence.
