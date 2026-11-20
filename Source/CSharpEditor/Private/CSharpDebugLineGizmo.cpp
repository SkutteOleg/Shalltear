#include "CSharpDebugLineGizmo.h"

#include "DebugLines.h"

#include "Shalltear/Core/StringHash.h"

#include "Shalltear/Editor/EditorGizmos.h"

namespace Shalltear::CSharpEditor {

	namespace {

		void DrawDebugLines(GizmoDrawContext& context) {
			CSharpImpl::DebugLines::Collect(*context.lines, *context.overlayLines);
		}

	} // anonymous namespace

	bool RegisterDebugLineGizmo(const void* owner) {
		return EditorGizmos::RegisterSceneGizmo(StringHash("CSharp.DebugLines"), &DrawDebugLines, owner);
	}

} // namespace Shalltear::CSharpEditor
