package main

import (
	"context"
	"net/http"
	"strconv"

	"github.com/labstack/echo/v4"
)

// APIServer exposes the REST endpoints (PRD §4.2.6, §7). No auth in v1 (Q7).
type APIServer struct {
	store       *Store
	worker      *Worker
	rotationMgr *RotationManager
	echo        *echo.Echo
	token       string // optional bearer token for notify endpoints (PRD Q8)
}

func NewAPIServer(store *Store, worker *Worker, rotationMgr *RotationManager, token string) *APIServer {
	e := echo.New()
	e.HideBanner = true
	s := &APIServer{store: store, worker: worker, rotationMgr: rotationMgr, echo: e, token: token}
	s.routes()
	return s
}

// notifyAuth guards the notify endpoints. If no API_TOKEN is configured, the
// endpoints are open (LAN-only, per PRD §8.3). Otherwise a Bearer token is
// required (PRD Q8).
func (s *APIServer) notifyAuth(next echo.HandlerFunc) echo.HandlerFunc {
	return func(c echo.Context) error {
		if s.token == "" {
			return next(c)
		}
		auth := c.Request().Header.Get("Authorization")
		const prefix = "Bearer "
		if len(auth) <= len(prefix) || auth[:len(prefix)] != prefix || auth[len(prefix):] != s.token {
			return c.JSON(http.StatusUnauthorized, map[string]string{"error": "unauthorized"})
		}
		return next(c)
	}
}

func (s *APIServer) routes() {
	s.echo.GET("/health", s.health)
	s.echo.GET("/api/v1/imports", s.listImports)
	s.echo.GET("/api/v1/imports/:sequence", s.getImport)
	s.echo.GET("/api/v1/imports/hash/:sha", s.getByHash)
	s.echo.POST("/api/v1/imports/:sequence/reconvert", s.reconvert)
	s.echo.GET("/api/v1/stats", s.stats)
	s.echo.GET("/api/v1/alerts", s.alerts)
	// Notify endpoints (PRD Q8): called by the Darktable Lua script after a
	// preview re-embed so the DB output_hash stays in sync (no false corruption).
	s.echo.POST("/api/v1/imports/by-path/preview-updated", s.previewUpdated, s.notifyAuth)
	// Resolve a source RAW path -> its DNG output path (used by the Darktable
	// export-hook to find which DNG should receive a re-embedded preview).
	s.echo.GET("/api/v1/imports/by-source", s.bySource)
	// Rotation / orientation sync (PRD §5, ORCH §7.4). Coalesced by RotationManager.
	s.echo.POST("/api/v1/imports/by-source/rotation-updated", s.rotationUpdated, s.notifyAuth)
	// Prometheus metrics (PRD §3.6).
	s.echo.GET("/metrics", s.metrics)
}

func (s *APIServer) health(c echo.Context) error {
	return c.JSON(http.StatusOK, map[string]string{"status": "ok"})
}

func (s *APIServer) listImports(c echo.Context) error {
	page, _ := strconv.Atoi(c.QueryParam("page"))
	if page < 1 {
		page = 1
	}
	limit, _ := strconv.Atoi(c.QueryParam("limit"))
	if limit < 1 {
		limit = 50
	}
	status := c.QueryParam("status")
	recs, total, err := s.store.ListImports(page, limit, status)
	if err != nil {
		return c.JSON(http.StatusInternalServerError, map[string]string{"error": err.Error()})
	}
	return c.JSON(http.StatusOK, map[string]interface{}{
		"total": total, "page": page, "limit": limit, "data": recs,
	})
}

func (s *APIServer) getImport(c echo.Context) error {
	seq := c.Param("sequence")
	rec, err := s.store.GetImportBySequence(seq)
	if err != nil {
		return c.JSON(http.StatusNotFound, map[string]string{"error": "not found"})
	}
	return c.JSON(http.StatusOK, rec)
}

func (s *APIServer) getByHash(c echo.Context) error {
	sha := c.Param("sha")
	rec, err := s.store.GetImportByHash(sha)
	if err != nil {
		return c.JSON(http.StatusNotFound, map[string]string{"error": "not found"})
	}
	return c.JSON(http.StatusOK, rec)
}

type reconvReq struct {
	ConversionSettings ConversionSettings `json:"conversion_settings"`
	Reason             string              `json:"reason"`
}

func (s *APIServer) reconvert(c echo.Context) error {
	seq := c.Param("sequence")
	rec, err := s.store.GetImportBySequence(seq)
	if err != nil {
		return c.JSON(http.StatusNotFound, map[string]string{"error": "not found"})
	}
	var body reconvReq
	if err := c.Bind(&body); err != nil {
		return c.JSON(http.StatusBadRequest, map[string]string{"error": err.Error()})
	}
	id, err := s.store.InsertReconversion(rec.ID, rec.OutputHash, prettyJSON(body.ConversionSettings))
	if err != nil {
		return c.JSON(http.StatusInternalServerError, map[string]string{"error": err.Error()})
	}
	// Trigger immediate drain (single worker). Use a background context so
	// the job survives after the HTTP response is sent (PRD §5.2).
	go s.worker.ProcessReconversions(context.Background())
	return c.JSON(http.StatusOK, map[string]interface{}{
		"reconversion_id": id, "sequence": seq, "status": "pending",
		"queued_at": "now",
	})
}

func (s *APIServer) stats(c echo.Context) error {
	st, err := s.store.Stats()
	if err != nil {
		return c.JSON(http.StatusInternalServerError, map[string]string{"error": err.Error()})
	}
	return c.JSON(http.StatusOK, st)
}

func (s *APIServer) alerts(c echo.Context) error {
	// Simple passthrough to a stored query; reuse ListImports-style not needed.
	rows, err := s.store.recentAlerts()
	if err != nil {
		return c.JSON(http.StatusInternalServerError, map[string]string{"error": err.Error()})
	}
	return c.JSON(http.StatusOK, map[string]interface{}{"data": rows})
}

func (s *APIServer) Start(addr string) error {
	return s.echo.Start(addr)
}

// previewUpdated is the notify endpoint (PRD Q8). The Darktable Lua script
// calls it after re-embedding a preview so the DB output_hash is updated and
// the corruption monitor does not false-positive. Body:
//
//	{ "output_path": "...", "worker": "exiftool|dnglab|dng_sdk",
//	  "preview_width": 2048, "preview_height": 2048, "preview_quality": 90 }
func (s *APIServer) previewUpdated(c echo.Context) error {
	var body struct {
		OutputPath    string `json:"output_path"`
		Worker        string `json:"worker"`
		PreviewWidth  int    `json:"preview_width"`
		PreviewHeight int    `json:"preview_height"`
		PreviewQuality int   `json:"preview_quality"`
	}
	if err := c.Bind(&body); err != nil {
		return c.JSON(http.StatusBadRequest, map[string]string{"error": err.Error()})
	}
	if body.OutputPath == "" {
		return c.JSON(http.StatusBadRequest, map[string]string{"error": "output_path required"})
	}
	rec, err := s.store.GetImportByOutputPath(body.OutputPath)
	if err != nil {
		return c.JSON(http.StatusNotFound, map[string]string{"error": "no import for path"})
	}
	newHash, err := sha256File(body.OutputPath)
	if err != nil {
		return c.JSON(http.StatusInternalServerError, map[string]string{"error": "hash failed: " + err.Error()})
	}
	worker := body.Worker
	if worker == "" {
		worker = "exiftool"
	}
	if err := s.store.RecordPreviewEdit(PreviewEdit{
		ImportID:       rec.ID,
		Worker:         worker,
		PrevHash:       rec.OutputHash,
		NewHash:        newHash,
		PreviewWidth:   body.PreviewWidth,
		PreviewHeight:  body.PreviewHeight,
		PreviewQuality: body.PreviewQuality,
	}); err != nil {
		return c.JSON(http.StatusInternalServerError, map[string]string{"error": err.Error()})
	}
	return c.JSON(http.StatusOK, map[string]interface{}{
		"sequence":         rec.SequenceName,
		"previous_hash":    rec.OutputHash,
		"new_output_hash":  newHash,
		"last_preview_edit_at": "now",
	})
}

// bySource resolves a source RAW path to its import record so the Darktable
// export-hook can locate the DNG that should receive a re-embedded preview.
// Query: ?path=<source RAW path>. Returns the full record (incl. output_path).
func (s *APIServer) bySource(c echo.Context) error {
	path := c.QueryParam("path")
	if path == "" {
		return c.JSON(http.StatusBadRequest, map[string]string{"error": "path required"})
	}
	rec, err := s.store.GetImportBySourcePath(path)
	if err != nil {
		return c.JSON(http.StatusNotFound, map[string]string{"error": "no import for source path"})
	}
	return c.JSON(http.StatusOK, rec)
}

// rotationUpdated is the Darktable orientation-sync notify endpoint (PRD §5,
// ORCH §7.4). Body: { "source_path": "<RAW path>", "orientation": <1-8>,
// "client_id": "<id>" }. The RotationManager coalesces rapid intents for the
// same file into a single exiftool rewrite (last orientation wins).
func (s *APIServer) rotationUpdated(c echo.Context) error {
	if s.rotationMgr == nil {
		return c.JSON(http.StatusServiceUnavailable, map[string]string{"error": "rotation disabled"})
	}
	var body struct {
		SourcePath string `json:"source_path"`
		Orientation int    `json:"orientation"`
		ClientID    string `json:"client_id"`
	}
	if err := c.Bind(&body); err != nil {
		return c.JSON(http.StatusBadRequest, map[string]string{"error": err.Error()})
	}
	if body.SourcePath == "" {
		return c.JSON(http.StatusBadRequest, map[string]string{"error": "source_path required"})
	}
	if body.Orientation < 1 || body.Orientation > 8 {
		return c.JSON(http.StatusBadRequest, map[string]string{"error": "orientation must be 1-8"})
	}
	rec, err := s.store.GetImportBySourcePath(body.SourcePath)
	if err != nil {
		return c.JSON(http.StatusNotFound, map[string]string{"error": "no import for source path"})
	}
	s.rotationMgr.Queue(rec.ID, rec.OutputPath, body.Orientation, body.ClientID)
	return c.JSON(http.StatusOK, map[string]string{"status": "queued"})
}

// metrics exposes Prometheus-format series (PRD §3.6).
func (s *APIServer) metrics(c echo.Context) error {
	c.Response().Header().Set(echo.HeaderContentType, "text/plain; version=0.0.4")
	return c.String(http.StatusOK, renderMetrics())
}
