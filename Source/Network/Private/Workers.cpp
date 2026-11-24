#include "NetworkInternal.h"

#include "Shalltear/Core/Jobs/JobSystem.h"

#include <condition_variable>
#include <deque>
#include <thread>

namespace Shalltear::Net::Workers {

	namespace {

		struct PoolState {
			explicit PoolState(u32 limit) : maxThreads(limit) {}

			void Run(std::function<void()> job) {
				if (!JobSystem::SupportsBackgroundThreads()) {
					if (job) {
						job();
					}
					return;
				}
				std::lock_guard<std::mutex> guard(lock);
				queue.push_back(std::move(job));
				if (idle == 0 && threads.size() < maxThreads) {
					threads.emplace_back([this] { Loop(); });
				} else {
					wake.notify_one();
				}
			}

			void Loop() {
				std::unique_lock<std::mutex> guard(lock);
				for (;;) {
					++idle;
					wake.wait(guard, [this] { return stopping || !queue.empty(); });
					--idle;
					if (stopping) {
						return;
					}
					std::function<void()> job = std::move(queue.front());
					queue.pop_front();
					guard.unlock();
					job();
					guard.lock();
				}
			}

			void Stop() {
				std::vector<std::thread> joining;
				{
					std::lock_guard<std::mutex> guard(lock);
					stopping = true;
					queue.clear();
					joining.swap(threads);
				}
				wake.notify_all();
				for (std::thread& thread : joining) {
					if (thread.joinable()) {
						thread.join();
					}
				}
				std::lock_guard<std::mutex> guard(lock);
				stopping = false;
			}

			const u32 maxThreads;
			std::mutex lock;
			std::condition_variable wake;
			std::deque<std::function<void()>> queue;
			std::vector<std::thread> threads;
			u32 idle = 0;
			bool stopping = false;
		};

		struct Pools {
			PoolState resolve{2};
			PoolState http{6};

			// HTTP state must outlive pools
			Pools() { HTTPInternal::Touch(); }

			~Pools() {
				HTTPInternal::CancelAll();
				HTTPInternal::StopWatchdog();
				http.Stop();
				resolve.Stop();
			}
		};

		PoolState& PoolOf(Pool pool) {
			static Pools pools;
			return pool == Pool::Resolve ? pools.resolve : pools.http;
		}

	} // anonymous namespace

	void Run(Pool pool, std::function<void()> job) {
		PoolOf(pool).Run(std::move(job));
	}

	void Shutdown() {
		HTTPInternal::CancelAll();
		HTTPInternal::StopWatchdog();
		PoolOf(Pool::HTTP).Stop();
		PoolOf(Pool::Resolve).Stop();
	}

} // namespace Shalltear::Net::Workers
