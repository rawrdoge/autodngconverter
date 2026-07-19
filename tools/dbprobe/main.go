package main

import (
	"database/sql"
	"fmt"
	"time"

	_ "github.com/go-sql-driver/mysql"
)

// dbprobe isolates the Go MySQL driver behavior against the e2e MariaDB.
// It runs the three suspect query shapes in SEPARATE connections on a fresh
// pool, with a hard 6s timeout per query, and prints PASS/FAIL + latency.
// This tells us whether the e2e hang is a driver-layer stall (probe also
// hangs) or an app-layer pool issue (probe works, app doesn't).

func dsn() string {
	return "rawimport:rawpw@tcp(host.docker.internal:3307)/rawimport?parseTime=true&collation=utf8mb4_unicode_ci&timeout=5s&readTimeout=8s&writeTimeout=8s"
}

// openLikeApp mirrors OpenDB exactly (pool=10, Ping retry loop).
func openLikeApp() (*sql.DB, error) {
	db, err := sql.Open("mysql", dsn())
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(10)
	db.SetMaxIdleConns(5)
	db.SetConnMaxLifetime(5 * time.Minute)
	for i := 0; i < 10; i++ {
		if err = db.Ping(); err == nil {
			break
		}
		time.Sleep(time.Second)
	}
	return db, err
}

func run(name string, fn func(db *sql.DB) error) {
	db, err := openLikeApp()
	if err != nil {
		fmt.Printf("[%s] OPEN FAIL: %v\n", name, err)
		return
	}
	defer db.Close()
	start := time.Now()
	done := make(chan error, 1)
	go func() { done <- fn(db) }()
	select {
	case e := <-done:
		dt := time.Since(start)
		if e != nil {
			fmt.Printf("[%s] FAIL (%v): %v\n", name, dt, e)
		} else {
			fmt.Printf("[%s] PASS (%v)\n", name, dt)
		}
	case <-time.After(6 * time.Second):
		fmt.Printf("[%s] HANG (>6s) — app-pool stall confirmed\n", name)
	}
}

func main() {
	// 0. Simulate the app's startup: run DDL (like Migrate) on a
	// long-lived pool, then serve queries from the SAME pool. This
	// reproduces the app's exact connection lifecycle.
	func() {
		db, err := openLikeApp()
		if err != nil {
			fmt.Printf("[sim-migrate] OPEN FAIL: %v\n", err)
			return
		}
		defer db.Close()
		// DDL + write, exactly like Migrate + AllocateSequence fallback.
		db.Exec("CREATE TABLE IF NOT EXISTS probe_t (id BIGINT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(32)) ENGINE=InnoDB")
		db.Exec("INSERT INTO probe_t (name) VALUES ('x')")
		db.Exec("INSERT INTO schema_migrations (version) VALUES ('probe')")
		// Now serve the sequences query from this SAME pooled conn.
		start := time.Now()
		done := make(chan error, 1)
		go func() {
			var id int64
			done <- db.QueryRow("SELECT id FROM sequences WHERE name = ?", "IMG_9001").Scan(&id)
		}()
		select {
		case e := <-done:
			fmt.Printf("[sim-migrate] sequences query: %v (%v)\n", e, time.Since(start))
		case <-time.After(6 * time.Second):
			fmt.Printf("[sim-migrate] HANG (>6s) — migration-pool-state theory CONFIRMED\n")
		}
	}()

	// 1. sequences single-equality (the query GetImportBySequence does first)
	run("seq-name", func(db *sql.DB) error {
		var id int64
		return db.QueryRow("SELECT id FROM sequences WHERE name = ?", "IMG_9001").Scan(&id)
	})

	// 2. imports OR on char(64) (GetImportByHash)
	run("hash-or", func(db *sql.DB) error {
		var id int64
		return db.QueryRow("SELECT id FROM imports WHERE source_hash = ? OR output_hash = ? LIMIT 1", "l1out", "l1out").Scan(&id)
	})

	// 3. JOIN sequences (the proposed fix shape)
	run("join", func(db *sql.DB) error {
		var id int64
		return db.QueryRow("SELECT imports.id FROM imports JOIN sequences ON sequences.id = imports.sequence_id WHERE sequences.name = ?", "IMG_9001").Scan(&id)
	})

	// 4. imports single-equality on text (by-source, known-good)
	run("src-eq", func(db *sql.DB) error {
		var id int64
		return db.QueryRow("SELECT id FROM imports WHERE source_path = ? LIMIT 1", "/watch/L1.NRW").Scan(&id)
	})

	// 5. two sequential QueryRows on same pool (current GetImportBySequence pattern)
	run("two-qr", func(db *sql.DB) error {
		var sid int64
		if err := db.QueryRow("SELECT id FROM sequences WHERE name = ?", "IMG_9001").Scan(&sid); err != nil {
			return err
		}
		var id int64
		return db.QueryRow("SELECT id FROM imports WHERE sequence_id = ?", sid).Scan(&id)
	})

	fmt.Println("probe done")
}