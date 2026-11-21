using System;
using System.Threading;

using Shalltear.Interop;

namespace Shalltear.Conformance;

static class ExtensionTests {
	[ConformanceTest]
	internal static void VariantsHoldValues() {
		using Variant number = Variant.From(42);
		Check.True(number.TryGet(out int held) && held == 42, "int");
		Check.True(!number.TryGet(out float _), "another type");
		Check.True(!number.IsEmpty, "has a value");
		using Variant text = Variant.From("привет");
		Check.True(text.TryGet(out string word) && word == "привет", "text");
		using Variant point = Variant.From(new Vec3(1f, 2f, 3f));
		Check.True(point.TryGet(out Vec3 at) && at.y == 2f, "Vec3");
		using Variant empty = new();
		Check.True(empty.IsEmpty, "empty");
	}

	[ConformanceTest]
	internal static void JobsRunDelegates() {
		if (JobSystem.WorkerCount == 0) {
			return;
		}
		int ran = 0;
		JobHandle first = JobSystem.Submit(() => Interlocked.Increment(ref ran));
		JobHandle second = JobSystem.Submit(() => Interlocked.Add(ref ran, 10), [first]);
		JobSystem.WaitFor(second);
		Check.Equal(11, Volatile.Read(ref ran));
		long total = 0;
		JobSystem.ParallelFor(2, 100, (lane, begin, end) => Interlocked.Add(ref total, end - begin));
		Check.Equal(100L, Interlocked.Read(ref total));
	}

	[ConformanceTest]
	internal static void AddressesReachObjects() {
		SceneObject made = BindingProbe.CreateObject("Addressed");
		ulong address = Addresses.Of(made);
		Check.True(address != 0 && made.InstanceId != 0, "address + id");
		Check.True(Addresses.ObjectAt<SceneObject>(address) == made, "same object");
		Check.True(Addresses.ObjectAt<SceneObject>(0) == null, "no object at 0");
		made.Destroy();
		BindingProbe.EndFrame();
		Check.True(!made.IsAlive, "destroyed");
	}

	[ConformanceTest]
	internal static void DerivedTypesCreateWiredObjects() {
		int wired = 0;
		TypeInfo? type = RenderPassProxy.Register(
			"Conformance.ProbePass",
			pass => {
				++wired;
				pass.SetGetRequests(static () => PassRequests.SceneDepth);
			},
			static builder => builder.AddAttribute("ProbeNote", Variant.From("drawn by a script")));
		Check.True(type is not null, "registered");
		Check.True(type!.Equals(TypeRegistry.FindType("Conformance.ProbePass")), "found by name");
		EngineObject? made = TypeRegistry.CreateObjectInstance(type, MemTag.Scripting);
		Check.Equal(1, wired);
		Check.True(made is RenderPass pass && pass.Requests == PassRequests.SceneDepth, "the script answers a virtual call");
		TypeRegistry.DestroyObjectInstance(made);
	}

	[ConformanceTest]
	internal static void LogSinkReadsEventsOnLogThread() {
		int main = Environment.CurrentManagedThreadId;
		string? message = null;
		LogLevel level = LogLevel.None;
		bool offMain = false;
		LogSinkProxy sink = new();
		sink.SetReceiveLog(logEvent => {
			if (logEvent.Category == "SinkCase") {
				message = logEvent.Message;
				level = logEvent.Level;
				offMain = Environment.CurrentManagedThreadId != main;
			}
		});
		LogManager.AddSink(sink.Base);
		Log.Warn("SinkCase", "read on the log thread");
		LogManager.DrainBeforeImageUnload();
		Check.True(LogManager.RemoveSink(sink.Base), "sink removed");
		Check.Equal("read on the log thread", message);
		Check.Equal(LogLevel.Warn, level);
		Check.True(offMain, "sink called on the log thread");
	}

	static bool Present(EngineObject? value) {
		return value != null;
	}

	static string Refused(Action call) {
		try {
			call();
		} catch (Exception exception) {
			return exception.Message;
		}
		return "allowed";
	}

	[ConformanceTest]
	internal static void ViewFieldsHoldTheirText() {
		ulong before = SlotProbe.KeptFieldCount();
		using (RHIBufferDesc buffer = new()) {
			buffer.DebugName = "first name";
			buffer.DebugName = "vertex data";
			Check.Equal("vertex data", buffer.DebugName);
			Check.Equal(before + 1UL, SlotProbe.KeptFieldCount());
		}
		Check.Equal(before, SlotProbe.KeptFieldCount());
		using UIWidgetsButtonDesc button = new();
		button.Label = "Play";
		Check.Equal("Play", button.Label);
		using UIWidgetsDropdownDesc dropdown = new();
		dropdown.Items = ["Low", "High"];
		Check.Equal("Low|High", string.Join("|", dropdown.Items));
		using UIWidgetsVectorFieldDesc vector = new();
		Check.Equal("X|Y|Z|W", string.Join("|", vector.Labels));
		vector.Labels = ["U", "V"];
		Check.Equal("U|V||", string.Join("|", vector.Labels));
		using UIDocumentNode node = new();
		node.Classes = ["panel", "wide"];
		Check.Equal("panel|wide||", string.Join("|", node.Classes));
		Check.True(Refused(() => node.Classes = ["a", "b", "c", "d", "e"]).Contains("at most 4"), "5 classes refused");
	}

	[ConformanceTest]
	internal static void SpanFieldsHoldTheirItems() {
		ulong before = SlotProbe.KeptFieldCount();
		using RHIGraphicsPipelineDesc pipeline = new();
		pipeline.VertexSpirv = [3, 2, 0x23, 7];
		Check.Equal("3|2|35|7", string.Join("|", pipeline.VertexSpirv));
		using ViewDesc view = new();
		Scene? primary = SceneSystem.PrimaryScene;
		Check.True(Present(primary), "a primary scene");
		view.Scenes = [primary!];
		Check.True(view.Scenes.Length == 1 && view.Scenes[0] == primary, "scene handle kept");
		view.Time = 0x40UL;
		Check.Equal(0x40UL, view.Time);
		view.Time = 0UL;
		using UIWidgetDesc widget = new();
		ulong mark = SlotProbe.KeptFieldCount();
		UIWidgetAttribute size = new();
		size.Name = "size";
		size.Choices = ["small", "large"];
		widget.Attributes = [size];
		Check.Equal(mark + 5UL, SlotProbe.KeptFieldCount());
		size.Dispose();
		Check.Equal(mark + 3UL, SlotProbe.KeptFieldCount());
		UIWidgetAttribute copied = widget.Attributes[0];
		Check.Equal("size", copied.Name);
		Check.Equal("small|large", string.Join("|", copied.Choices));
		widget.Attributes = [];
		Check.Equal(0, widget.Attributes.Length);
		Check.Equal(mark + 1UL, SlotProbe.KeptFieldCount());
		Check.True(mark > before, "pipeline + view fields held");
	}

	[ConformanceTest]
	internal static void ScriptImporterMakesAssets() {
		string seen = "";
		AssetImporterProxy importer = new();
		importer.SetGetAssetType(() => new StringHash("TextAsset"));
		importer.SetImport(path => {
			seen = path;
			TextAsset made = (TextAsset)TypeRegistry.CreateObjectInstance(TypeRegistry.FindType(new StringHash("TextAsset")), MemTag.Asset)!;
			made.Content = "made by a script";
			return made;
		});
		AssetImporterRegistry.Register(new StringHash("ScriptProbeText"), importer.Base, [".scriptprobe"]);
		string name = "Note" + Guid.NewGuid().ToString("N") + ".scriptprobe";
		Check.True(FileSystem.WriteTextFile("project://Assets/" + name, "source"), "source written");
		Asset? loaded = AssetDatabase.LoadAsset("project://Assets/" + name);
		Check.True(loaded is TextAsset, "imported by the script");
		Check.Equal("made by a script", ((TextAsset)loaded!).Content);
		Check.True(seen.EndsWith(name, StringComparison.Ordinal), "importer saw the source");
		AssetDatabase.Unload(loaded!);
		AssetImporterRegistry.Unregister(importer.Base);
		importer.Dispose();
	}

	[ConformanceTest]
	internal static void LogEventFieldsTakeWrites() {
		using LogEvent logEvent = new();
		logEvent.Timestamp = 1700000000000L;
		Check.Equal(1700000000000L, logEvent.Timestamp);
		logEvent.SetThreadIdToCurrent();
		Check.True(logEvent.ThreadId != 0UL, "thread id set");
	}
}
