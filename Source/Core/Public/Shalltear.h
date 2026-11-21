#pragma once

// IWYU pragma: begin_exports
#include "Shalltear/Core/Platform.h"
#include "Shalltear/Core/Types.h"
#include "Shalltear/Core/BuildInfo.h"
#include "Shalltear/Core/Memory.h"
#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/EnumFlags.h"
#include "Shalltear/Core/Assert.h"
#include "Shalltear/Core/Log.h"
#include "Shalltear/Core/Time.h"
#include "Shalltear/Core/StringHash.h"
#include "Shalltear/Core/StringUtil.h"
#include "Shalltear/Core/Event.h"
#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Object.h"
#include "Shalltear/Core/Handle.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Serialization/BinarySerializer.h"
#include "Shalltear/Core/Serialization/TextSerializer.h"
#include "Shalltear/Core/Asset.h"
#include "Shalltear/Core/Jobs/JobSystem.h"
#include "Shalltear/Core/Math.h"
#include "Shalltear/Core/Math/AnimationCurve.h"
#include "Shalltear/Core/Math/Gradient.h"
#include "Shalltear/Core/Config.h"
#include "Shalltear/Core/Window.h"
#include "Shalltear/Core/Screen.h"
#include "Shalltear/Core/Input.h"
#include "Shalltear/Core/Localization.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/Graphics.h"
#include "Shalltear/Core/Graphics/FullScreenPass.h"
#include "Shalltear/Core/Graphics/ImageEncoding.h"
#include "Shalltear/Core/Graphics/ColorSpace.h"
#include "Shalltear/Core/Graphics/LODGroup.h"
#include "Shalltear/Core/Graphics/ObjectShadow.h"
#include "Shalltear/Core/Graphics/ObjectPass.h"
#include "Shalltear/Core/Graphics/PrimitiveMesh.h"
#include "Shalltear/Core/Graphics/ProceduralRenderer.h"
#include "Shalltear/Core/Graphics/ReflectionProbe.h"
#include "Shalltear/Core/Graphics/ScreenCapture.h"
#include "Shalltear/Core/Graphics/ShaderGlobals.h"
#include "Shalltear/Core/Graphics/SkinnedMeshRenderer.h"
#include "Shalltear/Core/UI.h"
#include "Shalltear/Core/UI/StyleSheetAsset.h"
#include "Shalltear/Core/UI/VectorAsset.h"
#include "Shalltear/Core/UI/WidgetRegistry.h"
#include "Shalltear/Core/Module/ModuleAPI.h"
#include "Shalltear/Core/Module/ScriptFunctions.h"

#include <cmath>
#include <format>

#if __has_include("ShalltearPackages.h")
#include "ShalltearPackages.h"
#endif
// IWYU pragma: end_exports

// last, a using directive changes how later headers resolve
using namespace Shalltear;
