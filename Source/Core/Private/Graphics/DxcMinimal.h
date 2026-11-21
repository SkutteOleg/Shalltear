#pragma once

#include "Shalltear/Core/Types.h"

namespace Shalltear::Dxc {

	using HResult = i32;
	inline constexpr HResult S_OK_ = 0;
	inline bool Succeeded(HResult hr) { return hr >= 0; }

	struct Guid {
		u32 data1;
		u16 data2;
		u16 data3;
		u8 data4[8];
	};

	inline constexpr Guid CLSID_DxcCompiler{0x73e22d93, 0xe6ce, 0x47f3, {0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0}};
	inline constexpr Guid CLSID_DxcUtils{0x6245d6af, 0x66e0, 0x48fd, {0x80, 0xb4, 0x4d, 0x27, 0x17, 0x96, 0x74, 0x8c}};
	inline constexpr Guid IID_IDxcUtils{0x4605C4CB, 0x2019, 0x492A, {0xAD, 0xA4, 0x65, 0xF2, 0x0B, 0xB7, 0xD6, 0x7F}};
	inline constexpr Guid IID_IDxcCompiler3{0x228B4687, 0x5A6A, 0x4730, {0x90, 0x0C, 0x97, 0x02, 0xB2, 0x20, 0x3F, 0x54}};
	inline constexpr Guid IID_IDxcResult{0x58346CDA, 0xDDE7, 0x4497, {0x94, 0x61, 0x6F, 0x87, 0xAF, 0x5E, 0x06, 0x59}};
	inline constexpr Guid IID_IDxcBlob{0x8BA5FB08, 0x5195, 0x40e2, {0xAC, 0x58, 0x0D, 0x98, 0x9C, 0x3A, 0x01, 0x02}};
	inline constexpr Guid IID_IDxcBlobUtf8{0x3DA636C9, 0xBA71, 0x4024, {0xA3, 0x01, 0x30, 0xCB, 0xF1, 0x25, 0x30, 0x5B}};

	struct IDxcBlob;
	struct IDxcBlobWide;

	struct IUnknown {
		virtual HResult QueryInterface(const Guid& riid, void** object) = 0;
		virtual u32 AddRef() = 0;
		virtual u32 Release() = 0;
	};

	struct IDxcBlob : IUnknown {
		virtual void* GetBufferPointer() = 0;
		virtual size_t GetBufferSize() = 0;
	};

	struct IDxcBlobEncoding : IDxcBlob {
		virtual HResult GetEncoding(i32* known, u32* codePage) = 0;
	};

	struct IDxcBlobUtf8 : IDxcBlobEncoding {
		virtual const char* GetStringPointer() = 0;
		virtual size_t GetStringLength() = 0;
	};

	struct IDxcIncludeHandler : IUnknown {
		virtual HResult LoadSource(const wchar_t* filename, IDxcBlob** includeSource) = 0;
	};

	struct DxcBuffer {
		const void* ptr;
		size_t size;
		u32 encoding;
	};

	enum DxcOutKind : u32 {
		DXC_OUT_OBJECT = 1,
		DXC_OUT_ERRORS = 2,
	};

	struct IDxcOperationResult : IUnknown {
		virtual HResult GetStatus(HResult* status) = 0;
		virtual HResult GetResult(IDxcBlob** result) = 0;
		virtual HResult GetErrorBuffer(IDxcBlobEncoding** errors) = 0;
	};

	struct IDxcResult : IDxcOperationResult {
		virtual i32 HasOutput(DxcOutKind kind) = 0;
		virtual HResult GetOutput(DxcOutKind kind, const Guid& riid, void** object, IDxcBlobWide** outputName) = 0;
		virtual u32 GetNumOutputs() = 0;
		virtual DxcOutKind GetOutputByIndex(u32 index) = 0;
		virtual DxcOutKind PrimaryOutput() = 0;
	};

	struct IDxcCompiler3 : IUnknown {
		virtual HResult Compile(const DxcBuffer* source, const wchar_t** arguments, u32 argCount,
								IDxcIncludeHandler* includeHandler, const Guid& riid, void** result) = 0;
		virtual HResult Disassemble(const DxcBuffer* object, const Guid& riid, void** result) = 0;
	};

	// only CreateBlobFromPinned is called, other slots are vtable padding
	struct IDxcUtils : IUnknown {
		virtual HResult CreateBlobFromBlob(IDxcBlob* blob, u32 offset, u32 length, IDxcBlob** result) = 0;
		virtual HResult CreateBlobFromPinned(const void* data, u32 size, u32 codePage, IDxcBlobEncoding** result) = 0;
		virtual HResult MoveToBlob(const void* data, void* malloc, u32 size, u32 codePage, IDxcBlobEncoding** result) = 0;
		virtual HResult CreateBlob(const void* data, u32 size, u32 codePage, IDxcBlobEncoding** result) = 0;
		virtual HResult LoadFile(const wchar_t* fileName, u32* codePage, IDxcBlobEncoding** result) = 0;
		virtual HResult CreateReadOnlyStreamFromBlob(IDxcBlob* blob, void** stream) = 0;
		virtual HResult CreateDefaultIncludeHandler(IDxcIncludeHandler** result) = 0;
		virtual HResult GetBlobAsUtf8(IDxcBlob* blob, IDxcBlobUtf8** result) = 0;
		virtual HResult GetBlobAsWide(IDxcBlob* blob, IDxcBlobWide** result) = 0;
	};

	using DxcCreateInstanceProc = HResult (*)(const Guid& clsid, const Guid& iid, void** instance);

} // namespace Shalltear::Dxc
