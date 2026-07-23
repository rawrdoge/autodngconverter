package main

import (
	"log"
	"os/exec"
	"sync"
	"time"
)

// RotationManager coalesces spammy rotation intents via a per-image grace
// timer, then dispatches exactly one job that writes EXIF Orientation to the
// DNG and (optionally) notifies Immich/digiKam, all under a processing_locks
// guard. Ported from the C++ rewrite (src/rotation.cpp, PRD §5, ORCH §7.4).
type RotationManager struct {
	cfg   Config
	store *Store

	mu       sync.Mutex
	pending  map[int64]pendingRot
	cond     *sync.Cond
	stop     bool
	worker   *sync.WaitGroup
}

type pendingRot struct {
	orientation int
	dngPath    string
	clientID   string
	deadline   time.Time
}

// NewRotationManager starts the coalescing worker goroutine.
func NewRotationManager(cfg Config, store *Store) *RotationManager {
	m := &RotationManager{
		cfg:    cfg,
		store:  store,
		pending: make(map[int64]pendingRot),
	}
	m.cond = sync.NewCond(&m.mu)
	m.worker = &sync.WaitGroup{}
	m.worker.Add(1)
	go m.run()
	return m
}

// Queue registers a rotation intent for an import, resetting its grace timer.
// orientation must be 1-8 (EXIF).
func (m *RotationManager) Queue(importID int64, dngPath string, orientation int, clientID string) {
	m.mu.Lock()
	m.pending[importID] = pendingRot{
		orientation: orientation,
		dngPath:    dngPath,
		clientID:   clientID,
		deadline:   time.Now().Add(time.Duration(m.cfg.RotationGraceMs) * time.Millisecond),
	}
	m.mu.Unlock()
	m.cond.Broadcast()
	log.Printf("[rotation] queued import_id=%d orient=%d client=%s", importID, orientation, clientID)
}

func (m *RotationManager) run() {
	defer m.worker.Done()
	for {
		m.mu.Lock()
		for !m.stop && len(m.pending) == 0 {
			m.cond.Wait()
		}
		if m.stop && len(m.pending) == 0 {
			m.mu.Unlock()
			return
		}
		// Find soonest deadline.
		now := time.Now()
		var soonest time.Time
		first := true
		for _, r := range m.pending {
			if first || r.deadline.Before(soonest) {
				soonest = r.deadline
				first = false
			}
		}
		if soonest.After(now) {
			m.cond.Wait()
			m.mu.Unlock()
			continue
		}
		// Dispatch all due.
		for id, r := range m.pending {
			if !r.deadline.After(now) {
				delete(m.pending, id)
				m.mu.Unlock()
				m.dispatch(id, r)
				m.mu.Lock()
			}
		}
		m.mu.Unlock()
	}
}

func (m *RotationManager) dispatch(importID int64, r pendingRot) {
	// Lock keyed by the unique DNG path so concurrent rotation of different
	// files does not serialize.
	if ok, _ := m.store.AcquireLock(r.dngPath, r.dngPath, "rotation-mgr", 30*time.Second); !ok {
		log.Printf("[rotation] lock busy for import_id=%d, skip", importID)
		return
	}
	defer m.store.ReleaseLock(r.dngPath)

	cmd := exec.Command(m.cfg.ExifTool, "-Orientation="+itoaIface(r.orientation), "-n", "-overwrite_original_in_place", r.dngPath)
	if out, err := cmd.CombinedOutput(); err != nil {
		log.Printf("[rotation] exiftool failed for %s (rc=%v): %s", r.dngPath, err, string(out))
	} else {
		if err := m.store.UpdateOrientation(importID, r.orientation); err != nil {
			log.Printf("[rotation] UpdateOrientation failed for %d: %v", importID, err)
		} else {
			log.Printf("[rotation] wrote orientation %d to %s", r.orientation, r.dngPath)
		}
	}
	if m.cfg.ImmichURL != "" {
		// Best-effort Immich sidecar rescan webhook.
		_ = exec.Command("curl", "-s", "-o", "/dev/null", "-X", "POST", m.cfg.ImmichURL+"/api/jobs",
			"-H", "Content-Type: application/json", "-d", `{"type":"sidecar"}`).Run()
	}
	if m.cfg.DigikamRescan {
		_ = exec.Command("touch", "/tmp/digikam_rescan.trigger").Run()
	}
}

// Stop flushes any remaining pending intents and joins the worker.
func (m *RotationManager) Stop() {
	m.mu.Lock()
	m.stop = true
	m.mu.Unlock()
	m.cond.Broadcast()
	m.worker.Wait()
	// Dispatch remaining immediately.
	m.mu.Lock()
	remaining := make(map[int64]pendingRot, len(m.pending))
	for k, v := range m.pending {
		remaining[k] = v
	}
	m.pending = nil
	m.mu.Unlock()
	for id, r := range remaining {
		m.dispatch(id, r)
	}
	log.Printf("[rotation] stopped, %d pending dispatched", len(remaining))
}

// itoaIface is a tiny int->string for exiftool flag building (avoids fmt import here).
func itoaIface(v int) string {
	if v == 0 {
		return "0"
	}
	neg := v < 0
	if neg {
		v = -v
	}
	var t [20]byte
	i := len(t)
	for v > 0 {
		i--
		t[i] = byte('0' + v%10)
		v /= 10
	}
	if neg {
		i--
		t[i] = '-'
	}
	return string(t[i:])
}