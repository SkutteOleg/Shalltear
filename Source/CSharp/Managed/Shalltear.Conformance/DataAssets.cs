using System;
using System.Collections.Generic;

using Shalltear.Interop;

namespace Shalltear.Conformance;

[AssetMenu("Conformance/Recipe", ".recipe")]
sealed class Recipe : DataAsset {
	public int servings = 2;
	public string title = "Soup";
	public float weight = 1.5f;
	public Material? plate;
	public List<string> steps = new();
}

abstract class Dish : DataAsset {
	public int calories = 100;
}

[AssetMenu("Conformance/Dessert", ".dessert")]
sealed class Dessert : Dish {
	public bool frozen;
	public Recipe? sauce;
}

sealed class Kitchen : Behavior {
	public Recipe? recipe;
	public Dessert? dessert;
}

static class DataAssetProbe {
	[ConformanceTest]
	internal static void EditsSoup() {
		Recipe soup = AnimationTests.Stored<Recipe>(DataAssetTests.SOUP);
		soup.servings = 9;
		BindingProbe.Record("id " + soup._id);
	}

	[ConformanceTest]
	internal static void ReportsSoup() {
		Recipe soup = AnimationTests.Stored<Recipe>(DataAssetTests.SOUP);
		BindingProbe.Record("servings " + soup.servings + " id " + soup._id);
		soup.servings = DataAssetTests.SERVINGS;
	}
}

static class DataAssetTests {
	internal const string SOUP = "project://Assets/Soup.recipe";
	internal const int SERVINGS = 4;
	const string GLOW = "project://Assets/Glow.mat";

	static bool Gone(EngineObject? value) {
		return value == null;
	}

	static string Refusal(Action call) {
		try {
			call();
		} catch (EngineException exception) {
			return exception.Message;
		}
		return "allowed";
	}

	[ConformanceTest]
	internal static void DataAssetsLoadFromTheirFiles() {
		Recipe soup = AnimationTests.Stored<Recipe>(SOUP);
		Check.Equal(SERVINGS, soup.servings);
		Check.Equal("Stew", soup.title);
		Check.Equal(2.5f, soup.weight);
		Check.Same(AnimationTests.Stored<Material>(GLOW), soup.plate);
		Check.Equal(2, soup.steps.Count);
		Check.Equal("boil", soup.steps[1]);
		Check.Same(soup, AnimationTests.Stored<Recipe>(SOUP));
		Check.Equal(SOUP, soup.Path);
		Check.Equal(AssetLoadState.Loaded, soup.LoadState);
		Check.True(soup.AssetId.Length > 0 && AssetDatabase.LookupPath(soup.AssetId) == SOUP, "asset id");
		Check.Equal(soup.AssetId, AssetDatabase.LookupAssetId(SOUP));
	}

	[ConformanceTest]
	internal static void DataAssetsDeclareTheirImporter() {
		Check.Equal("Conformance/Recipe", BindingProbe.ImporterLabel(".recipe"));
		Check.Equal("New Recipe.recipe", BindingProbe.ImporterFileName(".recipe"));
		Check.Equal("Conformance/Dessert", BindingProbe.ImporterLabel(".dessert"));
		Check.Equal("", BindingProbe.ImporterLabel(".dish"));
	}

	[ConformanceTest]
	internal static void DataAssetsRoundTripThroughFiles() {
		string path = "project://Assets/Pudding" + Environment.ProcessId + "_" + Environment.TickCount64 + ".dessert";
		Dessert made = AssetDatabase.Create<Dessert>(path);
		Check.Equal(100, made.calories);
		Check.True(!made.frozen && Gone(made.sauce), "defaults");
		Check.Equal(path, made.Path);
		Check.Equal(1, made.RefCount);
		made.calories = 250;
		made.frozen = true;
		made.sauce = AnimationTests.Stored<Recipe>(SOUP);
		AssetDatabase.Save(made);
		AssetDatabase.Unload(made);
		Check.True(!Gone(made), "parked until collected");
		Check.True(AssetDatabase.CollectGarbage() >= 1, "collected");
		Check.True(Gone(made), "freed");

		Dessert again = AssetDatabase.Load<Dessert>(path)!;
		Check.Equal(250, again.calories);
		Check.True(again.frozen, "saved flag");
		Check.Same(AnimationTests.Stored<Recipe>(SOUP), again.sauce);
		Check.True(Gone(AssetDatabase.Load<Recipe>(path)), "not a Recipe");
		Check.Equal(1, again.RefCount);
		Check.Equal("AssetDatabase.Create got '" + path + "', an existing file. Load it (or pick another path)",
					Refusal(() => AssetDatabase.Create<Dessert>(path)));
		Check.Equal("AssetDatabase.Create got 'project://Assets/Tart.recipe', a path with another type's extension. Use the type's extension",
					Refusal(() => AssetDatabase.Create<Dessert>("project://Assets/Tart.recipe")));
		Check.True(Gone(AssetDatabase.Load<Dessert>("project://Assets/Missing.dessert")), "no file");
		AssetDatabase.Unload(again);
	}

	[ConformanceTest]
	internal static void EngineAssetsLoadAndRelease() {
		Material glow = AnimationTests.Stored<Material>(GLOW);
		int held = glow.RefCount;
		Material again = AssetDatabase.Load<Material>(GLOW)!;
		Check.Same(glow, again);
		Check.Equal(held + 1, glow.RefCount);
		Check.True(Gone(AssetDatabase.Load<Texture>(GLOW)), "not a Texture");
		Check.Equal(held + 1, glow.RefCount);
		AssetDatabase.Unload(again);
		Check.Equal(held, glow.RefCount);
		ulong looseId = BindingProbe.MakeAsset("TextureAsset");
		Texture loose = ObjectCache.Find<Texture>(looseId)!;
		Check.Equal("AssetDatabase.Save got an asset with no file. Make it with AssetDatabase.Create", Refusal(() => AssetDatabase.Save(loose)));
		BindingProbe.FreeAsset(looseId);
	}

	[ConformanceTest]
	internal static void ScriptFieldsHoldDataAssets() {
		SceneObject source = SceneSystem.CreateObject("Pantry");
		Kitchen kitchen = source.AddComponent<Kitchen>();
		kitchen.recipe = AnimationTests.Stored<Recipe>(SOUP);
		SceneObject copy = SceneSystem.Instantiate(source);
		Check.Same(kitchen.recipe, copy.GetComponent<Kitchen>()!.recipe);
		source.SetActive(false);
		source.Destroy();
		copy.SetActive(false);
		copy.Destroy();
	}
}
