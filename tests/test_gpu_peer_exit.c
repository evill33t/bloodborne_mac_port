// SPDX-FileCopyrightText: Copyright 2026 Ronny Stiftel
// SPDX-License-Identifier: GPL-2.0-or-later
/* Exercise the real waitpid/exit path with competing death observers, without game files
 * or a window. Each scenario runs in its own process because the runtime exits on host loss. */
#include "../src/gpu_process.c"

static void *observe_exit(void *unused) {
    (void)unused;
    gpu_process_host_exited();
    return NULL;
}

static int run_case(int host_status, int host_signal) {
    pid_t scenario = fork();
    if (scenario < 0) return 1;
    if (!scenario) {
        alarm(10);
        host_pid = fork();
        if (host_pid < 0) _exit(99);
        if (!host_pid) {
            if (host_signal) raise(host_signal);
            _exit(host_status);
        }
        alive = rendering = 1;
        pthread_t observers[4];
        for (unsigned i = 0; i < 4; ++i) {
            if (pthread_create(&observers[i], NULL, observe_exit, NULL)) _exit(98);
        }
        gpu_process_host_exited();
        _exit(97);
    }
    int status;
    if (waitpid(scenario, &status, 0) != scenario) return 1;
    const int expected = host_status || host_signal ? 1 : 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expected) {
        fprintf(stderr, "FAIL: host status %d signal %d: runtime wait status %#x (expected %d)\n",
                host_status, host_signal, status, expected);
        return 1;
    }
    return 0;
}

int main(void) {
    for (int i = 0; i < 10; ++i) {
        if (run_case(0, 0) || run_case(7, 0) || run_case(0, SIGKILL)) return 1;
    }
    puts("PASS: concurrent host-death observers preserve clean, nonzero and signalled exits (30 cases)");
    return 0;
}
