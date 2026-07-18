package main

import (
	"sort"
	"sync"
	"sync/atomic"
)

// metrics holds the Prometheus series required by PRD §3.6. Implemented without
// an external client library to keep the dependency surface minimal; the output
// is standard Prometheus text exposition format and is scrapable by Prometheus.
var metrics = struct {
	mu                  sync.Mutex
	filesDetected       int64
	completed           int64
	failed              int64
	durCount            int64
	durSum              float64
	durBuckets          [9]int64 // <=1,2,5,10,20,30,60,120,300 (last = +Inf)
	queueDepth          int64
	dbSizeBytes         int64
}{}

// Metrics increment helpers.
func metricFilesDetected(n int64) { atomic.AddInt64(&metrics.filesDetected, n) }
func metricConversionDone(status string, durSec float64) {
	switch status {
	case "failed":
		atomic.AddInt64(&metrics.failed, 1)
	default:
		atomic.AddInt64(&metrics.completed, 1)
	}
	atomic.AddInt64(&metrics.durCount, 1)
	// duration histogram
	edges := []float64{1, 2, 5, 10, 20, 30, 60, 120, 300}
	b := 0
	for b < len(edges) && durSec > edges[b] {
		b++
	}
	atomic.AddInt64(&metrics.durBuckets[b], 1)
	addDurSum(durSec)
}
func addDurSum(v float64) {
	metrics.mu.Lock()
	metrics.durSum += v
	metrics.mu.Unlock()
}
func metricSetQueueDepth(n int64) { atomic.StoreInt64(&metrics.queueDepth, n) }
func metricSetDBSize(n int64)     { atomic.StoreInt64(&metrics.dbSizeBytes, n) }

// renderMetrics produces Prometheus text exposition format.
func renderMetrics() string {
	var b stringsBuilder
	b.WriteString("# TYPE rawimport_files_detected_total counter\n")
	b.WriteString("rawimport_files_detected_total " + itoa(atomic.LoadInt64(&metrics.filesDetected)) + "\n")
	b.WriteString("# TYPE rawimport_conversions_completed_total counter\n")
	b.WriteString("rawimport_conversions_completed_total{status=\"completed\"} " + itoa(atomic.LoadInt64(&metrics.completed)) + "\n")
	b.WriteString("rawimport_conversions_completed_total{status=\"failed\"} " + itoa(atomic.LoadInt64(&metrics.failed)) + "\n")
	b.WriteString("# TYPE rawimport_conversion_duration_seconds histogram\n")
	edges := []string{"1", "2", "5", "10", "20", "30", "60", "120", "300", "+Inf"}
	metrics.mu.Lock()
	cum := int64(0)
	for i, le := range edges {
		cum += atomic.LoadInt64(&metrics.durBuckets[i])
		b.WriteString("rawimport_conversion_duration_seconds_bucket{le=\"" + le + "\"} " + itoa(cum) + "\n")
	}
	b.WriteString("rawimport_conversion_duration_seconds_sum " + ftoa(metrics.durSum) + "\n")
	b.WriteString("rawimport_conversion_duration_seconds_count " + itoa(atomic.LoadInt64(&metrics.durCount)) + "\n")
	metrics.mu.Unlock()
	b.WriteString("# TYPE rawimport_queue_depth gauge\n")
	b.WriteString("rawimport_queue_depth " + itoa(atomic.LoadInt64(&metrics.queueDepth)) + "\n")
	b.WriteString("# TYPE rawimport_db_size_bytes gauge\n")
	b.WriteString("rawimport_db_size_bytes " + itoa(atomic.LoadInt64(&metrics.dbSizeBytes)) + "\n")
	return b.String()
}

// minimal string builders to avoid importing fmt in the hot path.
type stringsBuilder struct {
	buf []byte
}

func (s *stringsBuilder) WriteString(v string) {
	s.buf = append(s.buf, v...)
}
func (s *stringsBuilder) String() string { return string(s.buf) }

func itoa(v int64) string {
	if v == 0 {
		return "0"
	}
	neg := v < 0
	if neg {
		v = -v
	}
	var tmp [20]byte
	i := len(tmp)
	for v > 0 {
		i--
		tmp[i] = byte('0' + v%10)
		v /= 10
	}
	if neg {
		i--
		tmp[i] = '-'
	}
	return string(tmp[i:])
}

func ftoa(v float64) string {
	// 2 decimal places, good enough for a duration sum.
	neg := v < 0
	if neg {
		v = -v
	}
	whole := int64(v)
	frac := int64((v - float64(whole)) * 100)
	s := itoa(whole) + "." + itoa(frac)
	if neg {
		s = "-" + s
	}
	return s
}

// ensure sort import is used (kept for future label sorting if needed).
var _ = sort.Strings