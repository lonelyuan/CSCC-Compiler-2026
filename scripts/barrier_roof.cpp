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

struct TreeBarrier2 {            // two-level fan-in/fan-out; only group leaders touch root
    struct alignas(64) Node { std::atomic<int> c{0}; std::atomic<int> g{0}; };
    std::vector<Node> leaf;
    Node root;
    TreeBarrier2() : leaf((NT + 3) / 4) {}
    static thread_local int my_tid;
    static void wait_for(Node &nd, int g) {
        for (int i = 0; i < 4000; ++i) {
            if (nd.g.load(std::memory_order_acquire) != g) return;
            __asm__ __volatile__("yield");
        }
        while (nd.g.load(std::memory_order_acquire) == g) __asm__ __volatile__("yield");
    }
    void arrive() {
        const int tid = my_tid;
        const int groups = (NT + 3) / 4;
        const int gidx = tid / 4;
        const int fan = (gidx == groups - 1) ? (NT - gidx * 4) : 4;
        Node &group = leaf[gidx];
        const int group_gen = group.g.load(std::memory_order_acquire);
        if (group.c.fetch_add(1, std::memory_order_acq_rel) + 1 != fan) {
            wait_for(group, group_gen);
            return;
        }

        group.c.store(0, std::memory_order_relaxed);
        const int root_gen = root.g.load(std::memory_order_acquire);
        if (root.c.fetch_add(1, std::memory_order_acq_rel) + 1 == groups) {
            root.c.store(0, std::memory_order_relaxed);
            root.g.store(root_gen + 1, std::memory_order_release);
        } else {
            wait_for(root, root_gen);
        }
        // Release this leaf only after every group has reached the root.
        group.g.store(group_gen + 1, std::memory_order_release);
    }
};

thread_local int TreeBarrier2::my_tid = 0;

template <typename B> double run(const char *name) {
    B bar; std::atomic<int> ready{0};
    auto body = [&](int tid) {
        TreeBarrier2::my_tid = tid;
        ready.fetch_add(1);
        while (ready.load() < NT) __asm__ __volatile__("yield");
        for (int p = 0; p < NPHASE; ++p) bar.arrive();
        (void)tid;
    };
    std::vector<std::thread> th;
    for (int i = 1; i < NT; ++i) th.emplace_back(body, i);
    auto t0 = std::chrono::steady_clock::now();
    body(0);
    for (auto &x : th) x.join();
    double w = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("  %-9s T=%-3d %8.3f ms total  %8.1f us/barrier\n",
           name, NT, w * 1e3, w * 1e6 / NPHASE);
    return w * 1e6 / NPHASE;
}

int main(int argc, char **argv) {
    NPHASE = (argc > 1) ? atoi(argv[1]) : 287;
    printf("phase transitions=%d (matches b=8 n=1152: wait_calls=287)\n", NPHASE);
    std::vector<int> tl;
    for (int i = 2; i < argc; ++i) tl.push_back(atoi(argv[i]));
    if (tl.empty()) tl = {8, 16, 24, 32, 40};
    for (int t : tl) {
        NT = t;
        double c = run<CvBarrier>("condvar");
        run<SpinBarrier>("spin");
        double h = run<HybridBarrier>("hybrid");
        double tr = run<TreeBarrier2>("tree");
        printf("  -> best saves %.1f ms over %d phases (condvar %.0fus -> %.0fus)\n\n",
               (c - (h < tr ? h : tr)) * NPHASE / 1e3, NPHASE, c, (h < tr ? h : tr));
    }
    return 0;
}
