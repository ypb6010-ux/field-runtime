-- SPDX-FileCopyrightText: 2026 ypb6010-ux
-- SPDX-License-Identifier: MPL-2.0
-- web_console SQLite schema. See docs/ARCHITECTURE.md §5. Idempotent.
-- (journal_mode/foreign_keys are per-connection PRAGMAs; set by the app's
-- connection if needed, not baked into the schema script.)

-- ── Protocol / datapoint configuration (draft set) ──────────────────────────
CREATE TABLE IF NOT EXISTS transports (
    id            TEXT PRIMARY KEY,
    name          TEXT NOT NULL DEFAULT '',
    kind          TEXT NOT NULL CHECK(kind IN ('modbus_tcp_client','opc_ua_client','s7_client')),
    enabled       INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0,1)),
    params_json   TEXT NOT NULL DEFAULT '{}',
    scheduler_json TEXT NOT NULL DEFAULT '{}',
    created_at    INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    updated_at    INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

CREATE TABLE IF NOT EXISTS codecs (
    id            TEXT PRIMARY KEY,
    kind          TEXT NOT NULL,            -- enum_u16 / lua
    params_json   TEXT NOT NULL DEFAULT '{}',
    script_path   TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS datapoints (
    id            TEXT PRIMARY KEY,
    transport_id  TEXT NOT NULL REFERENCES transports(id) ON DELETE CASCADE,
    reg_table     TEXT NOT NULL DEFAULT 'HR',
    addr          INTEGER NOT NULL DEFAULT 0,
    type          TEXT NOT NULL DEFAULT 'U16',
    word_order    TEXT NOT NULL DEFAULT 'hi_lo',
    scale         REAL NOT NULL DEFAULT 1.0,
    codec_id      TEXT REFERENCES codecs(id) ON DELETE SET NULL,
    kind          TEXT NOT NULL DEFAULT 'Status',
    enabled       INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0,1)),
    CHECK(addr BETWEEN 0 AND 65535),
    CHECK(scale != 0),
    CHECK(kind = 'Status')
);
CREATE INDEX IF NOT EXISTS ix_datapoints_transport ON datapoints(transport_id);

CREATE TABLE IF NOT EXISTS poll_ranges (
    id            TEXT PRIMARY KEY,
    transport_id  TEXT NOT NULL REFERENCES transports(id) ON DELETE CASCADE,
    reg_table     TEXT NOT NULL DEFAULT 'HR',
    start         INTEGER NOT NULL DEFAULT 0,
    count         INTEGER NOT NULL DEFAULT 1,
    period_ms     INTEGER NOT NULL DEFAULT 1000,
    enabled       INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0,1)),
    CHECK(start BETWEEN 0 AND 65535),
    CHECK(count BETWEEN 1 AND 125),
    CHECK(start + count <= 65536),
    CHECK(period_ms > 0)
);
CREATE INDEX IF NOT EXISTS ix_polls_transport ON poll_ranges(transport_id);

CREATE TABLE IF NOT EXISTS conversion_rules (
    id            TEXT PRIMARY KEY,
    name          TEXT NOT NULL DEFAULT '',
    enabled       INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0,1)),
    source_json   TEXT NOT NULL DEFAULT '{}',
    dest_json     TEXT NOT NULL DEFAULT '{}',
    transform_json TEXT NOT NULL DEFAULT '{}',
    trigger       TEXT NOT NULL DEFAULT 'onChange',
    period_ms     INTEGER NOT NULL DEFAULT 0
);

-- ── Hot-reload versioning ───────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS config_versions (
    version       INTEGER PRIMARY KEY AUTOINCREMENT,
    status        TEXT NOT NULL DEFAULT 'draft',  -- draft / active / superseded
    snapshot_json TEXT NOT NULL DEFAULT '{}',
    author        TEXT NOT NULL DEFAULT '',
    note          TEXT NOT NULL DEFAULT '',
    created_at    INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    applied_at    INTEGER
);

-- ── History + events ────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS samples (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    dp_id         TEXT NOT NULL,
    ts            INTEGER NOT NULL,
    value_num     REAL,
    value_text    TEXT,
    quality       TEXT NOT NULL DEFAULT 'good'
);
CREATE INDEX IF NOT EXISTS ix_samples_dp_ts ON samples(dp_id, ts);

CREATE TABLE IF NOT EXISTS events (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    ts            INTEGER NOT NULL,
    level         TEXT NOT NULL DEFAULT 'info',
    source        TEXT NOT NULL DEFAULT '',
    code          INTEGER NOT NULL DEFAULT 0,
    message       TEXT NOT NULL DEFAULT '',
    detail_json   TEXT NOT NULL DEFAULT '{}'
);
CREATE INDEX IF NOT EXISTS ix_events_ts ON events(ts);

CREATE TABLE IF NOT EXISTS settings (
    key           TEXT PRIMARY KEY,
    value_json    TEXT NOT NULL
);

-- ── Auth + RBAC ─────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS roles (
    id            TEXT PRIMARY KEY,
    description   TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS role_permissions (
    role_id       TEXT NOT NULL REFERENCES roles(id) ON DELETE CASCADE,
    permission    TEXT NOT NULL,
    PRIMARY KEY (role_id, permission)
);
CREATE TABLE IF NOT EXISTS users (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    username      TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL DEFAULT '',
    role_id       TEXT NOT NULL REFERENCES roles(id),
    enabled       INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0,1)),
    created_at    INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    last_login_at INTEGER
);

CREATE TABLE IF NOT EXISTS audit_log (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    ts            INTEGER NOT NULL,
    user_id       INTEGER REFERENCES users(id) ON DELETE SET NULL,
    action        TEXT NOT NULL DEFAULT '',
    target        TEXT NOT NULL DEFAULT '',
    detail_json   TEXT NOT NULL DEFAULT '{}'
);
CREATE INDEX IF NOT EXISTS ix_audit_ts ON audit_log(ts);

-- ── Seed defaults ───────────────────────────────────────────────────────────
INSERT OR IGNORE INTO settings(key, value_json) VALUES
    ('sample_retention_days', '30');

INSERT OR IGNORE INTO roles(id, description) VALUES
    ('viewer',   'Read-only monitoring'),
    ('operator', 'Monitoring + control writes + conversion管理'),
    ('admin',    'Full system configuration');

INSERT OR IGNORE INTO role_permissions(role_id, permission) VALUES
    ('viewer','data:read'),
    ('operator','data:read'),('operator','data:write'),('operator','config:read'),
    ('operator','config:write'),('operator','conversion:manage'),
    ('admin','data:read'),('admin','data:write'),('admin','config:read'),
    ('admin','config:write'),('admin','config:apply'),('admin','conversion:manage'),
    ('admin','system:settings'),('admin','user:manage');
