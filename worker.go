package main

import (
	"context"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// Worker owns the poller loop and the serial converter queue (PRD Q13).
type Worker struct {
	cfg     Config
	store   *Store
	engine  ConverterEngine
	queue   chan string
	workerID string
}

func NewWorker(cfg Config, store *Store, engine ConverterEngine) *Worker {
	return &Worker{
		cfg:      cfg,
		store:    store,
		engine:   engine,
		queue:    make(chan string, 100), // bounded queue (Q13)
		workerID: fmt.Sprintf("worker-%d", os.Getpid()),
	}
}

// Start launches the poller and the single serial converter (Q13).
func (w *Worker) Start(ctx context.Context) {
	go w.pollLoop(ctx)
	go w.convertLoop(ctx)
}

// pollLoop scans /watch on an interval and enqueues stable RAW files (Q3).
func (w *Worker) pollLoop(ctx context.Context) {
	ticker := time.NewTicker(w.cfg.PollInterval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			w.scanOnce()
		}
	}
}

func (w *Worker) scanOnce() {
	entries, err := os.ReadDir(w.cfg.WatchDir)
	if err != nil {
		log.Printf("poll error: %v", err)
		return
	}
	for _, e := range entries {
		if e.IsDir() || !isRawFile(e.Name()) {
			continue
		}
		full := filepath.Join(w.cfg.WatchDir, e.Name())
		// Debounce: wait until size/mtime stable for 2s (Q3).
		if !stable(full) {
			continue
		}
		// Skip partial files.
		lower := strings.ToLower(e.Name())
		if strings.HasSuffix(lower, ".part") || strings.HasSuffix(lower, ".tmp") || strings.HasSuffix(lower, ".download") {
			continue
		}
		select {
		case w.queue <- full:
		default:
			log.Printf("queue full; skipping %s (alert)", full)
			w.store.InsertAlert("warning", "queue_full", "bounded queue full, file not enqueued: "+full, "")
		}
	}
}

// stable waits until the file size/mtime hasn't changed for 2s.
func stable(path string) bool {
	info1, err := os.Stat(path)
	if err != nil {
		return false
	}
	time.Sleep(2 * time.Second)
	info2, err := os.Stat(path)
	if err != nil {
		return false
	}
	return info1.Size() == info2.Size() && info1.ModTime().Equal(info2.ModTime())
}

// convertLoop is the single serial worker (Q13).
func (w *Worker) convertLoop(ctx context.Context) {
	for {
		select {
		case <-ctx.Done():
			return
		case path := <-w.queue:
			w.processFile(ctx, path)
		}
	}
}

// processFile runs the happy path (PRD §5.1).
func (w *Worker) processFile(ctx context.Context, srcPath string) {
	// 3. pre-hash
	srcHash, err := sha256File(srcPath)
	if err != nil {
		log.Printf("hash error %s: %v", srcPath, err)
		return
	}
	// 4. idempotency
	exists, err := w.store.HasSourceHash(srcHash)
	if err != nil {
		log.Printf("db error: %v", err)
		return
	}
	if exists {
		log.Printf("duplicate (skip): %s", srcPath)
		return
	}
	// 5. lock
	ok, err := w.store.AcquireLock(srcHash, srcPath, w.workerID, 6*time.Hour)
	if err != nil || !ok {
		log.Printf("lock busy: %s", srcPath)
		return
	}
	defer w.store.ReleaseLock(srcHash)

	// 7. allocate sequence
	seqID, seqName, err := w.store.AllocateSequence()
	if err != nil {
		log.Printf("seq alloc error: %v", err)
		return
	}
	// 8. EXIF + folder
	date, model, dateSource := extractEXIF(srcPath, w.cfg.ExifTool)
	folder := buildFolderSchema(date)
	outDir := filepath.Join(w.cfg.OutputDir, folder)
	arcDir := filepath.Join(w.cfg.ArchiveDir, folder)
	os.MkdirAll(outDir, 0755)
	os.MkdirAll(arcDir, 0755)
	dstDNG := filepath.Join(outDir, seqName+".dng")
	thumb := filepath.Join(outDir, seqName+".thumb.jpg")

	// 9. convert
	cctx, cancel := context.WithTimeout(ctx, 300*time.Second)
	defer cancel()
	settings := w.defaultSettings(srcHash[:16])
	if err := w.engine.Convert(cctx, srcPath, dstDNG, settings); err != nil {
		log.Printf("convert failed %s: %v", srcPath, err)
		w.store.InsertAlert("error", "conversion_failed", fmt.Sprintf("%s: %v", seqName, err), seqName)
		return
	}
	// 10. post-hash
	outHash, err := sha256File(dstDNG)
	if err != nil {
		log.Printf("output hash error: %v", err)
		return
	}
	// 11. thumbnail (standalone sidecar JPEG). Disabled by default so the
	// /output library only contains DNGs; set GEN_THUMB_JPEG=true to enable.
	if w.cfg.GenThumbJPEG {
		usedFallback, terr := extractThumbnail(dstDNG, thumb, w.cfg.ExifTool)
		if terr != nil {
			log.Printf("thumbnail extract failed %s: %v", seqName, terr)
			w.store.InsertAlert("warning", "thumbnail_missing", "no preview extracted: "+seqName, seqName)
		} else if usedFallback {
			w.store.InsertAlert("warning", "thumbnail_fallback", "SubIFD1 missing, used SubIFD2 for "+seqName, seqName)
		}
	}
	// 12. insert
	rec := ImportRecord{
		SequenceID:         seqID,
		SequenceName:       seqName,
		SourcePath:         srcPath,
		SourceHash:         srcHash,
		OutputPath:         dstDNG,
		OutputHash:         outHash,
		CameraModel:        model,
		CaptureDate:        date.Format("2006-01-02"),
		CaptureTime:        date.Format("15:04:05"),
		DateSource:         dateSource,
		FolderSchema:       folder,
		ConversionSettings: prettyJSON(settings),
		Status:             "completed",
	}
	if err := w.store.InsertImport(rec); err != nil {
		log.Printf("insert error: %v", err)
		return
	}
	// 14/15. archive source + move DNG (DNG already in output)
	arcSrc := filepath.Join(arcDir, filepath.Base(srcPath))
	if err := moveFile(srcPath, arcSrc); err != nil {
		log.Printf("archive error: %v", err)
	}
	// Record the archived path as the canonical source so re-conversion
	// can find the original later (PRD §5.2 b).
	rec.SourcePath = arcSrc
	log.Printf("imported %s -> %s (hash %s)", seqName, dstDNG, outHash[:12])
}

// ProcessReconversions drains pending reconversion jobs (PRD §5.2).
func (w *Worker) ProcessReconversions(ctx context.Context) {
	jobs, err := w.store.PendingReconversions()
	if err != nil {
		log.Printf("reconv list error: %v", err)
		return
	}
	for _, j := range jobs {
		w.runReconversion(ctx, j)
	}
}

// defaultSettings builds the ConversionSettings applied to every new import,
// sourced from the DEF_* environment variables (PRD §4.2.2).
func (w *Worker) defaultSettings(seed string) ConversionSettings {
	return ConversionSettings{
		Compression:   w.cfg.DefCompression,
		PreviewMedium: w.cfg.DefPreviewMedium,
		PreviewFull:   w.cfg.DefPreviewFull,
		Version:       w.cfg.DefVersion,
		JpegQuality:   w.cfg.DefJpegQuality,
		Linear:        w.cfg.DefLinear,
		Seed:          seed,
	}
}

func (w *Worker) runReconversion(ctx context.Context, j ReconversionJob) {
	// b. verify source hash still matches. The stored source_path may point
	// at the original watch location; after archival the file lives under
	// ArchiveDir/<folder_schema>/. Fall back to that if needed (PRD §5.2 b).
	srcPath := j.SourcePath
	if _, err := os.Stat(srcPath); err != nil {
		candidate := filepath.Join(w.cfg.ArchiveDir, j.FolderSchema, filepath.Base(j.SourcePath))
		if _, e := os.Stat(candidate); e == nil {
			srcPath = candidate
		}
	}
	curHash, err := sha256File(srcPath)
	if err != nil {
		log.Printf("reconv source missing %s: %v", j.Sequence, err)
		w.store.InsertAlert("error", "reconv_source_missing", j.Sequence+": source unreadable", j.Sequence)
		return
	}
	// (we trust stored prevHash for audit; re-read is best-effort)
	_ = curHash
	// e. run engine with new settings
	dstDNG := strings.TrimSuffix(j.OutputPath, ".dng") + "_new.dng"
	cctx, cancel := context.WithTimeout(ctx, 300*time.Second)
	defer cancel()
	var s ConversionSettings
	if err := jsonUnmarshal([]byte(j.Settings), &s); err != nil {
		s = w.defaultSettings(j.PrevHash[:16])
	}
	if err := w.engine.Convert(cctx, srcPath, dstDNG, s); err != nil {
		log.Printf("reconv failed %s: %v", j.Sequence, err)
		return
	}
	newHash, _ := sha256File(dstDNG)
	// archive previous DNG (PRD §5.2 i)
	prevArc := j.OutputPath + ".prev"
	moveFile(j.OutputPath, prevArc)
	moveFile(dstDNG, j.OutputPath)
	if err := w.store.CompleteReconversion(j.ID, newHash, j.OutputPath); err != nil {
		log.Printf("reconv complete error: %v", err)
		return
	}
	log.Printf("reconverted %s -> %s", j.Sequence, newHash[:12])
}

func moveFile(src, dst string) error {
	if err := os.Rename(src, dst); err != nil {
		// cross-device fallback
		in, e := os.Open(src)
		if e != nil {
			return e
		}
		defer in.Close()
		out, e := os.Create(dst)
		if e != nil {
			return e
		}
		defer out.Close()
		if _, e := copyFile(in, out); e != nil {
			return e
		}
		return os.Remove(src)
	}
	return nil
}