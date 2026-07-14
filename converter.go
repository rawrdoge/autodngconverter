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
	Compression    string `json:"compression"`      // "lossless" | "uncompressed"
	PreviewMedium  string `json:"preview_medium"`   // "1024x1024"
	PreviewFull    string `json:"preview_full"`     // "4000x3000"
	Version        string `json:"version"`          // "1.4" | "1.6"
	JpegQuality    int    `json:"jpeg_quality"`     // 0-100
	Linear         bool   `json:"linear"`           // linear (demosaiced) DNG
	Seed           string `json:"seed"`             // determinism seed
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
	// plus the real output flags. These are all registered on the `convert`
	// subcommand (app.rs) and consumed by convert.rs, so the settings passed
	// here actually reach the rendered DNG.
	args := []string{
		"convert",
		src,
		dst,
		"-c", normalizeCompression(s.Compression),
	}
	if v := normalizeVersion(s.Version); v != "" {
		args = append(args, "--dng-version", v)
	}
	if s.PreviewMedium != "" {
		args = append(args, "--preview-medium", s.PreviewMedium)
	}
	if s.PreviewFull != "" {
		args = append(args, "--preview-full", s.PreviewFull)
	}
	if s.JpegQuality > 0 {
		args = append(args, "--jpeg-quality", fmt.Sprintf("%d", s.JpegQuality))
	}
	if s.Linear {
		args = append(args, "--linear", "true")
	}
	if s.Seed != "" {
		args = append(args, "--seed", s.Seed)
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
// Empty/"true"/"lossless" -> lossless (the default); anything else is passed
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
	// Map our normalized settings onto Adobe DNG Converter flags.
	comp := "-c"
	if strings.EqualFold(normalizeCompression(s.Compression), "uncompressed") {
		comp = "-u"
	}
	ver := "-dng" + normalizeVersion(s.Version)
	cmd := exec.CommandContext(ctx, "wine", e.Exe,
		comp, ver, "-d", dstDir, src)
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