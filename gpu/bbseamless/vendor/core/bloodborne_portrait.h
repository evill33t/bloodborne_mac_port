// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Party portraits (Kaniel 2026-10-13): the party list shows each member's head and shoulders, as
// its own game renders them (outfit and all), with its state icon as a small badge. Each game
// takes its character's portrait from the status page's model (see StatusModelWidth; the port's
// texture cache hands it over as the page samples it), keeps it on disk by the character's face
// data, and sends it to the party's games in
// PortraitPartType frames. The port shows a member's in the party HUD's CoopPartyFace slot
// (bbport/scripts/hud_patch.py), whose placeholder texture its texture cache fills.

#pragma once

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"
#include "core/bloodborne_party.h"

namespace Core::Bloodborne {

// A portrait: PortraitSide square, RGBA rows, opaque. Kaniel 2026-10-17 ("really poor quality"):
// 128, about the model's own head and shoulders (StatusModelCropSide), sharp where a 4K screen
// shows it 72 px across (64 was magnified there).
constexpr int PortraitSide = 128;
constexpr size_t PortraitBytes = size_t{PortraitSide} * PortraitSide * 4;
using PortraitPixels = std::vector<u8>;

// The status page's model target (SYSTEX_MENU_MDL, StatusModelWidth x StatusModelHeight: the whole
// character in its outfit, lit, on a transparent ground) and the square of it a portrait shows:
// the head and shoulders, StatusModelCropSide wide, from StatusModelCropAbove rows above the top
// of the head, the head (its first StatusModelHeadRows rows) in the middle. Measured on the rig's
// 2026-10-13 capture: the hood's top at y 4, its middle at x 274, the collar at about 150.
// (The page's face target, SYSTEX_Menu_StatusFace, is the bare head: no hat, no collar.)
constexpr int StatusModelWidth = 560;
constexpr int StatusModelHeight = 768;
constexpr int StatusModelCropSide = 152;
constexpr int StatusModelCropAbove = 4;
constexpr int StatusModelHeadRows = 60;
struct PortraitCrop {
    int x, y, side;
};
// What shows where the model page is transparent (a dark ground, as the save list's faces have).
constexpr std::array<u8, 3> PortraitBackdrop{0x16, 0x13, 0x11};
// The square of a model target's pixels (RGBA rows) the portrait shows; nothing when no figure
// is on it.
std::optional<PortraitCrop> HeadAndShoulders(std::span<const u8> rgba, int width, int height);
// The portrait of a model target's pixels (RGBA rows, `width` x `height`); nothing for another
// target or one with too little of a character on it yet.
std::optional<PortraitPixels> PortraitFromModel(std::span<const u8> rgba, int width, int height);
// Kaniel 2026-10-17 ("is there a way to render it like it's seen in a game?"): the off-screen
// model is drawn with the character creator's face camera (BodyRendererSetCameraMode, mode
// FaceCameraMode: a narrow lens on the head), so the head fills the target, about six times the
// status page's detail. Its portrait is the target's full-width square from FaceCropTop: the
// hood, the face and the collar (the camera frames the head, so no search). Nothing for another
// target or an empty one.
constexpr int FaceCropTop = 20;
std::optional<PortraitPixels> FacePortraitFromModel(std::span<const u8> rgba, int width,
                                                    int height);
// The portrait as the party list shows it, in the round (Kaniel 2026-10-13: a circle in a
// Bloodborne frame, hud_patch.py's): clear outside its inscribed circle, its edge smoothed,
// premultiplied as the HUD's bitmaps are. Nothing for anything but a portrait.
PortraitPixels CircularPortrait(const PortraitPixels& pixels);

// A portrait on the co-op link: RGB888 in PortraitPartCount parts of PortraitPartBytes, each
// naming the member (its board name) and the picture (PortraitId). Kaniel 2026-10-17 ("very bad
// colors"): true colour; RGB565 banded the dark hoods and backdrops and turned them green-grey.
// CoopFrameMax is the link's frame size (P2PCoopSendTo, the receive buffer in TakeDirectDamage).
constexpr u8 PortraitPartType = 0x3E;
constexpr size_t PortraitPartBytes = 1024;
constexpr size_t PortraitPartCount = size_t{PortraitSide} * PortraitSide * 3 / PortraitPartBytes;
constexpr size_t CoopFrameMax = 1400;
struct PortraitPart {
    std::string member;
    u32 id = 0;
    u8 part = 0;
    u8 parts = 0;
    std::vector<u8> bytes;
    bool operator==(const PortraitPart&) const = default;
    // The member has no picture any more (see EncodePortraitNone).
    bool None() const {
        return parts == 0;
    }
};
u32 PortraitId(std::span<const u8> pixels);
std::vector<PortraitPart> SplitPortrait(std::string_view member, const PortraitPixels& pixels);
// The payload: member (u8 length first), id (u32), part, parts, then PortraitPartBytes.
std::vector<u8> EncodePortraitPart(const PortraitPart& part);
std::optional<PortraitPart> DecodePortraitPart(std::span<const u8> payload);
// The member has no picture any more (its outfit changed and the status page has not shown it
// yet): the member, id 0, part 0 of 0 and no bytes.
std::vector<u8> EncodePortraitNone(std::string_view member);
// A member's parts until its picture is whole. The parts of a member's PicturesKept latest
// pictures are kept apart (a late part of the one before is no loss); an older one's go.
class PortraitAssembler {
public:
    static constexpr size_t PicturesKept = 2;
    // The member's picture, once this part made it whole (once per picture).
    std::optional<PortraitPixels> Take(const PortraitPart& part);

private:
    struct Pending {
        u32 id = 0;
        std::vector<u8> rgb;
        u64 have = 0; // a bit per part
        bool made = false;
        u64 touched = 0;
    };
    std::map<std::string, std::vector<Pending>, std::less<>> by_member;
    u64 touches = 0;
};

// The members' portraits and what the party list's slots show (CoopPartyFace0-2): a slot's version
// changes whenever its picture does, so the port refreshes its texture then only.
constexpr int PortraitSlotCount = 3;
class PartyPortraits {
public:
    void Take(std::string member, PortraitPixels pixels);
    bool Has(std::string_view member) const;
    // The member has no picture any more.
    void Forget(std::string_view member);
    // The slot shows `member` ("" for nobody): whether it shows a portrait.
    bool Show(int slot, std::string_view member);
    u32 Version(int slot) const;
    std::optional<PortraitPixels> Pixels(int slot) const;
    // A member's picture by its board name, and its id (0 for none; another picture, another id).
    u32 IdOf(std::string_view member) const;
    std::optional<PortraitPixels> PixelsOf(std::string_view member) const;
    // The members still in the party: the others' pictures go.
    void KeepOnly(std::span<const std::string> members);

private:
    struct Picture {
        u32 id = 0;
        PortraitPixels pixels;
    };
    std::map<std::string, Picture, std::less<>> by_member;
    std::array<std::string, PortraitSlotCount> shown{};
    std::array<u32, PortraitSlotCount> shown_id{};
    std::array<u32, PortraitSlotCount> versions{};
};

// The captures of the status page's model while it shows (one now and then): the last one becomes
// the portrait once none came for SettleMs (the page closed).
class FaceCapture {
public:
    static constexpr s64 SettleMs = 1'500;
    void Take(PortraitPixels pixels, s64 now_ms);
    std::optional<PortraitPixels> Settled(s64 now_ms);

private:
    std::optional<PortraitPixels> last;
    s64 at = 0;
};

// When this game sends its portrait (`id`, 0 for none) to its room peers (sorted): at once when it
// changed or a peer came, and again every ResendMs (a lost part). None is sent as a picture is
// once a picture was (the peers forget it), never before.
class PortraitSender {
public:
    static constexpr s64 ResendMs = 15'000;
    bool Due(u32 id, std::span<const u64> peers, s64 now_ms);

private:
    u32 sent_id = 0;
    bool sent_any = false;
    std::vector<u64> sent_peers;
    s64 sent_at = 0;
};

// The character's portrait on disk (user/portraits/<face>-<outfit>.<format>.rgba, PortraitBytes):
// by its face data and the armour it wears (head, body, arms, legs ids), and the capture's format
// (PortraitCaptureFormat; bump it when captures change): a picture of another outfit, or one an
// older build took, is never loaded (Kaniel 2026-10-13: #77's first build's bald heads were).
constexpr std::string_view PortraitCaptureFormat = "f1";
struct PortraitKey {
    u64 face = 0;
    std::array<s32, 4> outfit{};
    bool operator==(const PortraitKey&) const = default;
};
std::filesystem::path PortraitCachePath(const std::filesystem::path& dir, const PortraitKey& key);
bool SavePortrait(const std::filesystem::path& path, const PortraitPixels& pixels);
std::optional<PortraitPixels> LoadPortrait(const std::filesystem::path& path);

// A list slot's art: the icon beside the bar (CoopPartyIcon), the badge on the portrait
// (CoopPartyBadge) and the portrait itself (CoopPartyFace, PortraitFrame...), frames as
// GotoAndStop takes them.
constexpr u32 PortraitFrameNone = 1;
constexpr u32 PortraitFrameShown = 2;
// The portrait in the red ring: the member is downed (Kaniel 2026-10-18: no badge, the ring tells).
constexpr u32 PortraitFrameDowned = 3;
struct PartyListArt {
    u32 icon = PartyListIconNone;
    u32 badge = PartyListIconNone;
    u32 face = PortraitFrameNone;
    bool operator==(const PartyListArt&) const = default;
};
PartyListArt PartyListArtFor(const PartyListEntry& entry, bool portrait);

// The off-screen portrait (TickPortraits, the port only): while this character and outfit have no
// portrait, the status page's body renderer is built off-screen (index 5, RankingBody), read back
// once it has drawn the character and destroyed once the capture settled.
enum class OffscreenState : u8 { Idle, Building, Drawn, Closing };
enum class OffscreenAction : u8 { Wait, Build, ReadBack, Close, Destroy };
// A build at most this often (its capture settles 1.5 s after the read back ends), the time it may
// take to draw (a first load of the armour from disk), the read back's length and the close's.
constexpr s64 OffscreenRetryMs = 15000;
constexpr s64 OffscreenDrawTimeoutMs = 20000;
constexpr s64 OffscreenReadBackMs = 2000;
constexpr s64 OffscreenCloseMs = 250;
// `in_world`: a local player and no load; `in_state_ms`: since the state began; `since_build_ms`:
// since the last build.
OffscreenAction NextOffscreenAction(OffscreenState state, bool wanted, bool in_world, bool drawn,
                                    bool captured, s64 in_state_ms, s64 since_build_ms);

} // namespace Core::Bloodborne
