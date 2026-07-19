package main

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"image"
	_ "image/jpeg"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// ImportRecord is the persisted import row (PRD §4.2.5).
type ImportRecord struct {
	ID                int64
	SequenceID        int64
	SequenceName      string
	SourcePath        string
	SourceHash        string
	OutputPath        string
	OutputHash        string
	CameraModel       string
	CaptureDate       string
	CaptureTime       string
	DateSource        string
	FolderSchema      string
	ConversionSettings string
	Status            string
	Orientation       int // EXIF orientation 1-8, synced from Darktable (PRD §5, ORCH §7.4)
	CreatedAt         *time.Time
	CompletedAt       *time.Time
}

// ReconversionJob is a pending re-conversion (PRD §5.2).
type ReconversionJob struct {
	ID          int64
	Sequence    string
	SourcePath  string
	OutputPath  string
	PrevHash    string
	FolderSchema string
	Settings    string
}

// rawExtensions are the watched RAW formats (PRD §3.1).
var rawExtensions = map[string]bool{
	".nrw": true, ".nef": true, ".cr2": true, ".cr3": true,
	".arw": true, ".raf": true, ".rw2": true, ".dng": false,
}

func isRawFile(name string) bool {
	ext := strings.ToLower(filepath.Ext(name))
	v, ok := rawExtensions[ext]
	return ok && v // only true for actual RAW (not dng)
}

// sha256File computes SHA-256 of a file (PRD §4.2.3).
func sha256File(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// extractEXIF reads DateTimeOriginal + camera model via exiftool (PRD Q5, Q3).
// Falls back to file mtime if EXIF is missing/invalid.
func extractEXIF(path, exifTool string) (date time.Time, model string, dateSource string) {
	out, err := execCommand(exifTool, "-DateTimeOriginal", "-Model", "-Make", "-S", "-json", path)
	if err == nil {
		// Minimal parse: look for DateTimeOriginal and Model.
		blob := string(out)
		if dt := firstJSONField(blob, "DateTimeOriginal"); dt != "" {
			if t, e := time.Parse("2006:01:02 15:04:05", dt); e == nil {
				if m := firstJSONField(blob, "Model"); m != "" {
					return t, m, "exif"
				}
				return t, "", "exif"
			}
		}
	}
	// Fallback to mtime.
	info, err := os.Stat(path)
	if err == nil {
		return info.ModTime(), "", "mtime"
	}
	return time.Now(), "", "mtime"
}

// buildFolderSchema returns YYYY/MM from a date (PRD §4.2.4).
func buildFolderSchema(t time.Time) string {
	return fmt.Sprintf("%04d/%02d", t.Year(), int(t.Month()))
}

// extractThumbnail pulls the embedded SubIFD1 (medium) JPEG from a DNG,
// falling back to SubIFD2 (full) (PRD §4.2.7, Q8). Returns the path written
// and whether the fallback was used.
func extractThumbnail(dngPath, thumbPath, exifTool string) (usedFallback bool, err error) {
	// Use exiftool to extract the preview image (SubIFD1) first.
	if out, e := execCommand(exifTool, "-b", "-PreviewImage", dngPath); e == nil && len(out) > 0 {
		if err := os.WriteFile(thumbPath, out, 0644); err == nil {
			if ok := isValidJPEG(thumbPath); ok {
				return false, nil
			}
		}
	}
	// Fallback: JpgFromRaw (SubIFD2).
	if out, e := execCommand(exifTool, "-b", "-JpgFromRaw", dngPath); e == nil && len(out) > 0 {
		if err := os.WriteFile(thumbPath, out, 0644); err == nil {
			if ok := isValidJPEG(thumbPath); ok {
				return true, nil
			}
		}
	}
	return false, fmt.Errorf("no extractable preview in %s", dngPath)
}

// isValidJPEG confirms the file decodes as a JPEG (Q8 corruption signal).
func isValidJPEG(path string) bool {
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()
	_, _, err = image.Decode(f)
	return err == nil
}
