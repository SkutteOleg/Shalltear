using ZqGame;

namespace ZqExtra {
	[ZqTag(typeof(ZqHero), ZqElement.ZqIce, ZqBoxed = ZqElement.ZqFire, ZqMany = new[] { typeof(ZqBoss), typeof(ZqHero.ZqInner) })]
	public sealed class ZqBoss : ZqMonster, IZqUsable {
		public ZqElement zqWeakness = ZqElement.ZqFire;
		public override int ZqTick(int zqDelta) => base.ZqTick(zqDelta) + (int)zqWeakness + 11;
		int IZqUsable.ZqUse(int zqInput) => zqInput * 5;
	}

	public enum ZqPhase {
		ZqIntro,
		ZqEnraged,
	}

	public static class ZqArena {
		public static int ZqRun(int zqMode) {
			var zqBoss = new ZqBoss();
			IZqUsable zqUsable = zqBoss;
			var zqBag = new ZqBag<ZqHit>();
			zqBag.ZqAdd(new ZqHit { zqAmount = 2, zqKind = ZqElement.ZqStorm });
			var zqInner = new ZqHero.ZqInner();
			int zqSum = zqBoss.ZqTick(1) + zqUsable.ZqUse(2) + zqBag.ZqCount + zqInner.ZqDive() + (int)ZqPhase.ZqEnraged + ZqWorld.ZqShared(1);
			if (zqMode == 211) {
				Console.WriteLine(ZqPhase.ZqEnraged + "|" + zqBoss.GetType().FullName + "|" + typeof(ZqBoss).BaseType!.Name);
				foreach (var zqAttribute in typeof(ZqBoss).GetCustomAttributes(typeof(ZqTagAttribute), false)) {
					var zqTag = (ZqTagAttribute)zqAttribute;
					Console.WriteLine(zqTag.ZqType.Name + "|" + (int)zqTag.ZqElementValue + "|" + zqTag.ZqBoxed + "|" + zqTag.ZqMany!.Length);
				}
			}
			return zqSum;
		}
	}
}
