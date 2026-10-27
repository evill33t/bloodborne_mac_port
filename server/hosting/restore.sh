#!/usr/bin/env bash
# Restore a backup made by backup.sh into DATA_DIR. shadNet MUST be stopped first
# (docker compose stop shadnet / systemctl stop shadnet). The current DATA_DIR is moved aside,
# never deleted. Partly tested (local "dir" source only); restic/rclone paths UNTESTED.
#
#   restore.sh restic [snapshot-id|latest]
#   restore.sh rclone shadnet-20261023T040000Z.tar.zst
#   restore.sh dir /path/to/stage            (a staged copy; used by the drill)
# Env: DATA_DIR=/data, RESTORE_ONLY=world:<folder>  (restore one world folder only)
set -Eeuo pipefail

DATA_DIR=${DATA_DIR:-/data}
SRC_KIND=${1:?usage: restore.sh restic|rclone|dir <what>}
WHAT=${2:-latest}
WORK=$(mktemp -d /tmp/shadnet-restore.XXXXXX)
log() { echo "$(date -u +%FT%TZ) restore: $*"; }

if pgrep -x shadnet >/dev/null 2>&1; then
    log "shadnet is running on this host; stop it first"; exit 1
fi

case $SRC_KIND in
restic)
    restic restore "$WHAT" --host shadnet --target "$WORK"
    # restic keeps the absolute path it backed up (STAGE_DIR).
    STAGE=$(dirname "$(find "$WORK" -name BACKUP_TIME -print -quit)")
    ;;
rclone)
    rclone copy "$RCLONE_REMOTE/$WHAT" "$WORK/"
    mkdir "$WORK/x" && tar -C "$WORK/x" -I zstd -xf "$WORK/$WHAT"
    STAGE=$WORK/x
    ;;
dir) STAGE=$WHAT ;;
*) echo "unknown source $SRC_KIND"; exit 1 ;;
esac
[[ -f $STAGE/BACKUP_TIME ]] || { log "no BACKUP_TIME in $STAGE"; exit 1; }
log "backup taken $(cat "$STAGE/BACKUP_TIME")"

(cd "$STAGE" && sha256sum --quiet -c MANIFEST.sha256) || { log "checksum mismatch"; exit 1; }
for db in "$STAGE"/db/*.db; do
    [[ -f $db ]] || continue
    [[ $(sqlite3 "$db" "PRAGMA integrity_check;") == ok ]] || { log "bad $db"; exit 1; }
done

# One world only: put that folder back, keep everything else as it is.
if [[ ${RESTORE_ONLY:-} == world:* ]]; then
    name=${RESTORE_ONLY#world:}
    live=$DATA_DIR/bloodborne_worlds/$name
    [[ -d $STAGE/bloodborne_worlds/$name ]] || { log "no world $name in backup"; exit 1; }
    # Aside OUTSIDE bloodborne_worlds/: shadNet would take any folder there as a world.
    if [[ -d $live ]]; then
        mkdir -p "$DATA_DIR/restore-aside"
        mv "$live" "$DATA_DIR/restore-aside/$name-$(date -u +%Y%m%dT%H%M%SZ)"
    fi
    cp -a "$STAGE/bloodborne_worlds/$name" "$live"
    log "world $name restored"
    exit 0
fi

# Old contents go to restore-aside/ inside DATA_DIR (DATA_DIR may be a volume mount point).
ASIDE=$DATA_DIR/restore-aside/full-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$ASIDE"
shopt -s dotglob
for entry in "$DATA_DIR"/*; do
    # lost+found: a volume's root (cloud block storage) is not ours to move.
    case $(basename "$entry") in restore-aside | lost+found) continue ;; esac
    mv "$entry" "$ASIDE/"
done
shopt -u dotglob
log "old data kept at $ASIDE"
# The link key is the server's identity, not its state: a backup without one (taken after it
# was lost) must not cost the key the server still has.
keep_key=""
if [[ ! -f $STAGE/link_key.bin ]]; then
    if [[ -f $ASIDE/link_key.bin ]]; then
        keep_key=$ASIDE/link_key.bin
        log "WARNING: the backup has no link_key.bin; keeping the current one"
    else
        log "WARNING: neither the backup nor the data folder has link_key.bin: shadNet will make a" \
            "new key and every game's pinned key will be refused. Restore an older snapshot that has it."
    fi
fi
# tar, not cp -a: DATA_DIR itself may be a mount point the restoring user cannot touch (a
# Kubernetes volume, restored as the server's user); --no-overwrite-dir leaves its metadata.
tar -C "$STAGE" -cf - . | tar -C "$DATA_DIR" --no-overwrite-dir -xpf -
if [[ -n $keep_key ]]; then (umask 077 && cp "$keep_key" "$DATA_DIR/link_key.bin"); fi
rm -f "$DATA_DIR/MANIFEST.sha256" "$DATA_DIR/BACKUP_TIME" "$DATA_DIR/postgres.dump"
# The sqlite copies are self-contained; drop any WAL leftovers from the old folder.
rm -f "$DATA_DIR"/db/*.db-wal "$DATA_DIR"/db/*.db-shm

if [[ -f $STAGE/postgres.dump ]]; then
    log "postgres.dump present: restore it with"
    log "  pg_restore --clean --if-exists --no-owner -d \"\$PGDATABASE\" $STAGE/postgres.dump"
fi
if [[ -n ${SHADNET_UID:-} ]]; then chown -R "$SHADNET_UID" "$DATA_DIR"; fi
log "done; start shadnet and check its log for 'world store' warnings"
