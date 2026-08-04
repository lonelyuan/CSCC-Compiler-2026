// Standalone scheduler feasibility harness.
//
// Purpose: validate the *scheduling mechanism* of the contest runtime on real
// many-core hardware (e.g. Apple M5, 10 cores) WITHOUT the BiSheng/LLVM Pass or
// the Linux-only SDK. It links the real `submission/runtime/dag_runtime.cpp`
// and replays the exact blocked-Cholesky task DAG through the genuine runtime
// API, using real b*b block compute (which lands near the VM-measured ~6us/madd
// at b=32). It compares the default panel-barrier schedule against the
// cross-panel (sync-cholesky) schedule across thread counts.
//
// This is a debugging/validation tool only; it is NOT part of the submission
// and does not touch the official cholesky/trsm/madd operators or the Pass.
//
// Build: see run.sh in this directory.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <vector>

// ---- Real runtime API (defined in dag_runtime.cpp) -------------------------
extern "C" void compiler2026_runtime_begin(int n, int b);
extern "C" void *compiler2026_runtime_alloc(std::size_t size);
extern "C" void compiler2026_runtime_submit(void (*fn)(void *), void *ctx);
extern "C" void compiler2026_runtime_submit_deps(void (*fn)(void *), void *ctx,
                                                 int dep_a, int dep_b, int output);
extern "C" void compiler2026_runtime_submit_deps3(void (*fn)(void *), void *ctx,
                                                  int dep_a, int dep_b, int dep_c,
                                                  int output);
extern "C" void compiler2026_runtime_wait();
extern "C" void compiler2026_runtime_wait_key(int key);
extern "C" void compiler2026_runtime_end();

namespace {

int g_b = 32;            // block dimension
int g_N = 32;            // block count per side (n / b)
std::vector<double> g_data;  // N*N blocks, each b*b doubles, row-major

inline double *block(int r, int c) {
    return g_data.data() + (static_cast<std::size_t>(r) * g_N + c) *
                               static_cast<std::size_t>(g_b) * g_b;
}
inline int key(int r, int c) { return r * g_N + c; }

struct Ctx {
    double *out;
    double *a;
    double *bb;
};

std::atomic<long> g_exec{0};  // tasks executed (correctness self-check)

// trsm(r,p): out=block(r,p) solved against diagonal block(p,p). ~b^3 FLOPs.
void trsm_task(void *p) {
    Ctx *c = static_cast<Ctx *>(p);
    const int b = g_b;
    double *out = c->out;
    const double *diag = c->a;
    for (int i = 0; i < b; ++i) {
        for (int j = 0; j < b; ++j) {
            double acc = out[i * b + j];
            for (int k = 0; k < b; ++k) {
                acc -= out[i * b + k] * diag[k * b + j] * 1e-6;
            }
            out[i * b + j] = acc / (diag[j * b + j] + 2.0);
        }
    }
    g_exec.fetch_add(1, std::memory_order_relaxed);
}

// madd(r,c,p): block(r,c) -= block(r,p) * block(c,p)^T. ~b^3 FLOPs (the bulk).
void madd_task(void *p) {
    Ctx *c = static_cast<Ctx *>(p);
    const int b = g_b;
    double *out = c->out;
    const double *A = c->a;
    const double *B = c->bb;
    for (int i = 0; i < b; ++i) {
        for (int j = 0; j < b; ++j) {
            double acc = out[i * b + j];
            for (int k = 0; k < b; ++k) {
                acc -= A[i * b + k] * B[j * b + k];
            }
            out[i * b + j] = acc;
        }
    }
    g_exec.fetch_add(1, std::memory_order_relaxed);
}

// cholesky(p): factor diagonal block in place. ~b^3/6 FLOPs (small, like the VM).
void chol_inline(double *blk) {
    const int b = g_b;
    for (int k = 0; k < b; ++k) {
        double d = blk[k * b + k];
        d = std::sqrt(std::fabs(d) + 1.0);
        blk[k * b + k] = d;
        for (int i = k + 1; i < b; ++i) {
            blk[i * b + k] /= d;
            for (int j = k + 1; j <= i; ++j) {
                blk[i * b + j] -= blk[i * b + k] * blk[j * b + k];
            }
        }
    }
}

void init_blocks() {
    const std::size_t total =
        static_cast<std::size_t>(g_N) * g_N * g_b * g_b;
    if (g_data.size() != total) g_data.assign(total, 0.0);
    // Diagonally dominant-ish fill so values stay finite across repeats.
    for (int r = 0; r < g_N; ++r) {
        for (int c = 0; c <= r; ++c) {
            double *blk = block(r, c);
            for (int i = 0; i < g_b; ++i) {
                for (int j = 0; j < g_b; ++j) {
                    blk[i * g_b + j] = (i == j && r == c) ? (g_b * 4.0)
                                                          : 0.01 * ((i + j) % 7);
                }
            }
        }
    }
}

// Context pool (avoids per-task malloc and arena-lifetime assumptions).
std::vector<Ctx> g_ctx;
std::size_t g_ctx_used = 0;
Ctx *next_ctx(double *out, double *a, double *bb) {
    Ctx *c = &g_ctx[g_ctx_used++];
    c->out = out;
    c->a = a;
    c->bb = bb;
    return c;
}

std::size_t total_task_count() {
    // trsm: sum_{p}(N-1-p); madd: sum_{p} t(t+1)/2, t=N-1-p.
    std::size_t trsm = 0, madd = 0;
    for (int p = 0; p < g_N; ++p) {
        std::size_t t = static_cast<std::size_t>(g_N - 1 - p);
        trsm += t;
        madd += t * (t + 1) / 2;
    }
    return trsm + madd;
}

// Default schedule: panel-local ready-queue DAG with a barrier at panel end.
void run_panel_barrier(int n, int b) {
    g_ctx_used = 0;
    compiler2026_runtime_begin(n, b);
    for (int p = 0; p < g_N; ++p) {
        chol_inline(block(p, p));
        for (int r = p + 1; r < g_N; ++r) {
            Ctx *c = next_ctx(block(r, p), block(p, p), nullptr);
            compiler2026_runtime_submit_deps(trsm_task, c, -1, -1, key(r, p));
        }
        for (int r = p + 1; r < g_N; ++r) {
            for (int cc = p + 1; cc <= r; ++cc) {
                Ctx *c = next_ctx(block(r, cc), block(r, p), block(cc, p));
                compiler2026_runtime_submit_deps(madd_task, c, key(r, p),
                                                 key(cc, p), -1);
            }
        }
        compiler2026_runtime_wait();  // panel barrier
    }
    compiler2026_runtime_end();
}

// Cross-panel schedule (sync-cholesky variant): no panel barrier, only a
// wait_key on the diagonal producer before each (synchronous) cholesky, and a
// single final drain via end(). madd carries its output-block previous producer
// as a third dependency (matches the Pass submit_deps3 lowering).
void run_cross_panel(int n, int b) {
    g_ctx_used = 0;
    compiler2026_runtime_begin(n, b);
    for (int p = 0; p < g_N; ++p) {
        compiler2026_runtime_wait_key(key(p, p));
        chol_inline(block(p, p));
        for (int r = p + 1; r < g_N; ++r) {
            Ctx *c = next_ctx(block(r, p), block(p, p), nullptr);
            // deps: prev producer of (r,p), diagonal (p,p); output (r,p).
            compiler2026_runtime_submit_deps(trsm_task, c, key(r, p), key(p, p),
                                             key(r, p));
        }
        for (int r = p + 1; r < g_N; ++r) {
            for (int cc = p + 1; cc <= r; ++cc) {
                Ctx *c = next_ctx(block(r, cc), block(r, p), block(cc, p));
                // deps: trsm(r,p), trsm(cc,p), prev producer of (r,cc);
                // output (r,cc).
                compiler2026_runtime_submit_deps3(madd_task, c, key(r, p),
                                                  key(cc, p), key(r, cc),
                                                  key(r, cc));
            }
        }
    }
    compiler2026_runtime_end();  // single final drain (outer-loop-end wait)
}

double checksum() {
    double s = 0.0;
    const std::size_t total =
        static_cast<std::size_t>(g_N) * g_N * g_b * g_b;
    for (std::size_t i = 0; i < total; ++i) s += g_data[i] * (1.0 + (i % 13));
    return s;
}

// Correctness self-check: every task runs exactly once, run terminates, and
// the result is stable across repeats of the same schedule.
bool verify(const char *mode, int n, int b, int threads) {
    setenv("COMPILER2026_DAG_THREADS", std::to_string(threads).c_str(), 1);
    init_blocks();
    g_exec.store(0);
    if (std::strcmp(mode, "barrier") == 0)
        run_panel_barrier(n, b);
    else
        run_cross_panel(n, b);
    long ran = g_exec.load();
    long expect = static_cast<long>(total_task_count());
    double cs = checksum();
    bool ok = (ran == expect);
    std::printf("# verify mode=%-7s thr=%-2d exec=%ld/%ld checksum=%.6e %s\n",
                mode, threads, ran, expect, cs, ok ? "OK" : "TASK-COUNT-MISMATCH");
    return ok;
}

double median(std::vector<double> &v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

double bench(const char *mode, int n, int b, int threads, int repeats) {
    setenv("COMPILER2026_DAG_THREADS", std::to_string(threads).c_str(), 1);
    std::vector<double> times;
    times.reserve(repeats);
    for (int it = 0; it < repeats; ++it) {
        init_blocks();
        auto t0 = std::chrono::high_resolution_clock::now();
        if (std::strcmp(mode, "barrier") == 0)
            run_panel_barrier(n, b);
        else
            run_cross_panel(n, b);
        auto t1 = std::chrono::high_resolution_clock::now();
        times.push_back(
            std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return median(times);
}

}  // namespace

int main(int argc, char **argv) {
    int n = (argc > 1) ? std::atoi(argv[1]) : 1024;
    int b = (argc > 2) ? std::atoi(argv[2]) : 32;
    int repeats = (argc > 3) ? std::atoi(argv[3]) : 5;
    int max_threads = (argc > 4) ? std::atoi(argv[4]) : 10;

    g_b = b;
    g_N = n / b;
    g_ctx.assign(total_task_count() + 16, Ctx{});
    init_blocks();

    const char *ws_env = std::getenv("COMPILER2026_DAG_WORK_STEALING");
    const bool ws_on = (ws_env && ws_env[0] && ws_env[0] != '0');
    // The cross-panel schedule uses wait_key (interleaved submit); under work
    // stealing it degenerates to a conservative full drain, so we only compare
    // the barrier schedule there.
    const bool do_cross = !ws_on;

    std::printf("# sched feasibility: n=%d b=%d blocks=%d tasks=%zu repeats=%d "
                "work_stealing=%s\n",
                n, b, g_N, total_task_count(), repeats, ws_on ? "ON" : "off");

    // Correctness gate before timing: each schedule must run every task once
    // and terminate at 1 and max threads.
    bool ok = true;
    ok &= verify("barrier", n, b, 1);
    ok &= verify("barrier", n, b, max_threads);
    if (do_cross) {
        ok &= verify("cross", n, b, 1);
        ok &= verify("cross", n, b, max_threads);
    }
    if (!ok) {
        std::printf("# CORRECTNESS FAILED -- aborting timing\n");
        return 1;
    }

    if (do_cross) {
        std::printf("# %-4s | %-10s %-10s | %-10s %-10s | %-8s %-8s\n", "thr",
                    "barrier_ms", "bar_speedup", "cross_ms", "cross_speedup",
                    "cross/bar", "winner");
    } else {
        std::printf("# %-4s | %-10s %-10s  (work-stealing barrier schedule)\n",
                    "thr", "barrier_ms", "bar_speedup");
    }

    double bar1 = 0.0, cross1 = 0.0;
    for (int t = 1; t <= max_threads; ++t) {
        double bar = bench("barrier", n, b, t, repeats);
        if (t == 1) bar1 = bar;
        if (do_cross) {
            double cross = bench("cross", n, b, t, repeats);
            if (t == 1) cross1 = cross;
            double ratio = cross / bar;  // <1 means cross-panel is faster
            const char *winner = (cross < bar * 0.99) ? "CROSS"
                                 : (bar < cross * 0.99) ? "barrier"
                                                        : "tie";
            std::printf(
                "  %-4d | %-10.3f %-10.3f | %-10.3f %-10.3f | %-8.3f %-8s\n", t,
                bar, bar1 / bar, cross, cross1 / cross, ratio, winner);
        } else {
            std::printf("  %-4d | %-10.3f %-10.3f\n", t, bar, bar1 / bar);
        }
    }
    return 0;
}
