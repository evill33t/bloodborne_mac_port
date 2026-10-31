// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The server link (docs/SERVER_COOP_PLAN.md phase 2): every game keeps one UDP
// link to shadNet, on the matching UDP port its STUN pings come to, and sends
// through it what it would send to another game. shadNet forwards each frame to
// the game at the frame's destination, naming the sender by the address its
// STUN pings come from, which is the address the other games know it by: the
// games see the same peers as over P2P, and nothing goes game to game.
//
// The datagrams (the emulator's net_link.h writes and reads the same; one of
// each in tests/contracts/server_link_frames.txt): the signaling vport header
// (ffff ffff), "SHLK", the kind, its body. Numbers little-endian, addresses and
// ports in network byte order.
//
// shadNet never reads a frame: it checks who sent it and where it goes. It
// knows each game's link from its pings (once a second) and tells the others
// when one goes quiet, comes back or is gone, and logs enough to say whether a
// stalled join's packets went through (the first frames between each pair, and
// counters per member).
#pragma once

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include "coop/link_auth.h"

namespace ServerLink {

// 2 (phase 11): the hello carries the player's secret and signs it in.
// 3: a cookie first, then every datagram after the hello tagged.
// 4: the hello and its answer a Noise NK handshake, every datagram after them encrypted
// (coop/link_auth.h).
constexpr quint8 LinkVersion = 4;
enum class Kind : quint8 {
    Hello = 1,
    Welcome = 2,
    Data = 3,
    Ping = 4,
    Pong = 5,
    Member = 6,
    // Our messages (coop/coop.proto) between a game and shadNet itself, in the link's reliable
    // stream (coop/coop_stream.h): the body is one stream frame. Phase 3: the party's world.
    Coop = 7,
    // Phase 11: the calls that went over shadNet's TCP port (friends, scores, the NP sign-in), in
    // a second reliable stream (coop/coop_stream.h) of the link, so a host opens one UDP port.
    Rpc = 8,
    // Link version 3: the answer to a hello without a valid cookie (coop/link_auth.h), never
    // encrypted; version 4: with the server's public key.
    Cookie = 9,
    // Hosted: the answer to a hello for an id with no account and no work nonce (or not enough):
    // the bits its work needs (coop/link_auth.h), sealed like the hello's other answers.
    Work = 10,
    // A region of shards (docs/SCALING.md): continue on another shard, its address, key and the
    // ticket its hellos carry; the answer to a hello (in place of a welcome) or on the session.
    Redirect = 11,
};
enum class MemberState : quint8 { Quiet = 0, Back = 1, Gone = 2 };

// An IPv4 address and a port, both in host byte order.
struct Endpoint {
    quint32 ipv4 = 0;
    quint16 port = 0;
    quint64 Key() const {
        return (quint64{ipv4} << 16) | port;
    }
    QString Text() const;
    bool operator==(const Endpoint&) const = default;
};

// Phase 7: a game is known to the others by a virtual address made from its account, never by its
// real one: 10.x.y.z (x.y.z from a hash of the online id) and this port, the same from whatever
// port or address the game links from, and after a restart of the server.
constexpr quint16 VirtualPort = 3658;
Endpoint VirtualEndpointOf(const QByteArray& online_id);

struct Redirect {
    Endpoint to;
    LinkAuth::Key key{}; // the shard's link key (the game pins it)
    QByteArray ticket;   // opaque to the game (a region ticket, region.h); at most 4 KiB
    QByteArray text;     // why, for the game's log
};

// Phase 11: whether a hello signed in, and when not, the text its game hears (empty: the usual).
struct SignInVerdict {
    bool accepted = false;
    QByteArray refusal;
    bool work = false; // hosted: no account for the id and the hello did no work: ask for it
    // A region (docs/SCALING.md): the hello is answered with this instead of a welcome.
    std::optional<Redirect> redirect;
};

// The version, the cookie (16 bytes, zeros before the server gave one), the id, the build and the
// secret. These are the hello's plain bytes: link version 4 sends all after the cookie encrypted
// (LinkAuth::Client::SealHello), and the hub decodes what LinkAuth::Responder opened.
struct Hello {
    quint8 version = LinkVersion;
    QByteArray online_id;
    QByteArray build;
    QByteArray secret; // phase 11: the player id's secret (the launcher made it)
    QByteArray cookie; // 16 bytes on the wire
    // Hosted: the work nonce for a new account (8 bytes after the secret, only when there is one).
    std::optional<quint64> work;
    // A region: the ticket of the shard that sent the game here (after the work's 8 bytes, 0 for
    // none done, as a u16 size and its bytes).
    QByteArray ticket;
};
struct Welcome {
    bool accepted = false;
    QByteArray text;
};
struct Data {
    Endpoint peer; // where it goes (from a game), where it came from (to a game)
    QByteArray frame;
};
struct Ping {
    quint32 seq = 0;
    quint64 sent_ms = 0;
};
struct Member {
    MemberState state = MemberState::Quiet;
    quint32 gap_ms = 0;
    Endpoint endpoint;
    QByteArray online_id;
};

// The kind of a link datagram, or nothing for anything else (the STUN ping).
std::optional<Kind> KindOf(const QByteArray& datagram);
QByteArray Encode(const Hello& hello);
// The cookie, the bits a new account's work needs (0: none; hosted only), the server's public key.
QByteArray EncodeCookie(const LinkAuth::Cookie& cookie, int work_bits, const LinkAuth::Key& key);
std::optional<LinkAuth::Cookie> DecodeCookie(const QByteArray& datagram, int* work_bits = nullptr,
                                             LinkAuth::Key* key = nullptr);
QByteArray EncodeWork(int bits);
std::optional<int> DecodeWork(const QByteArray& datagram);
QByteArray Encode(const Welcome& welcome);
QByteArray Encode(const Data& data);
QByteArray Encode(Kind kind, const Ping& ping);
QByteArray Encode(const Member& member);
QByteArray EncodeCoop(const QByteArray& body);
QByteArray EncodeRpc(const QByteArray& body);
std::optional<Hello> DecodeHello(const QByteArray& datagram);
std::optional<Welcome> DecodeWelcome(const QByteArray& datagram);
std::optional<Data> DecodeData(const QByteArray& datagram);
std::optional<Ping> DecodePing(Kind kind, const QByteArray& datagram);
std::optional<Member> DecodeMember(const QByteArray& datagram);
std::optional<QByteArray> DecodeCoop(const QByteArray& datagram);
std::optional<QByteArray> DecodeRpc(const QByteArray& datagram);
QByteArray Encode(const Redirect& redirect);
std::optional<Redirect> DecodeRedirect(const QByteArray& datagram);
// A hello of another link version decodes to its version only.

// The server's static key (link version 4) from `path`, made there the first time (32 random bytes,
// only the server's user may read them); `made` says it was. Nothing when the file is unreadable or
// not a key (the server must not start with another key: its games pinned this one).
std::optional<LinkAuth::Key> KeyFromFile(const QString& path, bool* made, QString* why);

// The links of the games on this server, without sockets or clocks: the socket
// hands it each datagram and calls Tick now and then; it says what to send and
// what to log. Link version 4 (coop/link_auth.h): a hello needs a cookie of its
// source and opens with the server's key, and every datagram after it is
// encrypted with its link's keys; what it says to send is sealed already.
class Hub {
public:
    struct Send {
        Endpoint to;
        QByteArray datagram;
        bool sealed = false; // sealed already (or a cookie, never encrypted)
    };
    // A Coop datagram's body for the party's world (bloodborne_world.h), from a linked game.
    struct Coop {
        Endpoint from;
        QByteArray online_id;
        QByteArray body;
    };
    struct Result {
        QList<Send> sends;
        QStringList log;
        QList<Coop> coop;
        QList<Coop> rpc;      // phase 11: the Rpc stream's datagram bodies, the same way
        QList<Endpoint> gone; // links forgotten
        // Kaniel 2026-10-25: links newly quiet and heard again (the world's teams: a game gone a
        // few seconds leaves its started team).
        QList<Endpoint> quiet;
        QList<Endpoint> back;
        // Phase 11: links silent for lost_ms (once a silence): their game's sign-in session ends,
        // as its TCP connection's did when its game died. A blip under that rides out.
        QList<Endpoint> lost;
        // Games welcomed, with their virtual addresses (phase 7: what the rooms hand out).
        struct Linked {
            QByteArray online_id;
            Endpoint virtual_endpoint;
            Endpoint endpoint;
        };
        QList<Linked> linked;
    };

    explicit Hub(qint64 quiet_ms = 3000, qint64 gone_ms = 60000, qint64 stats_ms = 10000,
                 int pair_lines = 20, qint64 lost_ms = 12000);

    Result Receive(const Endpoint& from, const QByteArray& datagram, qint64 now_ms);
    // Links newly quiet (told to the others), gone (forgotten) and the counters.
    Result Tick(qint64 now_ms);

    // The key of the cookies (random per hub; a restart makes the games ask again).
    void SetCookieKey(const QByteArray& key);
    // The static key the hellos are encrypted to (random per hub unless set: LinkServer keeps one
    // in LinkKeyFile), and its public half (the launcher gives it to the games).
    void SetStaticKey(const LinkAuth::Key& secret);
    const LinkAuth::Key& PublicKey() const {
        return static_key.pub;
    }
    // Whether a hello datagram echoes a cookie of its source.
    bool CookieValid(const Endpoint& from, const QByteArray& hello, qint64 now_ms) const;
    // A datagram for the game linked at `to`, encrypted; nothing when none is.
    std::optional<QByteArray> Seal(const Endpoint& to, QByteArray datagram);
    // A refusal for a hello with a valid cookie that opens, sealed for its game; nothing otherwise.
    // It costs the hello's key exchange and the answer's (hosted: hellos are limited before).
    std::optional<QByteArray> Refusal(const Endpoint& from, const QByteArray& hello,
                                      const QByteArray& text, qint64 now_ms) const;
    // Whether a linked game's datagram, once it opened, goes on (hosted: its rates, admission.h).
    // A datagram is opened first: a spoofed one never counts against its address.
    using Admit = std::function<bool(const Endpoint& from, qsizetype bytes)>;
    void SetAdmit(Admit admit) {
        this->admit = std::move(admit);
    }
    // Datagrams dropped for a bad tag, and for a counter seen already (or too old); hellos with a
    // valid cookie that did not open (another server's key, or changed on the way).
    quint64 BadTags() const {
        return bad_tags;
    }
    quint64 BadHellos() const {
        return bad_hellos;
    }
    quint64 Replays() const {
        return replays;
    }
    quint64 CookiesSent() const {
        return cookies_sent;
    }

    // Phase 7: each link's team (its party, empty before the game said which). Once set, a frame
    // and a member notice go only to games of the sender's team, never to one in no team.
    using PartyOf = std::function<QString(const Endpoint&)>;
    void SetPartyOf(PartyOf party_of) {
        this->party_of = std::move(party_of);
    }
    // Invasions (docs/INVASIONS.md): the party a link visits as a hostile invader (empty: none).
    // A visitor and the visited party's games are one team for the relay.
    void SetVisiting(PartyOf visiting) {
        this->visiting = std::move(visiting);
    }
    // Phase 9: whether a game packet on `channel` goes from one link's game to another's
    // (coop/packet_routes.h). Once set, the hub reads every frame of game packets and forwards
    // only what this lets through; without it frames go unread.
    using PacketRoutes =
        std::function<bool(const Endpoint& from, const Endpoint& to, quint32 channel)>;
    void SetPacketRoutes(PacketRoutes packet_routes) {
        this->packet_routes = std::move(packet_routes);
    }

    // Phase 11: whether a hello's player id and secret sign in. Without it every hello does.
    // Hosted (docs/HOSTED_SERVER.md): nothing yet while the password is checked on another thread;
    // the hello gets no answer and the game's next one (a second later) gets the verdict.
    // A refusal may say why (a new id on another account's address: the launcher makes another).
    // `paid`: the hello may make an account (it did the work, or none is asked); one that may not
    // and finds none answers work, and its game says hello again with the work done.
    using SignIn = std::function<std::optional<SignInVerdict>(const QByteArray& online_id,
                                                              const QByteArray& secret, bool paid)>;
    void SetSignIn(SignIn sign_in) {
        this->sign_in = std::move(sign_in);
    }
    // A region: the ticket of the hello the sign-in reads (empty outside its call, or none).
    const QByteArray& HelloTicket() const {
        return hello_ticket;
    }

    // Hosted: the zero bits a new account's work needs (coop/link_auth.h; 0: none). The cookies say
    // it; a hello's work is checked against it with one hash before its sign-in.
    void SetSignupWork(int bits) {
        signup_work = std::clamp(bits, 0, LinkAuth::MaxWorkBits);
    }
    int SignupWork() const {
        return signup_work;
    }
    quint64 WorkAsked() const {
        return work_asked;
    }

    // Hosted: the build every game must say (empty: the first linked game's, as before).
    void SetRequiredBuild(QByteArray build) {
        required_build = std::move(build);
    }

    // Build check on join (coop/build_fingerprint.h): shadNet's own build, which a game's must
    // match where the server knows the field (its release); empty: the server does not check its
    // own (a dev or rig server built apart from the games). Without it only the games here count.
    void SetServerBuild(QByteArray fingerprint) {
        server_build = std::move(fingerprint);
    }

    // Explicit release-label aliases, applied to the server and linked peers alike.
    // Fingerprint protocol, authentication and legacy HostedBuild checks are unchanged.
    void SetCompatibleVersions(std::vector<std::string> versions) {
        compatible_versions = std::move(versions);
    }

    int Members() const {
        return static_cast<int>(members.size());
    }
    // The build every game here runs, empty while none is linked.
    QByteArray Build() const;
    // Whether a game of `online_id` is linked.
    bool HasLink(const QByteArray& online_id) const;
    // The virtual address of the game linked from `real`, nothing when none is.
    std::optional<Endpoint> VirtualOf(const Endpoint& real) const;

    // The operator's view (admin.h): every link.
    struct LinkRow {
        Endpoint endpoint;
        QByteArray online_id;
        QByteArray build;
        qint64 last_heard_ms = 0;
    };
    QList<LinkRow> Links() const;
    // The operator's kick: the links of `online_id` (when not empty) or from `ipv4` (when not 0)
    // forgotten now, the others told they went. A game whose link is forgotten hears no more pongs
    // and says hello again, as after any outage.
    Result Kick(const QByteArray& online_id, quint32 ipv4, qint64 now_ms);

private:
    struct Link {
        Endpoint endpoint;
        Endpoint virtual_endpoint;
        QByteArray online_id;
        QByteArray build;
        qint64 last_heard_ms = 0;
        bool quiet = false;
        bool lost = false; // its sign-in session ended for this silence
        quint64 frames_in = 0;
        quint64 bytes_in = 0;
        quint64 frames_out = 0;
        quint64 bytes_out = 0;
        quint32 pings = 0;
        std::map<quint32, quint64> refused; // game packets not forwarded, per channel
        std::optional<LinkAuth::Session> session;
        // The hello the session came from (its game's ephemeral key) and its welcome: the same
        // hello again gets the same welcome, and the session stays.
        LinkAuth::Key hello_key{};
        QByteArray welcome;
    };

    void OnHello(const Endpoint& from, const QByteArray& datagram, qint64 now_ms, Result& result);
    // Tags every send of `result` not tagged yet; one for a game not linked is dropped.
    void SealSends(Result& result);
    void Dispatch(Link& link, Kind kind, const QByteArray& datagram, Result& result);
    void OnData(Link& sender, const QByteArray& datagram, Result& result);
    void Heard(Link& link, qint64 now_ms, Result& result);
    void Unlink(quint64 key);
    void TellOthers(const Link& about, MemberState state, qint64 gap_ms, Result& result);
    QString Name(const Link& link) const;
    bool SameTeam(const Endpoint& a, const Endpoint& b) const;
    // Invasions: one of them visits the other's party as an invader.
    bool Visits(const Endpoint& a, const Endpoint& b) const;
    // The frame with only the game packets the routes let reach `target`, or nothing when it is
    // a frame of game packets that does not read.
    std::optional<QByteArray> Routed(Link& sender, const Link& target, const QByteArray& frame);

    qint64 quiet_ms;
    qint64 gone_ms;
    qint64 stats_ms;
    qint64 lost_ms;
    int pair_lines;
    bool stats_started = false;
    qint64 stats_at_ms = 0;
    QHash<quint64, Link> members;
    QHash<quint64, quint64> by_virtual; // a link's virtual address -> its real one (keys)
    QHash<QPair<quint64, quint64>, quint64> pair_frames;
    quint64 dropped = 0;
    LinkAuth::HmacKey cookie_key;
    LinkAuth::ServerKey static_key;
    quint64 bad_tags = 0;
    quint64 bad_hellos = 0;
    quint64 replays = 0;
    quint64 cookies_sent = 0;
    quint64 old_versions = 0;
    int signup_work = 0;
    quint64 work_asked = 0;
    Admit admit;
    PartyOf party_of;
    PartyOf visiting;
    PacketRoutes packet_routes;
    SignIn sign_in;
    QByteArray hello_ticket;
    QByteArray required_build;
    QByteArray server_build;
    std::vector<std::string> compatible_versions;
};

} // namespace ServerLink
