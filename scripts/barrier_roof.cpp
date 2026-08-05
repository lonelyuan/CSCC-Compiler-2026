// Experiment 1c: how much of the phase-transition cost is the condvar itself?
//
// The runtime ends every phase by having all participants find the queue empty
// under one global mutex_ and park on a condvar, then begins the next phase with
// a notify_all that wakes T threads which serialize re-acquiring that mutex.
// wait_calls = 2n/b - 1, so a b=8 n=1152 case pays this 287 times.
//
// This measures the floor: same participant count, same number of phase
// transitions, no matrix work at all. Three barrier implementations.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

static int NT, NPHASE;

struct CvBarrier {              // what the runtime effectively does
    std::mutex m; std::condition_variable cv;
    int count = 0, gen = 0;
    void arrive() {
        std::unique_lock<std::mutex> lk(m);
        const int g = gen;
        if (++count == NT) { count = 0; ++gen; cv.notify_all(); }
        else cv.wait(lk, [this, g] { return gen != g; });
    }
};
struct SpinBarrier {            // sense-reversing, no kernel involvement
    std::atomic<int> count{0}; std::atomic<int> gen{0};
    void arrive() {
        const int g = gen.load(std::memory_order_acquire);
        if (count.fetch_add(1, std::memory_order_acq_rel) + 1 == NT) {
            count.store(0, std::memory_order_relaxed);
            gen.store(g + 1, std::memory_order_release);
        } else {
            while (gen.load(std::memory_order_acquire) == g) __asm__ __volatile__("yield");
        }
    }
};
struct HybridBarrier {          // spin briefly, then park -- safe if oversubscribed
    std::atomic<int> count{0}; std::atomic<int> gen{0};
    std::mutex m; std::condition_variable cv;
    void arrive() {
        const int g = gen.load(std::memory_order_acquire);
        if (count.fetch_add(1, std::memory_order_acq_rel) + 1 == NT) {
            count.store(0, std::memory_order_relaxed);
            { std::lock_guard<std::mutex> lk(m); gen.store(g + 1, std::memory_order_release); }
            cv.notify_all();
            return;
        }
        for (int i = 0; i < 20000; ++i) {
            if (gen.load(std::memory_order_acquire) != g) return;
            __asm__ __volatile__("yield");
        }
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [this, g] { return gen.load(std::memory_order_acquire) != g; });
    }
};

template <typename B> double run(const char *name) {
    B bar; std::atomic<int> ready{0};
    auto body = [&] {
        ready.fetch_add(1);
        while (ready.load() < NT) __asm__ __volatile__("yield");
        for (int p = 0; p < NPHASE; ++p) bar.arrive();
    };
    std::vector<std::thread> th;
    for (int i = 1; i < NT; ++i) th.emplace_back(body);
    auto t0 = std::chrono::steady_clock::now();
    body();
    for (auto &x : th) x.join();
    double w = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("  %-9s T=%-3d %8.3f ms total  %8.1f us/barrier\n",
           name, NT, w * 1e3, w * 1e6 / NPHASE);
    return w * 1e6 / NPHASE;
}

int main(int argc, char **argv) {
    NPHASE = (argc > 1) ? atoi(argv[1]) : 287;
    printf("phase transitions=%d (matches b=8 n=1152: wait_calls=287)\n", NPHASE);
    for (int t : {8, 16, 24, 32, 40}) {
        NT = t;
        double c = run<CvBarrier>("condvar");
        double s = run<SpinBarrier>("spin");
        double h = run<HybridBarrier>("hybrid");
        printf("  -> spin saves %.1f ms, hybrid saves %.1f ms over %d phases\n\n",
               (c - s) * NPHASE / 1e3, (c - h) * NPHASE / 1e3, NPHASE);
    }
    return 0;
}
