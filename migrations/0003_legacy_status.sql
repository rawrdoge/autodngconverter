-- RawImport Pipeline v1 schema extension (nomenclature-aware sequencing).
-- Adds a 'legacy' status so pre-existing DNGs found in the output volume can be
-- registered as unmatched placeholders without faking a real conversion.
-- NOTE: the Go migration runner splits on ";" and runs one statement per Exec,
-- so each ALTER is a separate statement.

ALTER TABLE imports MODIFY COLUMN status ENUM('pending','converting','completed','failed','restored','legacy') DEFAULT 'pending';