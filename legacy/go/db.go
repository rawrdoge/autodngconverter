package main

import (
	"database/sql"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	// MySQL driver. NOTE: the Go service targets MySQL only. The pure-Go
	// MySQL driver (go-sql-driver/mysql) stalls on every query against
	// MariaDB 10.11 over container TCP (protocol framing incompatibility).
	// For a MariaDB backend, use the C++ service (src/, Dockerfile.cpp) which
	// uses libmariadb and is the validated MariaDB implementation.
	_ "github.com/go-sql-driver/mysql"
)

// Store wraps the database connection and all persistence operations (PRD §4.2.5).
// Go service: MySQL backend. MariaDB backend is served by the C++ service.
type Store struct {
	db *sql.DB
}

// DSN builds the MySQL connection string (Go service: MySQL backend only).
// For MariaDB, use the C++ service (src/, Dockerfile.cpp) which uses libmariadb.
func (c Config) DSN() string {
	return fmt.Sprintf("%s:%s@tcp(%s:%d)/%s?parseTime=true&collation=utf8mb4_unicode_ci&timeout=5s&readTimeout=8s&writeTimeout=8s",
		c.DBUser, c.DBPassword, c.DBHost, c.DBPort, c.DBName)
}

// OpenDB connects to MySQL and verifies connectivity.
// NOTE: Go service targets MySQL only. The pure-Go MySQL driver
// (go-sql-driver/mysql) stalls against MariaDB 10.11 over container TCP;
// use the C++ service for MariaDB backends.
func OpenDB(c Config) (*Store, error) {
	db, err := sql.Open("mysql", c.DSN())
	if err != nil {
		return nil, err
	}
	// Pool tuning: recycle connections before they go stale. MaxIdleTime <
	// MaxLifetime < server wait_timeout keeps the pool serving fresh
	// connections. readTimeout/writeTimeout (in DSN) bound any single stalled
	// query so it can never block the pool indefinitely.
	db.SetMaxOpenConns(10)
	db.SetMaxIdleConns(5)
	db.SetConnMaxLifetime(30 * time.Second)
	db.SetConnMaxIdleTime(10 * time.Second)
	// Retry briefly for container startup ordering.
	for i := 0; i < 10; i++ {
		if err = db.Ping(); err == nil {
			break
		}
		time.Sleep(time.Second)
	}
	if err != nil {
		return nil, fmt.Errorf("db ping: %w", err)
	}
	return &Store{db: db}, nil
}

// Migrate applies versioned SQL files from migrations/ (PRD Q10).
func (s *Store) Migrate(dir string) error {
	if err := s.ensureMigrationsTable(); err != nil {
		return err
	}
	files, err := filepath.Glob(filepath.Join(dir, "*.sql"))
	if err != nil {
		return err
	}
	sort.Strings(files)
	for _, f := range files {
		name := filepath.Base(f)
		applied, err := s.isApplied(name)
		if err != nil {
			return err
		}
		if applied {
			continue
		}
		sqlBytes, err := os.ReadFile(f)
		if err != nil {
			return err
		}
		// MySQL driver executes one statement per Exec; split on ";".
		for _, stmt := range splitSQL(string(sqlBytes)) {
			if _, err := s.db.Exec(stmt); err != nil {
				return fmt.Errorf("apply %s: %w", name, err)
			}
		}
		if _, err := s.db.Exec("INSERT INTO schema_migrations (version) VALUES (?)", name); err != nil {
			return err
		}
		fmt.Printf("applied migration: %s\n", name)
	}
	return nil
}

func (s *Store) ensureMigrationsTable() error {
	_, err := s.db.Exec(`CREATE TABLE IF NOT EXISTS schema_migrations (
		version VARCHAR(255) PRIMARY KEY,
		applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
	) ENGINE=InnoDB`)
	return err
}

func (s *Store) isApplied(name string) (bool, error) {
	var n int
	err := s.db.QueryRow("SELECT COUNT(*) FROM schema_migrations WHERE version = ?", name).Scan(&n)
	if err != nil {
		return false, err
	}
	return n > 0, nil
}

// AllocateSequence atomically allocates the next IMG_{n} (PRD §4.2.4, Q4).
// Uses AUTO_INCREMENT + LAST_INSERT_ID via a stored procedure if present,
// else direct insert.
func (s *Store) AllocateSequence() (int64, string, error) {
	// Try stored procedure first (MariaDB <10.5 compat, PRD §4.2.5).
	if _, err := s.db.Exec("CALL AllocateSequence(@seq_id, @seq_name)"); err == nil {
		var id int64
		var name string
		if err := s.db.QueryRow("SELECT @seq_id, @seq_name").Scan(&id, &name); err == nil {
			return id, name, nil
		}
	}
	// Fallback: insert with a temporary placeholder name, then update to IMG_n.
	res, err := s.db.Exec("INSERT INTO sequences (name) VALUES ('')")
	if err != nil {
		return 0, "", err
	}
	id, err := res.LastInsertId()
	if err != nil {
		return 0, "", err
	}
	name := fmt.Sprintf("IMG_%d", id)
	if _, err := s.db.Exec("UPDATE sequences SET name = ? WHERE id = ?", name, id); err != nil {
		return 0, "", err
	}
	return id, name, nil
}

// HasSourceHash reports whether a source hash already exists (idempotency, Q12).
func (s *Store) HasSourceHash(h string) (bool, error) {
	var n int
	err := s.db.QueryRow("SELECT COUNT(*) FROM imports WHERE source_hash = ?", h).Scan(&n)
	return n > 0, err
}

// HasOutputHash reports whether an output DNG path is already registered
// (used by the nomenclature-aware reconcile scan, Q13).
func (s *Store) HasOutputHash(h string) (bool, error) {
	var n int
	err := s.db.QueryRow("SELECT COUNT(*) FROM imports WHERE output_hash = ?", h).Scan(&n)
	return n > 0, err
}

// InsertImport writes the atomic import record (PRD §5.1 step 12).
func (s *Store) InsertImport(rec ImportRecord) error {
	_, err := s.db.Exec(`INSERT INTO imports
		(sequence_id, source_path, source_hash, output_path, output_hash,
		 camera_model, capture_date, capture_time, date_source, folder_schema,
		 conversion_settings, status, completed_at)
		VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)`,
		rec.SequenceID, rec.SourcePath, rec.SourceHash, rec.OutputPath, rec.OutputHash,
		rec.CameraModel, rec.CaptureDate, rec.CaptureTime, rec.DateSource, rec.FolderSchema,
		rec.ConversionSettings, rec.Status, time.Now())
	return err
}

// AcquireLock sets a processing lock keyed by source_hash with TTL (Q12).
func (s *Store) AcquireLock(sourceHash, sourcePath, workerID string, ttl time.Duration) (bool, error) {
	expires := time.Now().Add(ttl)
	res, err := s.db.Exec(`INSERT INTO processing_locks (source_hash, source_path, expires_at, worker_id)
		VALUES (?,?,?,?)
		ON DUPLICATE KEY UPDATE
		  expires_at = IF(expires_at < NOW(), VALUES(expires_at), expires_at),
		  worker_id = IF(expires_at < NOW(), VALUES(worker_id), worker_id)`,
		sourceHash, sourcePath, expires, workerID)
	if err != nil {
		return false, err
	}
	affected, _ := res.RowsAffected()
	return affected > 0, nil
}

// ReleaseLock removes a processing lock (Q12).
func (s *Store) ReleaseLock(sourceHash string) error {
	_, err := s.db.Exec("DELETE FROM processing_locks WHERE source_hash = ?", sourceHash)
	return err
}

// InsertAlert writes an alert row (Q11).
func (s *Store) InsertAlert(sev, category, message, ref string) error {
	_, err := s.db.Exec(`INSERT INTO alerts (severity, category, message, ref_sequence)
		VALUES (?,?,?,?)`, sev, category, message, ref)
	return err
}

// GetImportBySequence returns a record by IMG_n name (API §7.2).
// Resolves the sequence id from the sequences table first, then queries
// imports by sequence_id. Avoids a correlated subquery in WHERE, which
// deadlocked under MariaDB + the Go driver in e2e.
func (s *Store) GetImportBySequence(seq string) (*ImportRecord, error) {
	var sid int64
	if err := s.db.QueryRow(`SELECT id FROM sequences WHERE name = ?`, seq).Scan(&sid); err != nil {
		return nil, err
	}
	var rec ImportRecord
	err := s.db.QueryRow(`SELECT id, sequence_id, source_path, source_hash, output_path, output_hash,
		COALESCE(camera_model,''), COALESCE(capture_date,''), COALESCE(capture_time,''), date_source, COALESCE(folder_schema,''), status, created_at, completed_at
		FROM imports WHERE sequence_id = ?`, sid).
		Scan(&rec.ID, &sid, &rec.SourcePath, &rec.SourceHash, &rec.OutputPath, &rec.OutputHash,
			&rec.CameraModel, &rec.CaptureDate, &rec.CaptureTime, &rec.DateSource, &rec.FolderSchema,
			&rec.Status, rec.CreatedAt, rec.CompletedAt)
	if err != nil {
		return nil, err
	}
	rec.SequenceName = seq
	return &rec, nil
}

// ListImports returns paginated imports (API §7.1).
func (s *Store) ListImports(page, limit int, status string) ([]ImportRecord, int, error) {
	var total int
	if err := s.db.QueryRow("SELECT COUNT(*) FROM imports").Scan(&total); err != nil {
		return nil, 0, err
	}
	q := `SELECT (SELECT name FROM sequences WHERE id = sequence_id), source_path, source_hash,
		output_path, output_hash, COALESCE(camera_model,''), COALESCE(capture_date,''), COALESCE(capture_time,''), date_source,
		COALESCE(folder_schema,''), status, created_at, completed_at FROM imports`
	args := []interface{}{}
	if status != "" {
		q += " WHERE status = ?"
		args = append(args, status)
	}
	q += " ORDER BY id DESC LIMIT ? OFFSET ?"
	args = append(args, limit, (page-1)*limit)
	rows, err := s.db.Query(q, args...)
	if err != nil {
		return nil, 0, err
	}
	defer rows.Close()
	var out []ImportRecord
	for rows.Next() {
		var r ImportRecord
		if err := rows.Scan(&r.SequenceName, &r.SourcePath, &r.SourceHash, &r.OutputPath, &r.OutputHash,
			&r.CameraModel, &r.CaptureDate, &r.CaptureTime, &r.DateSource, &r.FolderSchema,
			&r.Status, r.CreatedAt, r.CompletedAt); err != nil {
			return nil, 0, err
		}
		out = append(out, r)
	}
	return out, total, nil
}

// InsertReconversion records a re-conversion request (PRD §5.2).
func (s *Store) InsertReconversion(importID int64, prevHash, settings string) (int64, error) {
	res, err := s.db.Exec(`INSERT INTO reconversions
		(import_id, previous_output_hash, conversion_settings, status)
		VALUES (?,?,?,'pending')`, importID, prevHash, settings)
	if err != nil {
		return 0, err
	}
	return res.LastInsertId()
}

// PendingReconversions returns reconversion jobs not yet completed.
func (s *Store) PendingReconversions() ([]ReconversionJob, error) {
	rows, err := s.db.Query(`SELECT r.id, (SELECT name FROM sequences WHERE id = i.sequence_id),
		i.source_path, i.output_path, i.output_hash, i.folder_schema, r.conversion_settings
		FROM reconversions r JOIN imports i ON i.id = r.import_id
		WHERE r.status = 'pending'`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []ReconversionJob
	for rows.Next() {
		var j ReconversionJob
		if err := rows.Scan(&j.ID, &j.Sequence, &j.SourcePath, &j.OutputPath, &j.PrevHash, &j.FolderSchema, &j.Settings); err != nil {
			return nil, err
		}
		out = append(out, j)
	}
	return out, nil
}

// CompleteReconversion marks a reconversion done and updates the import (§5.2 h,i).
func (s *Store) CompleteReconversion(jobID int64, newHash, newOutputPath string) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err := tx.Exec(`UPDATE reconversions SET new_output_hash=?, status='completed', completed_at=NOW() WHERE id=?`,
		newHash, jobID); err != nil {
		return err
	}
	if _, err := tx.Exec(`UPDATE imports SET output_path=?, output_hash=?, status='restored', completed_at=NOW() WHERE id=(SELECT import_id FROM reconversions WHERE id=?)`,
		newOutputPath, newHash, jobID); err != nil {
		return err
	}
	return tx.Commit()
}

// Stats returns aggregate counts for the dashboard (API §7 / §9).
func (s *Store) Stats() (map[string]int, error) {
	out := map[string]int{}
	var total, completed, failed int
	if err := s.db.QueryRow("SELECT COUNT(*) FROM imports").Scan(&total); err != nil {
		return nil, err
	}
	if err := s.db.QueryRow("SELECT COUNT(*) FROM imports WHERE status='completed'").Scan(&completed); err != nil {
		return nil, err
	}
	if err := s.db.QueryRow("SELECT COUNT(*) FROM imports WHERE status='failed'").Scan(&failed); err != nil {
		return nil, err
	}
	out["total"] = total
	out["completed"] = completed
	out["failed"] = failed
	return out, nil
}

// GetImportByHash finds a record by source OR output hash (API §7.3).
func (s *Store) GetImportByHash(hash string) (*ImportRecord, error) {
	var rec ImportRecord
	var sid int64
	err := s.db.QueryRow(`SELECT id, sequence_id, source_path, source_hash, output_path, output_hash,
		COALESCE(camera_model,''), COALESCE(capture_date,''), COALESCE(capture_time,''), date_source, COALESCE(folder_schema,''), status, created_at, completed_at
		FROM imports WHERE source_hash = ? OR output_hash = ? LIMIT 1`, hash, hash).
		Scan(&rec.ID, &sid, &rec.SourcePath, &rec.SourceHash, &rec.OutputPath, &rec.OutputHash,
			&rec.CameraModel, &rec.CaptureDate, &rec.CaptureTime, &rec.DateSource, &rec.FolderSchema,
			&rec.Status, rec.CreatedAt, rec.CompletedAt)
	if err != nil {
		return nil, err
	}
	rec.SequenceName = fmt.Sprintf("IMG_%d", sid)
	return &rec, nil
}

// AlertRow is a row from the alerts table (Q11).
type AlertRow struct {
	ID          int64
	Severity    string
	Category    string
	Message     string
	RefSequence string
	CreatedAt   time.Time
	Acknowledged bool
}

// recentAlerts returns the latest 100 alerts (API §7.4).
func (s *Store) recentAlerts() ([]AlertRow, error) {
	rows, err := s.db.Query(`SELECT id, severity, category, message, ref_sequence, created_at, acknowledged
		FROM alerts ORDER BY created_at DESC LIMIT 100`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []AlertRow
	for rows.Next() {
		var a AlertRow
		if err := rows.Scan(&a.ID, &a.Severity, &a.Category, &a.Message, &a.RefSequence, &a.CreatedAt, &a.Acknowledged); err != nil {
			return nil, err
		}
		out = append(out, a)
	}
	return out, nil
}

// helper to keep compiler happy about unused strings import in some builds
var _ = strings.TrimSpace

// PreviewEdit captures one preview re-embed event (PRD Q8).
type PreviewEdit struct {
	ImportID       int64
	Worker         string
	PrevHash       string
	NewHash        string
	PreviewWidth   int
	PreviewHeight  int
	PreviewQuality int
}

// RecordPreviewEdit writes the audit row and updates the import's output_hash
// + last_preview_edit_at atomically (PRD Q8). The corruption monitor treats a
// recent last_preview_edit_at as an expected hash change, not corruption.
func (s *Store) RecordPreviewEdit(e PreviewEdit) error {
	tx, err := s.db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()
	if _, err := tx.Exec(`INSERT INTO preview_edits
		(import_id, worker, previous_output_hash, new_output_hash, preview_width, preview_height, preview_quality)
		VALUES (?,?,?,?,?,?,?)`,
		e.ImportID, e.Worker, e.PrevHash, e.NewHash, e.PreviewWidth, e.PreviewHeight, e.PreviewQuality); err != nil {
		return err
	}
	if _, err := tx.Exec(`UPDATE imports SET output_hash=?, last_preview_edit_at=NOW() WHERE id=?`,
		e.NewHash, e.ImportID); err != nil {
		return err
	}
	return tx.Commit()
}

// UpdateOrientation persists a synced EXIF orientation for an import (PRD §5, ORCH §7.4).
func (s *Store) UpdateOrientation(id int64, orientation int) error {
	_, err := s.db.Exec("UPDATE imports SET orientation = ? WHERE id = ?", orientation, id)
	return err
}

// GetImportByOutputPath finds a record by its output DNG path (notify endpoint).
func (s *Store) GetImportByOutputPath(path string) (*ImportRecord, error) {
	var rec ImportRecord
	var sid int64
	err := s.db.QueryRow(`SELECT id, sequence_id, source_path, source_hash, output_path, output_hash,
		COALESCE(camera_model,''), COALESCE(capture_date,''), COALESCE(capture_time,''), date_source, COALESCE(folder_schema,''), status, created_at, completed_at
		FROM imports WHERE output_path = ? LIMIT 1`, path).
		Scan(&rec.ID, &sid, &rec.SourcePath, &rec.SourceHash, &rec.OutputPath, &rec.OutputHash,
			&rec.CameraModel, &rec.CaptureDate, &rec.CaptureTime, &rec.DateSource, &rec.FolderSchema,
			&rec.Status, rec.CreatedAt, rec.CompletedAt)
	if err != nil {
		return nil, err
	}
	rec.SequenceName = fmt.Sprintf("IMG_%d", sid)
	return &rec, nil
}

// GetImportBySourcePath resolves a source RAW path to its import record (used by
// the Darktable Lua export-hook to find the DNG that should receive a re-embedded
// preview). Matches on the stored source_path (the archived RAW location).
func (s *Store) GetImportBySourcePath(path string) (*ImportRecord, error) {
	var rec ImportRecord
	var sid int64
	err := s.db.QueryRow(`SELECT id, sequence_id, source_path, source_hash, output_path, output_hash,
		COALESCE(camera_model,''), COALESCE(capture_date,''), COALESCE(capture_time,''), date_source, COALESCE(folder_schema,''), status, created_at, completed_at
		FROM imports WHERE source_path = ? LIMIT 1`, path).
		Scan(&rec.ID, &sid, &rec.SourcePath, &rec.SourceHash, &rec.OutputPath, &rec.OutputHash,
			&rec.CameraModel, &rec.CaptureDate, &rec.CaptureTime, &rec.DateSource, &rec.FolderSchema,
			&rec.Status, rec.CreatedAt, rec.CompletedAt)
	if err != nil {
		return nil, err
	}
	rec.SequenceName = fmt.Sprintf("IMG_%d", sid)
	return &rec, nil
}