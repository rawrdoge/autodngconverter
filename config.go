package main

import (
	"os"
	"strconv"
	"time"
)

// Config holds all runtime configuration, sourced from environment variables
// per PRD §6 (Docker env) and §4.2.
type Config struct {
	WatchDir    string
	OutputDir   string
	ArchiveDir  string
	DBDriver    string // mariadb (v1 only)
	DBHost      string
	DBPort      int
	DBUser      string
	DBPassword  string
	DBName      string
	ConvEngine  string // dnglab (default) | libraw (deferred) | adobedng (opt-in)
	DnglabBin   string
	AdobeExe    string
	WinePrefix  string
	ExifTool    string // path to exiftool binary (PRD Q5)
	PollInterval time.Duration
	FolderSchema string
	FilePattern  string
	LogLevl      string
	AlertPushURL string
	Port         string
	APIToken     string // optional bearer token for notify endpoints (PRD Q8)
	GenThumbJPEG bool   // write a standalone IMG_n.thumb.jpg sidecar to /output (default false)
	// Default conversion settings applied to every new import (reconversions can override).
	DefCompression   string
	DefPreviewMedium string
	DefPreviewFull   string
	DefVersion       string
	DefJpegQuality   int
	DefLinear        bool
}

func env(key, def string) string {
	if v, ok := os.LookupEnv(key); ok && v != "" {
		return v
	}
	return def
}

func envInt(key string, def int) int {
	if v, ok := os.LookupEnv(key); ok && v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return def
}

// LoadConfig reads configuration from the environment with PRD defaults.
func LoadConfig() Config {
	poll := envInt("POLL_INTERVAL", 10)
	return Config{
		WatchDir:     env("WATCH_DIR", "/watch"),
		OutputDir:    env("OUTPUT_DIR", "/output"),
		ArchiveDir:   env("ARCHIVE_DIR", "/archive"),
		DBDriver:     env("DB_DRIVER", "mariadb"),
		DBHost:       env("DB_HOST", "mariadb"),
		DBPort:       envInt("DB_PORT", 3306),
		DBUser:       env("DB_USER", ""),
		DBPassword:   env("DB_PASSWORD", ""),
		DBName:       env("DB_NAME", "rawimport"),
		ConvEngine:   env("CONVERTER_ENGINE", "dnglab"),
		DnglabBin:    env("DNGLAB_BIN", "dnglab"),
		AdobeExe:     env("ADOBE_INSTALLER_PATH", "/wine/AdobeDNGConverter.exe"),
		WinePrefix:   env("WINEPREFIX", "/wine"),
		ExifTool:    env("EXIFTOOL_BIN", "exiftool"),
		PollInterval: time.Duration(poll) * time.Second,
		FolderSchema: env("FOLDER_SCHEMA", "%Y/%m"),
		FilePattern:  env("FILE_PATTERN", "IMG_{seq}"),
		LogLevl:      env("LOG_LEVEL", "info"),
		AlertPushURL: env("ALERT_PUSH_URL", ""),
		Port:         env("PORT", "8080"),
		APIToken:     env("API_TOKEN", ""),
		GenThumbJPEG: envBool("GEN_THUMB_JPEG", false),
		DefCompression:   env("DEF_COMPRESSION", "lossless"),
		DefPreviewMedium: env("DEF_PREVIEW_MEDIUM", "1024x1024"),
		DefPreviewFull:   env("DEF_PREVIEW_FULL", "4000x3000"),
		DefVersion:       env("DEF_DNG_VERSION", "1.4"),
		DefJpegQuality:   envInt("DEF_JPEG_QUALITY", 92),
		DefLinear:        envBool("DEF_LINEAR", false),
	}
}

func envBool(key string, def bool) bool {
	if v, ok := os.LookupEnv(key); ok && v != "" {
		b, err := strconv.ParseBool(v)
		if err == nil {
			return b
		}
	}
	return def
}
