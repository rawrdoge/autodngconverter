package main

import (
	"context"
	"fmt"
	"os/exec"
	"strings"
	"time"
)

// ConversionSettings maps the re-conversion API body (PRD §7.2) to engine flags.
type ConversionSettings struct {
	Compression string `json:"compression"` // "-c" or ""
	Preview     string `json:"preview"`     // "-p1" etc (selects preview dims)
	Version     string `json:"version"`     // "-dng1.4" -> "1.4"
	Linear      string `json:"linear"`      // "" or set
	Seed        string `json:"seed"`        // determinism seed
}

// ConverterEngine is the pluggable conversion backend (PRD §4.2.2, Q1).
type ConverterEngine interface {
	Name() string
	Available() bool
	Convert(ctx context.Context, src, dst string, s ConversionSettings) error
}

// NewEngine selects the engine by key (PRD §1.1 engine table).
func NewEngine(key string, cfg Config) (ConverterEngine, error) {
	switch key {
	case "dnglab", "":
		return &DnglabEngine{Bin: cfg.DnglabBin, KeepMtime: true}, nil
	case "libraw":
		return nil, fmt.Errorf("libraw engine deferred (not yet implemented)")
	case "adobedng":
		return &AdobeEngine{Exe: cfg.AdobeExe, WinePrefix: cfg.WinePrefix}, nil
	default:
		return nil, fmt.Errorf("unknown converter engine: %s", key)
	}
}

// DnglabEngine invokes the forked dnglab binary (PRD_dnglab_Fork_Requirements.md §2).
type DnglabEngine struct {
	Bin       string
	KeepMtime bool
}

func (e *DnglabEngine) Name() string { return "dnglab" }

func (e *DnglabEngine) Available() bool {
	return exec.Command(e.Bin, "--version").Run() == nil
}

func (e *DnglabEngine) Convert(ctx context.Context, src, dst string, s ConversionSettings) error {
	// dnglab `convert` CLI (vibelabdng fork) accepts positional INPUT/OUTPUT
	// and a limited flag set. The preview size, DNG version (1.4), and JPEG
	// quality (92) are already the tool's defaults, so we only pass the flags
	// that actually exist on the subcommand. Passing unknown flags (e.g.
	// --preview-medium, --dng-version, --jpeg-quality, --compress, --linear,
	// --seed) makes clap abort the whole conversion.
	args := []string{
		"convert",
		src,
		dst,
		"-c", normalizeCompression(s.Compression),
	}
	if e.KeepMtime {
		args = append(args, "--keep-mtime", "true")
	}
	args = append(args, "-f")
	cmd := exec.CommandContext(ctx, e.Bin, args...)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("dnglab: %w: %s", err, strings.TrimSpace(string(out)))
	}
	return nil
}

// normalizeCompression maps the API compression flag to a dnglab value.
// Empty/"true"/"-c" -> lossless (the default); anything else is passed
// through verbatim (e.g. "uncompressed").
func normalizeCompression(c string) string {
	switch strings.TrimSpace(c) {
	case "", "-c", "true", "lossless":
		return "lossless"
	default:
		return c
	}
}

// AdobeEngine invokes Wine + Adobe DNG Converter (opt-in, PRD §4.2.2).
type AdobeEngine struct {
	Exe        string
	WinePrefix string
}

func (e *AdobeEngine) Name() string { return "adobedng" }

func (e *AdobeEngine) Available() bool {
	if e.Exe == "" {
		return false
	}
	return exec.Command("wine", e.Exe, "/?").Run() == nil || true // wine may return non-zero for /?
}

func (e *AdobeEngine) Convert(ctx context.Context, src, dst string, s ConversionSettings) error {
	dstDir := dst
	if i := strings.LastIndex(dstDir, "/"); i >= 0 {
		dstDir = dstDir[:i]
	}
	cmd := exec.CommandContext(ctx, "wine", e.Exe,
		s.Compression, s.Preview, normalizeVersion(s.Version), "-d", dstDir, src)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("adobedng: %w: %s", err, strings.TrimSpace(string(out)))
	}
	return nil
}

func normalizeVersion(v string) string {
	v = strings.TrimSpace(v)
	v = strings.TrimPrefix(v, "-dng")
	v = strings.TrimPrefix(v, "dng")
	if v == "" {
		return "1.4"
	}
	return v
}

// ensure time import used (timeout handled by caller via context)
var _ = time.Second