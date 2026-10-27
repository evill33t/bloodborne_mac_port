// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "watchdog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <QTimer>

WatchdogPolicy::Action WatchdogPolicy::Tick(qint64 now_ms, bool running,
                                            std::optional<qint64> beat_ms) {
    if (running) {
        stopped_ms.reset();
        if (!started_ms) {
            started_ms = now_ms; // running when the watchdog came (not ours to time)
        }
        // A beat from before this start is the last run's.
        const bool beat_now = beat_ms && *beat_ms >= *started_ms;
        if (!beat_now && now_ms - *started_ms > settings.first_beat_ms) {
            why = QStringLiteral("no beat in its first %1 s").arg(settings.first_beat_ms / 1000);
            return Action::Kill;
        }
        if (beat_now && now_ms - *beat_ms > settings.hang_ms) {
            why = QStringLiteral("no beat for %1 s").arg((now_ms - *beat_ms) / 1000);
            return Action::Kill;
        }
        return Action::None;
    }
    if (!stopped_ms) {
        stopped_ms = now_ms;
    }
    if (now_ms < rest_until_ms) {
        return Action::None;
    }
    while (!starts.empty() && now_ms - starts.front() > settings.crash_window_ms) {
        starts.pop_front();
    }
    if (static_cast<int>(starts.size()) >= settings.crashes_allowed) {
        why = QStringLiteral("%1 crashes within %2 s: resting %3 s")
                  .arg(starts.size())
                  .arg(settings.crash_window_ms / 1000)
                  .arg(settings.rest_ms / 1000);
        rest_until_ms = now_ms + settings.rest_ms;
        starts.clear();
        return Action::None;
    }
    // The first start at once; a start again after a short wait.
    if (started_ms && now_ms - *stopped_ms < settings.restart_after_ms) {
        return Action::None;
    }
    return Action::Start;
}

void WatchdogPolicy::Started(qint64 now_ms) {
    if (!starts.empty() || started_ms) {
        ++restarts;
    }
    starts.push_back(now_ms);
    started_ms = now_ms;
    stopped_ms.reset();
}

int RunWatchdog(const QString& program, const QStringList& arguments) {
    WatchdogPolicy dog;
    QProcess child;
    child.setProgram(program);
    child.setArguments(arguments);
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    const QString beat = QFileInfo{QString::fromLatin1(WatchdogBeatFile)}.absoluteFilePath();
    qInfo().noquote() << "Watchdog: keeps" << program << "running (beat file" << beat << ")";
    bool rested = false;
    while (true) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const bool running = child.state() != QProcess::NotRunning;
        std::optional<qint64> beat_ms;
        if (const QFileInfo info{beat}; info.exists()) {
            beat_ms = info.lastModified().toMSecsSinceEpoch();
        }
        switch (dog.Tick(now, running, beat_ms)) {
        case WatchdogPolicy::Action::Start:
            rested = false;
            if (dog.Restarts() > 0 || child.exitCode() != 0) {
                qWarning().noquote()
                    << "Watchdog: starting shadNet again (exit code" << child.exitCode() << ")";
            }
            child.start();
            if (!child.waitForStarted(10'000)) {
                qCritical().noquote() << "Watchdog: shadNet did not start:" << child.errorString();
            }
            dog.Started(QDateTime::currentMSecsSinceEpoch());
            break;
        case WatchdogPolicy::Action::Kill:
            qWarning().noquote() << "Watchdog: shadNet hung (" + dog.Why() + "): killing it";
            child.kill();
            child.waitForFinished(10'000);
            break;
        case WatchdogPolicy::Action::None:
            if (!running && !rested && dog.Why().contains(QStringLiteral("resting"))) {
                rested = true;
                qCritical().noquote() << "Watchdog: shadNet keeps crashing (" + dog.Why() + ")";
            }
            break;
        }
        child.waitForFinished(500); // returns at once when it ends, else after the tick
        if (child.state() == QProcess::NotRunning) {
            QThread::msleep(100);
        }
    }
}

void StartWatchdogBeat(QObject* parent) {
    auto* timer = new QTimer(parent);
    const auto touch = [] {
        QFile file{QString::fromLatin1(WatchdogBeatFile)};
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QByteArray::number(QDateTime::currentMSecsSinceEpoch()));
        }
    };
    QObject::connect(timer, &QTimer::timeout, parent, touch);
    touch();
    timer->start(WatchdogBeatEveryMs);
}
