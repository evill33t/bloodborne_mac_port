// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Phase 11 (Kaniel 2026-10-15): no account to register. A player's id (the
// launcher makes it once) signs in, and the server makes its account the first
// time it sees it; after that the id's secret has to match, so nobody else
// signs in as that player.
#include <iostream>

#include <QCoreApplication>
#include <QSqlQuery>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>

#include "database.h"
#include "virtual_address.h"

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

int AFirstSignInMakesTheAccount(const QString &folder) {
  Database db("first");
  CHECK(db.Open(folder + "/first.db"));
  CHECK(db.Migrate());
  CHECK(!db.CheckUser("p0123456789abcd", "secret0123456789", {}, false));
  const auto made =
      db.SignIn("p0123456789abcd", "secret0123456789", {}, false, true);
  CHECK(made.has_value());
  CHECK(made->username == "p0123456789abcd");
  const auto again =
      db.SignIn("p0123456789abcd", "secret0123456789", {}, false, true);
  CHECK(again.has_value());
  CHECK(again->userId == made->userId);
  // Another secret is another player claiming this id: refused, not a second
  // account.
  CHECK(!db.SignIn("p0123456789abcd", "not-the-secret", {}, false, true));
  CHECK(db.TotalUsers() == 1);
  // Another case of the id is not this account (the link's hello takes the
  // id's bytes as they are: its game's address), nor a new one.
  CHECK(!db.SignIn("P0123456789ABCD", "secret0123456789", {}, false, true));
  CHECK(!db.SignIn("p0123456789abcD", "secret0123456789", {}, false, true));
  CHECK(db.TotalUsers() == 1);
  return 0;
}

int WithoutMakingAnUnknownIdIsRefused(const QString &folder) {
  Database db("off");
  CHECK(db.Open(folder + "/off.db"));
  CHECK(db.Migrate());
  CHECK(!db.SignIn("p0123456789abcd", "secret0123456789", {}, false, false));
  CHECK(db.TotalUsers() == 0);
  // An id the game cannot have is never made.
  CHECK(!db.SignIn("a b", "secret", {}, false, true));
  CHECK(!db.SignIn("p0123456789abcd", "", {}, false, true));
  CHECK(db.TotalUsers() == 0);
  return 0;
}

int AnOldAccountSignsInAsBefore(const QString &folder) {
  Database db("old");
  CHECK(db.Open(folder + "/old.db"));
  CHECK(db.Migrate());
  CHECK(!db.CreateAccount("kaniel", "pw", "avatar.png", "kaniel.pw@p2p.db"));
  CHECK(db.SignIn("kaniel", "pw", {}, false, true).has_value());
  CHECK(!db.SignIn("kaniel", "other", {}, false, true));
  CHECK(db.TotalUsers() == 1);
  return 0;
}

// Two ids whose games get the same virtual address (virtual_address.h): the
// second could never link while the first is linked, or could squat it.
const QString Holder = "p000000000048cf";
const QString Collider = "p00000000005b99";

QVariant AddressOf(Database &db, const QString &npid) {
  QSqlQuery q(db.Conn());
  q.prepare("SELECT virtual_address FROM account WHERE username=?");
  q.addBindValue(npid);
  return q.exec() && q.next() ? q.value(0) : QVariant{};
}

int ANewIdOnAnotherAccountsAddressIsRefused(const QString &folder) {
  CHECK(VirtualAddressOf(Holder.toUtf8()) ==
        VirtualAddressOf(Collider.toUtf8()));
  Database db("address");
  CHECK(db.Open(folder + "/address.db"));
  CHECK(db.SignIn(Holder, "secret0123456789", {}, false, true).has_value());
  CHECK(AddressOf(db, Holder).toLongLong() ==
        VirtualAddressOf(Holder.toUtf8()));
  DbError why = DbError::None;
  CHECK(!db.SignIn(Collider, "secret0123456789", {}, false, true, &why));
  CHECK(why == DbError::ExistingAddress);
  CHECK(db.CreateAccount(Collider, "pw", "avatar.png", "c.pw@p2p.db") ==
        DbError::ExistingAddress);
  CHECK(db.TotalUsers() == 1);
  // A wrong password is not an address taken.
  why = DbError::None;
  CHECK(!db.SignIn(Holder, "not-the-secret", {}, false, true, &why));
  CHECK(why != DbError::ExistingAddress);
  // Once the holder's account is gone its address is free.
  CHECK(db.DeleteUser(*db.GetUserId(Holder)));
  CHECK(db.SignIn(Collider, "secret0123456789", {}, false, true).has_value());
  return 0;
}

// A database from before the addresses were stored: each account gets its
// address, and two accounts that already share one both keep signing in (the
// later one has none stored).
int AnOldDatabaseGetsItsAddresses(const QString &folder) {
  const QString path = folder + "/old_addresses.db";
  {
    Database db("before");
    CHECK(db.Open(path));
    CHECK(!db.CreateAccount(Holder, "pw1", "avatar.png", "h.pw@p2p.db"));
    CHECK(!db.CreateAccount("kaniel", "pw2", "avatar.png", "k.pw@p2p.db"));
    CHECK(!db.CreateAccount("later", "pw3", "avatar.png", "l.pw@p2p.db"));
    QSqlQuery q(db.Conn());
    CHECK(q.exec("DROP INDEX account_virtual_address"));
    CHECK(q.exec("ALTER TABLE account DROP COLUMN virtual_address"));
    CHECK(q.exec("DELETE FROM migration WHERE migration_id=3"));
    CHECK(q.exec("UPDATE account SET username='" + Collider +
                 "' WHERE username='later'"));
  }
  Database db("after");
  CHECK(db.Open(path));
  CHECK(AddressOf(db, Holder).toLongLong() ==
        VirtualAddressOf(Holder.toUtf8()));
  CHECK(AddressOf(db, "kaniel").toLongLong() == VirtualAddressOf("kaniel"));
  CHECK(AddressOf(db, Collider).isNull());
  CHECK(db.SignIn(Holder, "pw1", {}, false, true).has_value());
  CHECK(db.SignIn(Collider, "pw3", {}, false, true).has_value());
  CHECK(db.SignIn("kaniel", "pw2", {}, false, true).has_value());
  // Migrating again changes nothing; once the holder is gone the later
  // account gets the address.
  CHECK(db.Migrate());
  CHECK(AddressOf(db, Collider).isNull());
  CHECK(db.DeleteUser(*db.GetUserId(Holder)));
  CHECK(AddressOf(db, Collider).toLongLong() ==
        VirtualAddressOf(Collider.toUtf8()));
  return 0;
}

QStringList warnings;

void Collect(QtMsgType type, const QMessageLogContext &, const QString &text) {
  if (type == QtWarningMsg) {
    warnings.append(text);
  }
}

// An account without an address on purpose (it shares an older one's) is
// looked at once, when the addresses are first stored: each connection opened
// later (every session, every sign-in thread) neither warns again nor takes the
// write lock for it.
int AnAccountWithoutItsAddressIsNotLookedAtOnEveryOpen(const QString &folder) {
  const QString path = folder + "/quiet.db";
  {
    Database db("quiet_before");
    CHECK(db.Open(path));
    CHECK(!db.CreateAccount(Holder, "pw1", "avatar.png", "h.pw@p2p.db"));
    CHECK(!db.CreateAccount("later", "pw3", "avatar.png", "l.pw@p2p.db"));
    QSqlQuery q(db.Conn());
    CHECK(q.exec("DROP INDEX account_virtual_address"));
    CHECK(q.exec("ALTER TABLE account DROP COLUMN virtual_address"));
    CHECK(q.exec("DELETE FROM migration WHERE migration_id=3"));
    CHECK(q.exec("UPDATE account SET username='" + Collider +
                 "' WHERE username='later'"));
  }
  {
    Database db("quiet_migrated");
    CHECK(db.Open(path));
    CHECK(AddressOf(db, Collider).isNull());
  }
  warnings.clear();
  const QtMessageHandler previous = qInstallMessageHandler(Collect);
  bool quiet = true;
  for (int i = 0; i < 3; ++i) {
    Database db(QStringLiteral("quiet_%1").arg(i));
    quiet = db.Open(path) && quiet;
    quiet = db.SignIn(Collider, "pw3", {}, false, true).has_value() && quiet;
  }
  qInstallMessageHandler(previous);
  CHECK(quiet);
  for (const QString &warning : warnings) {
    std::cerr << "warned: " << warning.toStdString() << '\n';
  }
  CHECK(warnings.isEmpty());
  return 0;
}

// The addresses not stored because their transaction did not begin (another
// connection writing past the busy timeout; here, one already open on this
// connection, which fails the same BEGIN IMMEDIATE): not marked done either,
// so the next migration stores them.
int AddressesNotStoredForABusyDatabaseAreStoredLater(const QString &folder) {
  Database db("busy");
  CHECK(db.Open(folder + "/busy.db"));
  CHECK(!db.CreateAccount("kaniel", "pw2", "avatar.png", "k.pw@p2p.db"));
  QSqlQuery q(db.Conn());
  CHECK(q.exec("UPDATE account SET virtual_address=NULL"));
  CHECK(q.exec("DELETE FROM migration WHERE migration_id=3"));
  CHECK(q.exec("BEGIN"));
  db.Migrate();
  CHECK(q.exec("COMMIT"));
  CHECK(AddressOf(db, "kaniel").isNull());
  CHECK(q.exec("SELECT 1 FROM migration WHERE migration_id=3"));
  CHECK(!q.next()); // not done
  CHECK(db.Migrate());
  CHECK(AddressOf(db, "kaniel").toLongLong() == VirtualAddressOf("kaniel"));
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir folder;
  if (!folder.isValid()) {
    std::cerr << "no temporary folder\n";
    return 1;
  }
  for (const auto test :
       {AFirstSignInMakesTheAccount, WithoutMakingAnUnknownIdIsRefused,
        AnOldAccountSignsInAsBefore, ANewIdOnAnotherAccountsAddressIsRefused,
        AnOldDatabaseGetsItsAddresses,
        AnAccountWithoutItsAddressIsNotLookedAtOnEveryOpen,
        AddressesNotStoredForABusyDatabaseAreStoredLater}) {
    if (const int failed = test(folder.path())) {
      return failed;
    }
  }
  std::cout << "accounts: all passed\n";
  return 0;
}
