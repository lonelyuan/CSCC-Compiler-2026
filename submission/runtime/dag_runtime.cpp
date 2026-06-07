#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

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
        std::vector<int> successors;
    };

public:
    explicit AsyncRuntime(std::size_t worker_count) {
        if (worker_count == 0) {
            return;
        }
        workers_.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this]() { workerLoop(); });
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
                      int n = 0, int b = 0, std::size_t total_threads = 1) {
        wait();
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
        if (reserve_tasks > latest_producer_.bucket_count()) {
            latest_producer_.reserve(reserve_tasks);
        }
        tasks_.clear();
        dag_nodes_.clear();
        latest_producer_.clear();
        pending_dag_tasks_ = 0;
        task_head_ = 0;
        chunk_offset_ = 0;
        pending_count_ = 0;
        task_batch_size_ = std::max<std::size_t>(
            1, std::min<std::size_t>(task_batch_size, kMaxTaskBatch));
        worker_error_ = nullptr;
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
                        int output) {
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
            dag_nodes_.push_back({fn, context, 0, false, {}});
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
                    }
                    continue;
                }

                DagNode &producer_node = dag_nodes_[producer->second];
                if (!producer_node.completed) {
                    producer_node.successors.push_back(node_index);
                    ++dag_nodes_[node_index].pending;
                    if (profiling) {
                        ++profile_dag_edges_;
                        max_dag_successors_ =
                            std::max(max_dag_successors_, producer_node.successors.size());
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

private:
    void workerLoop() {
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
                if (tasks_.empty() && active_tasks_ == 0) {
                    done_cv_.notify_all();
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

            for (int successor_index : node.successors) {
                DagNode &successor = dag_nodes_[static_cast<std::size_t>(successor_index)];
                --successor.pending;
                if (successor.pending == 0) {
                    enqueueTaskLocked({successor.fn, successor.context,
                                       profile_enabled_.load(std::memory_order_relaxed) ? nowNs() : 0,
                                       successor_index});
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
        latest_producer_.clear();
        pending_dag_tasks_ = 0;
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
                     "dag_missing_deps=%llu dag_initial_ready=%llu "
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
    std::unordered_map<int, int> latest_producer_;
    std::size_t pending_dag_tasks_ = 0;
    std::array<Task, kMaxTaskBatch> pending_tasks_{};
    std::size_t task_head_ = 0;
    std::size_t pending_count_ = 0;
    std::size_t task_batch_size_ = 1;
    std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable done_cv_;
    std::vector<std::unique_ptr<unsigned char[]>> chunks_;
    std::size_t chunk_size_ = 0;
    std::size_t chunk_offset_ = 0;
    std::size_t active_tasks_ = 0;
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
    if (!runtime || runtime->workerCount() != worker_threads) {
        runtime = std::make_unique<AsyncRuntime>(worker_threads);
        runtime->resetForCall(reserve_tasks, task_batch_size, n, b, total_threads);
        return;
    }
    runtime->resetForCall(reserve_tasks, task_batch_size, n, b, total_threads);
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

extern "C" void compiler2026_runtime_end() {
    if (runtime) {
        runtime->finishCall();
    }
}
