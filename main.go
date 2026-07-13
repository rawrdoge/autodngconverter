package main

import (
	"context"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	loadDotEnv() // read ./env if present (local dev / live DB creds)
	cfg := LoadConfig()
	log.Printf("rawimport-pipeline starting (engine=%s, watch=%s)", cfg.ConvEngine, cfg.WatchDir)

	engine, err := NewEngine(cfg.ConvEngine, cfg)
	if err != nil {
		log.Fatalf("engine init: %v", err)
	}
	if !engine.Available() {
		log.Fatalf("converter engine %q not available", engine.Name())
	}
	log.Printf("converter engine: %s", engine.Name())

	store, err := OpenDB(cfg)
	if err != nil {
		log.Fatalf("db open: %v", err)
	}
	if err := store.Migrate("migrations"); err != nil {
		log.Fatalf("migrate: %v", err)
	}

	worker := NewWorker(cfg, store, engine)
	// Nomenclature-aware sequencing: register any pre-existing IMG_*.dng in the
	// output volume as 'legacy' placeholders and advance the sequence so new
	// imports never collide with the existing library (PRD §4.2.4, Q13).
	if err := worker.ReconcileLibrary(); err != nil {
		log.Printf("reconcile warning: %v", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	worker.Start(ctx)

	api := NewAPIServer(store, worker, cfg.APIToken)
	go func() {
		addr := ":" + cfg.Port
		log.Printf("API listening on %s", addr)
		if err := api.Start(addr); err != nil {
			log.Printf("api stopped: %v", err)
		}
	}()

	// Graceful shutdown.
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig
	log.Println("shutting down...")
	cancel()
	time.Sleep(500 * time.Millisecond)
	fmt.Println("done")
}