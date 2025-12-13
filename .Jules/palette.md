# Palette's Journal

## 2024-05-21 - [C++ CLI Progress Bar]
**Learning:** Standard C++ `std::cout` with `\r` and `std::flush` is a reliable, zero-dependency way to add progress feedback in cross-platform CLI tools, significantly improving the "alive" feel of long-running benchmarks.
**Action:** Use this simple loop pattern for other time-based CLI tools in the repo instead of blocking sleeps.

## 2024-05-24 - [CLI Responsiveness & Readability]
**Learning:** Hardcoded startup sleeps (e.g., `sleep(2)`) kill perceived performance. Replacing them with an active polling loop + visual feedback (dots) makes the tool feel instantaneous when the dependency is ready. Also, aligning CLI output tables with `std::setw` and adding commas to large numbers transforms a "raw dump" into a "report".
**Action:** Always challenge arbitrary `sleep()` calls in startup sequences and use `FormatNumber` helpers for metrics.
