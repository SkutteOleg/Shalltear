using System;
using System.Collections.Generic;

namespace Fixture.Unsafe;

public static class Finder {
	public static object? MakeListOf(string typeName) {
		Type? type = Type.GetType(typeName);
		return type == null ? null : Activator.CreateInstance(typeof(List<>).MakeGenericType(type));
	}
}
