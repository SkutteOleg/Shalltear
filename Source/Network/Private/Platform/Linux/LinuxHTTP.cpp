#include "HTTPBackend.h"

#include <dlfcn.h>

#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>

namespace Shalltear::Net::HTTPBackend {

	namespace {

		// libcurl's stable ABI, from curl.h
		using CurlHandle = void;
		using CurlCode = int;
		struct CurlList;
		using CurlOffset = i64;

		constexpr int OPT_URL = 10002;
		constexpr int OPT_WRITEDATA = 10001;
		constexpr int OPT_ERRORBUFFER = 10010;
		constexpr int OPT_WRITEFUNCTION = 20011;
		constexpr int OPT_POSTFIELDS = 10015;
		constexpr int OPT_USERAGENT = 10018;
		constexpr int OPT_HTTPHEADER = 10023;
		constexpr int OPT_HEADERDATA = 10029;
		constexpr int OPT_CUSTOMREQUEST = 10036;
		constexpr int OPT_NOPROGRESS = 43;
		constexpr int OPT_NOBODY = 44;
		constexpr int OPT_FOLLOWLOCATION = 52;
		constexpr int OPT_XFERINFODATA = 10057;
		constexpr int OPT_MAXREDIRS = 68;
		constexpr int OPT_HEADERFUNCTION = 20079;
		constexpr int OPT_NOSIGNAL = 99;
		constexpr int OPT_ACCEPT_ENCODING = 10102;
		constexpr int OPT_POSTFIELDSIZE_LARGE = 30120;
		constexpr int OPT_TIMEOUT_MS = 155;
		constexpr int OPT_PROTOCOLS = 181;
		constexpr int OPT_REDIR_PROTOCOLS = 182;
		constexpr int OPT_XFERINFOFUNCTION = 20219;
		constexpr int INFO_EFFECTIVE_URL = 0x100001;
		constexpr int INFO_RESPONSE_CODE = 0x200002;
		constexpr long PROTO_HTTP_HTTPS = 1 | 2;
		constexpr long GLOBAL_DEFAULT = 3;
		constexpr CurlCode E_OK = 0;
		constexpr CurlCode E_WRITE_ERROR = 23;
		constexpr CurlCode E_OPERATION_TIMEDOUT = 28;
		constexpr CurlCode E_ABORTED_BY_CALLBACK = 42;
		constexpr size_t ERROR_SIZE = 256;

		using WriteFn = size_t (*)(char*, size_t, size_t, void*);
		using ProgressFn = int (*)(void*, CurlOffset, CurlOffset, CurlOffset, CurlOffset);

		struct Curl {
			CurlCode (*globalInit)(long) = nullptr;
			CurlHandle* (*easyInit)() = nullptr;
			CurlCode (*easySetopt)(CurlHandle*, int, ...) = nullptr;
			CurlCode (*easyPerform)(CurlHandle*) = nullptr;
			CurlCode (*easyGetinfo)(CurlHandle*, int, ...) = nullptr;
			void (*easyCleanup)(CurlHandle*) = nullptr;
			const char* (*easyStrerror)(CurlCode) = nullptr;
			CurlList* (*slistAppend)(CurlList*, const char*) = nullptr;
			void (*slistFreeAll)(CurlList*) = nullptr;
			bool loaded = false;
		};

		const Curl& Library() {
			static Curl curl;
			static std::once_flag once;
			std::call_once(once, [] {
				void* image = nullptr;
				for (const char* name : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"}) {
					image = dlopen(name, RTLD_NOW | RTLD_LOCAL);
					if (image) {
						break;
					}
				}
				if (!image) {
					return;
				}
				auto bind = [image](auto& target, const char* name) {
					target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(dlsym(image, name));
					return target != nullptr;
				};
				const bool all = bind(curl.globalInit, "curl_global_init") && bind(curl.easyInit, "curl_easy_init") &&
								 bind(curl.easySetopt, "curl_easy_setopt") && bind(curl.easyPerform, "curl_easy_perform") &&
								 bind(curl.easyGetinfo, "curl_easy_getinfo") && bind(curl.easyCleanup, "curl_easy_cleanup") &&
								 bind(curl.easyStrerror, "curl_easy_strerror") && bind(curl.slistAppend, "curl_slist_append") &&
								 bind(curl.slistFreeAll, "curl_slist_free_all");
				curl.loaded = all && curl.globalInit(GLOBAL_DEFAULT) == E_OK;
			});
			return curl;
		}

		struct Session {
			HTTP::Response* response = nullptr;
			Control* control = nullptr;
			bool writeFailed = false;
		};

		size_t OnBody(char* data, size_t size, size_t count, void* user) {
			auto* session = static_cast<Session*>(user);
			const size_t bytes = size * count;
			if (!session->control->write({reinterpret_cast<const u8*>(data), bytes})) {
				session->writeFailed = true;
				return 0;
			}
			return bytes;
		}

		size_t OnHeader(char* data, size_t size, size_t count, void* user) {
			auto* session = static_cast<Session*>(user);
			const size_t bytes = size * count;
			std::string_view line(data, bytes);
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
				line.remove_suffix(1);
			}
			if (line.starts_with("HTTP/")) {
				session->response->headers.clear();
				return bytes;
			}
			const size_t colon = line.find(':');
			if (colon != std::string_view::npos) {
				std::string_view value = line.substr(colon + 1);
				while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
					value.remove_prefix(1);
				}
				session->response->headers.push_back({str(line.substr(0, colon)), str(value)});
			}
			return bytes;
		}

		int OnProgress(void* user, CurlOffset total, CurlOffset now, CurlOffset, CurlOffset) {
			auto* session = static_cast<Session*>(user);
			if (session->control->cancelled) {
				return 1;
			}
			if (session->control->progress && now > 0) {
				session->control->progress(static_cast<u64>(now), static_cast<u64>(total > 0 ? total : 0));
			}
			return 0;
		}

	} // anonymous namespace

	bool IsAvailable() {
		return Library().loaded;
	}

	void Perform(const HTTP::Request& request, HTTP::Response& response, Control& control) {
		const Curl& curl = Library();
		CurlHandle* easy = curl.loaded ? curl.easyInit() : nullptr;
		if (!easy) {
			response.error = "libcurl couldn't start transfer";
			return;
		}
		Session session{&response, &control};
		char errorText[ERROR_SIZE] = {};
		CurlList* headers = nullptr;
		for (const HTTP::Header& header : request.headers) {
			headers = curl.slistAppend(headers, std::format("{}: {}", header.name, header.value).c_str());
		}
		curl.easySetopt(easy, OPT_URL, request.url.c_str());
		curl.easySetopt(easy, OPT_NOSIGNAL, 1L);
		curl.easySetopt(easy, OPT_USERAGENT, "Shalltear");
		curl.easySetopt(easy, OPT_ERRORBUFFER, errorText);
		curl.easySetopt(easy, OPT_PROTOCOLS, PROTO_HTTP_HTTPS);
		curl.easySetopt(easy, OPT_REDIR_PROTOCOLS, PROTO_HTTP_HTTPS);
		curl.easySetopt(easy, OPT_ACCEPT_ENCODING, "");
		curl.easySetopt(easy, OPT_FOLLOWLOCATION, request.maxRedirects > 0 ? 1L : 0L);
		curl.easySetopt(easy, OPT_MAXREDIRS, static_cast<long>(request.maxRedirects));
		if (request.timeoutSeconds > 0.0f) {
			curl.easySetopt(easy, OPT_TIMEOUT_MS, static_cast<long>(request.timeoutSeconds * 1000.0f));
		}
		if (headers) {
			curl.easySetopt(easy, OPT_HTTPHEADER, headers);
		}
		const str method(HTTP::GetMethodName(request.method));
		if (request.method == HTTP::Method::Head) {
			curl.easySetopt(easy, OPT_NOBODY, 1L);
		} else if (request.method != HTTP::Method::Get) {
			curl.easySetopt(easy, OPT_CUSTOMREQUEST, method.c_str());
		}
		if (!request.body.empty() || request.method == HTTP::Method::Post || request.method == HTTP::Method::Put ||
			request.method == HTTP::Method::Patch) {
			curl.easySetopt(easy, OPT_POSTFIELDSIZE_LARGE, static_cast<CurlOffset>(request.body.size()));
			curl.easySetopt(easy, OPT_POSTFIELDS, request.body.empty() ? "" : reinterpret_cast<const char*>(request.body.data()));
		}
		curl.easySetopt(easy, OPT_WRITEFUNCTION, static_cast<WriteFn>(&OnBody));
		curl.easySetopt(easy, OPT_WRITEDATA, &session);
		curl.easySetopt(easy, OPT_HEADERFUNCTION, static_cast<WriteFn>(&OnHeader));
		curl.easySetopt(easy, OPT_HEADERDATA, &session);
		curl.easySetopt(easy, OPT_NOPROGRESS, 0L);
		curl.easySetopt(easy, OPT_XFERINFOFUNCTION, static_cast<ProgressFn>(&OnProgress));
		curl.easySetopt(easy, OPT_XFERINFODATA, &session);

		const CurlCode code = curl.easyPerform(easy);
		long status = 0;
		curl.easyGetinfo(easy, INFO_RESPONSE_CODE, &status);
		response.status = static_cast<u32>(status);
		const char* finalURL = nullptr;
		if (curl.easyGetinfo(easy, INFO_EFFECTIVE_URL, &finalURL) == E_OK && finalURL) {
			response.url = finalURL;
		}
		if (code == E_OK) {
			response.result = HTTP::Result::Ok;
		} else if (code == E_ABORTED_BY_CALLBACK && control.cancelled) {
			response.result = control.timedOut ? HTTP::Result::TimedOut : HTTP::Result::Cancelled;
			response.error = control.timedOut ? "timed out" : "cancelled";
		} else if (code == E_OPERATION_TIMEDOUT) {
			response.result = HTTP::Result::TimedOut;
			response.error = "timed out";
		} else {
			response.result = HTTP::Result::Failed;
			const char* reason = errorText[0] ? errorText : curl.easyStrerror(code);
			response.error = code == E_WRITE_ERROR && session.writeFailed ? str("can't write response body") : std::format("{} ({})", reason, code);
		}
		curl.easyCleanup(easy);
		if (headers) {
			curl.slistFreeAll(headers);
		}
	}

	bool SupportsAsync() {
		return false;
	}

	void StartAsync(const HTTP::Request&, std::shared_ptr<Control>, std::function<void(HTTP::Response)>) {
	}

} // namespace Shalltear::Net::HTTPBackend
