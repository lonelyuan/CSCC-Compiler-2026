// Local-only measurement harness. NOT part of the contest submission.
//
// Why this exists: the judge scores an EQUAL-WEIGHT geometric mean of per-case
// speedups over the 150 public cases, while the official `main.cpp` only reports
// one accumulated `total_compute_seconds` for the whole run, and
// `submission/scripts/benchmark.sh` derives its speedup from that accumulated
// number. A total-time ratio is flops-weighted and therefore dominated by the
// largest cases, so it cannot see what the judge metric actually rewards.
//
// This harness times each `contest::block_cholesky` call separately and writes a
// per-case CSV. Apart from the timing bookkeeping it calls the operator exactly
// as `contestant_sdk/src/baseline/main.cpp` does: same input reader, same buffer
// setup, same call signature, same output writer. The official baseline copies
// under `submission/src/baseline/` are left untouched, since the judge rebuilds
// those and they must stay identical to the official sources apart from
// annotations.
//
// Environment:
//   COMPILER2026_PERCASE_CSV     output CSV path (default: percase.csv)
//   COMPILER2026_PERCASE_REPEAT  timed repetitions per case (default 1). Each
//                                repetition is a full block_cholesky call; the
//                                CSV reports min/median/max. The written output
//                                matrix comes from the last repetition, so the
//                                verifier still checks real results.
//   COMPILER2026_TIMING_FILE     same meaning as in the official main: total
//                                compute seconds (sum over cases, first
//                                repetition only, so it stays comparable).

#include "matrix_case_io.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <time.h>
#include <vector>

namespace contest {
int block_cholesky(const double *A, double *L, int n, int b);
}

namespace {

double monotonic_seconds() {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC) failed");
    }
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1.0e-9;
}

std::size_t repeat_count() {
    const char *env = std::getenv("COMPILER2026_PERCASE_REPEAT");
    if (env == nullptr || env[0] == '\0') {
        return 1;
    }
    char *end = nullptr;
    const unsigned long value = std::strtoul(env, &end, 10);
    if (end == env || *end != '\0' || value == 0) {
        return 1;
    }
    return static_cast<std::size_t>(value);
}

void maybe_write_timing_file(double elapsed_seconds) {
    const char *path = std::getenv("COMPILER2026_TIMING_FILE");
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    FILE *stream = std::fopen(path, "w");
    if (stream == nullptr) {
        throw std::runtime_error("Failed to open COMPILER2026_TIMING_FILE: " + std::string(path));
    }
    if (std::fprintf(stream, "%.17g\n", elapsed_seconds) < 0) {
        std::fclose(stream);
        throw std::runtime_error("Failed to write COMPILER2026_TIMING_FILE");
    }
    if (std::fclose(stream) != 0) {
        throw std::runtime_error("Failed to close COMPILER2026_TIMING_FILE");
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " input.bin output.bin\n";
        return 1;
    }

    try {
        const std::size_t repeats = repeat_count();
        const char *csv_env = std::getenv("COMPILER2026_PERCASE_CSV");
        const std::string csv_path = (csv_env != nullptr && csv_env[0] != '\0')
                                         ? std::string(csv_env)
                                         : std::string("percase.csv");

        const auto inputs = contest::read_input_cases(argv[1]);
        std::vector<contest::ResultCase> outputs;
        outputs.reserve(inputs.size());

        std::vector<std::vector<double>> timings(inputs.size());
        double total_compute_seconds = 0.0;

        for (std::size_t index = 0; index < inputs.size(); ++index) {
            const auto &item = inputs[index];
            contest::ResultCase result;
            result.n = item.n;
            result.b = item.b;
            result.l.assign(static_cast<std::size_t>(item.n) * item.n, 0.0);

            timings[index].reserve(repeats);
            for (std::size_t rep = 0; rep < repeats; ++rep) {
                const double start = monotonic_seconds();
                contest::block_cholesky(item.a.data(), result.l.data(), static_cast<int>(item.n),
                                        static_cast<int>(item.b));
                const double end = monotonic_seconds();
                timings[index].push_back(end - start);
            }
            total_compute_seconds += timings[index].front();
            outputs.push_back(std::move(result));
        }

        contest::write_output_cases(argv[2], outputs);
        maybe_write_timing_file(total_compute_seconds);

        FILE *csv = std::fopen(csv_path.c_str(), "w");
        if (csv == nullptr) {
            throw std::runtime_error("Failed to open COMPILER2026_PERCASE_CSV: " + csv_path);
        }
        std::fprintf(csv, "case_index,n,b,repeats,seconds_min,seconds_median,seconds_max,seconds_first\n");
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            std::vector<double> sorted = timings[index];
            std::sort(sorted.begin(), sorted.end());
            const double median = sorted[sorted.size() / 2];
            std::fprintf(csv, "%zu,%u,%u,%zu,%.9f,%.9f,%.9f,%.9f\n", index, inputs[index].n,
                         inputs[index].b, sorted.size(), sorted.front(), median, sorted.back(),
                         timings[index].front());
        }
        if (std::fclose(csv) != 0) {
            throw std::runtime_error("Failed to close per-case CSV");
        }

        std::printf("percase_csv=%s\n", csv_path.c_str());
        std::printf("cases=%zu repeats=%zu total_compute_seconds=%.9f\n", inputs.size(), repeats,
                    total_compute_seconds);
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "percase_harness failed: " << ex.what() << "\n";
        return 1;
    }
}
