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

// Error logs even in release mode — error-class events (worker panics,
// protocol violations) must never be silenced. Goes through slog.Default()
// so the embedding application controls the sink via slog.SetDefault.
func Error(msg string, args ...any) {
	slog.Default().Error(msg, args...)
}
