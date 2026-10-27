// SPDX-FileCopyrightText: Copyright 2019-2026 rpcsn Project
// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLocalSocket>
#include <QLoggingCategory>
#ifdef Q_OS_UNIX
#include <csignal>
#include <QSocketNotifier>
#include <sys/socket.h>
#include <unistd.h>
#endif
#ifndef SHADNET_NO_HTTP
#include "bloodborne_backend_server.h"
#endif
#include "config.h"
#include "server.h"
#include "watchdog.h"
#ifndef SHADNET_NO_HTTP
#include "webapi_server.h"
#endif

const QString versionString = QStringLiteral("0.0.13");

#ifdef Q_OS_UNIX
namespace {
int quitSockets[2] = {-1, -1};

void OnQuitSignal(int) {
    const char byte = 1;
    [[maybe_unused]] const auto written = ::write(quitSockets[1], &byte, 1);
}

// SIGTERM (docker stop, systemctl stop) and Ctrl+C end the event loop instead of the process, so
// the worlds' writes still queued reach the disk (LinkServer drains them on aboutToQuit).
void QuitOnSignals(QCoreApplication& app) {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, quitSockets) != 0)
        return;
    auto* notifier = new QSocketNotifier(quitSockets[0], QSocketNotifier::Read, &app);
    QObject::connect(notifier, &QSocketNotifier::activated, &app, [&app] {
        qInfo() << "Stopping (signal)";
        app.quit();
    });
    struct sigaction action{};
    action.sa_handler = OnQuitSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
}
} // namespace
#endif

namespace {
// `shadnet --admin <command>`: the operator's command (admin.h) to the running server's admin
// socket (HostedAdminSocket, in SHADNET_HOME), its JSON answer printed.
int RunAdminCommand(const QString& path, const QString& line) {
    QLocalSocket socket;
    socket.connectToServer(QFileInfo{path}.absoluteFilePath());
    if (!socket.waitForConnected(5000)) {
        fprintf(stderr, "no server at %s: %s\n", qPrintable(path),
                qPrintable(socket.errorString()));
        return 2;
    }
    socket.write(line.toUtf8() + '\n');
    QByteArray answer;
    while (socket.state() == QLocalSocket::ConnectedState && socket.waitForReadyRead(30000))
        answer += socket.readAll();
    answer += socket.readAll();
    fputs(answer.constData(), stdout);
    return answer.contains("\"ok\":true") ? 0 : 1;
}
} // namespace

int main(int argc, char* argv[]) {
    QLoggingCategory::setFilterRules(QStringLiteral("*.debug=false\n*.info=true\n*.warning=true"));
    qSetMessagePattern(QStringLiteral("%{time yyyy-MM-dd HH:mm:ss.zzz}  "
                                      "%{if-debug}DEBUG%{endif}"
                                      "%{if-info} INFO%{endif}"
                                      "%{if-warning} WARN%{endif}"
                                      "%{if-critical} CRIT%{endif}"
                                      "%{if-fatal}FATAL%{endif}"
                                      "  %{if-category}[%{category}] %{endif}%{message}"));

    QCoreApplication app(argc, argv);
    app.setApplicationName("shadnet");

    // Set working directory to executable location (SHADNET_HOME: another folder, for tests)
    const QString home = qEnvironmentVariable("SHADNET_HOME");
    QDir::setCurrent(home.isEmpty() ? QCoreApplication::applicationDirPath() : home);

    // Phase 11: `--watchdog` keeps shadNet running as its child (watchdog.h); the child beats.
    QStringList arguments = app.arguments().mid(1);
    if (arguments.removeAll(QStringLiteral("--watchdog")) > 0) {
        return RunWatchdog(QCoreApplication::applicationFilePath(),
                           arguments << QStringLiteral("--beat"));
    }
    if (const qsizetype at = arguments.indexOf(QStringLiteral("--admin")); at >= 0) {
        ConfigManager config;
        config.Load();
        return RunAdminCommand(config.GetHosted().adminSocket,
                               arguments.mid(at + 1).join(QLatin1Char(' ')));
    }
    if (arguments.contains(QStringLiteral("--beat"))) {
        StartWatchdogBeat(&app);
    }

    qInfo() << "ShadNet Qt server version" << versionString;
#ifdef Q_OS_UNIX
    QuitOnSignals(app);
#endif

    ConfigManager config;
    config.Load();
    config.LoadBannedDomains();

    ShadNetServer server;
    if (!server.Start(&config)) {
        qCritical() << "Failed to start server";
        return 1;
    }

#ifndef SHADNET_NO_HTTP
    // Start the HTTP/JSON WebAPI listener alongside the binary protocol.
    WebApiServer webapi;
    if (!webapi.Start(&config, "db/shadnet.db", &server.Shared())) {
        qWarning() << "WebApiServer failed to start; continuing without WebAPI";
    }

    // The game's own online services (on unless BloodborneBackend=false). When it cannot start, the
    // kits' Python backend can take its port instead.
    BloodborneBackendServer backend;
    if (config.GetBackend().enabled && !backend.Start(&config, &server.Shared().httpGate)) {
        qWarning() << "Backend failed to start; continuing without it";
    }
#else
    qWarning() << "Built without the HTTP services (Qt older than 6.8)";
#endif

    return app.exec();
}
