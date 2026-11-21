#include "Shalltear/Core/Module/EngineContext.h"

#include "Shalltear/Core/BuildInfo.h"

#include "Module/EngineStateTable.h"

namespace Shalltear {

	using namespace ModuleInternal;

	void CaptureEngineContext(EngineContext& out) {
		static EngineStateTable table{};

		CaptureMemoryEngineState(table);
		CaptureContainersEngineState(table);
		CaptureObjectRegistryEngineState(table);
		CaptureLogEngineState(table);
		CaptureTypeRegistryEngineState(table);
		CaptureVariantDispatchEngineState(table);
		CaptureHandleResolverEngineState(table);
		CaptureAssetDatabaseEngineState(table);
		CaptureAssetImporterEngineState(table);
		CaptureEventBusEngineState(table);
		CaptureTimeEngineState(table);
		CaptureJobSystemEngineState(table);
		CaptureFileSystemEngineState(table);
		CaptureWindowSystemEngineState(table);
		CaptureInputEngineState(table);
		CaptureSceneSystemEngineState(table);
		CaptureRenderSystemEngineState(table);
		CaptureUISystemEngineState(table);
		CaptureWidgetRegistryEngineState(table);
		CaptureSettingsEngineState(table);
		CaptureFrameStagesEngineState(table);
		CaptureFramePassesEngineState(table);
		CaptureModuleThunkRegistryEngineState(table);
		table.slotCount = ENGINE_STATE_SLOT_COUNT;

		out.version = SLT_ENGINE_CONTEXT_VERSION;
		out.size = sizeof(EngineContext);
		out.engineHash = BuildInfo::GetEngineHash();
		out.stateTable = &table;
	}

	bool BindEngineContext(const EngineContext* context) {
		if (!context || !context->stateTable) {
			return false;
		}
		if (context->version != SLT_ENGINE_CONTEXT_VERSION) {
			return false;
		}
		if (context->size != sizeof(EngineContext)) {
			return false;
		}
		if (context->engineHash != BuildInfo::GetEngineHash()) {
			return false;
		}

		const auto* table = static_cast<const EngineStateTable*>(context->stateTable);
		if (table->slotCount != ENGINE_STATE_SLOT_COUNT) {
			return false;
		}

		BindMemoryEngineState(*table);
		BindContainersEngineState(*table);
		BindObjectRegistryEngineState(*table);
		BindLogEngineState(*table);
		BindTypeRegistryEngineState(*table);
		BindVariantDispatchEngineState(*table);
		BindHandleResolverEngineState(*table);
		BindAssetDatabaseEngineState(*table);
		BindAssetImporterEngineState(*table);
		BindEventBusEngineState(*table);
		BindTimeEngineState(*table);
		BindJobSystemEngineState(*table);
		BindFileSystemEngineState(*table);
		BindWindowSystemEngineState(*table);
		BindInputEngineState(*table);
		BindSceneSystemEngineState(*table);
		BindRenderSystemEngineState(*table);
		BindUISystemEngineState(*table);
		BindWidgetRegistryEngineState(*table);
		BindSettingsEngineState(*table);
		BindFrameStagesEngineState(*table);
		BindFramePassesEngineState(*table);
		BindModuleThunkRegistryEngineState(*table);
		return true;
	}

} // namespace Shalltear
