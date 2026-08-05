// Feasibility probe for small-b task coarsening. NOT part of the submission.
//
// Round 9 established that under the judge's equal-weight per-case geometric
// mean, the 22 b<=10 cases sitting at ~0.95x carry about as much leverage as a
// 20% lift across all 150 cases. They sit there because the runtime's async
// predicate excludes b < 12: a b=8 madd is 2*8^3 = 1024 flops, far less than one
// task's scheduling cost, and Round 6 measured b=8 at 0.54x even at its own best
// participant count.
//
// The roadmap's answer (§3, "range task") is to stop making one task per madd and
// instead give a worker a whole range of them. Implementing that in the Pass is a
// real change -- a third clone, a synthesized range-task body, a barrier after the
// trsm loop -- so this probe measures the ceiling FIRST, using the official
// operators and a hand-written schedule, to find out whether the payoff justifies
// the change.
//
// It deliberately flatters the coarsened schedule: spin barriers, persistent
// workers, no dependency bookkeeping, an atomic counter for load balance. Whatever
// it reports is therefore an upper bound on what the Pass could achieve, not a
// prediction.
//
// Modes:
//   serial   the SDK's block_cholesky loop, unchanged -- the reference T0
//   fine     one work unit per madd call (what the Pass emits today)
//   coarse   one work unit per (panel, j): the whole inner k-loop
//
// Usage:
//   coarsen_probe <n> <b> <threads> [repeats]

#include "kernels.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <time.h>
#include <vector>

namespace {

double monotonic_seconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1.0e-9;
}

// Strictly diagonally dominant, hence SPD, and buildable in O(n^2) so that setup
// does not dominate the probe for n=2048.
std::vector<double> make_spd(int n) {
    std::vector<double> a(static_cast<std::size_t>(n) * n, 0.0);
    unsigned state = 12345u;
    auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state >> 8) / static_cast<double>(1u << 24) - 0.5;
    };
    for (int row = 0; row < n; ++row) {
        for (int col = 0; col < row; ++col) {
            const double value = next();
            a[static_cast<std::size_t>(row) * n + col] = value;
            a[static_cast<std::size_t>(col) * n + row] = value;
        }
    }
    for (int row = 0; row < n; ++row) {
        a[static_cast<std::size_t>(row) * n + row] = 2.0 * n;
    }
    return a;
}

void zero_upper(double *l, int n) {
    for (int row = 0; row < n; ++row) {
        for (int col = row + 1; col < n; ++col) {
            l[static_cast<std::size_t>(row) * n + col] = 0.0;
        }
    }
}

// Exactly contest::block_cholesky, so the reference time is the real T0.
void run_serial(const double *a, double *l, int n, int b) {
    std::copy(a, a + static_cast<std::size_t>(n) * n, l);
    for (int i = 0; i < n; i += b) {
        cholesky(&l[i * n + i], &l[i * n + i], b, n);
        for (int j = i + b; j < n; j += b) {
            trsm(&l[j * n + i], &l[i * n + i], &l[j * n + i], b, n);
        }
        for (int j = i + b; j < n; j += b) {
            for (int k = j; k < n; k += b) {
                madd(&l[k * n + i], &l[j * n + i], &l[k * n + j], b, n);
            }
        }
    }
    zero_upper(l, n);
}

class SpinBarrier {
public:
    explicit SpinBarrier(int participants) : participants_(participants) {}

    void wait() {
        const int generation = generation_.load(std::memory_order_acquire);
        if (arrived_.fetch_add(1, std::memory_order_acq_rel) == participants_ - 1) {
            arrived_.store(0, std::memory_order_relaxed);
            generation_.store(generation + 1, std::memory_order_release);
            return;
        }
        while (generation_.load(std::memory_order_acquire) == generation) {
            std::this_thread::yield();
        }
    }

private:
    int participants_;
    std::atomic<int> arrived_{0};
    std::atomic<int> generation_{0};
};

// Panel-barrier schedule with a configurable madd granularity. Every worker takes
// work from one atomic counter per phase, so load balance is dynamic and the
// coarse mode's uneven unit sizes (unit j carries B-j madds) are absorbed.
void run_parallel(const double *a, double *l, int n, int b, int threads, bool coarse) {
    std::copy(a, a + static_cast<std::size_t>(n) * n, l);
    const int blocks = n / b;

    SpinBarrier barrier(threads);
    std::atomic<int> trsm_cursor{0};
    std::atomic<int> madd_cursor{0};
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads) - 1);

    auto worker = [&](int rank) {
        for (int panel = 0; panel < blocks; ++panel) {
            const int i = panel * b;
            if (rank == 0) {
                cholesky(&l[i * n + i], &l[i * n + i], b, n);
                trsm_cursor.store(0, std::memory_order_relaxed);
            }
            barrier.wait();

            const int trailing = blocks - panel - 1;
            for (;;) {
                const int unit = trsm_cursor.fetch_add(1, std::memory_order_relaxed);
                if (unit >= trailing) {
                    break;
                }
                const int j = i + (unit + 1) * b;
                trsm(&l[j * n + i], &l[i * n + i], &l[j * n + i], b, n);
            }
            if (rank == 0) {
                madd_cursor.store(0, std::memory_order_relaxed);
            }
            barrier.wait();

            if (coarse) {
                // One unit per j: the whole inner k-loop. Unit j writes only
                // blocks in block-column j, so units never overlap.
                for (;;) {
                    const int unit = madd_cursor.fetch_add(1, std::memory_order_relaxed);
                    if (unit >= trailing) {
                        break;
                    }
                    const int j = i + (unit + 1) * b;
                    for (int k = j; k < n; k += b) {
                        madd(&l[k * n + i], &l[j * n + i], &l[k * n + j], b, n);
                    }
                }
            } else {
                // One unit per madd, i.e. today's granularity. The (j,k) pair is
                // recovered from a flat index so the counter stays a single atomic.
                const long total = static_cast<long>(trailing) * (trailing + 1) / 2;
                for (;;) {
                    const long unit = madd_cursor.fetch_add(1, std::memory_order_relaxed);
                    if (unit >= total) {
                        break;
                    }
                    long remaining = unit;
                    int column = 0;
                    while (remaining >= trailing - column) {
                        remaining -= trailing - column;
                        ++column;
                    }
                    const int j = i + (column + 1) * b;
                    const int k = j + static_cast<int>(remaining) * b;
                    madd(&l[k * n + i], &l[j * n + i], &l[k * n + j], b, n);
                }
            }
            if (rank == 0) {
                trsm_cursor.store(0, std::memory_order_relaxed);
            }
            barrier.wait();
        }
    };

    for (int rank = 1; rank < threads; ++rank) {
        pool.emplace_back(worker, rank);
    }
    worker(0);
    for (std::thread &thread : pool) {
        thread.join();
    }
    zero_upper(l, n);
}

double max_abs_diff(const std::vector<double> &x, const std::vector<double> &y) {
    double worst = 0.0;
    for (std::size_t index = 0; index < x.size(); ++index) {
        worst = std::max(worst, std::fabs(x[index] - y[index]));
    }
    return worst;
}

double best_of(int repeats, const std::function<void()> &body) {
    double best = 1e300;
    for (int attempt = 0; attempt < repeats; ++attempt) {
        const double start = monotonic_seconds();
        body();
        best = std::min(best, monotonic_seconds() - start);
    }
    return best;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr, "Usage: %s <n> <b> <threads> [repeats]\n", argv[0]);
        return 1;
    }
    const int n = std::atoi(argv[1]);
    const int b = std::atoi(argv[2]);
    const int threads = std::atoi(argv[3]);
    const int repeats = (argc > 4) ? std::atoi(argv[4]) : 3;
    if (n <= 0 || b <= 0 || n % b != 0 || threads <= 0) {
        std::fprintf(stderr, "bad arguments: need n%%b==0\n");
        return 1;
    }

    const std::vector<double> a = make_spd(n);
    const std::size_t elements = static_cast<std::size_t>(n) * n;
    std::vector<double> l_serial(elements), l_fine(elements), l_coarse(elements);

    const double serial = best_of(repeats, [&] { run_serial(a.data(), l_serial.data(), n, b); });
    const double fine = best_of(repeats, [&] {
        run_parallel(a.data(), l_fine.data(), n, b, threads, false);
    });
    const double coarse = best_of(repeats, [&] {
        run_parallel(a.data(), l_coarse.data(), n, b, threads, true);
    });

    const double fine_error = max_abs_diff(l_serial, l_fine);
    const double coarse_error = max_abs_diff(l_serial, l_coarse);

    std::printf("n=%d b=%d B=%d threads=%d repeats=%d\n", n, b, n / b, threads, repeats);
    std::printf("  serial       %9.6fs\n", serial);
    std::printf("  fine         %9.6fs  %6.3fx  max_abs_diff=%.3e\n",
                fine, serial / fine, fine_error);
    std::printf("  coarse       %9.6fs  %6.3fx  max_abs_diff=%.3e\n",
                coarse, serial / coarse, coarse_error);
    std::printf("  coarse/fine  %6.3fx\n", fine / coarse);
    if (fine_error > 1e-9 || coarse_error > 1e-9) {
        std::fprintf(stderr, "coarsen_probe: parallel result diverged from serial\n");
        return 4;
    }
    return 0;
}
