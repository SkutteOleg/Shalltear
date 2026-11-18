using System.Collections.Generic;
using System.Linq;

using Shalltear.Interop;

namespace Shalltear.Conformance;

static class ReloadPatternTests {
	sealed class Item {
		public int id;
		public string name = "";
	}

	interface IVisitor {
		T Visit<T>(T value);
	}

	sealed class Echo : IVisitor {
		public T Visit<T>(T value) {
			return value;
		}
	}

	static List<Item> Items() {
		return Enumerable.Range(0, 100).Select(index => new Item { id = index, name = "n" + index }).ToList();
	}

	[ConformanceTest]
	internal static void WhereSelectOverOwnType() {
		Check.Equal(50, Items().Where(item => item.id % 2 == 0).Select(item => item.name).Count());
	}

	[ConformanceTest]
	internal static void OrderByThenByOverOwnType() {
		Check.Equal(0, Items().OrderBy(item => item.name).ThenBy(item => item.id).First().id);
	}

	[ConformanceTest]
	internal static void GenericInterfaceMethodWithOwnType() {
		IVisitor visitor = new Echo();
		Check.Equal(3, visitor.Visit(Items()[3]).id);
	}
}
