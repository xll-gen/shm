// Regression test for the SHM_VERSION / shm.Version SSOT pair (AGENTS.md
// §Codebase Authority): C++ `SHM_VERSION` in include/shm/IPCUtils.h and Go
// `Version` in go/direct.go MUST hold the same value, because a mismatch is a
// total attach failure ("protocol version mismatch"), not a soft degradation.
//
// WHY THIS TEST EXISTS AND WHY THE EXISTING ONES DO NOT COVER IT
// --------------------------------------------------------------
// The 0x00080000 bump shipped HALF-APPLIED once (C++ at 0x00080000, Go still at
// 0x00070000) and BOTH suites went green, because:
//   * tests/test_protocol_version.cpp compares the value the C++ host WROTE into
//     the segment against the C++ constant it wrote it from. That is tautological
//     with respect to cross-language parity — it can never see the Go side.
//   * tests/test_stream_integration.cpp is the only test that runs Go code, and it
//     exits with CTest's SKIP_RETURN_CODE (77) whenever cmake's
//     find_program(NAMES go) misses. A C++-only build — the common case for a
//     consumer building this header-only library — therefore checks nothing.
//
// So this test reads the Go SOURCE as data. It needs no Go toolchain, which is the
// point: there is no configuration in which it silently checks nothing.
//
// SELF-DISABLING IS THE FAILURE MODE BEING AVOIDED
// ------------------------------------------------
// Every way this test could fail to reach a value is a hard FAIL, never a skip and
// never a pass:
//   * SHM_GO_DIRECT_GO not defined by CMake -> #error, the build breaks.
//   * file missing / unreadable -> FAIL.
//   * no `Version uint32 = <literal>` declaration found -> FAIL.
//   * more than one such declaration -> FAIL (ambiguous; do not guess).
//   * literal not parseable, or has trailing garbage -> FAIL.
// A "parse miss degrades to nothing to check" path is the same trap as the SKIP
// above, one layer down.

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <shm/IPCUtils.h>

#ifndef SHM_GO_DIRECT_GO
#error "SHM_GO_DIRECT_GO must be defined by tests/CMakeLists.txt with the path to go/direct.go. Without it this test would check nothing, which is the exact self-disabling failure it exists to prevent."
#endif

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Splits a Go source line into whitespace-separated tokens after dropping any
// line comment. `=` is treated as its own token so `Version uint32 = 0x...` and
// `Version uint32=0x...` tokenize identically.
std::vector<std::string> tokenize(const std::string& rawLine) {
    std::string line = rawLine;
    const size_t comment = line.find("//");
    if (comment != std::string::npos) line = line.substr(0, comment);

    std::vector<std::string> tokens;
    std::string cur;
    for (char c : line) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '=') {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
            if (c == '=') tokens.push_back("=");
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

// Parses a Go integer literal (hex, decimal, octal; underscore separators
// allowed). Returns false on anything it cannot fully consume, rather than
// yielding a partial value.
bool parseGoUint32(const std::string& literalIn, uint32_t& out, std::string& why) {
    std::string literal;
    for (char c : literalIn) {
        if (c != '_') literal.push_back(c);
    }
    if (literal.empty()) {
        why = "empty literal";
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(literal.c_str(), &end, 0);
    if (errno != 0) {
        why = "strtoull set errno";
        return false;
    }
    if (end == literal.c_str()) {
        why = "not a number";
        return false;
    }
    if (end != literal.c_str() + literal.size()) {
        why = std::string("trailing garbage after the number: '") + end + "'";
        return false;
    }
    if (v > 0xFFFFFFFFull) {
        why = "does not fit in uint32";
        return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

} // namespace

int main() {
    const char* path = SHM_GO_DIRECT_GO;

    std::ifstream in(path, std::ios::binary);
    // is_open(), NOT `!in`: on this libstdc++/MinGW build a failed open leaves the
    // stream in a good state, so `if (!in)` reads as success for a missing file and
    // the missing-file case would be misreported as "no declaration found". Same
    // class of bug as the SKIP this test replaces — a check that cannot see its own
    // precondition failing.
    if (!in.is_open()) {
        std::cerr << "FAIL: cannot open the Go SSOT twin at '" << path
                  << "'. This test asserts C++ SHM_VERSION == Go shm.Version by "
                     "reading the Go source; a missing file means the check did "
                     "not run, which must never be reported as a pass."
                  << std::endl;
        return 1;
    }

    std::string line;
    int lineNo = 0;
    std::vector<std::pair<int, std::string>> matches; // (line number, literal)
    while (std::getline(in, line)) {
        ++lineNo;
        const std::vector<std::string> t = tokenize(line);
        // Inside go/direct.go's `const (...)` block the declaration is
        //   Version uint32 = 0x00080000
        // Require the exact shape so `MinVersion`, `header.Version`, a struct
        // field named Version, or a comment mentioning it cannot match.
        if (t.size() == 4 && t[0] == "Version" && t[1] == "uint32" && t[2] == "=") {
            matches.emplace_back(lineNo, t[3]);
        }
    }
    in.close();

    if (matches.empty()) {
        std::cerr << "FAIL: no `Version uint32 = <literal>` declaration found in '"
                  << path
                  << "'. The Go constant was renamed, reshaped, or moved. Fix this "
                     "parser in the same commit — a parse miss must not degrade to "
                     "'nothing to check'."
                  << std::endl;
        return 1;
    }
    if (matches.size() != 1) {
        std::cerr << "FAIL: found " << matches.size()
                  << " `Version uint32 = ...` declarations in '" << path
                  << "' (lines";
        for (const auto& m : matches) std::cerr << " " << m.first;
        std::cerr << "). Ambiguous — the SSOT must be a single declaration."
                  << std::endl;
        return 1;
    }

    uint32_t goVersion = 0;
    std::string why;
    if (!parseGoUint32(matches[0].second, goVersion, why)) {
        std::cerr << "FAIL: could not parse the Go literal '" << matches[0].second
                  << "' at " << path << ":" << matches[0].first << " (" << why
                  << "). Refusing to pass without an actual comparison."
                  << std::endl;
        return 1;
    }

    if (goVersion != shm::SHM_VERSION) {
        std::fprintf(stderr,
                     "FAIL: wire protocol version SSOT mismatch.\n"
                     "  C++ shm::SHM_VERSION (include/shm/IPCUtils.h) = 0x%08X\n"
                     "  Go  shm.Version      (%s:%d)                  = 0x%08X\n"
                     "These two constants are ONE source of truth (AGENTS.md "
                     "section 'Codebase Authority') and must be edited in the same "
                     "commit. A mismatch is not a soft degradation: every guest "
                     "attach fails with \"protocol version mismatch\".\n",
                     static_cast<unsigned>(shm::SHM_VERSION), path,
                     matches[0].first, static_cast<unsigned>(goVersion));
        return 1;
    }

    std::fprintf(stdout,
                 "Test Passed (C++ SHM_VERSION == Go shm.Version == 0x%08X, "
                 "parsed from %s:%d)\n",
                 static_cast<unsigned>(shm::SHM_VERSION), path, matches[0].first);
    return 0;
}
