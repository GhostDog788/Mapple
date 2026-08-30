package main

import (
	"context"
	"crypto/tls"
	"flag"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/GhostDog788/Mapple/user/server"
)

func main() {
	configPath := flag.String("config", "/etc/mapple/mappled.json", "path to mappled config JSON")
	flag.Parse()

	logger := slog.New(slog.NewJSONHandler(os.Stdout, nil))

	if err := run(*configPath, logger); err != nil {
		logger.Error("mappled exiting", "error", err)
		os.Exit(1)
	}
}

func run(configPath string, logger *slog.Logger) error {
	cfg, err := server.LoadConfig(configPath)
	if err != nil {
		return fmt.Errorf("load config: %w", err)
	}

	sessions := server.NewSessionManager(cfg.HandshakeTimeout, cfg.LeaseTimeout)
	defer sessions.Stop()

	srv := server.New(cfg, sessions, logger)

	httpServer := &http.Server{
		Addr:              cfg.ListenAddress,
		Handler:           srv.Handler(),
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       30 * time.Second,
		IdleTimeout:       120 * time.Second,
		// No WriteTimeout: module commands may run indefinitely (plan's
		// "apply no timeout to normal commands").
		TLSConfig: &tls.Config{MinVersion: tls.VersionTLS12},
	}

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer stop()

	serveErr := make(chan error, 1)
	go func() {
		logger.Info("mappled listening", "address", cfg.ListenAddress)
		serveErr <- httpServer.ListenAndServeTLS(cfg.TLSCertFile, cfg.TLSKeyFile)
	}()

	select {
	case err := <-serveErr:
		if err != nil && err != http.ErrServerClosed {
			return fmt.Errorf("listen: %w", err)
		}
	case <-ctx.Done():
		logger.Info("shutting down: stopping new requests")
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		if err := httpServer.Shutdown(shutdownCtx); err != nil {
			logger.Warn("graceful shutdown timed out, closing listener", "error", err)
			httpServer.Close()
		}
		logger.Info("shutting down: force-closing all module sessions")
		sessions.CloseAll()
		<-serveErr
	}
	return nil
}
