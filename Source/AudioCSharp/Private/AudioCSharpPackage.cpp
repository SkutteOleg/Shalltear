#include "AudioBindings.h"
#include "PackageBindings.h"

#include "Shalltear/Core/Module/ModuleAPI.h"

namespace Shalltear::AudioCSharpImpl {

	namespace {

		constexpr const char* BINDINGS_NAME = "Audio";

		SltAudioBindings _table{};

	} // anonymous namespace

	void RegisterPackage(const void*) {
		CSharpImpl::Generated::FillAudioTable(_table);
		CSharpImpl::PackageBindings::Add(BINDINGS_NAME, &_table, static_cast<i32>(sizeof(_table)), SLT_AUDIO_LAYOUT_HASH);
	}

	void ShutdownPackage() {
		CSharpImpl::AudioBindings::Release();
		CSharpImpl::PackageBindings::Remove(BINDINGS_NAME);
	}

} // namespace Shalltear::AudioCSharpImpl

SLT_IMPLEMENT_PACKAGE_WITH_SHUTDOWN(AudioCSharp, &Shalltear::AudioCSharpImpl::RegisterPackage, &Shalltear::AudioCSharpImpl::ShutdownPackage)
