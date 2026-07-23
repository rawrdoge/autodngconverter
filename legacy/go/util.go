package main

import (
	"encoding/json"
	"io"
	"os"
	"os/exec"
	"strings"
)

// execCommand runs a command and returns combined stdout bytes.
func execCommand(name string, args ...string) ([]byte, error) {
	cmd := exec.Command(name, args...)
	return cmd.Output()
}

// firstJSONField extracts the first value for a key from a JSON blob
// (exiftool -json output). Minimal, dependency-free.
func firstJSONField(blob, key string) string {
	// exiftool emits a JSON array; find the key quickly.
	idx := strings.Index(blob, "\""+key+"\"")
	if idx < 0 {
		return ""
	}
	rest := blob[idx+len(key)+2:] // skip key + ":" + optional space
	rest = strings.TrimLeft(rest, " ")
	if len(rest) == 0 || rest[0] != '"' {
		return ""
	}
	end := strings.Index(rest[1:], "\"")
	if end < 0 {
		return ""
	}
	return rest[1 : end+1]
}

// prettyJSON is a tiny helper for logging structs.
func prettyJSON(v interface{}) string {
	b, err := json.Marshal(v)
	if err != nil {
		return ""
	}
	return string(b)
}

// jsonUnmarshal wraps json.Unmarshal.
func jsonUnmarshal(b []byte, v interface{}) error {
	return json.Unmarshal(b, v)
}

// copyFile copies from src to dst (used for cross-device moves).
func copyFile(src io.Reader, dst io.Writer) (int64, error) {
	return io.Copy(dst, src)
}

// splitSQL splits a SQL script into individual statements on ";" while
// ignoring lines that are comments or blank (Go MySQL driver runs one
// statement per Exec).
func splitSQL(script string) []string {
	var stmts []string
	var cur strings.Builder
	for _, line := range strings.Split(script, "\n") {
		trimmed := strings.TrimSpace(line)
		if trimmed == "" || strings.HasPrefix(trimmed, "--") {
			continue
		}
		cur.WriteString(line)
		cur.WriteString("\n")
		if strings.HasSuffix(trimmed, ";") {
			stmt := strings.TrimSpace(cur.String())
			stmt = strings.TrimSuffix(stmt, ";")
			if stmt != "" {
				stmts = append(stmts, stmt)
			}
			cur.Reset()
		}
	}
	if strings.TrimSpace(cur.String()) != "" {
		stmts = append(stmts, strings.TrimSpace(cur.String()))
	}
	return stmts
}

// loadDotEnv reads a local ".env" file (KEY=VALUE lines) into the environment
// so live MariaDB credentials can be supplied without polluting the image.
// Only setenv if the key is not already present (env wins).
func loadDotEnv() {
	data, err := os.ReadFile(".env")
	if err != nil {
		return // no .env is fine (container uses real env)
	}
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		parts := strings.SplitN(line, "=", 2)
		if len(parts) != 2 {
			continue
		}
		k := strings.TrimSpace(parts[0])
		v := strings.TrimSpace(parts[1])
		if _, exists := os.LookupEnv(k); !exists {
			os.Setenv(k, v)
		}
	}
}
