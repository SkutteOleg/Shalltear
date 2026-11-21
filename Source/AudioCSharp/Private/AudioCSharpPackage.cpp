#include "AudioBindings.h"
#include "PackageBindings.h"
#include "ScriptRuntime.h"

#include "Shalltear/Audio/AudioEffect.h"
#include "Shalltear/Audio/AudioSpatializer.h"

#include "Shalltear/Core/Module/ModuleAPI.h"

namespace Shalltear::AudioCSharpImpl {

	namespace {

		constexpr const char* BINDINGS_NAME = "Audio";

		SltAudioBindings _table{};

		void SweepScripts(const void* owner) {
			AudioEffects::UnregisterOwnedBy(owner);
			AudioSpatializers::UnregisterOwnedBy(owner);
		}

	} // anonymous namespace

	void RegisterPackage(const void*) {
		CSharpImpl::Generated::FillAudioTable(_table);
		CSharpImpl::PackageBindings::Add(BINDINGS_NAME, &_table, static_cast<i32>(sizeof(_table)), SLT_AUDIO_LAYOUT_HASH);
		CSharpImpl::ScriptRuntime::AddOwnerSweep(&SweepScripts);
	}

	void ShutdownPackage() {
		CSharpImpl::ScriptRuntime::RemoveOwnerSweep(&SweepScripts);
		SweepScripts(CSharpImpl::ScriptRuntime::GetScriptsOwner());
		CSharpImpl::AudioBindings::Release();
		CSharpImpl::PackageBindings::Remove(BINDINGS_NAME);
	}

} // namespace Shalltear::AudioCSharpImpl

SLT_IMPLEMENT_PACKAGE_WITH_SHUTDOWN(AudioCSharp, &Shalltear::AudioCSharpImpl::RegisterPackage, &Shalltear::AudioCSharpImpl::ShutdownPackage)
