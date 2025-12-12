//go:build shm_debug

package shm

import (
	"log/slog"
	"os"
)

var defaultLogger = slog.New(slog.NewTextHandler(os.Stdout, nil))

// SetLogger sets the logger for the shm package.
func SetLogger(l *slog.Logger) {
	defaultLogger = l
}

// Debug logs a message at Debug level.
func Debug(msg string, args ...any) {
	defaultLogger.Debug(msg, args...)
}

// Info logs a message at Info level.
func Info(msg string, args ...any) {
	defaultLogger.Info(msg, args...)
}
