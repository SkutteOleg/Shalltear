using System;

namespace Shalltear.Editor;

/// <summary>Marks a static method called after scripts load in the editor (again after each reload).</summary>
[AttributeUsage(AttributeTargets.Method)]
public sealed class EditorLoadAttribute : Attribute {}
