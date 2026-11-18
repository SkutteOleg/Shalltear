using System;
using Shalltear.Generated;
using ZqExtra;
using ZqGame;

namespace ZqHost;

public static class ZqEntry {
	public static int Main() {
		try {
			foreach (int zqMode in new[] { 0, 1, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212 }) {
				Console.WriteLine("mode " + zqMode);
				int zqSum = ZqGameTypeTable.Register() + ZqWorld.ZqRun(zqMode) + ZqArena.ZqRun(zqMode);
				Console.WriteLine("checksum " + zqSum);
			}
			return 0;
		} catch (Exception zqError) {
			Console.WriteLine("escaped: " + zqError.GetType().Name + " " + zqError.Message);
			return 1;
		}
	}
}
