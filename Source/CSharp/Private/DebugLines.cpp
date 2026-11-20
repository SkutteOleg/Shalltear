#include "DebugLines.h"

#include "Shalltear/Core/Time.h"

#include <algorithm>

namespace Shalltear::CSharpImpl::DebugLines {

#ifdef SLT_WITH_EDITOR
	namespace {

		struct Held {
			DebugLine line;
			u64 frame = 0;
			f64 until = 0.0;
			bool depthTested = true;
		};

		Vector<Held>& GetHeld() {
			static Vector<Held> held{GetMemoryResource(MemTag::Scripting)};
			return held;
		}

	} // anonymous namespace

	void Draw(const Vec3& from, const Vec3& to, const Color& color, f32 seconds, bool depthTested) noexcept {
		GetHeld().push_back(Held{DebugLine{from, to, color.ToColor32()}, Time::GetFrameCount(),
								 Time::GetRealtimeSinceStartup() + std::max(0.0, static_cast<f64>(seconds)), depthTested});
	}

	void Collect(Vector<DebugLine>& lines, Vector<DebugLine>& overlayLines) {
		for (const Held& held : GetHeld()) {
			(held.depthTested ? lines : overlayLines).push_back(held.line);
		}
	}

	void EndFrame() noexcept {
		const u64 frame = Time::GetFrameCount();
		const f64 now = Time::GetRealtimeSinceStartup();
		std::erase_if(GetHeld(), [&](const Held& held) { return held.frame < frame && held.until < now; });
	}

	u32 GetCount() noexcept {
		return static_cast<u32>(GetHeld().size());
	}

	void Clear() noexcept {
		GetHeld() = Vector<Held>{GetMemoryResource(MemTag::Scripting)};
	}
#else
	void Draw(const Vec3&, const Vec3&, const Color&, f32, bool) noexcept {}

	void Collect(Vector<DebugLine>&, Vector<DebugLine>&) {}

	void EndFrame() noexcept {}

	u32 GetCount() noexcept {
		return 0;
	}

	void Clear() noexcept {}
#endif

} // namespace Shalltear::CSharpImpl::DebugLines
