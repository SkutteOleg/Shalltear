#include "RenderingBindings.h"

#include "BindingSupport.h"

#include "Shalltear/Core/Graphics/RenderSystem.h"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace Shalltear::CSharpImpl::RenderingBindings {

	namespace {

		u32 SlotLimit(u32 bound, const MeshAsset* mesh) {
			return std::max(bound, mesh ? static_cast<u32>(mesh->submeshes.size()) : 0u);
		}

		template <typename Renderer>
		void PutMaterial(Renderer& renderer, const char* type, i32 slot, MaterialAsset* material) {
			const u32 limit = SlotLimit(renderer.GetMaterialCount(), renderer.GetMesh().Get());
			if (slot < 0 || static_cast<u32>(slot) > limit) {
				throw std::runtime_error(std::format("{} has {} material slot(s). Pass a slot from 0 to {}", type, renderer.GetMaterialCount(), limit));
			}
			renderer.SetMaterial(static_cast<u32>(slot), Handle<MaterialAsset>(material));
		}

		template <typename Renderer>
		MaterialAsset* Instance(Renderer& renderer, const char* type, i32 slot) {
			if (slot < 0 || static_cast<u32>(slot) >= renderer.GetMaterialCount()) {
				throw std::runtime_error(std::format("{} has no material slot {} ({} slot(s))", type, slot, renderer.GetMaterialCount()));
			}
			return renderer.GetMaterialInstance(static_cast<u32>(slot)).Get();
		}

		u32 BlendShapeCount(const SkinnedMeshRenderer& renderer) {
			const MeshAsset* mesh = renderer.GetMesh().Get();
			return mesh ? static_cast<u32>(mesh->blendShapes.size()) : 0u;
		}

	} // anonymous namespace

	i32 FillCameras(std::span<u64> ids) noexcept {
		const std::span<const Handle<Camera>> cameras = RenderSystem::GetCameras();
		const size_t count = std::min(cameras.size(), ids.size());
		for (size_t index = 0; index < count; ++index) {
			ids[index] = IdOf(cameras[index]);
		}
		return static_cast<i32>(cameras.size());
	}

	Vec4 GetViewport(const Camera& camera) noexcept {
		const RectF rect = camera.GetViewportRect();
		return Vec4(rect.position.x, rect.position.y, rect.size.x, rect.size.y);
	}

	void SetPass(Camera& camera, i32 index, RenderPass* pass) {
		if (index < 0 || static_cast<u32>(index) > camera.GetPassCount()) {
			throw std::runtime_error(std::format("Camera has {} pass slot(s). Pass a slot from 0 to {}", camera.GetPassCount(), camera.GetPassCount()));
		}
		camera.SetPass(static_cast<u32>(index), Handle<RenderPass>(pass));
	}

	void SetMaterial(MeshRenderer& renderer, i32 slot, MaterialAsset* material) {
		PutMaterial(renderer, "MeshRenderer", slot, material);
	}

	void SetMaterial(SkinnedMeshRenderer& renderer, i32 slot, MaterialAsset* material) {
		PutMaterial(renderer, "SkinnedMeshRenderer", slot, material);
	}

	MaterialAsset* GetMaterialInstance(MeshRenderer& renderer, i32 slot) {
		return Instance(renderer, "MeshRenderer", slot);
	}

	MaterialAsset* GetMaterialInstance(SkinnedMeshRenderer& renderer, i32 slot) {
		return Instance(renderer, "SkinnedMeshRenderer", slot);
	}

	void SetBlendShapeWeight(SkinnedMeshRenderer& renderer, i32 index, f32 weight) {
		if (index < 0 || static_cast<u32>(index) >= BlendShapeCount(renderer)) {
			throw std::runtime_error(std::format("SkinnedMeshRenderer's mesh has no blend shape {} ({} blend shape(s))", index, BlendShapeCount(renderer)));
		}
		renderer.SetBlendShapeWeight(static_cast<u32>(index), weight);
	}

	void SetBlendShapeWeightCount(SkinnedMeshRenderer& renderer, i32 count) {
		if (count < 0 || static_cast<u32>(count) > BlendShapeCount(renderer)) {
			throw std::runtime_error(std::format("SkinnedMeshRenderer's mesh has {} blend shape(s). Pass a count from 0 to {}", BlendShapeCount(renderer),
												 BlendShapeCount(renderer)));
		}
		renderer.SetBlendShapeWeightCount(static_cast<u32>(count));
	}

	i32 ReadBlendShapeWeights(const SkinnedMeshRenderer& renderer, std::span<f32> weights) noexcept {
		const std::span<const f32> held = renderer.GetBlendShapeWeights();
		std::copy_n(held.begin(), std::min(held.size(), weights.size()), weights.begin());
		return static_cast<i32>(held.size());
	}

	void SetTexture(MaterialAsset& material, std::string_view name, TextureAsset* texture) {
		if (texture && !texture->assetId.IsValid()) {
			throw std::runtime_error(std::format("Material.SetTexture got a texture with no asset id for '{}'. Pass a texture of the project", name));
		}
		material.SetTexture(name, texture ? texture->assetId : Guid{});
	}

	void SetKeyword(MaterialAsset& material, std::string_view keyword, bool enabled) {
		if (keyword.empty()) {
			throw std::runtime_error("Material.SetKeyword got an empty keyword. Pass a keyword's name");
		}
		Vector<str> keywords = material.keywords;
		const auto found = std::find(keywords.begin(), keywords.end(), keyword);
		if (enabled == (found != keywords.end())) {
			return;
		}
		if (enabled) {
			keywords.emplace_back(keyword);
		} else {
			keywords.erase(found);
		}
		material.SetKeywords(keywords);
	}

	bool HasKeyword(const MaterialAsset& material, std::string_view keyword) noexcept {
		return std::binary_search(material.keywords.begin(), material.keywords.end(), keyword);
	}

	std::string_view GetKeyword(const MaterialAsset& material, i32 index) {
		if (index < 0 || static_cast<size_t>(index) >= material.keywords.size()) {
			throw std::runtime_error(std::format("Material has no keyword {} ({} keyword(s))", index, material.keywords.size()));
		}
		return material.keywords[static_cast<size_t>(index)];
	}

	MaterialAsset* CreateInstance(MaterialAsset& material) {
		return MaterialAsset::CreateInstance(Handle<MaterialAsset>(&material)).Get();
	}

	std::string_view GetBlendShapeName(const MeshAsset& mesh, i32 index) {
		if (index < 0 || static_cast<size_t>(index) >= mesh.blendShapes.size()) {
			throw std::runtime_error(std::format("Mesh has no blend shape {} ({} blend shape(s))", index, mesh.blendShapes.size()));
		}
		return mesh.blendShapes[static_cast<size_t>(index)].name;
	}

} // namespace Shalltear::CSharpImpl::RenderingBindings
