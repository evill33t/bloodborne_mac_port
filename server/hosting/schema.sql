-- Draft Postgres schema for a hosted shadNet (PostgreSQL 16). NOT used by shadNet yet: today
-- accounts live in SQLite (src/database.cpp). Loads cleanly on PG 16; nothing reads it.
-- What is deliberately NOT here: the world log itself (journal/world.pb stay files, one writer
-- per world), presence and rate-limit counters (memory, or Redis at multi-node scale).

CREATE EXTENSION IF NOT EXISTS citext;

-- Mirrors account + account_timestamp. citext replaces SQLite's UNIQUE(username COLLATE NOCASE).
CREATE TABLE account (
    user_id      bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,  -- was AUTOINCREMENT
    username     citext NOT NULL UNIQUE,
    hash         bytea  NOT NULL,                                   -- was BLOB
    salt         bytea  NOT NULL,
    avatar_url   text   NOT NULL DEFAULT '',
    email        text   NOT NULL,
    email_check  text   NOT NULL UNIQUE,
    token        text   NOT NULL,
    reset_token  text,
    admin        boolean NOT NULL DEFAULT false,
    stat_agent   boolean NOT NULL DEFAULT false,
    banned       boolean NOT NULL DEFAULT false,                    -- kept for parity; see ban
    created_at   timestamptz NOT NULL DEFAULT now(),                -- was UNSIGNED INTEGER epoch
    last_login   timestamptz,
    token_last_sent timestamptz,
    reset_emit   timestamptz
);

CREATE TABLE friendship (
    user_id_1     bigint NOT NULL REFERENCES account ON DELETE CASCADE,
    user_id_2     bigint NOT NULL REFERENCES account ON DELETE CASCADE,
    status_user_1 smallint NOT NULL DEFAULT 0,
    status_user_2 smallint NOT NULL DEFAULT 0,
    PRIMARY KEY (user_id_1, user_id_2),
    CHECK (user_id_1 < user_id_2)
);
CREATE INDEX friendship_user2 ON friendship (user_id_2);

-- Sign-in sessions (web API / game). Store a hash of the token, never the token.
CREATE TABLE user_session (
    session_id   uuid PRIMARY KEY,
    user_id      bigint NOT NULL REFERENCES account ON DELETE CASCADE,
    token_sha256 bytea NOT NULL UNIQUE,
    created_at   timestamptz NOT NULL DEFAULT now(),
    expires_at   timestamptz NOT NULL,
    last_seen_at timestamptz NOT NULL DEFAULT now(),
    remote_addr  inet,
    client       text
);
CREATE INDEX user_session_by_user ON user_session (user_id);
CREATE INDEX user_session_expiry ON user_session (expires_at);

-- Bans by account, by address range, or by email domain. NULL expires_at = permanent.
CREATE TABLE ban (
    ban_id       bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id      bigint REFERENCES account ON DELETE CASCADE,
    addr         cidr,
    email_domain citext,
    reason       text NOT NULL,
    created_by   bigint REFERENCES account ON DELETE SET NULL,
    created_at   timestamptz NOT NULL DEFAULT now(),
    expires_at   timestamptz,
    lifted_at    timestamptz,
    CHECK (num_nonnulls(user_id, addr, email_domain) = 1)
);
CREATE INDEX ban_user ON ban (user_id) WHERE lifted_at IS NULL;
CREATE INDEX ban_addr ON ban USING gist (addr inet_ops) WHERE lifted_at IS NULL;

-- The world index (today: worlds.pb). One row per co-op world; the world's log stays on disk.
-- owner_node: which shadNet process may write this world (single writer). A worker claims a
-- world with UPDATE ... SET owner_node=$me, lease_until=now()+'30 s' WHERE owner_node IS NULL
-- OR lease_until < now(), and renews the lease while it runs the world.
CREATE TABLE world (
    world_id     text PRIMARY KEY,                -- TeamWorldInfo.id ("world:<id>" party key)
    folder       text NOT NULL UNIQUE,            -- folder under BloodborneWorldPath
    name         text NOT NULL,
    owner_id     bigint NOT NULL REFERENCES account ON DELETE RESTRICT,
    max_members  smallint NOT NULL DEFAULT 4 CHECK (max_members BETWEEN 1 AND 16),
    status       text NOT NULL DEFAULT 'active' CHECK (status IN ('active','archived','locked')),
    created_at   timestamptz NOT NULL DEFAULT now(),
    last_played  timestamptz,
    owner_node   text,
    lease_until  timestamptz
);
CREATE INDEX world_owner ON world (owner_id);

CREATE TABLE world_member (
    world_id   text   NOT NULL REFERENCES world ON DELETE CASCADE,
    user_id    bigint NOT NULL REFERENCES account ON DELETE CASCADE,
    role       text   NOT NULL DEFAULT 'member' CHECK (role IN ('host','member')),
    joined_at  timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (world_id, user_id)
);
CREATE INDEX world_member_user ON world_member (user_id);

-- Optional: an index of world snapshots pushed to object storage (the bytes are NOT in here).
CREATE TABLE world_snapshot (
    world_id     text NOT NULL REFERENCES world ON DELETE CASCADE,
    journal_from bigint NOT NULL,                 -- StoredWorld.journal_from
    taken_at     timestamptz NOT NULL DEFAULT now(),
    object_key   text NOT NULL,                   -- e.g. s3://bucket/worlds/<id>/<journal_from>.pb
    size_bytes   bigint NOT NULL,
    sha256       bytea NOT NULL,
    PRIMARY KEY (world_id, journal_from)
);

-- Append-only audit trail: sign-ins, bans, world create/delete/membership, admin actions.
CREATE TABLE audit_log (
    audit_id   bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    at         timestamptz NOT NULL DEFAULT now(),
    actor_id   bigint REFERENCES account ON DELETE SET NULL,
    actor_addr inet,
    action     text NOT NULL,                     -- 'login', 'login_failed', 'ban', 'world.create'
    target     text,                              -- 'user:42', 'world:abc'
    details    jsonb NOT NULL DEFAULT '{}'
);
CREATE INDEX audit_at ON audit_log (at);
CREATE INDEX audit_actor ON audit_log (actor_id, at);
-- Keep the app role from rewriting history (run as the owner):
--   REVOKE UPDATE, DELETE ON audit_log FROM shadnet_app;
