#!/usr/bin/env bash
# Consistent online backup of a shadNet data folder, pushed off the machine with restic (default)
# or rclone. UNTESTED against a live server; the world-copy ordering is derived from
# src/bloodborne_world.cpp (WorldStore::Load/Append/Compact), see the comment in copy_worlds.
#
# Env (all optional except the target):
#   DATA_DIR=/data                 shadNet's SHADNET_HOME
#   STAGE_DIR=/backup/stage        scratch folder, wiped each run (same disk is fine)
#   BACKUP_TOOL=restic|rclone|none   (none: stage only, for drills and tests)
#   RESTIC_REPOSITORY, RESTIC_PASSWORD_FILE (+ AWS_ACCESS_KEY_ID/AWS_SECRET_ACCESS_KEY or B2_*)
#     e.g. s3:https://s3.eu-central-003.backblazeb2.com/my-bucket/shadnet
#          sftp:u123456@u123456.your-storagebox.de:shadnet   (Hetzner Storage Box, port 23 via ssh config)
#   RCLONE_REMOTE=b2:my-bucket/shadnet   (rclone mode: dated .tar.zst archives)
#   PGHOST/PGUSER/PGDATABASE/PGPASSWORD  set => pg_dump too (only once Postgres is in use)
#   KEEP_HOURLY=48 KEEP_DAILY=14 KEEP_WEEKLY=8 KEEP_MONTHLY=12   restic retention
#   RCLONE_KEEP_DAYS=30
#   HEALTHCHECK_URL                 pinged on success (healthchecks.io style), /fail on failure
set -Eeuo pipefail

DATA_DIR=${DATA_DIR:-/data}
STAGE_DIR=${STAGE_DIR:-/backup/stage}
BACKUP_TOOL=${BACKUP_TOOL:-restic}
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
LOCK=${STAGE_DIR%/}.lock

log() { echo "$(date -u +%FT%TZ) backup: $*"; }
fail() {
    log "FAILED: $*"
    if [[ -n ${HEALTHCHECK_URL:-} ]]; then curl -fsS -m 10 "$HEALTHCHECK_URL/fail" >/dev/null || true; fi
    exit 1
}
trap 'fail "line $LINENO"' ERR

exec 9>"$LOCK"
flock -n 9 || { log "another backup is running"; exit 0; }

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

# 1. SQLite: .backup takes a consistent copy while shadNet keeps writing (WAL mode).
backup_sqlite() {
    local src=$1 dst=$2
    [[ -f $src ]] || return 0
    mkdir -p "$(dirname "$dst")"
    sqlite3 "$src" ".timeout 10000" ".backup '$dst'"
    [[ $(sqlite3 "$dst" "PRAGMA integrity_check;") == ok ]] || fail "integrity_check $dst"
    log "sqlite $src ok ($(stat -c %s "$dst") bytes)"
}
backup_sqlite "$DATA_DIR/db/shadnet.db" "$STAGE_DIR/db/shadnet.db"
# BackendStore (content store); entrypoint.sh defaults it to db/backend.db.
for f in "$DATA_DIR"/db/*.db; do
    if [[ ! -f $f || $(basename "$f") == shadnet.db ]]; then continue; fi
    backup_sqlite "$f" "$STAGE_DIR/db/$(basename "$f")"
done

# 2. Worlds. Per world folder the order matters: journal.pb FIRST, world.pb after.
#    - world.pb/trades.pb/worlds.pb are replaced by rename (QSaveFile), so any copy is whole.
#    - journal.pb is append-only and fsynced; a copy may end in a cut record, which Load() drops.
#    - Load() skips journal records below world.journal_from. A world.pb copied after the
#      journal is never older than the journal's start, so the pair always replays to a state
#      the server really had (at worst the state at the copy of world.pb's compaction).
#    Copying world.pb first could pair an old world with a journal truncated by a compaction in
#    between, silently losing everything since the old world.
copy_worlds() {
    local src=$DATA_DIR/bloodborne_worlds dst=$STAGE_DIR/bloodborne_worlds
    [[ -d $src ]] || return 0
    mkdir -p "$dst"
    local n=0 d name
    for d in "$src"/*/; do
        [[ -d $d ]] || continue
        name=$(basename "$d")
        if [[ $name == forgotten ]]; then continue; fi
        mkdir -p "$dst/$name"
        if [[ -f $d/journal.pb ]]; then cp "$d/journal.pb" "$dst/$name/journal.pb"; fi
        # rsync exit 24 = a file vanished mid-copy (a rotated backup); harmless here.
        rsync -a --exclude=/journal.pb "$d" "$dst/$name/" || [[ $? == 24 ]]
        n=$((n + 1))
    done
    # Top-level files (worlds.pb, legacy teams.pb) and the put-aside worlds.
    rsync -a --exclude='*/' "$src/" "$dst/" || [[ $? == 24 ]]
    if [[ -d $src/forgotten ]]; then rsync -a "$src/forgotten" "$dst/" || [[ $? == 24 ]]; fi
    log "worlds: $n folders"
}
copy_worlds

# 3. Everything else small: config, score attachments, ban lists, and the server's link key
#    (LinkKeyFile: its identity, pinned by every game; a lost key breaks every player's pin).
for p in shadnet.cfg worlds.cfg scoreboards.cfg domains_banlist.txt banned.txt link_key.bin \
    score_data; do
    if [[ -e $DATA_DIR/$p ]]; then cp -a "$DATA_DIR/$p" "$STAGE_DIR/"; fi
done

if [[ ! -f $DATA_DIR/link_key.bin ]]; then
    log "WARNING: no link_key.bin in $DATA_DIR: this backup cannot give the server its key back"
fi

# 4. Postgres (future: accounts/world index). Custom format so restore can be selective.
if [[ -n ${PGHOST:-} ]]; then
    pg_dump --format=custom --file="$STAGE_DIR/postgres.dump" || fail "pg_dump"
    log "postgres dump $(stat -c %s "$STAGE_DIR/postgres.dump") bytes"
fi

(cd "$STAGE_DIR" && find . -type f ! -name MANIFEST.sha256 -print0 | sort -z \
    | xargs -0 sha256sum > MANIFEST.sha256)
echo "$STAMP" > "$STAGE_DIR/BACKUP_TIME"

# 5. Off the machine.
case $BACKUP_TOOL in
restic)
    restic snapshots >/dev/null 2>&1 || restic init
    restic backup --host shadnet --tag shadnet "$STAGE_DIR"
    restic forget --host shadnet --tag shadnet --prune \
        --keep-hourly "${KEEP_HOURLY:-48}" --keep-daily "${KEEP_DAILY:-14}" \
        --keep-weekly "${KEEP_WEEKLY:-8}" --keep-monthly "${KEEP_MONTHLY:-12}"
    # Read back a slice of the data once a day (Sunday: 10%).
    if [[ $(date -u +%H) == 04 ]]; then
        restic check --read-data-subset="$([[ $(date -u +%u) == 7 ]] && echo 10% || echo 1%)"
    fi
    ;;
rclone)
    archive=/tmp/shadnet-$STAMP.tar.zst
    tar -C "$STAGE_DIR" -I 'zstd -10 -T0' -cf "$archive" .
    rclone copy "$archive" "$RCLONE_REMOTE/"
    rclone delete --min-age "${RCLONE_KEEP_DAYS:-30}d" "$RCLONE_REMOTE/"
    rm -f "$archive"
    ;;
none) log "BACKUP_TOOL=none: staged only, nothing uploaded (drills/tests)" ;;
*) fail "unknown BACKUP_TOOL=$BACKUP_TOOL" ;;
esac

log "done $STAMP ($(du -sh "$STAGE_DIR" | cut -f1) staged)"
if [[ -n ${HEALTHCHECK_URL:-} ]]; then curl -fsS -m 10 "$HEALTHCHECK_URL" >/dev/null || true; fi
