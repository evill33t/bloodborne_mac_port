// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QString>

namespace Bloodborne {

class SummonBroker {
public:
    enum class State { Advertised, Preparing, Claimed, Delivered, Consumed };
    enum class ClaimStatus { Claimed, AlreadyClaimed, NotFound, Conflict };

    struct Options {
        qint64 ttlMs = 130'000;
        qint64 seamlessTtlMs = 15 * 60 * 1000;
        bool seamlessCoop = false;
        bool seamlessAnywhereSummons = false;
        // A delivered claim the guest has not joined within this is dropped (see
        // DropUnjoinedClaimsLocked).
        qint64 claimJoinWindowMs = 20'000;
    };

    struct AdvertiseResult {
        State state = State::Advertised;
        QByteArray pendingClaim;
        QByteArray pendingHostPlacement;
    };

    struct ClaimResult {
        ClaimStatus status = ClaimStatus::NotFound;
        QString targetSessionId;
        qint64 targetUserId = -1;
    };

    struct ConsumeResult {
        int consumed = 0;
        int retained = 0;
        QByteArray pendingHostPlacement;
    };

    // Games re-advertise a placed sign about every 30 s. A sign not refreshed for longer than
    // this belongs to a game that quit, and is no longer offered to searches.
    static constexpr qint64 SignRefreshWindowMs = 75'000;
    // A sign shown to one host this many times without a claim shows a new id from the next
    // search on (rig 2026-10-11: the host's game kept a stale queue entry for it and never
    // claimed it in 48 searches).
    static constexpr int UnclaimedOffersBeforeNewId = 4;
    // Signs kept at most (one a player, refreshed every 30 s): past it a new sign takes the place
    // of the one refreshed longest ago.
    static constexpr int MaxRecords = 4096;

    SummonBroker();
    explicit SummonBroker(qint64 ttlMs);
    explicit SummonBroker(Options options);

    // `party` is the client's party key (X-ShadPS4-Bloodborne-Party). Signs advertised with a
    // key are only found and claimed by requests with the same key; empty means public.
    AdvertiseResult Advertise(const QJsonObject& body, const QByteArray& rawBody, qint64 nowMs,
                              const QByteArray& party = {});
    QList<QByteArray> Search(const QJsonObject& request, qint64 nowMs,
                             const QByteArray& hostPlacement = {}, const QByteArray& party = {});
    ClaimResult Claim(const QJsonObject& request, const QByteArray& rawRequest, qint64 nowMs,
                      const QByteArray& hostPlacement = {}, const QByteArray& party = {});
    // Only the party's signs, as for Search and Claim.
    ConsumeResult Consume(const QJsonObject& request, qint64 nowMs, const QByteArray& party = {});

    std::optional<State> StateFor(const QString& sessionId, qint64 userId, qint64 nowMs);
    int Size(qint64 nowMs);
    bool IsSeamlessCoopEnabled() const;
    bool IsSeamlessAnywhereSummonsEnabled() const;

private:
    struct Record {
        State state = State::Advertised;
        QJsonObject advertisement;
        QByteArray rawAdvertisement;
        QJsonObject claim;
        QByteArray rawClaim;
        QByteArray hostPlacement;
        QByteArray party;
        // The guest consumed a delivered claim, i.e. it is (or was) in the host's world.
        bool joined = false;
        qint64 preparationRequester = -1;
        qint64 updatedAtMs = 0;
        qint64 deliveredAtMs = 0;
        // The id searches show for this sign once a claim on it was dropped; empty: its own.
        QString searchSessionId;
        // The host it was last shown to, and how many times in a row without a claim.
        qint64 offeredTo = -1;
        int unclaimedOffers = 0;
    };

    void PurgeExpiredLocked(qint64 nowMs);
    // A claim delivered but not joined within the window (rig 2026-10-10: the guest never
    // joined the host's room) is dropped, and the sign shows a new id from then on: the host's
    // game keeps the old one on its pending list and never summons it again.
    void DropUnjoinedClaimsLocked(qint64 nowMs);

    qint64 m_ttlMs;
    qint64 m_claimJoinWindowMs = 20'000;
    qint64 m_seamlessTtlMs;
    bool m_seamlessCoop;
    bool m_seamlessAnywhereSummons;
    QMutex m_mutex;
    QHash<QString, Record> m_records;
};

bool HasRequiredAdvertisementFields(const QJsonObject& body);
// Validates an X-ShadPS4-Bloodborne-Party header value ("name:secret"); empty if unusable.
QByteArray NormalizePartyKey(const QByteArray& header);
QByteArray BuildClaimDeliveryResponse(const QByteArray& rawClaim);

} // namespace Bloodborne
