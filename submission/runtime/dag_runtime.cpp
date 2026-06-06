#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

namespace {

using TaskFn = void (*)(void *);

class AsyncRuntime {
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

    void resetForCall(std::size_t reserve_tasks = 0) {
        wait();
        if (reserve_tasks > tasks_.capacity()) {
            tasks_.reserve(reserve_tasks);
        }
        tasks_.clear();
        task_head_ = 0;
        chunk_offset_ = 0;
        worker_error_ = nullptr;
    }

    void submit(TaskFn fn, void *context) {
        if (workers_.empty()) {
            fn(context);
            return;
        }

        bool should_notify = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back({fn, context});
            should_notify = (tasks_.size() - task_head_) <= workers_.size();
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
        if (workers_.empty()) {
            return;
        }

        while (true) {
            Task task{};
            bool has_task = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (task_head_ < tasks_.size()) {
                    task = tasks_[task_head_++];
                    if (task_head_ == tasks_.size()) {
                        tasks_.clear();
                        task_head_ = 0;
                    }
                    ++active_tasks_;
                    has_task = true;
                } else if (active_tasks_ == 0) {
                    break;
                } else {
                    done_cv_.wait(lock);
                    continue;
                }
            }

            if (has_task) {
                try {
                    task.fn(task.context);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!worker_error_) {
                        worker_error_ = std::current_exception();
                    }
                }

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    --active_tasks_;
                    if (tasks_.empty() && active_tasks_ == 0) {
                        done_cv_.notify_all();
                    }
                }
            }
        }

        if (worker_error_) {
            std::exception_ptr error = worker_error_;
            worker_error_ = nullptr;
            std::rethrow_exception(error);
        }
    }

private:
    struct Task {
        TaskFn fn;
        void *context;
    };

    void workerLoop() {
        while (true) {
            Task task{};
            {
                std::unique_lock<std::mutex> lock(mutex_);
                work_cv_.wait(lock, [this]() { return stopping_ || task_head_ < tasks_.size(); });
                if (stopping_ && task_head_ == tasks_.size()) {
                    return;
                }
                task = tasks_[task_head_++];
                if (task_head_ == tasks_.size()) {
                    tasks_.clear();
                    task_head_ = 0;
                }
                ++active_tasks_;
            }

            try {
                task.fn(task.context);
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!worker_error_) {
                    worker_error_ = std::current_exception();
                }
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                --active_tasks_;
                if (tasks_.empty() && active_tasks_ == 0) {
                    done_cv_.notify_all();
                }
            }
        }
    }

    std::vector<std::thread> workers_;
    std::vector<Task> tasks_;
    std::size_t task_head_ = 0;
    std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable done_cv_;
    std::vector<std::unique_ptr<unsigned char[]>> chunks_;
    std::size_t chunk_size_ = 0;
    std::size_t chunk_offset_ = 0;
    std::size_t active_tasks_ = 0;
    bool stopping_ = false;
    std::exception_ptr worker_error_;
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
    int threshold = 32;
    if (const char *env = std::getenv("COMPILER2026_ASYNC_MIN_B")) {
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
    return trailing * (trailing + 1) / 2;
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
    if (!runtime || runtime->workerCount() != worker_threads) {
        runtime = std::make_unique<AsyncRuntime>(worker_threads);
        runtime->resetForCall(reserve_tasks);
        return;
    }
    runtime->resetForCall(reserve_tasks);
}

extern "C" void *compiler2026_runtime_alloc(std::size_t size) {
    return activeRuntime().allocate(size);
}

extern "C" void compiler2026_runtime_submit(TaskFn fn, void *context) {
    activeRuntime().submit(fn, context);
}

extern "C" void compiler2026_runtime_wait() {
    activeRuntime().wait();
}

extern "C" void compiler2026_runtime_end() {
    if (runtime) {
        runtime->resetForCall();
    }
}
