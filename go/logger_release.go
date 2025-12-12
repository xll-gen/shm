//go:build !shm_debug

package shm

import "log/slog"

// SetLogger sets the logger for the shm package.
// In release mode, this does nothing, but the signature must match to allow user code to compile.
func SetLogger(l *slog.Logger) {}

// Debug is a no-op in release mode.
// The compiler will inline and remove calls to this function.
func Debug(msg string, args ...any) {}

// Info is a no-op in release mode.
func Info(msg string, args ...any) {}
