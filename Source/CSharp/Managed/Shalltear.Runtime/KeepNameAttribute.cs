using System;

namespace Shalltear;

/// <summary>Type/member names shipped as written in Release players (other script names renamed).</summary>
[AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct | AttributeTargets.Enum | AttributeTargets.Interface | AttributeTargets.Delegate,
				Inherited = false)]
public sealed class KeepNameAttribute : Attribute {}
