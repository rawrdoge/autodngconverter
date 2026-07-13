-- RawImport Pipeline v1 schema extension (PRD Q8: preview re-embed tracking)
-- Applied by the embedded migration runner (Q10).

-- Track each preview re-embed event so we have an audit trail and can
-- distinguish an intentional preview edit from silent DNG corruption.
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

-- Denormalized timestamp on imports so the corruption monitor can tell a
-- recently-edited preview (expected hash change) from unexpected drift.
ALTER TABLE imports
    ADD COLUMN last_preview_edit_at TIMESTAMP NULL DEFAULT NULL;