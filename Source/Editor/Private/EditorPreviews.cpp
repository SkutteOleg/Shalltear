#include "Shalltear/Editor/EditorPreviews.h"

#include "PreviewPaneRegistry.h"

#include "Shalltear/Core/Asset/Asset.h"
#include "Shalltear/Core/Asset/AssetImporter.h"
#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/StringUtil.h"

#include "Shalltear/Core/Math.h"
#include "Shalltear/Core/Math/HalfFloat.h"

#include "Shalltear/Editor/EditorTheme.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace Shalltear {

	namespace {

		using EditorTheme::ColorToken;

		struct ProducerEntry {
			AssetPreviewProducerFn producer;
			const void* owner = nullptr;
		};

		struct PaneEntry {
			AssetPreviewPaneDesc desc;
			const void* owner = nullptr;
		};

		struct PreviewState {
			HashMap<StringHash, ProducerEntry> producers{GetMemoryResource(MemTag::Editor)};
			HashMap<StringHash, PaneEntry> panes{GetMemoryResource(MemTag::Editor)};
		};

		PreviewState* _state = nullptr;

		struct AssetBadgeEntry {
			EditorIcons::Icon glyph;
			ColorToken tint = ColorToken::TextDim;
			const void* owner = nullptr;
		};

		struct AssetBadgeState {
			HashMap<StringHash, AssetBadgeEntry> badges{GetMemoryResource(MemTag::Editor)};
			bool seeded = false;
		};

		AssetBadgeState* _assetBadges = nullptr;

		void SeedAssetBadges(bool onlyAbsent);

		AssetBadgeState& AssetBadgeTable() {
			if (!_assetBadges) {
				_assetBadges = New<AssetBadgeState>(MemTag::Editor);
			}
			if (!_assetBadges->seeded) {
				SeedAssetBadges(false);
			}
			return *_assetBadges;
		}

	} // anonymous namespace

	namespace EditorPreviewsInternal {
		void ReleaseBuiltInAssets();
	}

	void EditorPreviews::Initialize() {
		if (!_state) {
			_state = New<PreviewState>(MemTag::Editor);
		}
	}

	void EditorPreviews::Terminate() {
		EditorPreviewsInternal::ReleaseBuiltInAssets();
		if (_state) {
			Delete(_state);
			_state = nullptr;
		}
		Delete(_assetBadges);
		_assetBadges = nullptr;
	}

	void EditorPreviews::RegisterProducer(StringHash assetTypeName, AssetPreviewProducerFn producer, const void* owner) {
		if (_state && producer) {
			_state->producers[assetTypeName] = {std::move(producer), owner};
		}
	}

	void EditorPreviews::RegisterPane(StringHash assetTypeName, AssetPreviewPaneDesc desc, const void* owner) {
		if (_state) {
			_state->panes[assetTypeName] = {std::move(desc), owner};
		}
	}

	u32 EditorPreviews::UnregisterOwnedBy(const void* owner) {
		if (!_state || !owner) {
			return 0;
		}
		auto owned = [owner](const auto& pair) { return pair.second.owner == owner; };
		return static_cast<u32>(std::erase_if(_state->producers, owned) + std::erase_if(_state->panes, owned));
	}

	const AssetPreviewPaneDesc* EditorPreviewsInternal::FindPane(StringHash assetTypeName) {
		if (!_state) {
			return nullptr;
		}
		auto it = _state->panes.find(assetTypeName);
		return it != _state->panes.end() ? &it->second.desc : nullptr;
	}

	StringHash EditorPreviewsInternal::AssetTypeForFileName(std::string_view fileName) {
		str extension = Path(str(fileName)).GetExtension();
		IAssetImporter* importer = extension.empty() ? nullptr : AssetImporterRegistry::FindByExtension(extension);
		return importer ? importer->GetAssetType() : StringHash{};
	}

	bool EditorPreviewsInternal::HasProducer(StringHash assetTypeName) { return _state && _state->producers.contains(assetTypeName); }

	bool EditorPreviews::Produce(const AssetPreviewRequest& request, AssetPreviewImage& out) {
		if (!_state || !request.asset) {
			return false;
		}
		auto it = _state->producers.find(request.asset->GetAssetTypeId());
		if (it == _state->producers.end()) {
			return false;
		}
		out = {};
		if (!it->second.producer(request, out)) {
			out = {};
			return false;
		}
		return true;
	}

	namespace {

		void PlantBadge(StringHash type, EditorIcons::Icon glyph, ColorToken tint, bool onlyAbsent) {
			if (onlyAbsent && _assetBadges->badges.contains(type)) {
				return;
			}
			_assetBadges->badges[type] = {glyph, tint, nullptr};
		}

		void SeedAssetBadges(bool onlyAbsent) {
			using namespace EditorIcons::Glyph;
			_assetBadges->seeded = true;

			PlantBadge(StringHash("TextAsset"), AssetText, ColorToken::TextDim, onlyAbsent);
			PlantBadge(StringHash("ShaderAsset"), AssetShader, ColorToken::AssetShader, onlyAbsent);
			PlantBadge(StringHash("TextureAsset"), AssetTexture, ColorToken::AssetTexture, onlyAbsent);
			PlantBadge(StringHash("MeshAsset"), AssetMesh, ColorToken::AssetMesh, onlyAbsent);
			PlantBadge(StringHash("MaterialAsset"), AssetMaterial, ColorToken::AssetMaterial, onlyAbsent);
			PlantBadge(StringHash("FontAsset"), AssetFont, ColorToken::AssetFont, onlyAbsent);
			PlantBadge(StringHash("SceneAsset"), AssetScene, ColorToken::AssetScene, onlyAbsent);
			PlantBadge(StringHash("PrefabAsset"), AssetPrefab, ColorToken::AssetPrefab, onlyAbsent);

			PlantBadge(StringHash("UIDocumentAsset"), AssetUIDocument, ColorToken::AssetUI, onlyAbsent);
			PlantBadge(StringHash("StyleSheetAsset"), AssetStyleSheet, ColorToken::AssetUI, onlyAbsent);
			PlantBadge(StringHash("UIPanelSettings"), AssetPanelSettings, ColorToken::AssetUI, onlyAbsent);

			PlantBadge(StringHash("GraphicsProfile"), AssetGraphicsProfile, ColorToken::AssetProfile, onlyAbsent);
			PlantBadge(StringHash("RenderPass"), AssetRenderPass, ColorToken::AssetProfile, onlyAbsent);

			PlantBadge(StringHash("VectorAsset"), AssetVector, ColorToken::AssetTexture, onlyAbsent);

			PlantBadge(StringHash("SkeletonAsset"), AssetSkeleton, ColorToken::AssetAnimation, onlyAbsent);
			PlantBadge(StringHash("SkinAsset"), AssetSkin, ColorToken::AssetAnimation, onlyAbsent);
		}

	} // anonymous namespace

	void EditorPreviews::AssetBadges::RegisterBuiltIns() {
		AssetBadgeState& table = AssetBadgeTable();
		SeedAssetBadges(table.seeded);
	}

	void EditorPreviews::AssetBadges::Register(StringHash assetTypeName, EditorIcons::Icon glyph,
											   EditorTheme::ColorToken tint, const void* owner) {
		AssetBadgeTable().badges[assetTypeName] = {glyph, tint, owner};
	}

	u32 EditorPreviews::AssetBadges::UnregisterOwnedBy(const void* owner) {
		if (!_assetBadges || !owner) {
			return 0;
		}
		const u32 removed = static_cast<u32>(std::erase_if(_assetBadges->badges, [owner](const auto& pair) {
			return pair.second.owner == owner;
		}));
		if (removed > 0) {
			SeedAssetBadges(true);
		}
		return removed;
	}

	EditorPreviews::TypeBadge EditorPreviews::BadgeForAssetType(StringHash assetTypeName) {
		const AssetBadgeState& table = AssetBadgeTable();
		auto it = table.badges.find(assetTypeName);
		if (it == table.badges.end()) {
			return {EditorIcons::Glyph::AssetGeneric, EditorTheme::GetColor(ColorToken::TextDim)};
		}
		return {it->second.glyph, EditorTheme::GetColor(it->second.tint)};
	}

	EditorPreviews::TypeBadge EditorPreviews::BadgeForFileName(std::string_view fileName) {
		str extension = StringUtil::ToLower(Path(str(fileName)).GetExtension());
		if (extension == ".cs") {
			return {EditorIcons::Glyph::AssetShader, EditorTheme::GetColor(ColorToken::AssetShader)};
		}
		const StringHash type = EditorPreviewsInternal::AssetTypeForFileName(fileName);
		if (type == StringHash{}) {
			return {{}, EditorTheme::GetColor(ColorToken::TextDim)};
		}
		return BadgeForAssetType(type);
	}

	Vec2 EditorPreviewOps::FitImage(Vec2I imageSize, Vec2 available) {
		if (imageSize.x <= 0 || imageSize.y <= 0 || available.x <= 0.0f || available.y <= 0.0f) {
			return {};
		}
		f32 scale = std::min(available.x / static_cast<f32>(imageSize.x),
							 available.y / static_cast<f32>(imageSize.y));
		return {static_cast<f32>(imageSize.x) * scale, static_cast<f32>(imageSize.y) * scale};
	}

	namespace {

		constexpr f32 ORBIT_DEG_PER_PX = 0.6f;
		constexpr f32 ORBIT_PITCH_LIMIT_DEG = 89.0f;
		constexpr f32 ORBIT_ZOOM_PER_NOTCH = 1.15f;
		constexpr f32 ORBIT_ZOOM_MIN = 0.3f;
		constexpr f32 ORBIT_ZOOM_MAX = 3.0f;

	} // anonymous namespace

	AssetPreviewOrbit EditorPreviewOps::OrbitAfterDrag(const AssetPreviewOrbit& from, Vec2 dragPx) {
		AssetPreviewOrbit next = from;
		next.yawDeg = from.yawDeg + dragPx.x * ORBIT_DEG_PER_PX;
		next.pitchDeg = Math::Clamp(from.pitchDeg + dragPx.y * ORBIT_DEG_PER_PX,
									-ORBIT_PITCH_LIMIT_DEG, ORBIT_PITCH_LIMIT_DEG);
		return next;
	}

	AssetPreviewOrbit EditorPreviewOps::OrbitAfterZoom(const AssetPreviewOrbit& from, f32 wheelTicks) {
		AssetPreviewOrbit next = from;
		next.zoom = Math::Clamp(from.zoom * std::pow(ORBIT_ZOOM_PER_NOTCH, -wheelTicks),
								ORBIT_ZOOM_MIN, ORBIT_ZOOM_MAX);
		return next;
	}

	bool EditorPreviewOps::FitCameraToBounds(const AABB& bounds, f32 aspect, f32 verticalFOVDegrees,
											 f32 marginScale, const AssetPreviewOrbit& orbit, ViewDesc& outDesc) {
		if (!bounds.IsValid() || aspect <= 0.0f || verticalFOVDegrees <= 0.0f || verticalFOVDegrees >= 180.0f) {
			return false;
		}
		f32 radius = bounds.Extents().Length();
		if (radius <= 0.0f) {
			return false;
		}

		f32 halfVertical = verticalFOVDegrees * 0.5f * Math::DEG_TO_RAD;
		f32 halfHorizontal = std::atan(std::tan(halfVertical) * aspect);
		f32 half = Math::Min(halfVertical, halfHorizontal);

		f32 distance = radius / std::sin(half) * Math::Max(marginScale, 1.0f) * Math::Max(orbit.zoom, 0.01f);

		Vec3 center = bounds.Center();
		Quat rotation = Quat::FromEuler(Vec3(orbit.pitchDeg, orbit.yawDeg, 0.0f));
		Vec3 eye = center - (rotation * Vec3::Forward()) * distance;
		outDesc.cameraPosition = eye;
		outDesc.view = Mat4::TRS(eye, rotation, Vec3::One()).Inverse();
		f32 nearPlane = Math::Max(0.01f, distance - radius * 2.0f);
		outDesc.projection = Mat4::PerspectiveReverseZ(verticalFOVDegrees, aspect, nearPlane, distance + radius * 4.0f);
		return true;
	}

	namespace {

		struct SourceReader {
			u32 bytesPerPixel = 0;
			bool swapRedBlue = false;
			bool half = false;
			bool sRGB = false;
			bool ok = false;
		};

		SourceReader DescribeSource(RHI::Format format) {
			switch (format) {
			case RHI::Format::RGBA8Unorm:
				return {4, false, false, false, true};
			case RHI::Format::RGBA8SRGB:
				return {4, false, false, true, true};
			case RHI::Format::BGRA8Unorm:
				return {4, true, false, false, true};
			case RHI::Format::BGRA8SRGB:
				return {4, true, false, true, true};
			case RHI::Format::RGBA16Float:
				return {8, false, true, false, false};
			default:
				return {};
			}
		}

		constexpr i32 MAX_SAMPLES_PER_AXIS = 4;

	} // anonymous namespace

	RHI::Format EditorPreviewOps::ResampleToRGBA8(std::span<const u8> src, Vec2I srcSize, RHI::Format srcFormat,
												  Vec2I dstSize, Vector<u8>& out) {
		out.clear();
		SourceReader reader = DescribeSource(srcFormat);
		if (reader.bytesPerPixel == 0 || srcSize.x <= 0 || srcSize.y <= 0 || dstSize.x <= 0 || dstSize.y <= 0) {
			return RHI::Format::Unknown;
		}
		u64 needed = static_cast<u64>(srcSize.x) * static_cast<u64>(srcSize.y) * reader.bytesPerPixel;
		if (src.size() < needed) {
			return RHI::Format::Unknown;
		}

		auto sample = [&](i32 x, i32 y, f32* rgba) {
			const u8* pixel = src.data() + (static_cast<u64>(y) * srcSize.x + x) * reader.bytesPerPixel;
			if (reader.half) {
				for (u32 c = 0; c < 4; ++c) {
					u16 bits = static_cast<u16>(pixel[c * 2] | (static_cast<u16>(pixel[c * 2 + 1]) << 8));
					rgba[c] = std::clamp(Math::HalfToFloat(bits), 0.0f, 1.0f) * 255.0f;
				}
				return;
			}
			rgba[0] = static_cast<f32>(pixel[reader.swapRedBlue ? 2 : 0]);
			rgba[1] = static_cast<f32>(pixel[1]);
			rgba[2] = static_cast<f32>(pixel[reader.swapRedBlue ? 0 : 2]);
			rgba[3] = static_cast<f32>(pixel[3]);
		};

		out.resize(static_cast<size_t>(dstSize.x) * static_cast<size_t>(dstSize.y) * 4);
		for (i32 dy = 0; dy < dstSize.y; ++dy) {
			i32 y0 = static_cast<i32>(static_cast<i64>(dy) * srcSize.y / dstSize.y);
			i32 y1 = std::max(y0 + 1, static_cast<i32>(static_cast<i64>(dy + 1) * srcSize.y / dstSize.y));
			i32 yStep = std::max(1, (y1 - y0 + MAX_SAMPLES_PER_AXIS - 1) / MAX_SAMPLES_PER_AXIS);
			for (i32 dx = 0; dx < dstSize.x; ++dx) {
				i32 x0 = static_cast<i32>(static_cast<i64>(dx) * srcSize.x / dstSize.x);
				i32 x1 = std::max(x0 + 1, static_cast<i32>(static_cast<i64>(dx + 1) * srcSize.x / dstSize.x));
				i32 xStep = std::max(1, (x1 - x0 + MAX_SAMPLES_PER_AXIS - 1) / MAX_SAMPLES_PER_AXIS);

				f32 total[4] = {0.0f, 0.0f, 0.0f, 0.0f};
				u32 taken = 0;
				for (i32 sy = y0; sy < y1; sy += yStep) {
					for (i32 sx = x0; sx < x1; sx += xStep) {
						f32 rgba[4];
						sample(sx, sy, rgba);
						for (u32 c = 0; c < 4; ++c) {
							total[c] += rgba[c];
						}
						++taken;
					}
				}
				u8* dst = out.data() + (static_cast<size_t>(dy) * dstSize.x + dx) * 4;
				for (u32 c = 0; c < 4; ++c) {
					dst[c] = static_cast<u8>(std::clamp(total[c] / static_cast<f32>(taken) + 0.5f, 0.0f, 255.0f));
				}
			}
		}
		// half float sources are linear
		return reader.sRGB ? RHI::Format::RGBA8SRGB : RHI::Format::RGBA8Unorm;
	}

	str EditorPreviewOps::DescribeTexture(Vec2I size, RHI::Format format) {
		std::string_view formatName;
		switch (format) {
		case RHI::Format::R8Unorm:
			formatName = "R8Unorm";
			break;
		case RHI::Format::RGBA8Unorm:
			formatName = "RGBA8Unorm";
			break;
		case RHI::Format::RGBA8SRGB:
			formatName = "RGBA8SRGB";
			break;
		case RHI::Format::BGRA8Unorm:
			formatName = "BGRA8Unorm";
			break;
		case RHI::Format::BGRA8SRGB:
			formatName = "BGRA8SRGB";
			break;
		case RHI::Format::RGBA16Float:
			formatName = "RGBA16Float";
			break;
		case RHI::Format::RG16Float:
			formatName = "RG16Float";
			break;
		case RHI::Format::R32Uint:
			formatName = "R32Uint";
			break;
		case RHI::Format::D32Float:
			formatName = "D32Float";
			break;
		case RHI::Format::BC5Unorm:
			formatName = "BC5Unorm";
			break;
		case RHI::Format::BC7Unorm:
			formatName = "BC7Unorm";
			break;
		case RHI::Format::BC7SRGB:
			formatName = "BC7SRGB";
			break;
		case RHI::Format::BC6HUFloat:
			formatName = "BC6HUFloat";
			break;
		default:
			formatName = "Unknown";
			break;
		}
		return std::format("{} x {}  {}", size.x, size.y, formatName);
	}

} // namespace Shalltear
