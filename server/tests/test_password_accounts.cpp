// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Global servers' own accounts: a name and a password (password_accounts.h).
// The hash, the rules for names and passwords, the throttle, the recovery code
// and the signup work.
#include <chrono>
#include <iostream>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "password_accounts.h"
#include "signed_accounts.h"

#define CHECK(condition)                                                       \
  if (!(condition)) {                                                          \
    std::cerr << "FAILED " << __LINE__ << ": " #condition "\n";                \
    return 1;                                                                  \
  }

using namespace PasswordAccounts;

namespace {

// Small for the tests; the server's own are HashParams{} (about 100-150 ms).
const HashParams Quick{1024, 1};
constexpr qint64 Now = 1'800'000'000;
const QByteArray Key = QByteArray(32, 'k');

int AHashTellsItsPassword() {
  const QString hash = Hash("correct horse battery", Quick);
  CHECK(hash.startsWith("$argon2id$v=19$m=1024,t=1,p=1$"));
  CHECK(!hash.contains("correct"));
  CHECK(Verify("correct horse battery", hash));
  CHECK(!Verify("correct horse batterY", hash));
  CHECK(!Verify("", hash));
  // Each its own salt.
  CHECK(Hash("correct horse battery", Quick) != hash);
  // Never a stored text that is not ours.
  CHECK(!Verify("x", ""));
  CHECK(!Verify("x", "$argon2id$v=19$m=1024,t=1,p=1$$"));
  CHECK(
      !Verify("x", "$argon2i$v=19$m=1024,t=1,p=1$AAAAAAAAAAAAAAAAAAAAAA$AAAA"));
  CHECK(!Verify("x", "$argon2id$v=19$m=99999999,t=1,p=1$AAAAAAAAAAAAAAAAAAAAAA$"
                     "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
  return 0;
}

int ChangedParametersRehash() {
  const QString hash = Hash("correct horse battery", Quick);
  CHECK(!NeedsRehash(hash, Quick));
  CHECK(NeedsRehash(hash, HashParams{2048, 1}));
  CHECK(NeedsRehash(hash, HashParams{1024, 2}));
  CHECK(NeedsRehash("$argon2id$v=19$garbage", Quick));
  return 0;
}

// The server's own: about 100-150 ms on a 4-vCPU box, never under 64 MiB-ish
// strength.
int TheServersOwnIsStrong() {
  const HashParams own;
  CHECK(own.memory_kib >= 19 * 1024);
  CHECK(own.passes >= 2);
  const auto start = std::chrono::steady_clock::now();
  const QString hash = Hash("correct horse battery", own);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  std::cout << "Argon2id " << own.memory_kib << " KiB x" << own.passes << ": "
            << ms << " ms\n";
  CHECK(Verify("correct horse battery", hash));
  return 0;
}

int NamesHaveRules() {
  CHECK(CheckName("Kaniel") == NameProblem::None);
  CHECK(CheckName("a.b-c_d9") == NameProblem::None);
  CHECK(CheckName("abc") == NameProblem::None);
  CHECK(CheckName("abcdefghijklmnopqrst") == NameProblem::None); // 20
  CHECK(CheckName("ab") == NameProblem::Length);
  CHECK(CheckName("abcdefghijklmnopqrstu") == NameProblem::Length); // 21
  CHECK(CheckName("Kan iel") == NameProblem::Characters);
  CHECK(CheckName("Kaniél") == NameProblem::Characters);
  CHECK(CheckName("Kaniel!") == NameProblem::Characters);
  CHECK(CheckName("") == NameProblem::Length);
  // One account a name, whatever its case.
  CHECK(NameKey("KaNiEl") == NameKey("kaniel"));
  CHECK(NameKey("KaNiEl") == "kaniel");
  return 0;
}

int PasswordsHaveRules() {
  CommonPasswords common;
  CHECK(CheckPassword("long enough", "kaniel", common) ==
        PasswordProblem::None);
  CHECK(CheckPassword("123456789", "kaniel", common) ==
        PasswordProblem::TooShort);
  CHECK(CheckPassword(QString(129, 'x'), "kaniel", common) ==
        PasswordProblem::TooLong);
  CHECK(CheckPassword(QString(128, 'x'), "kaniel", common) ==
        PasswordProblem::None);
  // No rules for what it is made of: ten of anything.
  CHECK(CheckPassword("aaaaaaaaaa", "kaniel", common) == PasswordProblem::None);
  CHECK(CheckPassword("пароль-для-игры", "kaniel", common) ==
        PasswordProblem::None);
  // Characters, not bytes.
  CHECK(CheckPassword("ééééééééé", "kaniel", common) ==
        PasswordProblem::TooShort);
  CHECK(CheckPassword("my KANIEL pass", "kaniel", common) ==
        PasswordProblem::HasName);
  // The built-in common ones, any case.
  CHECK(CheckPassword("password123", "kaniel", common) ==
        PasswordProblem::Common);
  CHECK(CheckPassword("PASSWORD123", "kaniel", common) ==
        PasswordProblem::Common);
  CHECK(CheckPassword("1234567890", "kaniel", common) ==
        PasswordProblem::Common);
  CHECK(CheckPassword("qwertyuiop", "kaniel", common) ==
        PasswordProblem::Common);
  return 0;
}

// The list shipped with the server: plain lines, read as data, any case.
int TheListIsReadAsData() {
  QTemporaryDir dir;
  QFile file(dir.filePath("common.txt"));
  CHECK(file.open(QIODevice::WriteOnly));
  file.write("Bloodborne7x\r\n  \n#not a comment, a "
             "password\nyharnam-hunter\n\xff\xfe\n");
  file.close();
  CommonPasswords common;
  const int before = common.Size();
  CHECK(common.LoadFile(file.fileName()));
  CHECK(common.Size() == before + 3);
  CHECK(common.Contains("bloodborne7x"));
  CHECK(common.Contains("BLOODBORNE7X"));
  CHECK(common.Contains("YHARNAM-HUNTER"));
  CHECK(common.Contains("#not a comment, a password"));
  CHECK(!common.Contains(""));
  CHECK(!common.LoadFile(dir.filePath("none.txt")));
  return 0;
}

// The one shipped with the server (data/common-passwords.txt, from the source
// tree).
int TheShippedListRefusesTheCommonOnes() {
  CommonPasswords common;
  const QString path = QStringLiteral(PASSWORDS_FILE);
  CHECK(common.LoadFile(path));
  std::cout << "common passwords: " << common.Size() << "\n";
  CHECK(common.Contains("password123"));
  CHECK(CheckPassword("password123", "kaniel", common) ==
        PasswordProblem::Common);
  CHECK(CheckPassword("not in any list, surely", "kaniel", common) ==
        PasswordProblem::None);
  return 0;
}

int ANameIsAnOnlineId() {
  const QString id = OnlineId(Key, "kaniel");
  // The launcher's and the game's ids: [A-Za-z0-9_-]{3,16}.
  CHECK(id.size() == 15 && id.startsWith('u'));
  for (const QChar c : id.mid(1)) {
    CHECK(QStringLiteral("0123456789abcdef").contains(c));
  }
  CHECK(id == OnlineId(Key, "kaniel"));
  CHECK(id != OnlineId(Key, "kaniel", 1)); // the next try for a free address
  CHECK(id != OnlineId(QByteArray(32, 'j'), "kaniel"));
  CHECK(id != OnlineId(Key, "kanie1"));
  CHECK(SignedAccounts::IsSignedId(id.toLatin1()));
  CHECK(!SignedAccounts::IsSignedId(
      "d0123456789abcd")); // Sign in with Discord's, gone
  CHECK(!SignedAccounts::IsSignedId("p0123456789abcd"));
  CHECK(!SignedAccounts::IsSignedId("u0123456789abc"));
  return 0;
}

int ASignInsSecretIsToldOnAnyShard() {
  const std::array<quint8, 32> link{4, 5, 6};
  const QByteArray key = SignedAccounts::KeyFrom(link);
  const QByteArray id = OnlineId(key, "kaniel").toLatin1();
  const QByteArray secret =
      SignedAccounts::Secret(key, QString::fromLatin1(id), Now).toLatin1();
  // Another shard: the same link key, its own copy of it, nothing asked of the
  // first.
  const QByteArray elsewhere = SignedAccounts::KeyFrom(link);
  CHECK(SignedAccounts::Check(elsewhere, id, secret, Now + 5, 0) ==
        SignedAccounts::Verdict::Good);
  // A sign out, a password change or a recovery there (its NotBefore) kills it.
  CHECK(SignedAccounts::Check(elsewhere, id, secret, Now + 5, Now + 1) ==
        SignedAccounts::Verdict::SignedOut);
  CHECK(SignedAccounts::Check(elsewhere, id, secret, Now + 31 * 86400, 0) ==
        SignedAccounts::Verdict::Expired);
  return 0;
}

int ARecoveryCodeIsOnce() {
  const QString code = NewRecoveryCode();
  // 5 groups of 4, Crockford base32: 100 bits.
  CHECK(code.size() == 24 && code.count('-') == 4);
  CHECK(code != NewRecoveryCode());
  const QString stored = RecoveryDigest(Key, code);
  CHECK(stored.size() == 64 && !stored.contains(code));
  // As a player types it: any case, no dashes, spaces, o for 0, l and i for 1.
  QString typed = code.toLower();
  typed.remove('-');
  CHECK(RecoveryDigest(Key, typed) == stored);
  CHECK(RecoveryDigest(Key, " " + code + " ") == stored);
  CHECK(RecoveryDigest(Key, code.left(23)) != stored);
  CHECK(RecoveryDigest(Key, "") == QString());
  CHECK(RecoveryDigest(Key, " - ") == QString());
  CHECK(RecoveryDigest(QByteArray(32, 'j'), code) != stored);
  CHECK(RecoveryDigest(Key, "0000-1111") == RecoveryDigest(Key, "oooo-llii"));
  return 0;
}

// Growing waits, never a lock: an account and an address alike.
int TriesSlowDownAndNeverLock() {
  Throttle throttle;
  qint64 now = Now * 1000;
  for (int i = 0; i < Throttle::FreeTries; ++i) {
    CHECK(throttle.WaitMs("kaniel", now) == 0);
    throttle.Failed("kaniel", now);
  }
  const qint64 first = throttle.WaitMs("kaniel", now);
  CHECK(first > 0);
  CHECK(throttle.WaitMs("someone-else", now) == 0);
  now += first;
  CHECK(throttle.WaitMs("kaniel", now) == 0); // never locked: the wait ends
  throttle.Failed("kaniel", now);
  const qint64 second = throttle.WaitMs("kaniel", now);
  CHECK(second > first); // and grows
  for (int i = 0; i < 40; ++i) {
    now += throttle.WaitMs("kaniel", now);
    throttle.Failed("kaniel", now);
  }
  CHECK(throttle.WaitMs("kaniel", now) <= Throttle::MaxWaitMs);
  CHECK(throttle.WaitMs("kaniel", now) > 0);
  // The right password: from the start again.
  now += throttle.WaitMs("kaniel", now);
  throttle.Succeeded("kaniel");
  CHECK(throttle.WaitMs("kaniel", now) == 0);
  throttle.Failed("kaniel", now);
  CHECK(throttle.WaitMs("kaniel", now) == 0);
  // A day without a wrong one: forgotten.
  for (int i = 0; i < 20; ++i) {
    throttle.Failed("x", now);
  }
  CHECK(throttle.WaitMs("x", now) > 0);
  CHECK(throttle.WaitMs("x", now + Throttle::ForgetMs + Throttle::MaxWaitMs) ==
        0);
  // Many names: bounded.
  for (int i = 0; i < 300'000; ++i) {
    throttle.Failed(QString::number(i), now);
  }
  CHECK(throttle.Size() <= Throttle::MaxKeys);
  // An address: more free tries, a shorter longest wait.
  Throttle address{Throttle::AddressFreeTries, Throttle::AddressMaxWaitMs};
  for (int i = 0; i < Throttle::AddressFreeTries - 1; ++i) {
    address.Failed("203.0.113.9", now);
  }
  CHECK(address.WaitMs("203.0.113.9", now) == 0);
  for (int i = 0; i < 40; ++i) {
    address.Failed("203.0.113.9", now);
  }
  CHECK(address.WaitMs("203.0.113.9", now) == Throttle::AddressMaxWaitMs);
  return 0;
}

int ASignupDoesItsWork() {
  const QString challenge = NewChallenge(Key, Now);
  CHECK(ChallengeFresh(Key, challenge, Now + 10));
  CHECK(!ChallengeFresh(Key, challenge, Now + ChallengeSeconds + 1));
  CHECK(!ChallengeFresh(QByteArray(32, 'x'), challenge, Now));
  CHECK(!ChallengeFresh(Key, challenge.left(challenge.size() - 1) + "0", Now) ||
        challenge.endsWith('0'));
  CHECK(!ChallengeFresh(Key, "", Now));
  const int bits = 12;
  quint64 nonce = 0;
  while (!WorkDone(challenge, "kaniel", nonce, bits)) {
    ++nonce;
  }
  CHECK(WorkDone(challenge, "kaniel", nonce, bits));
  // Another name or challenge: its own work (one in 2^12 does it by luck).
  int luck = 0;
  for (quint64 n = 0; n < 64; ++n) {
    luck += WorkDone(challenge, "kaniel2", nonce + n, bits) ? 1 : 0;
  }
  CHECK(luck <= 2);
  CHECK(WorkDone(challenge, "kaniel", 12345, 0));
  CHECK(!WorkDone(challenge, "kaniel", nonce, 41));
  return 0;
}

} // namespace

int main() {
  int failed = 0;
  failed += AHashTellsItsPassword();
  failed += ChangedParametersRehash();
  failed += TheServersOwnIsStrong();
  failed += NamesHaveRules();
  failed += PasswordsHaveRules();
  failed += TheListIsReadAsData();
  failed += TheShippedListRefusesTheCommonOnes();
  failed += ANameIsAnOnlineId();
  failed += ASignInsSecretIsToldOnAnyShard();
  failed += ARecoveryCodeIsOnce();
  failed += TriesSlowDownAndNeverLock();
  failed += ASignupDoesItsWork();
  if (failed == 0) {
    std::cout << "password accounts: all passed\n";
  }
  return failed == 0 ? 0 : 1;
}
