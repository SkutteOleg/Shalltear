#pragma once

#include "Shalltear/Core/Types.h"
#include "Shalltear/Editor/EditorAPI.h"

#include <string_view>

namespace Shalltear::WebPreviewServer {

	// starts or updates the loopback preview HTTP server for directory and returns its URL
	SHALLTEAR_EDITOR_API str Serve(const str& rootDirectory, bool crossOriginIsolated);

	SHALLTEAR_EDITOR_API void Shutdown();

	SHALLTEAR_EDITOR_API u16 GetPort();

	SHALLTEAR_EDITOR_API bool IsRunning();

	SHALLTEAR_EDITOR_API std::string_view MimeTypeForPath(std::string_view path);

} // namespace Shalltear::WebPreviewServer
