#pragma once

#include "Shalltear/Core/Types.h"

#include <span>

namespace Shalltear::EditorPlatform::PreviewSocket {

	using Handle = i64;
	inline constexpr Handle INVALID = -1;

	Handle ListenLoopback(u16& outPort);
	Handle AcceptWithTimeout(Handle listener, i32 timeoutMs);
	i32 Receive(Handle socket, std::span<u8> buffer);
	bool SendAll(Handle socket, std::span<const u8> data);
	void Close(Handle socket);

} // namespace Shalltear::EditorPlatform::PreviewSocket
