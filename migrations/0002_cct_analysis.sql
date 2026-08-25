-- RawImport Pipeline v2.3.0 — CCT Analysis (PRD-CCT-001 §4.1).
-- Applied by the embedded migration runner (directory scan + schema_migrations).
-- Dialect: MariaDB (matches migrations/0001_init.sql).

CREATE TABLE IF NOT EXISTS cct_results (
    id              BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    import_id       BIGINT UNSIGNED NOT NULL,
    sequence_name   VARCHAR(32) NOT NULL,
    algorithm       VARCHAR(32) NOT NULL DEFAULT 'rawpy_grayworld',
    cct_kelvin      DOUBLE,
    tint            DOUBLE,
    xy_x            DOUBLE,
    xy_y            DOUBLE,
    hue             DOUBLE,           -- D50-adapted hue (radians) for color calibration
    chroma          DOUBLE,           -- D50-adapted chroma for color calibration
    confidence      DOUBLE,
    sampled_area    TEXT,             -- "full" | "center20" | "center10" | JSON rectangle
    source_used     ENUM('raw','dng') DEFAULT 'raw',
    status          ENUM('pending','running','completed','failed') DEFAULT 'pending',
    error_msg       TEXT,
    created_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    completed_at    TIMESTAMP NULL DEFAULT NULL,

    FOREIGN KEY (import_id) REFERENCES imports(id) ON DELETE CASCADE,
    UNIQUE KEY uq_import_algo (import_id, algorithm),
    INDEX idx_cct_status (status)
) ENGINE=InnoDB;
