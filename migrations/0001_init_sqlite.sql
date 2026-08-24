-- RawImport Portable v2.1.0 — SQLite schema (0001_init_sqlite).
-- Applied by the embedded migration runner. Idempotent: safe to re-run on a
-- fresh or already-migrated database. This file is the source of truth; the
-- same SQL is embedded at compile time in db.cpp so the release ZIP needs no
-- migrations/ directory.
--
-- Dialect notes (vs MariaDB main branch):
--   INTEGER PRIMARY KEY AUTOINCREMENT   <- BIGINT UNSIGNED AUTO_INCREMENT
--   TEXT ISO-8601 strings               <- DATE / TIME / TIMESTAMP columns
--   TEXT (unvalidated)                  <- JSON / ENUM
--   app-level status validation         <- ENUM constraint

CREATE TABLE IF NOT EXISTS schema_migrations (
    version TEXT PRIMARY KEY,
    applied_at TEXT DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS sequences (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,
    created_at TEXT DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS imports (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sequence_id INTEGER NOT NULL,
    source_path TEXT NOT NULL,
    source_hash TEXT NOT NULL,
    output_path TEXT NOT NULL,
    output_hash TEXT NOT NULL,
    camera_model TEXT,
    capture_date TEXT,
    capture_time TEXT,
    folder_schema TEXT,
    conversion_settings TEXT,
    status TEXT DEFAULT 'pending',
    date_source TEXT DEFAULT 'exif',
    orientation INTEGER,
    created_at TEXT DEFAULT (datetime('now')),
    completed_at TEXT,
    error_message TEXT,
    FOREIGN KEY (sequence_id) REFERENCES sequences(id)
);

-- Dedup performs a hash lookup per file; without these indexes every scan
-- would be O(n). Kept from the MariaDB schema where they existed too.
CREATE INDEX IF NOT EXISTS idx_imports_source_hash ON imports(source_hash);
CREATE INDEX IF NOT EXISTS idx_imports_output_hash ON imports(output_hash);
CREATE INDEX IF NOT EXISTS idx_imports_capture_date ON imports(capture_date);
CREATE INDEX IF NOT EXISTS idx_imports_status_created ON imports(status, created_at);
CREATE INDEX IF NOT EXISTS idx_imports_sequence_id ON imports(sequence_id);

-- Reserved per action-plan schema parity. Reconversions are an excluded
-- feature in the portable branch; nothing writes to this table.
CREATE TABLE IF NOT EXISTS reconversions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    import_id INTEGER NOT NULL,
    previous_output_hash TEXT NOT NULL,
    new_output_hash TEXT,
    conversion_settings TEXT NOT NULL,
    triggered_at TEXT DEFAULT (datetime('now')),
    completed_at TEXT,
    status TEXT DEFAULT 'pending',
    error_message TEXT,
    FOREIGN KEY (import_id) REFERENCES imports(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS processing_locks (
    import_id INTEGER PRIMARY KEY,
    worker_id TEXT NOT NULL,
    expires_at INTEGER NOT NULL,
    created_at TEXT DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS preview_edits (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    import_id INTEGER NOT NULL,
    worker TEXT,
    width INTEGER,
    height INTEGER,
    quality INTEGER,
    created_at TEXT DEFAULT (datetime('now')),
    FOREIGN KEY (import_id) REFERENCES imports(id) ON DELETE CASCADE
);