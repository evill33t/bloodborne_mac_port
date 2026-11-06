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
- Griggs: Residence Key door, rescue conversation and subsequent Firelink
  arrival, with source face, outfit and voices. The Residence door requires
  its actual key; the source does not accept the Master Key there.
- Andre: source appearance and forge idle, first conversations and a real
  one-stock Crest of Artorias purchase for 20,000 Blood Echoes. The Garden
  entrance requires this Crest; Sif's approach gate opens without it. Both
  gates retain two moving leaves, source timing and solid collision.
- Parish bell and Oswald: an actual bell-lever interaction sets the upper
  bell state; Oswald's source appearance and conversation become available
  afterward. Killing the Gargoyles alone does not ring the bell.
- Thorolund party: Rhea, Vince and Nico arrive after Capra's defeat. The
  source farewell and area reload govern departure, Petrus's absence and
  subsequent return. Existing death states stay absent. All four have source
  faces, outfits and voices; Rhea retains her kneeling pose.
- Catacombs: 63 skeleton/giant/necromancer/bonewheel actors with source AI,
  linked collapse/reassembly controllers; 12 Wisps,
  Titanite Demon, axe Black Knight and two Crystal Lizards with source
  request/lifecycle/reward handling. Six core character packages now carry
  converted source bodies, joint constraints and animation/ragdoll mappings.
- Pinwheel: source boss and fourteen stateful illusion helpers, encounter fog,
  authored entry regions, AI attacks, teleport/summoning controllers, persistent
  defeat, 15,000 Blood Echoes and the Rite of Kindling. Separate saved reward
  phases preserve the immediate quest item and five-second consumable delay.
  Native target socket220 follows the source marker instead of the giant
  carrier's head. The seven body/cape meshes retain source specular colour,
  roughness and alpha-tested edges. Six mask meshes convert packed metallic,
  reflectance and roughness channels, with the source tint applied before gamma.
  Uniform inner-face vertex tint is preserved; near-white outer-face variation
  is bounded to 0.87% linear colour error. Native highlight shape and incident
  lighting remain approximations. Catacombs ladders retain ten authored endpoint
  sets; the exit ladder is registered only after Pinwheel's defeat.
- Andre's finite Crest stock uses a separate persistent counter reservation.
  The earlier counter overlapped gate and Thorolund Boolean flags, so quest
  progress could make the shop appear sold out. Existing gate and NPC flag
  identities stay unchanged; an already-owned Crest remains capped at one.
- Catacombs environment: separate indoor/outdoor fog, source material gains,
  animated insect overlays through native texture scrolling. Explicit texture
  references between source object archives are resolved with bounded traversal,
  including shared statue textures. The exact Catacombs alpha backdrop retains
  its source visibility groups. Pinwheel's arena uses its own losslessly copied
  specular capture without changing lighting in other rooms.
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

Copied-save native combat confirms skeleton lock-on and damage, collapse at
the immortal 1-HP clamp, healing and protection release while its necromancer
lives. Killing another cohort's necromancer disables that cohort's reassembly;
its linked skeleton then dies normally. The integer-HP gate uses equality to
avoid the verified native comparison that entered collapse at full health.
These bounded checks do not establish every loadout or final ragdoll behavior.
Copied-save Pinwheel checks cover genuine fog admission and arena entry, normal
weapon damage, teleportation, low-health illusions, defeat, the three converted
rewards, exit-ladder climbing and persistence after area travel. This is a
bounded smoke, not campaign or every-attack validation. Pinwheel's source
cutscene, music and original effects remain unported. The imported Rite of
Kindling is an inventory quest item; lamp kindling is not implemented.

Pinwheel's authored specular capture is retained, but source diffuse GI and
DS1 single-texture baked lightmaps remain unconverted. Bloodborne's two-texture
RNM DOL layout is not treated as an interchangeable lightmap. Warm candle-lit
room matching remains incomplete; no global exposure or invented roof hides
this limitation.
Network/Gravelord-only populations and dummy placements are not fresh enemies.
Later Dusk/Patches services, hostile NPC combat and full quest chains remain
incomplete. Patches' equipment-driven armed idle is approximated by a verified
source unarmed idle. Sorcery/miracle services, Andre's forging and equipment
stock, Oswald's absolution/covenant services, the Thorolund Tomb rescue and later
quest stages remain unsupported. Source per-visit dialogue flags are retained;
their common-world reset is not guessed. The importer does not claim campaign
completion.

## Focused checks

The runners use `SMITHBOX_DIR` and accept the source DS1R folder followed by
the native Bloodborne `dvdroot_ps4` folder. Optional output paths are documented
by each runner's arguments.

The Catacombs creature regression also reads the supported decrypted native
executable to verify the integer-HP predicate's actual comparison order. Set
`BB_NATIVE_ELF` to that ELF; its default is the existing ignored QA extraction.
The equality gate is intentional: the native integer comparison differs from
the HP-ratio and NPC-part predicates used by boss phases and head cuts.

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
bash tests/test_additional_doors.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_griggs_quest.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_andre_quest.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_crest_gate.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_parish_bell.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_oswald_quest.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_thorolund_quest.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_catacombs_environment.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_catacombs_backdrop.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_pinwheel_environment.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_catacombs_creatures.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_catacombs_creatures_physics.sh "$DS1R_ROOT" "$BB_ASSETS" "$STAGED_MOD_ROOT"
bash tests/test_catacombs_special.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_catacombs_bridges.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_patches_quest.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_pinwheel.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_catacombs_ladders.sh "$DS1R_ROOT" "$BB_ASSETS"
bash tests/test_lua50.sh "$DS1R_ROOT" "$BB_ASSETS"
```

Native acceptance includes visual water/fog comparison, attacks and head cuts,
golem defeat and rescue, key collection and door interaction from both sides,
necromancer-linked reassembly and permanent deaths, insect motion, bridge
collision, dialogue interruption and save/reload persistence. Keep each live
result separate from fixture/build evidence.
