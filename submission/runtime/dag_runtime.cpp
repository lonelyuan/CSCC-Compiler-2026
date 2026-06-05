#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
void cholesky(double *A, double *L, int b, int lda);
void trsm(double *A, double *L, double *X, int b, int lda);
void madd(double *A, double *B, double *C, int b, int lda);
}

namespace {

class WorkerTeam {
public:
    explicit WorkerTeam(std::size_t thread_count) : thread_count_(std::max<std::size_t>(1, thread_count)) {
        if (thread_count_ == 1) {
            return;
        }
        workers_.reserve(thread_count_);
        for (std::size_t i = 0; i < thread_count_; ++i) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
    }

    ~WorkerTeam() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        work_cv_.notify_all();
        for (auto &worker : workers_) {
            worker.join();
        }
    }

    void run(std::size_t count, std::function<void(std::size_t)> task) {
        if (count == 0) {
            return;
        }
        if (thread_count_ == 1 || count == 1) {
            for (std::size_t i = 0; i < count; ++i) {
                task(i);
            }
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            task_ = std::move(task);
            total_ = count;
            next_.store(0, std::memory_order_relaxed);
            remaining_workers_.store(thread_count_, std::memory_order_relaxed);
            worker_error_ = nullptr;
            ++generation_;
        }

        work_cv_.notify_all();

        std::unique_lock<std::mutex> lock(mutex_);
        const std::size_t target_generation = generation_;
        done_cv_.wait(lock, [this, target_generation]() {
            return finished_generation_ >= target_generation;
        });
        if (worker_error_) {
            std::rethrow_exception(worker_error_);
        }
    }

private:
    void workerLoop() {
        std::size_t observed_generation = 0;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                work_cv_.wait(lock, [this, observed_generation]() {
                    return stopping_ || generation_ != observed_generation;
                });
                if (stopping_) {
                    return;
                }
                observed_generation = generation_;
            }

            try {
                while (true) {
                    const std::size_t index = next_.fetch_add(1, std::memory_order_relaxed);
                    if (index >= total_) {
                        break;
                    }
                    task_(index);
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!worker_error_) {
                    worker_error_ = std::current_exception();
                }
                next_.store(total_, std::memory_order_relaxed);
            }

            if (remaining_workers_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    finished_generation_ = observed_generation;
                }
                done_cv_.notify_one();
            }
        }
    }

    std::size_t thread_count_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable done_cv_;
    std::function<void(std::size_t)> task_;
    std::atomic<std::size_t> next_{0};
    std::atomic<std::size_t> remaining_workers_{0};
    std::size_t total_ = 0;
    std::size_t generation_ = 0;
    std::size_t finished_generation_ = 0;
    bool stopping_ = false;
    std::exception_ptr worker_error_;
};

std::size_t resolveThreadCount(int block_count) {
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

    if (block_count <= 1) {
        return 1;
    }
    return std::max<std::size_t>(1, threads);
}

inline double *blockPtr(double *matrix, int n, int b, int block_row, int block_col) {
    return &matrix[static_cast<std::size_t>(block_row * b) * n + block_col * b];
}

void clearUpperTriangle(double *L, int n, WorkerTeam &team) {
    team.run(static_cast<std::size_t>(n), [&](std::size_t row_index) {
        const int row = static_cast<int>(row_index);
        for (int col = row + 1; col < n; ++col) {
            L[static_cast<std::size_t>(row) * n + col] = 0.0;
        }
    });
}

int runBlockCholeskyDag(const double *A, double *L, int n, int b) {
    if (A == nullptr || L == nullptr) {
        throw std::invalid_argument("A and L must be non-null");
    }
    if (n <= 0 || b <= 0) {
        throw std::invalid_argument("n and b must be positive");
    }
    if (n % b != 0) {
        throw std::invalid_argument("Current runtime requires n to be divisible by b");
    }

    std::copy(A, A + static_cast<std::size_t>(n) * n, L);

    const int block_count = n / b;
    WorkerTeam team(resolveThreadCount(block_count));

    for (int panel = 0; panel < block_count; ++panel) {
        cholesky(blockPtr(L, n, b, panel, panel), blockPtr(L, n, b, panel, panel), b, n);

        const int trailing = block_count - panel - 1;
        team.run(static_cast<std::size_t>(trailing), [&](std::size_t task_index) {
            const int row_block = panel + 1 + static_cast<int>(task_index);
            trsm(blockPtr(L, n, b, row_block, panel),
                 blockPtr(L, n, b, panel, panel),
                 blockPtr(L, n, b, row_block, panel),
                 b, n);
        });

        std::vector<std::pair<int, int>> updates;
        updates.reserve(static_cast<std::size_t>(trailing) * (trailing + 1) / 2);
        for (int col_block = panel + 1; col_block < block_count; ++col_block) {
            for (int row_block = col_block; row_block < block_count; ++row_block) {
                updates.emplace_back(row_block, col_block);
            }
        }

        team.run(updates.size(), [&](std::size_t task_index) {
            const auto [row_block, col_block] = updates[task_index];
            madd(blockPtr(L, n, b, row_block, panel),
                 blockPtr(L, n, b, col_block, panel),
                 blockPtr(L, n, b, row_block, col_block),
                 b, n);
        });
    }

    clearUpperTriangle(L, n, team);
    return 0;
}

}  // namespace

extern "C" int compiler2026_block_cholesky_runtime(const double *A, double *L, int n, int b) {
    try {
        return runBlockCholeskyDag(A, L, n, b);
    } catch (...) {
        return -1;
    }
}
