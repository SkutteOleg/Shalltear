#include "Shalltear/Core/Asset/Asset.h"
#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/Asset/DataAsset.h"
#include "Shalltear/Core/Asset/TextAsset.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Frame/FrameStages.h"
#include "Shalltear/Core/Graphics/FramePass.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Serialization/TextSerializer.h"
#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Math.h"
#include "Shalltear/Core/Module/ModuleAPI.h"
#include "Shalltear/Core/Module/ScriptFunctions.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene/Behavior.h"
#include "Shalltear/Core/Scene/SceneObject.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/Settings.h"
#include "Shalltear/Core/Time.h"
#include "Shalltear/Core/Types.h"
#include "Shalltear/Core/UI/Context.h"
#include "Shalltear/Core/UI/WidgetRegistry.h"

#ifdef SLT_WITH_EDITOR
#include "Shalltear/Editor/BuildPipeline.h"
#include "Shalltear/Editor/BuildTarget.h"
#include "Shalltear/Editor/EditorExtensions.h"
#include "Shalltear/Editor/EditorPanel.h"
#include "Shalltear/Editor/SettingsPresentation.h"

#include <filesystem>
#include <fstream>
#endif

namespace HotReloadModuleImpl {

	using namespace Shalltear;

#ifndef SLT_HOTRELOAD_GONE

	i32 HrBuildNumber() {
#ifdef SLT_HOTRELOAD_V2
		return 2;
#else
		return 1;
#endif
	}
	SLT_SCRIPT_EXPORT(HrBuildNumber)

	class HotReloadBehavior : public Behavior {
	public:
		SLT_REFLECT_COMPONENT(HotReloadBehavior, Behavior)
		SLT_REFLECT_FIELD(counter)
		SLT_REFLECT_FIELD(label)
		SLT_REFLECT_FIELD(target)
		SLT_REFLECT_FIELD(buddy)
		SLT_REFLECT_FIELD(data)
		SLT_REFLECT_FIELD(enableCount)
		SLT_REFLECT_FIELD(disableCount)
#ifdef SLT_HOTRELOAD_V2
		SLT_REFLECT_FIELD(added)
#else
		SLT_REFLECT_FIELD(speed)
#endif
		SLT_REFLECT_METHOD(GetMagic)
		SLT_END_REFLECT()

		i32 counter = 0;
		str label;
		Handle<SceneObject> target;
		Handle<Component> buddy;

		Handle<Asset> data;

		i32 enableCount = 0;
		i32 disableCount = 0;

#ifdef SLT_HOTRELOAD_V2
		Vec3 added{};
#else
		f32 speed = 1.0f;
#endif

		i32 GetMagic() const {
#ifdef SLT_HOTRELOAD_V2
			return 2;
#else
			return 1;
#endif
		}

		void OnDisable() override {
			++disableCount;
			if (label == "destroy-owner-on-disable") {
				SceneSystem::Destroy(GetOwner());
			}
		}

		void OnEnable() override { ++enableCount; }

		void OnUpdate() override {
			++counter;
			lastDelta = Time::GetDeltaTime();
			lastUnscaledDelta = Time::GetUnscaledDeltaTime();
			lastTime = Time::GetTime();
		}

		f32 lastDelta = 0.0f;
		f32 lastUnscaledDelta = 0.0f;
		f64 lastTime = 0.0;
	};

#endif

#ifdef SLT_HOTRELOAD_V2
	constexpr i32 PROBE_MAGIC = 2;
#else
	constexpr i32 PROBE_MAGIC = 1;
#endif

	constexpr std::string_view GaugeClasses[] = {"probe-gauge"};
	constexpr UI::WidgetAttribute GaugeAttributes[] = {
		{"label", UI::WidgetAttributeType::String},
		{"ratio", UI::WidgetAttributeType::Number},
	};

	UI::WidgetResult MakeProbeGauge(UI::Context& context, UI::ElementHandle parent, const UI::WidgetArgs& args) {
		UI::ElementHandle root = context.CreateElement();
		context.AppendChild(parent, root);
		context.AddClass(root, StringHash("probe-gauge"));
		context.SetUserTag(root, static_cast<u64>(PROBE_MAGIC));

		UI::ElementHandle fill = context.CreateElement();
		context.AppendChild(root, fill);
		context.SetUserTag(fill, static_cast<u64>(args.GetNumber("ratio", 0.0) * 100.0));
		return {root, fill};
	}

	constexpr std::string_view ProbeButtonClasses[] = {"probe-button"};
	constexpr UI::WidgetAttribute ProbeButtonAttributes[] = {
		{"text", UI::WidgetAttributeType::String},
	};

	UI::WidgetResult MakeProbeButton(UI::Context& context, UI::ElementHandle parent, const UI::WidgetArgs&) {
		UI::ElementHandle root = context.CreateElement();
		context.AppendChild(parent, root);
		context.AddClass(root, StringHash("probe-button"));
		context.SetUserTag(root, static_cast<u64>(PROBE_MAGIC));
		return {root, {}};
	}

	struct ProbeSettings {
		SLT_REFLECT_VALUE(ProbeSettings)
		SLT_REFLECT_CONSTRUCTIBLE()
		SLT_REFLECT_FIELD(threshold)
		SLT_REFLECT_FIELD(profile)
#ifdef SLT_HOTRELOAD_V2
		SLT_REFLECT_FIELD(verbose)
#endif
		SLT_END_REFLECT()

		i32 threshold = 4;
		str profile = PROBE_MAGIC == 2 ? "probe-v2" : "probe-v1";
#ifdef SLT_HOTRELOAD_V2
		bool verbose = false;
#endif
	};

	ProbeSettings* ProbeCategory() {
		return Settings::DeclareCategory<ProbeSettings>("HotReloadProbe", SettingScope::Project);
	}

	struct FrameStageProbe {
		u32 runs = 0;
		f32 lastAlpha = 0.0f;
		FrameTime lastFrame;
	};

	FrameStageProbe _frameStageProbe;

	void OnProbeStage(const FrameStageArgs&, void* user) {
		auto* probe = static_cast<FrameStageProbe*>(user);
		++probe->runs;
		probe->lastAlpha = Time::GetFixedAlpha();
		probe->lastFrame = Time::GetFrame();
	}

	class ProbeFramePass final : public FramePass {
	public:
		void Declare(FramePassPlan&) override {}
		void Record(ComputeContext&) override {}
	};
	ProbeFramePass _framePassProbe;

	class ProbeImporter : public IAssetImporter {
	public:
		StringHash GetAssetType() const override { return StringHash("HotReloadProbeAsset"); }
		Asset* Import(const Path&) override { return nullptr; }
		u32 GetImporterVersion() const override { return PROBE_MAGIC; }
	};

	ProbeImporter _probeImporter;

#ifndef SLT_HOTRELOAD_GONE

	class HotReloadDataAsset : public Asset {
	public:
		SLT_REFLECT_CLASS(HotReloadDataAsset)
		SLT_REFLECT_PARENT(Asset)
		SLT_REFLECT_CREATABLE()
		SLT_REFLECT_FIELD(damage)
		SLT_REFLECT_FIELD(label)
		SLT_REFLECT_FIELD(holder)
		SLT_REFLECT_FIELD(dependency)
		SLT_REFLECT_FIELD(reference)
#ifdef SLT_HOTRELOAD_V2
		SLT_REFLECT_FIELD(tier)
#endif
		SLT_REFLECT_METHOD(GetAssetMagic)
		SLT_REFLECT_METHOD(GetResolvedDependencyId)
		SLT_REFLECT_METHOD(RebuildResolvedDependency)
		SLT_END_REFLECT()

		i32 damage = 7;
		str label = "probe";

#ifdef SLT_HOTRELOAD_V2
		i32 tier = 0;
#endif

		Handle<Component> holder;

		Guid dependency;
		Handle<Asset> resolvedDependency;

		Handle<TextAsset> reference;

		u64 GetResolvedDependencyId() const { return resolvedDependency.GetInstanceId(); }

		void RebuildResolvedDependency() {
			resolvedDependency = Handle<Asset>(AssetDatabase::FindLoadedByGuid(dependency));
		}

		virtual i32 GetAssetMagic() const { return PROBE_MAGIC; }

		StringHash GetAssetTypeId() const override { return HotReloadDataAsset::GetReflectType()->name; }

		bool Import(const Path& sourcePath) override {
			return TextSerializer::Deserialize(this, GetReflectType(), FileSystem::ReadTextFile(sourcePath, MemTag::Asset));
		}

		bool Export(const Path& destPath) const override {
			return FileSystem::WriteTextFile(destPath, TextSerializer::Serialize(this, GetReflectType()));
		}
	protected:
		void OnLoad() override {
			if (!dependency.IsValid()) {
				return;
			}
			if (Asset* loaded = AssetDatabase::LoadAssetByGuid(dependency)) {
				resolvedDependency = Handle<Asset>(loaded);
			}
		}

		void OnUnload() override {
			if (Asset* held = resolvedDependency.Get()) {
				AssetDatabase::UnloadAsset(held);
			}
			resolvedDependency = {};
		}
	};

	class ProbeDataAssetImporter : public IAssetImporter {
	public:
		StringHash GetAssetType() const override { return StringHash("HotReloadDataAsset"); }

		Asset* Import(const Path& sourcePath) override {
			auto* asset = New<HotReloadDataAsset>(MemTag::Asset);
			if (!asset->Import(sourcePath)) {
				Delete(asset);
				return nullptr;
			}
			return asset;
		}

		bool CanCreateAsset() const override { return true; }
		std::string_view GetCreateMenuLabel() const override { return "Hot Reload Data"; }
		std::string_view GetCreateDefaultFileName() const override { return "New Hot Reload Data.hrdata"; }
		Asset* CreateAsset() override { return New<HotReloadDataAsset>(MemTag::Asset); }

		u32 GetImporterVersion() const override { return PROBE_MAGIC; }
	};

	ProbeDataAssetImporter _probeDataAssetImporter;

	class HotReloadDeclaredAsset : public DataAsset {
	public:
		SLT_REFLECT_CLASS(HotReloadDeclaredAsset)
		SLT_REFLECT_PARENT(DataAsset)
		SLT_CREATE_ASSET_MENU("Hot Reload Declared", ".hrdeclared")
		SLT_REFLECT_FIELD(charges)
		SLT_END_REFLECT()

		i32 charges = 3;
	};

#endif

	void RegisterCoreExtensions(const void* owner) {
		UI::WidgetRegistry::Register(
			UI::WidgetDesc{"hot-reload-gauge", &MakeProbeGauge, GaugeAttributes, GaugeClasses}, owner);
		UI::WidgetRegistry::Register(
			UI::WidgetDesc{"button", &MakeProbeButton, ProbeButtonAttributes, ProbeButtonClasses}, owner);

		_frameStageProbe = FrameStageProbe{};
		FrameStages::Register(FrameStage::Update, FrameStagePosition::Normal, &OnProbeStage, &_frameStageProbe, owner);
		FramePasses::Register(_framePassProbe, owner);

		AssetImporterRegistry::Register(StringHash("HotReloadProbeAsset"), &_probeImporter, {".hotreloadprobe"}, owner);
		AssetImporterRegistry::RegisterExtension(".hotreloadprobe2", &_probeImporter, owner);

#ifndef SLT_HOTRELOAD_GONE
		AssetImporterRegistry::Register(StringHash("HotReloadDataAsset"), &_probeDataAssetImporter, {".hrdata"}, owner);
#endif

		ProbeCategory();
	}

#ifdef SLT_WITH_EDITOR

	class HotReloadEditorPanel : public EditorPanel {
	public:
		SLT_REFLECT_CLASS(HotReloadEditorPanel)
		SLT_REFLECT_PARENT(EditorPanel)
		SLT_REFLECT_CREATABLE()
		SLT_EDITOR_PANEL_NAME("Hot Reload Probe")
		SLT_REFLECT_FIELD(note)
		SLT_REFLECT_FIELD(tickCount)
		SLT_REFLECT_METHOD(GetPanelMagic)
		SLT_END_REFLECT()

		HotReloadEditorPanel() = default;

		str note;
		i32 tickCount = 0;

		i32 GetPanelMagic() const {
#ifdef SLT_HOTRELOAD_V2
			return 2;
#else
			return 1;
#endif
		}

		void OnBuildContent(UI::Context& context, UI::ElementHandle root) override {
			UI::ElementHandle line = context.CreateElement();
			context.AppendChild(root, line);
			context.SetUserTag(line, static_cast<u64>(GetPanelMagic()));
		}

		void OnTick(f32) override { ++tickCount; }
	};

	class HotReloadBuildTarget : public IBuildTarget {
	public:
		std::string_view GetId() const override { return "HotReloadProbe"; }
		std::string_view GetDisplayName() const override { return "Hot Reload Probe Console"; }

		bool IsAvailable(str&) const override { return true; }

		void DescribeSettings(Vector<BuildSettingDesc>& out) const override {
			BuildSettingDesc desc;
			desc.key = "signingIdentity";
			desc.type = BuildSettingType::String;
			desc.label = "Signing Identity";
			desc.help = "the certificate the probe's post-build step signs with";
			desc.defaultValue = PROBE_MAGIC == 2 ? "probe-v2" : "probe-v1";
			out.push_back(std::move(desc));
		}

		void Validate(const str&, const ResolvedProfile&, Vector<BuildIssue>&) const override {}

		void ConfigurePipeline(BuildContext& context) const override {
			context.entrySource = "PlayerMain.cpp";
			context.defines.push_back("SLT_HOTRELOAD_PROBE");
		}

		str GetLaunchCommand(const BuildContext&) const override { return {}; }
	};

	const HotReloadBuildTarget _ProbeTarget;

	BuildStepStatus ProbeSignStep(BuildContext& context) {
		if (!context.target || context.target->GetId() != "HotReloadProbe") {
			return BuildStepStatus::Succeeded;
		}

		str identity;
		if (const ResolvedSetting* setting = context.profile->Find("signingIdentity")) {
			identity = setting->value;
		}

		std::error_code ec;
		std::filesystem::create_directories(std::filesystem::path{context.stagingDir}, ec);
		std::ofstream file(std::filesystem::path{context.stagingDir} / "Signature.txt", std::ios::binary | std::ios::trunc);
		if (!file.is_open()) {
			context.AddIssue(BuildIssueSeverity::Error, "signingIdentity",
							 "the probe could not write a signature into " + context.stagingDir);
			return BuildStepStatus::Failed;
		}
		file << "signed by v" << PROBE_MAGIC << " as " << identity << "\n";
		context.Log("hot-reload probe signed the artifact");
		return BuildStepStatus::Succeeded;
	}

	void RegisterEditorExtensions(const void* owner) {
		BuildTargetRegistry::Register(&_ProbeTarget, owner);
		BuildStepRegistry::Register("hot-reload-sign", BuildPhase::PostBuild, &ProbeSignStep, owner);

		SettingsCategoryPresentation page;
		page.label = "Hot Reload Probe";
		page.tooltip = "settings a module declared, to prove one can";
		page.keywords = "probe module";
		page.order = 900;
		SettingsPresentation::RegisterCategory("HotReloadProbe", std::move(page), owner);

		SettingsFieldPresentation threshold;
		threshold.label = "Probe Threshold";
		threshold.keywords = PROBE_MAGIC == 2 ? "probe-v2" : "probe-v1";
		SettingsPresentation::RegisterField("HotReloadProbe", "threshold", std::move(threshold), owner);
	}

#endif

} // namespace HotReloadModuleImpl

#ifndef SLT_HOTRELOAD_GONE
SLT_REGISTER_TYPE(HotReloadModuleImpl::HotReloadBehavior)
SLT_REGISTER_TYPE(HotReloadModuleImpl::HotReloadDataAsset)
SLT_REGISTER_TYPE(HotReloadModuleImpl::HotReloadDeclaredAsset)
#endif
SLT_IMPLEMENT_CORE_EXTENSIONS(&HotReloadModuleImpl::RegisterCoreExtensions)

extern "C" SLT_API_EXPORT Shalltear::i32 HotReloadModuleGetProbeThreshold() {
	const HotReloadModuleImpl::ProbeSettings* live = HotReloadModuleImpl::ProbeCategory();
	return live ? live->threshold : -1;
}

extern "C" SLT_API_EXPORT Shalltear::u32 HotReloadModuleGetFrameStageRuns() {
	return HotReloadModuleImpl::_frameStageProbe.runs;
}
extern "C" SLT_API_EXPORT Shalltear::f32 HotReloadModuleGetFrameStageAlpha() {
	return HotReloadModuleImpl::_frameStageProbe.lastAlpha;
}

extern "C" SLT_API_EXPORT Shalltear::f32 HotReloadModuleGetClockDeltaTime() {
	return HotReloadModuleImpl::_frameStageProbe.lastFrame.deltaTime;
}
extern "C" SLT_API_EXPORT Shalltear::f32 HotReloadModuleGetClockUnscaledDeltaTime() {
	return HotReloadModuleImpl::_frameStageProbe.lastFrame.unscaledDeltaTime;
}
extern "C" SLT_API_EXPORT double HotReloadModuleGetClockTime() {
	return HotReloadModuleImpl::_frameStageProbe.lastFrame.time;
}
extern "C" SLT_API_EXPORT Shalltear::u64 HotReloadModuleGetClockFrameCount() {
	return HotReloadModuleImpl::_frameStageProbe.lastFrame.frameCount;
}

extern "C" SLT_API_EXPORT void HotReloadModuleSetTimeScale(float scale) {
	Shalltear::Time::SetTimeScale(scale);
}
#ifdef SLT_WITH_EDITOR
SLT_REGISTER_TYPE(HotReloadModuleImpl::HotReloadEditorPanel)
SLT_IMPLEMENT_EDITOR_EXTENSIONS(&HotReloadModuleImpl::RegisterEditorExtensions)
#endif

#ifdef SLT_HOTRELOAD_BAD

SLT_DETAIL_MODULE_DLLMAIN()
extern "C" SLT_API_EXPORT ::Shalltear::ShalltearModuleAPI* ShalltearModuleGetApi(::Shalltear::u32) {
	static ::Shalltear::ShalltearModuleAPI api{
		0xDEADBEEFu,
		SLT_DETAIL_MODULE_FLAGS,
		SLT_MODULE_ABI_HASH,
		"HotReloadModuleBad",
		+[](const ::Shalltear::EngineContext*) -> bool { return false; },
		+[]() {},
		+[]() {}};
	return &api;
}

#else

SLT_IMPLEMENT_MODULE(HotReloadModule)

#endif
