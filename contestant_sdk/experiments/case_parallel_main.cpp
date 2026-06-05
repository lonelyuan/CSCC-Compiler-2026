#include "matrix_case_io.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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

void maybe_write_timing_file(double elapsed_seconds) {
    const char *path = std::getenv("COMPILER2026_TIMING_FILE");
    if (path == nullptr || path[0] == '\0') {
        return;
    }

    FILE *stream = std::fopen(path, "w");
    if (stream == nullptr) {
        throw std::runtime_error(
            "Failed to open COMPILER2026_TIMING_FILE for writing: " + std::string(path));
    }

    if (std::fprintf(stream, "%.17g\n", elapsed_seconds) < 0) {
        std::fclose(stream);
        throw std::runtime_error(
            "Failed to write COMPILER2026_TIMING_FILE: " + std::string(path));
    }

    if (std::fclose(stream) != 0) {
        throw std::runtime_error(
            "Failed to close COMPILER2026_TIMING_FILE: " + std::string(path));
    }
}

std::size_t resolve_thread_count(std::size_t case_count) {
    std::size_t threads = std::thread::hardware_concurrency();
    if (threads == 0) {
        threads = 1;
    }

    if (const char *env = std::getenv("COMPILER2026_CASE_THREADS")) {
        char *end = nullptr;
        const unsigned long configured = std::strtoul(env, &end, 10);
        if (end != env && *end == '\0' && configured > 0) {
            threads = static_cast<std::size_t>(configured);
        }
    }

    if (case_count == 0) {
        return 1;
    }
    return std::max<std::size_t>(1, std::min(threads, case_count));
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " input.bin output.bin\n";
        return 1;
    }

    try {
        const auto inputs = contest::read_input_cases(argv[1]);
        std::vector<contest::ResultCase> outputs(inputs.size());

        const std::size_t thread_count = resolve_thread_count(inputs.size());
        std::atomic<std::size_t> next_case{0};
        std::exception_ptr worker_error;
        std::mutex error_mutex;

        const double start = monotonic_seconds();

        auto worker = [&]() {
            try {
                while (true) {
                    const std::size_t index = next_case.fetch_add(1, std::memory_order_relaxed);
                    if (index >= inputs.size()) {
                        break;
                    }

                    const auto &item = inputs[index];
                    contest::ResultCase result;
                    result.n = item.n;
                    result.b = item.b;
                    result.l.assign(static_cast<std::size_t>(item.n) * item.n, 0.0);
                    contest::block_cholesky(item.a.data(), result.l.data(), static_cast<int>(item.n),
                                            static_cast<int>(item.b));
                    outputs[index] = std::move(result);
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(error_mutex);
                if (!worker_error) {
                    worker_error = std::current_exception();
                }
                next_case.store(inputs.size(), std::memory_order_relaxed);
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            workers.emplace_back(worker);
        }
        for (auto &thread : workers) {
            thread.join();
        }

        if (worker_error) {
            std::rethrow_exception(worker_error);
        }

        const double end = monotonic_seconds();
        contest::write_output_cases(argv[2], outputs);
        maybe_write_timing_file(end - start);
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "case_parallel failed: " << ex.what() << "\n";
        return 1;
    }
}
