using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;

namespace Shalltear.Renamer;

internal static class WasmILStripper {
	const byte COR_ILMETHOD_TINY_FORMAT = 0x02;
	const byte COR_ILMETHOD_FAT_FORMAT = 0x03;
	const ushort COR_ILMETHOD_MORE_SECTS = 0x0008;
	const byte COR_ILMETHOD_SECT_FAT_FORMAT = 0x40;
	const byte COR_ILMETHOD_SECT_MORE_SECTS = 0x80;

	public static int Run(string dllDir, string tokensDir) {
		if (!Directory.Exists(dllDir)) {
			Console.Error.WriteLine($"strip-wasm-il: assembly directory not found: '{dllDir}'");
			return 2;
		}
		if (!Directory.Exists(tokensDir)) {
			Console.Error.WriteLine($"strip-wasm-il: tokens directory not found: '{tokensDir}'");
			return 2;
		}

		string fullDllDir = Path.GetFullPath(dllDir);
		string fullTokensDir = Path.GetFullPath(tokensDir);
		int assembliesStripped = 0;
		int methodsStripped = 0;
		long bytesZeroed = 0;

		foreach (string tokenFile in Directory.EnumerateFiles(fullTokensDir, "*.tokens", SearchOption.TopDirectoryOnly)
			.OrderBy(static p => Path.GetFileName(p), StringComparer.OrdinalIgnoreCase)) {
			string[] lines = File.ReadAllLines(tokenFile);
			if (lines.Length < 2) {
				continue;
			}
			string assemblyName = Path.GetFileName(lines[0].Trim());
			if (string.IsNullOrEmpty(assemblyName)) {
				continue;
			}
			if (!Guid.TryParse(lines[1].Trim(), out Guid expectedMvid)) {
				Console.Error.WriteLine($"strip-wasm-il: invalid MVID in '{tokenFile}'");
				return 1;
			}

			HashSet<uint> eligibleTokens = new();
			for (int i = 2; i < lines.Length; ++i) {
				string line = lines[i].Trim();
				if (line.Length == 0) {
					continue;
				}
				if (uint.TryParse(line, System.Globalization.NumberStyles.HexNumber, System.Globalization.CultureInfo.InvariantCulture, out uint token) &&
					(token >> 24) == 0x06) {
					eligibleTokens.Add(token);
				}
			}
			if (eligibleTokens.Count == 0) {
				continue;
			}

			string dllPath = Path.Combine(fullDllDir, assemblyName);
			if (!File.Exists(dllPath)) {
				Console.Error.WriteLine($"strip-wasm-il: assembly not found: '{dllPath}'");
				return 1;
			}

			if (!StripAssembly(dllPath, expectedMvid, eligibleTokens, out int strippedCount, out long zeroed)) {
				return 1;
			}
			if (strippedCount > 0) {
				++assembliesStripped;
				methodsStripped += strippedCount;
				bytesZeroed += zeroed;
			}
		}

		Console.WriteLine($"WASM IL stripped: {assembliesStripped} assembly(ies), {methodsStripped} method(s), {bytesZeroed} B zeroed");
		return 0;
	}

	static bool StripAssembly(string dllPath, Guid expectedMvid, HashSet<uint> eligibleTokens, out int strippedMethods, out long bytesZeroed) {
		strippedMethods = 0;
		bytesZeroed = 0;

		byte[] data = File.ReadAllBytes(dllPath);
		Dictionary<int, (int total, int eligible)> rvaCounts = new();
		List<int> protectedBounds = new();
		int checksumOffset = -1;

		using (MemoryStream stream = new(data, writable: false))
		using (PEReader peReader = new(stream)) {
			if (!peReader.HasMetadata) {
				return true;
			}
			PEHeaders headers = peReader.PEHeaders;
			MetadataReader reader = peReader.GetMetadataReader();

			Guid actualMvid = reader.GetGuid(reader.GetModuleDefinition().Mvid);
			if (actualMvid != expectedMvid) {
				Console.Error.WriteLine($"strip-wasm-il: MVID mismatch for '{Path.GetFileName(dllPath)}' (expected {expectedMvid}, actual {actualMvid})");
				return false;
			}

			if (headers.MetadataStartOffset > 0) {
				protectedBounds.Add(headers.MetadataStartOffset);
			}
			if (headers.CorHeader != null && headers.CorHeader.ResourcesDirectory.Size > 0 &&
				TryRvaToOffset(headers, headers.CorHeader.ResourcesDirectory.RelativeVirtualAddress, out int resOffset)) {
				protectedBounds.Add(resOffset);
			}
			if (headers.PEHeaderStartOffset >= 0) {
				// Optional header CheckSum field is at offset 64 from PE optional header start
				int candidate = headers.PEHeaderStartOffset + 64;
				if (candidate + 4 <= data.Length) {
					checksumOffset = candidate;
				}
			}

			foreach (MethodDefinitionHandle handle in reader.MethodDefinitions) {
				MethodDefinition method = reader.GetMethodDefinition(handle);
				int rva = method.RelativeVirtualAddress;
				if (rva == 0) {
					continue;
				}
				if (!TryRvaToOffset(headers, rva, out int offset)) {
					continue;
				}
				protectedBounds.Add(offset);
				uint token = (uint)MetadataTokens.GetToken(handle);
				bool isEligible = eligibleTokens.Contains(token);
				if (rvaCounts.TryGetValue(offset, out var entry)) {
					rvaCounts[offset] = (entry.total + 1, entry.eligible + (isEligible ? 1 : 0));
				} else {
					rvaCounts[offset] = (1, isEligible ? 1 : 0);
				}
			}
		}

		protectedBounds.Sort();
		int uniqueCount = 0;
		for (int i = 0; i < protectedBounds.Count; ++i) {
			if (uniqueCount == 0 || protectedBounds[i] != protectedBounds[uniqueCount - 1]) {
				protectedBounds[uniqueCount++] = protectedBounds[i];
			}
		}
		if (uniqueCount < protectedBounds.Count) {
			protectedBounds.RemoveRange(uniqueCount, protectedBounds.Count - uniqueCount);
		}

		foreach (var (offset, counts) in rvaCounts) {
			if (counts.eligible != counts.total) {
				continue;
			}
			int nextBound = FindNextBound(protectedBounds, offset, data.Length);
			if (!TryMeasureMethodBody(data, offset, nextBound, out int bodyLength)) {
				continue;
			}
			if (bodyLength < 3) {
				continue;
			}
			Array.Clear(data, offset, bodyLength);
			// ECMA-335 tiny method header (2 bytes IL): ldnull; throw
			data[offset] = (2 << 2) | COR_ILMETHOD_TINY_FORMAT;
			data[offset + 1] = 0x14;
			data[offset + 2] = 0x7A;
			strippedMethods += counts.total;
			bytesZeroed += bodyLength - 3;
		}

		if (strippedMethods > 0) {
			if (checksumOffset >= 0) {
				Array.Clear(data, checksumOffset, 4);
			}
			File.WriteAllBytes(dllPath, data);
		}
		return true;
	}

	static bool TryRvaToOffset(PEHeaders headers, int rva, out int offset) {
		foreach (SectionHeader section in headers.SectionHeaders) {
			int start = section.VirtualAddress;
			int size = Math.Max(section.VirtualSize, section.SizeOfRawData);
			if (rva >= start && rva < start + size) {
				int delta = rva - start;
				if (delta < section.SizeOfRawData) {
					offset = section.PointerToRawData + delta;
					return true;
				}
			}
		}
		offset = 0;
		return false;
	}

	static int FindNextBound(List<int> sortedBounds, int offset, int fileLength) {
		int idx = sortedBounds.BinarySearch(offset);
		int nextIdx = idx >= 0 ? idx + 1 : ~idx;
		return nextIdx < sortedBounds.Count ? sortedBounds[nextIdx] : fileLength;
	}

	static bool TryMeasureMethodBody(byte[] data, int offset, int limit, out int length) {
		length = 0;
		if (offset < 0 || offset >= limit || limit > data.Length) {
			return false;
		}
		byte first = data[offset];
		int format = first & 0x03;
		if (format == COR_ILMETHOD_TINY_FORMAT) {
			int codeSize = first >> 2;
			int end = offset + 1 + codeSize;
			if (end > limit) {
				return false;
			}
			length = end - offset;
			return true;
		}
		if (format == COR_ILMETHOD_FAT_FORMAT) {
			if (offset + 12 > limit) {
				return false;
			}
			ushort flagsAndSize = (ushort)(data[offset] | (data[offset + 1] << 8));
			int headerBytes = (flagsAndSize >> 12) * 4;
			int codeSize = data[offset + 4] | (data[offset + 5] << 8) | (data[offset + 6] << 16) | (data[offset + 7] << 24);
			if (headerBytes < 12 || codeSize < 0) {
				return false;
			}
			long endLong = (long)offset + headerBytes + codeSize;
			if (endLong > limit) {
				return false;
			}
			int pos = (int)endLong;
			if ((flagsAndSize & COR_ILMETHOD_MORE_SECTS) != 0) {
				while (true) {
					pos = (pos + 3) & ~3;
					if (pos + 4 > limit) {
						return false;
					}
					byte secFlags = data[pos];
					int secSize = (secFlags & COR_ILMETHOD_SECT_FAT_FORMAT) != 0
						? (data[pos + 1] | (data[pos + 2] << 8) | (data[pos + 3] << 16))
						: data[pos + 1];
					if (secSize < 4 || pos + secSize > limit) {
						return false;
					}
					pos += secSize;
					if ((secFlags & COR_ILMETHOD_SECT_MORE_SECTS) == 0) {
						break;
					}
				}
			}
			length = pos - offset;
			return true;
		}
		return false;
	}
}
