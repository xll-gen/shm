package shm

import (
	"os"
	"path/filepath"
	"regexp"
	"testing"
)

// cppHeaderRelPath is the C++ half of the SHM_VERSION SSOT pair, relative to
// this package directory. Keep the traversal explicit rather than walking up
// looking for a repo marker: if the layout changes, this test must FAIL, not
// quietly search elsewhere and find some other header.
const cppHeaderRelPath = "../include/shm/IPCUtils.h"

// shmVersionLiteral matches `static const uint32_t SHM_VERSION = 0x00080000;`
// and captures the hex literal. It deliberately does NOT accept a decimal
// literal or a macro: the header has always written this as a hex constant, and
// a shape change here should be looked at by a human rather than silently
// tolerated.
var shmVersionLiteral = regexp.MustCompile(`(?m)^\s*static\s+const\s+uint32_t\s+SHM_VERSION\s*=\s*(0[xX][0-9a-fA-F]+)\s*;`)

// TestSHMVersion_MatchesCppHeader is the CROSS-LANGUAGE guard on the
// SHM_VERSION SSOT pair (C++ include/shm/IPCUtils.h, Go shm.Version in
// go/direct.go). AGENTS.md §Codebase Authority: a mismatch is not a soft
// degradation, it is a total attach failure ("protocol version mismatch"), so a
// half-applied bump takes the product down.
//
// WHY THIS TEST EXISTS AT ALL, given tests/test_protocol_version.cpp already
// asserts a version. That test compares the C++ constant against a constant
// C++ itself wrote — it is tautological across the language boundary and cannot
// see a half-applied bump. The only test that could was
// tests/test_stream_integration, which returns SKIP (77) whenever cmake's
// find_program(NAMES go) misses — i.e. on the common C++-only build. During the
// 0x00080000 work the bump WAS half-applied (IPCUtils.h at 0x00080000,
// go/direct.go still 0x00070000) and BOTH suites reported green; it was caught
// only because a Go toolchain happened to be on PATH. This test reads the other
// language's source text directly, so it cannot self-disable.
//
// IT MUST FAIL LOUDLY, NEVER SKIP. A missing file or an unparseable literal is
// a FAILURE, not "nothing to check" — treating a parse miss as a pass is the
// same self-disabling trap that let the half-applied bump through. There is no
// t.Skip anywhere in this file, on purpose.
//
// A C++ counterpart that reads go/direct.go lives on the C++ side; this half
// only covers the Go direction.
func TestSHMVersion_MatchesCppHeader(t *testing.T) {
	path := filepath.FromSlash(cppHeaderRelPath)

	src, err := os.ReadFile(path)
	if err != nil {
		abs, _ := filepath.Abs(path)
		t.Fatalf("cannot read the C++ half of the SHM_VERSION SSOT pair at %s (resolved: %s): %v\n"+
			"This is a FAILURE, not a skip: without the header there is no cross-language check at all, "+
			"and a half-applied SHM_VERSION bump is a total attach failure at runtime. "+
			"If the header genuinely moved, update cppHeaderRelPath in this file.", path, abs, err)
	}

	m := shmVersionLiteral.FindSubmatch(src)
	if m == nil {
		abs, _ := filepath.Abs(path)
		t.Fatalf("could not find a `static const uint32_t SHM_VERSION = 0x...;` definition in %s (%d bytes read)\n"+
			"This is a FAILURE, not a skip: an unparseable literal means this guard is silently off, which is exactly "+
			"how the 0x00080000 bump got half-applied with both suites green. Either the header changed shape "+
			"(update shmVersionLiteral in this file) or the constant is gone (that is a wire break).", abs, len(src))
	}

	literal := string(m[1])
	cppVersion, err := parseHexU32(literal)
	if err != nil {
		t.Fatalf("SHM_VERSION literal %q in %s is not a valid uint32: %v", literal, path, err)
	}

	if cppVersion != Version {
		t.Fatalf("SHM_VERSION MISMATCH ACROSS THE LANGUAGE BOUNDARY\n"+
			"  C++  include/shm/IPCUtils.h  SHM_VERSION = 0x%08X  (literal %q)\n"+
			"  Go   go/direct.go            shm.Version = 0x%08X\n"+
			"The two constants MUST be edited in the same commit (AGENTS.md §Codebase Authority). "+
			"At runtime this is not a subtle bug: the guest's attach fails outright with "+
			"\"protocol version mismatch\", so a half-applied bump takes the product down.",
			cppVersion, literal, Version)
	}
}

// parseHexU32 parses a 0x-prefixed hex literal into a uint32 without pulling in
// strconv's error-string formatting, so the failure messages above own the
// wording. Returns an error on overflow or a stray character.
func parseHexU32(s string) (uint32, error) {
	if len(s) < 3 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X') {
		return 0, &parseHexError{s, "not a 0x-prefixed literal"}
	}
	var v uint64
	for _, c := range s[2:] {
		var d uint64
		switch {
		case c >= '0' && c <= '9':
			d = uint64(c - '0')
		case c >= 'a' && c <= 'f':
			d = uint64(c-'a') + 10
		case c >= 'A' && c <= 'F':
			d = uint64(c-'A') + 10
		default:
			return 0, &parseHexError{s, "non-hex digit"}
		}
		v = v<<4 | d
		if v > 0xFFFFFFFF {
			return 0, &parseHexError{s, "overflows uint32"}
		}
	}
	return uint32(v), nil
}

type parseHexError struct {
	literal string
	reason  string
}

func (e *parseHexError) Error() string { return e.reason + ": " + e.literal }
