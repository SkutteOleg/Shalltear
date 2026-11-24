#include "HTTPBackend.h"

#include "Shalltear/Core/StringUtil.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <cstdlib>
#include <format>

namespace Shalltear::Net::HTTPBackend {

	namespace {

		str ErrorText(DWORD code) {
			wchar_t* buffer = nullptr;
			DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
										  GetModuleHandleW(L"winhttp.dll"), code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
			if (length == 0) {
				length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
										reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
			}
			str text = length > 0 ? StringUtil::WideToUTF8(std::wstring_view(buffer, length)) : str{};
			if (buffer) {
				LocalFree(buffer);
			}
			while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == '.' || text.back() == ' ')) {
				text.pop_back();
			}
			return std::format("{} ({})", text.empty() ? "HTTP error" : text, code);
		}

		struct Handles {
			HINTERNET session = nullptr;
			HINTERNET connection = nullptr;
			HINTERNET request = nullptr;

			~Handles() {
				if (request) {
					WinHttpCloseHandle(request);
				}
				if (connection) {
					WinHttpCloseHandle(connection);
				}
				if (session) {
					WinHttpCloseHandle(session);
				}
			}
		};

		std::wstring QueryString(HINTERNET request, DWORD info) {
			DWORD bytes = 0;
			WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &bytes, WINHTTP_NO_HEADER_INDEX);
			if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
				return {};
			}
			std::wstring text(bytes / sizeof(wchar_t), L'\0');
			if (!WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) {
				return {};
			}
			text.resize(bytes / sizeof(wchar_t));
			return text;
		}

		void ParseHeaders(std::string_view raw, Vector<HTTP::Header>& out) {
			out.clear();
			size_t at = raw.find("\r\n");
			while (at != std::string_view::npos && at + 2 < raw.size()) {
				const size_t start = at + 2;
				const size_t end = raw.find("\r\n", start);
				const std::string_view line = raw.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
				const size_t colon = line.find(':');
				if (colon != std::string_view::npos) {
					std::string_view value = line.substr(colon + 1);
					while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
						value.remove_prefix(1);
					}
					out.push_back({str(line.substr(0, colon)), str(value)});
				}
				at = end;
			}
		}

		void Fail(HTTP::Response& response, const Control& control, DWORD code) {
			if (control.cancelled || code == ERROR_WINHTTP_OPERATION_CANCELLED || code == ERROR_INVALID_HANDLE) {
				response.result = control.timedOut ? HTTP::Result::TimedOut : HTTP::Result::Cancelled;
				response.error = control.timedOut ? "timed out" : "cancelled";
				return;
			}
			response.result = code == ERROR_WINHTTP_TIMEOUT ? HTTP::Result::TimedOut : HTTP::Result::Failed;
			response.error = ErrorText(code);
		}

	} // anonymous namespace

	bool IsAvailable() {
		return true;
	}

	void Perform(const HTTP::Request& request, HTTP::Response& response, Control& control) {
		const std::wstring url = StringUtil::UTF8ToWide(request.url);
		URL_COMPONENTS parts{};
		parts.dwStructSize = sizeof(parts);
		parts.dwSchemeLength = static_cast<DWORD>(-1);
		parts.dwHostNameLength = static_cast<DWORD>(-1);
		parts.dwUrlPathLength = static_cast<DWORD>(-1);
		parts.dwExtraInfoLength = static_cast<DWORD>(-1);
		if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)) {
			response.error = "not an http/https URL: " + request.url;
			return;
		}
		const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
		std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
		path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
		if (path.empty()) {
			path = L"/";
		}

		Handles handles;
		handles.session = WinHttpOpen(L"Shalltear", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!handles.session) {
			handles.session = WinHttpOpen(L"Shalltear", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		}
		if (!handles.session) {
			Fail(response, control, GetLastError());
			return;
		}
		const int timeout = request.timeoutSeconds > 0.0f ? static_cast<int>(request.timeoutSeconds * 1000.0f) : 0;
		WinHttpSetTimeouts(handles.session, timeout, timeout, timeout, timeout);
		DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
		WinHttpSetOption(handles.session, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));

		handles.connection = WinHttpConnect(handles.session, host.c_str(), parts.nPort, 0);
		if (!handles.connection) {
			Fail(response, control, GetLastError());
			return;
		}
		const std::wstring verb = StringUtil::UTF8ToWide(HTTP::GetMethodName(request.method));
		const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
		handles.request = WinHttpOpenRequest(handles.connection, verb.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
											 WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
		if (!handles.request) {
			Fail(response, control, GetLastError());
			return;
		}
		WinHttpSetTimeouts(handles.request, timeout, timeout, timeout, timeout);
		if (request.maxRedirects == 0) {
			DWORD disable = WINHTTP_DISABLE_REDIRECTS;
			WinHttpSetOption(handles.request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
		} else {
			DWORD redirects = request.maxRedirects;
			WinHttpSetOption(handles.request, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects, sizeof(redirects));
		}

		bool aborted = false;
		{
			std::lock_guard<std::mutex> guard(control.abortLock);
			if (control.cancelled) {
				Fail(response, control, ERROR_WINHTTP_OPERATION_CANCELLED);
				return;
			}
			HINTERNET target = handles.request;
			control.abort = [target, &aborted] {
				aborted = true;
				WinHttpCloseHandle(target);
			};
		}
		auto finish = [&] {
			std::lock_guard<std::mutex> guard(control.abortLock);
			control.abort = nullptr;
			if (aborted) {
				handles.request = nullptr;
			}
		};

		std::wstring headers;
		for (const HTTP::Header& header : request.headers) {
			headers += StringUtil::UTF8ToWide(header.name) + L": " + StringUtil::UTF8ToWide(header.value) + L"\r\n";
		}
		LPVOID body = request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<u8*>(request.body.data());
		const DWORD bodySize = static_cast<DWORD>(request.body.size());
		if (!WinHttpSendRequest(handles.request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), static_cast<DWORD>(-1L), body,
								bodySize, bodySize, 0) ||
			!WinHttpReceiveResponse(handles.request, nullptr)) {
			const DWORD code = GetLastError();
			finish();
			Fail(response, control, code);
			return;
		}

		DWORD status = 0;
		DWORD statusSize = sizeof(status);
		WinHttpQueryHeaders(handles.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
							WINHTTP_NO_HEADER_INDEX);
		response.status = status;
		ParseHeaders(StringUtil::WideToUTF8(QueryString(handles.request, WINHTTP_QUERY_RAW_HEADERS_CRLF)), response.headers);
		DWORD urlBytes = 0;
		WinHttpQueryOption(handles.request, WINHTTP_OPTION_URL, nullptr, &urlBytes);
		if (urlBytes > 0) {
			std::wstring finalURL(urlBytes / sizeof(wchar_t), L'\0');
			if (WinHttpQueryOption(handles.request, WINHTTP_OPTION_URL, finalURL.data(), &urlBytes)) {
				finalURL.resize(wcsnlen(finalURL.c_str(), finalURL.size()));
				response.url = StringUtil::WideToUTF8(finalURL);
			}
		}
		u64 total = 0;
		const str length(response.FindHeader("Content-Length"));
		if (!length.empty()) {
			total = std::strtoull(length.c_str(), nullptr, 10);
		}

		u64 received = 0;
		Vector<u8> buffer{GetMemoryResource(MemTag::Platform)};
		buffer.resize(64 * 1024);
		for (;;) {
			DWORD read = 0;
			if (!WinHttpReadData(handles.request, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
				const DWORD code = GetLastError();
				finish();
				Fail(response, control, code);
				return;
			}
			if (read == 0) {
				break;
			}
			received += read;
			if (!control.write({buffer.data(), read})) {
				finish();
				response.result = HTTP::Result::Failed;
				return;
			}
			if (control.progress) {
				control.progress(received, total);
			}
		}
		finish();
		response.result = control.cancelled ? HTTP::Result::Cancelled : HTTP::Result::Ok;
	}

	bool SupportsAsync() {
		return false;
	}

	void StartAsync(const HTTP::Request&, std::shared_ptr<Control>, std::function<void(HTTP::Response)>) {
	}

} // namespace Shalltear::Net::HTTPBackend
