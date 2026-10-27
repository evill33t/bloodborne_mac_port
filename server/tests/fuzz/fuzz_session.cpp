// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QTcpSocket>
#include <QTemporaryDir>

#include "client_session.h"
#include "config.h"
#include "database.h"
#include "fuzz_targets.h"
#include "protocol.h"
#include "score_cache.h"
#include "score_files.h"

// A session signed in as CmdLogin leaves it, without the password's hash (100k
// rounds) each time.
struct ClientSessionFuzz {
  static void SignIn(ClientSession &session, SharedState &shared,
                     int64_t user_id, const QString &npid) {
    session.m_authenticated = true;
    session.m_info.userId = user_id;
    session.m_info.npid = npid;
    QWriteLocker lock(&shared.clientsLock);
    SharedState::ClientEntry entry;
    entry.npid = npid;
    entry.send = [](QByteArray) {};
    entry.resetMatchingRoomState = [](uint64_t) {};
    shared.clients[user_id] = std::move(entry);
    shared.npidToUserId[npid] = user_id;
  }
  static void Take(ClientSession &session, const QByteArray &bytes) {
    session.Take(bytes);
  }
};

namespace ShadNetFuzz {

namespace {

constexpr CommandType Commands[] = {
    CommandType::Login,
    CommandType::Terminate,
    CommandType::Create,
    CommandType::Delete,
    CommandType::SendToken,
    CommandType::SendResetToken,
    CommandType::ResetPassword,
    CommandType::ResetState,
    CommandType::AddFriend,
    CommandType::RemoveFriend,
    CommandType::AddBlock,
    CommandType::RemoveBlock,
    CommandType::GetServerFeatures,
    CommandType::GetBoardInfos,
    CommandType::RecordScore,
    CommandType::RecordScoreData,
    CommandType::GetScoreData,
    CommandType::GetScoreRange,
    CommandType::GetScoreFriends,
    CommandType::GetScoreNpid,
    CommandType::GetScoreAccountId,
    CommandType::GetScoreGameDataByAccId,
    CommandType::GetToken,
    CommandType::SetAppearOffline,
    CommandType::ContextStart,
    CommandType::CreateRoom,
    CommandType::JoinRoom,
    CommandType::LeaveRoom,
    CommandType::SearchRoom,
    CommandType::RequestSignalingInfos,
    CommandType::ContextStop,
    CommandType::SetUserInfo,
    CommandType::SetRoomDataInternal,
    CommandType::SetRoomDataExternal,
    CommandType::KickoutRoomMember,
    CommandType::GetWorldInfoList,
    CommandType::GetRoomDataExternalList,
    CommandType::GetUserInfoList,
    CommandType::GetRoomMemberDataExternalList,
    CommandType::SendRoomMessage,
};
constexpr size_t CommandCount = sizeof(Commands) / sizeof(Commands[0]);

// One server for the process: its accounts (two players, friends), scores,
// rooms.
struct Server {
  Server() {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char name[] = "shadnet_fuzz";
      static char *argv[] = {name, nullptr};
      app = std::make_unique<QCoreApplication>(argc, argv);
    }
    QDir::setCurrent(folder.path()); // score_data/ goes here
    db_path = folder.path() + "/fuzz.db";
    Database db("fuzz_setup");
    db.Open(db_path);
    db.CreateAccount("fuzzer", "secret", "avatar.png", "fuzzer@p2p.db");
    db.CreateAccount("friend1", "secret", "avatar.png", "friend1@p2p.db");
    user_id = db.GetUserId("fuzzer").value_or(1);
    const int64_t friend_id = db.GetUserId("friend1").value_or(2);
    db.SetRelStatus(user_id, friend_id, 3, 3);
    shared.config = &config;
    shared.scoreCache = &scores;
    shared.scoreFiles = &files;
    files.Init();
    ScoreBoardConfig board;
    scores.LoadTable(QStringLiteral("NPWR00000_00"), 1, board, {});
  }

  std::unique_ptr<QCoreApplication> app;
  QTemporaryDir folder;
  QString db_path;
  ConfigManager config;
  ScoreCache scores;
  ScoreFiles files;
  SharedState shared;
  int64_t user_id = 1;
};

Server &TheServer() {
  static Server server;
  return server;
}

void PutU16(QByteArray &out, quint16 value) {
  out.append(static_cast<char>(value));
  out.append(static_cast<char>(value >> 8));
}
void PutU32(QByteArray &out, quint32 value) {
  for (int index = 0; index < 4; ++index)
    out.append(static_cast<char>(value >> (8 * index)));
}

// A request packet: the command, then the payload.
QByteArray Packet(CommandType command, const QByteArray &payload,
                  quint32 id = 1) {
  QByteArray out;
  out.append(static_cast<char>(PacketType::Request));
  PutU16(out, static_cast<quint16>(command));
  PutU32(out, HEADER_SIZE + static_cast<quint32>(payload.size()));
  PutU32(out, id);
  PutU32(out, 0);
  return out + payload;
}

} // namespace

// A chunk's selector: bit 0 the bytes are a request's command (its first byte,
// of the known commands) and payload (a header added), else bytes as the socket
// reads them. The first chunk's bit 7: the session is signed in.
void SessionPackets(std::span<const std::uint8_t> input) {
  Server &server = TheServer();
  auto *socket = new QTcpSocket; // the session's from here on
  ClientSession session(socket, &server.shared, server.db_path, false);
  const bool signed_in = !input.empty() && (input[0] & 0x80);
  if (signed_in)
    ClientSessionFuzz::SignIn(session, server.shared, server.user_id, "fuzzer");
  Chunks chunks{input};
  std::uint8_t selector = 0;
  std::span<const std::uint8_t> bytes;
  quint32 id = 0;
  while (chunks.Next(selector, bytes)) {
    QByteArray data(reinterpret_cast<const char *>(bytes.data()),
                    static_cast<qsizetype>(bytes.size()));
    if ((selector & 1) && !data.isEmpty()) {
      const CommandType command =
          Commands[static_cast<quint8>(data[0]) % CommandCount];
      data = Packet(command, data.mid(1), ++id);
    }
    ClientSessionFuzz::Take(session, data);
  }
  session.CleanupOnDisconnect();
  // A Delete may have taken the player's account: it is made again for the next
  // input.
  if (signed_in) {
    Database db("fuzz_again");
    db.Open(server.db_path);
    if (!db.GetUserId("fuzzer")) {
      db.CreateAccount("fuzzer", "secret", "avatar.png", "fuzzer@p2p.db");
      server.user_id = db.GetUserId("fuzzer").value_or(1);
    }
  }
}

std::vector<Bytes> SessionSeeds() {
  const auto put = [](Bytes &input, std::uint8_t selector,
                      const QByteArray &bytes) {
    PutChunk(
        input, selector,
        std::span{reinterpret_cast<const std::uint8_t *>(bytes.constData()),
                  static_cast<size_t>(bytes.size())});
  };
  std::vector<Bytes> seeds;
  QByteArray com_id("NPWR00000_00", 12);
  // Every command once with an empty blob, once after a com id, signed in.
  for (size_t index = 0; index < CommandCount; index += 4) {
    Bytes input;
    for (size_t command = index; command < std::min(index + 4, CommandCount);
         ++command) {
      if (Commands[command] == CommandType::Delete ||
          Commands[command] == CommandType::Login ||
          Commands[command] == CommandType::Create) {
        continue; // a password's hash each: slow
      }
      QByteArray empty(1, static_cast<char>(command));
      PutU32(empty, 0);
      put(input, input.empty() ? 0x81 : 1, empty);
      put(input, 1,
          QByteArray(1, static_cast<char>(command)) + com_id + empty.mid(1));
    }
    if (!input.empty())
      seeds.push_back(input);
  }
  // Not signed in: packets cut short, two in one read, a bad type.
  Bytes raw;
  const QByteArray terminate = Packet(CommandType::Terminate, {});
  put(raw, 0, Packet(CommandType::GetServerFeatures, {}).left(10));
  put(raw, 0, Packet(CommandType::GetServerFeatures, {}).mid(10) + terminate);
  put(raw, 0, QByteArray(1, '\x05') + terminate.mid(1));
  seeds.push_back(raw);
  return seeds;
}

} // namespace ShadNetFuzz
