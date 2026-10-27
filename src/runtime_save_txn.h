// SPDX-FileCopyrightText: Copyright 2026 Ronny Stiftel
// SPDX-License-Identifier: GPL-2.0-or-later
/* Native save transactions and server reward receipts. No game save fields are patched.
 * SaveData mounts call mount/umount; runtime_file reports real host descriptor writes.
 * All errors are negative host errno values. */
#ifndef BB_RUNTIME_SAVE_TXN_H
#define BB_RUNTIME_SAVE_TXN_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void runtime_save_txn_configure(int enabled, const char *save_root, const char *title);
int runtime_save_txn_mount(const char *canonical, int writable, char *mounted, size_t capacity);
int runtime_save_txn_umount(const char *mounted, int successful);
/* Check before writable open/truncate/path mutation: transaction writes cannot follow a
 * preserved symlink out of their isolated stage. Non-transaction paths are allowed. */
int runtime_save_txn_write_allowed(const char *host_path);
void runtime_save_txn_file_opened(int host_fd, const char *host_path, int writable);
void runtime_save_txn_file_written(int host_fd, int64_t offset, uint64_t requested, int64_t result);
void runtime_save_txn_file_truncated(int host_fd, int64_t length, int result);
void runtime_save_txn_file_closed(int host_fd, int close_error);
/* Call after successful explicit path truncate/unlink/rename, for each mutated host path.
 * A native pre-write userdata -> backup rename is allowed; changing a proved image is not. */
void runtime_save_txn_path_mutated(const char *host_path);
/* Lookup returns 1 only after the canonical receipt and its directory are durably synced,
 * 0 when absent, and a negative error for conflict, corruption, or an unavailable save. */
int runtime_reward_lookup(const char *scope, uint64_t id, const void *payload, size_t size, int slot);
/* Begin pins the canonical save. Unarmed native writes are staged then discarded. */
int runtime_reward_begin(const char *scope, uint64_t id, const void *payload, size_t size, int slot);
/* Arm immediately before the verified fresh native character serializer is queued. */
int runtime_reward_arm(uint64_t id, int slot);
int runtime_reward_disarm(uint64_t id);
int runtime_reward_cancel_before_apply(uint64_t id);
#ifdef BB_SAVE_TXN_TESTING
/* Tests inject failures/kills at named boundaries; absent from production builds. */
void runtime_save_txn_test_fault(int (*callback)(const char *));
void runtime_save_txn_test_reset(void);
#endif
#ifdef __cplusplus
}
#endif
#endif
