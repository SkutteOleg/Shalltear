#pragma once

#include "Shalltear/Core/Graphics/Camera.h"
#include "Shalltear/Core/Graphics/Light.h"
#include "Shalltear/Core/Graphics/MaterialAsset.h"
#include "Shalltear/Core/Graphics/MeshAsset.h"
#include "Shalltear/Core/Graphics/MeshRenderer.h"
#include "Shalltear/Core/Graphics/RenderPass.h"
#include "Shalltear/Core/Graphics/SkinAsset.h"
#include "Shalltear/Core/Graphics/SkinnedMeshRenderer.h"
#include "Shalltear/Core/Graphics/TextureAsset.h"
#include "Shalltear/Core/Math/Vector.h"
#include "Shalltear/Core/Types.h"

#include <span>
#include <string_view>

// refusals thrown as std::runtime_error
namespace Shalltear::CSharpImpl::RenderingBindings {

	i32 FillCameras(std::span<u64> ids) noexcept;

	Vec4 GetViewport(const Camera& camera) noexcept;
	void SetPass(Camera& camera, i32 index, RenderPass* pass);

	void SetMaterial(MeshRenderer& renderer, i32 slot, MaterialAsset* material);
	void SetMaterial(SkinnedMeshRenderer& renderer, i32 slot, MaterialAsset* material);
	MaterialAsset* GetMaterialInstance(MeshRenderer& renderer, i32 slot);
	MaterialAsset* GetMaterialInstance(SkinnedMeshRenderer& renderer, i32 slot);

	void SetBlendShapeWeight(SkinnedMeshRenderer& renderer, i32 index, f32 weight);
	void SetBlendShapeWeightCount(SkinnedMeshRenderer& renderer, i32 count);
	// returned count may exceed weights.size()
	i32 ReadBlendShapeWeights(const SkinnedMeshRenderer& renderer, std::span<f32> weights) noexcept;

	void SetTexture(MaterialAsset& material, std::string_view name, TextureAsset* texture);
	void SetKeyword(MaterialAsset& material, std::string_view keyword, bool enabled);
	bool HasKeyword(const MaterialAsset& material, std::string_view keyword) noexcept;
	std::string_view GetKeyword(const MaterialAsset& material, i32 index);
	// freed by caller via MaterialAsset::DestroyInstance
	MaterialAsset* CreateInstance(MaterialAsset& material);

	std::string_view GetBlendShapeName(const MeshAsset& mesh, i32 index);

} // namespace Shalltear::CSharpImpl::RenderingBindings
