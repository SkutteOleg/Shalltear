#pragma once

#include "Shalltear/Core/CoreAPI.h"
#include "Shalltear/Core/Jobs/Job.h"
#include "Shalltear/Core/Types.h"

#include <initializer_list>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace Shalltear {

	enum class JobType : u8 {
		Normal,
		LongRunning
	};

	class SHALLTEAR_CORE_API JobSystem {
	public:
		static void Initialize();
		static void Terminate();

		template <typename F>
		static JobHandle Submit(F&& func, JobType type = JobType::Normal) {
			return Submit(std::forward<F>(func), std::span<const JobHandle>(), type);
		}

		template <typename F>
		static JobHandle Submit(F&& func, JobHandle dependency, JobType type = JobType::Normal) {
			if (dependency.IsValid()) {
				JobHandle deps[1] = {dependency};
				return Submit(std::forward<F>(func), std::span<const JobHandle>(deps), type);
			} else {
				return Submit(std::forward<F>(func), std::span<const JobHandle>(), type);
			}
		}

		template <typename F>
		static JobHandle Submit(F&& func, std::span<const JobHandle> dependencies, JobType type = JobType::Normal) {
			static_assert(sizeof(std::decay_t<F>) <= 64, "Job lambda payload too large! Limit is 64 bytes.");

			auto wrapper = [](void* data) {
				using T = std::decay_t<F>;
				T* ptr = static_cast<T*>(data);
				(*ptr)();
				ptr->~T();
			};

			auto construct = [](void* dest, void* src) {
				using T = std::decay_t<F>;
				using OriginalF = std::remove_reference_t<F>;
				new (dest) T(std::forward<F>(*static_cast<OriginalF*>(src)));
			};

			return SubmitPayload(wrapper, construct, &func, dependencies, type);
		}

		template <typename F>
		static JobHandle Submit(F&& func, std::initializer_list<JobHandle> dependencies, JobType type = JobType::Normal) {
			return Submit(std::forward<F>(func), std::span<const JobHandle>(dependencies.begin(), dependencies.size()), type);
		}

		static void WaitFor(JobHandle handle);

		static void WaitForAll(std::span<const JobHandle> handles);
		static void WaitForAll(std::initializer_list<JobHandle> handles) {
			WaitForAll(std::span<const JobHandle>(handles.begin(), handles.size()));
		}

		static u32 GetWorkerCount();
		static bool SupportsBackgroundThreads();

		static constexpr u32 MAX_LANES = 64;

		// up to 4 per thread, each >= minPerLane items (1 without workers)
		static u32 LaneCount(u32 count, u32 minPerLane);

		// lane 0 on caller thread, wait limited to own lanes
		template <typename Fn>
		static void ParallelFor(u32 lanes, u32 count, const Fn& fn) {
			RunLanes(lanes, count, [](const void* body, u32 lane, u32 first, u32 end) { (*static_cast<const Fn*>(body))(lane, first, end); }, &fn);
		}
	private:
		using JobConstructFunc = void (*)(void* dest, void* src);
		static JobHandle SubmitPayload(void (*func)(void*), JobConstructFunc construct, void* srcPayload, std::span<const JobHandle> dependencies, JobType type);

		using LaneFunc = void (*)(const void* body, u32 lane, u32 first, u32 end);
		static void RunLanes(u32 lanes, u32 count, LaneFunc call, const void* body);
	};

} // namespace Shalltear
