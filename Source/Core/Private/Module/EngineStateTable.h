#pragma once

#include "Shalltear/Core/Types.h"

namespace Shalltear::ModuleInternal {

	struct EngineStateTable {
		u32 slotCount;

		void* memoryManager;
		void* memoryResources;
		void* objectRegistry;
		void* logManager;
		void* typeRegistry;
		void* variantDispatch;
		void* handleResolver;
		void* assetDatabase;
		void* assetImporters;
		void* eventBus;
		void* time;
		void* jobSystem;
		void* fileSystem;
		void* windowSystem;
		void* input;
		void* sceneSystem;
		void* renderSystem;
		void* uiSystem;
		void* widgetRegistry;
		void* settings;
		void* frameStages;
		void* framePasses;
		void* moduleThunkRegistry;
	};

	inline constexpr u32 ENGINE_STATE_SLOT_COUNT = 23;

	void CaptureMemoryEngineState(EngineStateTable& table);
	void BindMemoryEngineState(const EngineStateTable& table);
	void CaptureContainersEngineState(EngineStateTable& table);
	void BindContainersEngineState(const EngineStateTable& table);
	void CaptureObjectRegistryEngineState(EngineStateTable& table);
	void BindObjectRegistryEngineState(const EngineStateTable& table);
	void CaptureLogEngineState(EngineStateTable& table);
	void BindLogEngineState(const EngineStateTable& table);
	void CaptureTypeRegistryEngineState(EngineStateTable& table);
	void BindTypeRegistryEngineState(const EngineStateTable& table);
	void CaptureVariantDispatchEngineState(EngineStateTable& table);
	void BindVariantDispatchEngineState(const EngineStateTable& table);
	void CaptureHandleResolverEngineState(EngineStateTable& table);
	void BindHandleResolverEngineState(const EngineStateTable& table);
	void CaptureAssetDatabaseEngineState(EngineStateTable& table);
	void BindAssetDatabaseEngineState(const EngineStateTable& table);
	void CaptureAssetImporterEngineState(EngineStateTable& table);
	void BindAssetImporterEngineState(const EngineStateTable& table);
	void CaptureEventBusEngineState(EngineStateTable& table);
	void BindEventBusEngineState(const EngineStateTable& table);
	void CaptureTimeEngineState(EngineStateTable& table);
	void BindTimeEngineState(const EngineStateTable& table);
	void CaptureJobSystemEngineState(EngineStateTable& table);
	void BindJobSystemEngineState(const EngineStateTable& table);
	void CaptureFileSystemEngineState(EngineStateTable& table);
	void BindFileSystemEngineState(const EngineStateTable& table);
	void CaptureWindowSystemEngineState(EngineStateTable& table);
	void BindWindowSystemEngineState(const EngineStateTable& table);
	void CaptureInputEngineState(EngineStateTable& table);
	void BindInputEngineState(const EngineStateTable& table);
	void CaptureSceneSystemEngineState(EngineStateTable& table);
	void BindSceneSystemEngineState(const EngineStateTable& table);
	void CaptureRenderSystemEngineState(EngineStateTable& table);
	void BindRenderSystemEngineState(const EngineStateTable& table);
	void CaptureUISystemEngineState(EngineStateTable& table);
	void BindUISystemEngineState(const EngineStateTable& table);
	void CaptureWidgetRegistryEngineState(EngineStateTable& table);
	void BindWidgetRegistryEngineState(const EngineStateTable& table);
	void CaptureSettingsEngineState(EngineStateTable& table);
	void BindSettingsEngineState(const EngineStateTable& table);
	void CaptureFrameStagesEngineState(EngineStateTable& table);
	void BindFrameStagesEngineState(const EngineStateTable& table);
	void CaptureFramePassesEngineState(EngineStateTable& table);
	void BindFramePassesEngineState(const EngineStateTable& table);
	void CaptureModuleThunkRegistryEngineState(EngineStateTable& table);
	void BindModuleThunkRegistryEngineState(const EngineStateTable& table);

} // namespace Shalltear::ModuleInternal
