using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Shalltear.Roslyn;

public sealed class DiagnosticDto
{
    public string Severity { get; set; } = "error";
    public string File { get; set; } = "";
    public int Line { get; set; } = 1;
    public int Column { get; set; } = 1;
    public string Message { get; set; } = "";
    public string Raw { get; set; } = "";
}

public sealed class FileTranspileDto
{
    public bool Succeeded { get; set; }
    public string SourcePath { get; set; } = "";
    public string RelativePath { get; set; } = "";
    public string HeaderContent { get; set; } = "";
    public string SourceContent { get; set; } = "";
    public List<string> ReflectedTypes { get; set; } = new();
    public List<DiagnosticDto> Diagnostics { get; set; } = new();
}

public sealed class ProjectTranspileDto
{
    public bool Succeeded { get; set; }
    public string GeneratedStubsContent { get; set; } = "";
    public List<FileTranspileDto> Files { get; set; } = new();
    public List<DiagnosticDto> Diagnostics { get; set; } = new();
}

public static unsafe class RoslynTranspilerExports
{
    private static readonly object SyncLock = new();
    private static List<MetadataReference>? _cachedReferences;
    private static SyntaxTree? _cachedStubsTree;

    [UnmanagedCallersOnly(EntryPoint = "ShalltearRoslyn_FreeBuffer")]
    public static void FreeBuffer(byte* ptr)
    {
        if (ptr != null)
        {
            Marshal.FreeCoTaskMem((IntPtr)ptr);
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "ShalltearRoslyn_GetEngineStubs")]
    public static int GetEngineStubs(byte** outUtf8)
    {
        if (outUtf8 == null)
        {
            return 0;
        }
        *outUtf8 = AllocateUtf8(RoslynCompilerEngine.GetEngineStubsText());
        return 1;
    }

    [UnmanagedCallersOnly(EntryPoint = "ShalltearRoslyn_TranspileSource")]
    public static int TranspileSource(byte* sourcePathUtf8, byte* cSharpTextUtf8, byte** outJsonUtf8)
    {
        if (outJsonUtf8 == null)
        {
            return 0;
        }

        try
        {
            string sourcePath = PtrToUtf8(sourcePathUtf8);
            string cSharpText = PtrToUtf8(cSharpTextUtf8);

            FileTranspileDto result = RoslynCompilerEngine.TranspileSingle(sourcePath, cSharpText);
            string json = JsonSerializer.Serialize(result);
            *outJsonUtf8 = AllocateUtf8(json);
            return result.Succeeded ? 1 : 0;
        }
        catch (Exception ex)
        {
            var fallback = new FileTranspileDto
            {
                Succeeded = false,
                Diagnostics =
                {
                    new DiagnosticDto
                    {
                        Severity = "error",
                        File = PtrToUtf8(sourcePathUtf8),
                        Line = 1,
                        Column = 1,
                        Message = "Roslyn internal error: " + ex.Message,
                        Raw = $"{PtrToUtf8(sourcePathUtf8)}:1:1: error: Roslyn internal error: {ex.Message}"
                    }
                }
            };
            *outJsonUtf8 = AllocateUtf8(JsonSerializer.Serialize(fallback));
            return 0;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "ShalltearRoslyn_TranspileProject")]
    public static int TranspileProject(byte* projectPathUtf8, byte** outJsonUtf8)
    {
        if (outJsonUtf8 == null)
        {
            return 0;
        }

        try
        {
            string projectPath = PtrToUtf8(projectPathUtf8);
            ProjectTranspileDto result = RoslynCompilerEngine.TranspileProjectDirectory(projectPath);
            string json = JsonSerializer.Serialize(result);
            *outJsonUtf8 = AllocateUtf8(json);
            return result.Succeeded ? 1 : 0;
        }
        catch (Exception ex)
        {
            var fallback = new ProjectTranspileDto
            {
                Succeeded = false,
                Diagnostics =
                {
                    new DiagnosticDto
                    {
                        Severity = "error",
                        File = PtrToUtf8(projectPathUtf8),
                        Line = 1,
                        Column = 1,
                        Message = "Roslyn project error: " + ex.Message,
                        Raw = $"{PtrToUtf8(projectPathUtf8)}:1:1: error: Roslyn project error: {ex.Message}"
                    }
                }
            };
            *outJsonUtf8 = AllocateUtf8(JsonSerializer.Serialize(fallback));
            return 0;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "ShalltearRoslyn_GenerateEngineStubsCache")]
    public static int GenerateEngineStubsCache(byte* repoRootUtf8, byte* outputPathUtf8)
    {
        try
        {
            string repoRoot = PtrToUtf8(repoRootUtf8);
            string outputPath = PtrToUtf8(outputPathUtf8);
            return RoslynCompilerEngine.GenerateEngineStubsCacheFile(repoRoot, outputPath) ? 1 : 0;
        }
        catch
        {
            return 0;
        }
    }

    internal static (List<MetadataReference> References, SyntaxTree StubsTree) GetWarmCompilationInputs()
    {
        lock (SyncLock)
        {
            if (_cachedReferences == null)
            {
                var refs = new List<MetadataReference>();
                var trusted = AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") as string;
                if (!string.IsNullOrEmpty(trusted))
                {
                    var wanted = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
                    {
                        "System.Private.CoreLib.dll",
                        "System.Runtime.dll",
                        "System.Collections.dll",
                        "System.Linq.dll",
                        "System.Console.dll",
                        "netstandard.dll"
                    };
                    foreach (string path in trusted.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
                    {
                        string fileName = Path.GetFileName(path);
                        if (wanted.Contains(fileName) && File.Exists(path))
                        {
                            refs.Add(MetadataReference.CreateFromFile(path));
                        }
                    }
                }
                if (refs.Count == 0 && !string.IsNullOrEmpty(typeof(object).Assembly.Location))
                {
                    refs.Add(MetadataReference.CreateFromFile(typeof(object).Assembly.Location));
                }
                _cachedReferences = refs;
            }

            _cachedStubsTree ??= CSharpSyntaxTree.ParseText(
                RoslynCompilerEngine.GetEngineStubsText(),
                CSharpParseOptions.Default.WithLanguageVersion(LanguageVersion.Preview),
                path: "Shalltear.Stubs.cs");

            return (_cachedReferences, _cachedStubsTree);
        }
    }

    private static string PtrToUtf8(byte* ptr)
    {
        if (ptr == null)
        {
            return "";
        }
        return Marshal.PtrToStringUTF8((IntPtr)ptr) ?? "";
    }

    private static byte* AllocateUtf8(string text)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(text);
        IntPtr mem = Marshal.AllocCoTaskMem(bytes.Length + 1);
        Marshal.Copy(bytes, 0, mem, bytes.Length);
        Marshal.WriteByte(mem, bytes.Length, 0);
        return (byte*)mem;
    }
}

internal static class RoslynCompilerEngine
{
    private static readonly Dictionary<string, string> PrimitiveMap = new(StringComparer.Ordinal)
    {
        ["float"] = "::Shalltear::f32",
        ["Single"] = "::Shalltear::f32",
        ["System.Single"] = "::Shalltear::f32",
        ["double"] = "::Shalltear::f64",
        ["Double"] = "::Shalltear::f64",
        ["System.Double"] = "::Shalltear::f64",
        ["int"] = "::Shalltear::i32",
        ["Int32"] = "::Shalltear::i32",
        ["System.Int32"] = "::Shalltear::i32",
        ["uint"] = "::Shalltear::u32",
        ["UInt32"] = "::Shalltear::u32",
        ["System.UInt32"] = "::Shalltear::u32",
        ["long"] = "::Shalltear::i64",
        ["Int64"] = "::Shalltear::i64",
        ["System.Int64"] = "::Shalltear::i64",
        ["ulong"] = "::Shalltear::u64",
        ["UInt64"] = "::Shalltear::u64",
        ["System.UInt64"] = "::Shalltear::u64",
        ["short"] = "::Shalltear::i16",
        ["Int16"] = "::Shalltear::i16",
        ["System.Int16"] = "::Shalltear::i16",
        ["ushort"] = "::Shalltear::u16",
        ["UInt16"] = "::Shalltear::u16",
        ["System.UInt16"] = "::Shalltear::u16",
        ["sbyte"] = "::Shalltear::i8",
        ["SByte"] = "::Shalltear::i8",
        ["System.SByte"] = "::Shalltear::i8",
        ["byte"] = "::Shalltear::u8",
        ["Byte"] = "::Shalltear::u8",
        ["System.Byte"] = "::Shalltear::u8",
        ["bool"] = "bool",
        ["Boolean"] = "bool",
        ["System.Boolean"] = "bool",
        ["string"] = "::Shalltear::str",
        ["String"] = "::Shalltear::str",
        ["System.String"] = "::Shalltear::str",
        ["void"] = "void",
        ["System.Void"] = "void",
        ["var"] = "auto"
    };

    private static readonly HashSet<string> EngineTypeNames = new(StringComparer.Ordinal);
    private static readonly HashSet<string> ObjectDerivedEngineTypes = new(StringComparer.Ordinal);
    private static readonly HashSet<string> StaticScopeIdentifiers = new(StringComparer.Ordinal);
    private static readonly HashSet<string> LifecycleMethodNames = new(StringComparer.Ordinal);
    private static readonly HashSet<string> PhysicsCallbackMethodNames = new(StringComparer.Ordinal);
    private static readonly Dictionary<string, string> TypeToModuleUmbrellaHeader = new(StringComparer.Ordinal);

    private static readonly HashSet<string> CSharpKeywords = new(StringComparer.Ordinal)
    {
        "abstract", "as", "base", "bool", "break", "byte", "case", "catch", "char", "checked",
        "class", "const", "continue", "decimal", "default", "delegate", "do", "double", "else",
        "enum", "event", "explicit", "extern", "false", "finally", "fixed", "float", "for",
        "foreach", "goto", "if", "implicit", "in", "int", "interface", "internal", "is", "lock",
        "long", "namespace", "new", "null", "object", "operator", "out", "override", "params",
        "private", "protected", "public", "readonly", "ref", "return", "sbyte", "sealed",
        "short", "sizeof", "stackalloc", "static", "string", "struct", "switch", "this", "throw",
        "true", "try", "typeof", "uint", "ulong", "unchecked", "unsafe", "ushort", "using",
        "virtual", "void", "volatile", "while"
    };

    private static readonly HashSet<string> UnsupportedLinqMethods = new(StringComparer.Ordinal)
    {
        "Where", "Select", "SelectMany", "OrderBy", "OrderByDescending", "ThenBy", "ThenByDescending",
        "GroupBy", "ToList", "ToArray", "ToDictionary", "ToHashSet", "FirstOrDefault", "SingleOrDefault"
    };

    private static readonly HashSet<string> HandleIntrinsicMembers = new(StringComparer.Ordinal)
    {
        "Get", "IsValid", "GetInstanceId", "Reset"
    };

    private static readonly HashSet<string> SupportedListMembers = new(StringComparer.Ordinal)
    {
        "Add", "Clear", "Count", "RemoveAt", "RemoveAll", "Exists"
    };

    internal sealed class CppDiscoveredMethod
    {
        public string ReturnType { get; init; } = "void";
        public string Name { get; init; } = "";
        public string Params { get; init; } = "";
        public string DedupKey { get; init; } = "";
        public bool IsStatic { get; init; }
        public bool IsProtectedVirtual { get; init; }
        public bool IsGenericComponentMethod { get; init; }
        public bool IsConstructor { get; init; }
        public bool IsOperator { get; init; }
    }

    internal sealed class CppDiscoveredType
    {
        public string Name { get; init; } = "";
        public string Kind { get; set; } = "class"; // "class", "struct", "enum", "static_class"
        public string BaseName { get; set; } = "";
        public string HeaderRelPath { get; init; } = "";
        public string UmbrellaHeader { get; init; } = "";
        public List<(string Type, string Name)> Fields { get; } = new();
        public List<(string Type, string Name, string Value)> Constants { get; } = new();
        public List<CppDiscoveredMethod> Methods { get; } = new();
        public List<string> EnumValues { get; } = new();
    }

    internal sealed class ComputedPropertyInfo
    {
        public string Name { get; init; } = "";
        public string RawType { get; init; } = "";
        public bool IsStatic { get; init; }
        public ArrowExpressionClauseSyntax? ExpressionGetter { get; init; }
        public AccessorDeclarationSyntax? GetAccessor { get; init; }
        public AccessorDeclarationSyntax? SetAccessor { get; init; }
    }

    internal sealed class ProjectTypeIndex
    {
        public HashSet<string> UserObjectTypes { get; } = new(StringComparer.Ordinal);
        public HashSet<string> UserValueStructs { get; } = new(StringComparer.Ordinal);
        public HashSet<string> UserEnumNames { get; } = new(StringComparer.Ordinal);
        public Dictionary<string, string> TypeToGeneratedHeader { get; } = new(StringComparer.Ordinal);
        public Dictionary<string, HashSet<string>> TypeToComputedProperties { get; } = new(StringComparer.Ordinal);
        public List<CppDiscoveredType> CppTypes { get; } = new();
    }

    public static FileTranspileDto TranspileSingle(string sourcePath, string cSharpText)
    {
        string normalizedPath = NormalizeSlashes(sourcePath);
        var (refs, stubsTree) = RoslynTranspilerExports.GetWarmCompilationInputs();
        var parseOptions = CSharpParseOptions.Default.WithLanguageVersion(LanguageVersion.Preview);
        SyntaxTree userTree = CSharpSyntaxTree.ParseText(cSharpText, parseOptions, path: normalizedPath);

        var dto = new FileTranspileDto
        {
            SourcePath = normalizedPath,
            RelativePath = Path.GetFileName(normalizedPath)
        };

        if (!CollectSyntaxDiagnostics(userTree, dto.Diagnostics))
        {
            dto.Succeeded = false;
            return dto;
        }

        var compilation = CSharpCompilation.Create(
            "ShalltearScriptAssembly",
            new[] { stubsTree, userTree },
            refs,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));

        ProjectTypeIndex index = BuildProjectTypeIndex(new[] { (userTree, dto.RelativePath) }, Array.Empty<CppDiscoveredType>());
        SemanticModel semanticModel = compilation.GetSemanticModel(userTree);

        if (!EmitTranspiledFile(normalizedPath, dto.RelativePath, userTree, semanticModel, index, dto))
        {
            dto.Succeeded = false;
            return dto;
        }

        dto.Succeeded = true;
        return dto;
    }

    public static ProjectTranspileDto TranspileProjectDirectory(string projectPath)
    {
        var projectDto = new ProjectTranspileDto();
        string sourceDir = Path.Combine(projectPath, "Source");
        List<CppDiscoveredType> cppTypes = ScanCppHeadersInDirectory(sourceDir, isEngineRoot: false);
        string combinedStubs = BuildCombinedStubsText(cppTypes);
        projectDto.GeneratedStubsContent = combinedStubs;

        if (!Directory.Exists(sourceDir))
        {
            projectDto.Succeeded = true;
            return projectDto;
        }

        string[] csFiles = Directory.GetFiles(sourceDir, "*.cs", SearchOption.AllDirectories);
        Array.Sort(csFiles, StringComparer.Ordinal);
        if (csFiles.Length == 0)
        {
            projectDto.Succeeded = true;
            return projectDto;
        }

        var (refs, defaultStubsTree) = RoslynTranspilerExports.GetWarmCompilationInputs();
        var parseOptions = CSharpParseOptions.Default.WithLanguageVersion(LanguageVersion.Preview);
        SyntaxTree stubsTree = cppTypes.Count == 0
            ? defaultStubsTree
            : CSharpSyntaxTree.ParseText(combinedStubs, parseOptions, path: "Shalltear.Stubs.cs");

        var userTrees = new List<(SyntaxTree Tree, string FullPath, string RelativePath)>(csFiles.Length);

        foreach (string csFile in csFiles)
        {
            string fullGeneric = NormalizeSlashes(csFile);
            string relGeneric = NormalizeSlashes(Path.GetRelativePath(sourceDir, csFile));
            string code = File.ReadAllText(csFile, Encoding.UTF8);
            SyntaxTree tree = CSharpSyntaxTree.ParseText(code, parseOptions, path: fullGeneric);
            if (!CollectSyntaxDiagnostics(tree, projectDto.Diagnostics))
            {
                projectDto.Succeeded = false;
                return projectDto;
            }
            userTrees.Add((tree, fullGeneric, relGeneric));
        }

        var allTrees = new List<SyntaxTree>(userTrees.Count + 1) { stubsTree };
        allTrees.AddRange(userTrees.Select(t => t.Tree));

        var compilation = CSharpCompilation.Create(
            "ShalltearProjectAssembly",
            allTrees,
            refs,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));

        ProjectTypeIndex index = BuildProjectTypeIndex(userTrees.Select(t => (t.Tree, t.RelativePath)), cppTypes);

        foreach (var (tree, fullPath, relPath) in userTrees)
        {
            var fileDto = new FileTranspileDto
            {
                SourcePath = fullPath,
                RelativePath = relPath
            };
            SemanticModel semanticModel = compilation.GetSemanticModel(tree);
            if (!EmitTranspiledFile(fullPath, relPath, tree, semanticModel, index, fileDto))
            {
                projectDto.Diagnostics.AddRange(fileDto.Diagnostics);
                projectDto.Succeeded = false;
                return projectDto;
            }
            projectDto.Files.Add(fileDto);
        }

        projectDto.Succeeded = true;
        return projectDto;
    }

    private sealed class EngineStubsCacheDto
    {
        public string StubsText { get; set; } = "";
        public List<string> EngineTypeNames { get; set; } = new();
        public List<string> ObjectDerivedEngineTypes { get; set; } = new();
        public List<string> StaticScopeIdentifiers { get; set; } = new();
        public List<string> LifecycleMethodNames { get; set; } = new();
        public List<string> PhysicsCallbackMethodNames { get; set; } = new();
        public Dictionary<string, string> TypeToModuleUmbrellaHeader { get; set; } = new(StringComparer.Ordinal);
    }

    private static string? _cachedEngineStubsText;
    private static readonly object EngineScanLock = new();

    public static bool GenerateEngineStubsCacheFile(string repoRoot, string outputPath)
    {
        lock (EngineScanLock)
        {
            _cachedEngineStubsText = null;
            string stubsText = ScanAndPopulateEngineStubs(repoRoot);
            if (string.IsNullOrEmpty(stubsText))
            {
                return false;
            }

            var dto = new EngineStubsCacheDto
            {
                StubsText = stubsText,
                EngineTypeNames = EngineTypeNames.OrderBy(s => s, StringComparer.Ordinal).ToList(),
                ObjectDerivedEngineTypes = ObjectDerivedEngineTypes.OrderBy(s => s, StringComparer.Ordinal).ToList(),
                StaticScopeIdentifiers = StaticScopeIdentifiers.OrderBy(s => s, StringComparer.Ordinal).ToList(),
                LifecycleMethodNames = LifecycleMethodNames.OrderBy(s => s, StringComparer.Ordinal).ToList(),
                PhysicsCallbackMethodNames = PhysicsCallbackMethodNames.OrderBy(s => s, StringComparer.Ordinal).ToList(),
                TypeToModuleUmbrellaHeader = new Dictionary<string, string>(TypeToModuleUmbrellaHeader, StringComparer.Ordinal)
            };

            string json = JsonSerializer.Serialize(dto);
            string? dir = Path.GetDirectoryName(outputPath);
            if (!string.IsNullOrEmpty(dir))
            {
                Directory.CreateDirectory(dir);
            }
            if (File.Exists(outputPath))
            {
                string existing = File.ReadAllText(outputPath, Encoding.UTF8);
                if (string.Equals(existing, json, StringComparison.Ordinal))
                {
                    return true;
                }
            }
            File.WriteAllText(outputPath, json, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
            return true;
        }
    }

    private static bool TryLoadPrebuiltEngineStubsCache()
    {
        var candidates = new List<string>();
        if (!string.IsNullOrEmpty(AppContext.BaseDirectory))
        {
            candidates.Add(Path.Combine(AppContext.BaseDirectory, "Shalltear.EngineStubs.json"));
            candidates.Add(Path.Combine(AppContext.BaseDirectory, "Roslyn", "Shalltear.EngineStubs.json"));
        }
        string? asmLoc = typeof(RoslynCompilerEngine).Assembly.Location;
        if (!string.IsNullOrEmpty(asmLoc))
        {
            string? asmDir = Path.GetDirectoryName(asmLoc);
            if (!string.IsNullOrEmpty(asmDir))
            {
                candidates.Add(Path.Combine(asmDir, "Shalltear.EngineStubs.json"));
            }
        }

        foreach (string path in candidates)
        {
            if (!File.Exists(path))
            {
                continue;
            }
            try
            {
                string json = File.ReadAllText(path, Encoding.UTF8);
                EngineStubsCacheDto? dto = JsonSerializer.Deserialize<EngineStubsCacheDto>(json);
                if (dto == null || string.IsNullOrEmpty(dto.StubsText) || dto.EngineTypeNames.Count == 0)
                {
                    continue;
                }

                EngineTypeNames.Clear();
                foreach (string s in dto.EngineTypeNames)
                {
                    EngineTypeNames.Add(s);
                }

                ObjectDerivedEngineTypes.Clear();
                foreach (string s in dto.ObjectDerivedEngineTypes)
                {
                    ObjectDerivedEngineTypes.Add(s);
                }

                StaticScopeIdentifiers.Clear();
                foreach (string s in dto.StaticScopeIdentifiers)
                {
                    StaticScopeIdentifiers.Add(s);
                }

                LifecycleMethodNames.Clear();
                foreach (string s in dto.LifecycleMethodNames)
                {
                    LifecycleMethodNames.Add(s);
                }

                PhysicsCallbackMethodNames.Clear();
                foreach (string s in dto.PhysicsCallbackMethodNames)
                {
                    PhysicsCallbackMethodNames.Add(s);
                }

                TypeToModuleUmbrellaHeader.Clear();
                foreach (var kv in dto.TypeToModuleUmbrellaHeader)
                {
                    TypeToModuleUmbrellaHeader[kv.Key] = kv.Value;
                }

                _cachedEngineStubsText = dto.StubsText;
                return true;
            }
            catch
            {
            }
        }
        return false;
    }

    public static string GetEngineStubsText()
    {
        lock (EngineScanLock)
        {
            if (_cachedEngineStubsText != null)
            {
                return _cachedEngineStubsText;
            }

            if (TryLoadPrebuiltEngineStubsCache() && _cachedEngineStubsText != null)
            {
                return _cachedEngineStubsText;
            }

            return ScanAndPopulateEngineStubs(explicitRoot: null);
        }
    }

    private static string ScanAndPopulateEngineStubs(string? explicitRoot)
    {
        lock (EngineScanLock)
        {
            var discoveredByName = new Dictionary<string, CppDiscoveredType>(StringComparer.Ordinal);
            var discoveredOrder = new List<CppDiscoveredType>();
            var discoveredAttributes = new List<(string Name, string Targets, string CtorParams)>();
            var discoveredLogLevels = new List<string>();
            bool hasHandleTemplate = false;

            foreach (string headerRoot in LocateEnginePublicHeaderRoots(explicitRoot))
            {
                ScanHeaderTreeForAttributesAndIntrinsics(headerRoot, discoveredAttributes, discoveredLogLevels, ref hasHandleTemplate);
                foreach (CppDiscoveredType cpp in ScanCppHeadersInDirectory(headerRoot, isEngineRoot: true))
                {
                    if (!discoveredByName.TryGetValue(cpp.Name, out CppDiscoveredType? existing))
                    {
                        discoveredByName[cpp.Name] = cpp;
                        discoveredOrder.Add(cpp);
                    }
                    else
                    {
                        MergeDiscoveredType(existing, cpp);
                    }
                }
            }

            // Compute transitive closure of Object-derived classes from C++ inheritance graph
            ObjectDerivedEngineTypes.Clear();
            if (discoveredByName.ContainsKey("Object"))
            {
                ObjectDerivedEngineTypes.Add("Object");
            }
            bool addedAny = true;
            while (addedAny)
            {
                addedAny = false;
                foreach (CppDiscoveredType t in discoveredOrder)
                {
                    if (t.Kind == "class" &&
                        !string.IsNullOrEmpty(t.BaseName) &&
                        ObjectDerivedEngineTypes.Contains(t.BaseName) &&
                        ObjectDerivedEngineTypes.Add(t.Name))
                    {
                        addedAny = true;
                    }
                }
            }

            // Classify non-Object classes: static-only classes become static_class, non-Object value classes become struct
            foreach (CppDiscoveredType t in discoveredOrder)
            {
                if (t.Kind == "class" && !ObjectDerivedEngineTypes.Contains(t.Name))
                {
                    bool allStatic = t.Fields.Count == 0 &&
                                     t.Methods.Count > 0 &&
                                     t.Methods.All(m => m.IsStatic);
                    t.Kind = allStatic ? "static_class" : "struct";
                    t.BaseName = "";
                }
            }

            // Discover lifecycle and PhysicsListener callbacks from Component, Behavior, Asset, and PhysicsListener
            if (discoveredByName.TryGetValue("Component", out CppDiscoveredType? compType))
            {
                foreach (CppDiscoveredMethod m in compType.Methods.Where(m => m.IsProtectedVirtual))
                {
                    LifecycleMethodNames.Add(m.Name);
                }
            }
            if (discoveredByName.TryGetValue("Asset", out CppDiscoveredType? assetType))
            {
                foreach (CppDiscoveredMethod m in assetType.Methods.Where(m => m.IsProtectedVirtual))
                {
                    LifecycleMethodNames.Add(m.Name);
                }
            }
            if (discoveredByName.TryGetValue("Behavior", out CppDiscoveredType? behaviorType))
            {
                var tickMethods = behaviorType.Methods.Where(m => m.IsProtectedVirtual && m.Params.Length == 0).ToList();
                foreach (CppDiscoveredMethod tick in tickMethods)
                {
                    LifecycleMethodNames.Add(tick.Name);
                    behaviorType.Methods.Add(new CppDiscoveredMethod
                    {
                        ReturnType = "void",
                        Name = tick.Name,
                        Params = "float dt",
                        DedupKey = $"{tick.Name}(float)",
                        IsProtectedVirtual = true
                    });
                }
                if (discoveredByName.TryGetValue("PhysicsListener", out CppDiscoveredType? physListener))
                {
                    foreach (CppDiscoveredMethod pm in physListener.Methods)
                    {
                        PhysicsCallbackMethodNames.Add(pm.Name);
                        behaviorType.Methods.Add(new CppDiscoveredMethod
                        {
                            ReturnType = pm.ReturnType,
                            Name = pm.Name,
                            Params = pm.Params,
                            DedupKey = pm.DedupKey,
                            IsProtectedVirtual = true
                        });
                        if (pm.Params.Contains("Handle<Collider>", StringComparison.Ordinal))
                        {
                            string unwrappedParams = pm.Params.Replace("Handle<Collider>", "Collider");
                            behaviorType.Methods.Add(new CppDiscoveredMethod
                            {
                                ReturnType = pm.ReturnType,
                                Name = pm.Name,
                                Params = unwrappedParams,
                                DedupKey = $"{pm.Name}(Collider)",
                                IsProtectedVirtual = true
                            });
                        }
                    }
                }
                if (discoveredByName.ContainsKey("Component"))
                {
                    behaviorType.Methods.Add(new CppDiscoveredMethod
                    {
                        ReturnType = "T",
                        Name = "GetComponent",
                        Params = "",
                        DedupKey = "GetComponent<T>()",
                        IsGenericComponentMethod = true
                    });
                }
            }

            // Register discovered types in lookup tables
            EngineTypeNames.Clear();
            StaticScopeIdentifiers.Clear();
            TypeToModuleUmbrellaHeader.Clear();

            if (hasHandleTemplate)
            {
                EngineTypeNames.Add("Handle");
            }
            EngineTypeNames.Add("Vector");

            var knownTypeSet = new HashSet<string>(StringComparer.Ordinal);
            foreach (CppDiscoveredType t in discoveredOrder)
            {
                knownTypeSet.Add(t.Name);
                EngineTypeNames.Add(t.Name);
                if (!string.IsNullOrEmpty(t.UmbrellaHeader))
                {
                    TypeToModuleUmbrellaHeader[t.Name] = t.UmbrellaHeader;
                }
                if (t.Kind is "enum" or "static_class" || t.Methods.Any(m => m.IsStatic) || t.Constants.Count > 0)
                {
                    StaticScopeIdentifiers.Add(t.Name);
                }
            }

            var sb = new StringBuilder();
            sb.AppendLine("// GENERATED by Shalltear Roslyn CSharpTranspiler from engine C++ headers.");
            sb.AppendLine("using System;");
            sb.AppendLine("using System.Collections.Generic;");
            sb.AppendLine();
            sb.AppendLine("namespace Shalltear");
            sb.AppendLine("{");

            var emittedAttrs = new HashSet<string>(StringComparer.Ordinal);
            foreach (var (attrName, targets, ctorParams) in discoveredAttributes)
            {
                if (!emittedAttrs.Add(attrName))
                {
                    continue;
                }
                sb.AppendLine($"    [AttributeUsage({targets})]");
                if (string.IsNullOrEmpty(ctorParams))
                {
                    sb.AppendLine($"    public sealed class {attrName}Attribute : Attribute {{ }}");
                }
                else
                {
                    sb.AppendLine($"    public sealed class {attrName}Attribute : Attribute");
                    sb.AppendLine("    {");
                    sb.AppendLine($"        public {attrName}Attribute({ctorParams}) {{ }}");
                    sb.AppendLine("    }");
                }
            }

            if (hasHandleTemplate)
            {
                sb.AppendLine("    public struct Handle<T> where T : class");
                sb.AppendLine("    {");
                sb.AppendLine("        public T Get() => null!;");
                sb.AppendLine("        public bool IsValid() => false;");
                sb.AppendLine("        public ulong GetInstanceId() => 0;");
                sb.AppendLine("        public void Reset() { }");
                sb.AppendLine("        public static implicit operator Handle<T>(T obj) => default;");
                sb.AppendLine("    }");
            }

            if (discoveredLogLevels.Count > 0)
            {
                EngineTypeNames.Add("Log");
                StaticScopeIdentifiers.Add("Log");
                sb.AppendLine("    public static class Log");
                sb.AppendLine("    {");
                foreach (string lvl in discoveredLogLevels)
                {
                    sb.AppendLine($"        public static void {lvl}(string category, string message, params object[] args) {{ }}");
                }
                sb.AppendLine("    }");
            }

            foreach (CppDiscoveredType t in discoveredOrder)
            {
                AppendDiscoveredStubDeclaration(sb, t, knownTypeSet, "    ");
            }

            sb.AppendLine("}");
            _cachedEngineStubsText = sb.ToString();
            return _cachedEngineStubsText;
        }
    }

    private static void ScanHeaderTreeForAttributesAndIntrinsics(
        string headerRoot,
        List<(string Name, string Targets, string CtorParams)> outAttributes,
        List<string> outLogLevels,
        ref bool hasHandleTemplate)
    {
        string reflectPath = Path.Combine(headerRoot, "Shalltear", "Core", "Reflection", "Reflect.h");
        if (File.Exists(reflectPath))
        {
            string text = File.ReadAllText(reflectPath, Encoding.UTF8);
            var attrInfoRegex = new Regex(
                @"#define\s+SLT_([A-Z0-9_]+)(?:\(([^)]*)\))?[\s\\]+::Shalltear::AttributeInfo\s*\{\s*[\s\\]*::Shalltear::StringHash\(""([A-Za-z0-9_]+)""\)",
                RegexOptions.Multiline);
            foreach (Match m in attrInfoRegex.Matches(text))
            {
                string rawArgs = m.Groups[2].Value.Trim();
                string attrName = m.Groups[3].Value.Trim();
                string ctorParams = "";
                if (!string.IsNullOrEmpty(rawArgs))
                {
                    string[] parts = rawArgs.Split(',', StringSplitOptions.RemoveEmptyEntries);
                    if (parts.Length == 2)
                    {
                        ctorParams = "float min, float max";
                    }
                    else if (parts.Length == 1)
                    {
                        ctorParams = rawArgs.Contains("Text", StringComparison.OrdinalIgnoreCase)
                            ? "string text"
                            : "int value";
                    }
                }
                outAttributes.Add((attrName, "AttributeTargets.Field | AttributeTargets.Property", ctorParams));
            }

            if (text.Contains("SLT_REFLECT_SUBOBJECT(", StringComparison.Ordinal))
            {
                outAttributes.Add(("Subobject", "AttributeTargets.Field | AttributeTargets.Property", ""));
            }
            if (text.Contains("SLT_REFLECT_SUBOBJECT_ARRAY(", StringComparison.Ordinal))
            {
                outAttributes.Add(("SubobjectArray", "AttributeTargets.Field | AttributeTargets.Property", ""));
            }
            if (text.Contains("SLT_REFLECT_METHOD(", StringComparison.Ordinal))
            {
                outAttributes.Add(("Callable", "AttributeTargets.Method", ""));
                outAttributes.Add(("ReflectMethod", "AttributeTargets.Method", ""));
            }
            if (text.Contains("SLT_SETTINGS_CATEGORY(", StringComparison.Ordinal))
            {
                outAttributes.Add(("SettingsCategory", "AttributeTargets.Class", "string categoryPath"));
            }
        }

        string dataAssetPath = Path.Combine(headerRoot, "Shalltear", "Core", "Asset", "DataAsset.h");
        if (File.Exists(dataAssetPath))
        {
            string text = File.ReadAllText(dataAssetPath, Encoding.UTF8);
            if (text.Contains("SLT_CREATE_ASSET_MENU(", StringComparison.Ordinal))
            {
                outAttributes.Add(("CreateAssetMenu", "AttributeTargets.Class", "string menuPath, string defaultFileName"));
            }
        }

        string handlePath = Path.Combine(headerRoot, "Shalltear", "Core", "Handle.h");
        if (File.Exists(handlePath))
        {
            string text = File.ReadAllText(handlePath, Encoding.UTF8);
            if (text.Contains("class Handle", StringComparison.Ordinal))
            {
                hasHandleTemplate = true;
            }
        }

        string logPath = Path.Combine(headerRoot, "Shalltear", "Core", "Log.h");
        if (File.Exists(logPath))
        {
            string text = File.ReadAllText(logPath, Encoding.UTF8);
            var logEnumMatch = Regex.Match(text, @"enum\s+class\s+LogLevel\s*\{([^}]*)\}");
            if (logEnumMatch.Success)
            {
                foreach (Match id in Regex.Matches(logEnumMatch.Groups[1].Value, @"\b([A-Za-z_][A-Za-z0-9_]*)\b"))
                {
                    string lvl = id.Groups[1].Value;
                    if (lvl != "None" && !outLogLevels.Contains(lvl))
                    {
                        outLogLevels.Add(lvl);
                        if (lvl == "Warn" && !outLogLevels.Contains("Warning"))
                        {
                            outLogLevels.Add("Warning");
                        }
                    }
                }
            }
        }
    }

    private static void MergeDiscoveredType(CppDiscoveredType target, CppDiscoveredType incoming)
    {
        if (string.IsNullOrEmpty(target.BaseName) && !string.IsNullOrEmpty(incoming.BaseName))
        {
            target.BaseName = incoming.BaseName;
        }
        foreach (var f in incoming.Fields)
        {
            if (!target.Fields.Any(ef => ef.Name == f.Name))
            {
                target.Fields.Add(f);
            }
        }
        foreach (var c in incoming.Constants)
        {
            if (!target.Constants.Any(ec => ec.Name == c.Name))
            {
                target.Constants.Add(c);
            }
        }
        foreach (var m in incoming.Methods)
        {
            if (!target.Methods.Any(em => em.DedupKey == m.DedupKey))
            {
                target.Methods.Add(m);
            }
        }
        foreach (string ev in incoming.EnumValues)
        {
            if (!target.EnumValues.Contains(ev))
            {
                target.EnumValues.Add(ev);
            }
        }
    }

    private static List<string> LocateEnginePublicHeaderRoots(string? explicitRoot = null)
    {
        var roots = new List<string>();
        var seeds = new List<string>();
        if (!string.IsNullOrEmpty(explicitRoot))
        {
            seeds.Add(explicitRoot);
        }
        if (!string.IsNullOrEmpty(AppContext.BaseDirectory))
        {
            seeds.Add(AppContext.BaseDirectory);
        }
        try
        {
            string cwd = Directory.GetCurrentDirectory();
            if (!string.IsNullOrEmpty(cwd))
            {
                seeds.Add(cwd);
            }
        }
        catch
        {
        }
        string? envSdk = Environment.GetEnvironmentVariable("SHALLTEAR_SDK_ROOT");
        if (!string.IsNullOrEmpty(envSdk))
        {
            seeds.Add(envSdk);
        }

        var excludedModules = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            "Editor", "ImportPipeline", "Tests", "SampleModule", "HotReloadModule", "Player", "Graphs", "NodeShader"
        };

        foreach (string seed in seeds)
        {
            var probe = new DirectoryInfo(seed);
            for (int depth = 0; depth < 8 && probe != null; ++depth)
            {
                string sdkInclude = Path.Combine(probe.FullName, "Include");
                if (Directory.Exists(sdkInclude) && Directory.Exists(Path.Combine(sdkInclude, "Shalltear")))
                {
                    if (!roots.Contains(sdkInclude))
                    {
                        roots.Add(sdkInclude);
                    }
                    return roots;
                }
                string sourceDir = Path.Combine(probe.FullName, "Source");
                if (Directory.Exists(sourceDir) && Directory.Exists(Path.Combine(sourceDir, "Core", "Public")))
                {
                    foreach (string moduleDir in Directory.GetDirectories(sourceDir).OrderBy(d => d, StringComparer.Ordinal))
                    {
                        string moduleName = Path.GetFileName(moduleDir);
                        if (excludedModules.Contains(moduleName))
                        {
                            continue;
                        }
                        string pub = Path.Combine(moduleDir, "Public");
                        if (Directory.Exists(pub) && !roots.Contains(pub))
                        {
                            roots.Add(pub);
                        }
                    }
                    return roots;
                }
                probe = probe.Parent;
            }
        }
        return roots;
    }

    private static string InferModuleUmbrellaHeader(string headerPath)
    {
        string norm = NormalizeSlashes(headerPath);
        if (norm.Contains("/Physics3D/", StringComparison.Ordinal))
        {
            return "Shalltear/Physics3D.h";
        }
        if (norm.Contains("/Audio/", StringComparison.Ordinal))
        {
            return "Shalltear/Audio.h";
        }
        if (norm.Contains("/Animation/", StringComparison.Ordinal))
        {
            return "Shalltear/Animation.h";
        }
        if (norm.Contains("/VisualEffects/", StringComparison.Ordinal))
        {
            return "Shalltear/VisualEffects.h";
        }
        if (norm.Contains("/PostProcessing/", StringComparison.Ordinal))
        {
            return "Shalltear/PostProcessing.h";
        }
        return "";
    }

    private static List<CppDiscoveredType> ScanCppHeadersInDirectory(string sourceDir, bool isEngineRoot)
    {
        var results = new List<CppDiscoveredType>();
        if (!Directory.Exists(sourceDir))
        {
            return results;
        }

        string[] headers = Directory.GetFiles(sourceDir, "*.h", SearchOption.AllDirectories)
            .Concat(Directory.GetFiles(sourceDir, "*.hpp", SearchOption.AllDirectories))
            .Where(f => !f.EndsWith(".generated.h", StringComparison.OrdinalIgnoreCase))
            .OrderBy(f => f, StringComparer.Ordinal)
            .ToArray();

        foreach (string headerPath in headers)
        {
            string relHeader = NormalizeSlashes(Path.GetRelativePath(sourceDir, headerPath));
            string umbrella = isEngineRoot ? InferModuleUmbrellaHeader(headerPath) : "";
            string rawContent;
            try
            {
                rawContent = File.ReadAllText(headerPath, Encoding.UTF8);
            }
            catch
            {
                continue;
            }

            ParseCppHeaderFile(rawContent, relHeader, umbrella, isEngineRoot, results);
        }

        return results;
    }

    private static void ParseCppHeaderFile(
        string rawContent,
        string relHeader,
        string umbrella,
        bool isEngineRoot,
        List<CppDiscoveredType> results)
    {
        string clean = StripCommentsAndMacros(rawContent);
        clean = ExpandTemplateStructAliases(clean);

        // 1. Discover static namespaces (`namespace Shalltear::Math` or `namespace Physics`)
        if (isEngineRoot)
        {
            foreach (Match nsMatch in Regex.Matches(clean, @"namespace\s+(?:Shalltear::)?(Math|Physics)\s*\{"))
            {
                string nsName = nsMatch.Groups[1].Value;
                int openBrace = nsMatch.Index + nsMatch.Length - 1;
                string nsBody = ExtractBalancedBlock(clean, openBrace, out _);
                var nsType = new CppDiscoveredType
                {
                    Name = nsName,
                    Kind = "static_class",
                    UmbrellaHeader = umbrella,
                    HeaderRelPath = relHeader
                };
                ParseNamespaceStaticMembers(nsBody, nsName == "Math", nsType);
                results.Add(nsType);
            }
        }

        // 2. Discover reflected enums via SLT_REFLECT_ENUM
        var enumMacro = new Regex(@"SLT_REFLECT_ENUM\s*\(\s*([A-Za-z0-9_:]+)\s*\)");
        var enumValMacro = new Regex(@"SLT_ENUM_VALUE\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)");
        foreach (Match em in enumMacro.Matches(rawContent))
        {
            string shortEnum = StripCppNamespace(em.Groups[1].Value.Trim());
            if (results.Any(r => r.Name == shortEnum))
            {
                continue;
            }
            var enumType = new CppDiscoveredType
            {
                Name = shortEnum,
                Kind = "enum",
                UmbrellaHeader = umbrella,
                HeaderRelPath = relHeader
            };
            int endEnumPos = rawContent.IndexOf("SLT_END_ENUM", em.Index, StringComparison.Ordinal);
            string enumSlice = endEnumPos > em.Index
                ? rawContent.Substring(em.Index, endEnumPos - em.Index)
                : rawContent.Substring(em.Index);
            foreach (Match ev in enumValMacro.Matches(enumSlice))
            {
                enumType.EnumValues.Add(ev.Groups[1].Value);
            }
            if (enumType.EnumValues.Count > 0)
            {
                results.Add(enumType);
            }
        }

        // 3. Discover top-level `enum class` declarations
        foreach (Match ec in Regex.Matches(clean, @"enum\s+class\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?::\s*[A-Za-z0-9_:]+)?\s*\{([^}]*)\}"))
        {
            string enumName = ec.Groups[1].Value;
            if (enumName is "ScaleKind" or "LogLevel" || results.Any(r => r.Name == enumName))
            {
                continue;
            }
            var enumType = new CppDiscoveredType
            {
                Name = enumName,
                Kind = "enum",
                UmbrellaHeader = umbrella,
                HeaderRelPath = relHeader
            };
            foreach (string rawItem in ec.Groups[2].Value.Split(',', StringSplitOptions.RemoveEmptyEntries))
            {
                string item = rawItem.Trim();
                int eq = item.IndexOf('=');
                string ident = (eq > 0 ? item.Substring(0, eq) : item).Trim();
                if (!string.IsNullOrEmpty(ident) && Regex.IsMatch(ident, @"^[A-Za-z_][A-Za-z0-9_]*$"))
                {
                    enumType.EnumValues.Add(ident);
                }
            }
            if (enumType.EnumValues.Count > 0)
            {
                results.Add(enumType);
            }
        }

        // 4. Discover top-level classes and structs
        var typeHeaderRegex = new Regex(@"(?:template\s*<[^>]*>\s*)?\b(class|struct)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:final\s*)?(?::\s*([^{;]+))?\s*\{");
        int searchFrom = 0;
        while (searchFrom < clean.Length)
        {
            Match tm = typeHeaderRegex.Match(clean, searchFrom);
            if (!tm.Success)
            {
                break;
            }

            string fullMatch = tm.Value;
            string kind = tm.Groups[1].Value;
            string typeName = tm.Groups[2].Value;
            string baseClause = tm.Groups[3].Value.Trim();
            int openBrace = tm.Index + tm.Length - 1;
            string body = ExtractBalancedBlock(clean, openBrace, out int closeBrace);
            searchFrom = closeBrace > openBrace ? closeBrace + 1 : tm.Index + tm.Length;

            if (fullMatch.TrimStart().StartsWith("template", StringComparison.Ordinal))
            {
                continue;
            }
            if (typeName.EndsWith("Impl", StringComparison.Ordinal) ||
                typeName.EndsWith("Access", StringComparison.Ordinal) ||
                typeName.EndsWith("Traits", StringComparison.Ordinal) ||
                typeName is "DeclaredAssetImporter" or "ObjectRegistry" or "LogManager" or "ConsoleSink" or "FileSink" or "ILogSink" or "FrameScope")
            {
                continue;
            }

            string baseName = "";
            if (!string.IsNullOrEmpty(baseClause))
            {
                string firstBase = baseClause.Split(',')[0].Trim();
                firstBase = Regex.Replace(firstBase, @"^(?:public|protected|private|virtual)\s+", "").Trim();
                baseName = StripCppNamespace(firstBase);
            }

            Match compRefl = Regex.Match(body, @"SLT_REFLECT_COMPONENT(?:_ABSTRACT)?\s*\(\s*[A-Za-z0-9_]+\s*,\s*([A-Za-z0-9_:]+)\s*\)");
            if (compRefl.Success)
            {
                baseName = StripCppNamespace(compRefl.Groups[1].Value.Trim());
                kind = "class";
            }
            Match parentRefl = Regex.Match(body, @"SLT_REFLECT_PARENT\s*\(\s*([A-Za-z0-9_:]+)\s*\)");
            if (parentRefl.Success)
            {
                baseName = StripCppNamespace(parentRefl.Groups[1].Value.Trim());
                kind = "class";
            }
            bool hasReflectMacro = compRefl.Success ||
                                   parentRefl.Success ||
                                   body.Contains("SLT_REFLECT_CLASS", StringComparison.Ordinal) ||
                                   body.Contains("SLT_REFLECT_VALUE", StringComparison.Ordinal);

            if (!isEngineRoot && !hasReflectMacro)
            {
                continue;
            }

            var discovered = new CppDiscoveredType
            {
                Name = typeName,
                Kind = kind,
                BaseName = baseName,
                UmbrellaHeader = umbrella,
                HeaderRelPath = relHeader
            };

            ParseClassOrStructBody(body, typeName, kind == "struct", isEngineRoot, discovered);
            results.Add(discovered);
        }
    }

    private static string StripCommentsAndMacros(string src)
    {
        // Strip block and line comments
        string noComments = Regex.Replace(src, @"/\*[\s\S]*?\*/|//[^\r\n]*", "");
        // Strip multi-line #define and preprocessor lines
        var sb = new StringBuilder(noComments.Length);
        string[] lines = noComments.Split('\n');
        bool inMacroContinuation = false;
        foreach (string rawLine in lines)
        {
            string trimmed = rawLine.TrimEnd('\r', ' ', '\t');
            if (inMacroContinuation)
            {
                inMacroContinuation = trimmed.EndsWith("\\", StringComparison.Ordinal);
                sb.Append('\n');
                continue;
            }
            string leading = trimmed.TrimStart();
            if (leading.StartsWith("#", StringComparison.Ordinal))
            {
                inMacroContinuation = trimmed.EndsWith("\\", StringComparison.Ordinal);
                sb.Append('\n');
                continue;
            }
            sb.Append(rawLine).Append('\n');
        }

        string res = sb.ToString();
        res = Regex.Replace(res, @"\[\[nodiscard(?:\(""[^""]*""\))?\]\]|\[\[maybe_unused\]\]", "");
        res = Regex.Replace(res, @"\balignas\s*\([^)]*\)", "");
        res = Regex.Replace(res, @"\bSHALLTEAR_[A-Z0-9_]+_API\b", "");
        return res;
    }

    private static string ExpandTemplateStructAliases(string cleanSrc)
    {
        var templates = new Dictionary<string, (string TypeParam, string Body)>(StringComparer.Ordinal);
        var tmplRegex = new Regex(@"template\s*<\s*typename\s+([A-Za-z_][A-Za-z0-9_]*)\s*>\s*struct\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{");
        foreach (Match m in tmplRegex.Matches(cleanSrc))
        {
            string tParam = m.Groups[1].Value;
            string tName = m.Groups[2].Value;
            int openBrace = m.Index + m.Length - 1;
            string body = ExtractBalancedBlock(cleanSrc, openBrace, out _);
            if (!string.IsNullOrEmpty(body))
            {
                templates[tName] = (tParam, body);
            }
        }
        if (templates.Count == 0)
        {
            return cleanSrc;
        }

        var appended = new StringBuilder(cleanSrc);
        foreach (Match um in Regex.Matches(cleanSrc, @"using\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*<\s*([A-Za-z0-9_]+)\s*>\s*;"))
        {
            string aliasName = um.Groups[1].Value;
            string tmplName = um.Groups[2].Value;
            string typeArg = um.Groups[3].Value;
            if (!templates.TryGetValue(tmplName, out var tmpl))
            {
                continue;
            }
            string instantiated = tmpl.Body;
            bool isFloat = typeArg is "f32" or "float" or "f64" or "double";
            if (!isFloat)
            {
                instantiated = Regex.Replace(instantiated, @"[^\r\n;{]*\brequires\s+IsFloat\b\s*\{[^{}]*\}", "");
            }
            instantiated = Regex.Replace(instantiated, @"\brequires\s+IsFloat\b", "");
            instantiated = Regex.Replace(instantiated, $@"\b{Regex.Escape(tmplName)}\b", aliasName);
            instantiated = Regex.Replace(instantiated, $@"\b{Regex.Escape(tmpl.TypeParam)}\b", typeArg);
            appended.Append($"\nstruct {aliasName} {{\n{instantiated}\n}};\n");
        }
        return appended.ToString();
    }

    private static string ExtractBalancedBlock(string text, int openBraceIndex, out int closeBraceIndex)
    {
        closeBraceIndex = -1;
        if (openBraceIndex < 0 || openBraceIndex >= text.Length || text[openBraceIndex] != '{')
        {
            return "";
        }
        int depth = 0;
        for (int i = openBraceIndex; i < text.Length; ++i)
        {
            char c = text[i];
            if (c == '{')
            {
                ++depth;
            }
            else if (c == '}')
            {
                --depth;
                if (depth == 0)
                {
                    closeBraceIndex = i;
                    return text.Substring(openBraceIndex + 1, i - openBraceIndex - 1);
                }
            }
        }
        return "";
    }

    private static void ParseNamespaceStaticMembers(string nsBody, bool isMathNs, CppDiscoveredType target)
    {
        foreach (var (_, stmt) in ExtractTopLevelClassDeclarations(nsBody, defaultPublic: true))
        {
            string s = stmt.Trim();
            if (string.IsNullOrEmpty(s))
            {
                continue;
            }

            // Constant: inline constexpr f32 PI = 3.14159f
            Match constMatch = Regex.Match(s, @"^(?:inline\s+)?constexpr\s+([A-Za-z0-9_]+)\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$");
            if (constMatch.Success && !s.Contains('('))
            {
                string cType = MapCppTypeToCSharpStub(constMatch.Groups[1].Value);
                string cName = constMatch.Groups[2].Value;
                string cVal = constMatch.Groups[3].Value.Trim();
                target.Constants.Add((cType, cName, cVal));
                continue;
            }

            bool isTemplateT = false;
            if (s.StartsWith("template", StringComparison.Ordinal))
            {
                if (Regex.IsMatch(s, @"^template\s*<\s*typename\s+T\s*>"))
                {
                    isTemplateT = true;
                    s = Regex.Replace(s, @"^template\s*<\s*typename\s+T\s*>\s*", "").Trim();
                }
                else
                {
                    continue;
                }
            }

            if (isTemplateT && isMathNs)
            {
                string floatStmt = Regex.Replace(s, @"\bT\b", "f32");
                string intStmt = Regex.Replace(s, @"\bT\b", "i32");
                TryAddParsedMethod(floatStmt, isStatic: true, isProtectedVirtual: false, target);
                TryAddParsedMethod(intStmt, isStatic: true, isProtectedVirtual: false, target);
            }
            else
            {
                TryAddParsedMethod(s, isStatic: true, isProtectedVirtual: false, target);
            }
        }
    }

    private static void ParseClassOrStructBody(
        string rawBody,
        string typeName,
        bool defaultPublic,
        bool isEngineRoot,
        CppDiscoveredType target)
    {
        var reflectedFields = new HashSet<string>(StringComparer.Ordinal);
        foreach (Match fm in Regex.Matches(rawBody, @"SLT_REFLECT_(?:FIELD|ARRAY|SUBOBJECT|SUBOBJECT_ARRAY)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)"))
        {
            reflectedFields.Add(fm.Groups[1].Value);
        }

        string cleanBody = Regex.Replace(rawBody, @"SLT_[A-Z0-9_]+\s*(?:\([^)]*\))?", "");

        foreach (var (access, rawStmt) in ExtractTopLevelClassDeclarations(cleanBody, defaultPublic))
        {
            string s = rawStmt.Trim();
            if (string.IsNullOrEmpty(s))
            {
                continue;
            }
            if ((s.StartsWith("friend ", StringComparison.Ordinal) && !s.Contains("operator", StringComparison.Ordinal)) ||
                s.StartsWith("using ", StringComparison.Ordinal) ||
                s.StartsWith("static_assert", StringComparison.Ordinal) ||
                s.StartsWith("~", StringComparison.Ordinal) ||
                s.StartsWith($"virtual ~{typeName}", StringComparison.Ordinal) ||
                s.EndsWith("= delete", StringComparison.Ordinal))
            {
                continue;
            }

            if (access == "private")
            {
                // For user C++ headers, reflected fields might be declared anywhere
                if (!isEngineRoot && !s.Contains('('))
                {
                    TryParseFieldDeclaration(s, reflectedFields, requireReflected: true, target);
                }
                continue;
            }

            if (access == "protected")
            {
                if (s.Contains("virtual ", StringComparison.Ordinal) && s.Contains('('))
                {
                    TryAddParsedMethod(s, isStatic: false, isProtectedVirtual: true, target);
                }
                continue;
            }

            // Public member: check operator
            if (s.Contains("operator", StringComparison.Ordinal) && s.Contains('('))
            {
                TryParseOperatorDeclaration(s, typeName, target);
                continue;
            }

            // Public constructor
            Match ctorMatch = Regex.Match(s, $@"^(?:constexpr\s+|explicit\s+|inline\s+)*{Regex.Escape(typeName)}\s*\(([^)]*)\)");
            if (ctorMatch.Success)
            {
                string rawParams = ctorMatch.Groups[1].Value.Trim();
                if (!string.IsNullOrEmpty(rawParams) &&
                    !rawParams.Contains($"{typeName}&", StringComparison.Ordinal) &&
                    TryMapCppParamsToCSharp(rawParams, out string csParams, out string keyParams, out _))
                {
                    string dedup = $"{typeName}({keyParams})";
                    if (!target.Methods.Any(m => m.DedupKey == dedup))
                    {
                        target.Methods.Add(new CppDiscoveredMethod
                        {
                            ReturnType = "",
                            Name = typeName,
                            Params = csParams,
                            DedupKey = dedup,
                            IsConstructor = true
                        });
                    }
                }
                continue;
            }

            // Public template <typename T> component method (e.g. AddComponent<T>, GetComponent<T>)
            if (s.StartsWith("template", StringComparison.Ordinal))
            {
                Match tmplMethod = Regex.Match(
                    s,
                    @"^template\s*<\s*typename\s+T\s*>\s*(?:static\s+)?(Handle<T>|T\*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)");
                if (tmplMethod.Success)
                {
                    string ret = tmplMethod.Groups[1].Value == "T*" ? "T" : "Handle<T>";
                    string mName = tmplMethod.Groups[2].Value;
                    string rawParams = tmplMethod.Groups[3].Value.Trim();
                    if (TryMapCppParamsToCSharp(rawParams, out string csParams, out string keyParams, out _))
                    {
                        string dedup = $"{mName}<T>({keyParams})";
                        if (!target.Methods.Any(m => m.DedupKey == dedup))
                        {
                            target.Methods.Add(new CppDiscoveredMethod
                            {
                                ReturnType = ret,
                                Name = mName,
                                Params = csParams,
                                DedupKey = dedup,
                                IsGenericComponentMethod = true
                            });
                        }
                    }
                }
                continue;
            }

            // Public method vs public field
            int parenIdx = s.IndexOf('(');
            int eqIdx = s.IndexOf('=');
            bool isMethod = parenIdx > 0 && (eqIdx < 0 || parenIdx < eqIdx);
            if (isMethod)
            {
                bool isStatic = Regex.IsMatch(s, @"\bstatic\b");
                TryAddParsedMethod(s, isStatic, isProtectedVirtual: false, target);
            }
            else
            {
                TryParseFieldDeclaration(s, reflectedFields, requireReflected: false, target);
            }
        }
    }

    private static List<(string Access, string Statement)> ExtractTopLevelClassDeclarations(string classBody, bool defaultPublic)
    {
        var list = new List<(string Access, string Statement)>();
        string currentAccess = defaultPublic ? "public" : "private";
        var current = new StringBuilder();
        int braceDepth = 0;
        int parenDepth = 0;
        int n = classBody.Length;

        for (int i = 0; i < n; ++i)
        {
            char c = classBody[i];
            if (braceDepth == 0 && parenDepth == 0 && c == ':')
            {
                string tok = current.ToString().Trim();
                if (tok is "public" or "protected" or "private")
                {
                    currentAccess = tok;
                    current.Clear();
                    continue;
                }
            }

            if (c == '(')
            {
                ++parenDepth;
                current.Append(c);
                continue;
            }
            if (c == ')')
            {
                if (parenDepth > 0)
                {
                    --parenDepth;
                }
                current.Append(c);
                continue;
            }

            if (c == '{')
            {
                if (braceDepth == 0 && parenDepth == 0)
                {
                    string headerSoFar = current.ToString().Trim();
                    // Check if nested struct/class/enum
                    if (Regex.IsMatch(headerSoFar, @"^(?:struct|class|enum|union)\b"))
                    {
                        ExtractBalancedBlock(classBody, i, out int closeNested);
                        if (closeNested > i)
                        {
                            i = closeNested;
                            while (i + 1 < n && (char.IsWhiteSpace(classBody[i + 1]) || classBody[i + 1] == ';'))
                            {
                                ++i;
                                if (classBody[i] == ';')
                                {
                                    break;
                                }
                            }
                        }
                        current.Clear();
                        continue;
                    }

                    // Check if inline method/constructor body (has '(' and ')')
                    if (headerSoFar.Contains('(') && headerSoFar.Contains(')'))
                    {
                        ExtractBalancedBlock(classBody, i, out int closeMethod);
                        if (closeMethod > i)
                        {
                            i = closeMethod;
                        }
                        string cleanHeader = StripConstructorInitializerList(headerSoFar);
                        if (!string.IsNullOrWhiteSpace(cleanHeader))
                        {
                            list.Add((currentAccess, cleanHeader));
                        }
                        current.Clear();
                        continue;
                    }

                    // Otherwise it's a field brace-initializer like `Vec3 _size{1.0f, 1.0f, 1.0f};`
                    ExtractBalancedBlock(classBody, i, out int closeInit);
                    if (closeInit > i)
                    {
                        i = closeInit;
                    }
                    continue;
                }
                ++braceDepth;
                continue;
            }

            if (c == '}')
            {
                if (braceDepth > 0)
                {
                    --braceDepth;
                }
                continue;
            }

            if (c == ';' && braceDepth == 0 && parenDepth == 0)
            {
                string stmt = current.ToString().Trim();
                if (!string.IsNullOrWhiteSpace(stmt))
                {
                    list.Add((currentAccess, stmt));
                }
                current.Clear();
                continue;
            }

            if (braceDepth == 0)
            {
                current.Append(c);
            }
        }

        return list;
    }

    private static string StripConstructorInitializerList(string methodHeader)
    {
        int closeParen = methodHeader.LastIndexOf(')');
        if (closeParen < 0)
        {
            return methodHeader;
        }
        string afterParen = methodHeader.Substring(closeParen + 1);
        int colon = afterParen.IndexOf(':');
        if (colon >= 0)
        {
            return methodHeader.Substring(0, closeParen + 1 + colon).Trim();
        }
        return methodHeader;
    }

    private static void TryParseOperatorDeclaration(string stmt, string enclosingType, CppDiscoveredType target)
    {
        Match opMatch = Regex.Match(
            stmt,
            @"^(?:friend\s+|constexpr\s+|inline\s+|static\s+)*([A-Za-z0-9_:<>&\s]+?)\s+operator\s*(\+|-|\*|/)\s*\(([^)]*)\)");
        if (!opMatch.Success)
        {
            return;
        }
        string rawRet = opMatch.Groups[1].Value.Trim();
        string opSym = opMatch.Groups[2].Value.Trim();
        string rawParams = opMatch.Groups[3].Value.Trim();
        if (!TryMapCppTypeToCSharp(rawRet, out string csRet, out _))
        {
            return;
        }

        if (string.IsNullOrEmpty(rawParams))
        {
            // Unary member operator (e.g. `Vec3 operator-() const`)
            string dedup = $"operator{opSym}({enclosingType})";
            if (!target.Methods.Any(m => m.DedupKey == dedup))
            {
                target.Methods.Add(new CppDiscoveredMethod
                {
                    ReturnType = csRet,
                    Name = $"operator {opSym}",
                    Params = $"{enclosingType} a",
                    DedupKey = dedup,
                    IsStatic = true,
                    IsOperator = true
                });
            }
            return;
        }

        if (!TryMapCppParamsToCSharp(rawParams, out string csParams, out string keyParams, out _))
        {
            return;
        }

        int paramCount = keyParams.Split(',', StringSplitOptions.RemoveEmptyEntries).Length;
        if (paramCount == 1)
        {
            // Binary member operator (implicit `this` as lhs)
            csParams = $"{enclosingType} lhs, {csParams}";
            keyParams = $"{enclosingType},{keyParams}";
        }
        string dedupKey = $"operator{opSym}({keyParams})";
        if (!target.Methods.Any(m => m.DedupKey == dedupKey))
        {
            target.Methods.Add(new CppDiscoveredMethod
            {
                ReturnType = csRet,
                Name = $"operator {opSym}",
                Params = csParams,
                DedupKey = dedupKey,
                IsStatic = true,
                IsOperator = true
            });
        }
    }

    private static void TryAddParsedMethod(
        string stmt,
        bool isStatic,
        bool isProtectedVirtual,
        CppDiscoveredType target)
    {
        string clean = Regex.Replace(stmt, @"\b(?:virtual|inline|constexpr|static|explicit)\b", "").Trim();
        int closeParen = clean.LastIndexOf(')');
        if (closeParen > 0)
        {
            clean = clean.Substring(0, closeParen + 1).Trim();
        }

        Match m = Regex.Match(clean, @"^([A-Za-z0-9_:<>&*,\s]+?)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)$");
        if (!m.Success)
        {
            return;
        }

        string rawRet = m.Groups[1].Value.Trim();
        string mName = m.Groups[2].Value.Trim();
        string rawParams = m.Groups[3].Value.Trim();

        if (!TryMapCppTypeToCSharp(rawRet, out string csRet, out _))
        {
            return;
        }
        if (!TryMapCppParamsToCSharp(rawParams, out string csParams, out string keyParams, out string? outOverloadParams))
        {
            return;
        }

        string dedup = $"{mName}({keyParams})";
        if (!target.Methods.Any(existing => existing.DedupKey == dedup))
        {
            target.Methods.Add(new CppDiscoveredMethod
            {
                ReturnType = csRet,
                Name = mName,
                Params = csParams,
                DedupKey = dedup,
                IsStatic = isStatic,
                IsProtectedVirtual = isProtectedVirtual
            });
        }

        if (!string.IsNullOrEmpty(outOverloadParams))
        {
            string outDedup = $"{mName}(out:{keyParams})";
            if (!target.Methods.Any(existing => existing.DedupKey == outDedup))
            {
                target.Methods.Add(new CppDiscoveredMethod
                {
                    ReturnType = csRet,
                    Name = mName,
                    Params = outOverloadParams,
                    DedupKey = outDedup,
                    IsStatic = isStatic,
                    IsProtectedVirtual = isProtectedVirtual
                });
            }
        }
    }

    private static void TryParseFieldDeclaration(
        string stmt,
        HashSet<string> reflectedFields,
        bool requireReflected,
        CppDiscoveredType target)
    {
        if (stmt.Contains("static constexpr", StringComparison.Ordinal) ||
            stmt.StartsWith("static ", StringComparison.Ordinal) ||
            stmt.Contains('[') ||
            stmt.Contains("std::atomic", StringComparison.Ordinal))
        {
            return;
        }

        string noInit = stmt;
        int eq = noInit.IndexOf('=');
        if (eq > 0)
        {
            noInit = noInit.Substring(0, eq).Trim();
        }

        // Check multi-declarator fields like `f32 x, y, z`
        Match multiMatch = Regex.Match(noInit, @"^([A-Za-z0-9_:<>]+)\s+([A-Za-z_][A-Za-z0-9_]*(?:\s*,\s*[A-Za-z_][A-Za-z0-9_]*)+)$");
        if (multiMatch.Success)
        {
            if (!TryMapCppTypeToCSharp(multiMatch.Groups[1].Value.Trim(), out string csType, out _))
            {
                return;
            }
            foreach (string rawName in multiMatch.Groups[2].Value.Split(',', StringSplitOptions.RemoveEmptyEntries))
            {
                string fName = rawName.Trim();
                if (!fName.StartsWith("_", StringComparison.Ordinal) && (!requireReflected || reflectedFields.Contains(fName)))
                {
                    target.Fields.Add((csType, EscapeCSharpIdent(fName)));
                }
            }
            return;
        }

        Match singleMatch = Regex.Match(noInit, @"^([A-Za-z0-9_:<>\s]+?)\s+([A-Za-z_][A-Za-z0-9_]*)$");
        if (!singleMatch.Success)
        {
            return;
        }

        string rawCppType = singleMatch.Groups[1].Value.Trim();
        string fieldName = singleMatch.Groups[2].Value.Trim();
        if (fieldName.StartsWith("_", StringComparison.Ordinal))
        {
            return;
        }
        if (requireReflected && !reflectedFields.Contains(fieldName))
        {
            return;
        }
        if (TryMapCppTypeToCSharp(rawCppType, out string mappedCsType, out _))
        {
            target.Fields.Add((mappedCsType, EscapeCSharpIdent(fieldName)));
        }
    }

    private static string EscapeCSharpIdent(string ident)
    {
        return CSharpKeywords.Contains(ident) ? "@" + ident : ident;
    }

    private static bool TryMapCppTypeToCSharp(string rawCppType, out string csType, out bool isNonConstRef)
    {
        csType = "";
        isNonConstRef = false;
        if (string.IsNullOrWhiteSpace(rawCppType))
        {
            return false;
        }

        string t = Regex.Replace(rawCppType, @"\b(?:virtual|inline|constexpr|static|explicit|typename|class|struct)\b", "").Trim();
        if (t.Contains("std::span", StringComparison.Ordinal) ||
            t.Contains("std::function", StringComparison.Ordinal) ||
            t.Contains("std::format", StringComparison.Ordinal) ||
            t.Contains("std::shared_ptr", StringComparison.Ordinal) ||
            t.Contains("(*)", StringComparison.Ordinal) ||
            t.Contains("&&", StringComparison.Ordinal) ||
            t.Contains("void*", StringComparison.Ordinal) ||
            t.Contains("char*", StringComparison.Ordinal))
        {
            return false;
        }

        bool hasConst = Regex.IsMatch(t, @"\bconst\b");
        bool hasRef = t.EndsWith("&", StringComparison.Ordinal);
        bool hasPtr = t.EndsWith("*", StringComparison.Ordinal);
        if (hasRef && !hasConst)
        {
            isNonConstRef = true;
        }

        t = Regex.Replace(t, @"\bconst\b", "").Trim().TrimEnd('&').Trim();
        if (hasPtr)
        {
            return false;
        }

        // Handle<T>
        Match handleMatch = Regex.Match(t, @"^(?:::)?(?:Shalltear::)?Handle<\s*([A-Za-z0-9_:]+)\s*>$");
        if (handleMatch.Success)
        {
            string inner = StripCppNamespace(handleMatch.Groups[1].Value.Trim());
            csType = $"Handle<{inner}>";
            return true;
        }

        // Vector<T>
        Match vecMatch = Regex.Match(t, @"^(?:::)?(?:Shalltear::)?Vector<\s*(.+)\s*>$");
        if (vecMatch.Success)
        {
            if (TryMapCppTypeToCSharp(vecMatch.Groups[1].Value.Trim(), out string elemCs, out _))
            {
                csType = $"List<{elemCs}>";
                return true;
            }
            return false;
        }

        t = StripCppNamespace(t);
        if (t.Contains("::", StringComparison.Ordinal) || t.Contains('<') || t.Contains('>'))
        {
            return false;
        }

        csType = t switch
        {
            "f32" or "float" => "float",
            "f64" or "double" => "double",
            "i32" or "int" => "int",
            "u32" => "uint",
            "i64" => "long",
            "u64" or "size_t" => "ulong",
            "i16" => "short",
            "u16" => "ushort",
            "i8" => "sbyte",
            "u8" => "byte",
            "bool" => "bool",
            "str" or "string" or "string_view" or "Path" => "string",
            "void" => "void",
            _ => t
        };
        return Regex.IsMatch(csType, @"^[A-Za-z_][A-Za-z0-9_]*$");
    }

    private static bool TryMapCppParamsToCSharp(
        string rawParams,
        out string csParams,
        out string keyParams,
        out string? outOverloadParams)
    {
        csParams = "";
        keyParams = "";
        outOverloadParams = null;
        if (string.IsNullOrWhiteSpace(rawParams) || rawParams == "void")
        {
            return true;
        }

        List<string> parts = SplitTopLevelComma(rawParams);
        var normalList = new List<string>(parts.Count);
        var outList = new List<string>(parts.Count);
        var keyList = new List<string>(parts.Count);
        bool hasOutRefParam = false;
        int idx = 0;

        foreach (string part in parts)
        {
            string p = part.Trim();
            string? defaultExpr = null;
            int eq = p.IndexOf('=');
            if (eq > 0)
            {
                defaultExpr = p.Substring(eq + 1).Trim();
                p = p.Substring(0, eq).Trim();
            }

            int lastSpace = p.LastIndexOf(' ');
            string rawType;
            string rawName;
            if (lastSpace > 0)
            {
                rawType = p.Substring(0, lastSpace).Trim();
                rawName = p.Substring(lastSpace + 1).Trim();
                while (rawName.StartsWith("&", StringComparison.Ordinal) || rawName.StartsWith("*", StringComparison.Ordinal))
                {
                    rawType += rawName[0];
                    rawName = rawName.Substring(1).Trim();
                }
            }
            else
            {
                rawType = p;
                rawName = $"arg{idx}";
            }

            if (rawName.Contains('['))
            {
                return false;
            }

            if (!TryMapCppTypeToCSharp(rawType, out string mappedType, out bool isNonConstRef))
            {
                return false;
            }

            string safeName = EscapeCSharpIdent(string.IsNullOrEmpty(rawName) ? $"arg{idx}" : rawName);
            string defSuffix = defaultExpr != null ? $" = {MapCppDefaultArgToCSharp(defaultExpr, mappedType)}" : "";

            normalList.Add($"{mappedType} {safeName}{defSuffix}");
            if (isNonConstRef)
            {
                hasOutRefParam = true;
                outList.Add($"out {mappedType} {safeName}");
            }
            else
            {
                outList.Add($"{mappedType} {safeName}{defSuffix}");
            }
            keyList.Add(mappedType);
            ++idx;
        }

        csParams = string.Join(", ", normalList);
        keyParams = string.Join(",", keyList);
        if (hasOutRefParam)
        {
            outOverloadParams = string.Join(", ", outList);
        }
        return true;
    }

    private static string MapCppDefaultArgToCSharp(string cppDefault, string csType)
    {
        if (csType == "bool" && cppDefault is "true" or "false")
        {
            return cppDefault;
        }
        if ((csType is "float" or "double" or "int" or "uint") &&
            Regex.IsMatch(cppDefault, @"^-?[0-9]+(?:\.[0-9]+)?f?$"))
        {
            return cppDefault;
        }
        return "default";
    }

    private static List<string> SplitTopLevelComma(string input)
    {
        var list = new List<string>();
        int depth = 0;
        int start = 0;
        for (int i = 0; i < input.Length; ++i)
        {
            char c = input[i];
            if (c is '<' or '(' or '{')
            {
                ++depth;
            }
            else if (c is '>' or ')' or '}')
            {
                if (depth > 0)
                {
                    --depth;
                }
            }
            else if (c == ',' && depth == 0)
            {
                list.Add(input.Substring(start, i - start));
                start = i + 1;
            }
        }
        if (start < input.Length)
        {
            list.Add(input.Substring(start));
        }
        return list;
    }

    private static string StripCppNamespace(string typeName)
    {
        int idx = typeName.LastIndexOf("::", StringComparison.Ordinal);
        return idx >= 0 ? typeName.Substring(idx + 2) : typeName;
    }

    private static string MapCppTypeToCSharpStub(string cppType)
    {
        return TryMapCppTypeToCSharp(cppType, out string cs, out _) ? cs : StripCppNamespace(cppType);
    }

    private static bool IsTypeResolvableInStubs(string csType, HashSet<string> knownTypes)
    {
        if (string.IsNullOrEmpty(csType) || csType is "void" or "T" || PrimitiveMap.ContainsKey(csType))
        {
            return true;
        }
        if (csType.StartsWith("Handle<", StringComparison.Ordinal) && csType.EndsWith(">", StringComparison.Ordinal))
        {
            string inner = csType.Substring(7, csType.Length - 8).Trim();
            return inner == "T" || knownTypes.Contains(inner);
        }
        if (csType.StartsWith("List<", StringComparison.Ordinal) && csType.EndsWith(">", StringComparison.Ordinal))
        {
            string inner = csType.Substring(5, csType.Length - 6).Trim();
            return IsTypeResolvableInStubs(inner, knownTypes);
        }
        return knownTypes.Contains(csType);
    }

    private static bool AreMethodTypesResolvable(CppDiscoveredMethod m, HashSet<string> knownTypes)
    {
        if (!IsTypeResolvableInStubs(m.ReturnType, knownTypes))
        {
            return false;
        }
        if (string.IsNullOrWhiteSpace(m.Params))
        {
            return true;
        }
        foreach (string rawParam in SplitTopLevelComma(m.Params))
        {
            string p = rawParam.Trim();
            int eq = p.IndexOf('=');
            if (eq > 0)
            {
                p = p.Substring(0, eq).Trim();
            }
            if (p.StartsWith("out ", StringComparison.Ordinal))
            {
                p = p.Substring(4).Trim();
            }
            else if (p.StartsWith("ref ", StringComparison.Ordinal))
            {
                p = p.Substring(4).Trim();
            }
            int lastSpace = p.LastIndexOf(' ');
            if (lastSpace > 0)
            {
                string pType = p.Substring(0, lastSpace).Trim();
                if (!IsTypeResolvableInStubs(pType, knownTypes))
                {
                    return false;
                }
            }
        }
        return true;
    }

    private static void AppendDiscoveredStubDeclaration(
        StringBuilder sb,
        CppDiscoveredType t,
        HashSet<string> knownTypes,
        string indent)
    {
        if (t.Kind == "enum")
        {
            sb.AppendLine($"{indent}public enum {t.Name}");
            sb.AppendLine($"{indent}{{");
            foreach (string v in t.EnumValues)
            {
                sb.AppendLine($"{indent}    {v},");
            }
            sb.AppendLine($"{indent}}}");
            return;
        }

        if (t.Kind == "static_class")
        {
            sb.AppendLine($"{indent}public static class {t.Name}");
            sb.AppendLine($"{indent}{{");
            foreach (var (cType, cName, _) in t.Constants)
            {
                if (IsTypeResolvableInStubs(cType, knownTypes))
                {
                    sb.AppendLine($"{indent}    public static readonly {cType} {cName} = default!;");
                }
            }
            if (t.Name == "Time")
            {
                sb.AppendLine($"{indent}    public static float DeltaTime => 0.0f;");
                sb.AppendLine($"{indent}    public static float UnscaledDeltaTime => 0.0f;");
                sb.AppendLine($"{indent}    public static float FixedDeltaTime => 0.0f;");
                sb.AppendLine($"{indent}    public static double TotalTime => 0.0;");
                sb.AppendLine($"{indent}    public static ulong FrameCount => 0;");
            }
            foreach (CppDiscoveredMethod m in t.Methods)
            {
                if (!AreMethodTypesResolvable(m, knownTypes))
                {
                    continue;
                }
                EmitStubMethodLine(sb, m, $"{indent}    ");
            }
            sb.AppendLine($"{indent}}}");
            return;
        }

        if (t.Kind == "struct")
        {
            sb.AppendLine($"{indent}public struct {t.Name}");
            sb.AppendLine($"{indent}{{");
            foreach (var (fType, fName) in t.Fields)
            {
                if (IsTypeResolvableInStubs(fType, knownTypes))
                {
                    sb.AppendLine($"{indent}    public {fType} {fName};");
                }
            }
            foreach (CppDiscoveredMethod m in t.Methods)
            {
                if (!AreMethodTypesResolvable(m, knownTypes))
                {
                    continue;
                }
                EmitStubMethodLine(sb, m, $"{indent}    ");
            }
            sb.AppendLine($"{indent}}}");
            return;
        }

        string baseClause = !string.IsNullOrEmpty(t.BaseName) && knownTypes.Contains(t.BaseName)
            ? $" : Shalltear.{t.BaseName}"
            : "";
        sb.AppendLine($"{indent}public class {t.Name}{baseClause}");
        sb.AppendLine($"{indent}{{");
        foreach (var (fType, fName) in t.Fields)
        {
            if (IsTypeResolvableInStubs(fType, knownTypes))
            {
                sb.AppendLine($"{indent}    public {fType} {fName};");
            }
        }
        foreach (CppDiscoveredMethod m in t.Methods)
        {
            if (!AreMethodTypesResolvable(m, knownTypes))
            {
                continue;
            }
            EmitStubMethodLine(sb, m, $"{indent}    ");
        }
        sb.AppendLine($"{indent}}}");
    }

    private static void EmitStubMethodLine(StringBuilder sb, CppDiscoveredMethod m, string indent)
    {
        if (m.IsConstructor)
        {
            sb.AppendLine($"{indent}public {m.Name}({m.Params}) {{ }}");
            return;
        }
        if (m.IsOperator)
        {
            sb.AppendLine($"{indent}public static {m.ReturnType} {m.Name}({m.Params}) => default!;");
            return;
        }
        if (m.IsGenericComponentMethod)
        {
            sb.AppendLine($"{indent}public {m.ReturnType} {m.Name}<T>({m.Params}) where T : Component => default!;");
            return;
        }
        if (m.IsProtectedVirtual)
        {
            sb.AppendLine($"{indent}protected virtual {m.ReturnType} {m.Name}({m.Params}) {{ }}");
            return;
        }

        string staticMod = m.IsStatic ? "static " : "";
        bool hasOutParam = m.Params.Contains("out ", StringComparison.Ordinal);
        if (hasOutParam)
        {
            var assigns = new StringBuilder();
            foreach (string rawParam in SplitTopLevelComma(m.Params))
            {
                string p = rawParam.Trim();
                if (p.StartsWith("out ", StringComparison.Ordinal))
                {
                    int lastSpace = p.LastIndexOf(' ');
                    string outVar = p.Substring(lastSpace + 1).Trim();
                    assigns.Append($"{outVar} = default!; ");
                }
            }
            string retStmt = m.ReturnType == "void" ? "" : "return default!;";
            sb.AppendLine($"{indent}public {staticMod}{m.ReturnType} {m.Name}({m.Params}) {{ {assigns}{retStmt} }}");
            return;
        }

        string body = m.ReturnType == "void" ? "{ }" : "=> default!;";
        sb.AppendLine($"{indent}public {staticMod}{m.ReturnType} {m.Name}({m.Params}) {body}");
    }

    private static string BuildCombinedStubsText(List<CppDiscoveredType> cppTypes)
    {
        string baseStubs = GetEngineStubsText();
        if (cppTypes.Count == 0)
        {
            return baseStubs;
        }

        var knownTypes = new HashSet<string>(EngineTypeNames, StringComparer.Ordinal);
        foreach (CppDiscoveredType t in cppTypes)
        {
            knownTypes.Add(t.Name);
        }

        var sb = new StringBuilder(baseStubs);
        sb.AppendLine();
        sb.AppendLine("// discovered C++ project types");
        foreach (CppDiscoveredType t in cppTypes)
        {
            AppendDiscoveredStubDeclaration(sb, t, knownTypes, "");
        }
        return sb.ToString();
    }

    private static bool CollectSyntaxDiagnostics(SyntaxTree tree, List<DiagnosticDto> outDiagnostics)
    {
        bool ok = true;
        foreach (Diagnostic diag in tree.GetDiagnostics())
        {
            if (diag.Severity != DiagnosticSeverity.Error)
            {
                continue;
            }
            ok = false;
            FileLinePositionSpan span = diag.Location.GetLineSpan();
            string file = NormalizeSlashes(span.Path ?? tree.FilePath ?? "");
            int line = span.StartLinePosition.Line + 1;
            int col = span.StartLinePosition.Character + 1;
            string msg = diag.GetMessage();
            outDiagnostics.Add(new DiagnosticDto
            {
                Severity = "error",
                File = file,
                Line = line,
                Column = col,
                Message = msg,
                Raw = $"{file}:{line}:{col}: error: {msg}"
            });
        }
        return ok;
    }

    private static ProjectTypeIndex BuildProjectTypeIndex(
        IEnumerable<(SyntaxTree Tree, string RelativePath)> trees,
        IReadOnlyList<CppDiscoveredType> cppTypes)
    {
        var index = new ProjectTypeIndex();
        foreach (CppDiscoveredType cpp in cppTypes)
        {
            index.CppTypes.Add(cpp);
            index.TypeToGeneratedHeader[cpp.Name] = cpp.HeaderRelPath;
            if (cpp.Kind == "enum")
            {
                index.UserEnumNames.Add(cpp.Name);
            }
            else if (cpp.Kind == "struct")
            {
                index.UserValueStructs.Add(cpp.Name);
            }
            else
            {
                index.UserObjectTypes.Add(cpp.Name);
            }
        }

        foreach (var (tree, relPath) in trees)
        {
            string headerRel = ChangeExtensionToGeneratedHeader(relPath);
            CompilationUnitSyntax root = tree.GetCompilationUnitRoot();
            foreach (BaseTypeDeclarationSyntax typeDecl in root.DescendantNodes().OfType<BaseTypeDeclarationSyntax>())
            {
                string typeName = typeDecl.Identifier.ValueText;
                if (string.IsNullOrEmpty(typeName))
                {
                    continue;
                }
                index.TypeToGeneratedHeader[typeName] = headerRel;
                if (typeDecl is EnumDeclarationSyntax)
                {
                    index.UserEnumNames.Add(typeName);
                }
                else if (typeDecl is StructDeclarationSyntax)
                {
                    index.UserValueStructs.Add(typeName);
                }
                else if (typeDecl is ClassDeclarationSyntax classDecl)
                {
                    index.UserObjectTypes.Add(typeName);
                    List<ComputedPropertyInfo> computedProps = ExtractComputedProperties(classDecl);
                    if (computedProps.Count > 0)
                    {
                        index.TypeToComputedProperties[typeName] = new HashSet<string>(
                            computedProps.Select(p => p.Name),
                            StringComparer.Ordinal);
                    }
                }
            }
        }
        return index;
    }

    private static string ChangeExtensionToGeneratedHeader(string relPath)
    {
        string dir = Path.GetDirectoryName(relPath) ?? "";
        string stem = Path.GetFileNameWithoutExtension(relPath);
        string combined = string.IsNullOrEmpty(dir) ? $"{stem}.generated.h" : $"{NormalizeSlashes(dir)}/{stem}.generated.h";
        return combined;
    }

    private static void AddNodeError(string sourcePath, SyntaxNode node, string message, List<DiagnosticDto> outDiagnostics)
    {
        FileLinePositionSpan span = node.GetLocation().GetLineSpan();
        string file = string.IsNullOrEmpty(span.Path) ? sourcePath : NormalizeSlashes(span.Path);
        int line = span.StartLinePosition.Line + 1;
        int col = span.StartLinePosition.Character + 1;
        outDiagnostics.Add(new DiagnosticDto
        {
            Severity = "error",
            File = file,
            Line = line,
            Column = col,
            Message = message,
            Raw = $"{file}:{line}:{col}: error: {message}"
        });
    }

    private static bool ValidateUnsupportedClrFeatures(
        string sourcePath,
        CompilationUnitSyntax root,
        SemanticModel semanticModel,
        ProjectTypeIndex index,
        List<DiagnosticDto> outDiagnostics)
    {
        bool ok = true;

        // 0. Check non-static classes without an engine base type
        foreach (ClassDeclarationSyntax classDecl in root.DescendantNodes().OfType<ClassDeclarationSyntax>())
        {
            if (classDecl.Modifiers.Any(m => m.IsKind(SyntaxKind.StaticKeyword)))
            {
                continue;
            }
            if (classDecl.BaseList == null || classDecl.BaseList.Types.Count == 0)
            {
                string className = classDecl.Identifier.ValueText;
                AddNodeError(
                    sourcePath,
                    classDecl,
                    $"C# class '{className}' has no engine base type. Inherit from Behavior or DataAsset, or declare 'struct {className}' for a value type",
                    outDiagnostics);
                ok = false;
            }
        }

        // 0b. Check value structs containing collection fields/properties (copying the struct in C++ copies the Vector)
        foreach (StructDeclarationSyntax structDecl in root.DescendantNodes().OfType<StructDeclarationSyntax>())
        {
            string structName = structDecl.Identifier.ValueText;
            foreach (FieldDeclarationSyntax fieldDecl in structDecl.Members.OfType<FieldDeclarationSyntax>())
            {
                if (IsContainerTypeSyntax(fieldDecl.Declaration.Type, semanticModel, out _))
                {
                    foreach (VariableDeclaratorSyntax v in fieldDecl.Declaration.Variables)
                    {
                        AddNodeError(
                            sourcePath,
                            v,
                            $"Value struct '{structName}' cannot contain collection field '{v.Identifier.ValueText}' (copying the struct in C++ deep-copies the Vector, unlike C# reference semantics)",
                            outDiagnostics);
                        ok = false;
                    }
                }
            }
            foreach (PropertyDeclarationSyntax propDecl in structDecl.Members.OfType<PropertyDeclarationSyntax>())
            {
                if (IsContainerTypeSyntax(propDecl.Type, semanticModel, out _))
                {
                    AddNodeError(
                        sourcePath,
                        propDecl,
                        $"Value struct '{structName}' cannot contain collection property '{propDecl.Identifier.ValueText}' (copying the struct in C++ deep-copies the Vector, unlike C# reference semantics)",
                        outDiagnostics);
                    ok = false;
                }
            }
        }

        // 0c. Allowlist check on declared types (fields, properties, methods, parameters, locals, foreach, object/array creations)
        foreach (FieldDeclarationSyntax fieldDecl in root.DescendantNodes().OfType<FieldDeclarationSyntax>())
        {
            if (!IsAllowedCSharpType(fieldDecl.Declaration.Type, semanticModel, index, out string err))
            {
                AddNodeError(sourcePath, fieldDecl.Declaration.Type, err, outDiagnostics);
                ok = false;
            }
        }
        foreach (PropertyDeclarationSyntax propDecl in root.DescendantNodes().OfType<PropertyDeclarationSyntax>())
        {
            if (!IsAllowedCSharpType(propDecl.Type, semanticModel, index, out string err))
            {
                AddNodeError(sourcePath, propDecl.Type, err, outDiagnostics);
                ok = false;
            }
        }

        // 1. Check async methods / lambdas and await expressions
        foreach (MethodDeclarationSyntax method in root.DescendantNodes().OfType<MethodDeclarationSyntax>())
        {
            if (method.Modifiers.Any(m => m.IsKind(SyntaxKind.AsyncKeyword)))
            {
                AddNodeError(
                    sourcePath,
                    method,
                    $"C# 'async' method '{method.Identifier.ValueText}' is not supported in transpiled C# scripts (no CLR state machine). Use 'OnUpdate(float dt)' timers instead",
                    outDiagnostics);
                ok = false;
            }

            if (!IsAllowedCSharpType(method.ReturnType, semanticModel, index, out string retErr))
            {
                AddNodeError(sourcePath, method.ReturnType, retErr, outDiagnostics);
                ok = false;
            }

            if (IsContainerTypeSyntax(method.ReturnType, semanticModel, out _))
            {
                AddNodeError(
                    sourcePath,
                    method.ReturnType,
                    $"Method '{method.Identifier.ValueText}' cannot return a collection by value (copies the underlying C++ Vector). Pass an output collection by 'ref' instead",
                    outDiagnostics);
                ok = false;
            }

            foreach (ParameterSyntax param in method.ParameterList.Parameters)
            {
                if (param.Type != null && !IsAllowedCSharpType(param.Type, semanticModel, index, out string paramErr))
                {
                    AddNodeError(sourcePath, param.Type, paramErr, outDiagnostics);
                    ok = false;
                }
                if (param.Type != null &&
                    IsContainerTypeSyntax(param.Type, semanticModel, out _) &&
                    !param.Modifiers.Any(m => m.IsKind(SyntaxKind.RefKeyword) || m.IsKind(SyntaxKind.InKeyword) || m.IsKind(SyntaxKind.OutKeyword)))
                {
                    AddNodeError(
                        sourcePath,
                        param,
                        $"Collection parameter '{param.Identifier.ValueText}' must use 'ref' or 'in' (C++ Vector is a value type and would be copied by value)",
                        outDiagnostics);
                    ok = false;
                }
            }
        }

        foreach (PropertyDeclarationSyntax propDecl in root.DescendantNodes().OfType<PropertyDeclarationSyntax>())
        {
            bool isComputed = propDecl.ExpressionBody != null ||
                              (propDecl.AccessorList != null && propDecl.AccessorList.Accessors.Any(a => a.Body != null || a.ExpressionBody != null));
            if (isComputed && IsContainerTypeSyntax(propDecl.Type, semanticModel, out _))
            {
                AddNodeError(
                    sourcePath,
                    propDecl.Type,
                    $"Computed property '{propDecl.Identifier.ValueText}' cannot return a collection by value (copies the underlying C++ Vector)",
                    outDiagnostics);
                ok = false;
            }
        }

        foreach (AwaitExpressionSyntax awaitExpr in root.DescendantNodes().OfType<AwaitExpressionSyntax>())
        {
            AddNodeError(
                sourcePath,
                awaitExpr,
                "C# 'await' requires a managed CLR state machine and is not supported in transpiled C# scripts. Use 'OnUpdate(float dt)' timers instead",
                outDiagnostics);
            ok = false;
        }

        // 2. Check yield return / yield break
        foreach (YieldStatementSyntax yieldStmt in root.DescendantNodes().OfType<YieldStatementSyntax>())
        {
            AddNodeError(
                sourcePath,
                yieldStmt,
                "C# 'yield' iterators require a managed CLR state machine and are not supported in transpiled C# scripts",
                outDiagnostics);
            ok = false;
        }

        // 3. Check `new <ObjectType>(...)` on Shalltear Object-derived types and unsupported `new` types
        foreach (ObjectCreationExpressionSyntax objCreate in root.DescendantNodes().OfType<ObjectCreationExpressionSyntax>())
        {
            string rawType = NormalizeTypeSyntaxText(objCreate.Type);
            string shortType = rawType.StartsWith("Shalltear.", StringComparison.Ordinal) ? rawType.Substring(10) : rawType;
            ITypeSymbol? sym = semanticModel.GetTypeInfo(objCreate.Type).Type;
            if ((ObjectDerivedEngineTypes.Contains(shortType) || index.UserObjectTypes.Contains(shortType) || IsSymbolObjectPointer(sym, index)) &&
                !shortType.StartsWith("Handle<", StringComparison.Ordinal))
            {
                AddNodeError(
                    sourcePath,
                    objCreate,
                    $"Cannot instantiate Shalltear Object type '{shortType}' with 'new' (no managed GC heap). Use 'SceneSystem.CreateObject(...)' or 'AddComponent<{shortType}>()' instead",
                    outDiagnostics);
                ok = false;
            }
            else if (!IsAllowedCSharpType(objCreate.Type, semanticModel, index, out string newTypeErr))
            {
                AddNodeError(sourcePath, objCreate.Type, newTypeErr, outDiagnostics);
                ok = false;
            }
        }

        foreach (ArrayCreationExpressionSyntax arrCreate in root.DescendantNodes().OfType<ArrayCreationExpressionSyntax>())
        {
            AddNodeError(
                sourcePath,
                arrCreate,
                $"C# array allocation '{arrCreate.Type}' is not supported in transpiled C# scripts (no managed GC array). Use 'List<{arrCreate.Type.ElementType}>' instead",
                outDiagnostics);
            ok = false;
        }

        foreach (ImplicitArrayCreationExpressionSyntax impArr in root.DescendantNodes().OfType<ImplicitArrayCreationExpressionSyntax>())
        {
            AddNodeError(
                sourcePath,
                impArr,
                "C# implicit array allocation ('new[]') is not supported in transpiled C# scripts (no managed GC array). Use 'List<T>' instead",
                outDiagnostics);
            ok = false;
        }

        foreach (ForEachStatementSyntax feStmt in root.DescendantNodes().OfType<ForEachStatementSyntax>())
        {
            if (!feStmt.Type.IsVar && !IsAllowedCSharpType(feStmt.Type, semanticModel, index, out string feTypeErr))
            {
                AddNodeError(sourcePath, feStmt.Type, feTypeErr, outDiagnostics);
                ok = false;
            }
        }

        // 4. Check anonymous object creation (`new { ... }`)
        foreach (AnonymousObjectCreationExpressionSyntax anon in root.DescendantNodes().OfType<AnonymousObjectCreationExpressionSyntax>())
        {
            AddNodeError(
                sourcePath,
                anon,
                "Anonymous heap-allocated C# objects ('new { ... }') are not supported in transpiled C# scripts. Declare a value 'struct' instead",
                outDiagnostics);
            ok = false;
        }

        // 5. Check LINQ query expressions and System.Linq allocations
        foreach (QueryExpressionSyntax query in root.DescendantNodes().OfType<QueryExpressionSyntax>())
        {
            AddNodeError(
                sourcePath,
                query,
                "C# LINQ query expressions require managed GC allocations and are not supported in transpiled C# scripts. Use 'foreach', 'List.RemoveAll', or 'List.Exists' instead",
                outDiagnostics);
            ok = false;
        }

        foreach (InvocationExpressionSyntax inv in root.DescendantNodes().OfType<InvocationExpressionSyntax>())
        {
            if (inv.Expression is MemberAccessExpressionSyntax ma)
            {
                string methodName = ma.Name.Identifier.ValueText;
                if (UnsupportedLinqMethods.Contains(methodName))
                {
                    AddNodeError(
                        sourcePath,
                        inv,
                        $"LINQ method '{methodName}' requires managed GC iterator/collection allocations and is not supported in transpiled C# scripts. Use 'foreach', 'List.RemoveAll', or 'List.Exists' instead",
                        outDiagnostics);
                    ok = false;
                }
            }
        }

        // 6. Check `lock (...)` statements
        foreach (LockStatementSyntax lockStmt in root.DescendantNodes().OfType<LockStatementSyntax>())
        {
            AddNodeError(
                sourcePath,
                lockStmt,
                "CLR monitor 'lock' statements are not supported in transpiled C# scripts",
                outDiagnostics);
            ok = false;
        }

        // 7. Check `try`/`catch`/`finally` and `throw` (Shalltear compiles with -fno-exceptions)
        foreach (TryStatementSyntax tryStmt in root.DescendantNodes().OfType<TryStatementSyntax>())
        {
            AddNodeError(
                sourcePath,
                tryStmt,
                "CLR exception handling ('try'/'catch'/'finally') is not supported in transpiled C# scripts (compiled with -fno-exceptions). Use return checks or 'Log.Error' instead",
                outDiagnostics);
            ok = false;
        }

        foreach (ThrowStatementSyntax throwStmt in root.DescendantNodes().OfType<ThrowStatementSyntax>())
        {
            AddNodeError(
                sourcePath,
                throwStmt,
                "CLR 'throw' is not supported in transpiled C# scripts (compiled with -fno-exceptions). Use 'Log.Error' and return early instead",
                outDiagnostics);
            ok = false;
        }

        // 8. Check collection value-copy and rebinding traps using semantic symbol resolution
        foreach (VariableDeclarationSyntax varDecl in root.DescendantNodes().OfType<VariableDeclarationSyntax>())
        {
            if (varDecl.Parent is FieldDeclarationSyntax)
            {
                continue;
            }
            if (!varDecl.Type.IsVar && !IsAllowedCSharpType(varDecl.Type, semanticModel, index, out string localTypeErr))
            {
                AddNodeError(sourcePath, varDecl.Type, localTypeErr, outDiagnostics);
                ok = false;
            }
            bool declIsContainer = IsContainerTypeSyntax(varDecl.Type, semanticModel, out _);
            foreach (VariableDeclaratorSyntax v in varDecl.Variables)
            {
                if (v.Initializer == null)
                {
                    continue;
                }
                ExpressionSyntax rhs = v.Initializer.Value;
                if (varDecl.Type.IsVar && rhs is not TypeOfExpressionSyntax)
                {
                    ITypeSymbol? inferredType = semanticModel.GetTypeInfo(rhs).Type;
                    if (inferredType != null &&
                        inferredType.TypeKind != TypeKind.Error &&
                        !IsAllowedTypeSymbol(inferredType, index, out string varTypeErr))
                    {
                        AddNodeError(sourcePath, v, varTypeErr, outDiagnostics);
                        ok = false;
                    }
                }
                if (rhs is ObjectCreationExpressionSyntax or ImplicitObjectCreationExpressionSyntax or DefaultExpressionSyntax)
                {
                    continue;
                }
                if (declIsContainer || IsExpressionContainer(rhs, semanticModel))
                {
                    AddNodeError(
                        sourcePath,
                        v,
                        $"Assigning collection to local '{v.Identifier.ValueText}' copies the underlying C++ Vector (unlike C# reference semantics). Mutate the collection in place or pass it by 'ref'",
                        outDiagnostics);
                    ok = false;
                }
            }
        }

        foreach (AssignmentExpressionSyntax assign in root.DescendantNodes().OfType<AssignmentExpressionSyntax>())
        {
            if (IsExpressionContainer(assign.Left, semanticModel) || IsExpressionContainer(assign.Right, semanticModel))
            {
                AddNodeError(
                    sourcePath,
                    assign,
                    $"Assigning collection to '{assign.Left}' copies or overwrites the underlying C++ Vector in place (unlike C# reference semantics). Mutate the collection in place (e.g. '.Clear()') or pass it by 'ref'",
                    outDiagnostics);
                ok = false;
            }
        }

        foreach (ArgumentSyntax arg in root.DescendantNodes().OfType<ArgumentSyntax>())
        {
            if (arg.Expression is ObjectCreationExpressionSyntax oc &&
                IsContainerTypeSyntax(oc.Type, semanticModel, out _))
            {
                AddNodeError(
                    sourcePath,
                    arg,
                    "Cannot pass temporary 'new List<T>()' directly as a method argument (collection parameters require 'ref' or 'in' variables)",
                    outDiagnostics);
                ok = false;
            }
            else if (arg.Expression is ImplicitObjectCreationExpressionSyntax &&
                     IsExpressionContainer(arg.Expression, semanticModel))
            {
                AddNodeError(
                    sourcePath,
                    arg,
                    "Cannot pass temporary 'new()' collection directly as a method argument (collection parameters require 'ref' or 'in' variables)",
                    outDiagnostics);
                ok = false;
            }
        }

        foreach (ReturnStatementSyntax retStmt in root.DescendantNodes().OfType<ReturnStatementSyntax>())
        {
            if (retStmt.Expression == null)
            {
                continue;
            }
            if (IsExpressionContainer(retStmt.Expression, semanticModel))
            {
                AddNodeError(
                    sourcePath,
                    retStmt,
                    "Returning a collection copies the underlying C++ Vector (unlike C# reference semantics). Pass an output collection by 'ref' instead",
                    outDiagnostics);
                ok = false;
            }
        }

        // 9. Check lambda, anonymous delegate, and local function capture lifetime traps
        foreach (LambdaExpressionSyntax lambda in root.DescendantNodes().OfType<LambdaExpressionSyntax>())
        {
            bool isSafeListPredicate =
                lambda.Parent is ArgumentSyntax arg &&
                arg.Parent is ArgumentListSyntax argList &&
                argList.Parent is InvocationExpressionSyntax inv &&
                inv.Expression is MemberAccessExpressionSyntax ma &&
                ma.Name.Identifier.ValueText is "RemoveAll" or "Exists" &&
                IsExpressionContainer(ma.Expression, semanticModel);
            if (!isSafeListPredicate)
            {
                AddNodeError(
                    sourcePath,
                    lambda,
                    "Lambdas capture stack locals by reference in C++ and cannot be stored, returned, or passed to arbitrary callbacks. Only inline 'List.RemoveAll' and 'List.Exists' predicates are supported",
                    outDiagnostics);
                ok = false;
            }
        }

        foreach (AnonymousMethodExpressionSyntax anonMethod in root.DescendantNodes().OfType<AnonymousMethodExpressionSyntax>())
        {
            AddNodeError(
                sourcePath,
                anonMethod,
                "Anonymous 'delegate' methods capture stack locals by reference in C++ and are not supported in transpiled C# scripts",
                outDiagnostics);
            ok = false;
        }

        foreach (LocalFunctionStatementSyntax localFn in root.DescendantNodes().OfType<LocalFunctionStatementSyntax>())
        {
            AddNodeError(
                sourcePath,
                localFn,
                $"C# local function '{localFn.Identifier.ValueText}' can capture stack locals by reference in C++ and is not supported in transpiled C# scripts. Declare a private method on the class instead",
                outDiagnostics);
            ok = false;
        }

        // 10. Check member accesses and calls on BCL types (string, List<T>, Math, Console, System.*)
        INamedTypeSymbol? shalltearMath = semanticModel.Compilation.GetTypeByMetadataName("Shalltear.Math");
        foreach (MemberAccessExpressionSyntax ma in root.DescendantNodes().OfType<MemberAccessExpressionSyntax>())
        {
            if (ma.Ancestors().Any(a => a is UsingDirectiveSyntax or AttributeSyntax or BaseListSyntax or TypeArgumentListSyntax))
            {
                continue;
            }
            SymbolInfo maSymInfo = semanticModel.GetSymbolInfo(ma);
            if (maSymInfo.Symbol is INamespaceSymbol or ITypeSymbol)
            {
                continue;
            }

            string memberName = ma.Name.Identifier.ValueText;
            if (UnsupportedLinqMethods.Contains(memberName))
            {
                continue;
            }

            if (IsExpressionContainer(ma.Expression, semanticModel))
            {
                if (!SupportedListMembers.Contains(memberName))
                {
                    AddNodeError(
                        sourcePath,
                        ma,
                        $"List<T> member '{memberName}' is not supported in transpiled C# scripts. Supported List<T> members are 'Add', 'Clear', 'Count', 'RemoveAt', 'RemoveAll', and 'Exists'",
                        outDiagnostics);
                    ok = false;
                }
                continue;
            }

            SymbolInfo recvSymInfo = semanticModel.GetSymbolInfo(ma.Expression);
            ITypeSymbol? staticRecvSym = recvSymInfo.Symbol as ITypeSymbol;
            ITypeSymbol? recvType = semanticModel.GetTypeInfo(ma.Expression).Type;

            string recvText = ma.Expression.ToString().Trim();
            bool isMathReceiver = recvText is "Math" or "System.Math" or "Shalltear.Math" ||
                                  staticRecvSym?.Name == "Math" ||
                                  recvSymInfo.CandidateSymbols.Any(c => c is ITypeSymbol ts && ts.Name == "Math");
            if (isMathReceiver)
            {
                if (shalltearMath != null && shalltearMath.GetMembers(memberName).Length == 0)
                {
                    AddNodeError(
                        sourcePath,
                        ma,
                        $"Member 'Math.{memberName}' is not defined on Shalltear.Math",
                        outDiagnostics);
                    ok = false;
                }
                continue;
            }

            if (recvType?.SpecialType == SpecialType.System_String ||
                staticRecvSym?.SpecialType == SpecialType.System_String)
            {
                if (memberName != "Length")
                {
                    AddNodeError(
                        sourcePath,
                        ma,
                        $"C# string member '{memberName}' is not supported in transpiled C# scripts (maps to C++ '::Shalltear::str'). Supported string property is 'Length'",
                        outDiagnostics);
                    ok = false;
                }
                continue;
            }

            if (staticRecvSym != null)
            {
                string ns = staticRecvSym.ContainingNamespace?.ToDisplayString() ?? "";
                if (ns == "System" || ns.StartsWith("System.", StringComparison.Ordinal))
                {
                    AddNodeError(
                        sourcePath,
                        ma,
                        $"BCL member '{staticRecvSym.Name}.{memberName}' is not supported in transpiled C# scripts (no .NET runtime in built games). Use Shalltear APIs (e.g. 'Log.Info', 'Time', 'Math') instead",
                        outDiagnostics);
                    ok = false;
                    continue;
                }
            }

            if (recvType != null)
            {
                string ns = recvType.ContainingNamespace?.ToDisplayString() ?? "";
                if ((ns == "System" || ns.StartsWith("System.", StringComparison.Ordinal)) &&
                    recvType.Name is not ("List" or "Vector"))
                {
                    AddNodeError(
                        sourcePath,
                        ma,
                        $"BCL member '{memberName}' on '{recvType.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat)}' is not supported in transpiled C# scripts",
                        outDiagnostics);
                    ok = false;
                }
            }
        }

        return ok;
    }

    private static bool IsExpressionContainer(ExpressionSyntax expr, SemanticModel semanticModel)
    {
        if (expr is ObjectCreationExpressionSyntax oc &&
            IsContainerTypeSyntax(oc.Type, semanticModel, out _))
        {
            return true;
        }
        ITypeSymbol? type = semanticModel.GetTypeInfo(expr).Type ?? semanticModel.GetTypeInfo(expr).ConvertedType;
        if (type != null && (type.Name == "List" || type.Name == "Vector"))
        {
            return true;
        }
        ISymbol? sym = semanticModel.GetSymbolInfo(expr).Symbol;
        if (sym != null)
        {
            ITypeSymbol? symType = sym switch
            {
                ILocalSymbol loc => loc.Type,
                IFieldSymbol fld => fld.Type,
                IParameterSymbol prm => prm.Type,
                IPropertySymbol prop => prop.Type,
                IMethodSymbol mth => mth.ReturnType,
                _ => null
            };
            if (symType != null && (symType.Name == "List" || symType.Name == "Vector"))
            {
                return true;
            }
            foreach (SyntaxReference sref in sym.DeclaringSyntaxReferences)
            {
                SyntaxNode declNode = sref.GetSyntax();
                TypeSyntax? declType = declNode switch
                {
                    VariableDeclaratorSyntax vd when vd.Parent is VariableDeclarationSyntax vdecl => vdecl.Type,
                    ParameterSyntax p => p.Type,
                    PropertyDeclarationSyntax pd => pd.Type,
                    MethodDeclarationSyntax md => md.ReturnType,
                    _ => null
                };
                if (declType != null && IsContainerTypeSyntax(declType, semanticModel, out _))
                {
                    return true;
                }
            }
        }
        return false;
    }

    private static bool IsContainerTypeSyntax(TypeSyntax typeSyntax, SemanticModel semanticModel, out string elementType)
    {
        if (IsContainerTypeSpelling(NormalizeTypeSyntaxText(typeSyntax), out elementType))
        {
            return true;
        }
        if (typeSyntax.SyntaxTree == semanticModel.SyntaxTree)
        {
            ITypeSymbol? sym = semanticModel.GetTypeInfo(typeSyntax).Type;
            if (sym is INamedTypeSymbol namedSym &&
                (namedSym.Name == "List" || namedSym.Name == "Vector") &&
                namedSym.TypeArguments.Length == 1)
            {
                elementType = namedSym.TypeArguments[0].ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat);
                return true;
            }
        }
        elementType = "";
        return false;
    }

    private static bool IsAllowedCSharpType(
        TypeSyntax typeSyntax,
        SemanticModel semanticModel,
        ProjectTypeIndex index,
        out string errorMessage)
    {
        if (typeSyntax.IsVar)
        {
            errorMessage = "";
            return true;
        }
        if (typeSyntax is ArrayTypeSyntax arrType)
        {
            errorMessage = $"C# array type '{arrType}' is not supported in transpiled C# scripts (no managed GC array). Use 'List<{arrType.ElementType}>' instead";
            return false;
        }
        if (IsContainerTypeSyntax(typeSyntax, semanticModel, out string elemType))
        {
            return IsAllowedNormalizedTypeString(elemType, index, out errorMessage);
        }
        string normalized = NormalizeTypeSyntaxText(typeSyntax);
        if (IsAllowedNormalizedTypeString(normalized, index, out errorMessage))
        {
            return true;
        }
        if (typeSyntax.SyntaxTree == semanticModel.SyntaxTree)
        {
            ITypeSymbol? sym = semanticModel.GetTypeInfo(typeSyntax).Type;
            if (sym != null &&
                sym.ContainingNamespace?.ToDisplayString() == "Shalltear" &&
                sym.TypeKind is TypeKind.Class or TypeKind.Struct or TypeKind.Enum)
            {
                errorMessage = "";
                return true;
            }
        }
        return false;
    }

    private static bool IsAllowedTypeSymbol(ITypeSymbol sym, ProjectTypeIndex index, out string errorMessage)
    {
        if (sym.TypeKind == TypeKind.Error)
        {
            errorMessage = "";
            return true;
        }
        if (sym is IArrayTypeSymbol arrSym)
        {
            string elem = arrSym.ElementType.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat);
            errorMessage = $"C# array type '{arrSym.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat)}' is not supported in transpiled C# scripts (no managed GC array). Use 'List<{elem}>' instead";
            return false;
        }
        if (sym is INamedTypeSymbol namedSym &&
            (namedSym.Name == "List" || namedSym.Name == "Vector") &&
            namedSym.TypeArguments.Length == 1)
        {
            return IsAllowedTypeSymbol(namedSym.TypeArguments[0], index, out errorMessage);
        }
        if (sym is INamedTypeSymbol handleSym &&
            handleSym.Name == "Handle" &&
            handleSym.TypeArguments.Length == 1)
        {
            return IsAllowedTypeSymbol(handleSym.TypeArguments[0], index, out errorMessage);
        }
        if (sym.ContainingNamespace?.ToDisplayString() == "Shalltear")
        {
            errorMessage = "";
            return true;
        }
        string minDisplay = sym.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat);
        return IsAllowedNormalizedTypeString(minDisplay, index, out errorMessage);
    }

    private static bool IsAllowedNormalizedTypeString(string rawType, ProjectTypeIndex index, out string errorMessage)
    {
        string trimmed = rawType.Trim();
        if (trimmed.StartsWith("global::", StringComparison.Ordinal))
        {
            trimmed = trimmed.Substring(8).Trim();
        }
        if (trimmed.EndsWith("[]", StringComparison.Ordinal))
        {
            string elem = trimmed.Substring(0, trimmed.Length - 2).Trim();
            errorMessage = $"C# array type '{trimmed}' is not supported in transpiled C# scripts (no managed GC array). Use 'List<{elem}>' instead";
            return false;
        }
        if (IsContainerTypeSpelling(trimmed, out string containerElem))
        {
            return IsAllowedNormalizedTypeString(containerElem, index, out errorMessage);
        }
        string shortRaw = trimmed.StartsWith("Shalltear.", StringComparison.Ordinal) ? trimmed.Substring(10) : trimmed;
        if (shortRaw.EndsWith("*", StringComparison.Ordinal))
        {
            shortRaw = shortRaw.Substring(0, shortRaw.Length - 1).Trim();
        }
        if (PrimitiveMap.ContainsKey(trimmed) || PrimitiveMap.ContainsKey(shortRaw))
        {
            errorMessage = "";
            return true;
        }
        if (shortRaw.StartsWith("Handle<", StringComparison.Ordinal) && shortRaw.EndsWith(">", StringComparison.Ordinal))
        {
            string inner = shortRaw.Substring(7, shortRaw.Length - 8).Trim();
            return IsAllowedNormalizedTypeString(inner, index, out errorMessage);
        }
        if (EngineTypeNames.Contains(shortRaw) ||
            ObjectDerivedEngineTypes.Contains(shortRaw) ||
            StaticScopeIdentifiers.Contains(shortRaw) ||
            index.UserObjectTypes.Contains(shortRaw) ||
            index.UserValueStructs.Contains(shortRaw) ||
            index.UserEnumNames.Contains(shortRaw))
        {
            errorMessage = "";
            return true;
        }
        int lastDot = shortRaw.LastIndexOf('.');
        if (lastDot > 0 && !shortRaw.StartsWith("System.", StringComparison.Ordinal))
        {
            string unqual = shortRaw.Substring(lastDot + 1);
            if (index.UserObjectTypes.Contains(unqual) ||
                index.UserValueStructs.Contains(unqual) ||
                index.UserEnumNames.Contains(unqual))
            {
                errorMessage = "";
                return true;
            }
        }
        errorMessage = $"C# type '{trimmed}' is not supported in transpiled C# scripts. Supported types are primitives, 'string', 'List<T>', 'Handle<T>', Shalltear engine types, and project classes/structs/enums";
        return false;
    }

    private static bool EmitTranspiledFile(
        string sourcePath,
        string relativePath,
        SyntaxTree tree,
        SemanticModel semanticModel,
        ProjectTypeIndex index,
        FileTranspileDto outDto)
    {
        CompilationUnitSyntax root = tree.GetCompilationUnitRoot();
        if (!ValidateUnsupportedClrFeatures(sourcePath, root, semanticModel, index, outDto.Diagnostics))
        {
            return false;
        }

        string ownHeaderRel = ChangeExtensionToGeneratedHeader(relativePath);
        string stem = Path.GetFileNameWithoutExtension(sourcePath);

        var referencedHeaders = new SortedSet<string>(StringComparer.Ordinal);
        var referencedUmbrellaHeaders = new SortedSet<string>(StringComparer.Ordinal);
        bool usesPhysicsCallbacks = false;

        foreach (IdentifierNameSyntax idNode in root.DescendantNodes().OfType<IdentifierNameSyntax>())
        {
            string name = idNode.Identifier.ValueText;
            if (index.TypeToGeneratedHeader.TryGetValue(name, out string? depHeader) &&
                !string.Equals(depHeader, ownHeaderRel, StringComparison.Ordinal))
            {
                referencedHeaders.Add(depHeader);
            }
            if (TypeToModuleUmbrellaHeader.TryGetValue(name, out string? umbrellaHeader))
            {
                referencedUmbrellaHeaders.Add(umbrellaHeader);
            }
        }

        foreach (MethodDeclarationSyntax m in root.DescendantNodes().OfType<MethodDeclarationSyntax>())
        {
            if (PhysicsCallbackMethodNames.Contains(m.Identifier.ValueText))
            {
                referencedUmbrellaHeaders.Add("Shalltear/Physics3D.h");
                usesPhysicsCallbacks = true;
            }
        }

        var header = new StringBuilder();
        header.Append("#pragma once\n\n");
        header.Append($"// GENERATED by Shalltear Roslyn CSharpTranspiler from '{sourcePath}'. Do not edit manually.\n\n");
        header.Append("#include \"Shalltear.h\"\n");
        header.Append("#include \"Shalltear/Core/Asset/DataAsset.h\"\n");
        foreach (string umbrellaHeader in referencedUmbrellaHeaders)
        {
            header.Append($"#if __has_include(\"{umbrellaHeader}\")\n");
            header.Append($"#include \"{umbrellaHeader}\"\n");
            header.Append("#endif\n");
        }
        if (usesPhysicsCallbacks)
        {
            header.Append("#if __has_include(\"Shalltear/Physics3D/PhysicsListener.h\")\n");
            header.Append("#include \"Shalltear/Physics3D/PhysicsListener.h\"\n");
            header.Append("#endif\n");
        }
        foreach (string depHeader in referencedHeaders)
        {
            header.Append($"#include \"{depHeader}\"\n");
        }
        header.Append('\n');

        var cpp = new StringBuilder();
        cpp.Append($"// GENERATED by Shalltear Roslyn CSharpTranspiler from '{sourcePath}'. Do not edit manually.\n\n");
        cpp.Append("#include \"Shalltear.h\"\n");
        cpp.Append("#include \"Shalltear/Core/Asset/DataAsset.h\"\n");
        cpp.Append("#include <algorithm>\n");
        if (!string.IsNullOrEmpty(stem))
        {
            cpp.Append($"#include \"{stem}.generated.h\"\n");
        }
        cpp.Append('\n');

        var typeDecls = root.DescendantNodes().OfType<BaseTypeDeclarationSyntax>().ToList();
        foreach (BaseTypeDeclarationSyntax typeDecl in typeDecls)
        {
            string nsName = GetEnclosingNamespace(typeDecl);
            string cppNs = nsName.Replace(".", "::");
            if (!string.IsNullOrEmpty(cppNs))
            {
                header.Append($"namespace {cppNs} {{\n\n");
                cpp.Append($"namespace {cppNs} {{\n\n");
            }

            if (typeDecl is EnumDeclarationSyntax enumDecl)
            {
                EmitEnum(enumDecl, cppNs, header, cpp, outDto);
                continue;
            }

            if (typeDecl is StructDeclarationSyntax structDecl)
            {
                EmitStruct(structDecl, cppNs, index, header, cpp, outDto);
                continue;
            }

            if (typeDecl is ClassDeclarationSyntax classDecl)
            {
                if (!EmitClass(sourcePath, classDecl, cppNs, semanticModel, index, header, cpp, outDto))
                {
                    return false;
                }
            }
        }

        outDto.HeaderContent = header.ToString();
        outDto.SourceContent = cpp.ToString();
        return true;
    }

    private static void EmitEnum(
        EnumDeclarationSyntax enumDecl,
        string cppNs,
        StringBuilder header,
        StringBuilder cpp,
        FileTranspileDto outDto)
    {
        string enumName = enumDecl.Identifier.ValueText;
        header.Append($"enum class {enumName} : ::Shalltear::i32 {{\n");
        foreach (EnumMemberDeclarationSyntax member in enumDecl.Members)
        {
            header.Append($"\t{member.Identifier.ValueText}");
            if (member.EqualsValue != null)
            {
                header.Append($" = {member.EqualsValue.Value.ToFullString().Trim()}");
            }
            header.Append(",\n");
        }
        header.Append("};\n\n");

        if (!string.IsNullOrEmpty(cppNs))
        {
            header.Append($"}} // namespace {cppNs}\n\n");
            string qualified = $"{cppNs}::{enumName}";
            header.Append($"SLT_REFLECT_ENUM({qualified})\n");
            foreach (EnumMemberDeclarationSyntax member in enumDecl.Members)
            {
                header.Append($"\tSLT_ENUM_VALUE({member.Identifier.ValueText})\n");
            }
            header.Append("SLT_END_ENUM()\n\n");
            cpp.Append($"}} // namespace {cppNs}\n\n");
        }
        else
        {
            header.Append($"SLT_REFLECT_ENUM({enumName})\n");
            foreach (EnumMemberDeclarationSyntax member in enumDecl.Members)
            {
                header.Append($"\tSLT_ENUM_VALUE({member.Identifier.ValueText})\n");
            }
            header.Append("SLT_END_ENUM()\n\n");
        }
        outDto.ReflectedTypes.Add(enumName);
    }

    private sealed class ExtractedMemberField
    {
        public string Name { get; init; } = "";
        public string RawType { get; init; } = "";
        public string Initializer { get; init; } = "";
        public bool IsPublic { get; init; }
        public bool IsStatic { get; init; }
        public bool IsConst { get; init; }
        public SyntaxList<AttributeListSyntax> AttributeLists { get; init; }
    }

    private static List<ExtractedMemberField> ExtractFieldsAndAutoProperties(TypeDeclarationSyntax typeDecl)
    {
        var list = new List<ExtractedMemberField>();
        foreach (MemberDeclarationSyntax member in typeDecl.Members)
        {
            if (member is FieldDeclarationSyntax fieldDecl)
            {
                bool isPublic = fieldDecl.Modifiers.Any(m => m.IsKind(SyntaxKind.PublicKeyword));
                bool isStatic = fieldDecl.Modifiers.Any(m => m.IsKind(SyntaxKind.StaticKeyword));
                bool isConst = fieldDecl.Modifiers.Any(m => m.IsKind(SyntaxKind.ConstKeyword));
                string rawType = NormalizeTypeSyntaxText(fieldDecl.Declaration.Type);
                foreach (VariableDeclaratorSyntax variable in fieldDecl.Declaration.Variables)
                {
                    list.Add(new ExtractedMemberField
                    {
                        Name = variable.Identifier.ValueText,
                        RawType = rawType,
                        Initializer = variable.Initializer?.Value.ToString().Trim() ?? "",
                        IsPublic = isPublic,
                        IsStatic = isStatic,
                        IsConst = isConst,
                        AttributeLists = fieldDecl.AttributeLists
                    });
                }
            }
            else if (member is PropertyDeclarationSyntax propDecl)
            {
                bool isAuto = propDecl.ExpressionBody == null &&
                              propDecl.AccessorList != null &&
                              propDecl.AccessorList.Accessors.All(a => a.Body == null && a.ExpressionBody == null);
                if (!isAuto)
                {
                    continue;
                }
                bool isPublic = propDecl.Modifiers.Any(m => m.IsKind(SyntaxKind.PublicKeyword));
                bool isStatic = propDecl.Modifiers.Any(m => m.IsKind(SyntaxKind.StaticKeyword));
                string rawType = NormalizeTypeSyntaxText(propDecl.Type);
                list.Add(new ExtractedMemberField
                {
                    Name = propDecl.Identifier.ValueText,
                    RawType = rawType,
                    Initializer = propDecl.Initializer?.Value.ToString().Trim() ?? "",
                    IsPublic = isPublic,
                    IsStatic = isStatic,
                    IsConst = false,
                    AttributeLists = propDecl.AttributeLists
                });
            }
        }
        return list;
    }

    private static List<ComputedPropertyInfo> ExtractComputedProperties(TypeDeclarationSyntax typeDecl)
    {
        var list = new List<ComputedPropertyInfo>();
        foreach (PropertyDeclarationSyntax propDecl in typeDecl.Members.OfType<PropertyDeclarationSyntax>())
        {
            bool isAuto = propDecl.ExpressionBody == null &&
                          propDecl.AccessorList != null &&
                          propDecl.AccessorList.Accessors.All(a => a.Body == null && a.ExpressionBody == null);
            if (isAuto)
            {
                continue;
            }
            AccessorDeclarationSyntax? getAcc = propDecl.AccessorList?.Accessors
                .FirstOrDefault(a => a.IsKind(SyntaxKind.GetAccessorDeclaration));
            AccessorDeclarationSyntax? setAcc = propDecl.AccessorList?.Accessors
                .FirstOrDefault(a => a.IsKind(SyntaxKind.SetAccessorDeclaration));
            list.Add(new ComputedPropertyInfo
            {
                Name = propDecl.Identifier.ValueText,
                RawType = NormalizeTypeSyntaxText(propDecl.Type),
                IsStatic = propDecl.Modifiers.Any(m => m.IsKind(SyntaxKind.StaticKeyword)),
                ExpressionGetter = propDecl.ExpressionBody,
                GetAccessor = getAcc,
                SetAccessor = setAcc
            });
        }
        return list;
    }

    private static void EmitStruct(
        StructDeclarationSyntax structDecl,
        string cppNs,
        ProjectTypeIndex index,
        StringBuilder header,
        StringBuilder cpp,
        FileTranspileDto outDto)
    {
        string structName = structDecl.Identifier.ValueText;
        List<ExtractedMemberField> fields = ExtractFieldsAndAutoProperties(structDecl);
        bool reflectStruct = HasAttribute(structDecl.AttributeLists, "Serializable") || fields.Count > 0;

        header.Append($"struct {structName} {{\n");
        if (reflectStruct)
        {
            header.Append($"\tSLT_REFLECT_VALUE({structName})\n");
            header.Append("\tSLT_REFLECT_CONSTRUCTIBLE()\n");
            foreach (ExtractedMemberField field in fields)
            {
                if (field.IsStatic || field.IsConst)
                {
                    continue;
                }
                if (!field.IsPublic)
                {
                    continue;
                }
                string attrList = BuildFieldAttributesMacroList(field.AttributeLists);
                if (IsContainerTypeSpelling(field.RawType, out _))
                {
                    header.Append($"\tSLT_REFLECT_ARRAY({field.Name}{attrList})\n");
                }
                else if (index.UserValueStructs.Contains(field.RawType) || HasAttribute(field.AttributeLists, "Subobject"))
                {
                    header.Append($"\tSLT_REFLECT_SUBOBJECT({field.Name}{attrList})\n");
                }
                else
                {
                    header.Append($"\tSLT_REFLECT_FIELD({field.Name}{attrList})\n");
                }
            }
            header.Append("\tSLT_END_REFLECT()\n\n");
            outDto.ReflectedTypes.Add(structName);
        }

        foreach (ExtractedMemberField field in fields)
        {
            string mappedType = MapTypeName(field.RawType, index, pointerForObjectType: false, wrapObjectInHandle: true);
            string init = TranspileInitializer(field.Initializer, index);
            header.Append($"\t{mappedType} {field.Name}");
            if (!string.IsNullOrEmpty(init))
            {
                header.Append($" = {init}");
            }
            else if (!mappedType.StartsWith("::Shalltear::Handle<", StringComparison.Ordinal))
            {
                header.Append("{}");
            }
            header.Append(";\n");
        }
        header.Append("};\n\n");

        if (!string.IsNullOrEmpty(cppNs))
        {
            header.Append($"}} // namespace {cppNs}\n\n");
            cpp.Append($"}} // namespace {cppNs}\n\n");
            if (reflectStruct)
            {
                cpp.Append($"SLT_REGISTER_TYPE({cppNs}::{structName})\n\n");
            }
        }
        else if (reflectStruct)
        {
            cpp.Append($"SLT_REGISTER_TYPE({structName})\n\n");
        }
    }

    private static bool EmitClass(
        string sourcePath,
        ClassDeclarationSyntax classDecl,
        string cppNs,
        SemanticModel semanticModel,
        ProjectTypeIndex index,
        StringBuilder header,
        StringBuilder cpp,
        FileTranspileDto outDto)
    {
        string className = classDecl.Identifier.ValueText;
        if (classDecl.BaseList == null || classDecl.BaseList.Types.Count == 0)
        {
            AddNodeError(
                sourcePath,
                classDecl.Identifier.Parent ?? classDecl,
                $"C# class '{className}' has no engine base type. Inherit from Behavior or DataAsset, or declare 'struct {className}' for a value type",
                outDto.Diagnostics);
            return false;
        }
        string baseRaw = NormalizeTypeSyntaxText(classDecl.BaseList.Types[0].Type);
        if (baseRaw.StartsWith("Shalltear.", StringComparison.Ordinal))
        {
            baseRaw = baseRaw.Substring(10);
        }

        if (baseRaw == "SceneObject")
        {
            FileLinePositionSpan span = classDecl.Identifier.GetLocation().GetLineSpan();
            int line = span.StartLinePosition.Line + 1;
            int col = span.StartLinePosition.Character + 1;
            string msg = $"C# class '{className}' cannot inherit from SceneObject. Inherit from Behavior or Component";
            outDto.Diagnostics.Add(new DiagnosticDto
            {
                Severity = "error",
                File = sourcePath,
                Line = line,
                Column = col,
                Message = msg,
                Raw = $"{sourcePath}:{line}:{col}: error: {msg}"
            });
            return false;
        }

        if (!ObjectDerivedEngineTypes.Contains(baseRaw) && !index.UserObjectTypes.Contains(baseRaw))
        {
            AddNodeError(
                sourcePath,
                classDecl.BaseList.Types[0],
                $"C# class '{className}' must inherit from Behavior, Component, or DataAsset (got '{baseRaw}')",
                outDto.Diagnostics);
            return false;
        }

        bool isDataAsset = baseRaw == "DataAsset" || baseRaw == "Asset";
        string cppBase = MapTypeName(baseRaw, index, pointerForObjectType: false, wrapObjectInHandle: false);

        List<MethodDeclarationSyntax> methods = classDecl.Members.OfType<MethodDeclarationSyntax>().ToList();
        bool hasPhysicsCallbacks = methods.Any(m => PhysicsCallbackMethodNames.Contains(m.Identifier.ValueText));

        header.Append($"class {className} : public {cppBase}");
        if (hasPhysicsCallbacks && !isDataAsset)
        {
            header.Append(", public ::Shalltear::PhysicsListener");
        }
        header.Append(" {\n");
        header.Append("public:\n");
        header.Append($"\tusing Super = {cppBase};\n");

        if (isDataAsset)
        {
            header.Append($"\tSLT_REFLECT_CLASS({className})\n");
            header.Append($"\tSLT_REFLECT_PARENT({cppBase})\n");
            List<string>? menuArgs = FindAttributeArguments(classDecl.AttributeLists, "CreateAssetMenu");
            if (menuArgs != null && menuArgs.Count >= 2)
            {
                header.Append($"\tSLT_CREATE_ASSET_MENU({menuArgs[0]}, {menuArgs[1]})\n");
            }
            else
            {
                header.Append("\tSLT_REFLECT_CREATABLE()\n");
            }
        }
        else
        {
            header.Append($"\tSLT_REFLECT_COMPONENT({className}, {cppBase})\n");
        }

        List<string>? settingsCat = FindAttributeArguments(classDecl.AttributeLists, "SettingsCategory");
        if (settingsCat != null && settingsCat.Count >= 1)
        {
            header.Append($"\tSLT_SETTINGS_CATEGORY({settingsCat[0]})\n");
        }

        List<ExtractedMemberField> fields = ExtractFieldsAndAutoProperties(classDecl);
        List<ComputedPropertyInfo> computedProps = ExtractComputedProperties(classDecl);

        foreach (ExtractedMemberField field in fields)
        {
            if (field.IsStatic || field.IsConst)
            {
                continue;
            }
            if (!field.IsPublic)
            {
                continue;
            }
            string attrList = BuildFieldAttributesMacroList(field.AttributeLists);
            if (IsContainerTypeSpelling(field.RawType, out _))
            {
                header.Append($"\tSLT_REFLECT_ARRAY({field.Name}{attrList})\n");
            }
            else if (HasAttribute(field.AttributeLists, "SubobjectArray"))
            {
                header.Append($"\tSLT_REFLECT_SUBOBJECT_ARRAY({field.Name}{attrList})\n");
            }
            else if (index.UserValueStructs.Contains(field.RawType) || HasAttribute(field.AttributeLists, "Subobject"))
            {
                header.Append($"\tSLT_REFLECT_SUBOBJECT({field.Name}{attrList})\n");
            }
            else
            {
                header.Append($"\tSLT_REFLECT_FIELD({field.Name}{attrList})\n");
            }
        }

        foreach (MethodDeclarationSyntax method in methods)
        {
            if (HasAttribute(method.AttributeLists, "Callable") ||
                HasAttribute(method.AttributeLists, "ReflectMethod"))
            {
                header.Append($"\tSLT_REFLECT_METHOD({method.Identifier.ValueText})\n");
            }
        }

        header.Append("\tSLT_END_REFLECT()\n\n");
        outDto.ReflectedTypes.Add(className);

        foreach (ExtractedMemberField field in fields)
        {
            if (IsContainerTypeSpelling(field.RawType, out string elemType))
            {
                string mappedElem = MapTypeName(elemType, index, pointerForObjectType: false, wrapObjectInHandle: true);
                header.Append($"\t::Shalltear::Vector<{mappedElem}> {field.Name}{{::Shalltear::GetMemoryResource(::Shalltear::MemTag::Scripting)}};\n");
                continue;
            }
            string mappedType = MapTypeName(field.RawType, index, pointerForObjectType: false, wrapObjectInHandle: true);
            string init = TranspileInitializer(field.Initializer, index);
            header.Append('\t');
            if (field.IsStatic && field.IsConst)
            {
                header.Append("static constexpr ");
            }
            else if (field.IsStatic)
            {
                header.Append("static ");
            }
            header.Append($"{mappedType} {field.Name}");
            if (!string.IsNullOrEmpty(init))
            {
                header.Append($" = {init}");
            }
            else if (!mappedType.StartsWith("::Shalltear::Handle<", StringComparison.Ordinal))
            {
                header.Append("{}");
            }
            header.Append(";\n");
        }

        // Emit computed property getters/setters in header
        foreach (ComputedPropertyInfo prop in computedProps)
        {
            string propType = MapTypeName(prop.RawType, index, pointerForObjectType: true, wrapObjectInHandle: false);
            string staticPrefix = prop.IsStatic ? "static " : "";
            string constSuffix = prop.IsStatic ? "" : " const";
            if (prop.ExpressionGetter != null || prop.GetAccessor != null)
            {
                header.Append($"\t{staticPrefix}{propType} Get{prop.Name}(){constSuffix};\n");
            }
            if (prop.SetAccessor != null)
            {
                header.Append($"\t{staticPrefix}void Set{prop.Name}({propType} value);\n");
            }
        }

        if (methods.Count > 0 || computedProps.Count > 0)
        {
            header.Append('\n');
        }

        foreach (MethodDeclarationSyntax method in methods)
        {
            string methodName = method.Identifier.ValueText;
            bool isLifecycle = LifecycleMethodNames.Contains(methodName);
            bool isPhysicsCallback = PhysicsCallbackMethodNames.Contains(methodName);
            bool isTickWithDelta = (methodName == "OnUpdate" || methodName == "OnFixedUpdate" || methodName == "OnLateUpdate") &&
                                   method.ParameterList.Parameters.Count == 1;
            bool isStatic = method.Modifiers.Any(m => m.IsKind(SyntaxKind.StaticKeyword));
            bool isVirtual = method.Modifiers.Any(m => m.IsKind(SyntaxKind.VirtualKeyword));
            bool isOverride = method.Modifiers.Any(m => m.IsKind(SyntaxKind.OverrideKeyword));
            string retType = MapTypeName(NormalizeTypeSyntaxText(method.ReturnType), index, pointerForObjectType: true, wrapObjectInHandle: false);

            header.Append('\t');
            if (isStatic)
            {
                header.Append("static ");
            }
            else if (isVirtual && !isLifecycle && !isPhysicsCallback && !isOverride)
            {
                header.Append("virtual ");
            }
            header.Append($"{retType} {methodName}(");
            if (!isTickWithDelta)
            {
                AppendMethodParameters(method.ParameterList.Parameters, isPhysicsCallback, index, header);
            }
            header.Append(')');
            if (isLifecycle || isPhysicsCallback || isOverride)
            {
                header.Append(" override");
            }
            header.Append(";\n");
        }

        header.Append("};\n\n");

        // Emit computed property definitions in .cpp
        foreach (ComputedPropertyInfo prop in computedProps)
        {
            string propType = MapTypeName(prop.RawType, index, pointerForObjectType: true, wrapObjectInHandle: false);
            string constSuffix = prop.IsStatic ? "" : " const";
            if (prop.ExpressionGetter != null)
            {
                string exprCode = LowerTokensInNode(
                    sourcePath,
                    prop.ExpressionGetter.Expression,
                    semanticModel,
                    index,
                    className,
                    new HashSet<string>(StringComparer.Ordinal),
                    new HashSet<string>(StringComparer.Ordinal),
                    new HashSet<string>(StringComparer.Ordinal),
                    emitLineDirectives: false);
                cpp.Append($"{propType} {className}::Get{prop.Name}(){constSuffix} {{\n\treturn {exprCode};\n}}\n\n");
            }
            else if (prop.GetAccessor != null)
            {
                string bodyCode = prop.GetAccessor.ExpressionBody != null
                    ? "return " + LowerTokensInNode(sourcePath, prop.GetAccessor.ExpressionBody.Expression, semanticModel, index, className, new(), new(), new(), false) + ";"
                    : prop.GetAccessor.Body != null
                        ? LowerTokensInBlock(sourcePath, prop.GetAccessor.Body, semanticModel, index, className, new(), new(), new())
                        : "";
                cpp.Append($"{propType} {className}::Get{prop.Name}(){constSuffix} {{\n\t{bodyCode}\n}}\n\n");
            }

            if (prop.SetAccessor != null)
            {
                string bodyCode = prop.SetAccessor.ExpressionBody != null
                    ? LowerTokensInNode(sourcePath, prop.SetAccessor.ExpressionBody.Expression, semanticModel, index, className, new(), new(), new(), false) + ";"
                    : prop.SetAccessor.Body != null
                        ? LowerTokensInBlock(sourcePath, prop.SetAccessor.Body, semanticModel, index, className, new(), new(), new())
                        : "";
                cpp.Append($"void {className}::Set{prop.Name}({propType} value) {{\n\t{bodyCode}\n}}\n\n");
            }
        }

        foreach (MethodDeclarationSyntax method in methods)
        {
            string methodName = method.Identifier.ValueText;
            bool isPhysicsCallback = PhysicsCallbackMethodNames.Contains(methodName);
            bool isTickWithDelta = (methodName == "OnUpdate" || methodName == "OnFixedUpdate" || methodName == "OnLateUpdate") &&
                                   method.ParameterList.Parameters.Count == 1;
            string retType = MapTypeName(NormalizeTypeSyntaxText(method.ReturnType), index, pointerForObjectType: true, wrapObjectInHandle: false);
            int methodLine = method.Identifier.GetLocation().GetLineSpan().StartLinePosition.Line + 1;
            if (!string.IsNullOrEmpty(sourcePath) && methodLine > 0)
            {
                cpp.Append($"#line {methodLine} \"{sourcePath}\"\n");
            }
            cpp.Append($"{retType} {className}::{methodName}(");
            if (!isTickWithDelta)
            {
                AppendMethodParameters(method.ParameterList.Parameters, isPhysicsCallback, index, cpp);
            }
            cpp.Append(") {\n\t");
            if (isTickWithDelta)
            {
                string dtParamName = method.ParameterList.Parameters[0].Identifier.ValueText;
                cpp.Append($"[[maybe_unused]] const ::Shalltear::f32 {dtParamName} = ::Shalltear::Time::GetDeltaTime();\n\t");
            }
            cpp.Append(TranspileMethodBody(sourcePath, className, fields, method, semanticModel, index));
            cpp.Append("\n}\n\n");
        }

        if (!string.IsNullOrEmpty(cppNs))
        {
            header.Append($"}} // namespace {cppNs}\n\n");
            cpp.Append($"}} // namespace {cppNs}\n\n");
            cpp.Append($"SLT_REGISTER_TYPE({cppNs}::{className})\n\n");
        }
        else
        {
            cpp.Append($"SLT_REGISTER_TYPE({className})\n\n");
        }

        return true;
    }

    private static void AppendMethodParameters(
        SeparatedSyntaxList<ParameterSyntax> parameters,
        bool isPhysicsCallback,
        ProjectTypeIndex index,
        StringBuilder sb)
    {
        for (int p = 0; p < parameters.Count; ++p)
        {
            if (p > 0)
            {
                sb.Append(", ");
            }
            ParameterSyntax param = parameters[p];
            string rawType = param.Type != null ? NormalizeTypeSyntaxText(param.Type) : "auto";
            bool isOutOrRef = param.Modifiers.Any(m => m.IsKind(SyntaxKind.OutKeyword) || m.IsKind(SyntaxKind.RefKeyword));
            bool isIn = param.Modifiers.Any(m => m.IsKind(SyntaxKind.InKeyword));
            if (isPhysicsCallback)
            {
                if (rawType.EndsWith("Collision", StringComparison.Ordinal))
                {
                    sb.Append($"const ::Shalltear::Collision& {param.Identifier.ValueText}");
                    continue;
                }
                if (rawType.EndsWith("Collider", StringComparison.Ordinal))
                {
                    sb.Append($"::Shalltear::Handle<::Shalltear::Collider> {param.Identifier.ValueText}");
                    continue;
                }
            }
            string paramType = MapTypeName(rawType, index, pointerForObjectType: true, wrapObjectInHandle: false);
            if (isIn)
            {
                paramType = $"const {paramType}&";
            }
            else if (isOutOrRef)
            {
                paramType += "&";
            }
            sb.Append($"{paramType} {param.Identifier.ValueText}");
        }
    }

    private static string TranspileMethodBody(
        string sourcePath,
        string enclosingClassName,
        List<ExtractedMemberField> classFields,
        MethodDeclarationSyntax method,
        SemanticModel semanticModel,
        ProjectTypeIndex index)
    {
        var pointerLocals = new HashSet<string>(StringComparer.Ordinal);
        var containerLocals = new HashSet<string>(StringComparer.Ordinal);
        var stringLocals = new HashSet<string>(StringComparer.Ordinal);

        foreach (ExtractedMemberField field in classFields)
        {
            if (IsContainerTypeSpelling(field.RawType, out _))
            {
                containerLocals.Add(field.Name);
            }
            else if (field.RawType is "string" or "String" or "System.String")
            {
                stringLocals.Add(field.Name);
            }
            else if (IsRawTypeObjectReference(field.RawType, index))
            {
                pointerLocals.Add(field.Name);
            }
        }

        foreach (ParameterSyntax param in method.ParameterList.Parameters)
        {
            string rawType = param.Type != null ? NormalizeTypeSyntaxText(param.Type) : "";
            string mapped = MapTypeName(rawType, index, pointerForObjectType: true, wrapObjectInHandle: false);
            if (mapped.EndsWith("*", StringComparison.Ordinal) || IsRawTypeObjectReference(rawType, index))
            {
                pointerLocals.Add(param.Identifier.ValueText);
            }
            if (IsContainerTypeSpelling(rawType, out _))
            {
                containerLocals.Add(param.Identifier.ValueText);
            }
            else if (rawType is "string" or "String")
            {
                stringLocals.Add(param.Identifier.ValueText);
            }
        }

        // Pre-discover local variables via Roslyn AST + SemanticModel
        foreach (VariableDeclarationSyntax varDecl in method.DescendantNodes().OfType<VariableDeclarationSyntax>())
        {
            string rawType = NormalizeTypeSyntaxText(varDecl.Type);
            TypeInfo semanticType = semanticModel.GetTypeInfo(varDecl.Type);
            foreach (VariableDeclaratorSyntax v in varDecl.Variables)
            {
                string localName = v.Identifier.ValueText;
                if (IsSymbolObjectPointer(semanticType.Type, index))
                {
                    pointerLocals.Add(localName);
                }
                else
                {
                    string mapped = MapTypeName(rawType, index, pointerForObjectType: true, wrapObjectInHandle: false);
                    if (mapped.EndsWith("*", StringComparison.Ordinal) || IsRawTypeObjectReference(rawType, index))
                    {
                        pointerLocals.Add(localName);
                    }
                    else if (rawType == "var" && v.Initializer != null)
                    {
                        TypeInfo initType = semanticModel.GetTypeInfo(v.Initializer.Value);
                        if (IsSymbolObjectPointer(initType.Type, index) ||
                            InitializerLooksLikeObjectPointer(v.Initializer.Value))
                        {
                            pointerLocals.Add(localName);
                        }
                    }
                }

                if (IsContainerTypeSpelling(rawType, out _))
                {
                    containerLocals.Add(localName);
                }
                else if (rawType is "string" or "String" || semanticType.Type?.SpecialType == SpecialType.System_String)
                {
                    stringLocals.Add(localName);
                }
            }
        }

        // Also discover pattern variables: `if (x is Enemy e)` -> `e` is a pointer local
        foreach (DeclarationPatternSyntax declPat in method.DescendantNodes().OfType<DeclarationPatternSyntax>())
        {
            if (declPat.Designation is SingleVariableDesignationSyntax singleVar)
            {
                string rawPatType = NormalizeTypeSyntaxText(declPat.Type);
                if (IsRawTypeObjectReference(rawPatType, index))
                {
                    pointerLocals.Add(singleVar.Identifier.ValueText);
                }
            }
        }

        if (method.ExpressionBody != null)
        {
            int exprLine = method.ExpressionBody.Expression.GetLocation().GetLineSpan().StartLinePosition.Line + 1;
            string lineDir = !string.IsNullOrEmpty(sourcePath) ? $"#line {exprLine} \"{sourcePath}\"\n\t" : "";
            bool isVoid = NormalizeTypeSyntaxText(method.ReturnType) == "void";
            string lowered = LowerTokensInNode(
                sourcePath,
                method.ExpressionBody.Expression,
                semanticModel,
                index,
                enclosingClassName,
                pointerLocals,
                containerLocals,
                stringLocals,
                emitLineDirectives: false);
            return isVoid ? $"{lineDir}{lowered};" : $"{lineDir}return {lowered};";
        }

        if (method.Body == null)
        {
            return "";
        }

        return LowerTokensInBlock(
            sourcePath,
            method.Body,
            semanticModel,
            index,
            enclosingClassName,
            pointerLocals,
            containerLocals,
            stringLocals);
    }

    private static bool IsRawTypeObjectReference(string rawType, ProjectTypeIndex index)
    {
        string clean = rawType.StartsWith("Shalltear.", StringComparison.Ordinal) ? rawType.Substring(10) : rawType;
        if (clean.StartsWith("Handle<", StringComparison.Ordinal) && clean.EndsWith(">", StringComparison.Ordinal))
        {
            return true;
        }
        return ObjectDerivedEngineTypes.Contains(clean) || index.UserObjectTypes.Contains(clean);
    }

    private static bool IsSymbolObjectPointer(ITypeSymbol? symbol, ProjectTypeIndex index)
    {
        if (symbol == null)
        {
            return false;
        }
        if (symbol.Name == "Handle" && symbol is INamedTypeSymbol named && named.IsGenericType)
        {
            return true;
        }
        if (symbol.IsValueType)
        {
            return false;
        }
        for (ITypeSymbol? cur = symbol; cur != null; cur = cur.BaseType)
        {
            if (cur.SpecialType == SpecialType.System_Object ||
                string.Equals(cur.ContainingNamespace?.Name, "System", StringComparison.Ordinal))
            {
                continue;
            }
            if (ObjectDerivedEngineTypes.Contains(cur.Name) || index.UserObjectTypes.Contains(cur.Name))
            {
                return true;
            }
        }
        return false;
    }

    private static bool InitializerLooksLikeObjectPointer(ExpressionSyntax expr)
    {
        string text = expr.ToString();
        return text.Contains("GetComponent", StringComparison.Ordinal) ||
               text.Contains("AddComponent", StringComparison.Ordinal) ||
               text.Contains("CreateObject", StringComparison.Ordinal) ||
               text.Contains("Instantiate", StringComparison.Ordinal) ||
               text.Contains("FindObjectByName", StringComparison.Ordinal) ||
               text.Contains("FindChildByName", StringComparison.Ordinal) ||
               text.EndsWith(".Get()", StringComparison.Ordinal) ||
               text.EndsWith(".Resolve()", StringComparison.Ordinal);
    }

    private static string LowerTokensInBlock(
        string sourcePath,
        BlockSyntax bodyBlock,
        SemanticModel semanticModel,
        ProjectTypeIndex index,
        string enclosingClassName,
        HashSet<string> pointerLocals,
        HashSet<string> containerLocals,
        HashSet<string> stringLocals)
    {
        var sb = new StringBuilder();
        foreach (StatementSyntax stmt in bodyBlock.Statements)
        {
            int line = stmt.GetLocation().GetLineSpan().StartLinePosition.Line + 1;
            if (!string.IsNullOrEmpty(sourcePath) && line > 0)
            {
                sb.Append($"\n#line {line} \"{sourcePath}\"\n\t");
            }

            // Hoist any inline `out Type varName` declarations for non-if statements
            if (stmt is not IfStatementSyntax)
            {
                foreach (ArgumentSyntax arg in stmt.DescendantNodes().OfType<ArgumentSyntax>())
                {
                    if (arg.RefKindKeyword.IsKind(SyntaxKind.OutKeyword) &&
                        arg.Expression is DeclarationExpressionSyntax declExpr &&
                        declExpr.Designation is SingleVariableDesignationSyntax singleVar)
                    {
                        string rawOutType = NormalizeTypeSyntaxText(declExpr.Type);
                        string mappedOutType = rawOutType == "var"
                            ? MapTypeSymbolToCpp(semanticModel.GetTypeInfo(declExpr.Type).Type, index)
                            : MapTypeName(rawOutType, index, pointerForObjectType: true, wrapObjectInHandle: false);
                        string outVarName = singleVar.Identifier.ValueText;
                        if (IsRawTypeObjectReference(rawOutType, index))
                        {
                            pointerLocals.Add(outVarName);
                        }
                        sb.Append($"{mappedOutType} {outVarName}{{}};\n\t");
                    }
                }
            }

            sb.Append(LowerTokensInNode(
                sourcePath,
                stmt,
                semanticModel,
                index,
                enclosingClassName,
                pointerLocals,
                containerLocals,
                stringLocals,
                emitLineDirectives: true));
        }
        return sb.ToString();
    }

    private static string MapTypeSymbolToCpp(ITypeSymbol? symbol, ProjectTypeIndex index)
    {
        if (symbol == null)
        {
            return "auto";
        }
        return MapTypeName(symbol.Name, index, pointerForObjectType: true, wrapObjectInHandle: false);
    }

    private static string LowerTokensInNode(
        string sourcePath,
        SyntaxNode node,
        SemanticModel semanticModel,
        ProjectTypeIndex index,
        string enclosingClassName,
        HashSet<string> pointerLocals,
        HashSet<string> containerLocals,
        HashSet<string> stringLocals,
        bool emitLineDirectives)
    {
        var spanOverrides = new Dictionary<SyntaxSpanKey, string>();

        // 1. Pre-lower interpolated strings: $"HP={Health}" -> std::format("HP={}", Health)
        foreach (InterpolatedStringExpressionSyntax interp in node.DescendantNodesAndSelf().OfType<InterpolatedStringExpressionSyntax>())
        {
            var fmt = new StringBuilder("\"");
            var args = new List<string>();
            foreach (InterpolatedStringContentSyntax content in interp.Contents)
            {
                if (content is InterpolatedStringTextSyntax textPart)
                {
                    fmt.Append(textPart.TextToken.Text.Replace("{", "{{").Replace("}", "}}"));
                }
                else if (content is InterpolationSyntax hole)
                {
                    fmt.Append("{}");
                    args.Add(LowerTokensInNode(
                        sourcePath,
                        hole.Expression,
                        semanticModel,
                        index,
                        enclosingClassName,
                        pointerLocals,
                        containerLocals,
                        stringLocals,
                        emitLineDirectives: false));
                }
            }
            fmt.Append('"');
            string replacement = args.Count == 0
                ? fmt.ToString()
                : $"std::format({fmt}, {string.Join(", ", args)})";
            spanOverrides[new SyntaxSpanKey(interp.SpanStart, interp.Span.End)] = replacement;
        }

        // 1b. Pre-lower typeof(T) -> ::Shalltear::GetStaticType<MappedT>()
        foreach (TypeOfExpressionSyntax typeofExpr in node.DescendantNodesAndSelf().OfType<TypeOfExpressionSyntax>())
        {
            string mappedTarget = MapTypeName(NormalizeTypeSyntaxText(typeofExpr.Type), index, pointerForObjectType: false, wrapObjectInHandle: false);
            spanOverrides[new SyntaxSpanKey(typeofExpr.SpanStart, typeofExpr.Span.End)] =
                $"::Shalltear::GetStaticType<{mappedTarget}>()";
        }

        // 2. Pre-lower C# switch expressions: `val switch { A => 1, _ => 0 }` -> IIFE lambda
        foreach (SwitchExpressionSyntax switchExpr in node.DescendantNodesAndSelf().OfType<SwitchExpressionSyntax>())
        {
            string gov = LowerTokensInNode(
                sourcePath,
                switchExpr.GoverningExpression,
                semanticModel,
                index,
                enclosingClassName,
                pointerLocals,
                containerLocals,
                stringLocals,
                emitLineDirectives: false);
            var iife = new StringBuilder($"([&]() {{ auto&& _slt_sw = ({gov}); ");
            bool firstArm = true;
            string? discardArmExpr = null;
            foreach (SwitchExpressionArmSyntax arm in switchExpr.Arms)
            {
                string armVal = LowerTokensInNode(
                    sourcePath,
                    arm.Expression,
                    semanticModel,
                    index,
                    enclosingClassName,
                    pointerLocals,
                    containerLocals,
                    stringLocals,
                    emitLineDirectives: false);
                if (arm.Pattern is DiscardPatternSyntax)
                {
                    discardArmExpr = armVal;
                    continue;
                }
                string cond = BuildSwitchArmCondition("_slt_sw", arm.Pattern, sourcePath, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals);
                iife.Append(firstArm ? $"if ({cond}) return {armVal}; " : $"else if ({cond}) return {armVal}; ");
                firstArm = false;
            }
            if (discardArmExpr != null)
            {
                iife.Append($"return {discardArmExpr}; }})()");
            }
            else
            {
                iife.Append("return decltype(_slt_sw){}; })()");
            }
            spanOverrides[new SyntaxSpanKey(switchExpr.SpanStart, switchExpr.Span.End)] = iife.ToString();
        }

        // 3. Pre-lower lambdas: `x => expr` or `(a, b) => expr` -> `[&](auto&& x) { return expr; }`
        foreach (LambdaExpressionSyntax lambda in node.DescendantNodesAndSelf().OfType<LambdaExpressionSyntax>())
        {
            var paramList = new List<string>();
            if (lambda is SimpleLambdaExpressionSyntax simple)
            {
                paramList.Add($"auto&& {simple.Parameter.Identifier.ValueText}");
            }
            else if (lambda is ParenthesizedLambdaExpressionSyntax paren)
            {
                foreach (ParameterSyntax p in paren.ParameterList.Parameters)
                {
                    string pType = p.Type != null
                        ? MapTypeName(NormalizeTypeSyntaxText(p.Type), index, pointerForObjectType: true, wrapObjectInHandle: false)
                        : "auto&&";
                    paramList.Add($"{pType} {p.Identifier.ValueText}");
                }
            }
            string bodyStr;
            if (lambda.ExpressionBody != null)
            {
                string inner = LowerTokensInNode(
                    sourcePath,
                    lambda.ExpressionBody,
                    semanticModel,
                    index,
                    enclosingClassName,
                    pointerLocals,
                    containerLocals,
                    stringLocals,
                    emitLineDirectives: false);
                bodyStr = $"{{ return {inner}; }}";
            }
            else if (lambda.Block != null)
            {
                string inner = LowerTokensInBlock(
                    sourcePath,
                    lambda.Block,
                    semanticModel,
                    index,
                    enclosingClassName,
                    pointerLocals,
                    containerLocals,
                    stringLocals);
                bodyStr = $"{{ {inner} }}";
            }
            else
            {
                bodyStr = "{}";
            }
            spanOverrides[new SyntaxSpanKey(lambda.SpanStart, lambda.Span.End)] =
                $"[&]({string.Join(", ", paramList)}) {bodyStr}";
        }

        // 4. Pre-lower pattern matching: `expr is TypeName varName`, `expr is null`, `expr is not null`
        foreach (IsPatternExpressionSyntax isPat in node.DescendantNodesAndSelf().OfType<IsPatternExpressionSyntax>())
        {
            string exprStr = LowerTokensInNode(
                sourcePath,
                isPat.Expression,
                semanticModel,
                index,
                enclosingClassName,
                pointerLocals,
                containerLocals,
                stringLocals,
                emitLineDirectives: false);
            if (isPat.Pattern is ConstantPatternSyntax constPat && constPat.Expression.IsKind(SyntaxKind.NullLiteralExpression))
            {
                spanOverrides[new SyntaxSpanKey(isPat.SpanStart, isPat.Span.End)] = $"(({exprStr}) == nullptr)";
            }
            else if (isPat.Pattern is UnaryPatternSyntax unaryPat &&
                     unaryPat.Pattern is ConstantPatternSyntax innerConst &&
                     innerConst.Expression.IsKind(SyntaxKind.NullLiteralExpression))
            {
                spanOverrides[new SyntaxSpanKey(isPat.SpanStart, isPat.Span.End)] = $"(({exprStr}) != nullptr)";
            }
            else if (isPat.Pattern is DeclarationPatternSyntax declPat &&
                     declPat.Designation is SingleVariableDesignationSyntax singleVar)
            {
                string targetType = MapTypeName(NormalizeTypeSyntaxText(declPat.Type), index, pointerForObjectType: false, wrapObjectInHandle: false);
                string varName = singleVar.Identifier.ValueText;
                ITypeSymbol? exprType = semanticModel.GetTypeInfo(isPat.Expression).Type;
                bool isHandleReceiver = exprType?.Name == "Handle" ||
                                        (isPat.Expression is IdentifierNameSyntax id &&
                                         !exprStr.EndsWith(".Get()", StringComparison.Ordinal));
                string rawPtrExpr = isHandleReceiver && !exprStr.EndsWith(".Get()", StringComparison.Ordinal) && exprStr.Contains("GetOwner", StringComparison.Ordinal)
                    ? $"({exprStr}).Get()"
                    : exprStr;
                spanOverrides[new SyntaxSpanKey(isPat.SpanStart, isPat.Span.End)] =
                    $"{targetType}* {varName} = dynamic_cast<{targetType}*>({rawPtrExpr})";
            }
        }

        // 5. Pre-lower null-conditional (`a?.b`) and null-coalescing (`a ?? b`)
        foreach (BinaryExpressionSyntax bin in node.DescendantNodesAndSelf().OfType<BinaryExpressionSyntax>())
        {
            if (bin.IsKind(SyntaxKind.CoalesceExpression))
            {
                if (bin.Left is ConditionalAccessExpressionSyntax condLeft)
                {
                    string target = LowerTokensInNode(sourcePath, condLeft.Expression, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, false);
                    string whenNotNull = LowerConditionalBinding(sourcePath, target, condLeft.WhenNotNull, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals);
                    string fallback = LowerTokensInNode(sourcePath, bin.Right, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, false);
                    spanOverrides[new SyntaxSpanKey(bin.SpanStart, bin.Span.End)] =
                        $"(({target}) != nullptr ? ({whenNotNull}) : ({fallback}))";
                }
                else
                {
                    string lhs = LowerTokensInNode(sourcePath, bin.Left, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, false);
                    string rhs = LowerTokensInNode(sourcePath, bin.Right, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, false);
                    spanOverrides[new SyntaxSpanKey(bin.SpanStart, bin.Span.End)] =
                        $"(({lhs}) != nullptr ? ({lhs}) : ({rhs}))";
                }
            }
        }

        foreach (ConditionalAccessExpressionSyntax condAccess in node.DescendantNodesAndSelf().OfType<ConditionalAccessExpressionSyntax>())
        {
            if (condAccess.Parent is BinaryExpressionSyntax parentBin && parentBin.IsKind(SyntaxKind.CoalesceExpression))
            {
                continue;
            }
            string target = LowerTokensInNode(sourcePath, condAccess.Expression, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, false);
            string whenNotNull = LowerConditionalBinding(sourcePath, target, condAccess.WhenNotNull, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals);
            if (condAccess.Parent is ExpressionStatementSyntax)
            {
                spanOverrides[new SyntaxSpanKey(condAccess.Parent.SpanStart, condAccess.Parent.Span.End)] =
                    $"if (({target}) != nullptr) {{ {whenNotNull}; }}\n\t";
            }
            else
            {
                spanOverrides[new SyntaxSpanKey(condAccess.SpanStart, condAccess.Span.End)] =
                    $"(({target}) != nullptr ? ({whenNotNull}) : nullptr)";
            }
        }

        // 6. Pre-lower IfStatement with inline `out Type varName` in condition -> C++17 `if (Type varName{}; cond)`
        if (node is IfStatementSyntax ifStmt)
        {
            var inlineOuts = new List<string>();
            foreach (ArgumentSyntax arg in ifStmt.Condition.DescendantNodes().OfType<ArgumentSyntax>())
            {
                if (arg.RefKindKeyword.IsKind(SyntaxKind.OutKeyword) &&
                    arg.Expression is DeclarationExpressionSyntax declExpr &&
                    declExpr.Designation is SingleVariableDesignationSyntax singleVar)
                {
                    string rawOutType = NormalizeTypeSyntaxText(declExpr.Type);
                    string mappedOutType = rawOutType == "var"
                        ? MapTypeSymbolToCpp(semanticModel.GetTypeInfo(declExpr.Type).Type, index)
                        : MapTypeName(rawOutType, index, pointerForObjectType: true, wrapObjectInHandle: false);
                    string outVarName = singleVar.Identifier.ValueText;
                    if (IsRawTypeObjectReference(rawOutType, index))
                    {
                        pointerLocals.Add(outVarName);
                    }
                    inlineOuts.Add($"{mappedOutType} {outVarName}{{}}");
                    spanOverrides[new SyntaxSpanKey(arg.SpanStart, arg.Span.End)] = outVarName;
                }
            }
            if (inlineOuts.Count > 0)
            {
                string condStr = LowerTokensInNodeWithOverrides(
                    sourcePath,
                    ifStmt.Condition,
                    semanticModel,
                    index,
                    enclosingClassName,
                    pointerLocals,
                    containerLocals,
                    stringLocals,
                    spanOverrides,
                    emitLineDirectives: false);
                spanOverrides[new SyntaxSpanKey(ifStmt.Condition.SpanStart, ifStmt.Condition.Span.End)] =
                    $"{string.Join("; ", inlineOuts)}; {condStr}";
            }
        }

        // 7. Pre-lower inline `out Type varName` arguments in non-if expressions to just `varName` (declaration already hoisted in Block)
        foreach (ArgumentSyntax arg in node.DescendantNodesAndSelf().OfType<ArgumentSyntax>())
        {
            if (arg.RefKindKeyword.IsKind(SyntaxKind.OutKeyword) &&
                arg.Expression is DeclarationExpressionSyntax declExpr &&
                declExpr.Designation is SingleVariableDesignationSyntax singleVar)
            {
                spanOverrides[new SyntaxSpanKey(arg.SpanStart, arg.Span.End)] = singleVar.Identifier.ValueText;
            }
            else if (arg.RefKindKeyword.IsKind(SyntaxKind.OutKeyword) || arg.RefKindKeyword.IsKind(SyntaxKind.RefKeyword))
            {
                string innerArg = LowerTokensInNode(sourcePath, arg.Expression, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, false);
                spanOverrides[new SyntaxSpanKey(arg.SpanStart, arg.Span.End)] = innerArg;
            }
        }

        // 8. Pre-lower `list.RemoveAll(pred)` -> `std::erase_if(list, pred)` and `list.Exists(pred)` -> `std::any_of(...)`
        foreach (InvocationExpressionSyntax inv in node.DescendantNodesAndSelf().OfType<InvocationExpressionSyntax>())
        {
            if (inv.Expression is MemberAccessExpressionSyntax ma && inv.ArgumentList.Arguments.Count == 1)
            {
                string methodName = ma.Name.Identifier.ValueText;
                ITypeSymbol? recvType = semanticModel.GetTypeInfo(ma.Expression).Type;
                bool isContainer = (ma.Expression is IdentifierNameSyntax id && containerLocals.Contains(id.Identifier.ValueText)) ||
                                   recvType?.Name is "List" or "Vector";
                if (isContainer && methodName == "RemoveAll")
                {
                    string recv = LowerTokensInNodeWithOverrides(sourcePath, ma.Expression, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, spanOverrides, false);
                    string pred = LowerTokensInNodeWithOverrides(sourcePath, inv.ArgumentList.Arguments[0].Expression, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, spanOverrides, false);
                    spanOverrides[new SyntaxSpanKey(inv.SpanStart, inv.Span.End)] = $"std::erase_if({recv}, {pred})";
                }
                else if (isContainer && methodName == "Exists")
                {
                    string recv = LowerTokensInNodeWithOverrides(sourcePath, ma.Expression, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, spanOverrides, false);
                    string pred = LowerTokensInNodeWithOverrides(sourcePath, inv.ArgumentList.Arguments[0].Expression, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, spanOverrides, false);
                    spanOverrides[new SyntaxSpanKey(inv.SpanStart, inv.Span.End)] = $"std::any_of(({recv}).begin(), ({recv}).end(), {pred})";
                }
                else if (isContainer && methodName == "RemoveAt")
                {
                    string recv = LowerTokensInNodeWithOverrides(sourcePath, ma.Expression, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, spanOverrides, false);
                    string idxArg = LowerTokensInNodeWithOverrides(sourcePath, inv.ArgumentList.Arguments[0].Expression, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, spanOverrides, false);
                    spanOverrides[new SyntaxSpanKey(inv.SpanStart, inv.Span.End)] = $"({recv}).erase(({recv}).begin() + ({idxArg}))";
                }
            }
        }

        // 9. Pre-lower computed property assignments (`Prop = val` or `obj.Prop = val`)
        foreach (AssignmentExpressionSyntax assign in node.DescendantNodesAndSelf().OfType<AssignmentExpressionSyntax>())
        {
            if (!assign.IsKind(SyntaxKind.SimpleAssignmentExpression))
            {
                continue;
            }
            if (assign.Left is IdentifierNameSyntax idLeft &&
                index.TypeToComputedProperties.TryGetValue(enclosingClassName, out var ownProps) &&
                ownProps.Contains(idLeft.Identifier.ValueText))
            {
                string rhs = LowerTokensInNodeWithOverrides(sourcePath, assign.Right, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, spanOverrides, false);
                spanOverrides[new SyntaxSpanKey(assign.SpanStart, assign.Span.End)] = $"Set{idLeft.Identifier.ValueText}({rhs})";
            }
            else if (assign.Left is MemberAccessExpressionSyntax maLeft)
            {
                ITypeSymbol? recvType = semanticModel.GetTypeInfo(maLeft.Expression).Type;
                string propName = maLeft.Name.Identifier.ValueText;
                if (recvType != null &&
                    index.TypeToComputedProperties.TryGetValue(recvType.Name, out var targetProps) &&
                    targetProps.Contains(propName))
                {
                    string recv = LowerTokensInNodeWithOverrides(sourcePath, maLeft.Expression, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, spanOverrides, false);
                    string rhs = LowerTokensInNodeWithOverrides(sourcePath, assign.Right, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, spanOverrides, false);
                    string op = IsSymbolObjectPointer(recvType, index) ? "->" : ".";
                    spanOverrides[new SyntaxSpanKey(assign.SpanStart, assign.Span.End)] = $"{recv}{op}Set{propName}({rhs})";
                }
            }
        }

        return LowerTokensInNodeWithOverrides(
            sourcePath,
            node,
            semanticModel,
            index,
            enclosingClassName,
            pointerLocals,
            containerLocals,
            stringLocals,
            spanOverrides,
            emitLineDirectives);
    }

    private static string BuildSwitchArmCondition(
        string swVar,
        PatternSyntax pattern,
        string sourcePath,
        SemanticModel semanticModel,
        ProjectTypeIndex index,
        string enclosingClassName,
        HashSet<string> pointerLocals,
        HashSet<string> containerLocals,
        HashSet<string> stringLocals)
    {
        if (pattern is ConstantPatternSyntax constPat)
        {
            string val = LowerTokensInNode(sourcePath, constPat.Expression, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, false);
            return $"{swVar} == {val}";
        }
        if (pattern is RelationalPatternSyntax relPat)
        {
            string val = LowerTokensInNode(sourcePath, relPat.Expression, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, false);
            return $"{swVar} {relPat.OperatorToken.Text} {val}";
        }
        if (pattern is BinaryPatternSyntax binPat)
        {
            string left = BuildSwitchArmCondition(swVar, binPat.Left, sourcePath, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals);
            string right = BuildSwitchArmCondition(swVar, binPat.Right, sourcePath, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals);
            string op = binPat.IsKind(SyntaxKind.OrPattern) ? "||" : "&&";
            return $"({left}) {op} ({right})";
        }
        return "true";
    }

    private static string LowerConditionalBinding(
        string sourcePath,
        string loweredTarget,
        ExpressionSyntax whenNotNull,
        SemanticModel semanticModel,
        ProjectTypeIndex index,
        string enclosingClassName,
        HashSet<string> pointerLocals,
        HashSet<string> containerLocals,
        HashSet<string> stringLocals)
    {
        string raw = LowerTokensInNode(sourcePath, whenNotNull, semanticModel, index, enclosingClassName, pointerLocals, containerLocals, stringLocals, false).TrimStart('.');
        return $"({loweredTarget})->{raw}";
    }

    private static string LowerTokensInNodeWithOverrides(
        string sourcePath,
        SyntaxNode node,
        SemanticModel semanticModel,
        ProjectTypeIndex index,
        string enclosingClassName,
        HashSet<string> pointerLocals,
        HashSet<string> containerLocals,
        HashSet<string> stringLocals,
        Dictionary<SyntaxSpanKey, string> spanOverrides,
        bool emitLineDirectives)
    {
        if (spanOverrides.TryGetValue(new SyntaxSpanKey(node.SpanStart, node.Span.End), out string? directOverride))
        {
            return directOverride;
        }

        List<SyntaxToken> toks = node.DescendantTokens().ToList();
        int n = toks.Count;
        var outSb = new StringBuilder();
        int lastEmittedLine = node.GetLocation().GetLineSpan().StartLinePosition.Line + 1;

        int i = 0;
        while (i < n)
        {
            SyntaxToken tok = toks[i];

            // Check if token starts a pre-lowered AST span override
            SyntaxSpanKey matchedKey = default;
            int bestLen = -1;
            foreach (SyntaxSpanKey k in spanOverrides.Keys)
            {
                if (k.Start == tok.SpanStart && k.End <= node.Span.End && (k.End - k.Start) > bestLen)
                {
                    matchedKey = k;
                    bestLen = k.End - k.Start;
                }
            }
            if (bestLen >= 0)
            {
                outSb.Append(spanOverrides[matchedKey]);
                while (i < n && toks[i].Span.End <= matchedKey.End)
                {
                    ++i;
                }
                continue;
            }

            int tokLine = tok.GetLocation().GetLineSpan().StartLinePosition.Line + 1;
            if (emitLineDirectives && tokLine != lastEmittedLine &&
                (i == 0 || toks[i - 1].IsKind(SyntaxKind.SemicolonToken) ||
                 toks[i - 1].IsKind(SyntaxKind.OpenBraceToken) ||
                 toks[i - 1].IsKind(SyntaxKind.CloseBraceToken)))
            {
                if (!string.IsNullOrEmpty(sourcePath) && tokLine > 0)
                {
                    outSb.Append($"\n#line {tokLine} \"{sourcePath}\"\n\t");
                    lastEmittedLine = tokLine;
                }
            }

            // foreach (Type name in collection) -> for (MappedType& name : collection)
            if (tok.IsKind(SyntaxKind.ForEachKeyword) && i + 1 < n && toks[i + 1].IsKind(SyntaxKind.OpenParenToken))
            {
                int closeIdx = i + 2;
                int inIdx = n;
                int parenDepth = 1;
                while (closeIdx < n && parenDepth > 0)
                {
                    if (toks[closeIdx].IsKind(SyntaxKind.OpenParenToken))
                    {
                        ++parenDepth;
                    }
                    else if (toks[closeIdx].IsKind(SyntaxKind.CloseParenToken))
                    {
                        --parenDepth;
                        if (parenDepth == 0)
                        {
                            break;
                        }
                    }
                    else if (parenDepth == 1 && toks[closeIdx].IsKind(SyntaxKind.InKeyword))
                    {
                        inIdx = closeIdx;
                    }
                    ++closeIdx;
                }
                if (inIdx < closeIdx && inIdx >= i + 4)
                {
                    string varName = toks[inIdx - 1].ValueText;
                    string rawTypeStr;
                    bool isContainerVar;
                    if (tok.Parent is ForEachStatementSyntax feNode)
                    {
                        rawTypeStr = NormalizeTypeSyntaxText(feNode.Type);
                        isContainerVar = IsContainerTypeSyntax(feNode.Type, semanticModel, out _);
                    }
                    else
                    {
                        var rawVarType = new StringBuilder();
                        for (int k = i + 2; k + 1 < inIdx; ++k)
                        {
                            rawVarType.Append(toks[k].ValueText);
                        }
                        rawTypeStr = rawVarType.ToString();
                        isContainerVar = IsContainerTypeSpelling(rawTypeStr, out _);
                    }
                    string mappedVarType;
                    if (rawTypeStr == "var")
                    {
                        mappedVarType = "auto&&";
                    }
                    else
                    {
                        mappedVarType = MapTypeName(rawTypeStr, index, pointerForObjectType: true, wrapObjectInHandle: false);
                        if (isContainerVar)
                        {
                            mappedVarType += "&";
                            containerLocals.Add(varName);
                        }
                    }
                    if (mappedVarType.EndsWith("*", StringComparison.Ordinal) || IsRawTypeObjectReference(rawTypeStr, index))
                    {
                        pointerLocals.Add(varName);
                    }
                    outSb.Append($"for ({mappedVarType} {varName} : ");
                    i = inIdx + 1;
                    continue;
                }
            }

            // Log.Info / Log.Warn / Log.Error / Log.Trace(...)
            if (tok.ValueText == "Log" && i + 3 < n &&
                toks[i + 1].IsKind(SyntaxKind.DotToken) &&
                toks[i + 3].IsKind(SyntaxKind.OpenParenToken))
            {
                string level = toks[i + 2].ValueText;
                if (level == "Info")
                {
                    outSb.Append("SLT_LOG_INFO");
                    i += 3;
                    continue;
                }
                if (level is "Warn" or "Warning")
                {
                    outSb.Append("SLT_LOG_WARN");
                    i += 3;
                    continue;
                }
                if (level == "Error")
                {
                    outSb.Append("SLT_LOG_ERROR");
                    i += 3;
                    continue;
                }
                if (level == "Trace")
                {
                    outSb.Append("SLT_LOG_TRACE");
                    i += 3;
                    continue;
                }
            }

            // Time.DeltaTime / Time.UnscaledDeltaTime / Time.FixedDeltaTime / Time.TotalTime / Time.FrameCount
            if (tok.ValueText == "Time" && i + 2 < n && toks[i + 1].IsKind(SyntaxKind.DotToken))
            {
                string prop = toks[i + 2].ValueText;
                bool hasParens = i + 3 < n && toks[i + 3].IsKind(SyntaxKind.OpenParenToken);
                if (!hasParens)
                {
                    if (prop is "DeltaTime" or "GetDeltaTime")
                    {
                        outSb.Append("::Shalltear::Time::GetDeltaTime()");
                        i += 3;
                        continue;
                    }
                    if (prop is "UnscaledDeltaTime" or "GetUnscaledDeltaTime")
                    {
                        outSb.Append("::Shalltear::Time::GetUnscaledDeltaTime()");
                        i += 3;
                        continue;
                    }
                    if (prop is "FixedDeltaTime" or "GetFixedDeltaTime")
                    {
                        outSb.Append("::Shalltear::Time::GetFixedDeltaTime()");
                        i += 3;
                        continue;
                    }
                    if (prop is "TotalTime" or "GetTime")
                    {
                        outSb.Append("::Shalltear::Time::GetTime()");
                        i += 3;
                        continue;
                    }
                    if (prop is "FrameCount" or "GetFrameCount")
                    {
                        outSb.Append("::Shalltear::Time::GetFrameCount()");
                        i += 3;
                        continue;
                    }
                }
            }

            // string.IsNullOrEmpty(expr) -> (expr).empty()
            if (tok.ValueText is "string" or "String" &&
                i + 4 < n && toks[i + 1].IsKind(SyntaxKind.DotToken) &&
                toks[i + 2].ValueText == "IsNullOrEmpty" &&
                toks[i + 3].IsKind(SyntaxKind.OpenParenToken))
            {
                int closeParen = i + 4;
                int depth = 1;
                var inner = new StringBuilder();
                while (closeParen < n && depth > 0)
                {
                    if (toks[closeParen].IsKind(SyntaxKind.OpenParenToken))
                    {
                        ++depth;
                    }
                    else if (toks[closeParen].IsKind(SyntaxKind.CloseParenToken))
                    {
                        --depth;
                        if (depth == 0)
                        {
                            break;
                        }
                    }
                    inner.Append(toks[closeParen].Text);
                    ++closeParen;
                }
                if (closeParen < n)
                {
                    outSb.Append($"({inner}).empty()");
                    i = closeParen + 1;
                    continue;
                }
            }

            // new ValueType(...) -> ValueType(...)
            if (tok.IsKind(SyntaxKind.NewKeyword) && i + 1 < n && toks[i + 1].IsKind(SyntaxKind.IdentifierToken))
            {
                ++i;
                continue;
            }

            // GetComponent<T>() -> (GetOwner().Get() ? GetOwner().Get()->GetComponent<MappedT>().Get() : nullptr)
            if (tok.ValueText == "GetComponent" &&
                i + 5 < n && toks[i + 1].IsKind(SyntaxKind.LessThanToken) &&
                toks[i + 2].IsKind(SyntaxKind.IdentifierToken) &&
                toks[i + 3].IsKind(SyntaxKind.GreaterThanToken) &&
                toks[i + 4].IsKind(SyntaxKind.OpenParenToken) &&
                toks[i + 5].IsKind(SyntaxKind.CloseParenToken))
            {
                string mappedArg = MapTypeName(toks[i + 2].ValueText, index, pointerForObjectType: false, wrapObjectInHandle: false);
                bool afterDot = i > 0 && toks[i - 1].IsKind(SyntaxKind.DotToken);
                if (afterDot)
                {
                    outSb.Append($"GetComponent<{mappedArg}>().Get()");
                }
                else
                {
                    outSb.Append($"(GetOwner().Get() ? GetOwner().Get()->GetComponent<{mappedArg}>().Get() : nullptr)");
                }
                i += 6;
                continue;
            }

            // Local variable declaration at statement boundary
            if ((tok.IsKind(SyntaxKind.IdentifierToken) || PrimitiveMap.ContainsKey(tok.ValueText)) &&
                (i == 0 || toks[i - 1].IsKind(SyntaxKind.SemicolonToken) ||
                 toks[i - 1].IsKind(SyntaxKind.OpenBraceToken) ||
                 toks[i - 1].IsKind(SyntaxKind.CloseBraceToken) ||
                 toks[i - 1].IsKind(SyntaxKind.ConstKeyword)))
            {
                int cursor = i;
                var candidateType = new StringBuilder(toks[cursor].ValueText);
                ++cursor;
                if (cursor < n && toks[cursor].IsKind(SyntaxKind.LessThanToken))
                {
                    candidateType.Append('<');
                    int depth = 1;
                    ++cursor;
                    while (cursor < n && depth > 0)
                    {
                        if (toks[cursor].IsKind(SyntaxKind.LessThanToken))
                        {
                            ++depth;
                        }
                        else if (toks[cursor].IsKind(SyntaxKind.GreaterThanToken))
                        {
                            --depth;
                        }
                        candidateType.Append(toks[cursor].ValueText);
                        ++cursor;
                    }
                }
                if (cursor < n && toks[cursor].IsKind(SyntaxKind.AsteriskToken))
                {
                    candidateType.Append('*');
                    ++cursor;
                }
                bool isKnownType = PrimitiveMap.ContainsKey(tok.ValueText) ||
                                   EngineTypeNames.Contains(tok.ValueText) ||
                                   index.UserObjectTypes.Contains(tok.ValueText) ||
                                   index.UserValueStructs.Contains(tok.ValueText) ||
                                   index.UserEnumNames.Contains(tok.ValueText) ||
                                   tok.ValueText == "List";
                if (isKnownType && cursor < n && toks[cursor].IsKind(SyntaxKind.IdentifierToken) &&
                    cursor + 1 < n &&
                    (toks[cursor + 1].IsKind(SyntaxKind.EqualsToken) || toks[cursor + 1].IsKind(SyntaxKind.SemicolonToken)))
                {
                    string localName = toks[cursor].ValueText;
                    string candidateStr = candidateType.ToString();
                    string mapped = MapTypeName(candidateStr, index, pointerForObjectType: true, wrapObjectInHandle: false);
                    if (mapped.EndsWith("*", StringComparison.Ordinal) || IsRawTypeObjectReference(candidateStr, index))
                    {
                        pointerLocals.Add(localName);
                    }
                    outSb.Append($"{mapped} {localName} ");
                    i = cursor + 1;
                    continue;
                }
            }

            // SemanticModel-aware dot operator lowering for arbitrary receiver expressions (e.g., `GetTarget().SetActive(true)`)
            if (tok.IsKind(SyntaxKind.DotToken) &&
                tok.Parent is MemberAccessExpressionSyntax dotParent &&
                dotParent.OperatorToken == tok)
            {
                string memberName = dotParent.Name.Identifier.ValueText;
                SymbolInfo exprSymbolInfo = semanticModel.GetSymbolInfo(dotParent.Expression);
                if (exprSymbolInfo.Symbol is ITypeSymbol staticTypeSym)
                {
                    outSb.Append("::");
                    ++i;
                    continue;
                }

                ITypeSymbol? recvType = semanticModel.GetTypeInfo(dotParent.Expression).Type;
                if (IsSymbolObjectPointer(recvType, index) && !HandleIntrinsicMembers.Contains(memberName))
                {
                    if (recvType != null &&
                        index.TypeToComputedProperties.TryGetValue(recvType.Name, out var computedSet) &&
                        computedSet.Contains(memberName))
                    {
                        outSb.Append($"->Get{memberName}()");
                        i += 2;
                        continue;
                    }
                    outSb.Append("->");
                    ++i;
                    continue;
                }
            }

            // Static scope / enum / pointer local / container local access: X.Y
            if ((tok.IsKind(SyntaxKind.IdentifierToken) || tok.IsKind(SyntaxKind.ThisKeyword) || tok.IsKind(SyntaxKind.BaseKeyword)) &&
                i + 2 < n && toks[i + 1].IsKind(SyntaxKind.DotToken) && toks[i + 2].IsKind(SyntaxKind.IdentifierToken))
            {
                string lhs = tok.ValueText;
                string rhs = toks[i + 2].ValueText;

                if (StaticScopeIdentifiers.Contains(lhs))
                {
                    outSb.Append($"::Shalltear::{lhs}::{rhs}");
                    i += 3;
                    continue;
                }
                if (index.UserEnumNames.Contains(lhs))
                {
                    outSb.Append($"{lhs}::{rhs}");
                    i += 3;
                    continue;
                }
                if (lhs == "base")
                {
                    outSb.Append($"Super::{rhs}");
                    i += 3;
                    continue;
                }
                if (lhs == "this")
                {
                    if (index.TypeToComputedProperties.TryGetValue(enclosingClassName, out var ownProps) && ownProps.Contains(rhs))
                    {
                        outSb.Append($"this->Get{rhs}()");
                    }
                    else
                    {
                        outSb.Append($"this->{rhs}");
                    }
                    i += 3;
                    continue;
                }
                if (pointerLocals.Contains(lhs) && !HandleIntrinsicMembers.Contains(rhs))
                {
                    ITypeSymbol? lhsType = tok.Parent != null ? semanticModel.GetTypeInfo(tok.Parent).Type : null;
                    if (lhsType != null &&
                        index.TypeToComputedProperties.TryGetValue(lhsType.Name, out var targetProps) &&
                        targetProps.Contains(rhs))
                    {
                        outSb.Append($"{lhs}->Get{rhs}()");
                        i += 3;
                        continue;
                    }
                    outSb.Append($"{lhs}->");
                    i += 2;
                    continue;
                }
                if (containerLocals.Contains(lhs))
                {
                    if (rhs == "Add")
                    {
                        outSb.Append($"{lhs}.push_back");
                        i += 3;
                        continue;
                    }
                    if (rhs == "Clear")
                    {
                        outSb.Append($"{lhs}.clear");
                        i += 3;
                        continue;
                    }
                    if (rhs == "Count")
                    {
                        outSb.Append($"static_cast<::Shalltear::i32>({lhs}.size())");
                        i += 3;
                        continue;
                    }
                }
                if (stringLocals.Contains(lhs) && rhs == "Length")
                {
                    outSb.Append($"static_cast<::Shalltear::i32>({lhs}.size())");
                    i += 3;
                    continue;
                }
            }

            // Chained .Get().Member -> .Get()->Member
            if (tok.IsKind(SyntaxKind.DotToken) && i >= 3 &&
                toks[i - 1].IsKind(SyntaxKind.CloseParenToken) &&
                toks[i - 2].IsKind(SyntaxKind.OpenParenToken) &&
                toks[i - 3].ValueText is "Get" or "Resolve" or "LoadAssetByGuid" or "FindLoadedByGuid")
            {
                outSb.Append("->");
                ++i;
                continue;
            }

            if (tok.IsKind(SyntaxKind.NullKeyword) || tok.ValueText == "null")
            {
                outSb.Append("nullptr");
                ++i;
                continue;
            }

            if (tok.IsKind(SyntaxKind.IdentifierToken) || PrimitiveMap.ContainsKey(tok.ValueText))
            {
                // Unqualified computed property read on `this`
                if (index.TypeToComputedProperties.TryGetValue(enclosingClassName, out var ownComputed) &&
                    ownComputed.Contains(tok.ValueText) &&
                    (i == 0 || !toks[i - 1].IsKind(SyntaxKind.DotToken)))
                {
                    outSb.Append($"Get{tok.ValueText}()");
                }
                else if (PrimitiveMap.TryGetValue(tok.ValueText, out string? primMapped))
                {
                    outSb.Append(primMapped);
                }
                else if (EngineTypeNames.Contains(tok.ValueText) &&
                         (i == 0 || !toks[i - 1].IsKind(SyntaxKind.DotToken)))
                {
                    outSb.Append($"::Shalltear::{tok.ValueText}");
                }
                else
                {
                    outSb.Append(tok.ValueText);
                }

                if (i + 1 < n && (toks[i + 1].IsKind(SyntaxKind.IdentifierToken) ||
                                  toks[i + 1].IsKind(SyntaxKind.NumericLiteralToken) ||
                                  toks[i + 1].IsKind(SyntaxKind.StringLiteralToken) ||
                                  toks[i + 1].IsKind(SyntaxKind.InKeyword)))
                {
                    outSb.Append(' ');
                }
                ++i;
                continue;
            }

            if (tok.IsKind(SyntaxKind.ReturnKeyword) ||
                tok.IsKind(SyntaxKind.IfKeyword) ||
                tok.IsKind(SyntaxKind.ElseKeyword) ||
                tok.IsKind(SyntaxKind.ForKeyword) ||
                tok.IsKind(SyntaxKind.WhileKeyword) ||
                tok.IsKind(SyntaxKind.SwitchKeyword) ||
                tok.IsKind(SyntaxKind.CaseKeyword) ||
                tok.IsKind(SyntaxKind.ConstKeyword))
            {
                outSb.Append(tok.Text);
                outSb.Append(' ');
                ++i;
                continue;
            }

            if (tok.IsKind(SyntaxKind.SemicolonToken))
            {
                outSb.Append(";\n\t");
                ++i;
                continue;
            }
            if (tok.IsKind(SyntaxKind.OpenBraceToken))
            {
                outSb.Append(" {\n\t");
                ++i;
                continue;
            }
            if (tok.IsKind(SyntaxKind.CloseBraceToken))
            {
                outSb.Append("}\n\t");
                ++i;
                continue;
            }
            if (tok.IsKind(SyntaxKind.CommaToken))
            {
                outSb.Append(", ");
                ++i;
                continue;
            }

            if (IsBinarySpacingOperator(tok.Kind()))
            {
                outSb.Append($" {tok.Text} ");
                ++i;
                continue;
            }

            outSb.Append(tok.Text);
            ++i;
        }

        return outSb.ToString();
    }

    private readonly record struct SyntaxSpanKey(int Start, int End);

    private static bool IsBinarySpacingOperator(SyntaxKind kind)
    {
        return kind is SyntaxKind.EqualsToken
            or SyntaxKind.PlusEqualsToken
            or SyntaxKind.MinusEqualsToken
            or SyntaxKind.AsteriskEqualsToken
            or SyntaxKind.SlashEqualsToken
            or SyntaxKind.EqualsEqualsToken
            or SyntaxKind.ExclamationEqualsToken
            or SyntaxKind.LessThanEqualsToken
            or SyntaxKind.GreaterThanEqualsToken
            or SyntaxKind.AmpersandAmpersandToken
            or SyntaxKind.BarBarToken
            or SyntaxKind.PlusToken
            or SyntaxKind.MinusToken
            or SyntaxKind.AsteriskToken
            or SyntaxKind.SlashToken
            or SyntaxKind.PercentToken
            or SyntaxKind.QuestionToken
            or SyntaxKind.ColonToken;
    }

    private static string GetEnclosingNamespace(SyntaxNode node)
    {
        var parts = new List<string>();
        for (SyntaxNode? cur = node.Parent; cur != null; cur = cur.Parent)
        {
            if (cur is BaseNamespaceDeclarationSyntax ns)
            {
                parts.Add(ns.Name.ToString());
            }
        }
        parts.Reverse();
        return string.Join(".", parts);
    }

    private static string NormalizeTypeSyntaxText(TypeSyntax typeSyntax)
    {
        string text = typeSyntax.ToString().Replace(" ", "").Replace("global::", "");
        if (typeSyntax.SyntaxTree?.GetRoot() is SyntaxNode root)
        {
            foreach (UsingDirectiveSyntax u in root.DescendantNodes().OfType<UsingDirectiveSyntax>())
            {
                if (u.Alias != null && u.Name != null)
                {
                    string aliasName = u.Alias.Name.Identifier.ValueText;
                    if (string.Equals(text, aliasName, StringComparison.Ordinal))
                    {
                        text = u.Name.ToString().Replace(" ", "").Replace("global::", "");
                        break;
                    }
                }
            }
        }
        return text;
    }

    private static bool HasAttribute(SyntaxList<AttributeListSyntax> lists, string shortName)
    {
        return FindAttribute(lists, shortName) != null;
    }

    private static AttributeSyntax? FindAttribute(SyntaxList<AttributeListSyntax> lists, string shortName)
    {
        foreach (AttributeListSyntax list in lists)
        {
            foreach (AttributeSyntax attr in list.Attributes)
            {
                string raw = attr.Name.ToString();
                int lastDot = raw.LastIndexOf('.');
                if (lastDot >= 0)
                {
                    raw = raw.Substring(lastDot + 1);
                }
                if (raw.EndsWith("Attribute", StringComparison.Ordinal) && raw.Length > 9)
                {
                    raw = raw.Substring(0, raw.Length - 9);
                }
                if (string.Equals(raw, shortName, StringComparison.Ordinal))
                {
                    return attr;
                }
            }
        }
        return null;
    }

    private static List<string>? FindAttributeArguments(SyntaxList<AttributeListSyntax> lists, string shortName)
    {
        AttributeSyntax? attr = FindAttribute(lists, shortName);
        if (attr?.ArgumentList == null)
        {
            return null;
        }
        return attr.ArgumentList.Arguments.Select(a => a.Expression.ToString().Trim()).ToList();
    }

    private static string BuildFieldAttributesMacroList(SyntaxList<AttributeListSyntax> lists)
    {
        var parts = new List<string>();
        foreach (AttributeListSyntax list in lists)
        {
            foreach (AttributeSyntax attr in list.Attributes)
            {
                string name = attr.Name.ToString();
                int lastDot = name.LastIndexOf('.');
                if (lastDot >= 0)
                {
                    name = name.Substring(lastDot + 1);
                }
                if (name.EndsWith("Attribute", StringComparison.Ordinal) && name.Length > 9)
                {
                    name = name.Substring(0, name.Length - 9);
                }
                List<string> args = attr.ArgumentList?.Arguments.Select(a => a.Expression.ToString().Trim()).ToList() ?? new List<string>();
                if (name == "Range" && args.Count == 2)
                {
                    parts.Add($"SLT_RANGE({args[0]}, {args[1]})");
                }
                else if (name == "Tooltip" && args.Count >= 1)
                {
                    parts.Add($"SLT_TOOLTIP({args[0]})");
                }
                else if (name == "Header" && args.Count >= 1)
                {
                    parts.Add($"SLT_HEADER({args[0]})");
                }
                else if (name == "Hidden")
                {
                    parts.Add("SLT_HIDDEN");
                }
                else if (name == "ReadOnly")
                {
                    parts.Add("SLT_READONLY");
                }
                else if (name == "Transient")
                {
                    parts.Add("SLT_TRANSIENT");
                }
                else if (name == "AssetRefs")
                {
                    parts.Add("SLT_ASSET_REFS");
                }
            }
        }
        return parts.Count == 0 ? "" : ", " + string.Join(", ", parts);
    }

    private static bool IsContainerTypeSpelling(string rawType, out string elementType)
    {
        string trimmed = rawType.Trim();
        if (trimmed.StartsWith("global::", StringComparison.Ordinal))
        {
            trimmed = trimmed.Substring(8).Trim();
        }
        foreach (string prefix in new[] { "List<", "Vector<", "Shalltear.Vector<", "System.Collections.Generic.List<" })
        {
            if (trimmed.StartsWith(prefix, StringComparison.Ordinal) && trimmed.EndsWith(">", StringComparison.Ordinal))
            {
                elementType = trimmed.Substring(prefix.Length, trimmed.Length - prefix.Length - 1).Trim();
                return !string.IsNullOrEmpty(elementType);
            }
        }
        elementType = "";
        return false;
    }

    private static string MapTypeName(
        string rawType,
        ProjectTypeIndex index,
        bool pointerForObjectType,
        bool wrapObjectInHandle)
    {
        string trimmed = rawType.Trim();
        if (string.IsNullOrEmpty(trimmed))
        {
            return "";
        }
        if (trimmed.StartsWith("global::", StringComparison.Ordinal))
        {
            trimmed = trimmed.Substring(8).Trim();
        }
        if (trimmed.StartsWith("Shalltear.", StringComparison.Ordinal))
        {
            trimmed = trimmed.Substring(10);
        }

        bool explicitPointer = false;
        if (trimmed.EndsWith("*", StringComparison.Ordinal))
        {
            explicitPointer = true;
            trimmed = trimmed.Substring(0, trimmed.Length - 1).Trim();
        }

        if (IsContainerTypeSpelling(trimmed, out string elem))
        {
            return $"::Shalltear::Vector<{MapTypeName(elem, index, pointerForObjectType: false, wrapObjectInHandle: true)}>";
        }

        if (trimmed.StartsWith("Handle<", StringComparison.Ordinal) && trimmed.EndsWith(">", StringComparison.Ordinal))
        {
            string inner = trimmed.Substring(7, trimmed.Length - 8).Trim();
            return $"::Shalltear::Handle<{MapTypeName(inner, index, pointerForObjectType: false, wrapObjectInHandle: false)}>";
        }

        if (PrimitiveMap.TryGetValue(trimmed, out string? prim))
        {
            return prim + (explicitPointer ? "*" : "");
        }

        bool isObjectType = ObjectDerivedEngineTypes.Contains(trimmed) || index.UserObjectTypes.Contains(trimmed);
        string mapped = EngineTypeNames.Contains(trimmed)
            ? $"::Shalltear::{trimmed}"
            : trimmed.Replace(".", "::");

        if (explicitPointer || (pointerForObjectType && isObjectType))
        {
            mapped += "*";
        }
        else if (wrapObjectInHandle && isObjectType)
        {
            mapped = $"::Shalltear::Handle<{mapped}>";
        }
        return mapped;
    }

    private static string TranspileInitializer(string rawInit, ProjectTypeIndex index)
    {
        string text = rawInit.Trim();
        if (string.IsNullOrEmpty(text))
        {
            return "";
        }
        if (text == "null")
        {
            return "nullptr";
        }
        if (text.StartsWith("new ", StringComparison.Ordinal))
        {
            string afterNew = text.Substring(4).Trim();
            if (afterNew.StartsWith("List<", StringComparison.Ordinal) ||
                afterNew.StartsWith("Vector<", StringComparison.Ordinal))
            {
                return "";
            }
            return "::Shalltear::" + afterNew;
        }
        int dot = text.IndexOf('.');
        if (dot > 0)
        {
            string head = text.Substring(0, dot).Trim();
            string tail = text.Substring(dot + 1).Trim();
            if (StaticScopeIdentifiers.Contains(head))
            {
                return $"::Shalltear::{head}::{tail}";
            }
            if (index.UserEnumNames.Contains(head))
            {
                return $"{head}::{tail}";
            }
        }
        return text;
    }

    private static string NormalizeSlashes(string path)
    {
        return path.Replace('\\', '/');
    }
}
