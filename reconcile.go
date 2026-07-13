package main

import (
	"fmt"
	"log"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

// imgNameRe matches our nomenclature: IMG_{n}.dng (no leading zeros, PRD §4.2.4).
var imgNameRe = regexp.MustCompile(`^IMG_(\d+)\.dng$`)

// ReconcileLibrary implements nomenclature-aware sequencing (PRD §4.2.4, Q13).
// On startup it scans the output AND archive volumes for pre-existing
// IMG_{n}.dng files that are not yet in the database. Each is registered as a
// 'legacy' placeholder so the global sequence continues cleanly after the
// highest existing number and newly imported RAW files never collide with, or
// get re-converted over, the existing library. A warning alert is raised when a
// populated library is found because RAW->DNG provenance cannot be reconstructed
// without re-converting.
func (w *Worker) ReconcileLibrary() error {
	// Scan both the live output tree and the archived tree; a populated library
	// may live in either (or both) depending on retention policy.
	roots := []string{w.cfg.OutputDir, w.cfg.ArchiveDir}
	var matches []sequencedDNG
	for _, root := range roots {
		found, err := findSequencedDNGs(root)
		if err != nil {
			// A missing/unmounted volume is not fatal; just skip it.
			log.Printf("reconcile: scan skipped for %s (%v)", root, err)
			continue
		}
		matches = append(matches, found...)
	}
	if len(matches) == 0 {
		log.Printf("reconcile: no pre-existing IMG_*.dng found in %s or %s", w.cfg.OutputDir, w.cfg.ArchiveDir)
		return nil
	}

	// Sort by numeric sequence so we register in order and can report the max.
	sort.Slice(matches, func(i, j int) bool { return matches[i].n < matches[j].n })

	registered := 0
	var maxN int64
	for _, m := range matches {
		if m.n > maxN {
			maxN = m.n
		}
		// Idempotency: skip if this output path is already recorded.
		exists, err := w.store.HasOutputHash(m.outputHash)
		if err != nil {
			log.Printf("reconcile: db error for %s: %v", m.path, err)
			continue
		}
		if exists {
			continue
		}
		if err := w.registerLegacy(m); err != nil {
			log.Printf("reconcile: register failed %s: %v", m.path, err)
			continue
		}
		registered++
	}

	// Reserve the sequence up to maxN so AllocateSequence never reuses a number
	// that already exists on disk (PRD §4.2.4 collision handling).
	if maxN > 0 {
		if err := w.reserveSequencesUpTo(maxN); err != nil {
			log.Printf("reconcile: sequence reserve error: %v", err)
		}
	}

	// Warn the operator that a populated library was detected and RAWs cannot
	// be systematically matched without re-converting.
	msg := fmt.Sprintf(
		"Existing photo library detected (output=%s, archive=%s): found %d pre-converted IMG_*.dng file(s) "+
			"(highest IMG_%d). Registered %d as unmatched 'legacy' placeholders. "+
			"The library cannot be systematically matched to RAW sources without re-converting; "+
			"new RAW imports will continue cleanly from IMG_%d onward with verified raw-to-DNG hashes.",
		w.cfg.OutputDir, w.cfg.ArchiveDir, len(matches), maxN, registered, maxN+1)
	if err := w.store.InsertAlert("warning", "legacy_library", msg, ""); err != nil {
		log.Printf("reconcile: alert insert error: %v", err)
	}
	log.Printf("reconcile: %s", msg)
	return nil
}

// sequencedDNG is one discovered pre-existing DNG in the output volume.
type sequencedDNG struct {
	n          int64
	path       string
	outputHash string
}

// findSequencedDNGs walks the output tree (recursively, since folder schema is
// YYYY/MM) and returns every file matching IMG_{n}.dng with its SHA-256.
func findSequencedDNGs(root string) ([]sequencedDNG, error) {
	var out []sequencedDNG
	err := filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		name := d.Name()
		m := imgNameRe.FindStringSubmatch(name)
		if m == nil {
			return nil
		}
		n, e := strconv.ParseInt(m[1], 10, 64)
		if e != nil {
			return nil
		}
		h, he := sha256File(path)
		if he != nil {
			// Unreadable file: skip but log.
			log.Printf("reconcile: cannot hash %s: %v", path, he)
			return nil
		}
		out = append(out, sequencedDNG{n: n, path: path, outputHash: h})
		return nil
	})
	if err != nil {
		if os.IsNotExist(err) {
			return nil, err
		}
		return nil, err
	}
	return out, nil
}

// registerLegacy writes a single 'legacy' placeholder import for a pre-existing
// DNG. The source RAW is unknown, so we synthesize a non-SHA256 source hash
// (prefix "legacy:") that is unique per sequence and will never collide with a
// real 64-hex SHA-256. The placeholder is left for manual RAW matching later.
func (w *Worker) registerLegacy(m sequencedDNG) error {
	seqID, seqName, err := w.store.AllocateSequence()
	if err != nil {
		return err
	}
	// Defensive: if the allocated name doesn't match the on-disk number (e.g.
	// sequence was already ahead), still record under the on-disk name by
	// using the discovered number for the placeholder label.
	if seqName != fmt.Sprintf("IMG_%d", m.n) {
		log.Printf("reconcile: allocated %s but file is %s; using file number for placeholder", seqName, fmt.Sprintf("IMG_%d", m.n))
	}
	rec := ImportRecord{
		SequenceID:   seqID,
		SequenceName: fmt.Sprintf("IMG_%d", m.n),
		SourcePath:   "", // unknown RAW; manual match later
		SourceHash:   fmt.Sprintf("legacy:IMG_%d", m.n),
		OutputPath:   m.path,
		OutputHash:   m.outputHash,
		FolderSchema: folderOf(m.path, w.cfg.OutputDir),
		ConversionSettings: prettyJSON(ConversionSettings{Compression: "-c", Version: "-dng1.4", Seed: m.outputHash[:16]}),
		Status:       "legacy",
	}
	return w.store.InsertImport(rec)
}

// reserveSequencesUpTo inserts placeholder sequence rows up to n so the next
// AllocateSequence() call returns n+1 and never collides with on-disk files.
func (w *Worker) reserveSequencesUpTo(n int64) error {
	for {
		_, seqName, err := w.store.AllocateSequence()
		if err != nil {
			return err
		}
		cur, e := strconv.ParseInt(strings.TrimPrefix(seqName, "IMG_"), 10, 64)
		if e != nil {
			return e
		}
		if cur >= n {
			return nil
		}
	}
}

// folderOf derives the YYYY/MM folder schema from an output path relative to
// the output root, for display/consistency with normal imports.
func folderOf(path, root string) string {
	rel, err := filepath.Rel(root, path)
	if err != nil {
		return ""
	}
	parts := strings.Split(rel, string(os.PathSeparator))
	if len(parts) >= 2 {
		return filepath.Join(parts[0], parts[1])
	}
	return ""
}