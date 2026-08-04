#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
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
constexpr std::size_t kMaxTaskBatch = 32;
constexpr std::size_t kMaxProfiledTasks = 8;
// Upper bound on how many dependency-aware submits the submitting thread may
// stage before publishing them under one lock acquisition.
constexpr std::size_t kMaxDagSubmitBatch = 32;
// Widest dependency list the runtime API exposes (submit_deps3*).
constexpr std::size_t kMaxDeps = 3;

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

bool criticalPriorityEnabledFromEnv() {
    const char *env = std::getenv("COMPILER2026_DAG_CRITICAL_PRIORITY");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
}

// 0 means "let the runtime size the submit batch from the problem shape".
std::size_t dagSubmitBatchOverride() {
    static const std::size_t value = []() -> std::size_t {
        if (const char *env = std::getenv("COMPILER2026_DAG_SUBMIT_BATCH")) {
            char *end = nullptr;
            const unsigned long configured = std::strtoul(env, &end, 10);
            if (end != env && *end == '\0') {
                return static_cast<std::size_t>(configured);
            }
        }
        return 0;
    }();
    return value;
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
        std::uint8_t priority = 0;
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
        // Reuse the padding after completed: DagNode stays 40 B on 64-bit hosts.
        std::uint8_t priority = 0;
        int first_successor = -1;
        int last_successor = -1;
        std::size_t successor_count = 0;
    };

    struct DagEdge {
        int successor = -1;
        int next = -1;
    };

    // One dependency-aware submit held back by the submitting thread until the
    // staging buffer is published. Dependency keys are resolved to producer
    // node indices here, outside the runtime mutex, because latest_producer_
    // and the node/edge vectors are only ever appended to by the submitter.
    struct StagedSubmit {
        TaskFn fn = nullptr;
        void *context = nullptr;
        std::uint8_t priority = 0;
        std::size_t dep_count = 0;
        // Per dep: >= 0 producer node index, kDepSkip when the dep was
        // negative or a duplicate, kDepMissing when no live producer exists.
        std::array<int, kMaxDeps> dep_nodes{};
        std::array<bool, kMaxDeps> dep_first_touch{};
    };

    static constexpr int kDepSkip = -1;
    static constexpr int kDepMissing = -2;

public:
    explicit AsyncRuntime(std::size_t worker_count)
        : critical_priority_enabled_(criticalPriorityEnabledFromEnv()),
          pin_workers_(workerPinningEnabledFromEnv()),
          worker_cpus_(pin_workers_ ? allowedCpuList() : std::vector<int>{}) {
        if (worker_count == 0) {
            return;
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
        for (auto &queue : priority_tasks_) {
            queue.clear();
        }
        dag_nodes_.clear();
        successor_edges_.clear();
        latest_producer_.clear();
        pending_dag_tasks_ = 0;
        task_head_ = 0;
        priority_task_heads_.fill(0);
        chunk_offset_ = 0;
        pending_count_ = 0;
        task_batch_size_ = std::max<std::size_t>(
            1, std::min<std::size_t>(task_batch_size, kMaxTaskBatch));
        dag_staging_.clear();
        dag_next_node_index_ = 0;
        dag_staging_limit_ = chooseDagStagingLimit(reserve_tasks);
        dag_staging_.reserve(dag_staging_limit_);
        worker_error_ = nullptr;
    }

    // How many dependency-aware submits to withhold before publishing them
    // under one lock. Withholding trades a small publish delay for a large cut
    // in submitter lock acquisitions, so it is only worth doing when the DAG
    // holds clearly more ready work than the pool drains while a batch is being
    // staged. Small block counts fall back to publish-per-submit.
    std::size_t chooseDagStagingLimit(std::size_t reserve_tasks) const {
        if (const std::size_t configured = dagSubmitBatchOverride()) {
            return std::min(configured, kMaxDagSubmitBatch);
        }
        const std::size_t participants = workers_.size() + 1;
        if (participants <= 1) {
            return 1;
        }
        const std::size_t budget = reserve_tasks / (participants * 2);
        return std::max<std::size_t>(1, std::min(budget, kMaxDagSubmitBatch));
    }

    void submit(TaskFn fn, void *context) {
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
                        int output, int priority = 0) {
        const bool profiling = profile_enabled_.load(std::memory_order_relaxed);
        const std::uint8_t task_priority = static_cast<std::uint8_t>(
            critical_priority_enabled_ ? std::max(0, std::min(priority, 3)) : 0);
        if (workers_.empty()) {
            Task task{fn, context, profiling ? nowNs() : 0, -1, task_priority};
            BatchProfile profile = runBatch(&task, 1, profiling);
            if (profiling) {
                std::lock_guard<std::mutex> lock(mutex_);
                recordBatchProfileLocked(&task, 1, profile, false);
            }
            return;
        }

        // Stage outside the runtime mutex. Resolving dependency keys is the
        // expensive part (hash lookups) and touches only submitter-private
        // state, so it must not sit inside the critical section that every
        // worker contends for on dequeue and completion.
        StagedSubmit staged;
        staged.fn = fn;
        staged.context = context;
        staged.priority = task_priority;
        staged.dep_count = std::min(dep_count, kMaxDeps);

        const int node_index =
            static_cast<int>(dag_next_node_index_ + dag_staging_.size());

        for (std::size_t i = 0; i < staged.dep_count; ++i) {
            const int dep = deps[i];
            staged.dep_nodes[i] = kDepSkip;
            staged.dep_first_touch[i] = false;
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
                staged.dep_nodes[i] = kDepMissing;
                if (profiling) {
                    staged.dep_first_touch[i] =
                        profile_output_keys_.find(dep) == profile_output_keys_.end();
                }
                continue;
            }
            staged.dep_nodes[i] = producer->second;
        }

        // A producer recorded here may still be staged rather than published.
        // That is fine: an unpublished node cannot be completed, so the wiring
        // pass below always turns it into a real edge.
        if (output >= 0) {
            latest_producer_[output] = node_index;
            if (profiling) {
                profile_output_keys_.insert(output);
            }
        }

        dag_staging_.push_back(staged);
        if (dag_staging_.size() >= dag_staging_limit_) {
            flushDagStaging();
        }

        if (max_live_window_ != 0) {
            flushDagStaging();
            drainReadyTasksForLiveWindow();
        }
    }

    // Publish every staged submit under a single lock acquisition. Order inside
    // the critical section matters: append all nodes first so same-batch
    // producer indices are valid, then wire edges, and only then enqueue the
    // ready nodes. Enqueuing earlier would let a worker complete a same-batch
    // producer before its successor edge exists and lose the dependency.
    void flushDagStaging() {
        if (dag_staging_.empty()) {
            return;
        }

        const bool profiling = profile_enabled_.load(std::memory_order_relaxed);
        const std::size_t staged_count = dag_staging_.size();
        std::size_t ready_count = 0;
        bool has_priority_ready = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::size_t base = dag_nodes_.size();
            for (const StagedSubmit &entry : dag_staging_) {
                dag_nodes_.push_back(
                    {entry.fn, entry.context, 0, false, entry.priority, -1, -1, 0});
            }
            pending_dag_tasks_ += staged_count;
            if (profiling) {
                profile_dag_nodes_ += staged_count;
                max_dag_live_ = std::max(max_dag_live_, pending_dag_tasks_);
            }

            for (std::size_t s = 0; s < staged_count; ++s) {
                const StagedSubmit &entry = dag_staging_[s];
                const int successor_index = static_cast<int>(base + s);
                for (std::size_t i = 0; i < entry.dep_count; ++i) {
                    const int producer_index = entry.dep_nodes[i];
                    if (producer_index == kDepSkip) {
                        continue;
                    }
                    if (producer_index == kDepMissing) {
                        if (profiling) {
                            ++profile_dag_missing_deps_;
                            if (entry.dep_first_touch[i]) {
                                ++profile_dag_first_touch_deps_;
                            }
                        }
                        continue;
                    }

                    DagNode &producer_node =
                        dag_nodes_[static_cast<std::size_t>(producer_index)];
                    if (producer_node.completed) {
                        if (profiling) {
                            ++profile_dag_satisfied_deps_;
                        }
                        continue;
                    }

                    const int edge_index = static_cast<int>(successor_edges_.size());
                    successor_edges_.push_back({successor_index, -1});
                    if (producer_node.last_successor >= 0) {
                        successor_edges_[static_cast<std::size_t>(
                                             producer_node.last_successor)]
                            .next = edge_index;
                    } else {
                        producer_node.first_successor = edge_index;
                    }
                    producer_node.last_successor = edge_index;
                    ++producer_node.successor_count;
                    ++dag_nodes_[static_cast<std::size_t>(successor_index)].pending;
                    if (profiling) {
                        ++profile_dag_edges_;
                        max_dag_successors_ =
                            std::max(max_dag_successors_, producer_node.successor_count);
                    }
                }

                if (profiling) {
                    max_dag_pending_ = std::max(
                        max_dag_pending_,
                        static_cast<std::size_t>(
                            dag_nodes_[static_cast<std::size_t>(successor_index)]
                                .pending));
                }
            }

            for (std::size_t s = 0; s < staged_count; ++s) {
                const std::size_t node = base + s;
                if (dag_nodes_[node].pending != 0) {
                    continue;
                }
                const StagedSubmit &entry = dag_staging_[s];
                enqueueTaskLocked({entry.fn, entry.context,
                                   profiling ? nowNs() : 0, static_cast<int>(node),
                                   entry.priority});
                ++ready_count;
                has_priority_ready = has_priority_ready || entry.priority > 0;
                if (profiling) {
                    ++profile_dag_initial_ready_;
                }
            }
        }

        dag_next_node_index_ += staged_count;
        dag_staging_.clear();

        if (has_priority_ready) {
            work_cv_.notify_one();
        }
        notifyWorkers(ready_count);
    }

    void enqueueTask(Task task) {
        bool should_notify = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            enqueueTaskLocked(task);
            const std::size_t ready = readyTaskCountLocked();
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

    // Wake at most one worker per newly ready task instead of storming the
    // whole pool. notify_all costs O(participants) futex wakeups per release
    // event while at most `count` of the woken threads can find work; the rest
    // re-acquire the shared mutex only to re-check the predicate and park
    // again. Under-waking is safe: every participant re-evaluates
    // hasReadyTasksLocked() after finishing a batch, and submit-side flushes
    // notify independently.
    void notifyWorkers(std::size_t count) {
        if (count == 0) {
            return;
        }
        if (count >= workers_.size()) {
            work_cv_.notify_all();
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            work_cv_.notify_one();
        }
    }

    void wait() {
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
                if (hasReadyTasksLocked()) {
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
                    notifyWorkers(released);
                }
                if (!hasReadyTasksLocked() && active_tasks_ == 0) {
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

                    if (hasReadyTasksLocked()) {
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
                        notifyWorkers(released);
                    }
                    if (key_waiters_ > 0 ||
                        (!hasReadyTasksLocked() && active_tasks_ == 0)) {
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
        pinCurrentWorker(worker_index);
        while (true) {
            std::array<Task, kMaxTaskBatch> batch{};
            std::size_t batch_count = 0;
            const bool profiling = profile_enabled_.load(std::memory_order_relaxed);
            const std::uint64_t wait_start_ns = profiling ? nowNs() : 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                work_cv_.wait(lock, [this]() { return stopping_ || hasReadyTasksLocked(); });
                if (profiling) {
                    worker_idle_ns_ += nowNs() - wait_start_ns;
                }
                if (stopping_ && !hasReadyTasksLocked()) {
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
                    notifyWorkers(released);
                }
                if (key_waiters_ > 0 ||
                    (!hasReadyTasksLocked() && active_tasks_ == 0)) {
                    done_cv_.notify_all();
                }
            }
        }
    }

    void flushPendingTasks() {
        flushDagStaging();
        if (pending_count_ == 0) {
            return;
        }

        const std::size_t flushed = pending_count_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.insert(tasks_.end(), pending_tasks_.begin(),
                          pending_tasks_.begin() + pending_count_);
            recordReadyWidthLocked(readyTaskCountLocked());
            if (profile_enabled_.load(std::memory_order_relaxed)) {
                ++submit_flushes_;
            }
        }
        pending_count_ = 0;
        notifyWorkers(flushed);
    }

    std::size_t normalReadyCountLocked() const {
        return tasks_.size() - task_head_;
    }

    std::size_t priorityReadyCountLocked() const {
        std::size_t ready = 0;
        for (std::size_t i = 0; i < priority_tasks_.size(); ++i) {
            ready += priority_tasks_[i].size() - priority_task_heads_[i];
        }
        return ready;
    }

    std::size_t readyTaskCountLocked() const {
        const std::size_t normal_ready = normalReadyCountLocked();
        if (!critical_priority_enabled_) {
            return normal_ready;
        }
        return normal_ready + priorityReadyCountLocked();
    }

    bool hasReadyTasksLocked() const {
        // Preserve the original normal-queue check as the default hot path.
        if (task_head_ < tasks_.size()) {
            return true;
        }
        return critical_priority_enabled_ && priorityReadyCountLocked() != 0;
    }

    std::size_t takeReadyTasksLocked(Task *batch) {
        if (!critical_priority_enabled_) {
            return takeNormalReadyTasksLocked(batch);
        }

        int priority_level = 0;
        for (int level = 3; level >= 1; --level) {
            const std::size_t index = static_cast<std::size_t>(level - 1);
            if (priority_task_heads_[index] < priority_tasks_[index].size()) {
                priority_level = level;
                break;
            }
        }
        const bool take_priority = priority_level != 0;
        std::vector<Task> &queue =
            take_priority
                ? priority_tasks_[static_cast<std::size_t>(priority_level - 1)]
                : tasks_;
        std::size_t &head =
            take_priority
                ? priority_task_heads_[static_cast<std::size_t>(priority_level - 1)]
                : task_head_;
        const std::size_t available = queue.size() - head;
        // Rank 2/3 form the diagonal chain and release successors immediately.
        // Rank 1 is a wider frontier, so it keeps adaptive batching to avoid
        // recreating one global-lock operation per trsm/update task.
        const std::size_t count =
            priority_level >= 2 ? 1 : chooseBatchCount(available);
        for (std::size_t i = 0; i < count; ++i) {
            batch[i] = queue[head + i];
        }
        head += count;
        active_tasks_ += count;
        if (profile_enabled_.load(std::memory_order_relaxed)) {
            ++dequeue_batches_;
            if (take_priority) {
                ++priority_dequeue_batches_;
            }
            max_dequeue_batch_ = std::max(max_dequeue_batch_, count);
        }
        if (head == queue.size()) {
            queue.clear();
            head = 0;
        }
        return count;
    }

    std::size_t takeNormalReadyTasksLocked(Task *batch) {
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
        if (!critical_priority_enabled_) {
            tasks_.push_back(task);
            recordReadyWidthLocked(normalReadyCountLocked());
            return;
        }
        if (task.priority > 0) {
            const std::size_t index = static_cast<std::size_t>(
                task.priority - 1);
            priority_tasks_[index].push_back(task);
        } else {
            tasks_.push_back(task);
        }
        recordReadyWidthLocked(readyTaskCountLocked());
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
                                       edge.successor, successor.priority});
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
        // Node indices restart with the cleared vector. The submitter is the
        // caller here and always flushes staging before waiting, so there is no
        // staged entry whose predicted index would be invalidated.
        dag_next_node_index_ = 0;
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
                if (pending_dag_tasks_ <= max_live_window_ || !hasReadyTasksLocked()) {
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
                    notifyWorkers(released);
                }
                if (key_waiters_ > 0 ||
                    (!hasReadyTasksLocked() && active_tasks_ == 0)) {
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
        if (critical_priority_enabled_) {
            max_priority_ready_ =
                std::max(max_priority_ready_, priorityReadyCountLocked());
        }
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
        const std::size_t ready = readyTaskCountLocked();
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
            if (batch[i].priority > 0) {
                ++profile_priority_tasks_;
            }
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
        profile_priority_tasks_ = 0;
        priority_dequeue_batches_ = 0;
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
        max_priority_ready_ = 0;
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
                     "ready_samples=%llu ready_sum=%llu priority_enabled=%d "
                     "priority_tasks=%llu priority_dequeue_batches=%llu "
                     "max_priority_ready=%zu "
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
                     critical_priority_enabled_ ? 1 : 0,
                     static_cast<unsigned long long>(profile_priority_tasks_),
                     static_cast<unsigned long long>(priority_dequeue_batches_),
                     max_priority_ready_,
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
    std::array<std::vector<Task>, 3> priority_tasks_;
    std::vector<DagNode> dag_nodes_;
    std::vector<DagEdge> successor_edges_;
    std::unordered_map<int, int> latest_producer_;
    std::unordered_set<int> profile_output_keys_;
    std::size_t pending_dag_tasks_ = 0;
    std::array<Task, kMaxTaskBatch> pending_tasks_{};
    std::size_t task_head_ = 0;
    std::array<std::size_t, 3> priority_task_heads_{};
    std::size_t pending_count_ = 0;
    std::size_t task_batch_size_ = 1;
    // Submitter-private staging for dependency-aware submits. Only the thread
    // running the Pass-generated submit sequence touches these.
    std::vector<StagedSubmit> dag_staging_;
    std::size_t dag_staging_limit_ = 1;
    std::size_t dag_next_node_index_ = 0;
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
    std::uint64_t profile_priority_tasks_ = 0;
    std::uint64_t priority_dequeue_batches_ = 0;
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
    std::size_t max_priority_ready_ = 0;
    std::size_t max_dag_release_batch_ = 0;
    std::size_t max_dag_pending_ = 0;
    std::size_t max_dag_successors_ = 0;
    std::size_t max_dag_live_ = 0;
    std::size_t max_wait_ready_ = 0;
    std::size_t max_wait_active_ = 0;
    std::size_t max_wait_dag_live_ = 0;
    std::array<TaskProfile, kMaxProfiledTasks> task_profiles_{};
    std::size_t task_profile_count_ = 0;
    bool critical_priority_enabled_ = false;
    bool pin_workers_ = false;
    std::vector<int> worker_cpus_;
};

thread_local std::unique_ptr<AsyncRuntime> runtime;

// Sustainable participant count for a b x b tile task, independent of how much
// parallelism the DAG exposes. Adding participants past this point makes the
// shared ready queue the bottleneck and actively loses throughput.
//
// Measured per-case optima on a 40-physical-core host, sweeping thread counts
// with the cap disabled (best of 3 per point):
//   b=18 B=64 -> 8 threads 4.20x, 40 threads 1.62x  (-61%)
//   b=24 B=48 -> 16 threads 8.08x, 40 threads 3.57x (-56%)
//   b=32 B=36 -> 16 threads 9.73x, 40 threads 6.48x (-33%)
//   b=32 B=64 -> 24 threads 15.70x, 40 threads 9.90x (-37%)
//   b=64 B=16 and b=96 B=8 already sit under the block-count cap, -1%.
// The optimum tracks b roughly linearly over that range, which `b - 8`
// reproduces: 18->10, 24->16, 32->24, and no cap from b=48 up.
//
// PLATFORM CAVEAT: the constant encodes this host's lock throughput relative to
// tile task duration. The aarch64 target has narrower vector units, so the same
// b yields a longer task and a genuinely higher sustainable count; this default
// therefore under-provisions there, costing throughput but never correctness.
// Re-measure with COMPILER2026_DAG_PARTICIPANT_CAP=off before trusting it as a
// tuned value on a new platform.
std::size_t participantCapForTile(int b) {
    if (const char *env = std::getenv("COMPILER2026_DAG_PARTICIPANT_CAP")) {
        if (std::strcmp(env, "off") == 0 || std::strcmp(env, "none") == 0) {
            return std::numeric_limits<std::size_t>::max();
        }
        char *end = nullptr;
        const unsigned long configured = std::strtoul(env, &end, 10);
        if (end != env && *end == '\0' && configured > 0) {
            return static_cast<std::size_t>(configured);
        }
    }
    if (b <= 12) {
        return 4;
    }
    return static_cast<std::size_t>(b - 8);
}

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
    // Three interacting bounds, all fitted to the 11 measured per-case optima
    // rather than to a single point:
    //   * block count: the DAG never exposes more than that many panels.
    //   * tile granularity: participantCapForTile(b), the shared queue's
    //     sustainable participant count for a b x b task.
    //   * average panel width: the panel index sweeps from block_count down to
    //     1, so participants beyond about half the blocks sit idle for the
    //     second half of the run. This is what separates b=32 B=32 (optimum 16)
    //     from b=32 B=64 (optimum 24) -- the granularity bound alone predicts 24
    //     for both and loses 14% on the former.
    //   * a floor of min(block_count, 16) on the WIDTH bound only: for few
    //     blocks the early panels still hold block_count*(block_count-1)/2
    //     tasks, so half-width is too pessimistic there (b=64 B=16 wants 16, not
    //     8). The floor must not lift the granularity bound: doing so gave
    //     b=16 B=72 sixteen participants where 8 is optimal (3.76x vs 2.60x) and
    //     cost 7% aggregate.
    const std::size_t blocks = static_cast<std::size_t>(block_count);
    const std::size_t width = std::max(std::min<std::size_t>(blocks, 16),
                                       std::max<std::size_t>(1, blocks / 2));
    const std::size_t shaped = std::min(participantCapForTile(b), width);
    return std::max<std::size_t>(1, std::min(std::min<std::size_t>(threads, blocks), shaped));
}

int asyncMinBlockSize() {
    // Measured crossover, not a guess. Two rounds of evidence on a
    // 40-physical-core host:
    //   * Aggregate public-suite geomean at 40 available cores, participant cap
    //     on: 2.284x at 16, 2.337x at 12, 2.318x at 10.
    //   * Single cases at their own optimum participant count: b=16 reaches
    //     3.76x-4.04x, b=12 reaches 1.65x, while b=9 (0.75x) and b=8 (0.54x)
    //     regress because per-task overhead exceeds the 2*b^3 flops a madd
    //     carries. The crossover therefore sits between 9 and 12.
    // 12 is also the conservative direction for the aarch64 target: its
    // narrower vector units make each tile task longer, so a threshold that
    // pays off where tasks are shortest also pays off there.
    int threshold = 12;
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
    if (b <= 64) {
        batch = 16;
    } else if (b <= 128) {
        batch = 8;
    }

    // Batch size is purely a task-granularity decision: small tiles make the
    // per-task queue round trip expensive relative to the work they carry,
    // large tiles do not. Fairness is enforced dynamically by
    // chooseBatchCount(), which never hands a participant more than
    // available / participants and returns 1 whenever the ready width is thin.
    // The static block-count clamps that used to shrink this value were a
    // stand-in for the same guard, and they mis-fire once the participant count
    // approaches the block count: at blocks=64 with 40 participants they forced
    // batch 1 while the measured ready width was 283, maximizing lock traffic
    // exactly where it hurts most.
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

extern "C" void compiler2026_runtime_submit_deps3_priority(
    TaskFn fn, void *context, int dep_a, int dep_b, int dep_c, int output,
    int priority) {
    const int deps[] = {dep_a, dep_b, dep_c};
    activeRuntime().submitWithDeps(fn, context, deps, 3, output, priority);
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
