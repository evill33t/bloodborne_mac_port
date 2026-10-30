# Souls map import: verified format and environment notes

These notes describe the DS1 Remastered (`DS1R`) to Bloodborne adapter in
[MapEnvironment.cs](../editor/souls_import/MapEnvironment.cs) and
[SourceFlver.cs](../editor/souls_import/SourceFlver.cs). They distinguish numeric
conversion proved from installed artifacts from rendering that still uses a
native approximation. They do not establish identical rendering across games.
Original game files are read only; repairs and native parameter additions operate
on private copies written into the exported mod.

## Artifact identities and address conventions

The observations below apply to these exact artifacts. A different executable or
shader requires checking the formulas again rather than reusing address labels.
DS1R addresses are PE virtual addresses, with image base `0x140000000`.
Bloodborne addresses are guest virtual addresses in the decrypted ELF; for these
functions the file offset is the guest address plus `0x4000`. Shader offsets are
byte offsets within the instruction stream, not offsets into a shader binder.

| Artifact | SHA-256 |
| --- | --- |
| DS1R `DarkSoulsRemastered.exe` (50,286,344 bytes) | `a45aaa36dd2f6cc151670a639ea5547043cf38ea79ff4178b963c6ed71f98d7b` |
| Bloodborne decrypted `eboot.elf` (93,667,640 bytes) | `cec1b276e7f9e4db978e57f524f41fbaac594530a3437b002e23f3fab14b4f86` |
| DS1R `FRPG_Deferred_DX11.shaderbnd.dcx` entry `FRPG_Deferred_LightProbe.fpo` | `bca77240941a7561e6b2f241a17232c7536e9cf4c7640a25c4b46b641143dcb5` |
| Bloodborne `gxflvershader.shaderbnd.dcx` entry `GXFlver_Col_Fwd.ppo` | `b3b34eba44f0b6c40a0c9384c3c446476faf00756bbe2ec5452d42fba673c6e4` |
| Bloodborne same binder, entry `GXFlver_Col_Fwd.vpo` | `962a0decfd057304eac5dd521b7384312e1582b2b77863a2ac1ab4821a984bf8` |
| Bloodborne `gxrenderershader.shaderbnd.dcx` entry `Filter/GXLightComposite.ppo` | `07be7f12f94acd7edd3fb04fa4d16c265f139a8c26fb00adac51c575d198ab0e` |
| DS1R `map/m10_02_00_00/m9000B2A10.flver.dcx` (29,687 bytes) | `4d656557471ff399601353930fff8e538b8065d2e9a3434600bb5f41a091fd45` |

The shader hashes cover extracted entry bytes. Extract using SoulsFormats
DCX/BND3/BND4 readers. DS1R DXBC operands and constant-buffer names can be decoded
using Microsoft's [tokenized shader format](https://github.com/microsoft/DirectXShaderCompiler/blob/main/include/dxc/Support/d3d12TokenizedProgramFormat.hpp).
Native GCN opcodes and operands were checked against this repository's
[frontend decoder](../gpu/shadps4/shader_recompiler/frontend/decode.cpp) and
[opcode tables](../gpu/shadps4/shader_recompiler/frontend/opcodes.h).
No binaries, extracted shaders or full disassemblies are included here.

## GPARAM row registration

New rows require both field values and the separate group-ID index represented
by SoulsFormats `UnkParamExtras`. Native readers at `0x0218f870`, `0x0218f8b0`
and `0x0218f8f0` respectively check group existence, obtain its ID count and read
its IDs through header offsets `0x34`, `0x38` and `0x3c`. Smithbox's
`AddValueAction.UpdateGroupIndexes` also updates these indexes after adding rows.
Increasing field capacity and writing values alone is insufficient registration.

The adapter appends each allocated ID to its group's index, creating a group
entry when necessary. Existing index IDs and their order remain intact, including
historical IDs absent from field values. These index-only IDs also reserve
allocator space. The regression fixture includes such a reservation, a previously
unindexed group and a duplicate-index rejection case. A native green-light
diagnostic without the added registrations produced no visible lighting change.
Adding the registrations recolored the imported geometry green while an original
unimported tree retained its normal appearance. This proves application of the
imported light rows; it does not establish identical source rendering.

## Directional light and shadow directions

For pitch `x` and yaw `y`, both executables construct the same vector:

```text
direction = (sin(y) * cos(x), -sin(x), cos(y) * cos(x))
```

| Operation | Verified code |
| --- | --- |
| DS1R LightBank direction | `0x140564630` |
| DS1R ShadowBank direction | `0x14055b7e6` through `0x14055b8d2` |
| Bloodborne directional-light finalization | `0x022e0860` |
| Bloodborne shadow finalization | `0x0226a0d0`, direction body at `0x0226a110` |

Convert source degrees to native radians and preserve both signs. DS1R's degree
constant at `0x141615c54` is approximately `0.01745329238`. Native default negative
pitch and Firelink's positive pitch describe different light directions; they
are not evidence of an inverted coordinate convention.

Source shadow direction is independent of the main diffuse light. Firelink's
used ShadowBank row 20 has `(50,160)` degrees, while LightBank row 20's main light
has `(40,75)` degrees. Each collision requesting a scene binding receives the
new shadow row through `SceneGparamConfig.ShadowParamID`. The native group and
field keys are exactly `ShadowParam ParamEditor` and
`hadowParam - Shadow: Angle0`; the missing initial `S` is present in the asset.

Only the angle is converted. Native row 0's cascades, range, fade, bias, filters,
color and density remain in the appended row because equivalence of their source
fields has not been proved. Source `ShadowID = 255` is reported without allocating a
row or claiming disabled-shadow behavior. Source MSB1 `IsShadowSrc`,
`IsShadowDest` and `IsShadowOnly` flags are carried separately into native part
flags. Firelink's terrain and static objects have different source casting flags;
enabling every part would discard this distinction.

## DS1R light color energy

DS1R's single-color routine at `0x140564510` calls imported `powf` with exponent
`2.2` (constant at `0x1412aa960`). For each color component it evaluates:

```text
sourceEnergy = pow(abs((RGB / 255) * (A / 100)), 2.2)
```

Bloodborne consumes `nativeRGB * nativeRGB * nativeW` for directed colors at
`0x022de8b0` and hemispheric colors at `0x022e0b03`. The adapter therefore writes:

```text
nativeRGB = pow(abs(RGB / 255), 1.1)
nativeW   = pow(abs(A / 100), 2.2)
```

This preserves the source light energy without clipping brightness above 1.
It does not prove equivalence of material BRDFs, environment probes, exposure or
tone mapping. The original `DS1` profile retains its prior normalized transfer;
the Remastered executable is not evidence for original DS1's color processing.

Three source diffuse directions and the separate source specular direction use
separate native slots. Source hemisphere up/down colors are converted too.
[MapEnvironmentTests.cs](../tests/souls_import/MapEnvironmentTests.cs) evaluates
the independent energy equations after GPARAM serialization for dim, bright,
colored, zero and negative-intensity cases. It also checks independent shadow
rows and preservation of existing native values, including duplicate field keys.

## Fog: different curves and controlled native approximation

DS1R FogBank's getter at `0x140565600` outputs begin distance, end-minus-begin,
strength `degRotW/100`, and color `(RGB/255)*(A/100)`. This getter does not apply
the light-color gamma operation. The shipped deferred LightProbe shader uses
`gFC_FogParam` and `gFC_FogCol` to evaluate a capped linear ramp at instruction
offsets `0x1444` through `0x14d4`:

```text
sourceAlpha = saturate(saturate((depth - begin) * inverseRange) * strength)
result      = lerp(litColor, fogColor, sourceAlpha)
```

The shader reconstructs the depth term from reciprocal homogeneous W. Here
`inverseRange` names shader parameter `gFC_FogParam.y`; the CPU getter stores the
range. Its subsequent reciprocal/upload step still needs tracing before an
implementation claims the complete source CPU-to-GPU fog transfer.
Firelink row 20 records begin `-150`, end `70`, strength `65%`, and fog color
`(95,95,92)` with intensity `120%`.

Bloodborne's CPU packer at `0x022d6720` supplies distance reciprocal, offset,
height terms, squared RGB and contrast. The native forward shader at
`0x00f0`–`0x013c` and deferred composite at `0x0194`–`0x01d8` independently show:

```text
A           = FogColor.W * fogAlpha
nativeAlpha = saturate(A * (1 - exp2(-(horizontalDistance - DistOffset) / Dist)))
```

The forward vertex shader exports camera-relative world position at `0x01c0`
through `0x01dc`; the main fog uses its X/Z length. A separate height-fog lerp is
applied afterward. Contrast also scales fog color using the rendered scene's
luminance. These differ from the source depth-based linear ramp, including the
source's plateau below 1. Copying source begin/end into native distance/offset
cannot make the curves identical.

The default `EnvironmentCalibration` creates a new native fog row using source
hue with controlled horizon haze. These are explicit artistic controls rather
than a fit of the complete source curve:

| Control | Default | Meaning |
| --- | --- | --- |
| `SourceFog` | `true` | Create a calibrated native fog row from source color |
| `FogStrength` | `0.30` | Native maximum fog opacity |
| `FogDistance` | `80` | Exponential distance scale in metres |
| `FogStart` | `25` | Distance offset in metres |
| `FogColorScale` | `0.35` | Gain on source `(RGB/255)*(A/100)` color |
| `ToneEV` | `-0.4` | Relative manual exposure adjustment described below |

With these defaults, main fog opacity is zero at 25m, about 0.058 at 50m and 0.143
at 100m. Color is square-root encoded for native CPU squaring; height-fog color
and luminance contrast are zeroed in the new row. Native original rows remain
intact. `SourceFog=false` keeps native FogParam row 0; explicit source
`FogID = 255` always creates an independent disabled row. Reports retain source
fields, controls and the approximation status. The fixture evaluates the actual
native exponential equation independently after serialization. The initial
calibrated Firelink view was inspected in the native game; fitting other views
and maps still requires visual review.

## Tone: relative native exposure calibration

The checked native group is `Tone Map[Yebis]`. `Yebis-ToneMapExposure` is a
positive linear multiplier, unlike the separate `GS Presents - ToneMap-Exposure`
control whose metadata permits negative values. Native `0x021d4f43` copies
manual exposure into the render settings; `0x021d7056` and `0x021d7163` pass it
through `0x00bb3550` to Yebis `SetTonemapParameters` at `0x00bd4d40`. That function
stores it at member offset `0x700`. The consumer at `0x00bd92f0` divides the
stored exposure by scene luminance. Assertion strings independently identify
`SetTonemapExposure` and require a positive exposure value.

The adapter creates an independent ToneMap row and writes
`nativeTemplateExposure * pow(2,ToneEV)`: for template exposure 3, `-0.4EV` gives
approximately 2.273575. Each phase keeps its own native template exposure.
Only collision scene bindings select this row. Other native tone fields,
gamma, grading, bloom and GS Presents controls remain unchanged. Templates
with automatic exposure enabled are rejected because a fixed manual multiplier
would not establish the requested adjustment. `ToneEV=0` retains native tone
bindings. Regression checks evaluate `log2(newExposure/originalExposure)` across
different phase templates and compare every original parameter value.

Source ToneMapBank values remain provenance, not copied tone behavior. Firelink
uses ToneMapID 20 with adapted-luminance bounds 0.04–0.08 and inverseToneMapMul 3;
these are not equivalent to native manual exposure. Source tone curves and color
grading remain unconverted. Fog/tone controls are calibrated for the imported
scene rather than claimed to reproduce the source post-processing.

A native application test changed only imported ToneMap row 30032's exposure to
`-4EV` relative to its native template. The same Firelink view became dramatically
darker, including geometry and player, while the HUD retained its brightness.
All nine staged diagnostic files matched their audited hashes; restoring the
single changed exposure in each profile recovered the exact original uncompressed
GPARAM bytes. This independently proves the selected scene ToneMap row affects
rendering. No additional MSB variant override was needed. The production
calibration remains `-0.4EV`; the dark diagnostic is not a production default.

## DSR sky mixed vertex strides

Firelink's `m9000B2A10` is a little-endian FLVER2 version `0x2000C` with three
meshes containing 352, 341 and 397 vertices. The original file declares three
vertex buffers with strides 36, 32 and 28. The first two share layout 0. Its six
declared members have offsets `0,12,16,20,24,28`, types `2,17,19,19,19,21`, and
semantics `0,2,3,6,10,5`. The first buffer actually includes an extra four-byte
tangent/binormal field at offset 24; its original color and UV bytes start at 28
and 32. Reading this buffer at stride 28 or 32 corrupts subsequent positions and
colors. This is a bounded reader/layout problem, not invalid source geometry.

`SourceFlver.Normalize` recognizes only this exact DS1R layout and stride 36.
For each vertex it retains bytes 0–23, drops 24–27 and copies 28–35 into the
32-byte representation; it updates that buffer's stride/length in a private
copy. Other buffers and layouts remain unchanged. The original data section
begins at 2912 and the first vertex buffer's relative offset is 7472.
[SourceFlverTests.cs](../tests/souls_import/SourceFlverTests.cs) compares all 352
recovered positions and original color channels directly against the source's
36-byte records, in addition to a shared-layout fixture and source immutability.

Sky/cloud textures, vertex RGBA and color multipliers are carried through native
compiled sky templates. The distant landscape uses an unlit sky material; its
source lighting response is approximated. Exact cloud normal lighting, scrolling
and sky scattering are not reproduced. Scattering-bank coefficients are copied
only into checked, typed native fields; shader equivalence remains unproved.
Environment cubemaps, source point lights, source shadow color/density and guards,
source tone curves and color grading also remain outside the current conversion.

## Native collision area-name transitions

Location FMG 19 retains a positive imported name ID. The collision reference uses
its negative value: native handling at guest offset `0x1769f96` accepts a positive
collision ID only while the previous name is unset, whereas a negative ID updates
the absolute name ID, queues banner 8 and marks its text for refresh when the
name changes. A saved native area can therefore suppress a new positive ID.
`MapAreaName` writes negative collision references and preserves the positive FMG
and report IDs; it leaves existing location entries intact. Generic EMEVD banner
8 alone does not perform this collision-driven text refresh. Fixture/readback
coverage verifies the sign and localization preservation; gameplay display needs
the corresponding native transition test.

## Remastered color views and imported scene correction

Legacy/source texture containers do not reliably expose the color-view intent through
the DDS format alone. The shipped native foliage template uses
`m24_01_leaf_pack_600_a` with TPF DXGI 72 (BC1 sRGB), while its normal uses DXGI 71
(BC1 linear). The source `m10_grass_03` DDS reports DXGI 98 (BC7 linear); treating its
encoded RGB directly as linear light makes dark olive texels much brighter and less
saturated after native lighting and output gamma. Texture format numbers and the native
TPF's explicit DXGI field are separate; the latter must carry the correct color view.

`NativeSrgbColor` tags supported color DDSs with their corresponding sRGB DX10 view,
preserving all compressed texels, alpha blocks and mip payloads. The Remastered map
adapter applies this only to diffuse/sky slots, using separate texture names for color
and data views. Normals and material data retain their linear views. Fixture checks
verify unchanged payloads, idempotence and the sRGB DXGI field after PS4 TPF serialization.
DS1 map vertex RGB likewise retains authored shading/tint; opaque native materials use
alpha 1, while sky/effect cards retain source alpha.

Firelink's source `ToneCorrectBank` row 20 specifies saturation 1.2 and uniform brightness
1. Its native donor instead has saturation 0.9 and enables LUT 4. The importer now carries
source correction IDs independently of tone-map IDs, adds a registered scene grading row
that disables the native LUT, and copies saturation/uniform brightness into independent
native tone rows. Native chromatic aberration is disabled on those imported rows.
Native rows, gamma and exposure calibration remain unchanged.

A private gameplay ablation copied source contrast 1.15 into `Yebis-Contrust` and crushed
the scene's shadows. Resetting only that field to native 1 restored detail with the LUT
still disabled. These contrast controls are not numerically interchangeable; source
contrast, unequal RGB correction, nonzero hue and the exact source tone curve remain
unconverted. A separate direct packed-shininess experiment made terrain look wet and was
discarded; static material response continues to use the existing native approximation.

## DS1 FaceGen coordinates and draw variants

The FaceGen TRI/EGM coordinates use the opposite depth axis from the authored DS1
character parts. Reflect position and normal Z before assembling them with the
canonical skeleton. Preserve the FLVER template's original triangle order and
recompute tangents after this reflection. Negating triangle winding alone does
not repair the misplaced geometry: a two-sided material can show the back of the
face through the skull opening while leaving the mouth visibly wrong.

An independent check extracts the 40-vertex boundary from the original
`FC_M_0000` skull triangles. Both Firelink human NPCs' generated face boundaries
then match its X/Y/Z positions within 0.7 micrometres; the unreflected geometry
misses the opening by approximately 8–18 centimetres. This comparison uses
authored geometry rather than another copy of the generated face.

Retain all six authored face draw variants: base, LOD1, LOD2 and their motion-blur
counterparts. Recover UV seams from the base topology and reuse those vertices
for the collapsed LOD topology. Normal backface culling remains enabled.

## Firelink world-state scope

DS1 event `11020350` selects pool floor `o1480`/collision `h0005B2` initially and
the opened `o1481`/`h0105B2` pair after both bell interaction flags `11010700` and
`11400200`. These are distinct from the boss-death conditions. The adapter gives
every converted collision record a unique entity ID, including duplicates that
share a model, and switches all corresponding records together. The completion
flag restores the opened state at map startup.

The separate quest-flag manifest reserves explicit stable native bits, preserving
saved mappings when further source flags are added. Initialization selects the
three source NPC baseline states only when their ranges are empty, once per mod
save. This does not implement the bell interactions, Frampt, water physics,
Lordvessel offering or the Kiln map. A private native-gameplay test confirmed that
one bell retains the physical cap and both bells remove it; its bell-setting
triggers are diagnostic only and are excluded from exports.

## Bounded Firelink speech conversion

Source TalkParam rows have 32 bytes; native rows have 68. Only supported source
sequences are adapted into independent native rows and ESDs. Their localized
subtitles remain in menu FMG 1. Unvoiced imported rows did not sustain readable
subtitles in a native comparison, while a local retail voiced row using the same
actor and state machine did. Registering the original source speech restores
normal subtitle duration.

The source `frpg_sm10.fsb` contains FSB4 MPEG samples with four-byte frame
alignment. Remove alignment padding, decode the selected clips with an installed
`ffmpeg`, and verify their declared mono 44.1 kHz frame counts before producing
native FSB5 PCM16 samples. Each NPC receives an owned character sound project
with one resident bank and default-language metadata, matching the native PCM
character layout. The native map sound project and its banks stay unchanged.
The adapter does not replace a retail voice to borrow its subtitle timing.
The native wrapper uses its shipped
conversation-completion function 57 and completion command 71.

The 45 selected lines share 41 source clips. Only this bounded Firelink voice
schema is supported; these formats and checks do not establish a general sound
bank converter or full NPC quest compatibility.

Native character-resource loading parses the `cXXXX` resource name and requests
its sound project only for IDs 1 through 8999. A higher imported ID can render
correctly while its matching sound project is never requested. This condition
is independent of TAE sound events or the NPC's disabled combat AI.

The same loader looks up `NpcParam[modelID * 100]` and reads `isMultilingual`
(payload byte `0x149`, bit 5). That flag controls both the loaded filename and
the memory-bank registration name, independently of the FEV language metadata.
Clear only this bit on imported NPC clones paired with default-language banks;
the Doll donor enables it. Adding localized filename aliases alone does not
repair the mismatched registration. Original NPC rows remain unchanged.

The map sound task preloads the bank named after its project. An additional FEV
bank descriptor alone does not prove that the native sound manager will load
that bank. Its separate file callback can reject filenames before a filesystem
request, depending on the native loader configuration and allow-list. Validate
actual file requests, finite speech completion and captured game audio rather
than treating subtitle display or FEV readback as playback proof.
