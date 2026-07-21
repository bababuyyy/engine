#pragma once
#include <thread>
#include <atomic>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <cstdint>
#include <functional>
#include <algorithm>

namespace core {

    class JobSystem {
    private:
        std::vector<std::thread> workers;
        std::mutex mutex;
        std::condition_variable cv_wake;
        std::condition_variable cv_done;

        std::function<void(size_t, size_t)> current_job;
        size_t total_items = 0;
        size_t chunk_size = 0;
        int target_threads = 0;

        std::atomic<size_t> next_item{ 0 };
        std::atomic<size_t> items_done{ 0 };
        bool quit = false;
        bool job_active = false;

        void worker_loop(int worker_id) {
            while (true) {
                std::unique_lock<std::mutex> lock(mutex);
                cv_wake.wait(lock, [this, worker_id] {
                    return quit || (job_active && worker_id < target_threads - 1);
                    });

                if (quit && !job_active) break;
                lock.unlock();

                while (true) {
                    // CORREÇÃO: memory_order_acquire garante que a atribuição de 'current_job'
                    // (feita com memory_order_release) seja 100% visível para esta thread antes de avançar.
                    size_t begin = next_item.fetch_add(chunk_size, std::memory_order_acquire);
                    if (begin >= total_items) break;

                    size_t end = std::min(begin + chunk_size, total_items);

                    current_job(begin, end);

                    size_t prev = items_done.fetch_add(end - begin, std::memory_order_acq_rel);
                    if (prev + (end - begin) == total_items) {
                        std::lock_guard<std::mutex> lk(mutex);
                        cv_done.notify_one();
                    }
                }
            }
        }

    public:
        JobSystem() {
            int num_cores = (int)std::thread::hardware_concurrency();
            if (num_cores < 1) num_cores = 1;

            for (int i = 0; i < num_cores - 1; ++i) {
                workers.emplace_back(&JobSystem::worker_loop, this, i);
            }
        }

        ~JobSystem() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                quit = true;
            }
            cv_wake.notify_all();
            for (auto& t : workers) {
                if (t.joinable()) t.join();
            }
        }

        template<typename F>
        void parallel_for(size_t total, size_t chunk, int num_threads, F&& fn) {
            if (total == 0) return;

            if (num_threads <= 1) {
                for (size_t i = 0; i < total; i += chunk) {
                    fn(i, std::min(i + chunk, total));
                }
                return;
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                current_job = std::forward<F>(fn);
                total_items = total;
                chunk_size = chunk;
                target_threads = num_threads;

                // CORREÇÃO: memory_order_release atua como barreira. 
                // Tudo o que foi escrito antes (como o state da std::function current_job) 
                // fica visível globalmente antes do contador ser resetado.
                next_item.store(0, std::memory_order_release);
                items_done.store(0, std::memory_order_relaxed);
                job_active = true;
            }
            cv_wake.notify_all();

            while (true) {
                // CORREÇÃO: memory_order_acquire pelo mesmo motivo do worker_loop.
                size_t begin = next_item.fetch_add(chunk_size, std::memory_order_acquire);
                if (begin >= total_items) break;

                size_t end = std::min(begin + chunk_size, total_items);
                current_job(begin, end);

                size_t prev = items_done.fetch_add(end - begin, std::memory_order_acq_rel);
                if (prev + (end - begin) == total_items) {
                    std::lock_guard<std::mutex> lk(mutex);
                    cv_done.notify_one();
                }
            }

            std::unique_lock<std::mutex> lock(mutex);
            cv_done.wait(lock, [this] { return items_done.load(std::memory_order_acquire) == total_items; });
            job_active = false;
        }
    };
}