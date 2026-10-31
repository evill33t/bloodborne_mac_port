// SPDX-FileCopyrightText: Copyright 2026 CLS_AMG (Amanises)
// SPDX-License-Identifier: GPL-2.0-or-later
// The lobby's friends in shadNet's database (lobby_friends_db.h): the search
// and the friend rules against a real database file, as the server runs them.
#include <iostream>

#include <QCoreApplication>
#include <QTemporaryDir>

#include "client_session.h"
#include "lobby_friends_db.h"

// The notifications go nowhere here (no connections): only the link needs it.
QByteArray ClientSession::BuildNotification(NotificationType,
                                            const QByteArray &payload) {
  return payload;
}

namespace {

bool Check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
  }
  return condition;
}

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!Check((expression), #expression, __LINE__))                           \
      return 1;                                                                \
  } while (false)

using BloodborneWorld::DatabaseFriendBook;

std::string M(const char *npid) {
  return DatabaseFriendBook::MemberOf(QString::fromLatin1(npid));
}

int Run() {
  QTemporaryDir dir;
  CHECK(dir.isValid());
  Database db{"friends_test"};
  CHECK(db.Open(dir.filePath("shadnet.db")));
  for (const char *npid :
       {"CLS_AMG", "Kaniel", "Supermedo", "kanie_2", "100%_hunter"}) {
    const auto error = db.CreateAccount(
        QString::fromLatin1(npid), "secret-password", QStringLiteral(""),
        QString::fromLatin1(npid) + "@test.local");
    if (error) {
      std::cerr << npid << ": account error " << static_cast<int>(*error) << ' '
                << db.lastError().toStdString() << '\n';
    }
    CHECK(!error);
  }
  DatabaseFriendBook book{db, nullptr};

  CHECK(DatabaseFriendBook::NpidOf(M("CLS_AMG")) == "CLS_AMG");
  // The search: any case, names starting with the text first, LIKE's characters
  // as themselves.
  auto found = book.Search(M("CLS_AMG"), "KANIEL", 20);
  CHECK(found.size() == 1 && found[0].name == "Kaniel" &&
        found[0].member == M("Kaniel"));
  found = book.Search(M("CLS_AMG"), "medo", 20);
  CHECK(found.size() == 1 && found[0].name == "Supermedo");
  found = book.Search(M("CLS_AMG"), "an", 20);
  CHECK(found.size() == 2); // Kaniel, kanie_2 (none starts with it: by name)
  found = book.Search(M("CLS_AMG"), "e_", 20);
  CHECK(found.size() == 1 && found[0].name == "kanie_2");
  found = book.Search(M("CLS_AMG"), "100%", 20);
  CHECK(found.size() == 1 && found[0].name == "100%_hunter");
  CHECK(book.Search(M("CLS_AMG"), "a", 1).size() == 1);

  // A request, then the answer: friends both ways.
  CHECK(book.Add(M("CLS_AMG"), M("Nobody")) == "no such player");
  CHECK(book.Add(M("CLS_AMG"), M("CLS_AMG")) == "that is you");
  CHECK(book.Add(M("CLS_AMG"), M("Kaniel")).empty());
  CHECK(book.Add(M("CLS_AMG"), M("Kaniel")) == "request already sent");
  auto mine = book.Of(M("CLS_AMG"));
  auto theirs = book.Of(M("Kaniel"));
  CHECK(mine.sent.size() == 1 && mine.sent[0].name == "Kaniel" &&
        mine.friends.empty());
  CHECK(theirs.received.size() == 1 &&
        theirs.received[0].member == M("CLS_AMG"));
  CHECK(book.Add(M("Kaniel"), M("CLS_AMG")).empty());
  CHECK(book.Of(M("CLS_AMG")).friends.size() == 1 &&
        book.Of(M("Kaniel")).friends.size() == 1);
  CHECK(book.Add(M("CLS_AMG"), M("Kaniel")) == "already friends");
  // The game's own lists read the same table (cmd_friend.cpp's rows).
  const auto me = db.GetUserId("CLS_AMG");
  CHECK(me && db.GetRelationships(*me).friends.size() == 1);

  // Declined: gone for both; taken back: gone too.
  CHECK(book.Add(M("Supermedo"), M("CLS_AMG")).empty());
  CHECK(book.Decline(M("CLS_AMG"), M("Supermedo")).empty());
  CHECK(book.Of(M("Supermedo")).sent.empty() &&
        book.Of(M("CLS_AMG")).received.empty());
  CHECK(book.Decline(M("CLS_AMG"), M("Supermedo")) ==
        "no request from this player");
  CHECK(book.Add(M("CLS_AMG"), M("Supermedo")).empty());
  CHECK(book.Remove(M("CLS_AMG"), M("Supermedo")).empty());
  CHECK(book.Of(M("Supermedo")).received.empty());

  // Removed: no longer friends on either side.
  CHECK(book.Remove(M("Kaniel"), M("CLS_AMG")).empty());
  CHECK(book.Of(M("CLS_AMG")).friends.empty() &&
        book.Of(M("Kaniel")).friends.empty());
  CHECK(book.Remove(M("Kaniel"), M("CLS_AMG")) == "not a friend");

  // A block (the game's AddBlock) refuses a request either way.
  const auto super = db.GetUserId("Supermedo");
  CHECK(super &&
        db.SetRelStatus(*super, *me,
                        static_cast<uint8_t>(FriendStatus::Blocked), 0));
  CHECK(book.Add(M("CLS_AMG"), M("Supermedo")) ==
        "this player cannot be added");
  std::cout << "lobby friends db: all checks passed\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  QCoreApplication app{argc, argv};
  return Run();
}
