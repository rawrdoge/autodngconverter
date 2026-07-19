-- Migration 0002: add orientation column for Cross-App Rotation Sync (PRD §5).
-- Idempotent: safe to re-run on fresh or already-migrated DB.
-- Applied by the embedded migration runner (ORCHESTRATION_CppRewrite.md §2).
-- The Go service's existing 0001_init.sql remains the authoritative base schema;
-- this file ONLY adds the new column so C++ and Go DBs stay compatible.

ALTER TABLE imports ADD COLUMN IF NOT EXISTS orientation TINYINT UNSIGNED NULL COMMENT 'EXIF Orientation 1-8 agreed across apps, NULL means unset';
