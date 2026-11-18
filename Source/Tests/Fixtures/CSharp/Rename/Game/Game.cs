using System.Collections;
using System.Runtime.CompilerServices;

[assembly:InternalsVisibleTo("ZqExtra.Scripts")]

namespace Shalltear.Generated {
	public static class ZqGameTypeTable {
		public static int Register() => ZqGame.ZqWorld.ZqRegistered = 7;
	}
}

namespace ZqGame {
	public enum ZqElement {
		ZqFire,
		ZqIce,
		ZqStorm,
	}

	[Flags]
	public enum ZqTraits : byte {
		ZqNone = 0,
		ZqFast = 1,
		ZqHeavy = 2,
	}

	[Shalltear.KeepName]
	public enum ZqSaveSlot {
		ZqSlotAlpha,
		ZqSlotBeta,
	}

	public struct ZqHit {
		public int zqAmount;
		public ZqElement zqKind;
	}

	public interface IZqUsable {
		int ZqUse(int zqInput);
	}

	public delegate int ZqSignal(int zqArg);

	[AttributeUsage(AttributeTargets.All)]
	public sealed class ZqTagAttribute : Attribute {
		public ZqTagAttribute(Type zqType, ZqElement zqElement) {
			ZqType = zqType;
			ZqElementValue = zqElement;
		}
		public Type ZqType { get; }
		public ZqElement ZqElementValue { get; }
		public object? ZqBoxed { get; set; }
		public Type[]? ZqMany { get; set; }
	}

	public sealed class ZqFailure : Exception {
		public ZqFailure(string zqMessage) : base(zqMessage) {}
	}

	public record ZqPoint(int ZqX, int ZqY);

	public readonly record struct ZqSize(float ZqWidth, float ZqHeight);

	public sealed class ZqBag<TZqThing>
		where TZqThing : struct {
		readonly List<TZqThing> zqItems = new();
		public void ZqAdd(TZqThing zqItem) => zqItems.Add(zqItem);
		public int ZqCount => zqItems.Count;
		public TZqOut ZqFold<TZqOut>(TZqOut zqSeed, Func<TZqOut, TZqThing, TZqOut> zqStep) {
			foreach (var zqItem in zqItems)
				zqSeed = zqStep(zqSeed, zqItem);
			return zqSeed;
		}
	}

	[ZqTag(typeof(ZqMonster), ZqElement.ZqStorm, ZqBoxed = ZqTraits.ZqHeavy, ZqMany = new[] { typeof(ZqHit), typeof(ZqBag<ZqHit>) })]
	public class ZqHero : IZqUsable {
		public float zqHealth = 10;
		public string zqName = "ZqLiteral";
		public (int zqHp, int zqMp) zqStats = (3, 4);
		public int ZqLevel { get; set; } = 2;
		public event ZqSignal? ZqChanged;
		int zqHidden = 41;

		public sealed class ZqInner {
			public int zqDepth = 5;
			public int ZqDive() => zqDepth * 2;
		}

		public virtual int ZqTick(int zqDelta) {
			zqHealth += zqDelta;
			return (int)zqHealth + (ZqChanged?.Invoke(zqDelta) ?? 0);
		}

		public int ZqUse(int zqInput) => zqInput + ZqLevel;

		public IEnumerator ZqRunRoutine(int zqSteps) {
			for (int zqIndex = 0; zqIndex < zqSteps; zqIndex++) {
				zqHealth += 1;
				yield return zqIndex;
			}
		}

		public async Task<int> ZqLoadAsync(int zqSeed) {
			await Task.Yield();
			int zqLoaded = zqSeed * 3;
			await Task.Delay(1);
			return zqLoaded + ZqLevel;
		}

		public int ZqCount(IEnumerable<ZqHit> zqHits, int zqThreshold) {
			int zqLocalTotal = 0;
			foreach (var zqHit in zqHits.Where(zqEach => zqEach.zqAmount > zqThreshold)) {
				zqLocalTotal += zqHit.zqAmount;
			}
			Func<int, int> zqAdder = zqMore => zqLocalTotal + zqMore + zqThreshold;
			return zqAdder(1);
		}

		public override string ToString() => $"{zqName}:{ZqLevel}";
	}

	public class ZqMonster : ZqHero {
		public ZqTraits zqTraits = ZqTraits.ZqFast | ZqTraits.ZqHeavy;
		public override int ZqTick(int zqDelta) => base.ZqTick(zqDelta) * 2 + (int)zqTraits;
	}

	public class ZqPlain {
		public int zqField = 1;
	}

	public static class ZqSecret {
		[UnsafeAccessor(UnsafeAccessorKind.Field, Name = "zqHidden")]
		static extern ref int ZqHidden(ZqHero zqHero);

		public static int ZqPeek(ZqHero zqHero) => ZqHidden(zqHero);
	}

	public static class ZqWorld {
		public static int ZqRegistered;

		internal static int ZqShared(int zqValue) => zqValue + 1;

		public static int ZqRun(int zqMode) {
			var zqHero = new ZqHero();
			var zqMonster = new ZqMonster();
			zqHero.ZqChanged += zqValue => zqValue + 1;
			int zqSum = zqHero.ZqTick(2) + zqMonster.ZqTick(3);

			IZqUsable zqUsable = zqMode % 2 == 0 ? zqHero : zqMonster;
			zqSum += zqUsable.ZqUse(4);

			var zqRoutine = zqHero.ZqRunRoutine(3);
			while (zqRoutine.MoveNext())
				zqSum += (int)zqRoutine.Current!;

			zqSum += zqHero.ZqLoadAsync(5).GetAwaiter().GetResult();

			var zqHits = new List<ZqHit> {
				new ZqHit { zqAmount = 5, zqKind = ZqElement.ZqIce },
				new ZqHit { zqAmount = 9, zqKind = ZqElement.ZqStorm },
			};
			zqSum += zqHero.ZqCount(zqHits, 6);

			var zqBag = new ZqBag<ZqHit>();
			foreach (var zqHit in zqHits)
				zqBag.ZqAdd(zqHit);
			zqSum += zqBag.ZqFold(0, (zqTotal, zqHit) => zqTotal + zqHit.zqAmount) + zqBag.ZqCount;

			var zqByKind = zqHits.ToDictionary(zqHit => zqHit.zqKind, zqHit => zqHit.zqAmount);
			zqSum += zqByKind[ZqElement.ZqStorm];
			zqSum += zqHits.GroupBy(zqHit => zqHit.zqKind).Select(zqGroup => zqGroup.Count()).Sum();

			var zqPoint = new ZqPoint(3, 4);
			var zqMoved = zqPoint with { ZqX = 10 };
			zqSum += zqMoved.ZqX + zqMoved.ZqY + (zqPoint == new ZqPoint(3, 4) ? 1 : 0);
			var zqSize = new ZqSize(2, 3);
			zqSum += (int)(zqSize.ZqWidth * zqSize.ZqHeight);

			var zqAnon = new { ZqAnonName = "x", ZqAnonValue = 6 };
			zqSum += zqAnon.ZqAnonValue + zqHero.zqStats.zqHp + zqHero.zqStats.zqMp;
			zqSum += new ZqHero.ZqInner().ZqDive();
			zqSum += zqMonster.zqTraits.HasFlag(ZqTraits.ZqHeavy) ? 100 : 0;
			zqSum += zqHits[0].zqKind switch { ZqElement.ZqFire => 1, ZqElement.ZqIce => 2,
											   _ => 3 };
			zqSum += (int)ZqSaveSlot.ZqSlotBeta;

			try {
				if (zqMode >= 0)
					throw new ZqFailure("ZqBoom");
			} catch (ZqFailure zqCaught) {
				zqSum += zqCaught.Message.Length;
			}

			return zqSum + ZqRegistered + ZqAsk(zqMode, zqHero, zqMonster, zqPoint, zqAnon);
		}

		static int ZqAsk(int zqMode, ZqHero zqHero, ZqMonster zqMonster, ZqPoint zqPoint, object zqAnon) {
			switch (zqMode) {
			case 201:
				Console.WriteLine(ZqElement.ZqIce.ToString());
				break;
			case 202:
				Console.WriteLine(new ZqPlain().ToString());
				break;
			case 203:
				Console.WriteLine(typeof(ZqBag<ZqHit>).Name + "|" + typeof(ZqHero.ZqInner).FullName);
				break;
			case 204:
				Console.WriteLine(zqPoint.ToString() + "|" + new ZqSize(1, 2).ToString());
				break;
			case 205:
				Console.WriteLine(zqMonster.zqTraits.ToString());
				break;
			case 206:
				try {
					throw new ZqFailure("x");
				} catch (Exception zqError) { Console.WriteLine(zqError.GetType().Name + "|" + zqError.ToString().Split('\n').Length); }
				break;
			case 207:
				Console.WriteLine(Enum.TryParse<ZqElement>("ZqFire", out var zqParsed) ? "parsed " + (int)zqParsed : "not parsed");
				Console.WriteLine(Enum.TryParse<ZqSaveSlot>("ZqSlotBeta", out var zqSlot) ? "parsed " + (int)zqSlot : "not parsed");
				break;
			case 208:
				Console.WriteLine(zqAnon.ToString());
				break;
			case 209:
				Console.WriteLine(nameof(ZqHero) + "|" + ZqSaveSlot.ZqSlotAlpha);
				break;
			case 210:
				foreach (var zqAttribute in typeof(ZqHero).GetCustomAttributes(typeof(ZqTagAttribute), false)) {
					var zqTag = (ZqTagAttribute)zqAttribute;
					Console.WriteLine(zqTag.ZqType.Name + "|" + (int)zqTag.ZqElementValue + "|" + zqTag.ZqBoxed + "|" + zqTag.ZqMany!.Length);
				}
				break;
			case 212:
				Console.WriteLine(zqHero.ToString() + "|" + ZqSecret.ZqPeek(zqHero));
				break;
			}
			return 0;
		}
	}
}
