// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The config (config.h): `shadnet --admin` reads it beside a running server and must never write
// it. The Docker smoke test's first `--admin stats` ran before the entrypoint wrote its hosted
// shadnet.cfg: it made one with every default (Hosted=false), the entrypoint then kept it, and the
// server started without its admin socket (and without its hosted limits).
#include <iostream>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "config.h"

namespace {

bool Check(bool condition, const char* expression, int line) {
    if (!condition)
        std::cerr << "check failed at line " << line << ": " << expression << '\n';
    return condition;
}

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!Check((expression), #expression, __LINE__))                                           \
            return 1;                                                                              \
    } while (false)

QByteArray ReadAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray{};
}

int ReadOnlyLoadNeverMakesAMissingFile() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString path = dir.filePath("shadnet.cfg");
    ConfigManager config;
    config.LoadReadOnly(path);
    CHECK(!QFile::exists(path));
    // Its defaults still hold: the admin command looks for admin.sock.
    CHECK(config.GetHosted().adminSocket == QStringLiteral("admin.sock"));
    CHECK(!config.GetHosted().enabled);
    std::cout << "config: a read-only load never makes a missing file\n";
    return 0;
}

int ReadOnlyLoadReadsAndKeepsTheFile() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString path = dir.filePath("shadnet.cfg");
    const QByteArray text = "[General]\nHosted=true\nHostedAdminSocket=ops.sock\n";
    {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(text);
    }
    ConfigManager config;
    config.LoadReadOnly(path);
    CHECK(config.GetHosted().enabled);
    CHECK(config.GetHosted().adminSocket == QStringLiteral("ops.sock"));
    CHECK(ReadAll(path) == text); // no defaults added
    std::cout << "config: a read-only load reads the file and leaves it as it is\n";
    return 0;
}

int LoadStillWritesTheDefaults() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    const QString path = dir.filePath("shadnet.cfg");
    ConfigManager config;
    config.Load(path);
    CHECK(QFile::exists(path));
    CHECK(ReadAll(path).contains("HostedAdminSocket=admin.sock"));
    std::cout << "config: the server's load still writes every default\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    int failed = 0;
    failed += ReadOnlyLoadNeverMakesAMissingFile();
    failed += ReadOnlyLoadReadsAndKeepsTheFile();
    failed += LoadStillWritesTheDefaults();
    if (failed)
        std::cerr << failed << " config check(s) failed\n";
    return failed ? 1 : 0;
}
