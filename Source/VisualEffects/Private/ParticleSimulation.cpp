#include "ParticleSimulation.h"

#include "ParticleKernels.h"

#include "Shalltear/Core/Asset/AssetDatabase.h"
#include "Shalltear/Core/Graphics/Camera.h"
#include "Shalltear/Core/Graphics/MaterialAsset.h"
#include "Shalltear/Core/Graphics/MeshAsset.h"
#include "Shalltear/Core/Graphics/RHIDevice.h"
#include "Shalltear/Core/Graphics/RenderSystem.h"
#include "Shalltear/Core/Graphics/ShaderAsset.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Profiler.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace Shalltear::VisualEffectsImpl {

	namespace {

		struct ParticleFrame {
			u32 capacity = 0;
			u32 batchSlots = 1;
			u32 slots = 0;
			u32 firstSlot = 0;
			u32 attributeCount = 0;
			u32 outputCount = 0;
			u32 updateGroupSize = 0;
			u32 batchFresh = 0;
			u64 instances = 0;
			f32 sortViewX = 0.0f;
			f32 sortViewY = 0.0f;
			f32 sortViewZ = 0.0f;
			f32 sortSign = 1.0f;
			f32 sortForwardX = 0.0f;
			f32 sortForwardY = 0.0f;
			f32 sortForwardZ = 0.0f;
			u32 sortCount = 0;
			u32 stripCount = 0;
			u32 particlesPerStrip = 0;
			u32 parentCapacity = 0;
			u32 events = 0;
		};
		static_assert(sizeof(ParticleFrame) == 88, "the push block is ParticleSystem.hlsli's layout");

		struct ParticleInstanceRecord {
			u32 flags = 0;
			u32 parity = 0;
			u32 spawnCount = 0;
			f32 deltaTime = 0.0f;
			u32 frame = 0;
			u32 drawTable = 0;
			u64 effect = 0;
		};
		static_assert(sizeof(ParticleInstanceRecord) == 32, "a slot's record is ParticleSystem.hlsli's layout");
		constexpr u32 INSTANCE_ADVANCES = 1;
		constexpr u32 INSTANCE_FRESH = 2;
		constexpr u32 INSTANCE_CLAIMED = 4;

		struct ParticleOutputFrame {
			u64 lists = 0;
			u64 attributes = 0;
			u32 capacity = 0;
			u32 list = 0;
			u32 vertices = 0;
			u32 frame = 0;
			u64 effect = 0;
			u64 order = 0;
			u32 pass = 0;
			u32 particlesPerStrip = 0;
			u32 value = 0;
			u32 reserved = 0;
		};
		static_assert(sizeof(ParticleOutputFrame) == 64, "the push block is ParticleOutput.hlsli's layout");

		struct ParticleSortFrame {
			u32 count = 0;
			u32 size = 0;
			u32 source = 0;
			u32 last = 0;
			u32 order = 0;
		};
		static_assert(sizeof(ParticleSortFrame) == 20, "the push block is ParticleSortPass.hlsli's layout");

		// must match ParticleSort.hlsli and ParticleSortPass.hlsli
		constexpr u32 SORT_LOCAL = 1024;
		constexpr u32 SORT_THREADS = 256;
		constexpr u32 SORT_MAX_GROUPS = 32768;

		u32 SortCountFor(u32 capacity) {
			u32 count = 1;
			while (count < capacity) {
				count <<= 1;
			}
			return count;
		}

		constexpr u64 LISTS_HEADER_WORDS = 4;
		constexpr u64 COUNT_BYTES = LISTS_HEADER_WORDS * 4;
		constexpr u64 DISPATCH_ARGUMENT_WORDS = 3;
		constexpr u64 DRAW_ARGUMENT_WORDS = 4;
		constexpr u64 DRAW_ARGUMENTS_OFFSET = DISPATCH_ARGUMENT_WORDS * 4;
		constexpr u32 DRAW_TABLE_ENTRY_BYTES = 8;
		constexpr u32 DRAW_PER_PARTICLE = 0;
		constexpr u32 DRAW_INSTANCED = 1;
		constexpr u32 DRAW_SEGMENTS = 2;
		// must match the layout in ParticleSystem.hlsli
		constexpr u64 STRIP_WORDS = 6;
		constexpr u64 EVENT_HEADER_WORDS = 2;
		constexpr u64 BOUNDS_BYTES = 24;
		constexpr u32 BOUNDS_EMPTY_LOW = 0xFFFFFFFFu;

		u64 BoundsOffset(const ParticleSystemDesc& desc, u32 which) {
			const u64 events = desc.parent != nullptr ? EVENT_HEADER_WORDS + desc.capacity : 0ull;
			return (LISTS_HEADER_WORDS + 3ull * desc.capacity + STRIP_WORDS * desc.strips + events) * 4 + which * BOUNDS_BYTES;
		}

		u64 ListsStride(const ParticleSystemDesc& desc) { return BoundsOffset(desc, 2); }
		u64 ArgumentsStride(const ParticleSystemDesc& desc) {
			return (DISPATCH_ARGUMENT_WORDS + DRAW_ARGUMENT_WORDS * std::max<size_t>(desc.outputs.size(), 1) + (desc.parent ? DISPATCH_ARGUMENT_WORDS : 0ull)) * 4;
		}
		u64 AttributesStride(const ParticleSystemDesc& desc) { return static_cast<u64>(desc.capacity) * desc.attributeCount * 4; }

		bool Shares(const ParticleSystemDesc& desc) {
			return desc.batch.owner != 0 && desc.batchSlots > 1 && !desc.parent && desc.initializeEvents.empty() && desc.updateEvents.empty();
		}

		f32 FromBoundsKey(u32 key) {
			return std::bit_cast<f32>(key ^ ((key >> 31) != 0 ? 0x80000000u : 0xFFFFFFFFu));
		}

		ParticleSimulation* _simulation = nullptr;
		const void* _owner = nullptr;

		bool Counted(bool recorded, u32& count) {
			count += recorded ? 1u : 0u;
			return recorded;
		}

		bool IsUsableKernel(const Handle<ShaderAsset>& kernel) {
			const ShaderAsset* shader = kernel.Get();
			return shader && shader->IsKernel();
		}

		u32 GroupsFor(u32 threads, const Handle<ShaderAsset>& kernel) {
			const u32 width = static_cast<u32>(std::max(1, kernel.Get()->threadGroupSize.x));
			return (threads + width - 1) / width;
		}

		ParticleFrame BatchFrame(const ParticleBatch& batch) {
			const ParticleSystemDesc& desc = batch.desc;
			ParticleFrame frame;
			frame.capacity = desc.capacity;
			frame.batchSlots = batch.slots;
			frame.slots = batch.watermark;
			frame.attributeCount = desc.attributeCount;
			frame.outputCount = static_cast<u32>(desc.outputs.size());
			const ShaderAsset* update = desc.update.Get();
			frame.updateGroupSize = static_cast<u32>(std::max(1, update ? update->threadGroupSize.x : 1));
			frame.stripCount = desc.strips;
			frame.particlesPerStrip = desc.strips > 0 ? desc.capacity / desc.strips : 0u;
			const ParticleSystem* parent = desc.parent && desc.parent->IsCreated() ? desc.parent : nullptr;
			frame.parentCapacity = parent ? parent->GetCapacity() : 0u;
			frame.events = desc.parent ? 1u : 0u;
			return frame;
		}

		constexpr u64 FRAME_HEADER_BYTES = 96;

		u32 DrawTableAt(size_t size) {
			return static_cast<u32>((std::max<u64>(FRAME_HEADER_BYTES, size) + 15) & ~u64{15});
		}

		u64 UploadFrame(RHI::Device& device, std::span<const u8> bytes, std::span<const u32> table) {
			const u64 at = DrawTableAt(bytes.size());
			const u64 size = at + table.size_bytes();
			const RHI::TransientAllocation allocation = device.AllocateTransient(size, 16);
			if (!allocation.IsValid()) {
				return 0;
			}
			std::memset(allocation.cpu, 0, size);
			if (!bytes.empty()) {
				std::memcpy(allocation.cpu, bytes.data(), bytes.size());
			}
			if (!table.empty()) {
				std::memcpy(static_cast<u8*>(allocation.cpu) + at, table.data(), table.size_bytes());
			}
			return device.GetBufferDeviceAddress(allocation.buffer) + allocation.offset;
		}

		u32 MeshIndexCount(const Handle<MeshAsset>& held) {
			const MeshAsset* mesh = held.Get();
			return mesh && mesh->indexBufferAddress != 0 && mesh->vertexBufferAddress != 0 ? static_cast<u32>(mesh->indices.size()) : 0u;
		}

		Handle<Camera> MainCamera() {
			Handle<Camera> best;
			i32 priority = 0;
			for (Handle<Camera> handle : RenderSystem::GetCameras()) {
				const Camera* camera = handle.Get();
				if (camera && (!best.IsValid() || camera->GetPriority() > priority)) {
					best = handle;
					priority = camera->GetPriority();
				}
			}
			return best;
		}

		void WriteCameraRecord(std::span<u8> frame, u32 offset, bool world, const CameraDepth& depth) {
			if (frame.size() < offset + 4u * 64u + 16u) {
				return;
			}
			Mat4 systemToView = depth.view;
			if (!world && frame.size() >= 96) {
				f32 rows[16];
				std::memcpy(rows, frame.data() + 32, sizeof(rows));
				const Mat4 localToWorld =
					Mat4::FromRows(Vec4(rows[0], rows[1], rows[2], rows[3]), Vec4(rows[4], rows[5], rows[6], rows[7]),
								   Vec4(rows[8], rows[9], rows[10], rows[11]), Vec4(rows[12], rows[13], rows[14], rows[15]));
				systemToView = depth.view * localToWorld;
			}
			const Mat4 matrices[4] = {systemToView, systemToView.Inverse(), depth.projection, depth.projection.Inverse()};
			f32* out = reinterpret_cast<f32*>(frame.data() + offset);
			for (const Mat4& matrix : matrices) {
				for (u32 row = 0; row < 4; ++row) {
					for (u32 column = 0; column < 4; ++column) {
						*out++ = matrix(row, column);
					}
				}
			}
			const bool drawn = depth.size.x > 0 && depth.size.y > 0;
			const f32 size[4] = {static_cast<f32>(depth.size.x), static_cast<f32>(depth.size.y), drawn ? 1.0f : 0.0f,
								 depth.projection(3, 3) > 0.5f ? 1.0f : 0.0f};
			std::memcpy(out, size, sizeof(size));
		}

		void WriteMeshRecord(std::span<u8> frame, u32 offset, const Handle<MeshAsset>& held) {
			if (frame.size() < static_cast<size_t>(offset) + 32) {
				return;
			}
			u64 addresses[2] = {};
			u32 counts[2] = {};
			if (const MeshAsset* mesh = held.Get()) {
				if (mesh->vertexBufferAddress != 0) {
					addresses[0] = mesh->vertexBufferAddress;
					counts[0] = static_cast<u32>(mesh->vertices.size());
				}
				if (mesh->indexBufferAddress != 0) {
					addresses[1] = mesh->indexBufferAddress;
					counts[1] = static_cast<u32>(mesh->indices.size());
				}
			}
			std::memcpy(frame.data() + offset, addresses, sizeof(addresses));
			std::memcpy(frame.data() + offset + sizeof(addresses), counts, sizeof(counts));
		}

		Handle<ShaderAsset> LoadKernel(const Guid& guid) {
			Asset* asset = AssetDatabase::LoadAssetByGuid(guid);
			if (asset && asset->GetAssetTypeId() == StringHash("ShaderAsset") && static_cast<ShaderAsset*>(asset)->IsKernel()) {
				return Handle<ShaderAsset>(static_cast<ShaderAsset*>(asset));
			}
			if (asset) {
				AssetDatabase::UnloadAsset(asset);
			}
			return {};
		}

	} // anonymous namespace

	ParticleSystem::~ParticleSystem() {
		Release();
	}

	bool ParticleSystem::Create(const ParticleSystemDesc& desc) {
		Release();
		RHI::Device* device = RenderSystem::GetDevice();
		if (!device) {
			SLT_LOG_WARN("VisualEffects", "a particle system wasn't created (nothing renders)");
			return false;
		}
		if (desc.capacity == 0 || desc.attributeCount == 0) {
			SLT_LOG_WARN("VisualEffects", "a particle system wasn't created (capacity + attribute count should be above 0)");
			return false;
		}
		if (desc.cpu && (desc.strips > 0 || desc.parent || !desc.initializeEvents.empty() || !desc.updateEvents.empty())) {
			SLT_LOG_WARN("VisualEffects", "a particle system wasn't created (a system the CPU simulates holds no strips, parent or children)");
			return false;
		}
		if (!desc.cpu && (!IsUsableKernel(desc.initialize) || !IsUsableKernel(desc.update))) {
			SLT_LOG_WARN("VisualEffects", "a particle system wasn't created (its initialize or update shader is unavailable or no kernel)");
			return false;
		}
		if (desc.strips > 0 && desc.capacity % desc.strips != 0) {
			SLT_LOG_WARN("VisualEffects", "a particle system wasn't created (its {} strips don't divide its capacity of {})", desc.strips, desc.capacity);
			return false;
		}
		if (desc.strips > 0 && !GetSimulation().AcquireStripKernel()) {
			SLT_LOG_WARN("VisualEffects", "a particle system wasn't created (the package's strip kernel didn't load)");
			return false;
		}
		if (desc.parent && (desc.parent == this || !desc.parent->IsCreated())) {
			SLT_LOG_WARN("VisualEffects", "a particle system wasn't created (the system its GPU event comes from isn't made)");
			return false;
		}

		if (desc.cpu) {
			_feedSlotBytes = (LISTS_HEADER_WORDS + desc.capacity + static_cast<u64>(desc.capacity) * desc.attributeCount) * 4;
			RHI::BufferDesc feed;
			feed.size = FEED_SLOTS * _feedSlotBytes;
			feed.usage = RHI::BufferUsage::TransferSrc;
			feed.access = RHI::MemoryAccess::CPUToGPU;
			feed.debugName = "ParticleFeed";
			_feed = device->CreateBufferResource(feed);
			if (!_feed.IsValid()) {
				SLT_LOG_WARN("VisualEffects", "a particle system of capacity {} couldn't allocate its feed", desc.capacity);
				Release();
				return false;
			}
		}
		if (!GetSimulation().Claim(*this, desc)) {
			SLT_LOG_WARN("VisualEffects", "a particle system of capacity {} couldn't allocate its buffers", desc.capacity);
			Release();
			return false;
		}

		_orderOffsets.assign(desc.outputs.size(), NO_ORDER);
		_ordered.assign(desc.outputs.size(), 0);
		u32 sorted = 0;
		for (size_t o = 0; o < desc.outputs.size(); ++o) {
			if (desc.outputs[o].sort != ParticleSort::None) {
				_orderOffsets[o] = sorted++ * SortCountFor(desc.capacity);
			}
		}
		if (sorted > 0) {
			_sortCount = SortCountFor(desc.capacity);
			RHI::BufferDesc order;
			order.size = static_cast<u64>(sorted) * _sortCount * 4;
			order.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::DeviceAddress | RHI::BufferUsage::TransferSrc;
			order.debugName = "ParticleOrder";
			RHI::BufferDesc keys;
			keys.size = static_cast<u64>(_sortCount > SORT_LOCAL ? 2 : 1) * _sortCount * 8;
			keys.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::TransferSrc;
			keys.debugName = "ParticleSortKeys";
			_order = device->CreateBufferResource(order);
			_sortKeys = device->CreateBufferResource(keys);
			if (!_order.IsValid() || !_sortKeys.IsValid()) {
				SLT_LOG_WARN("VisualEffects", "a particle system of capacity {} couldn't allocate its sort's buffers", desc.capacity);
				Release();
				return false;
			}
			_orderAddress = device->GetBufferDeviceAddress(_order);
		}

		_desc = desc;
		_frame = 0;
		_parity = 0;
		_advanced = false;
		_fresh = true;
		GetSimulation().Add(*this);
		return true;
	}

	void ParticleSystem::Release() {
		if (_simulation) {
			_simulation->Remove(*this);
		}
		if (_batch) {
			if (_simulation) {
				_simulation->Leave(*this);
			} else {
				ParticleBatch* batch = _batch;
				batch->members[_slot] = nullptr;
				if (--batch->claimed == 0) {
					batch->Free();
					Delete(batch);
				}
			}
			_batch = nullptr;
			_slot = 0;
		}
		if (RHI::Device* device = RenderSystem::GetDevice()) {
			for (RHI::BufferHandle buffer : {_order, _sortKeys, _feed}) {
				if (buffer.IsValid()) {
					device->DestroyResource(buffer);
				}
			}
		}
		_countRing.Release();
		_order = {};
		_sortKeys = {};
		_feed = {};
		_feedSlotBytes = 0;
		_fedSlot = 0;
		_fedAlive = 0;
		_fed = false;
		_orderAddress = 0;
		_sortCount = 0;
		_orderOffsets.clear();
		_ordered.clear();
		_listsReady = false;
		_counts = {};
		_watched = false;
		_bounds = {};
		_boundsFrom = ParticleReadback::NO_FRAME;
		_updates.clear();
		_updateBlocks.clear();
		_declared.clear();
		_declaredBlocks.clear();
		_advanced = false;
		_fresh = false;
	}

	RHI::BufferHandle ParticleSystem::GetListsBuffer() const { return _batch ? _batch->lists : RHI::BufferHandle{}; }
	RHI::BufferHandle ParticleSystem::GetArgumentsBuffer() const { return _batch ? _batch->arguments : RHI::BufferHandle{}; }
	RHI::BufferHandle ParticleSystem::GetAttributesBuffer() const { return _batch ? _batch->attributes : RHI::BufferHandle{}; }
	u64 ParticleSystem::GetListsOffset() const { return _batch ? _batch->listsStride * _slot : 0ull; }
	u64 ParticleSystem::GetArgumentsOffset() const { return _batch ? _batch->argumentsStride * _slot : 0ull; }
	u64 ParticleSystem::GetAttributesOffset() const { return _batch ? _batch->attributesStride * _slot : 0ull; }
	u64 ParticleSystem::SlotBytes(const ParticleSystemDesc& desc) { return ListsStride(desc) + BOUNDS_BYTES + ArgumentsStride(desc) + AttributesStride(desc); }

	bool ParticleBatch::Make(const ParticleSystemDesc& made, u32 count) {
		Free();
		RHI::Device* device = RenderSystem::GetDevice();
		if (!device || count == 0) {
			return false;
		}
		desc = made;
		slots = count;
		listsStride = ListsStride(made);
		argumentsStride = ArgumentsStride(made);
		attributesStride = AttributesStride(made);
		RHI::BufferDesc listsDesc;
		listsDesc.size = listsStride * count + BOUNDS_BYTES * count;
		listsDesc.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::DeviceAddress | RHI::BufferUsage::TransferSrc;
		listsDesc.debugName = "ParticleLists";
		RHI::BufferDesc argumentsDesc;
		argumentsDesc.size = argumentsStride * count + DISPATCH_ARGUMENT_WORDS * 4;
		argumentsDesc.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::Indirect | RHI::BufferUsage::TransferSrc;
		argumentsDesc.debugName = "ParticleArguments";
		RHI::BufferDesc attributesDesc;
		attributesDesc.size = attributesStride * count;
		attributesDesc.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::DeviceAddress | RHI::BufferUsage::TransferSrc;
		attributesDesc.debugName = "ParticleAttributes";
		if (made.cpu) {
			listsDesc.usage = listsDesc.usage | RHI::BufferUsage::TransferDst;
			attributesDesc.usage = attributesDesc.usage | RHI::BufferUsage::TransferDst;
		}
		lists = device->CreateBufferResource(listsDesc);
		arguments = device->CreateBufferResource(argumentsDesc);
		attributes = device->CreateBufferResource(attributesDesc);
		const bool measured = made.measuresBounds && !made.cpu;
		if (!lists.IsValid() || !arguments.IsValid() || !attributes.IsValid() || (measured && !boundsRing.Make(BOUNDS_BYTES * count, "ParticleBounds"))) {
			Free();
			return false;
		}
		listsAddress = device->GetBufferDeviceAddress(lists);
		attributesAddress = device->GetBufferDeviceAddress(attributes);
		members.assign(count, nullptr);
		claimed = 0;
		watermark = 0;
		fresh = true;
		return true;
	}

	void ParticleBatch::Free() {
		if (RHI::Device* device = RenderSystem::GetDevice()) {
			for (RHI::BufferHandle buffer : {lists, arguments, attributes}) {
				if (buffer.IsValid()) {
					device->DestroyResource(buffer);
				}
			}
		}
		boundsRing.Release();
		lists = {};
		arguments = {};
		attributes = {};
		listsAddress = 0;
		attributesAddress = 0;
		members.clear();
		claimed = 0;
		watermark = 0;
		rounds.clear();
		standing = 0;
	}

	bool ParticleBatch::Fits(const ParticleSystemDesc& other) const {
		return other.capacity == desc.capacity && other.strips == desc.strips && other.attributeCount == desc.attributeCount &&
			   other.outputs.size() == desc.outputs.size() && other.cpu == desc.cpu && other.measuresBounds == desc.measuresBounds &&
			   other.initialize == desc.initialize && other.update == desc.update && !other.parent && !desc.parent;
	}

	bool ParticleReadback::Make(u64 size, const char* debugName) {
		Release();
		RHI::Device* device = RenderSystem::GetDevice();
		RHI::BufferDesc desc;
		desc.size = SLOTS * size;
		desc.usage = RHI::BufferUsage::TransferDst;
		desc.access = RHI::MemoryAccess::Readback;
		desc.debugName = debugName;
		ring = device ? device->CreateBufferResource(desc) : RHI::BufferHandle{};
		bytes = ring.IsValid() ? size : 0;
		return ring.IsValid();
	}

	// slot FRAMES_IN_FLIGHT old has finished on the device
	const u8* ParticleReadback::TakeNewest(u64 frame) {
		RHI::Device* device = RenderSystem::GetDevice();
		const auto* mapped = device && ring.IsValid() ? static_cast<const u8*>(device->GetMappedPointer(ring)) : nullptr;
		if (!mapped) {
			return nullptr;
		}
		u32 newest = SLOTS;
		for (u32 slot = 0; slot < SLOTS; ++slot) {
			const u64 written = frames[slot];
			if (written == NO_FRAME || written + RHI::FRAMES_IN_FLIGHT > frame || (any && written <= read)) {
				continue;
			}
			if (newest == SLOTS || written > frames[newest]) {
				newest = slot;
			}
		}
		if (newest == SLOTS) {
			return nullptr;
		}
		read = frames[newest];
		any = true;
		return mapped + newest * bytes;
	}

	void ParticleReadback::Forget() {
		frames.fill(NO_FRAME);
		read = 0;
		any = false;
	}

	void ParticleReadback::Release() {
		if (RHI::Device* device = RenderSystem::GetDevice(); device && ring.IsValid()) {
			device->DestroyResource(ring);
		}
		ring = {};
		bytes = 0;
		Forget();
	}

	void ParticleSystem::WatchCounts(bool watch) {
		_watched = watch && IsCreated() && !_desc.cpu;
		if (_watched && !_countRing.ring.IsValid()) {
			_watched = _countRing.Make(COUNT_BYTES, "ParticleCounts");
		}
	}

	void ParticleSystem::ReadCounts(u64 frame) {
		const auto* counts = reinterpret_cast<const u32*>(_countRing.TakeNewest(frame));
		if (!counts) {
			return;
		}
		_counts.alive = counts[0] + counts[1];
		_counts.dropped = counts[3];
		_counts.known = true;
	}

	void ParticleSystem::ReadBounds(const u8* boxes, u64 frame) {
		if (!boxes || _boundsFrom == ParticleReadback::NO_FRAME || frame < _boundsFrom) {
			return;
		}
		const auto* keys = reinterpret_cast<const u32*>(boxes + static_cast<u64>(_slot) * BOUNDS_BYTES);
		_bounds.known = true;
		_bounds.box = AABB::Empty();
		if (keys[0] != BOUNDS_EMPTY_LOW) {
			_bounds.box = AABB(Vec3(FromBoundsKey(keys[0]), FromBoundsKey(keys[1]), FromBoundsKey(keys[2])),
							   Vec3(FromBoundsKey(keys[3]), FromBoundsKey(keys[4]), FromBoundsKey(keys[5])));
		}
	}

	void ParticleSystem::Advance(f32 deltaTime, u32 spawnCount) {
		_advanced = true;
		if (_desc.cpu) {
			return;
		}
		Update update;
		update.deltaTime = deltaTime;
		update.spawnCount = spawnCount;
		update.block = static_cast<u32>(_updateBlocks.size());
		update.size = static_cast<u32>(_frameData.size());
		_updateBlocks.insert(_updateBlocks.end(), _frameData.begin(), _frameData.end());
		_updates.push_back(update);
	}

	void ParticleSystem::BeginUpdates() {
		_updates.clear();
		_updateBlocks.clear();
	}

	void ParticleSystem::Feed(std::span<const u32> alive, std::span<const u32> attributes, u32 dropped) {
		RHI::Device* device = RenderSystem::GetDevice();
		const u64 words = static_cast<u64>(_desc.capacity) * _desc.attributeCount;
		if (!IsCreated() || !_desc.cpu || !device || !_feed.IsValid() || alive.size() > _desc.capacity || attributes.size() != words) {
			return;
		}
		_fedSlot = static_cast<u32>((GetSimulation()._frame + 1) % FEED_SLOTS);
		_fedAlive = static_cast<u32>(alive.size());
		u32 header[LISTS_HEADER_WORDS] = {};
		header[1 - _parity] = _fedAlive;
		header[2] = _desc.capacity - _fedAlive;
		header[3] = dropped;
		const u64 slotOffset = static_cast<u64>(_fedSlot) * _feedSlotBytes;
		if (device->GetCapabilities().persistentBufferMapping) {
			u8* ring = static_cast<u8*>(device->GetMappedPointer(_feed));
			if (!ring) {
				return;
			}
			u8* slot = ring + slotOffset;
			std::memcpy(slot, header, sizeof(header));
			if (!alive.empty()) {
				std::memcpy(slot + sizeof(header), alive.data(), alive.size_bytes());
				std::memcpy(slot + sizeof(header) + static_cast<u64>(_desc.capacity) * 4, attributes.data(), attributes.size_bytes());
			}
		} else {
			device->UploadBuffer(_feed, {reinterpret_cast<const u8*>(header), sizeof(header)}, slotOffset);
			if (!alive.empty()) {
				device->UploadBuffer(_feed, {reinterpret_cast<const u8*>(alive.data()), alive.size_bytes()}, slotOffset + sizeof(header));
				device->UploadBuffer(_feed, {reinterpret_cast<const u8*>(attributes.data()), attributes.size_bytes()},
									 slotOffset + sizeof(header) + static_cast<u64>(_desc.capacity) * 4);
			}
		}
		_fed = true;
		_advanced = true;
		_counts = {_fedAlive, dropped, true};
	}

	void ParticleSystem::SetFrameData(std::span<const u8> bytes) {
		_frameData.assign(bytes.begin(), bytes.end());
	}

	bool ParticleSystem::DeclareOutput(ProceduralPlan& plan, const Vec3& sortPoint, u32 output) const {
		if (!IsCreated() || output >= _desc.outputs.size() || !_desc.outputs[output].material.IsValid()) {
			return false;
		}
		plan.AddItem(_desc.outputs[output].material, sortPoint, _desc.outputs[output].castShadows ? ShadowCasting::On : ShadowCasting::Off);
		plan.Read(_batch->lists, RHI::ResourceState::ShaderResource);
		plan.Read(_batch->attributes, RHI::ResourceState::ShaderResource);
		plan.Read(_batch->arguments, RHI::ResourceState::IndirectArgument);
		if (_orderOffsets[output] != NO_ORDER) {
			plan.Read(_order, RHI::ResourceState::ShaderResource);
		}
		return true;
	}

	bool ParticleSystem::RecordOutput(ProceduralContext& context, u32 output) const {
		if (!IsCreated() || output >= _desc.outputs.size()) {
			return false;
		}
		ParticleOutputFrame frame;
		frame.lists = _batch->listsAddress + GetListsOffset();
		frame.attributes = _batch->attributesAddress + GetAttributesOffset();
		frame.capacity = _desc.capacity;
		frame.list = _parity;
		frame.vertices = _desc.outputs[output].vertices;
		frame.frame = _frame;
		frame.effect = _effectAddress;
		frame.pass = static_cast<u32>(context.GetPass());
		frame.particlesPerStrip = _desc.strips > 0 ? _desc.capacity / _desc.strips : 0u;
		frame.value = context.GetValue();
		if (_ordered[output] != 0 && context.GetPass() != ProceduralPass::Shadow) {
			frame.order = _orderAddress + static_cast<u64>(_orderOffsets[output]) * 4;
		}
		const bool drawn = context.DrawIndirect({reinterpret_cast<const u8*>(&frame), sizeof(frame)}, _batch->arguments,
												GetArgumentsOffset() + DRAW_ARGUMENTS_OFFSET + DRAW_ARGUMENT_WORDS * 4 * output);
		return Counted(drawn, GetSimulation()._frameCounts.draws);
	}

	bool ParticleSystem::Sorts(u32 output, ParticleSort when) const {
		return IsCreated() && output < _desc.outputs.size() && _desc.outputs[output].sort == when && _orderOffsets[output] != NO_ORDER &&
			   IsUsableKernel(_desc.outputs[output].sortKernel) && GetSimulation().AcquireSortKernels();
	}

	u32 ParticleSystem::SortSteps() const {
		u32 steps = 2;
		for (u32 run = SORT_LOCAL; run < _sortCount; run <<= 1) {
			++steps;
		}
		return steps;
	}

	template <typename Plan>
	void ParticleSystem::DeclareSortStep(Plan& plan, u32 step) const {
		constexpr RHI::ResourceState STORAGE = RHI::ResourceState::UnorderedAccess;
		if (step == 0) {
			plan.Read(_batch->lists, STORAGE);
			plan.Read(_batch->attributes, STORAGE);
			plan.Write(_sortKeys, STORAGE);
		} else if (step + 1 == SortSteps()) {
			plan.Read(_sortKeys, STORAGE);
			plan.Write(_order, STORAGE);
		} else {
			plan.Write(_sortKeys, STORAGE);
			plan.Read(_order, STORAGE);
		}
	}

	void ParticleSystem::DeclareViewSorts(ProceduralPlan& plan, Vector<ParticleSortStep>& steps) {
		for (u32 o = 0; o < _desc.outputs.size(); ++o) {
			if (_desc.outputs[o].sort != ParticleSort::View) {
				continue;
			}
			_ordered[o] = 0;
			if (!_listsReady || !Sorts(o, ParticleSort::View)) {
				continue;
			}
			for (u32 step = 0; step < SortSteps(); ++step) {
				plan.AddViewStep();
				DeclareSortStep(plan, step);
				steps.push_back({o, step});
			}
			_ordered[o] = 1;
		}
	}

	bool ParticleSystem::RecordSort(ComputeContext& context, u32 output, u32 step, const Vec3& viewPosition, const Vec3& viewForward) const {
		if (!IsCreated() || output >= _desc.outputs.size() || _orderOffsets[output] == NO_ORDER) {
			return false;
		}
		const ParticleOutputDesc& drawn = _desc.outputs[output];
		const ParticleSimulation& simulation = GetSimulation();
		if (step == 0) {
			ParticleFrame frame = BatchFrame(*_batch);
			frame.slots = 1;
			frame.firstSlot = _slot;
			frame.instances = _batch->standing;
			frame.sortViewX = viewPosition.x;
			frame.sortViewY = viewPosition.y;
			frame.sortViewZ = viewPosition.z;
			frame.sortSign = drawn.sortSign;
			frame.sortForwardX = viewForward.x;
			frame.sortForwardY = viewForward.y;
			frame.sortForwardZ = viewForward.z;
			frame.sortCount = _sortCount;
			const ComputeBinding bindings[3] = {{0, _batch->lists}, {1, _batch->attributes}, {2, _sortKeys}};
			const bool keyed = context.Dispatch(drawn.sortKernel, bindings, {}, {reinterpret_cast<const u8*>(&frame), sizeof(frame)},
												std::min((_sortCount + SORT_THREADS - 1) / SORT_THREADS, SORT_MAX_GROUPS));
			return Counted(keyed, GetSimulation()._frameCounts.dispatches);
		}
		ParticleSortFrame frame;
		frame.count = _sortCount;
		frame.last = step + 1 == SortSteps() ? 1u : 0u;
		frame.order = _orderOffsets[output];
		const ComputeBinding bindings[2] = {{0, _sortKeys}, {1, _order}};
		const std::span<const u8> push{reinterpret_cast<const u8*>(&frame), sizeof(frame)};
		if (step == 1) {
			return Counted(context.Dispatch(simulation._sortLocal, bindings, {}, push, (_sortCount + SORT_LOCAL - 1) / SORT_LOCAL), GetSimulation()._frameCounts.dispatches);
		}
		const u32 merge = step - 2;
		frame.size = SORT_LOCAL << merge;
		frame.source = merge & 1u;
		const bool merged = context.Dispatch(simulation._sortMerge, bindings, {}, push, std::min((_sortCount + SORT_THREADS - 1) / SORT_THREADS, SORT_MAX_GROUPS));
		return Counted(merged, GetSimulation()._frameCounts.dispatches);
	}

	void ParticleSimulation::Add(ParticleSystem& system) {
		if (std::find(_systems.begin(), _systems.end(), &system) == _systems.end()) {
			_systems.push_back(&system);
		}
	}

	void ParticleSimulation::Remove(ParticleSystem& system) {
		std::erase(_systems, &system);
	}

	bool ParticleSimulation::Claim(ParticleSystem& system, const ParticleSystemDesc& desc) {
		const bool shares = Shares(desc);
		ParticleBatch* batch = nullptr;
		if (shares) {
			for (ParticleBatch* held : _batches) {
				if (held->key == desc.batch && held->claimed < held->slots && held->Fits(desc)) {
					batch = held;
					break;
				}
			}
		}
		if (!batch) {
			batch = New<ParticleBatch>(MemTag::Graphics);
			if (!batch->Make(desc, shares ? desc.batchSlots : 1u)) {
				Delete(batch);
				return false;
			}
			batch->key = shares ? desc.batch : ParticleBatchKey{};
			_batches.push_back(batch);
		}
		u32 slot = 0;
		while (batch->members[slot] != nullptr) {
			++slot;
		}
		batch->members[slot] = &system;
		++batch->claimed;
		batch->watermark = std::max(batch->watermark, slot + 1);
		system._batch = batch;
		system._slot = slot;
		return true;
	}

	void ParticleSimulation::Leave(ParticleSystem& system) {
		ParticleBatch* batch = system._batch;
		if (!batch) {
			return;
		}
		batch->members[system._slot] = nullptr;
		--batch->claimed;
		while (batch->watermark > 0 && batch->members[batch->watermark - 1] == nullptr) {
			--batch->watermark;
		}
		if (batch->claimed > 0) {
			return;
		}
		batch->emptiedAt = _frame;
		if (batch->orphan) {
			batch->Free();
			Delete(batch);
		}
	}

	void ParticleSimulation::FreeEmptyBatches() {
		std::erase_if(_batches, [this](ParticleBatch* batch) {
			if (batch->claimed > 0 || (batch->key.owner != 0 && _frame - batch->emptiedAt < _batchEmptyLifetime)) {
				return false;
			}
			batch->Free();
			Delete(batch);
			return true;
		});
	}

	void ParticleSimulation::FreeBatches() {
		for (ParticleBatch* batch : _batches) {
			if (batch->claimed == 0) {
				batch->Free();
				Delete(batch);
			} else {
				batch->orphan = true;
			}
		}
		_batches.clear();
	}

	void ParticleSimulation::RestoreDeviceContent() {
		RHI::Device* device = RenderSystem::GetDevice();
		if (!device) {
			return;
		}
		const auto restart = [device](ParticleBatch& batch) {
			batch.listsAddress = device->GetBufferDeviceAddress(batch.lists);
			batch.attributesAddress = device->GetBufferDeviceAddress(batch.attributes);
			batch.boundsRing.Forget();
			batch.fresh = true;
		};
		for (ParticleBatch* batch : _batches) {
			restart(*batch);
		}
		for (ParticleSystem* system : _systems) {
			if (!system->_batch) {
				continue;
			}
			restart(*system->_batch);
			system->_orderAddress = system->_order.IsValid() ? device->GetBufferDeviceAddress(system->_order) : 0;
			std::fill(system->_ordered.begin(), system->_ordered.end(), u8{0});
			system->_listsReady = false;
			system->_countRing.Forget();
			system->_counts = {};
			system->_bounds = {};
			system->_boundsFrom = ParticleReadback::NO_FRAME;
			system->_fed = false;
			system->_frame = 0;
			system->_parity = 0;
			system->_fresh = true;
		}
	}

	bool ParticleSimulation::AcquireKernels() {
		if (IsUsableKernel(_reset) && IsUsableKernel(_counts)) {
			return true;
		}
		for (Handle<ShaderAsset>* kernel : {&_reset, &_counts}) {
			if (ShaderAsset* shader = kernel->Get()) {
				AssetDatabase::UnloadAsset(shader);
			}
			*kernel = {};
		}
		_reset = LoadKernel(ParticleResetKernel);
		_counts = LoadKernel(ParticleArgumentsKernel);
		if (IsUsableKernel(_reset) && IsUsableKernel(_counts)) {
			return true;
		}
		if (!_warnedNoKernels) {
			_warnedNoKernels = true;
			SLT_LOG_WARN("VisualEffects", "the package's reset and arguments kernels didn't load");
		}
		return false;
	}

	bool ParticleSimulation::AcquireSortKernels() {
		if (IsUsableKernel(_sortLocal) && IsUsableKernel(_sortMerge)) {
			return true;
		}
		for (Handle<ShaderAsset>* kernel : {&_sortLocal, &_sortMerge}) {
			if (ShaderAsset* shader = kernel->Get()) {
				AssetDatabase::UnloadAsset(shader);
			}
			*kernel = {};
		}
		_sortLocal = LoadKernel(ParticleSortLocalKernel);
		_sortMerge = LoadKernel(ParticleSortMergeKernel);
		if (IsUsableKernel(_sortLocal) && IsUsableKernel(_sortMerge)) {
			return true;
		}
		if (!_warnedNoSortKernels) {
			_warnedNoSortKernels = true;
			SLT_LOG_WARN("VisualEffects", "the package's sort kernels didn't load");
		}
		return false;
	}

	bool ParticleSimulation::AcquireStripKernel() {
		if (IsUsableKernel(_strips)) {
			return true;
		}
		if (ShaderAsset* shader = _strips.Get()) {
			AssetDatabase::UnloadAsset(shader);
		}
		_strips = LoadKernel(ParticleStripsKernel);
		return IsUsableKernel(_strips);
	}

	void ParticleSimulation::ReleaseKernels() {
		for (Handle<ShaderAsset>* kernel : {&_reset, &_counts, &_sortLocal, &_sortMerge, &_strips}) {
			if (ShaderAsset* shader = kernel->Get()) {
				AssetDatabase::UnloadAsset(shader);
			}
			*kernel = {};
		}
	}

	void ParticleSimulation::Declare(FramePassPlan& plan) {
		SLT_PROFILE_ZONE("Render.Effects.Simulate");
		_steps.clear();
		++_frame;
		_mainCamera = MainCamera();
		FreeEmptyBatches();
		_frameCounts = {};
		for (ParticleSystem* system : _systems) {
			system->ReadCounts(_frame);
			if (system->_counts.known) {
				_frameCounts.alive += system->_counts.alive;
				_frameCounts.dropped += system->_counts.dropped;
			}
		}
		for (ParticleBatch* batch : _batches) {
			const u8* boxes = batch->boundsRing.ring.IsValid() ? batch->boundsRing.TakeNewest(_frame) : nullptr;
			for (ParticleSystem* member : batch->members) {
				if (member && boxes) {
					member->ReadBounds(boxes, batch->boundsRing.read);
				}
			}
		}
		constexpr size_t STEADY_UPDATES = 16;
		u32 rounds = 0;
		bool anyWork = false;
		for (ParticleSystem* system : _systems) {
			system->_declared.swap(system->_updates);
			system->_declaredBlocks.swap(system->_updateBlocks);
			system->_updates.clear();
			system->_updateBlocks.clear();
			if (system->_updates.capacity() > STEADY_UPDATES) {
				Vector<ParticleSystem::Update>(GetMemoryResource(MemTag::Graphics)).swap(system->_updates);
				Vector<u8>(GetMemoryResource(MemTag::Graphics)).swap(system->_updateBlocks);
			}
			system->_rounds = system->_desc.cpu ? (system->_advanced && system->_fed ? 1u : 0u) : static_cast<u32>(system->_declared.size());
			rounds = std::max(rounds, system->_rounds);
			anyWork = anyWork || system->_fresh || system->_advanced;
		}
		const bool simulate = anyWork && AcquireKernels();
		if (simulate) {
			for (ParticleBatch* batch : _batches) {
				DeclareReset(plan, *batch);
			}
			for (u32 round = 0; round < std::max(rounds, 1u); ++round) {
				for (ParticleBatch* batch : _batches) {
					if (!batch->desc.cpu) {
						DeclareRound(plan, *batch, round);
					} else if (round == 0) {
						DeclareFed(plan, *batch);
					}
				}
			}
		}
		for (ParticleSystem* system : _systems) {
			if (!simulate) {
				system->_rounds = 0;
			}
			if (system->_rounds > 0 && system->_boundsFrom == ParticleReadback::NO_FRAME) {
				system->_boundsFrom = _frame;
			}
			system->_advanced = false;
		}
		for (ParticleSystem* system : _systems) {
			if (!system->_watched || (system->_fresh && !simulate)) {
				continue;
			}
			_steps.push_back({nullptr, system, StepKind::Counts});
			plan.AddStep();
			plan.Read(system->_batch->lists, RHI::ResourceState::TransferSrc);
			plan.Write(system->_countRing.ring, RHI::ResourceState::TransferDst);
		}
		for (ParticleBatch* batch : _batches) {
			bool simulated = false;
			for (const ParticleSystem* member : batch->members) {
				simulated = simulated || (member && member->_rounds > 0);
			}
			_frameCounts.batches += simulated ? 1u : 0u;
			if (!simulated || !batch->boundsRing.ring.IsValid()) {
				continue;
			}
			_steps.push_back({batch, nullptr, StepKind::Bounds});
			plan.AddStep();
			plan.Read(batch->lists, RHI::ResourceState::TransferSrc);
			plan.Write(batch->boundsRing.ring, RHI::ResourceState::TransferDst);
		}
		if (RHI::Device* device = RenderSystem::GetDevice()) {
			Vector<u32> table{GetMemoryResource(MemTag::Graphics)};
			for (ParticleSystem* system : _systems) {
				const ParticleSystemDesc& desc = system->_desc;
				table.clear();
				for (const ParticleOutputDesc& output : desc.outputs) {
					u32 vertices = output.material.IsValid() ? output.vertices : 0u;
					if (output.mesh != ParticleOutputDesc::NO_MESH) {
						vertices = output.mesh < desc.meshes.size() && output.material.IsValid() ? MeshIndexCount(desc.meshes[output.mesh].mesh) : 0u;
					}
					if (output.strip) {
						vertices *= desc.capacity;
					}
					table.push_back(vertices);
					table.push_back(output.instanced ? DRAW_INSTANCED : (output.strip ? DRAW_SEGMENTS : DRAW_PER_PARTICLE));
				}
				const std::span<u8> blocks{system->_declaredBlocks.data(), system->_declaredBlocks.size()};
				for (ParticleSystem::Update& update : system->_declared) {
					const std::span<u8> block = blocks.subspan(update.block, update.size);
					for (const ParticleMesh& mesh : desc.meshes) {
						WriteMeshRecord(block, mesh.offset, mesh.mesh);
					}
					update.drawTable = DrawTableAt(block.size());
					update.effect = UploadFrame(*device, block, table);
				}
				if (!system->_declared.empty()) {
					const ParticleSystem::Update& last = system->_declared.back();
					system->_frameData.assign(blocks.begin() + last.block, blocks.begin() + last.block + last.size);
					system->_drawTable = last.drawTable;
					system->_effectAddress = last.effect;
					continue;
				}
				for (const ParticleMesh& mesh : desc.meshes) {
					WriteMeshRecord(system->_frameData, mesh.offset, mesh.mesh);
				}
				system->_drawTable = DrawTableAt(system->_frameData.size());
				system->_effectAddress = UploadFrame(*device, system->_frameData, table);
			}
			WriteTables(*device);
		}
	}

	void ParticleSimulation::WriteTables(RHI::Device& device) {
		for (ParticleBatch* batch : _batches) {
			batch->rounds.clear();
			batch->standing = 0;
			if (batch->watermark == 0) {
				continue;
			}
			u32 rounds = 0;
			for (const ParticleSystem* member : batch->members) {
				rounds = std::max(rounds, member ? member->_rounds : 0u);
			}
			const u64 bytes = static_cast<u64>(batch->watermark) * sizeof(ParticleInstanceRecord);
			for (u32 round = 0; round <= rounds; ++round) {
				_records.assign(bytes, 0);
				auto* records = reinterpret_cast<ParticleInstanceRecord*>(_records.data());
				for (u32 slot = 0; slot < batch->watermark; ++slot) {
					const ParticleSystem* member = batch->members[slot];
					if (!member) {
						continue;
					}
					ParticleInstanceRecord& record = records[slot];
					const bool advances = round < member->_rounds;
					const u32 run = std::min(round, member->_rounds);
					record.flags = INSTANCE_CLAIMED | (member->_fresh ? INSTANCE_FRESH : 0u) | (advances ? INSTANCE_ADVANCES : 0u);
					record.parity = member->_parity ^ (run & 1u);
					record.frame = member->_frame + run;
					record.drawTable = member->_drawTable;
					record.effect = member->_effectAddress;
					if (advances && !member->_desc.cpu) {
						const ParticleSystem::Update& update = member->_declared[round];
						record.spawnCount = update.spawnCount;
						record.deltaTime = update.deltaTime;
						record.drawTable = update.drawTable;
						record.effect = update.effect;
					}
				}
				u64 address = 0;
				const RHI::TransientAllocation allocation = device.AllocateTransient(bytes, 16);
				if (allocation.IsValid()) {
					std::memcpy(allocation.cpu, _records.data(), bytes);
					address = device.GetBufferDeviceAddress(allocation.buffer) + allocation.offset;
				}
				if (round < rounds) {
					batch->rounds.push_back(address);
				} else {
					batch->standing = address;
				}
			}
		}
	}

	FrameImage ParticleSimulation::ReadDepth(FramePassPlan& plan, ParticleBatch& batch, u32 round, u32 binding, bool& recorded) {
		if (binding == ParticleSystemDesc::NO_DEPTH) {
			return {};
		}
		const CameraDepth depth = plan.ReadCameraDepth(_mainCamera);
		if (!recorded) {
			for (ParticleSystem* member : batch.members) {
				if (!member || round >= member->_rounds) {
					continue;
				}
				const ParticleSystem::Update& held = member->_declared[round];
				const std::span<u8> block{member->_declaredBlocks.data() + held.block, held.size};
				WriteCameraRecord(block, batch.desc.cameraOffset, batch.desc.world, depth);
			}
			recorded = true;
		}
		return depth.image;
	}

	void ParticleSimulation::DeclareEvents(FramePassPlan& plan, std::span<const ParticleEventTarget> targets) {
		for (const ParticleEventTarget& target : targets) {
			if (target.child && target.child->IsCreated()) {
				plan.Write(target.child->_batch->lists, RHI::ResourceState::UnorderedAccess);
				plan.Write(target.child->_batch->arguments, RHI::ResourceState::UnorderedAccess);
			}
		}
	}

	void ParticleSimulation::DeclareReset(FramePassPlan& plan, ParticleBatch& batch) {
		bool fresh = false;
		for (const ParticleSystem* member : batch.members) {
			fresh = fresh || (member && member->_fresh);
		}
		if (!fresh) {
			return;
		}
		constexpr RHI::ResourceState STORAGE = RHI::ResourceState::UnorderedAccess;
		_steps.push_back({&batch, nullptr, StepKind::Reset});
		plan.AddStep();
		plan.Write(batch.lists, STORAGE);
		plan.Write(batch.attributes, STORAGE);
		plan.Write(batch.arguments, STORAGE);
		for (ParticleSystem* member : batch.members) {
			if (member && member->_fresh) {
				member->_listsReady = true;
			}
		}
	}

	void ParticleSimulation::DeclareRound(FramePassPlan& plan, ParticleBatch& batch, u32 round) {
		bool any = false;
		bool spawns = false;
		for (const ParticleSystem* member : batch.members) {
			if (member && round < member->_rounds) {
				any = true;
				spawns = spawns || member->_declared[round].spawnCount > 0;
			}
		}
		if (!any) {
			return;
		}
		constexpr RHI::ResourceState STORAGE = RHI::ResourceState::UnorderedAccess;
		const ParticleSystemDesc& desc = batch.desc;
		const auto add = [&](StepKind kind, FrameImage depth) {
			Step step{&batch, nullptr, kind};
			step.update = round;
			step.depth = depth;
			_steps.push_back(step);
		};
		bool recorded = false;
		const ParticleSystem* parent = desc.parent;
		const bool fed = parent && parent->IsCreated();
		if (spawns || fed) {
			plan.AddStep();
			add(StepKind::Initialize, ReadDepth(plan, batch, round, desc.initializeDepth, recorded));
			plan.Write(batch.lists, STORAGE);
			plan.Write(batch.attributes, STORAGE);
			if (fed) {
				plan.Read(parent->_batch->attributes, STORAGE);
				plan.Read(batch.arguments, RHI::ResourceState::IndirectArgument);
			}
			DeclareEvents(plan, desc.initializeEvents);
		}
		plan.AddStep();
		add(StepKind::Update,
			ReadDepth(plan, batch, round, desc.updateDepth != ParticleSystemDesc::NO_DEPTH ? desc.updateDepth : desc.initializeDepth, recorded));
		plan.Read(batch.arguments, RHI::ResourceState::IndirectArgument);
		plan.Write(batch.lists, STORAGE);
		plan.Write(batch.attributes, STORAGE);
		DeclareEvents(plan, desc.updateEvents);

		if (desc.strips > 0 && AcquireStripKernel()) {
			add(StepKind::Strips, {});
			plan.AddStep();
			plan.Write(batch.lists, STORAGE);
			plan.Read(batch.attributes, STORAGE);
			if (fed) {
				plan.Write(parent->_batch->lists, STORAGE);
			}
		}

		add(StepKind::Arguments, {});
		plan.AddStep();
		plan.Write(batch.lists, STORAGE);
		plan.Read(batch.attributes, STORAGE);
		plan.Write(batch.arguments, STORAGE);
		for (ParticleSystem* member : batch.members) {
			if (member && round + 1 == member->_rounds) {
				DeclareFrameSorts(plan, *member);
			}
		}
	}

	void ParticleSimulation::DeclareFed(FramePassPlan& plan, ParticleBatch& batch) {
		constexpr RHI::ResourceState STORAGE = RHI::ResourceState::UnorderedAccess;
		bool any = false;
		for (ParticleSystem* member : batch.members) {
			if (!member || member->_rounds == 0) {
				continue;
			}
			any = true;
			_steps.push_back({&batch, member, StepKind::Upload});
			plan.AddStep();
			plan.Read(member->_feed, RHI::ResourceState::TransferSrc);
			plan.Write(batch.lists, RHI::ResourceState::TransferDst);
			plan.Write(batch.attributes, RHI::ResourceState::TransferDst);
		}
		if (!any) {
			return;
		}
		Step arguments{&batch, nullptr, StepKind::Arguments};
		arguments.update = 0;
		_steps.push_back(arguments);
		plan.AddStep();
		plan.Write(batch.lists, STORAGE);
		plan.Read(batch.attributes, STORAGE);
		plan.Write(batch.arguments, STORAGE);
		for (ParticleSystem* member : batch.members) {
			if (member && member->_rounds > 0) {
				DeclareFrameSorts(plan, *member);
			}
		}
	}

	void ParticleSimulation::DeclareFrameSorts(FramePassPlan& plan, ParticleSystem& system) {
		for (u32 o = 0; o < system._desc.outputs.size(); ++o) {
			if (!system.Sorts(o, ParticleSort::Frame)) {
				continue;
			}
			for (u32 step = 0; step < system.SortSteps(); ++step) {
				Step sort{nullptr, &system, StepKind::Sort};
				sort.sort = {o, step};
				_steps.push_back(sort);
				plan.AddStep();
				system.DeclareSortStep(plan, step);
			}
			system._ordered[o] = 1;
		}
	}

	std::span<const ComputeTexture> ParticleSimulation::WithDepth(std::span<const ComputeTexture> textures, u32 binding, FrameImage depth) {
		if (binding == ParticleSystemDesc::NO_DEPTH) {
			return textures;
		}
		_textures.assign(textures.begin(), textures.end());
		ComputeTexture read;
		read.binding = binding;
		read.image = depth;
		_textures.push_back(read);
		return {_textures.data(), _textures.size()};
	}

	void ParticleSimulation::Record(ComputeContext& context) {
		if (context.GetStep() >= _steps.size()) {
			return;
		}
		const Step& step = _steps[context.GetStep()];

		if (!step.batch) {
			ParticleSystem& system = *step.system;
			if (step.kind == StepKind::Sort) {
				system.RecordSort(context, step.sort.output, step.sort.step, Vec3{}, Vec3{});
			} else if (step.kind == StepKind::Counts) {
				const u32 slot = ParticleReadback::SlotOf(_frame);
				if (Counted(context.CopyBuffer(system._batch->lists, system._countRing.ring, system.GetListsOffset(), slot * COUNT_BYTES, COUNT_BYTES),
							_frameCounts.copies)) {
					system._countRing.frames[slot] = _frame;
				}
			}
			return;
		}

		ParticleBatch& batch = *step.batch;
		const ParticleSystemDesc& desc = batch.desc;
		ParticleFrame frame = BatchFrame(batch);
		frame.instances = step.update < batch.rounds.size() ? batch.rounds[step.update] : batch.standing;
		frame.batchFresh = batch.fresh ? 1u : 0u;
		const std::span<const u8> push{reinterpret_cast<const u8*>(&frame), sizeof(frame)};

		const ComputeBinding storage[3] = {{0, batch.lists}, {1, batch.attributes}, {2, batch.arguments}};
		const std::span<const ComputeBinding> particles{storage, 2};
		const std::span<const ComputeBinding> withArguments{storage, 3};
		const ParticleSystem* parent = desc.parent && desc.parent->IsCreated() ? desc.parent : nullptr;
		const auto withOthers = [&](std::span<const ParticleEventTarget> targets, bool readsParent) -> std::span<const ComputeBinding> {
			_bindings.assign(particles.begin(), particles.end());
			if (readsParent && parent) {
				_bindings.push_back({desc.parentBinding, parent->_batch->attributes});
			}
			for (const ParticleEventTarget& target : targets) {
				if (target.child && target.child->IsCreated()) {
					_bindings.push_back({target.lists, target.child->_batch->lists});
					_bindings.push_back({target.arguments, target.child->_batch->arguments});
				}
			}
			return {_bindings.data(), _bindings.size()};
		};

		switch (step.kind) {
		case StepKind::Reset:
			Counted(context.Dispatch(_reset, withArguments, {}, push, GroupsFor(desc.capacity, _reset), batch.watermark), _frameCounts.dispatches);
			batch.fresh = false;
			for (ParticleSystem* member : batch.members) {
				if (member) {
					member->_fresh = false;
				}
			}
			break;
		case StepKind::Initialize: {
			const std::span<const ComputeBinding> buffers = withOthers(desc.initializeEvents, true);
			const std::span<const ComputeTexture> textures = WithDepth(desc.initializeTextures, desc.initializeDepth, step.depth);
			if (parent) {
				Counted(context.DispatchIndirect(desc.initialize, buffers, textures, push, batch.arguments,
												 DRAW_ARGUMENTS_OFFSET + DRAW_ARGUMENT_WORDS * 4 * desc.outputs.size()),
						_frameCounts.dispatches);
				break;
			}
			u32 groups = 0;
			for (const ParticleSystem* member : batch.members) {
				if (member && step.update < member->_rounds) {
					groups = std::max(groups, GroupsFor(member->_declared[step.update].spawnCount, desc.initialize));
				}
			}
			Counted(context.Dispatch(desc.initialize, buffers, textures, push, groups, batch.watermark), _frameCounts.dispatches);
			break;
		}
		case StepKind::Update:
			Counted(context.DispatchIndirect(desc.update, withOthers(desc.updateEvents, false), WithDepth(desc.updateTextures, desc.updateDepth, step.depth),
											 push, batch.arguments, batch.DispatchOffset()),
					_frameCounts.dispatches);
			break;
		case StepKind::Arguments:
			Counted(context.Dispatch(_counts, withArguments, {}, push, 1), _frameCounts.dispatches);
			for (ParticleSystem* member : batch.members) {
				if (member && step.update < member->_rounds) {
					member->_parity ^= 1u;
					++member->_frame;
				}
			}
			break;
		case StepKind::Strips: {
			const ComputeBinding strips[3] = {{0, batch.lists}, {1, batch.attributes}, {2, parent ? parent->_batch->lists : batch.lists}};
			Counted(context.Dispatch(_strips, strips, {}, push, GroupsFor(desc.strips, _strips), batch.watermark), _frameCounts.dispatches);
			break;
		}
		case StepKind::Upload: {
			ParticleSystem& system = *step.system;
			const u64 slot = static_cast<u64>(system._fedSlot) * system._feedSlotBytes;
			const u64 header = LISTS_HEADER_WORDS * 4;
			const u64 lists = system.GetListsOffset();
			const u64 list = lists + (LISTS_HEADER_WORDS + static_cast<u64>(1 - system._parity) * desc.capacity) * 4;
			Counted(context.CopyBuffer(system._feed, batch.lists, slot, lists, header), _frameCounts.copies);
			if (system._fedAlive > 0) {
				Counted(context.CopyBuffer(system._feed, batch.lists, slot + header, list, static_cast<u64>(system._fedAlive) * 4), _frameCounts.copies);
			}
			const bool copied = context.CopyBuffer(system._feed, batch.attributes, slot + header + static_cast<u64>(desc.capacity) * 4, system.GetAttributesOffset(),
												   static_cast<u64>(desc.capacity) * desc.attributeCount * 4);
			Counted(copied, _frameCounts.copies);
			system._fed = false;
			break;
		}
		case StepKind::Bounds: {
			const u32 slot = ParticleReadback::SlotOf(_frame);
			const u64 bytes = BOUNDS_BYTES * batch.slots;
			if (Counted(context.CopyBuffer(batch.lists, batch.boundsRing.ring, batch.FinishedBoxesOffset(), slot * bytes, bytes), _frameCounts.copies)) {
				batch.boundsRing.frames[slot] = _frame;
			}
			break;
		}
		case StepKind::Counts:
		case StepKind::Sort:
			break;
		}
	}

	ParticleSimulation& GetSimulation() {
		if (!_simulation) {
			_simulation = New<ParticleSimulation>(MemTag::Graphics);
		}
		return *_simulation;
	}

	void RegisterSimulation(const void* owner) {
		_owner = owner;
		FramePasses::Register(GetSimulation(), owner);
	}

	void ShutdownSimulation() {
		if (_owner) {
			FramePasses::UnregisterOwnedBy(_owner);
			_owner = nullptr;
		}
		if (_simulation) {
			_simulation->FreeBatches();
			_simulation->ReleaseKernels();
			Delete(_simulation);
			_simulation = nullptr;
		}
	}

} // namespace Shalltear::VisualEffectsImpl
