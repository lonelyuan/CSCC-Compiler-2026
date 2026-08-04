#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace {

using TaskFn = void (*)(void *);
constexpr std::size_t kMaxTaskBatch = 16;
constexpr std::size_t kMaxProfiledTasks = 8;

using Clock = std::chrono::steady_clock;

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
}

bool profileEnabledFromEnv() {
    const char *env = std::getenv("COMPILER2026_DAG_PROFILE");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
}

bool workerPinningEnabledFromEnv() {
    const char *env = std::getenv("COMPILER2026_DAG_PIN_WORKERS");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
}

// Opt-in experimental scheduler: per-worker deques with work stealing instead
// of the single global ready queue. Default off keeps the validated path.
bool workStealingEnabledFromEnv() {
    const char *env = std::getenv("COMPILER2026_DAG_WORK_STEALING");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
}

std::vector<int> allowedCpuList() {
    std::vector<int> cpus;
#ifdef __linux__
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        return cpus;
    }
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &allowed)) {
            cpus.push_back(cpu);
        }
    }
#endif
    return cpus;
}

class AsyncRuntime {
    struct Task {
        TaskFn fn;
        void *context;
        std::uint64_t enqueue_ns;
        int dag_node = -1;
    };

    struct BatchProfile {
        std::array<std::uint64_t, kMaxTaskBatch> task_exec_ns{};
        std::array<std::uint64_t, kMaxTaskBatch> task_queue_ns{};
        std::uint64_t total_exec_ns = 0;
        std::uint64_t total_queue_ns = 0;
    };

    struct TaskProfile {
        TaskFn fn = nullptr;
        const char *name = nullptr;
        std::uint64_t count = 0;
        std::uint64_t queue_ns = 0;
        std::uint64_t exec_ns = 0;
    };

    struct DagNode {
        TaskFn fn = nullptr;
        void *context = nullptr;
        int pending = 0;
        bool completed = false;
        int first_successor = -1;
        int last_successor = -1;
        std::size_t successor_count = 0;
    };

    struct DagEdge {
        int successor = -1;
        int next = -1;
    };

    // Work-stealing ready queue shard (one per participant: main + each worker).
    struct WsDeque {
        std::mutex m;
        std::deque<Task> q;
    };

public:
    explicit AsyncRuntime(std::size_t worker_count)
        : pin_workers_(workerPinningEnabledFromEnv()),
          worker_cpus_(pin_workers_ ? allowedCpuList() : std::vector<int>{}) {
        work_stealing_ = workStealingEnabledFromEnv();
        if (worker_count == 0) {
            return;
        }
        if (work_stealing_) {
            // One shard for the main thread plus one per worker.
            ws_queues_ = std::vector<WsDeque>(worker_count + 1);
        }
        workers_.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this, i]() { workerLoop(i); });
        }
    }

    std::size_t workerCount() const {
        return workers_.size();
    }

    ~AsyncRuntime() {
        wait();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        work_cv_.notify_all();
        ws_start_cv_.notify_all();
        for (auto &worker : workers_) {
            worker.join();
        }
    }

    void resetForCall(std::size_t reserve_tasks = 0, std::size_t task_batch_size = 1,
                      int n = 0, int b = 0, std::size_t total_threads = 1,
                      std::size_t max_live_window = 0) {
        wait();
        max_live_window_ = max_live_window;
        resetProfile(n, b, total_threads, task_batch_size);
        resetQueue(reserve_tasks, task_batch_size);
    }

    void finishCall() {
        wait();
        reportProfile();
        resetQueue(0, 1);
        profile_enabled_.store(false, std::memory_order_relaxed);
    }

    void registerTask(TaskFn fn, const char *name) {
        if (!profile_enabled_.load(std::memory_order_relaxed)) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        TaskProfile &profile = profileForTaskLocked(fn);
        profile.name = name;
    }

    void resetQueue(std::size_t reserve_tasks, std::size_t task_batch_size) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reserve_tasks > tasks_.capacity()) {
            tasks_.reserve(reserve_tasks);
        }
        if (reserve_tasks > dag_nodes_.capacity()) {
            dag_nodes_.reserve(reserve_tasks);
        }
        const std::size_t reserve_edges = reserve_tasks * 2;
        if (reserve_edges > successor_edges_.capacity()) {
            successor_edges_.reserve(reserve_edges);
        }
        if (reserve_tasks > latest_producer_.bucket_count()) {
            latest_producer_.reserve(reserve_tasks);
        }
        if (profile_enabled_.load(std::memory_order_relaxed) &&
            reserve_tasks > profile_output_keys_.bucket_count()) {
            profile_output_keys_.reserve(reserve_tasks);
        }
        tasks_.clear();
        dag_nodes_.clear();
        successor_edges_.clear();
        latest_producer_.clear();
        pending_dag_tasks_ = 0;
        task_head_ = 0;
        chunk_offset_ = 0;
        pending_count_ = 0;
        task_batch_size_ = std::max<std::size_t>(
            1, std::min<std::size_t>(task_batch_size, kMaxTaskBatch));
        worker_error_ = nullptr;

        if (work_stealing_) {
            ws_initial_ready_.clear();
            ws_outstanding_.store(0, std::memory_order_relaxed);
            for (auto &shard : ws_queues_) {
                std::lock_guard<std::mutex> sl(shard.m);
                shard.q.clear();
            }
        }
    }

    void submit(TaskFn fn, void *context) {
        if (work_stealing_) {
            submitWorkStealing(fn, context, nullptr, 0, -1);
            return;
        }
        const bool profiling = profile_enabled_.load(std::memory_order_relaxed);
        Task task{fn, context, profiling ? nowNs() : 0};
        if (workers_.empty()) {
            BatchProfile profile = runBatch(&task, 1, profiling);
            if (profiling) {
                std::lock_guard<std::mutex> lock(mutex_);
                recordBatchProfileLocked(&task, 1, profile, false);
            }
            return;
        }

        if (task_batch_size_ > 1) {
            pending_tasks_[pending_count_++] = task;
            if (pending_count_ >= task_batch_size_) {
                flushPendingTasks();
            }
            return;
        }

        enqueueTask(task);
    }

    void submitWithDeps(TaskFn fn, void *context, const int *deps, std::size_t dep_count,
                        int output) {
        if (work_stealing_) {
            submitWorkStealing(fn, context, deps, dep_count, output);
            return;
        }
        const bool profiling = profile_enabled_.load(std::memory_order_relaxed);
        if (workers_.empty()) {
            Task task{fn, context, profiling ? nowNs() : 0};
            BatchProfile profile = runBatch(&task, 1, profiling);
            if (profiling) {
                std::lock_guard<std::mutex> lock(mutex_);
                recordBatchProfileLocked(&task, 1, profile, false);
            }
            return;
        }

        bool should_notify = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const int node_index = static_cast<int>(dag_nodes_.size());
            dag_nodes_.push_back({fn, context, 0, false});
            ++pending_dag_tasks_;
            if (profiling) {
                ++profile_dag_nodes_;
                max_dag_live_ = std::max(max_dag_live_, pending_dag_tasks_);
            }

            for (std::size_t i = 0; i < dep_count; ++i) {
                const int dep = deps[i];
                if (dep < 0) {
                    continue;
                }

                bool duplicate = false;
                for (std::size_t prev = 0; prev < i; ++prev) {
                    if (deps[prev] == dep) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) {
                    continue;
                }

                auto producer = latest_producer_.find(dep);
                if (producer == latest_producer_.end()) {
                    if (profiling) {
                        ++profile_dag_missing_deps_;
                        if (profile_output_keys_.find(dep) == profile_output_keys_.end()) {
                            ++profile_dag_first_touch_deps_;
                        }
                    }
                    continue;
                }

                DagNode &producer_node = dag_nodes_[producer->second];
                if (!producer_node.completed) {
                    const int edge_index = static_cast<int>(successor_edges_.size());
                    successor_edges_.push_back({node_index, -1});
                    if (producer_node.last_successor >= 0) {
                        successor_edges_[static_cast<std::size_t>(
                                             producer_node.last_successor)]
                            .next = edge_index;
                    } else {
                        producer_node.first_successor = edge_index;
                    }
                    producer_node.last_successor = edge_index;
                    ++producer_node.successor_count;
                    ++dag_nodes_[node_index].pending;
                    if (profiling) {
                        ++profile_dag_edges_;
                        max_dag_successors_ =
                            std::max(max_dag_successors_, producer_node.successor_count);
                    }
                } else if (profiling) {
                    ++profile_dag_satisfied_deps_;
                }
            }

            if (profiling) {
                max_dag_pending_ =
                    std::max(max_dag_pending_,
                             static_cast<std::size_t>(dag_nodes_[node_index].pending));
            }

            if (output >= 0) {
                latest_producer_[output] = node_index;
                if (profiling) {
                    profile_output_keys_.insert(output);
                }
            }

            if (dag_nodes_[node_index].pending == 0) {
                enqueueTaskLocked({fn, context, profiling ? nowNs() : 0, node_index});
                if (profiling) {
                    ++profile_dag_initial_ready_;
                }
                should_notify = (tasks_.size() - task_head_) <= workers_.size();
            }
        }

        if (should_notify) {
            work_cv_.notify_one();
        }
        if (max_live_window_ != 0) {
            drainReadyTasksForLiveWindow();
        }
    }

    void enqueueTask(Task task) {
        bool should_notify = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            enqueueTaskLocked(task);
            const std::size_t ready = tasks_.size() - task_head_;
            should_notify = ready <= workers_.size();
        }
        if (should_notify) {
            work_cv_.notify_one();
        }
    }

    void *allocate(std::size_t size) {
        constexpr std::size_t alignment = alignof(std::max_align_t);
        constexpr std::size_t default_chunk_size = 1 << 20;
        const std::size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);
        if (chunks_.empty() || chunk_offset_ + aligned_size > chunk_size_) {
            chunk_size_ = std::max(default_chunk_size, aligned_size);
            chunks_.push_back(std::make_unique<unsigned char[]>(chunk_size_));
            chunk_offset_ = 0;
        }

        void *memory = chunks_.back().get() + chunk_offset_;
        chunk_offset_ += aligned_size;
        return memory;
    }

    void wait() {
        if (work_stealing_) {
            waitWorkStealing();
            return;
        }
        const bool profiling = profile_enabled_.load(std::memory_order_relaxed);
        const std::uint64_t wait_enter_ns = profiling ? nowNs() : 0;
        flushPendingTasks();
        if (profiling) {
            std::lock_guard<std::mutex> lock(mutex_);
            recordWaitEntryLocked();
        }
        if (workers_.empty()) {
            recordWaitProfile(profiling, wait_enter_ns);
            return;
        }

        while (true) {
            std::array<Task, kMaxTaskBatch> batch{};
            std::size_t batch_count = 0;
            bool dag_deadlock = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (task_head_ < tasks_.size()) {
                    batch_count = takeReadyTasksLocked(batch.data());
                } else if (active_tasks_ == 0) {
                    if (pending_dag_tasks_ == 0) {
                        clearCompletedDagStateLocked();
                        break;
                    }
                    dag_deadlock = true;
                } else {
                    const bool profiling = profile_enabled_.load(std::memory_order_relaxed);
                    const std::uint64_t wait_start_ns = profiling ? nowNs() : 0;
                    done_cv_.wait(lock);
                    if (profiling) {
                        main_wait_ns_ += nowNs() - wait_start_ns;
                    }
                    continue;
                }
            }

            if (dag_deadlock) {
                throw std::runtime_error("compiler2026 runtime DAG has unresolved dependencies");
            }

            BatchProfile profile =
                runBatch(batch.data(), batch_count,
                         profile_enabled_.load(std::memory_order_relaxed));

            {
                std::lock_guard<std::mutex> lock(mutex_);
                active_tasks_ -= batch_count;
                const std::size_t released = completeDagTasksLocked(batch.data(), batch_count);
                recordBatchProfileLocked(batch.data(), batch_count, profile, false);
                recordDagReleaseBatchLocked(released);
                if (released > 0) {
                    work_cv_.notify_all();
                }
                if (tasks_.empty() && active_tasks_ == 0) {
                    done_cv_.notify_all();
                }
            }
        }

        if (worker_error_) {
            std::exception_ptr error = worker_error_;
            worker_error_ = nullptr;
            recordWaitProfile(profiling, wait_enter_ns);
            std::rethrow_exception(error);
        }
        recordWaitProfile(profiling, wait_enter_ns);
    }

    __attribute__((noinline, cold)) void waitForKey(int key) {
        if (key < 0) {
            return;
        }
        if (work_stealing_) {
            // The work-stealing path builds its DAG single-threaded and has no
            // per-key wait; draining everything is a correct (conservative)
            // superset of waiting for one key's producer.
            waitWorkStealing();
            return;
        }

        const bool profiling = profile_enabled_.load(std::memory_order_relaxed);
        const std::uint64_t wait_enter_ns = profiling ? nowNs() : 0;
        flushPendingTasks();
        if (workers_.empty()) {
            recordWaitProfile(profiling, wait_enter_ns);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (profiling) {
                recordWaitEntryLocked();
            }
            ++key_waiters_;
        }

        try {
            while (true) {
                std::array<Task, kMaxTaskBatch> batch{};
                std::size_t batch_count = 0;
                bool dag_deadlock = false;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    auto producer = latest_producer_.find(key);
                    if (producer == latest_producer_.end() ||
                        dag_nodes_[static_cast<std::size_t>(producer->second)].completed) {
                        break;
                    }

                    if (task_head_ < tasks_.size()) {
                        batch_count = takeReadyTasksLocked(batch.data());
                    } else if (active_tasks_ == 0) {
                        dag_deadlock = true;
                    } else {
                        const bool wait_profiling =
                            profile_enabled_.load(std::memory_order_relaxed);
                        const std::uint64_t wait_start_ns =
                            wait_profiling ? nowNs() : 0;
                        done_cv_.wait(lock);
                        if (wait_profiling) {
                            main_wait_ns_ += nowNs() - wait_start_ns;
                        }
                        continue;
                    }
                }

                if (dag_deadlock) {
                    throw std::runtime_error(
                        "compiler2026 runtime DAG key wait has unresolved dependencies");
                }

                BatchProfile profile =
                    runBatch(batch.data(), batch_count,
                             profile_enabled_.load(std::memory_order_relaxed));

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    active_tasks_ -= batch_count;
                    const std::size_t released =
                        completeDagTasksLocked(batch.data(), batch_count);
                    recordBatchProfileLocked(batch.data(), batch_count, profile, false);
                    recordDagReleaseBatchLocked(released);
                    if (released > 0) {
                        work_cv_.notify_all();
                    }
                    if (key_waiters_ > 0 || (tasks_.empty() && active_tasks_ == 0)) {
                        done_cv_.notify_all();
                    }
                }
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --key_waiters_;
            }
            recordWaitProfile(profiling, wait_enter_ns);
            throw;
        }

        std::exception_ptr error;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --key_waiters_;
            if (worker_error_) {
                error = worker_error_;
                worker_error_ = nullptr;
            }
        }
        if (error) {
            recordWaitProfile(profiling, wait_enter_ns);
            std::rethrow_exception(error);
        }
        recordWaitProfile(profiling, wait_enter_ns);
    }

private:
    void pinCurrentWorker(std::size_t worker_index) {
#ifdef __linux__
        if (!pin_workers_ || worker_cpus_.empty()) {
            return;
        }
        const int cpu = worker_cpus_[worker_index % worker_cpus_.size()];
        cpu_set_t target;
        CPU_ZERO(&target);
        CPU_SET(cpu, &target);
        (void)pthread_setaffinity_np(pthread_self(), sizeof(target), &target);
#else
        (void)worker_index;
#endif
    }

    void workerLoop(std::size_t worker_index) {
        if (work_stealing_) {
            workerLoopWorkStealing(worker_index);
            return;
        }
        pinCurrentWorker(worker_index);
        while (true) {
            std::array<Task, kMaxTaskBatch> batch{};
            std::size_t batch_count = 0;
            const bool profiling = profile_enabled_.load(std::memory_order_relaxed);
            const std::uint64_t wait_start_ns = profiling ? nowNs() : 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                work_cv_.wait(lock, [this]() { return stopping_ || task_head_ < tasks_.size(); });
                if (profiling) {
                    worker_idle_ns_ += nowNs() - wait_start_ns;
                }
                if (stopping_ && task_head_ == tasks_.size()) {
                    return;
                }
                batch_count = takeReadyTasksLocked(batch.data());
            }

            BatchProfile profile = runBatch(batch.data(), batch_count, profiling);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                active_tasks_ -= batch_count;
                const std::size_t released = completeDagTasksLocked(batch.data(), batch_count);
                recordBatchProfileLocked(batch.data(), batch_count, profile, true);
                recordDagReleaseBatchLocked(released);
                if (released > 0) {
                    work_cv_.notify_all();
                }
                if (key_waiters_ > 0 || (tasks_.empty() && active_tasks_ == 0)) {
                    done_cv_.notify_all();
                }
            }
        }
    }

    // ---- Work-stealing path (opt-in, default off) -------------------------
    //
    // Design contract that keeps this correct without a global hot-path lock:
    //   * The DAG (nodes, successor edges, latest_producer_) is built only by
    //     the submitting thread while workers are parked (ws_run_ == false), so
    //     submit never races task completion. successor_edges_ are therefore
    //     immutable for the duration of a drain.
    //   * During the drain only per-node atomic pending counters and the
    //     per-shard ready deques change. Termination is a single atomic
    //     outstanding-task counter reaching zero.
    // Restriction: incompatible with the interleaved cross-panel submit path
    // (waitForKey); intended for the default panel-barrier schedule.

    void submitWorkStealing(TaskFn fn, void *context, const int *deps,
                            std::size_t dep_count, int output) {
        if (workers_.empty()) {
            // Serial fallback: run inline, mirroring the non-stealing path.
            fn(context);
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const int node_index = static_cast<int>(dag_nodes_.size());
        dag_nodes_.push_back({fn, context, 0, false});

        for (std::size_t i = 0; i < dep_count; ++i) {
            const int dep = deps[i];
            if (dep < 0) {
                continue;
            }
            bool duplicate = false;
            for (std::size_t prev = 0; prev < i; ++prev) {
                if (deps[prev] == dep) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }

            auto producer = latest_producer_.find(dep);
            if (producer == latest_producer_.end()) {
                continue;
            }
            // Producers are never completed during the (parked-worker) submit
            // phase, so every found producer becomes a pending edge.
            const int edge_index = static_cast<int>(successor_edges_.size());
            successor_edges_.push_back({node_index, -1});
            DagNode &producer_node = dag_nodes_[producer->second];
            if (producer_node.last_successor >= 0) {
                successor_edges_[static_cast<std::size_t>(producer_node.last_successor)]
                    .next = edge_index;
            } else {
                producer_node.first_successor = edge_index;
            }
            producer_node.last_successor = edge_index;
            ++dag_nodes_[node_index].pending;
        }

        if (output >= 0) {
            latest_producer_[output] = node_index;
        }
        if (dag_nodes_[node_index].pending == 0) {
            ws_initial_ready_.push_back(node_index);
        }
    }

    void waitWorkStealing() {
        if (workers_.empty()) {
            return;  // serial submit already executed inline
        }

        std::size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            count = dag_nodes_.size();
            if (count == 0) {
                return;
            }
            if (count > ws_pending_cap_) {
                ws_pending_.reset(new std::atomic<int>[count]);
                ws_pending_cap_ = count;
            }
            for (std::size_t i = 0; i < count; ++i) {
                ws_pending_[i].store(dag_nodes_[i].pending, std::memory_order_relaxed);
            }
            ws_outstanding_.store(count, std::memory_order_relaxed);

            const std::size_t participants = workers_.size() + 1;
            for (std::size_t k = 0; k < ws_initial_ready_.size(); ++k) {
                const int ni = ws_initial_ready_[k];
                WsDeque &shard = ws_queues_[k % participants];
                std::lock_guard<std::mutex> sl(shard.m);
                shard.q.push_back({dag_nodes_[static_cast<std::size_t>(ni)].fn,
                                   dag_nodes_[static_cast<std::size_t>(ni)].context, 0, ni});
            }
            ws_initial_ready_.clear();
            ws_epoch_.fetch_add(1, std::memory_order_seq_cst);
            ws_run_.store(true, std::memory_order_seq_cst);
        }
        ws_start_cv_.notify_all();

        driveWorkStealing(0);

        std::exception_ptr error;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ws_run_.store(false, std::memory_order_seq_cst);
            ws_done_cv_.wait(lock, [this]() { return ws_active_workers_ == 0; });
            clearCompletedDagStateLocked();
            if (worker_error_) {
                error = worker_error_;
                worker_error_ = nullptr;
            }
        }
        if (error) {
            std::rethrow_exception(error);
        }
    }

    void driveWorkStealing(std::size_t participant) {
        constexpr int kSpinLimit = 256;  // brief spin before sleeping
        // Match the single-queue path's batched dequeue: amortize per-task
        // synchronization by draining several tasks from the own shard at once.
        const std::size_t batch_limit =
            std::max<std::size_t>(1, std::min(task_batch_size_, kMaxTaskBatch));
        std::array<Task, kMaxTaskBatch> batch{};
        int idle_spins = 0;
        while (true) {
            const std::uint64_t seq_before =
                ws_push_seq_.load(std::memory_order_seq_cst);
            std::size_t got = popLocalBatchLocked(participant, batch.data(), batch_limit);
            if (got == 0 && stealOneLocked(participant, batch[0])) {
                got = 1;
            }
            if (got > 0) {
                idle_spins = 0;
                for (std::size_t i = 0; i < got; ++i) {
                    try {
                        batch[i].fn(batch[i].context);
                    } catch (...) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (!worker_error_) {
                            worker_error_ = std::current_exception();
                        }
                    }
                    if (batch[i].dag_node >= 0) {
                        releaseSuccessorsWorkStealing(batch[i].dag_node, participant);
                    }
                }
                // Decrement only after successors are pushed, so outstanding
                // never hits zero while more work is still becoming ready.
                if (ws_outstanding_.fetch_sub(got, std::memory_order_seq_cst) == got) {
                    // Last tasks done: wake any sleepers so they can exit.
                    std::lock_guard<std::mutex> lock(ws_idle_mutex_);
                    ws_idle_cv_.notify_all();
                }
                continue;
            }

            if (ws_outstanding_.load(std::memory_order_seq_cst) == 0) {
                return;
            }
            if (++idle_spins < kSpinLimit) {
                std::this_thread::yield();  // bounded spin: work may appear soon
                continue;
            }
            // Sustained idle: sleep until a producer pushes work or the drain
            // ends. The push-sequence guard (read before our failed scan)
            // closes the lost-wakeup window: if any task was pushed since we
            // scanned, we skip the sleep and retry instead of blocking. The
            // timeout is only a coarse backstop.
            {
                std::unique_lock<std::mutex> lock(ws_idle_mutex_);
                ws_sleepers_.fetch_add(1, std::memory_order_seq_cst);
                if (ws_outstanding_.load(std::memory_order_seq_cst) != 0 &&
                    ws_push_seq_.load(std::memory_order_seq_cst) == seq_before) {
                    ws_idle_cv_.wait_for(lock, std::chrono::milliseconds(2));
                }
                ws_sleepers_.fetch_sub(1, std::memory_order_seq_cst);
            }
            idle_spins = 0;
        }
    }

    void releaseSuccessorsWorkStealing(int node_index, std::size_t participant) {
        const DagNode &node = dag_nodes_[static_cast<std::size_t>(node_index)];
        for (int edge_index = node.first_successor; edge_index >= 0;) {
            const DagEdge &edge = successor_edges_[static_cast<std::size_t>(edge_index)];
            edge_index = edge.next;
            const int successor = edge.successor;
            if (ws_pending_[static_cast<std::size_t>(successor)].fetch_sub(
                    1, std::memory_order_seq_cst) == 1) {
                {
                    WsDeque &shard = ws_queues_[participant];
                    std::lock_guard<std::mutex> sl(shard.m);
                    shard.q.push_back(
                        {dag_nodes_[static_cast<std::size_t>(successor)].fn,
                         dag_nodes_[static_cast<std::size_t>(successor)].context, 0,
                         successor});
                }
                ws_push_seq_.fetch_add(1, std::memory_order_seq_cst);
                // Wake one sleeper only if someone is actually parked.
                if (ws_sleepers_.load(std::memory_order_seq_cst) > 0) {
                    std::lock_guard<std::mutex> lock(ws_idle_mutex_);
                    ws_idle_cv_.notify_one();
                }
            }
        }
    }

    std::size_t popLocalBatchLocked(std::size_t participant, Task *out,
                                    std::size_t limit) {
        WsDeque &own = ws_queues_[participant];
        std::lock_guard<std::mutex> sl(own.m);
        std::size_t count = 0;
        while (count < limit && !own.q.empty()) {
            out[count++] = own.q.back();  // LIFO on own shard for locality
            own.q.pop_back();
        }
        return count;
    }

    bool stealOneLocked(std::size_t participant, Task &out) {
        const std::size_t participants = workers_.size() + 1;
        for (std::size_t k = 1; k < participants; ++k) {
            WsDeque &victim = ws_queues_[(participant + k) % participants];
            std::unique_lock<std::mutex> sl(victim.m, std::try_to_lock);
            if (sl.owns_lock() && !victim.q.empty()) {
                out = victim.q.front();  // FIFO steal from the other end
                victim.q.pop_front();
                return true;
            }
        }
        return false;
    }

    void workerLoopWorkStealing(std::size_t worker_index) {
        pinCurrentWorker(worker_index);
        const std::size_t participant = worker_index + 1;
        std::uint64_t seen_epoch = 0;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ws_start_cv_.wait(lock, [this, seen_epoch]() {
                    return stopping_ ||
                           (ws_run_.load(std::memory_order_seq_cst) &&
                            ws_epoch_.load(std::memory_order_seq_cst) != seen_epoch);
                });
                if (stopping_) {
                    return;
                }
                seen_epoch = ws_epoch_.load(std::memory_order_seq_cst);
                ++ws_active_workers_;
            }

            driveWorkStealing(participant);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (--ws_active_workers_ == 0) {
                    ws_done_cv_.notify_all();
                }
            }
        }
    }

    void flushPendingTasks() {
        if (pending_count_ == 0) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.insert(tasks_.end(), pending_tasks_.begin(),
                          pending_tasks_.begin() + pending_count_);
            recordReadyWidthLocked(tasks_.size() - task_head_);
            if (profile_enabled_.load(std::memory_order_relaxed)) {
                ++submit_flushes_;
            }
        }
        pending_count_ = 0;
        work_cv_.notify_all();
    }

    std::size_t takeReadyTasksLocked(Task *batch) {
        const std::size_t available = tasks_.size() - task_head_;
        const std::size_t count = chooseBatchCount(available);
        for (std::size_t i = 0; i < count; ++i) {
            batch[i] = tasks_[task_head_ + i];
        }
        task_head_ += count;
        active_tasks_ += count;
        if (profile_enabled_.load(std::memory_order_relaxed)) {
            ++dequeue_batches_;
            max_dequeue_batch_ = std::max(max_dequeue_batch_, count);
        }
        if (task_head_ == tasks_.size()) {
            tasks_.clear();
            task_head_ = 0;
        }
        return count;
    }

    void enqueueTaskLocked(Task task) {
        tasks_.push_back(task);
        recordReadyWidthLocked(tasks_.size() - task_head_);
    }

    std::size_t completeDagTasksLocked(const Task *batch, std::size_t count) {
        std::size_t released = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const int node_index = batch[i].dag_node;
            if (node_index < 0) {
                continue;
            }

            DagNode &node = dag_nodes_[static_cast<std::size_t>(node_index)];
            if (node.completed) {
                continue;
            }
            node.completed = true;
            --pending_dag_tasks_;

            for (int edge_index = node.first_successor; edge_index >= 0;) {
                const DagEdge &edge = successor_edges_[static_cast<std::size_t>(edge_index)];
                edge_index = edge.next;
                DagNode &successor = dag_nodes_[static_cast<std::size_t>(edge.successor)];
                --successor.pending;
                if (successor.pending == 0) {
                    enqueueTaskLocked({successor.fn, successor.context,
                                       profile_enabled_.load(std::memory_order_relaxed) ? nowNs() : 0,
                                       edge.successor});
                    if (profile_enabled_.load(std::memory_order_relaxed)) {
                        ++profile_dag_released_;
                    }
                    ++released;
                }
            }
        }
        return released;
    }

    void clearCompletedDagStateLocked() {
        dag_nodes_.clear();
        successor_edges_.clear();
        latest_producer_.clear();
        profile_output_keys_.clear();
        pending_dag_tasks_ = 0;
    }

    void drainReadyTasksForLiveWindow() {
        if (max_live_window_ == 0 || workers_.empty()) {
            return;
        }

        while (true) {
            std::array<Task, kMaxTaskBatch> batch{};
            std::size_t batch_count = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (pending_dag_tasks_ <= max_live_window_ || task_head_ >= tasks_.size()) {
                    return;
                }
                batch_count = takeReadyTasksLocked(batch.data());
            }

            BatchProfile profile =
                runBatch(batch.data(), batch_count,
                         profile_enabled_.load(std::memory_order_relaxed));

            {
                std::lock_guard<std::mutex> lock(mutex_);
                active_tasks_ -= batch_count;
                const std::size_t released = completeDagTasksLocked(batch.data(), batch_count);
                recordBatchProfileLocked(batch.data(), batch_count, profile, false);
                recordDagReleaseBatchLocked(released);
                if (released > 0) {
                    work_cv_.notify_all();
                }
                if (key_waiters_ > 0 || (tasks_.empty() && active_tasks_ == 0)) {
                    done_cv_.notify_all();
                }
            }
        }
    }

    std::size_t chooseBatchCount(std::size_t available) const {
        if (task_batch_size_ <= 1) {
            return 1;
        }

        const std::size_t participants = workers_.size() + 1;
        if (available <= participants * 2) {
            return 1;
        }

        const std::size_t fair_batch = std::max<std::size_t>(1, available / participants);
        return std::min(task_batch_size_, std::min(fair_batch, available));
    }

    BatchProfile runBatch(const Task *batch, std::size_t count, bool profiling) {
        BatchProfile profile;
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint64_t start_ns = profiling ? nowNs() : 0;
            if (profiling && batch[i].enqueue_ns != 0) {
                profile.task_queue_ns[i] = start_ns - batch[i].enqueue_ns;
                profile.total_queue_ns += profile.task_queue_ns[i];
            }
            try {
                batch[i].fn(batch[i].context);
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!worker_error_) {
                    worker_error_ = std::current_exception();
                }
            }
            if (profiling) {
                const std::uint64_t elapsed_ns = nowNs() - start_ns;
                profile.task_exec_ns[i] = elapsed_ns;
                profile.total_exec_ns += elapsed_ns;
            }
        }
        return profile;
    }

    void recordReadyWidthLocked(std::size_t ready) {
        if (!profile_enabled_.load(std::memory_order_relaxed)) {
            return;
        }
        max_ready_tasks_ = std::max(max_ready_tasks_, ready);
        ++ready_width_samples_;
        ready_width_sum_ += ready;
    }

    void recordDagReleaseBatchLocked(std::size_t released) {
        if (!profile_enabled_.load(std::memory_order_relaxed) || released == 0) {
            return;
        }
        ++dag_release_batches_;
        max_dag_release_batch_ = std::max(max_dag_release_batch_, released);
    }

    void recordWaitEntryLocked() {
        const std::size_t ready = tasks_.size() - task_head_;
        wait_ready_sum_ += ready;
        wait_active_sum_ += active_tasks_;
        wait_dag_live_sum_ += pending_dag_tasks_;
        max_wait_ready_ = std::max(max_wait_ready_, ready);
        max_wait_active_ = std::max(max_wait_active_, active_tasks_);
        max_wait_dag_live_ = std::max(max_wait_dag_live_, pending_dag_tasks_);
    }

    void recordWaitProfile(bool profiling, std::uint64_t wait_enter_ns) {
        if (!profiling) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ++wait_calls_;
        wait_total_ns_ += nowNs() - wait_enter_ns;
    }

    TaskProfile &profileForTaskLocked(TaskFn fn) {
        for (std::size_t i = 0; i < task_profile_count_; ++i) {
            if (task_profiles_[i].fn == fn) {
                return task_profiles_[i];
            }
        }
        if (task_profile_count_ < task_profiles_.size()) {
            TaskProfile &profile = task_profiles_[task_profile_count_++];
            profile.fn = fn;
            return profile;
        }
        return task_profiles_.back();
    }

    void recordBatchProfileLocked(const Task *batch, std::size_t count,
                                  const BatchProfile &profile, bool worker_thread) {
        if (!profile_enabled_.load(std::memory_order_relaxed)) {
            return;
        }

        total_tasks_ += count;
        total_queue_ns_ += profile.total_queue_ns;
        total_exec_ns_ += profile.total_exec_ns;
        if (worker_thread) {
            worker_tasks_ += count;
        } else {
            main_tasks_ += count;
        }

        for (std::size_t i = 0; i < count; ++i) {
            TaskProfile &task_profile = profileForTaskLocked(batch[i].fn);
            ++task_profile.count;
            task_profile.queue_ns += profile.task_queue_ns[i];
            task_profile.exec_ns += profile.task_exec_ns[i];
        }
    }

    void resetProfile(int n, int b, std::size_t total_threads, std::size_t task_batch_size) {
        profile_enabled_.store(profileEnabledFromEnv(), std::memory_order_relaxed);
        n_ = n;
        b_ = b;
        total_threads_ = total_threads;
        configured_batch_size_ = std::max<std::size_t>(
            1, std::min<std::size_t>(task_batch_size, kMaxTaskBatch));
        total_tasks_ = 0;
        main_tasks_ = 0;
        worker_tasks_ = 0;
        total_queue_ns_ = 0;
        total_exec_ns_ = 0;
        worker_idle_ns_ = 0;
        main_wait_ns_ = 0;
        wait_total_ns_ = 0;
        wait_calls_ = 0;
        wait_ready_sum_ = 0;
        wait_active_sum_ = 0;
        wait_dag_live_sum_ = 0;
        submit_flushes_ = 0;
        dequeue_batches_ = 0;
        profile_dag_nodes_ = 0;
        profile_dag_edges_ = 0;
        profile_dag_satisfied_deps_ = 0;
        profile_dag_missing_deps_ = 0;
        profile_dag_first_touch_deps_ = 0;
        profile_dag_initial_ready_ = 0;
        profile_dag_released_ = 0;
        dag_release_batches_ = 0;
        ready_width_samples_ = 0;
        ready_width_sum_ = 0;
        max_dag_pending_ = 0;
        max_dag_successors_ = 0;
        max_dag_live_ = 0;
        max_dag_release_batch_ = 0;
        max_wait_ready_ = 0;
        max_wait_active_ = 0;
        max_wait_dag_live_ = 0;
        max_dequeue_batch_ = 0;
        max_ready_tasks_ = 0;
        task_profile_count_ = 0;
        task_profiles_ = {};
    }

    void reportProfile() {
        if (!profile_enabled_.load(std::memory_order_relaxed)) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        std::fprintf(stderr,
                     "[compiler2026_profile] n=%d b=%d threads=%zu workers=%zu "
                     "batch=%zu tasks=%llu main_tasks=%llu worker_tasks=%llu "
                     "flushes=%llu dequeue_batches=%llu max_batch=%zu max_ready=%zu "
                     "ready_samples=%llu ready_sum=%llu "
                     "dag_nodes=%llu dag_edges=%llu dag_satisfied_deps=%llu "
                     "dag_missing_deps=%llu dag_first_touch_deps=%llu "
                     "dag_initial_ready=%llu "
                     "dag_released=%llu dag_release_batches=%llu "
                     "max_dag_release_batch=%zu max_dag_pending=%zu max_dag_successors=%zu "
                     "max_dag_live=%zu queue_ms=%.3f exec_ms=%.3f worker_idle_ms=%.3f "
                     "main_wait_ms=%.3f wait_calls=%llu wait_ms=%.3f "
                     "wait_ready_sum=%llu wait_active_sum=%llu wait_dag_live_sum=%llu "
                     "max_wait_ready=%zu max_wait_active=%zu max_wait_dag_live=%zu\n",
                     n_, b_, total_threads_, workers_.size(), configured_batch_size_,
                     static_cast<unsigned long long>(total_tasks_),
                     static_cast<unsigned long long>(main_tasks_),
                     static_cast<unsigned long long>(worker_tasks_),
                     static_cast<unsigned long long>(submit_flushes_),
                     static_cast<unsigned long long>(dequeue_batches_),
                     max_dequeue_batch_, max_ready_tasks_,
                     static_cast<unsigned long long>(ready_width_samples_),
                     static_cast<unsigned long long>(ready_width_sum_),
                     static_cast<unsigned long long>(profile_dag_nodes_),
                     static_cast<unsigned long long>(profile_dag_edges_),
                     static_cast<unsigned long long>(profile_dag_satisfied_deps_),
                     static_cast<unsigned long long>(profile_dag_missing_deps_),
                     static_cast<unsigned long long>(profile_dag_first_touch_deps_),
                     static_cast<unsigned long long>(profile_dag_initial_ready_),
                     static_cast<unsigned long long>(profile_dag_released_),
                     static_cast<unsigned long long>(dag_release_batches_),
                     max_dag_release_batch_, max_dag_pending_, max_dag_successors_,
                     max_dag_live_,
                     static_cast<double>(total_queue_ns_) / 1000000.0,
                     static_cast<double>(total_exec_ns_) / 1000000.0,
                     static_cast<double>(worker_idle_ns_) / 1000000.0,
                     static_cast<double>(main_wait_ns_) / 1000000.0,
                     static_cast<unsigned long long>(wait_calls_),
                     static_cast<double>(wait_total_ns_) / 1000000.0,
                     static_cast<unsigned long long>(wait_ready_sum_),
                     static_cast<unsigned long long>(wait_active_sum_),
                     static_cast<unsigned long long>(wait_dag_live_sum_),
                     max_wait_ready_, max_wait_active_, max_wait_dag_live_);

        for (std::size_t i = 0; i < task_profile_count_; ++i) {
            const TaskProfile &profile = task_profiles_[i];
            const char *name = (profile.name != nullptr) ? profile.name : "unknown";
            std::fprintf(stderr,
                         "[compiler2026_profile_task] name=%s count=%llu "
                         "queue_ms=%.3f exec_ms=%.3f\n",
                         name, static_cast<unsigned long long>(profile.count),
                         static_cast<double>(profile.queue_ns) / 1000000.0,
                         static_cast<double>(profile.exec_ns) / 1000000.0);
        }
    }

    std::vector<std::thread> workers_;
    std::vector<Task> tasks_;
    std::vector<DagNode> dag_nodes_;
    std::vector<DagEdge> successor_edges_;
    std::unordered_map<int, int> latest_producer_;
    std::unordered_set<int> profile_output_keys_;
    std::size_t pending_dag_tasks_ = 0;
    std::array<Task, kMaxTaskBatch> pending_tasks_{};
    std::size_t task_head_ = 0;
    std::size_t pending_count_ = 0;
    std::size_t task_batch_size_ = 1;
    std::size_t max_live_window_ = 0;
    std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable done_cv_;
    std::vector<std::unique_ptr<unsigned char[]>> chunks_;
    std::size_t chunk_size_ = 0;
    std::size_t chunk_offset_ = 0;
    std::size_t active_tasks_ = 0;
    // Only wait_key users need completion notifications before the whole DAG drains.
    std::size_t key_waiters_ = 0;
    bool stopping_ = false;
    std::exception_ptr worker_error_;
    std::atomic<bool> profile_enabled_{false};
    int n_ = 0;
    int b_ = 0;
    std::size_t total_threads_ = 1;
    std::size_t configured_batch_size_ = 1;
    std::uint64_t total_tasks_ = 0;
    std::uint64_t main_tasks_ = 0;
    std::uint64_t worker_tasks_ = 0;
    std::uint64_t total_queue_ns_ = 0;
    std::uint64_t total_exec_ns_ = 0;
    std::uint64_t worker_idle_ns_ = 0;
    std::uint64_t main_wait_ns_ = 0;
    std::uint64_t wait_total_ns_ = 0;
    std::uint64_t wait_calls_ = 0;
    std::uint64_t wait_ready_sum_ = 0;
    std::uint64_t wait_active_sum_ = 0;
    std::uint64_t wait_dag_live_sum_ = 0;
    std::uint64_t submit_flushes_ = 0;
    std::uint64_t dequeue_batches_ = 0;
    std::uint64_t profile_dag_nodes_ = 0;
    std::uint64_t profile_dag_edges_ = 0;
    std::uint64_t profile_dag_satisfied_deps_ = 0;
    std::uint64_t profile_dag_missing_deps_ = 0;
    std::uint64_t profile_dag_first_touch_deps_ = 0;
    std::uint64_t profile_dag_initial_ready_ = 0;
    std::uint64_t profile_dag_released_ = 0;
    std::uint64_t dag_release_batches_ = 0;
    std::uint64_t ready_width_samples_ = 0;
    std::uint64_t ready_width_sum_ = 0;
    std::size_t max_dequeue_batch_ = 0;
    std::size_t max_ready_tasks_ = 0;
    std::size_t max_dag_release_batch_ = 0;
    std::size_t max_dag_pending_ = 0;
    std::size_t max_dag_successors_ = 0;
    std::size_t max_dag_live_ = 0;
    std::size_t max_wait_ready_ = 0;
    std::size_t max_wait_active_ = 0;
    std::size_t max_wait_dag_live_ = 0;
    std::array<TaskProfile, kMaxProfiledTasks> task_profiles_{};
    std::size_t task_profile_count_ = 0;
    bool pin_workers_ = false;
    std::vector<int> worker_cpus_;

    // Work-stealing path state (used only when work_stealing_ == true).
    bool work_stealing_ = false;
    std::vector<WsDeque> ws_queues_;
    std::vector<int> ws_initial_ready_;
    std::unique_ptr<std::atomic<int>[]> ws_pending_;
    std::size_t ws_pending_cap_ = 0;
    std::atomic<std::size_t> ws_outstanding_{0};
    std::atomic<bool> ws_run_{false};
    std::atomic<std::uint64_t> ws_epoch_{0};
    std::size_t ws_active_workers_ = 0;
    std::condition_variable ws_start_cv_;
    std::condition_variable ws_done_cv_;
    std::mutex ws_idle_mutex_;
    std::condition_variable ws_idle_cv_;
    std::atomic<int> ws_sleepers_{0};
    std::atomic<std::uint64_t> ws_push_seq_{0};
};

thread_local std::unique_ptr<AsyncRuntime> runtime;

std::size_t resolveThreadCount(int n, int b) {
    std::size_t threads = std::thread::hardware_concurrency();
    if (threads == 0) {
        threads = 1;
    }

    if (const char *env = std::getenv("COMPILER2026_DAG_THREADS")) {
        char *end = nullptr;
        const unsigned long configured = std::strtoul(env, &end, 10);
        if (end != env && *end == '\0' && configured > 0) {
            threads = static_cast<std::size_t>(configured);
        }
    }

    if (n <= 0 || b <= 0) {
        return 1;
    }

    const int block_count = n / b;
    if (block_count <= 1) {
        return 1;
    }
    return std::max<std::size_t>(1, std::min<std::size_t>(threads, block_count));
}

int asyncMinBlockSize() {
    int threshold = 18;
    if (const char *env = std::getenv("COMPILER2026_ASYNC_MIN_B")) {
        char *end = nullptr;
        const long configured = std::strtol(env, &end, 10);
        if (end != env && *end == '\0' && configured > 0) {
            threshold = static_cast<int>(configured);
        }
    }
    return threshold;
}

int asyncMinBlockCount() {
    int threshold = 2;
    if (const char *env = std::getenv("COMPILER2026_ASYNC_MIN_BLOCKS")) {
        char *end = nullptr;
        const long configured = std::strtol(env, &end, 10);
        if (end != env && *end == '\0' && configured > 0) {
            threshold = static_cast<int>(configured);
        }
    }
    return threshold;
}

std::size_t reserveTaskCount(int n, int b) {
    if (n <= 0 || b <= 0) {
        return 0;
    }
    const int block_count = n / b;
    if (block_count <= 1) {
        return 0;
    }
    const std::size_t trailing = static_cast<std::size_t>(block_count - 1);
    return trailing + trailing * (trailing + 1) / 2;
}

std::size_t selectTaskBatchSize(int n, int b, std::size_t worker_threads) {
    if (worker_threads == 0 || n <= 0 || b <= 0) {
        return 1;
    }

    if (const char *env = std::getenv("COMPILER2026_TASK_BATCH")) {
        char *end = nullptr;
        const unsigned long configured = std::strtoul(env, &end, 10);
        if (end != env && *end == '\0' && configured > 0) {
            return std::min<std::size_t>(configured, kMaxTaskBatch);
        }
    }

    const int block_count = n / b;
    if (block_count <= 1) {
        return 1;
    }

    std::size_t batch = 1;
    if (b <= 32) {
        batch = 8;
    } else if (b <= 64) {
        batch = 8;
    } else if (b <= 128) {
        batch = 4;
    }

    const std::size_t participants = worker_threads + 1;
    const std::size_t blocks = static_cast<std::size_t>(block_count);
    if (blocks <= participants * 2) {
        return 1;
    }
    if (blocks <= participants * 4) {
        return std::min<std::size_t>(batch, 2);
    }
    if (blocks <= participants * 8) {
        return std::min<std::size_t>(batch, 4);
    }
    return batch;
}

std::size_t dagMaxLiveWindow() {
    if (const char *env = std::getenv("COMPILER2026_DAG_MAX_LIVE")) {
        char *end = nullptr;
        const unsigned long configured = std::strtoul(env, &end, 10);
        if (end != env && *end == '\0') {
            return static_cast<std::size_t>(configured);
        }
    }
    return 0;
}

AsyncRuntime &activeRuntime() {
    if (!runtime) {
        runtime = std::make_unique<AsyncRuntime>(std::thread::hardware_concurrency());
    }
    return *runtime;
}

}  // namespace

extern "C" void compiler2026_runtime_begin(int n, int b) {
    const std::size_t total_threads = (b < asyncMinBlockSize()) ? 1 : resolveThreadCount(n, b);
    const std::size_t worker_threads = (total_threads > 1) ? (total_threads - 1) : 0;
    const std::size_t reserve_tasks = reserveTaskCount(n, b);
    const std::size_t task_batch_size = selectTaskBatchSize(n, b, worker_threads);
    const std::size_t max_live_window = dagMaxLiveWindow();
    if (!runtime || runtime->workerCount() != worker_threads) {
        runtime = std::make_unique<AsyncRuntime>(worker_threads);
        runtime->resetForCall(reserve_tasks, task_batch_size, n, b, total_threads,
                              max_live_window);
        return;
    }
    runtime->resetForCall(reserve_tasks, task_batch_size, n, b, total_threads,
                          max_live_window);
}

extern "C" int compiler2026_runtime_should_async(int n, int b) {
    const int min_b = asyncMinBlockSize();
    const int min_blocks = asyncMinBlockCount();
    std::size_t total_threads = 1;
    int block_count = 0;
    bool enabled = false;
    const char *reason = "invalid";

    if (n > 0 && b > 0) {
        block_count = n / b;
        if (b < min_b) {
            reason = "small_b";
        } else if (block_count <= 1) {
            reason = "single_block";
        } else if (block_count < min_blocks) {
            reason = "small_blocks";
        } else {
            total_threads = resolveThreadCount(n, b);
            if (total_threads > 1) {
                enabled = true;
                reason = "enabled";
            } else {
                reason = "threads";
            }
        }
    }

    if (profileEnabledFromEnv()) {
        std::fprintf(stderr,
                     "[compiler2026_async_decision] n=%d b=%d block_count=%d "
                     "threshold=%d min_blocks=%d threads=%zu enabled=%d reason=%s\n",
                     n, b, block_count, min_b, min_blocks, total_threads,
                     enabled ? 1 : 0, reason);
    }

    return enabled ? 1 : 0;
}

extern "C" void compiler2026_runtime_register_task(TaskFn fn, const char *name) {
    activeRuntime().registerTask(fn, name);
}

extern "C" void *compiler2026_runtime_alloc(std::size_t size) {
    return activeRuntime().allocate(size);
}

extern "C" void compiler2026_runtime_submit(TaskFn fn, void *context) {
    activeRuntime().submit(fn, context);
}

extern "C" void compiler2026_runtime_submit_deps(TaskFn fn, void *context,
                                                   int dep_a, int dep_b, int output) {
    const int deps[] = {dep_a, dep_b};
    activeRuntime().submitWithDeps(fn, context, deps, 2, output);
}

extern "C" void compiler2026_runtime_submit_deps3(TaskFn fn, void *context,
                                                   int dep_a, int dep_b, int dep_c,
                                                   int output) {
    const int deps[] = {dep_a, dep_b, dep_c};
    activeRuntime().submitWithDeps(fn, context, deps, 3, output);
}

extern "C" void compiler2026_runtime_wait() {
    activeRuntime().wait();
}

extern "C" void compiler2026_runtime_wait_key(int key) {
    activeRuntime().waitForKey(key);
}

extern "C" void compiler2026_runtime_end() {
    if (runtime) {
        runtime->finishCall();
    }
}
