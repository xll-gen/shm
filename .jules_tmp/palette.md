# Palette's Journal

## 2024-05-21 - [C++ CLI Progress Bar]
**Learning:** Standard C++ `std::cout` with `\r` and `std::flush` is a reliable, zero-dependency way to add progress feedback in cross-platform CLI tools, significantly improving the "alive" feel of long-running benchmarks.
**Action:** Use this simple loop pattern for other time-based CLI tools in the repo instead of blocking sleeps.

## 2024-05-22 - [Readable Numbers in CLI]
**Learning:** Adding thousands separators (e.g., "1,000,000") to benchmark results instantly transforms them from "data" to "information", reducing cognitive load for users comparing high-throughput metrics.
**Action:** Always format "Total Ops" and "Throughput" metrics in performance tools using a simple helper function.
