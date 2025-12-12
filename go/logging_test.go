//go:build shm_debug

package shm

import (
    "testing"
    "log/slog"
    "bytes"
)

func TestLogging(t *testing.T) {
    var buf bytes.Buffer
    l := slog.New(slog.NewTextHandler(&buf, nil))
    SetLogger(l)

    Info("Test Log Message")

    if !bytes.Contains(buf.Bytes(), []byte("Test Log Message")) {
        t.Fatal("Log message not captured")
    }
}

func TestDebugLog(t *testing.T) {
     var buf bytes.Buffer
     l := slog.New(slog.NewTextHandler(&buf, &slog.HandlerOptions{Level: slog.LevelDebug}))
     SetLogger(l)

     Debug("Debug Message")

     if !bytes.Contains(buf.Bytes(), []byte("Debug Message")) {
         t.Fatal("Debug message not captured")
     }
}
