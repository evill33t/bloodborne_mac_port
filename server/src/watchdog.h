// SPDX-FileCopyrightText: Copyright 2026 shadNet Project
// SPDX-License-Identifier: GPL-2.0-or-later
// Phase 11: shadNet's watchdog. `shadnet --watchdog` runs shadNet as its child and starts it again
// when it stops (a crash) or hangs: the child touches a beat file every 2 s (BeatEvery), and one
// that stays still for 15 s is killed and started again. A server that keeps crashing gets a rest
// (5 starts within 2 minutes: a minute) instead of a start loop.
#pragma once

#include <deque>
#include <optional>
#include <QString>
#include <QStringList>

class QObject;

// When to start or kill shadNet, without processes or clocks.
class WatchdogPolicy {
public:
    struct Settings {
        qint64 restart_after_ms = 1000; // after it stopped
        qint64 first_beat_ms = 30'000;  // its start (worlds, the database) before the first beat
        qint64 hang_ms = 15'000;        // no beat for this long: hung
        qint64 crash_window_ms = 120'000;
        int crashes_allowed = 5; // starts within the window before a rest
        qint64 rest_ms = 60'000;
    };
    enum class Action { None, Start, Kill };

    WatchdogPolicy() = default;
    explicit WatchdogPolicy(Settings settings) : settings{settings} {}

    // What to do now: whether shadNet runs, and when it last beat (its beat file's time).
    Action Tick(qint64 now_ms, bool running, std::optional<qint64> beat_ms);
    void Started(qint64 now_ms);
    // Starts after the first.
    int Restarts() const {
        return restarts;
    }
    // Why the last kill or rest (for the log).
    const QString& Why() const {
        return why;
    }

private:
    Settings settings;
    std::deque<qint64> starts;
    std::optional<qint64> started_ms;
    std::optional<qint64> stopped_ms;
    qint64 rest_until_ms = 0;
    int restarts = 0;
    QString why;
};

// The beat file's name, in shadNet's folder.
inline const char* WatchdogBeatFile = "watchdog.beat";
constexpr int WatchdogBeatEveryMs = 2000;

// Runs the watchdog: shadNet (`program` with `arguments`) as a child, until this process ends.
int RunWatchdog(const QString& program, const QStringList& arguments);
// In shadNet itself: touches the beat file every WatchdogBeatEveryMs while its event loop runs.
void StartWatchdogBeat(QObject* parent);
