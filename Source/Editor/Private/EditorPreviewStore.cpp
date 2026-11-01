#include "Shalltear/Editor/EditorPreviews.h"

#include "PreviewPaneRegistry.h"

#include "Shalltear/Core/Asset/Asset.h"
#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Graphics/FullScreenPass.h"
#include "Shalltear/Core/Graphics/RHIDevice.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Scene/Scene.h"
#include "Shalltear/Core/Scene/SceneSystem.h"
#include "Shalltear/Core/StringUtil.h"
#include "Shalltear/Core/Time.h"

#include "Shalltear/Editor/EditorAssetWatch.h"
#include "Shalltear/Editor/EditorTheme.h"

#include <algorithm>

namespace Shalltear {

	namespace {

		constexpr u32 DEFAULT_CAPACITY = 288;

		constexpr f32 DEFAULT_BUDGET_MS = 4.0f;

		struct StoreEntry {
			RHI::TextureHandle texture;
			Vec2I size{};
			UI::SamplerKind sampler = UI::SamplerKind::LinearClamp;
			str detail;
			u64 bytes = 0;
			u64 lastUsed = 0;
			bool declined = false;
			bool stale = false;
			u64 revision = 0;
			Vector<Guid> dependencies{GetMemoryResource(MemTag::Editor)};
		};

		struct InFlight {
			bool active = false;
			bool superseded = false;
			Guid guid;
			Handle<Scene> stage;
			Asset* asset = nullptr;
			UI::SamplerKind sampler = UI::SamplerKind::LinearClamp;
			str detail;
			Vector<Guid> dependencies{GetMemoryResource(MemTag::Editor)};
		};

		struct StoreState {
			HashMap<Guid, StoreEntry> entries{GetMemoryResource(MemTag::Editor)};
			Vector<Guid> queue{GetMemoryResource(MemTag::Editor)};
			HashSet<Guid> queued{GetMemoryResource(MemTag::Editor)};
			InFlight inFlight;
			FullScreenPass row;

			ViewTargetId target = INVALID_VIEW_TARGET;
			u32 capacity = DEFAULT_CAPACITY;
			f32 budgetMs = DEFAULT_BUDGET_MS;
			u64 clock = 0;
			u64 revisions = 0;
			u64 bytes = 0;
			bool warnedUncacheable = false;

			Vector<u8> scratch{GetMemoryResource(MemTag::Editor)};
			Vector<u8> readback{GetMemoryResource(MemTag::Editor)};
		};

		StoreState* _state = nullptr;

		i32 EntrySidePx() {
			f32 side = EditorTheme::GetMetric(EditorTheme::MetricToken::PreviewRenderSize);
			return std::max(1, static_cast<i32>(side));
		}

		f32 ElapsedMs(u64 startTicks) {
			u64 perSecond = Time::GetTicksPerSecond();
			if (perSecond == 0) {
				return 0.0f;
			}
			return static_cast<f32>(static_cast<f64>(Time::GetTicks() - startTicks) * 1000.0 /
									static_cast<f64>(perSecond));
		}

		void DestroyEntryTexture(StoreEntry& entry) {
			if (!entry.texture.IsValid()) {
				return;
			}
			if (RHI::Device* device = RenderSystem::GetDevice()) {
				device->DestroyResource(entry.texture);
			}
			entry.texture = {};
		}

		void EraseEntry(HashMap<Guid, StoreEntry>::iterator it) {
			_state->bytes -= it->second.bytes;
			DestroyEntryTexture(it->second);
			_state->entries.erase(it);
		}

		void EvictToCapacity() {
			while (_state->entries.size() > _state->capacity) {
				auto oldest = _state->entries.begin();
				for (auto it = _state->entries.begin(); it != _state->entries.end(); ++it) {
					if (it->second.lastUsed < oldest->second.lastUsed) {
						oldest = it;
					}
				}
				EraseEntry(oldest);
			}
		}

		void Insert(const Guid& guid, StoreEntry&& entry) {
			auto existing = _state->entries.find(guid);
			if (existing != _state->entries.end()) {
				entry.revision = existing->second.revision;
				EraseEntry(existing);
			}
			if (entry.stale) {
				entry.revision = ++_state->revisions;
			}
			entry.lastUsed = ++_state->clock;
			_state->bytes += entry.bytes;
			_state->entries.emplace(guid, std::move(entry));
			EvictToCapacity();
		}

		void RecordDeclined(const Guid& guid, str detail = {}, bool stale = false, std::span<const Guid> dependencies = {}) {
			StoreEntry entry;
			entry.dependencies.assign(dependencies.begin(), dependencies.end());
			entry.declined = true;
			entry.stale = stale;
			entry.detail = std::move(detail);
			Insert(guid, std::move(entry));
		}

		void StoreImage(const Guid& guid, std::span<const u8> pixels, Vec2I pixelsSize, RHI::Format pixelsFormat,
						UI::SamplerKind sampler, str detail, bool stale = false, std::span<const Guid> dependencies = {}) {
			i32 side = EntrySidePx();
			Vec2I entrySize{side, side};
			RHI::Format uploadFormat =
				EditorPreviewOps::ResampleToRGBA8(pixels, pixelsSize, pixelsFormat, entrySize, _state->scratch);
			if (uploadFormat == RHI::Format::Unknown) {
				RecordDeclined(guid, std::move(detail), stale, dependencies);
				return;
			}

			StoreEntry entry;
			entry.dependencies.assign(dependencies.begin(), dependencies.end());
			entry.stale = stale;
			entry.size = entrySize;
			entry.sampler = sampler;
			entry.detail = std::move(detail);
			entry.bytes = static_cast<u64>(entrySize.x) * static_cast<u64>(entrySize.y) * 4u;

			if (RHI::Device* device = RenderSystem::GetDevice()) {
				RHI::TextureDesc desc;
				desc.size = entrySize;
				desc.format = uploadFormat;
				desc.usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::TransferDst;
				desc.debugName = "PreviewStore";
				entry.texture = device->CreateTextureResource(desc);
				if (entry.texture.IsValid()) {
					device->UploadTexture(entry.texture, _state->scratch);
				}
			}
			Insert(guid, std::move(entry));
		}

		void ReleaseInFlight() {
			if (_state->inFlight.stage.IsValid()) {
				SceneSystem::UnloadScene(_state->inFlight.stage);
			}
			if (_state->inFlight.asset) {
				AssetDatabase::UnloadAsset(_state->inFlight.asset);
			}
			_state->inFlight = {};
		}

		void CompleteInFlight() {
			if (!_state->inFlight.active) {
				return;
			}
			Vec2I size{};
			if (RenderSystem::ReadbackViewTarget(_state->target, _state->readback, size) && size.x > 0 &&
				size.y > 0) {
				// UNORM, the target is already display encoded
				StoreImage(_state->inFlight.guid, _state->readback, size, RHI::Format::RGBA8Unorm,
						   _state->inFlight.sampler, std::move(_state->inFlight.detail), _state->inFlight.superseded,
						   _state->inFlight.dependencies);
			} else {
				RecordDeclined(_state->inFlight.guid, {}, _state->inFlight.superseded, _state->inFlight.dependencies);
			}
			ReleaseInFlight();
		}

		bool IsPending(const Guid& guid) {
			if (_state->queued.contains(guid)) {
				return true;
			}
			return _state->inFlight.active && _state->inFlight.guid == guid && !_state->inFlight.superseded;
		}

		void Enqueue(const Guid& guid) {
			if (!IsPending(guid)) {
				_state->queued.insert(guid);
				_state->queue.push_back(guid);
			}
		}

		bool StartOne(const Guid& guid) {
			const Path path = AssetDatabase::LookupPath(guid);
			if (StringUtil::ToLower(path.GetExtension()) == ".cs") {
				RecordDeclined(guid);
				return false;
			}
			const StringHash type = EditorPreviewsInternal::AssetTypeForFileName(path.ToString());
			if (type != StringHash{} && !EditorPreviewsInternal::HasProducer(type)) {
				RecordDeclined(guid);
				return false;
			}
			Asset* asset = AssetDatabase::LoadAssetByGuid(guid);
			if (!asset) {
				RecordDeclined(guid);
				return false;
			}

			Handle<Scene> stage = SceneSystem::CreateIsolatedScene("PreviewStage");
			if (_state->target == INVALID_VIEW_TARGET) {
				i32 side = EntrySidePx();
				_state->target = RenderSystem::CreateViewTarget({side, side});
			}

			AssetPreviewRequest request;
			request.guid = guid;
			request.asset = asset;
			request.stage = stage;
			request.target = _state->target;
			request.targetSize =
				_state->target == INVALID_VIEW_TARGET ? Vec2I{} : RenderSystem::GetViewTargetSize(_state->target);
			_state->row.material = {};
			request.row = &_state->row;

			AssetPreviewImage image;
			bool produced = EditorPreviews::Produce(request, image);

			auto giveUp = [&](bool declined) {
				SceneSystem::UnloadScene(stage);
				AssetDatabase::UnloadAsset(asset);
				if (declined) {
					RecordDeclined(guid);
				}
			};

			if (!produced) {
				giveUp(true);
				return false;
			}
			if (!image.pixels.empty()) {
				// consumed before the asset it points into is released
				StoreImage(guid, image.pixels, image.size, image.pixelsFormat, image.sampler, std::move(image.detail), false,
						   image.dependencies);
				giveUp(false);
				return false;
			}
			if (_state->target != INVALID_VIEW_TARGET && image.texture.IsValid() &&
				image.texture == RenderSystem::GetViewTargetTexture(_state->target)) {
				_state->inFlight = {true, false, guid, stage, asset, image.sampler, std::move(image.detail)};
				_state->inFlight.dependencies = std::move(image.dependencies);
				return true;
			}

			if (!_state->warnedUncacheable) {
				_state->warnedUncacheable = true;
				SLT_LOG_WARN("Editor",
							 "EditorPreviewStore: a producer answered with a texture the store can't own. "
							 "Render into the given target or supply AssetPreviewImage::pixels");
			}
			giveUp(true);
			return false;
		}

		void ApplySourceChanges() {
			if (EditorAssetWatch::DidOverflowThisFrame()) {
				EditorPreviewStore::InvalidateAll();
				return;
			}
			for (const Path& changed : EditorAssetWatch::GetChangedThisFrame()) {
				Guid guid = AssetDatabase::LookupGuid(changed);
				if (guid.IsValid()) {
					EditorPreviewStore::Invalidate(guid);
				}
			}
		}

	} // anonymous namespace

	void EditorPreviewStore::Initialize() {
		if (!_state) {
			_state = New<StoreState>(MemTag::Editor);
		}
	}

	void EditorPreviewStore::Terminate() {
		if (!_state) {
			return;
		}
		ReleaseInFlight();
		for (auto it = _state->entries.begin(); it != _state->entries.end(); ++it) {
			DestroyEntryTexture(it->second);
		}
		_state->entries.clear();
		_state->bytes = 0;
		if (_state->target != INVALID_VIEW_TARGET) {
			RenderSystem::DestroyViewTarget(_state->target);
			_state->target = INVALID_VIEW_TARGET;
		}
		Delete(_state);
		_state = nullptr;
	}

	bool EditorPreviewStore::Request(const Guid& guid, AssetPreviewEntry& out) {
		out = {};
		if (!_state || !guid.IsValid()) {
			return false;
		}
		auto it = _state->entries.find(guid);
		if (it != _state->entries.end()) {
			it->second.lastUsed = ++_state->clock;
			if (it->second.stale) {
				Enqueue(guid);
			}
			out.revision = it->second.revision;
			if (it->second.declined) {
				return false;
			}
			out.texture = it->second.texture;
			out.size = it->second.size;
			out.sampler = it->second.sampler;
			out.detail = it->second.detail;
			return true;
		}
		Enqueue(guid);
		return false;
	}

	bool EditorPreviewStore::Peek(const Guid& guid, AssetPreviewEntry& out) {
		out = {};
		if (!_state || !guid.IsValid()) {
			return false;
		}
		auto it = _state->entries.find(guid);
		if (it == _state->entries.end()) {
			return false;
		}
		out.revision = it->second.revision;
		if (it->second.declined) {
			return false;
		}
		out.texture = it->second.texture;
		out.size = it->second.size;
		out.sampler = it->second.sampler;
		out.detail = it->second.detail;
		return true;
	}

	void EditorPreviewStore::Drain() {
		if (!_state) {
			return;
		}
		ApplySourceChanges();
		CompleteInFlight();

		u64 start = Time::GetTicks();
		u32 started = 0;
		while (!_state->queue.empty()) {
			if (started > 0 && ElapsedMs(start) >= _state->budgetMs) {
				break;
			}
			Guid guid = _state->queue.front();
			_state->queue.erase(_state->queue.begin());
			_state->queued.erase(guid);
			++started;
			if (StartOne(guid)) {
				break;
			}
		}
	}

	void EditorPreviewStore::SetCapacity(u32 previews) {
		if (_state) {
			_state->capacity = previews;
			EvictToCapacity();
		}
	}

	u32 EditorPreviewStore::GetCapacity() {
		return _state ? _state->capacity : 0;
	}

	void EditorPreviewStore::SetFrameBudgetMs(f32 milliseconds) {
		if (_state) {
			_state->budgetMs = std::max(0.0f, milliseconds);
		}
	}

	f32 EditorPreviewStore::GetFrameBudgetMs() {
		return _state ? _state->budgetMs : 0.0f;
	}

	u32 EditorPreviewStore::GetCachedCount() {
		return _state ? static_cast<u32>(_state->entries.size()) : 0;
	}

	u64 EditorPreviewStore::GetCachedBytes() {
		return _state ? _state->bytes : 0;
	}

	u32 EditorPreviewStore::GetPendingCount() {
		if (!_state) {
			return 0;
		}
		return static_cast<u32>(_state->queue.size()) + (_state->inFlight.active ? 1u : 0u);
	}

	void EditorPreviewStore::Invalidate(const Guid& guid) {
		if (!_state) {
			return;
		}
		auto it = _state->entries.find(guid);
		if (it != _state->entries.end()) {
			it->second.stale = true;
			it->second.revision = ++_state->revisions;
		}
		if (_state->inFlight.active && _state->inFlight.guid == guid) {
			_state->inFlight.superseded = true;
		}
		auto reads = [&guid](const Vector<Guid>& dependencies) {
			return std::find(dependencies.begin(), dependencies.end(), guid) != dependencies.end();
		};
		for (auto entry = _state->entries.begin(); entry != _state->entries.end(); ++entry) {
			if (reads(entry->second.dependencies)) {
				entry->second.stale = true;
				entry->second.revision = ++_state->revisions;
			}
		}
		if (_state->inFlight.active && reads(_state->inFlight.dependencies)) {
			_state->inFlight.superseded = true;
		}
	}

	void EditorPreviewStore::InvalidateAll() {
		if (!_state) {
			return;
		}
		if (_state->inFlight.active) {
			_state->inFlight.superseded = true;
		}
		const u64 revision = ++_state->revisions;
		for (auto it = _state->entries.begin(); it != _state->entries.end(); ++it) {
			it->second.stale = true;
			it->second.revision = revision;
		}
	}

} // namespace Shalltear
