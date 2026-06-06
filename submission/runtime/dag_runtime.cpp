#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <deque>
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
        if (worker_count <= 1) {
            return;
        }
        workers_.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
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

    void submit(TaskFn fn, void *context) {
        if (workers_.empty()) {
            fn(context);
            std::free(context);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back({fn, context});
        }
        work_cv_.notify_one();
    }

    void wait() {
        if (workers_.empty()) {
            return;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        done_cv_.wait(lock, [this]() {
            return tasks_.empty() && active_tasks_ == 0;
        });

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
                work_cv_.wait(lock, [this]() { return stopping_ || !tasks_.empty(); });
                if (stopping_ && tasks_.empty()) {
                    return;
                }
                task = tasks_.front();
                tasks_.pop_front();
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
            std::free(task.context);

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
    std::deque<Task> tasks_;
    std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable done_cv_;
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
    int threshold = 64;
    if (const char *env = std::getenv("COMPILER2026_ASYNC_MIN_B")) {
        char *end = nullptr;
        const long configured = std::strtol(env, &end, 10);
        if (end != env && *end == '\0' && configured > 0) {
            threshold = static_cast<int>(configured);
        }
    }
    return threshold;
}

AsyncRuntime &activeRuntime() {
    if (!runtime) {
        runtime = std::make_unique<AsyncRuntime>(std::thread::hardware_concurrency());
    }
    return *runtime;
}

}  // namespace

extern "C" void compiler2026_runtime_begin(int n, int b) {
    const std::size_t threads = (b < asyncMinBlockSize()) ? 1 : resolveThreadCount(n, b);
    runtime = std::make_unique<AsyncRuntime>(threads);
}

extern "C" void *compiler2026_runtime_alloc(std::size_t size) {
    void *memory = std::malloc(size);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return memory;
}

extern "C" void compiler2026_runtime_submit(TaskFn fn, void *context) {
    activeRuntime().submit(fn, context);
}

extern "C" void compiler2026_runtime_wait() {
    activeRuntime().wait();
}

extern "C" void compiler2026_runtime_end() {
    if (runtime) {
        runtime->wait();
        runtime.reset();
    }
}
