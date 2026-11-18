using System;

namespace Shalltear;

/// <summary>Thrown when an engine call fails.</summary>
public sealed class EngineException : Exception {
	internal EngineException(string message) : base(message) {}
}
