#include "HTTPBackend.h"

#include "Shalltear/Core/Containers.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <emscripten/fetch.h>

namespace Shalltear::Net::HTTPBackend {

	namespace {

		struct WebFetchContext {
			std::shared_ptr<Control> control;
			std::function<void(HTTP::Response)> onComplete;
			str requestUrl;
			std::vector<str> ownedHeaders;
			std::vector<const char*> headerPtrs;
			std::vector<u8> requestBody;
			std::mutex stateLock;
			emscripten_fetch_t* fetch = nullptr;
			bool completed = false;
		};

		void ParseResponseHeaders(emscripten_fetch_t* fetch, Vector<HTTP::Header>& out) {
			out.clear();
			const size_t length = emscripten_fetch_get_response_headers_length(fetch);
			if (length == 0) {
				return;
			}
			std::string raw(length + 1, '\0');
			emscripten_fetch_get_response_headers(fetch, raw.data(), raw.size());
			char** unpacked = emscripten_fetch_unpack_response_headers(raw.c_str());
			if (!unpacked) {
				return;
			}
			for (size_t i = 0; unpacked[i] && unpacked[i + 1]; i += 2) {
				out.push_back(HTTP::Header{str(unpacked[i]), str(unpacked[i + 1])});
			}
			emscripten_fetch_free_unpacked_response_headers(unpacked);
		}

		void FinishFetch(emscripten_fetch_t* fetch, bool succeeded) {
			auto* box = static_cast<std::shared_ptr<WebFetchContext>*>(fetch->userData);
			if (!box) {
				emscripten_fetch_close(fetch);
				return;
			}
			std::shared_ptr<WebFetchContext> ctx = *box;
			delete box;

			{
				std::lock_guard<std::mutex> guard(ctx->stateLock);
				if (ctx->completed) {
					emscripten_fetch_close(fetch);
					return;
				}
				ctx->completed = true;
				ctx->fetch = nullptr;
			}
			{
				std::lock_guard<std::mutex> guard(ctx->control->abortLock);
				ctx->control->abort = nullptr;
			}

			HTTP::Response response;
			response.url = fetch->url ? str(fetch->url) : ctx->requestUrl;
			response.status = fetch->status;
			ParseResponseHeaders(fetch, response.headers);

			if (fetch->data && fetch->numBytes > 0) {
				const u8* bytes = reinterpret_cast<const u8*>(fetch->data);
				response.body.assign(bytes, bytes + static_cast<size_t>(fetch->numBytes));
			}

			if (ctx->control->cancelled) {
				response.result = ctx->control->timedOut ? HTTP::Result::TimedOut : HTTP::Result::Cancelled;
				response.error = ctx->control->timedOut ? "timed out" : "cancelled";
			} else if (succeeded) {
				response.result = HTTP::Result::Ok;
			} else if (fetch->status == 408) {
				response.result = HTTP::Result::TimedOut;
				response.error = "timed out";
			} else {
				response.result = HTTP::Result::Failed;
				response.error = fetch->statusText[0] ? str(fetch->statusText) : str("fetch failed");
			}

			emscripten_fetch_close(fetch);

			if (ctx->onComplete) {
				ctx->onComplete(std::move(response));
			}
		}

		void OnSuccess(emscripten_fetch_t* fetch) {
			FinishFetch(fetch, true);
		}

		void OnError(emscripten_fetch_t* fetch) {
			FinishFetch(fetch, false);
		}

		void OnProgress(emscripten_fetch_t* fetch) {
			auto* box = static_cast<std::shared_ptr<WebFetchContext>*>(fetch->userData);
			if (!box || !(*box) || !(*box)->control->progress) {
				return;
			}
			const u64 received = static_cast<u64>(fetch->dataOffset) + static_cast<u64>(fetch->numBytes);
			const u64 total = static_cast<u64>(fetch->totalBytes);
			(*box)->control->progress(received, total);
		}

	} // anonymous namespace

	bool IsAvailable() {
		return true;
	}

	bool SupportsAsync() {
		return true;
	}

	void Perform(const HTTP::Request&, HTTP::Response& response, Control&) {
		response.result = HTTP::Result::Failed;
		response.error = "synchronous HTTP is unsupported on Web";
	}

	void StartAsync(const HTTP::Request& request, std::shared_ptr<Control> control,
					std::function<void(HTTP::Response)> onComplete) {
		auto ctx = std::make_shared<WebFetchContext>();
		ctx->control = std::move(control);
		ctx->onComplete = std::move(onComplete);
		ctx->requestUrl = request.url;

		emscripten_fetch_attr_t attr{};
		emscripten_fetch_attr_init(&attr);
		const std::string_view method = HTTP::GetMethodName(request.method);
		std::memcpy(attr.requestMethod, method.data(), std::min(method.size(), sizeof(attr.requestMethod) - 1));
		attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
		attr.onsuccess = &OnSuccess;
		attr.onerror = &OnError;
		attr.onprogress = &OnProgress;
		if (request.timeoutSeconds > 0.0f) {
			attr.timeoutMSecs = static_cast<unsigned long>(request.timeoutSeconds * 1000.0f);
		}

		if (!request.headers.empty()) {
			ctx->ownedHeaders.reserve(request.headers.size() * 2);
			ctx->headerPtrs.reserve(request.headers.size() * 2 + 1);
			for (const HTTP::Header& h : request.headers) {
				ctx->ownedHeaders.push_back(h.name);
				ctx->ownedHeaders.push_back(h.value);
			}
			for (const str& entry : ctx->ownedHeaders) {
				ctx->headerPtrs.push_back(entry.c_str());
			}
			ctx->headerPtrs.push_back(nullptr);
			attr.requestHeaders = ctx->headerPtrs.data();
		}

		if (!request.body.empty()) {
			ctx->requestBody.assign(request.body.begin(), request.body.end());
			attr.requestData = reinterpret_cast<const char*>(ctx->requestBody.data());
			attr.requestDataSize = ctx->requestBody.size();
		}

		auto* box = new std::shared_ptr<WebFetchContext>(ctx);
		attr.userData = box;

		emscripten_fetch_t* fetch = emscripten_fetch(&attr, request.url.c_str());
		if (!fetch) {
			delete box;
			HTTP::Response failed;
			failed.url = request.url;
			failed.result = HTTP::Result::Failed;
			failed.error = "emscripten_fetch failed to start";
			if (ctx->onComplete) {
				ctx->onComplete(std::move(failed));
			}
			return;
		}

		{
			std::lock_guard<std::mutex> guard(ctx->stateLock);
			ctx->fetch = fetch;
		}

		std::weak_ptr<WebFetchContext> weakCtx = ctx;
		std::lock_guard<std::mutex> guard(ctx->control->abortLock);
		ctx->control->abort = [weakCtx]() {
			if (std::shared_ptr<WebFetchContext> live = weakCtx.lock()) {
				std::lock_guard<std::mutex> stateGuard(live->stateLock);
				if (!live->completed && live->fetch) {
					live->completed = true;
					emscripten_fetch_t* handle = live->fetch;
					live->fetch = nullptr;
					auto* handleBox = static_cast<std::shared_ptr<WebFetchContext>*>(handle->userData);
					handle->userData = nullptr;
					delete handleBox;
					emscripten_fetch_close(handle);
					HTTP::Response cancelled;
					cancelled.url = live->requestUrl;
					cancelled.result = live->control->timedOut ? HTTP::Result::TimedOut : HTTP::Result::Cancelled;
					cancelled.error = live->control->timedOut ? "timed out" : "cancelled";
					if (live->onComplete) {
						live->onComplete(std::move(cancelled));
					}
				}
			}
		};
	}

} // namespace Shalltear::Net::HTTPBackend
