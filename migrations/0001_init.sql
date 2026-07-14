-- RawImport Pipeline v1 schema (merged: base + preview-edits + legacy-status).
-- Applied by the embedded migration runner (Q10). Idempotent: safe to re-run
-- on a fresh or already-migrated database.
--
-- This single file replaces the former 0001/0002/0003 split. The schema below
-- is the complete, current state (including the preview_edits audit table, the
-- imports.last_preview_edit_at column, and the 'legacy' status value).

CREATE TABLE IF NOT EXISTS sequences (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(32) NOT NULL UNIQUE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS imports (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    sequence_id BIGINT UNSIGNED NOT NULL,
    source_path TEXT NOT NULL,
    source_hash CHAR(64) NOT NULL,
    output_path TEXT NOT NULL,
    output_hash CHAR(64) NOT NULL,
    camera_model VARCHAR(64),
    capture_date DATE,
    capture_time TIME,
    date_source ENUM('exif','mtime') NOT NULL DEFAULT 'exif',
    folder_schema VARCHAR(16),
    conversion_settings JSON,
    status ENUM('pending','converting','completed','failed','restored','legacy') DEFAULT 'pending',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    completed_at TIMESTAMP NULL DEFAULT NULL,
    error_message TEXT,
    last_preview_edit_at TIMESTAMP NULL DEFAULT NULL,
    FOREIGN KEY (sequence_id) REFERENCES sequences(id) ON DELETE RESTRICT,
    INDEX idx_source_hash (source_hash),
    INDEX idx_output_hash (output_hash),
    INDEX idx_sequence_id (sequence_id),
    INDEX idx_capture_date (capture_date),
    INDEX idx_status_created (status, created_at),
    INDEX idx_camera_date (camera_model, capture_date)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS reconversions (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    import_id BIGINT UNSIGNED NOT NULL,
    previous_output_hash CHAR(64) NOT NULL,
    new_output_hash CHAR(64),
    conversion_settings JSON NOT NULL,
    triggered_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    completed_at TIMESTAMP NULL DEFAULT NULL,
    status ENUM('pending','running','completed','failed') DEFAULT 'pending',
    error_message TEXT,
    FOREIGN KEY (import_id) REFERENCES imports(id) ON DELETE CASCADE,
    INDEX idx_import_id (import_id),
    INDEX idx_status (status)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS processing_locks (
    source_hash CHAR(64) NOT NULL,
    source_path TEXT NOT NULL,
    locked_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    expires_at TIMESTAMP NOT NULL,
    worker_id VARCHAR(64),
    PRIMARY KEY (source_hash),
    INDEX idx_expires (expires_at)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS alerts (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    severity ENUM('info','warning','error') NOT NULL,
    category VARCHAR(32) NOT NULL,
    message TEXT NOT NULL,
    ref_sequence VARCHAR(32),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    acknowledged TINYINT(1) DEFAULT 0,
    INDEX idx_severity_created (severity, created_at),
    INDEX idx_acknowledged (acknowledged)
) ENGINE=InnoDB;

-- Preview re-embed audit trail (PRD Q8). Distinguishes an intentional preview
-- edit from silent DNG corruption.
CREATE TABLE IF NOT EXISTS preview_edits (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    import_id BIGINT UNSIGNED NOT NULL,
    worker VARCHAR(16) NOT NULL COMMENT 'exiftool|dnglab|dng_sdk',
    previous_output_hash CHAR(64) NOT NULL,
    new_output_hash CHAR(64) NOT NULL,
    preview_width INT UNSIGNED,
    preview_height INT UNSIGNED,
    preview_quality TINYINT UNSIGNED,
    edited_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (import_id) REFERENCES imports(id) ON DELETE CASCADE,
    INDEX idx_import_id (import_id),
    INDEX idx_edited_at (edited_at)
) ENGINE=InnoDB;

-- NOTE: AllocateSequence stored procedure is intentionally omitted here.
-- The Go migration runner executes one statement per Exec and does not
-- support DELIMITER. Sequence allocation is handled in code via
-- AUTO_INCREMENT + LAST_INSERT_ID() (see Store.AllocateSequence fallback).