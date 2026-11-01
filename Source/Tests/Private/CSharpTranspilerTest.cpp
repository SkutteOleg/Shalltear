#include "TestsPCH.h"

#include "Shalltear/Core/Config.h"
#include "Shalltear/Core/Module.h"
#include "Shalltear/Core/Reflection.h"
#include "Shalltear/Core/Scene.h"
#include "Shalltear/Core/StringUtil.h"

#include "Shalltear/Editor/AssetBrowserOps.h"
#include "Shalltear/Editor/CompileService.h"
#include "Shalltear/Editor/CppToolchain.h"
#include "Shalltear/Editor/CSharpTranspiler.h"
#include "Shalltear/Editor/EditorIcons.h"
#include "Shalltear/Editor/EditorPreviews.h"
#include "Shalltear/Editor/ScriptProjectOps.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace Shalltear;

namespace {

	constexpr f64 COMPILE_TIMEOUT_SECONDS = 300.0;

	bool IsRunningInCi() {
		return std::getenv("CI") != nullptr || std::getenv("GITHUB_ACTIONS") != nullptr;
	}

	std::filesystem::path RepoDir(const char* relative) {
		std::error_code ec;
		std::filesystem::path probe = std::filesystem::current_path(ec);
		for (int depth = 0; depth < 6 && !probe.empty(); ++depth) {
			std::filesystem::path candidate = probe / relative;
			if (std::filesystem::exists(candidate, ec)) {
				return candidate;
			}
			std::filesystem::path parent = probe.parent_path();
			if (parent == probe) {
				break;
			}
			probe = parent;
		}
		return {};
	}

} // anonymous namespace

TEST(CSharpTranspilerUnitTest, TranspilesBehaviorComponentWithReflectionAndLineDirectives) {
	if (!CSharpTranspiler::IsRoslynLoaded()) {
		if (IsRunningInCi()) {
			FAIL() << "ShalltearRoslyn.dll must be staged in CI";
		}
		GTEST_SKIP() << "ShalltearRoslyn.dll is not staged";
	}
	constexpr std::string_view csSource = R"(using Shalltear;
using System.Collections.Generic;

public class OrbitSpinner : Behavior
{
	[Header("Motion")]
	[Range(0.0f, 360.0f)]
	[Tooltip("Angular speed in degrees per second")]
	public float degreesPerSecond = 90.0f;

	[Hidden]
	public int stepCount = 3;

	public SceneObject targetObject;
	public List<float> waypoints;

	private float _accumulatedAngle = 0.0f;

	public override void OnEnable()
	{
		_accumulatedAngle = 0.0f;
	}

	public override void OnUpdate(float dt)
	{
		var owner = GetOwner().Get();
		if (owner != null)
		{
			_accumulatedAngle += degreesPerSecond * dt;
			Quat step = Quat.AngleAxis(degreesPerSecond * dt, Vec3.Up());
			owner.SetLocalRotation((owner.GetLocalRotation() * step).Normalized());
		}
	}

	[Callable]
	public float ComputeStepAngle(float scale)
	{
		int waypoints = 1;
		int shadowedCopy = waypoints;
		return degreesPerSecond * scale * shadowedCopy;
	}
}
)";

	CSharpTranspiler::TranspileFileResult result;
	ASSERT_TRUE(CSharpTranspiler::TranspileSource("C:/Project/Source/OrbitSpinner.cs", csSource, result))
		<< (result.diagnostics.empty() ? str("no diagnostics") : result.diagnostics[0].raw);
	EXPECT_TRUE(result.succeeded);
	EXPECT_TRUE(result.diagnostics.empty());
	ASSERT_EQ(result.reflectedTypes.size(), 1u);
	EXPECT_EQ(result.reflectedTypes[0], "OrbitSpinner");

	EXPECT_NE(result.headerContent.find("class OrbitSpinner : public ::Shalltear::Behavior"), str::npos)
		<< result.headerContent;
	EXPECT_NE(result.headerContent.find("SLT_REFLECT_COMPONENT(OrbitSpinner, ::Shalltear::Behavior)"), str::npos)
		<< result.headerContent;
	EXPECT_NE(result.headerContent.find("SLT_REFLECT_FIELD(degreesPerSecond, SLT_HEADER(\"Motion\"), SLT_RANGE(0.0f, 360.0f), SLT_TOOLTIP(\"Angular speed in degrees per second\"))"), str::npos)
		<< result.headerContent;
	EXPECT_NE(result.headerContent.find("SLT_REFLECT_FIELD(stepCount, SLT_HIDDEN)"), str::npos)
		<< result.headerContent;
	EXPECT_NE(result.headerContent.find("SLT_REFLECT_FIELD(targetObject)"), str::npos)
		<< result.headerContent;
	EXPECT_NE(result.headerContent.find("SLT_REFLECT_ARRAY(waypoints)"), str::npos)
		<< result.headerContent;
	EXPECT_EQ(result.headerContent.find("SLT_REFLECT_FIELD(_accumulatedAngle)"), str::npos)
		<< result.headerContent;
	EXPECT_NE(result.headerContent.find("SLT_REFLECT_METHOD(ComputeStepAngle)"), str::npos)
		<< result.headerContent;
	EXPECT_NE(result.headerContent.find("::Shalltear::Handle<::Shalltear::SceneObject> targetObject;"), str::npos)
		<< result.headerContent;
	EXPECT_NE(result.headerContent.find("::Shalltear::Vector<::Shalltear::f32> waypoints"), str::npos)
		<< result.headerContent;

	EXPECT_NE(result.sourceContent.find("SLT_REGISTER_TYPE(OrbitSpinner)"), str::npos)
		<< result.sourceContent;
	EXPECT_NE(result.sourceContent.find("#line "), str::npos)
		<< result.sourceContent;
	EXPECT_NE(result.sourceContent.find("const ::Shalltear::f32 dt = ::Shalltear::Time::GetDeltaTime();"), str::npos)
		<< result.sourceContent;
	EXPECT_NE(result.sourceContent.find("owner->SetLocalRotation"), str::npos)
		<< result.sourceContent;
}

TEST(CSharpTranspilerUnitTest, TranspilesEnumValueStructAndDataAsset) {
	if (!CSharpTranspiler::IsRoslynLoaded()) {
		if (IsRunningInCi()) {
			FAIL() << "ShalltearRoslyn.dll must be staged in CI";
		}
		GTEST_SKIP() << "ShalltearRoslyn.dll is not staged";
	}
	constexpr std::string_view csSource = R"(using Shalltear;

public enum DamageElement
{
	Physical = 0,
	Fire = 1,
	Frost = 2
}

[Serializable]
public struct DamageRoll
{
	public float minDamage = 5.0f;
	public float maxDamage = 15.0f;
	public DamageElement element = DamageElement.Fire;
}

[CreateAssetMenu("Gameplay/WeaponData", "WeaponData.asset")]
public class WeaponData : DataAsset
{
	public string weaponName = "Sunblade";
	public DamageRoll primaryRoll;
}
)";

	CSharpTranspiler::TranspileFileResult result;
	ASSERT_TRUE(CSharpTranspiler::TranspileSource("C:/Project/Source/WeaponData.cs", csSource, result))
		<< (result.diagnostics.empty() ? str("no diagnostics") : result.diagnostics[0].raw);
	EXPECT_TRUE(result.succeeded);
	ASSERT_EQ(result.reflectedTypes.size(), 3u);

	EXPECT_NE(result.headerContent.find("enum class DamageElement : ::Shalltear::i32"), str::npos)
		<< result.headerContent;
	EXPECT_NE(result.headerContent.find("SLT_REFLECT_ENUM(DamageElement)"), str::npos)
		<< result.headerContent;
	EXPECT_NE(result.headerContent.find("SLT_ENUM_VALUE(Fire)"), str::npos)
		<< result.headerContent;

	EXPECT_NE(result.headerContent.find("SLT_REFLECT_VALUE(DamageRoll)"), str::npos)
		<< result.headerContent;
	EXPECT_NE(result.headerContent.find("SLT_REFLECT_CONSTRUCTIBLE()"), str::npos)
		<< result.headerContent;

	EXPECT_NE(result.headerContent.find("SLT_REFLECT_CLASS(WeaponData)"), str::npos)
		<< result.headerContent;
	EXPECT_NE(result.headerContent.find("SLT_CREATE_ASSET_MENU(\"Gameplay/WeaponData\", \"WeaponData.asset\")"), str::npos)
		<< result.headerContent;
	EXPECT_NE(result.headerContent.find("SLT_REFLECT_SUBOBJECT(primaryRoll)"), str::npos)
		<< result.headerContent;
	EXPECT_NE(result.sourceContent.find("SLT_REGISTER_TYPE(DamageRoll)"), str::npos)
		<< result.sourceContent;
	EXPECT_NE(result.sourceContent.find("SLT_REGISTER_TYPE(WeaponData)"), str::npos)
		<< result.sourceContent;
}

TEST(CSharpTranspilerUnitTest, RejectsInvalidInheritanceAndSyntaxWithAccurateDiagnostics) {
	if (!CSharpTranspiler::IsRoslynLoaded()) {
		if (IsRunningInCi()) {
			FAIL() << "ShalltearRoslyn.dll must be staged in CI";
		}
		GTEST_SKIP() << "ShalltearRoslyn.dll is not staged";
	}
	constexpr std::string_view badBaseSource = R"(using Shalltear;

public class IllegalActor : SceneObject
{
	public float health = 100.0f;
}
)";

	CSharpTranspiler::TranspileFileResult result;
	EXPECT_FALSE(CSharpTranspiler::TranspileSource("C:/Project/Source/IllegalActor.cs", badBaseSource, result));
	ASSERT_FALSE(result.diagnostics.empty());
	EXPECT_EQ(result.diagnostics[0].file, "C:/Project/Source/IllegalActor.cs");
	EXPECT_EQ(result.diagnostics[0].line, 3u);
	EXPECT_EQ(result.diagnostics[0].severity, CompilerDiagnosticSeverity::Error);
	EXPECT_NE(result.diagnostics[0].message.find("cannot inherit from SceneObject"), str::npos);

	constexpr std::string_view unsupportedClrSource = R"(using System;
using Shalltear;
using System.Collections.Generic;
using System.Linq;
using FloatList = global::System.Collections.Generic.List<float>;

public class PlainHelper
{
	public int counter = 1;
}

public struct BadStructWithList
{
	public List<float> items;
}

public class UnsupportedClrComponent : Behavior
{
	public List<float> values;
	public List<float> otherValues;
	public int[] rawArray;
	public Dictionary<string, int> lookup;
	public List<float> Items => values;

	public List<float> GetValues() => values;

	public async void BadAsync()
	{
	}

	public IEnumerable<int> BadYield()
	{
		yield return 1;
	}

	public void BadListParam(List<float> input)
	{
	}

	public void BadAliasedListParam(FloatList aliasedInput)
	{
	}

	public void RebindRefList(ref List<float> listRef)
	{
		listRef = new List<float>();
	}

	public bool Exists(Func<float, bool> pred) => false;

	public override void OnEnable()
	{
		SceneObject badNew = new SceneObject();
		var copiedList = values;
		otherValues = values;
		BadListParam(new List<float>());
		var storedLambda = (float v) => v > 0.0f;
		this.Exists(v => v > 0.0f);
		Func<float, bool> anonDel = delegate(float v) { return v > 0.0f; };
		void LocalHelper() { }
		var filtered = values.Where(v => v > 0.0f);
		string label = "a,b";
		var parts = label.Split(',');
		Console.WriteLine("hello");
		Math.Cbrt(8.0);
		values.Sort();
		try
		{
			BadAsync();
		}
		catch
		{
		}
	}
}
)";

	CSharpTranspiler::TranspileFileResult clrResult;
	EXPECT_FALSE(CSharpTranspiler::TranspileSource("C:/Project/Source/UnsupportedClrComponent.cs", unsupportedClrSource, clrResult));
	EXPECT_GE(clrResult.diagnostics.size(), 25u);
	bool foundBareClass = false;
	bool foundStructList = false;
	bool foundArrayType = false;
	bool foundDictType = false;
	bool foundAliasedListParam = false;
	bool foundVarArrayType = false;
	bool foundStringSplit = false;
	bool foundConsoleCall = false;
	bool foundMathCall = false;
	bool foundListSort = false;
	bool foundAsync = false;
	bool foundYield = false;
	bool foundListParam = false;
	bool foundListReturnMethod = false;
	bool foundListReturnProp = false;
	bool foundListCopy = false;
	bool foundListAssign = false;
	bool foundRefRebind = false;
	bool foundTempListArg = false;
	bool foundStoredLambda = false;
	bool foundAnonDelegate = false;
	bool foundLocalFn = false;
	bool foundNewObj = false;
	bool foundLinq = false;
	bool foundTry = false;
	for (const CompilerDiagnostic& d : clrResult.diagnostics) {
		if (d.message.find("has no engine base type") != str::npos) {
			foundBareClass = true;
		}
		if (d.message.find("Value struct 'BadStructWithList' cannot contain collection field 'items'") != str::npos) {
			foundStructList = true;
		}
		if (d.message.find("C# array type 'int[]' is not supported") != str::npos) {
			foundArrayType = true;
		}
		if (d.message.find("C# type 'Dictionary<string,int>' is not supported") != str::npos) {
			foundDictType = true;
		}
		if (d.message.find("Collection parameter 'aliasedInput' must use 'ref' or 'in'") != str::npos) {
			foundAliasedListParam = true;
		}
		if (d.message.find("C# array type 'string[]' is not supported") != str::npos) {
			foundVarArrayType = true;
		}
		if (d.message.find("C# string member 'Split' is not supported") != str::npos) {
			foundStringSplit = true;
		}
		if (d.message.find("BCL member 'Console.WriteLine' is not supported") != str::npos) {
			foundConsoleCall = true;
		}
		if (d.message.find("Member 'Math.Cbrt' is not defined on Shalltear.Math") != str::npos) {
			foundMathCall = true;
		}
		if (d.message.find("List<T> member 'Sort' is not supported") != str::npos) {
			foundListSort = true;
		}
		if (d.message.find("async") != str::npos) {
			foundAsync = true;
		}
		if (d.message.find("yield") != str::npos) {
			foundYield = true;
		}
		if (d.message.find("Collection parameter 'input' must use 'ref' or 'in'") != str::npos) {
			foundListParam = true;
		}
		if (d.message.find("Method 'GetValues' cannot return a collection by value") != str::npos) {
			foundListReturnMethod = true;
		}
		if (d.message.find("Computed property 'Items' cannot return a collection by value") != str::npos) {
			foundListReturnProp = true;
		}
		if (d.message.find("Assigning collection to local 'copiedList' copies the underlying C++ Vector") != str::npos) {
			foundListCopy = true;
		}
		if (d.message.find("Assigning collection to 'otherValues' copies or overwrites the underlying C++ Vector") != str::npos) {
			foundListAssign = true;
		}
		if (d.message.find("Assigning collection to 'listRef' copies or overwrites the underlying C++ Vector") != str::npos) {
			foundRefRebind = true;
		}
		if (d.message.find("Cannot pass temporary 'new List<T>()' directly as a method argument") != str::npos) {
			foundTempListArg = true;
		}
		if (d.message.find("Lambdas capture stack locals by reference in C++") != str::npos) {
			foundStoredLambda = true;
		}
		if (d.message.find("Anonymous 'delegate' methods") != str::npos) {
			foundAnonDelegate = true;
		}
		if (d.message.find("local function 'LocalHelper'") != str::npos) {
			foundLocalFn = true;
		}
		if (d.message.find("Cannot instantiate Shalltear Object type 'SceneObject' with 'new'") != str::npos) {
			foundNewObj = true;
		}
		if (d.message.find("LINQ") != str::npos) {
			foundLinq = true;
		}
		if (d.message.find("try") != str::npos) {
			foundTry = true;
		}
	}
	EXPECT_TRUE(foundBareClass);
	EXPECT_TRUE(foundStructList);
	EXPECT_TRUE(foundArrayType);
	EXPECT_TRUE(foundDictType);
	EXPECT_TRUE(foundAliasedListParam);
	EXPECT_TRUE(foundVarArrayType);
	EXPECT_TRUE(foundStringSplit);
	EXPECT_TRUE(foundConsoleCall);
	EXPECT_TRUE(foundMathCall);
	EXPECT_TRUE(foundListSort);
	EXPECT_TRUE(foundAsync);
	EXPECT_TRUE(foundYield);
	EXPECT_TRUE(foundListParam);
	EXPECT_TRUE(foundListReturnMethod);
	EXPECT_TRUE(foundListReturnProp);
	EXPECT_TRUE(foundListCopy);
	EXPECT_TRUE(foundListAssign);
	EXPECT_TRUE(foundRefRebind);
	EXPECT_TRUE(foundTempListArg);
	EXPECT_TRUE(foundStoredLambda);
	EXPECT_TRUE(foundAnonDelegate);
	EXPECT_TRUE(foundLocalFn);
	EXPECT_TRUE(foundNewObj);
	EXPECT_TRUE(foundLinq);
	EXPECT_TRUE(foundTry);
}

TEST(CSharpTranspilerUnitTest, RoslynDllIsLoadedAndLowersModernCSharpSyntax) {
	if (!CSharpTranspiler::IsRoslynLoaded()) {
		if (IsRunningInCi()) {
			FAIL() << "ShalltearRoslyn.dll must be staged in CI";
		}
		GTEST_SKIP() << "ShalltearRoslyn.dll is not staged";
	}
	EXPECT_TRUE(CSharpTranspiler::IsRoslynLoaded());

	constexpr std::string_view modernCs = R"(using Shalltear;
using System.Collections.Generic;

public class ModernSyntaxComponent : Behavior
{
	public float MoveSpeed { get; set; } = 12.0f;
	public SceneObject TargetActor;
	public List<float> ActiveTimers;
	public List<bool> ActiveFlags;
	public Camera MainCamera;
	public Light SceneLight;
	public Animator CharacterAnimator;
	public VisualEffect HitEffect;
	public PostProcessVolume GlobalVolume;
	public UIPanelRenderer HudPanel;

	public float SpeedRatio => MoveSpeed / 24.0f;

	[Callable]
	public float DoubleSpeed() => MoveSpeed * 2.0f;

	[Callable]
	public float EvaluateModeMultiplier(int mode)
	{
		return mode switch
		{
			0 => 1.0f,
			> 1 => 2.5f,
			_ => 0.5f
		};
	}

	[Callable]
	public string DescribeSpeed()
	{
		return $"Speed={MoveSpeed} Ratio={SpeedRatio}";
	}

	public override void OnUpdate(float dt)
	{
		if (Input.IsKeyDown(Key.Space) || Input.IsKeyDown(Key.W))
		{
			MoveSpeed = Math.Clamp(MoveSpeed + dt, 0.0f, 100.0f);
		}

		TargetActor?.SetActive(true);
		Vec3 targetPos = TargetActor?.GetLocalPosition() ?? Vec3.Zero();
		MainCamera?.SetFieldOfViewDeg(75.0f);
		SceneLight?.SetIntensity(2.0f);
		CharacterAnimator?.SetSpeed(1.25f);
		HitEffect?.SendEvent("OnBurst");
		HudPanel?.SetSortOrder(10.0f);

		var ownType = typeof(ModernSyntaxComponent);

		Ray probeRay = new Ray(targetPos, Vec3.Forward());
		if (Physics.Raycast(probeRay, out RaycastHit hit, 50.0f))
		{
			hit.@object.SetActive(false);
		}

		Component anyComp = GetComponent<Behavior>();
		if (anyComp is ModernSyntaxComponent modern)
		{
			modern.MoveSpeed += 1.0f;
		}

		ActiveTimers.RemoveAll(t => t <= 0.0f);
		foreach (var flag in ActiveFlags)
		{
			if (flag)
			{
				MoveSpeed += 0.1f;
			}
		}
	}

	protected override void OnCollisionEnter(Collision collision)
	{
		collision.@object.SetActive(false);
	}

	protected override void OnTriggerEnter(Collider other)
	{
		other.SetTrigger(true);
	}
}
)";

	CSharpTranspiler::TranspileFileResult result;
	ASSERT_TRUE(CSharpTranspiler::TranspileSource("C:/Project/Source/ModernSyntaxComponent.cs", modernCs, result))
		<< (result.diagnostics.empty() ? str("no diagnostics") : result.diagnostics[0].raw);
	EXPECT_NE(result.headerContent.find("class ModernSyntaxComponent : public ::Shalltear::Behavior, public ::Shalltear::PhysicsListener"), str::npos)
		<< result.headerContent;
	EXPECT_NE(result.headerContent.find("#include \"Shalltear/Animation.h\""), str::npos) << result.headerContent;
	EXPECT_NE(result.headerContent.find("#include \"Shalltear/VisualEffects.h\""), str::npos) << result.headerContent;
	EXPECT_NE(result.headerContent.find("#include \"Shalltear/PostProcessing.h\""), str::npos) << result.headerContent;
	EXPECT_NE(result.headerContent.find("SLT_REFLECT_FIELD(MoveSpeed)"), str::npos) << result.headerContent;
	EXPECT_NE(result.headerContent.find("SLT_REFLECT_METHOD(DoubleSpeed)"), str::npos) << result.headerContent;
	EXPECT_NE(result.headerContent.find("::Shalltear::f32 GetSpeedRatio() const;"), str::npos) << result.headerContent;
	EXPECT_NE(result.headerContent.find("void OnCollisionEnter(const ::Shalltear::Collision& collision) override;"), str::npos)
		<< result.headerContent;
	EXPECT_NE(result.headerContent.find("void OnTriggerEnter(::Shalltear::Handle<::Shalltear::Collider> other) override;"), str::npos)
		<< result.headerContent;

	EXPECT_NE(result.sourceContent.find("return MoveSpeed * 2.0f;"), str::npos) << result.sourceContent;
	EXPECT_NE(result.sourceContent.find("std::format(\"Speed={} Ratio={}\", MoveSpeed, GetSpeedRatio())"), str::npos)
		<< result.sourceContent;
	EXPECT_NE(result.sourceContent.find("::Shalltear::Input::IsKeyDown(::Shalltear::Key::Space)"), str::npos)
		<< result.sourceContent;
	EXPECT_NE(result.sourceContent.find("::Shalltear::Math::Clamp"), str::npos)
		<< result.sourceContent;
	EXPECT_NE(result.sourceContent.find("if ((TargetActor) != nullptr) { (TargetActor)->SetActive(true); }"), str::npos)
		<< result.sourceContent;
	EXPECT_NE(result.sourceContent.find("::Shalltear::GetStaticType<ModernSyntaxComponent>()"), str::npos)
		<< result.sourceContent;
	EXPECT_NE(result.sourceContent.find("if (::Shalltear::RaycastHit hit{}; ::Shalltear::Physics::Raycast(probeRay, hit, 50.0f))"), str::npos)
		<< result.sourceContent;
	EXPECT_NE(result.sourceContent.find("ModernSyntaxComponent* modern = dynamic_cast<ModernSyntaxComponent*>(anyComp)"), str::npos)
		<< result.sourceContent;
	EXPECT_NE(result.sourceContent.find("std::erase_if(ActiveTimers, [&](auto&& t) { return t <= 0.0f; })"), str::npos)
		<< result.sourceContent;
	EXPECT_NE(result.sourceContent.find("for (auto&& flag : ActiveFlags)"), str::npos)
		<< result.sourceContent;
}

class CSharpCompileIntegrationTest : public ::testing::Test {
protected:
	std::filesystem::path _root;
	str _config;
	str _sdkRoot;

	void SetUp() override {
		if (!CSharpTranspiler::IsRoslynLoaded()) {
			if (IsRunningInCi()) {
				FAIL() << "ShalltearRoslyn.dll must be staged in CI";
			}
			GTEST_SKIP() << "ShalltearRoslyn.dll is not staged";
		}
		_sdkRoot = ScriptProjectOps::GetSDKRoot();
		if (_sdkRoot.empty()) {
			GTEST_SKIP() << "no Shalltear CMake package found";
		}

		CppToolchain::DiscoveryOptions options;
		options.sdkRoot = _sdkRoot;
		CppToolchain::ToolchainInfo toolchain = CppToolchain::Discover(options);
		if (!toolchain.IsComplete()) {
			GTEST_SKIP() << "toolchain incomplete:\n"
						 << CppToolchain::Describe(toolchain);
		}

		static std::atomic<u64> counter{0};
		u64 stamp = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
		_root = std::filesystem::temp_directory_path() /
				("shalltear_csharp_" + std::to_string(stamp) + "_" + std::to_string(counter.fetch_add(1)));
		std::filesystem::create_directories(_root);
		FileSystem::Mount("project", Path(_root.generic_string()));

		std::filesystem::path engineAssets = RepoDir("EngineAssets");
		ASSERT_FALSE(engineAssets.empty());
		FileSystem::Mount("engine", Path(engineAssets.generic_string()));

		_config = str(ScriptProjectOps::GetActiveConfigName());
		SceneSystem::Initialize();
		ModuleSystem::Initialize();
	}

	void TearDown() override {
		if (IsSkipped()) {
			return;
		}
		SceneSystem::Terminate();
		ModuleSystem::Terminate();
		FileSystem::Unmount("engine");
		FileSystem::Unmount("project");
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	str Root() const { return _root.generic_string(); }

	void WriteFile(const str& relative, const str& contents) {
		std::filesystem::path target = _root / relative;
		std::error_code ec;
		std::filesystem::create_directories(target.parent_path(), ec);
		std::ofstream file(target, std::ios::binary | std::ios::trunc);
		file << contents;
	}

	bool RunCompile(CompileService& service) {
		CompileRequest request;
		request.projectPath = Root();
		if (!service.RequestCompile(request)) {
			return false;
		}
		auto deadline = std::chrono::steady_clock::now() +
						std::chrono::duration<f64>(COMPILE_TIMEOUT_SECONDS);
		while (service.IsBusy() && std::chrono::steady_clock::now() < deadline) {
			service.PumpMainThread();
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		service.PumpMainThread();
		return service.GetState() == CompileState::Succeeded;
	}
};

TEST_F(CSharpCompileIntegrationTest, CreateTranspileAndCleanStaleCSharpProjectFiles) {
	Config projectFile;
	projectFile.SetString("Project", "name", "CSharpOpsProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "CSharpOpsProbe";
	options.sdkRoot = _sdkRoot;
	options.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	EXPECT_FALSE(CSharpTranspiler::HasCSharpSources(Root()));

	ScriptProjectOps::ScaffoldResult csCreated;
	ASSERT_TRUE(ScriptProjectOps::CreateCSharpComponentSource(Root(), "", "PlayerMotor", csCreated));
	ASSERT_EQ(csCreated.written.size(), 1u);
	EXPECT_EQ(csCreated.written[0], "Source/PlayerMotor.cs");
	EXPECT_TRUE(CSharpTranspiler::HasCSharpSources(Root()));
	EXPECT_TRUE(CSharpTranspiler::IsGeneratedOutputOutOfDate(Root()));

	CSharpTranspiler::TranspileProjectResult firstTranspile;
	ASSERT_TRUE(CSharpTranspiler::TranspileProject(Root(), firstTranspile));
	EXPECT_TRUE(firstTranspile.succeeded);
	EXPECT_EQ(firstTranspile.sourceFileCount, 1u);
	EXPECT_FALSE(firstTranspile.written.empty());
	EXPECT_FALSE(CSharpTranspiler::IsGeneratedOutputOutOfDate(Root()));
	EXPECT_TRUE(std::filesystem::exists(_root / "CSharpOpsProbe.csproj"));
	EXPECT_TRUE(std::filesystem::exists(_root / "Library" / "Generated" / "CSharpStubs" / "Shalltear.Stubs.cs"));

	// Add a C++ component header and a second C# file that references both the first C# file and the C++ header
	WriteFile("Source/CppArmorComponent.h", R"(#pragma once
#include "Shalltear.h"

class CppArmorComponent : public ::Shalltear::Behavior {
public:
	SLT_REFLECT_COMPONENT(CppArmorComponent, ::Shalltear::Behavior)
	SLT_REFLECT_FIELD(armorRating)
	SLT_REFLECT_METHOD(RepairArmor)
	SLT_END_REFLECT()

	::Shalltear::f32 armorRating = 50.0f;
	void RepairArmor(::Shalltear::f32 amount) { armorRating += amount; }
};
)");

	WriteFile("Source/MotorConsumer.cs", R"(using Shalltear;

public class MotorConsumer : Behavior
{
	public PlayerMotor linkedMotor;
	public CppArmorComponent linkedArmor;

	public override void OnUpdate(float dt)
	{
		if (linkedArmor != null)
		{
			linkedArmor.RepairArmor(dt);
		}
	}
}
)");

	CSharpTranspiler::TranspileProjectResult crossFileTranspile;
	ASSERT_TRUE(CSharpTranspiler::TranspileProject(Root(), crossFileTranspile));
	EXPECT_EQ(crossFileTranspile.sourceFileCount, 2u);
	str consumerHeader = FileSystem::ReadTextFile(
		Path((_root / "Library" / "Generated" / "CSharp" / "MotorConsumer.generated.h").generic_string()),
		MemTag::Editor);
	EXPECT_NE(consumerHeader.find("#include \"PlayerMotor.generated.h\""), str::npos) << consumerHeader;
	EXPECT_NE(consumerHeader.find("#include \"CppArmorComponent.h\""), str::npos) << consumerHeader;
	EXPECT_NE(consumerHeader.find("::Shalltear::Handle<PlayerMotor> linkedMotor;"), str::npos) << consumerHeader;
	EXPECT_NE(consumerHeader.find("::Shalltear::Handle<CppArmorComponent> linkedArmor;"), str::npos) << consumerHeader;

	str generatedStubs = FileSystem::ReadTextFile(
		Path((_root / "Library" / "Generated" / "CSharpStubs" / "Shalltear.Stubs.cs").generic_string()),
		MemTag::Editor);
	EXPECT_NE(generatedStubs.find("public class CppArmorComponent : Shalltear.Behavior"), str::npos) << generatedStubs;
	EXPECT_NE(generatedStubs.find("public float armorRating;"), str::npos) << generatedStubs;
	EXPECT_NE(generatedStubs.find("public void RepairArmor(float amount)"), str::npos) << generatedStubs;

	std::filesystem::remove(_root / "Source" / "PlayerMotor.cs");
	std::filesystem::remove(_root / "Source" / "MotorConsumer.cs");
	CSharpTranspiler::TranspileProjectResult cleanupTranspile;
	ASSERT_TRUE(CSharpTranspiler::TranspileProject(Root(), cleanupTranspile));
	EXPECT_EQ(cleanupTranspile.sourceFileCount, 0u);
	EXPECT_FALSE(cleanupTranspile.removed.empty());
}

TEST_F(CSharpCompileIntegrationTest, EndToEndCompileExecuteAndHotReloadCSharpComponent) {
	Config projectFile;
	projectFile.SetString("Project", "name", "CSharpScriptProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "CSharpScriptProbe";
	options.sdkRoot = _sdkRoot;
	options.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	WriteFile("Source/SpinnerController.cs", R"(using Shalltear;

public class SpinnerController : Behavior
{
	[Range(0.0f, 360.0f)]
	public float degreesPerSecond = 60.0f;

	public int tickCount = 0;

	public override void OnEnable()
	{
		tickCount = 1;
	}

	public override void OnUpdate(float dt)
	{
		tickCount += 1;
		SceneObject owner = GetOwner().Get();
		if (owner != null)
		{
			Quat step = Quat.AngleAxis(degreesPerSecond * dt, Vec3.Up());
			owner.SetLocalRotation((owner.GetLocalRotation() * step).Normalized());
		}
	}

	[Callable]
	public float BoostSpeed(float bonus)
	{
		degreesPerSecond += bonus;
		return degreesPerSecond;
	}
}
)");

	CompileService service;
	service.RefreshToolchain();
	bool compiled = RunCompile(service);
	if (!compiled) {
		str diagnostics;
		for (const CompilerDiagnostic& diagnostic : service.GetDiagnostics()) {
			diagnostics += diagnostic.raw + "\n";
		}
		FAIL() << "C# project didn't compile:\n"
			   << diagnostics;
	}

	str binary = ScriptProjectOps::GetModuleBinaryPath(Root(), _config);
	ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(binary))) << binary;

	ModuleHandle module = ModuleSystem::Load(Path(binary));
	ASSERT_TRUE(ModuleSystem::IsLoaded(module));

	const TypeInfo* spinnerType = TypeRegistry::FindType(StringHash("SpinnerController"));
	ASSERT_NE(spinnerType, nullptr);

	Handle<Scene> scene = SceneSystem::CreateScene("CSharpScriptScene");
	Handle<SceneObject> object = SceneSystem::CreateObject("SpinnerActor", scene);
	Handle<Component> component = object.Get()->AddComponent(spinnerType);
	ASSERT_TRUE(component.IsValid());

	FrameTime clock;
	clock.deltaTime = 0.5f;
	clock.unscaledDeltaTime = 0.5f;
	auto tick = [&clock]() {
		Time::FrameScope scope{clock};
		SceneSystem::UpdateAll();
	};

	const Quat initialRot = object.Get()->GetLocalRotation();
	tick();
	EXPECT_NEAR(Quat::Angle(initialRot, object.Get()->GetLocalRotation()), 30.0f, 0.5f);

	Variant ticksAfterUpdate;
	ASSERT_TRUE(GetField(component.Get(), spinnerType, StringHash("tickCount"), &ticksAfterUpdate));
	ASSERT_NE(ticksAfterUpdate.TryGet<i32>(), nullptr);
	EXPECT_EQ(*ticksAfterUpdate.TryGet<i32>(), 2);

	Variant bonusArg(15.0f);
	Variant boostResult;
	ASSERT_TRUE(InvokeMethod(component.Get(), spinnerType, StringHash("BoostSpeed"), &bonusArg, 1, &boostResult));
	ASSERT_NE(boostResult.TryGet<f32>(), nullptr);
	EXPECT_FLOAT_EQ(*boostResult.TryGet<f32>(), 75.0f);

	// Modify C# script to add a new field and recompile + hot-reload
	WriteFile("Source/SpinnerController.cs", R"(using Shalltear;

public class SpinnerController : Behavior
{
	[Range(0.0f, 360.0f)]
	public float degreesPerSecond = 60.0f;

	public int tickCount = 0;

	public float altitudeOffset = 12.5f;

	public override void OnEnable()
	{
	}

	public override void OnUpdate(float dt)
	{
		tickCount += 10;
	}

	[Callable]
	public float BoostSpeed(float bonus)
	{
		degreesPerSecond += bonus;
		return degreesPerSecond;
	}
}
)");

	ASSERT_TRUE(RunCompile(service));
	ASSERT_TRUE(ModuleSystem::Reload(module));

	const TypeInfo* reloadedType = TypeRegistry::FindType(StringHash("SpinnerController"));
	ASSERT_NE(reloadedType, nullptr);
	ASSERT_TRUE(component.IsValid());

	Variant migratedSpeed;
	ASSERT_TRUE(GetField(component.Get(), reloadedType, StringHash("degreesPerSecond"), &migratedSpeed));
	ASSERT_NE(migratedSpeed.TryGet<f32>(), nullptr);
	EXPECT_FLOAT_EQ(*migratedSpeed.TryGet<f32>(), 75.0f);

	Variant migratedTicks;
	ASSERT_TRUE(GetField(component.Get(), reloadedType, StringHash("tickCount"), &migratedTicks));
	ASSERT_NE(migratedTicks.TryGet<i32>(), nullptr);
	EXPECT_EQ(*migratedTicks.TryGet<i32>(), 2);

	Variant addedAltitude;
	ASSERT_TRUE(GetField(component.Get(), reloadedType, StringHash("altitudeOffset"), &addedAltitude));
	ASSERT_NE(addedAltitude.TryGet<f32>(), nullptr);
	EXPECT_FLOAT_EQ(*addedAltitude.TryGet<f32>(), 12.5f);

	tick();

	Variant updatedTicks;
	ASSERT_TRUE(GetField(component.Get(), reloadedType, StringHash("tickCount"), &updatedTicks));
	ASSERT_NE(updatedTicks.TryGet<i32>(), nullptr);
	EXPECT_EQ(*updatedTicks.TryGet<i32>(), 12);
}

TEST_F(CSharpCompileIntegrationTest, StarterCSharpTemplateCompilesAndRotatesOwner) {
	Config projectFile;
	projectFile.SetString("Project", "name", "CSharpStarterProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "CSharpStarterProbe";
	options.sdkRoot = _sdkRoot;
	options.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	ScriptProjectOps::ScaffoldResult csComponent;
	ASSERT_TRUE(ScriptProjectOps::CreateCSharpComponentSource(Root(), "", "TurretRotator", csComponent));

	CompileService service;
	service.RefreshToolchain();
	ASSERT_TRUE(RunCompile(service));

	str binary = ScriptProjectOps::GetModuleBinaryPath(Root(), _config);
	ModuleHandle module = ModuleSystem::Load(Path(binary));
	ASSERT_TRUE(ModuleSystem::IsLoaded(module));

	const TypeInfo* turretType = TypeRegistry::FindType(StringHash("TurretRotator"));
	ASSERT_NE(turretType, nullptr);

	Handle<Scene> scene = SceneSystem::CreateScene("TurretScene");
	Handle<SceneObject> owner = SceneSystem::CreateObject("Turret", scene);
	Handle<Component> component = owner.Get()->AddComponent(turretType);
	ASSERT_TRUE(component.IsValid());

	FrameTime clock;
	clock.deltaTime = 0.5f;
	clock.unscaledDeltaTime = 0.5f;
	{
		Time::FrameScope scope{clock};
		SceneSystem::UpdateAll();
	}

	EXPECT_NEAR(Quat::Angle(Quat::Identity(), owner.Get()->GetLocalRotation()), 30.0f, 0.5f);
}

TEST_F(CSharpCompileIntegrationTest, AssetsDirectoryCSharpScriptWithoutHandlesCompilesAndExecutes) {
	Config projectFile;
	projectFile.SetString("Project", "name", "CSharpAssetsProbe");
	ASSERT_TRUE(projectFile.Save(Path(Root() + "/Project.ini")));

	ScriptProjectOps::ScaffoldOptions options;
	options.moduleName = "CSharpAssetsProbe";
	options.sdkRoot = _sdkRoot;
	options.editorExecutable = ScriptProjectOps::GetEditorExecutablePath();
	ScriptProjectOps::ScaffoldResult scaffolded;
	ASSERT_TRUE(ScriptProjectOps::ScaffoldScriptProject(Root(), options, scaffolded));

	std::filesystem::create_directories(_root / "Assets" / "Scripts");
	str createdRelPath;
	ASSERT_TRUE(AssetBrowserOps::CreateCSharpScript(Path("project://Assets/Scripts"), "NewCSharpScript", &createdRelPath));
	EXPECT_EQ(createdRelPath, "Assets/Scripts/NewCSharpScript.cs");
	ASSERT_TRUE(AssetBrowserOps::RenameEntry(Path("project://Assets/Scripts/NewCSharpScript.cs"), "AssetSpinner.cs"));
	EXPECT_TRUE(std::filesystem::exists(_root / "Assets" / "Scripts" / "AssetSpinner.cs"));
	EXPECT_TRUE(CSharpTranspiler::HasCSharpSources(Root()));
	EXPECT_EQ(EditorPreviews::BadgeForFileName("AssetSpinner.cs").icon.glyph, EditorIcons::Glyph::AssetShader.glyph);

	CompileService service;
	service.RefreshToolchain();
	ASSERT_TRUE(RunCompile(service));

	str binary = ScriptProjectOps::GetModuleBinaryPath(Root(), _config);
	ModuleHandle module = ModuleSystem::Load(Path(binary));
	ASSERT_TRUE(ModuleSystem::IsLoaded(module));

	const TypeInfo* spinnerType = TypeRegistry::FindType(StringHash("AssetSpinner"));
	ASSERT_NE(spinnerType, nullptr);
	EXPECT_EQ(AssetBrowserOps::ResolveScriptComponentType("Assets/Scripts/AssetSpinner.cs"), StringHash("AssetSpinner"));

	Handle<Scene> scene = SceneSystem::CreateScene("AssetSpinnerScene");
	Handle<SceneObject> owner = SceneSystem::CreateObject("AssetSpinnerActor", scene);
	Handle<Component> component = owner.Get()->AddComponent(spinnerType);
	ASSERT_TRUE(component.IsValid());

	FrameTime clock;
	clock.deltaTime = 0.5f;
	clock.unscaledDeltaTime = 0.5f;
	{
		Time::FrameScope scope{clock};
		SceneSystem::UpdateAll();
	}

	EXPECT_NEAR(Quat::Angle(Quat::Identity(), owner.Get()->GetLocalRotation()), 30.0f, 0.5f);
}
