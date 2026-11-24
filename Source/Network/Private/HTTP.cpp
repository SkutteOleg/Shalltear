#include "Shalltear/Network/HTTP.h"

#include "HTTPBackend.h"
#include "NetworkInternal.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>
#include <unordered_map>

namespace Shalltear::Net::HTTP {

	namespace {

		struct Transfer {
			RequestId id = 0;
			Request request;
			DoneCallback onDone;
			ProgressCallback onProgress;
			std::shared_ptr<HTTPBackend::Control> control = std::make_shared<HTTPBackend::Control>();
			std::atomic<u64> received{0};
			std::atomic<u64> total{0};
			std::atomic<bool> progressQueued{false};
			std::atomic<bool> wantsProgress{false};
		};

		void Abort(Transfer& transfer, bool timedOut) {
			transfer.control->timedOut = transfer.control->timedOut || timedOut;
			transfer.control->cancelled = true;
			std::lock_guard<std::mutex> guard(transfer.control->abortLock);
			if (transfer.control->abort) {
				transfer.control->abort();
				transfer.control->abort = nullptr;
			}
		}

		struct Watchdog {
			using Clock = std::chrono::steady_clock;

			void Add(Clock::time_point deadline, const std::shared_ptr<Transfer>& transfer) {
				std::lock_guard<std::mutex> guard(lock);
				deadlines.emplace(deadline, transfer);
				if (!thread.joinable()) {
					thread = std::thread([this] { Loop(); });
				}
				wake.notify_one();
			}

			void Loop() {
				std::unique_lock<std::mutex> guard(lock);
				while (!stopping) {
					if (deadlines.empty()) {
						wake.wait(guard);
						continue;
					}
					const auto first = deadlines.begin();
					if (Clock::now() < first->first) {
						wake.wait_until(guard, first->first);
						continue;
					}
					std::shared_ptr<Transfer> transfer = first->second.lock();
					deadlines.erase(first);
					guard.unlock();
					if (transfer) {
						Abort(*transfer, true);
					}
					guard.lock();
				}
			}

			void Stop() {
				std::thread joining;
				{
					std::lock_guard<std::mutex> guard(lock);
					stopping = true;
					joining.swap(thread);
				}
				wake.notify_all();
				if (joining.joinable()) {
					joining.join();
				}
				std::lock_guard<std::mutex> guard(lock);
				stopping = false;
				deadlines.clear();
			}

			std::mutex lock;
			std::condition_variable wake;
			std::multimap<Clock::time_point, std::weak_ptr<Transfer>> deadlines;
			std::thread thread;
			bool stopping = false;
		};

		struct HTTPState {
			std::mutex lock;
			std::unordered_map<RequestId, std::shared_ptr<Transfer>> active;
			RequestId nextId = 1;
			Watchdog watchdog;
		};

		HTTPState& State() {
			static HTTPState state;
			return state;
		}

		char Lower(char c) {
			return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
		}

		bool SameName(std::string_view a, std::string_view b) {
			if (a.size() != b.size()) {
				return false;
			}
			for (size_t i = 0; i < a.size(); ++i) {
				if (Lower(a[i]) != Lower(b[i])) {
					return false;
				}
			}
			return true;
		}

		void QueueProgress(const std::shared_ptr<Transfer>& transfer) {
			if (!transfer->wantsProgress.load() || transfer->progressQueued.exchange(true)) {
				return;
			}
			Dispatcher::Post([transfer] {
				transfer->progressQueued = false;
				if (const ProgressCallback onProgress = transfer->onProgress) {
					onProgress(transfer->received.load(), transfer->total.load());
				}
			});
		}

		void FinalizeDownload(const str& target, Response& response) {
			if (target.empty()) {
				return;
			}
			const std::filesystem::path partial =
				std::filesystem::path(reinterpret_cast<const char8_t*>(target.c_str())).concat(".download");
			const bool succeeded = response.status >= 200 && response.status < 300;
			std::error_code ec;
			bool moved = false;
			if (response.result == Result::Ok && succeeded) {
				std::ofstream file(partial, std::ios::binary | std::ios::trunc);
				if (!file) {
					response.result = Result::Failed;
					response.error = "can't open " + target + ".download for writing";
				} else {
					if (!response.body.empty()) {
						file.write(reinterpret_cast<const char*>(response.body.data()),
								   static_cast<std::streamsize>(response.body.size()));
					}
					const bool ok = static_cast<bool>(file);
					file.close();
					if (!ok) {
						response.result = Result::Failed;
						response.error = "can't write " + target;
					} else {
						std::filesystem::rename(partial,
												std::filesystem::path(reinterpret_cast<const char8_t*>(target.c_str())), ec);
						moved = !ec;
						if (ec) {
							response.result = Result::Failed;
							response.error = "can't move download into place: " + ec.message();
						} else {
							response.body.clear();
						}
					}
				}
			}
			if (!moved) {
				std::filesystem::remove(partial, ec);
			}
		}

		void PostFinished(const std::shared_ptr<Transfer>& transfer, Response response) {
			transfer->control->write = nullptr;
			transfer->control->progress = nullptr;
			Dispatcher::Post([transfer, finished = std::move(response)]() {
				{
					HTTPState& state = State();
					std::lock_guard<std::mutex> guard(state.lock);
					state.active.erase(transfer->id);
				}
				if (transfer->onDone) {
					transfer->onDone(finished);
				}
			});
		}

		void StartAsyncTransfer(const std::shared_ptr<Transfer>& transfer) {
			transfer->control->progress = [transfer](u64 received, u64 total) {
				transfer->received = received;
				transfer->total = total;
				QueueProgress(transfer);
			};
			HTTPBackend::StartAsync(transfer->request, transfer->control, [transfer](Response response) {
				FinalizeDownload(transfer->request.downloadPath, response);
				PostFinished(transfer, std::move(response));
			});
		}

		void Run(const std::shared_ptr<Transfer>& transfer) {
			if (transfer->request.timeoutSeconds > 0.0f) {
				const std::chrono::duration<f32> limit(transfer->request.timeoutSeconds);
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(limit);
				State().watchdog.Add(deadline, transfer);
			}
			Response response;
			response.url = transfer->request.url;
			const str& target = transfer->request.downloadPath;
			const std::filesystem::path partial = std::filesystem::path(reinterpret_cast<const char8_t*>(target.c_str())).concat(".download");
			std::ofstream file;
			bool writeFailed = false;
			if (!target.empty()) {
				file.open(partial, std::ios::binary | std::ios::trunc);
				if (!file) {
					response.error = "can't open " + target + ".download for writing";
				}
			}
			if (target.empty() || file) {
				transfer->control->write = [&](std::span<const u8> bytes) {
					if (target.empty()) {
						response.body.insert(response.body.end(), bytes.begin(), bytes.end());
					} else {
						file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
						writeFailed = writeFailed || !file;
					}
					return !writeFailed;
				};
				transfer->control->progress = [&transfer](u64 received, u64 total) {
					transfer->received = received;
					transfer->total = total;
					QueueProgress(transfer);
				};
				if (transfer->control->cancelled) {
					response.result = transfer->control->timedOut ? Result::TimedOut : Result::Cancelled;
					response.error = transfer->control->timedOut ? "timed out" : "cancelled";
				} else if (!HTTPBackend::IsAvailable()) {
					response.error = "no HTTP backend on this system";
				} else {
					HTTPBackend::Perform(transfer->request, response, *transfer->control);
				}
			}
			if (!target.empty()) {
				file.close();
				std::error_code ec;
				const bool succeeded = response.status >= 200 && response.status < 300;
				bool moved = false;
				if (response.result == Result::Ok && !writeFailed && succeeded) {
					std::filesystem::rename(partial, std::filesystem::path(reinterpret_cast<const char8_t*>(target.c_str())), ec);
					moved = !ec;
					if (ec) {
						response.result = Result::Failed;
						response.error = "can't move download into place: " + ec.message();
					}
				}
				if (!moved) {
					std::filesystem::remove(partial, ec);
				}
			}
			if (writeFailed && response.result == Result::Ok) {
				response.result = Result::Failed;
				response.error = "can't write " + target;
			}
			PostFinished(transfer, std::move(response));
		}

	} // anonymous namespace

	std::string_view Response::FindHeader(std::string_view name) const {
		for (const Header& header : headers) {
			if (SameName(header.name, name)) {
				return header.value;
			}
		}
		return {};
	}

	RequestId Send(Request request, DoneCallback onDone, ProgressCallback onProgress) {
		auto transfer = std::make_shared<Transfer>();
		transfer->request = std::move(request);
		transfer->onDone = std::move(onDone);
		transfer->onProgress = std::move(onProgress);
		transfer->wantsProgress = static_cast<bool>(transfer->onProgress);
		{
			HTTPState& state = State();
			std::lock_guard<std::mutex> guard(state.lock);
			transfer->id = state.nextId++;
			state.active.emplace(transfer->id, transfer);
		}
		const RequestId id = transfer->id;
		if (HTTPBackend::SupportsAsync()) {
			StartAsyncTransfer(transfer);
		} else {
			Workers::Run(Workers::Pool::HTTP, [transfer] { Run(transfer); });
		}
		return id;
	}

	void Cancel(RequestId id) {
		std::shared_ptr<Transfer> transfer;
		{
			HTTPState& state = State();
			std::lock_guard<std::mutex> guard(state.lock);
			const auto found = state.active.find(id);
			if (found == state.active.end()) {
				return;
			}
			transfer = found->second;
		}
		Abort(*transfer, false);
	}

	bool IsAvailable() {
		return HTTPBackend::IsAvailable();
	}

	std::string_view GetMethodName(Method method) {
		switch (method) {
		case Method::Get:
			return "GET";
		case Method::Post:
			return "POST";
		case Method::Put:
			return "PUT";
		case Method::Delete:
			return "DELETE";
		case Method::Patch:
			return "PATCH";
		case Method::Head:
			return "HEAD";
		}
		return "GET";
	}

} // namespace Shalltear::Net::HTTP

namespace Shalltear::Net::HTTPInternal {

	void Touch() {
		HTTP::State();
	}

	void StopWatchdog() {
		HTTP::State().watchdog.Stop();
	}

	void CancelAll() {
		Vector<HTTP::RequestId> ids{GetMemoryResource(MemTag::Platform)};
		{
			HTTP::HTTPState& state = HTTP::State();
			std::lock_guard<std::mutex> guard(state.lock);
			for (const auto& [id, transfer] : state.active) {
				ids.push_back(id);
			}
		}
		for (HTTP::RequestId id : ids) {
			HTTP::Cancel(id);
		}
	}

	void DropCallbacks() {
		Vector<std::shared_ptr<HTTP::Transfer>> transfers{GetMemoryResource(MemTag::Platform)};
		{
			HTTP::HTTPState& state = HTTP::State();
			std::lock_guard<std::mutex> guard(state.lock);
			for (const auto& [id, transfer] : state.active) {
				transfers.push_back(transfer);
			}
		}
		for (const std::shared_ptr<HTTP::Transfer>& transfer : transfers) {
			transfer->wantsProgress = false;
			transfer->onDone = {};
			transfer->onProgress = {};
			HTTP::Abort(*transfer, false);
		}
	}

} // namespace Shalltear::Net::HTTPInternal
