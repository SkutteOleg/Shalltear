import argparse
import ctypes
import os
import sys


class HostfxrInitializeParameters(ctypes.Structure):
	_fields_ = [
		("size", ctypes.c_size_t),
		("host_path", ctypes.c_wchar_p),
		("dotnet_root", ctypes.c_wchar_p),
	]


def find_hostfxr(roslyn_dir):
	candidates = [
		os.path.join(roslyn_dir, "dotnet"),
		os.environ.get("DOTNET_ROOT", ""),
		os.path.join(os.environ.get("USERPROFILE", ""), ".dotnet"),
		r"C:\Program Files\dotnet",
	]
	for root in candidates:
		if not root:
			continue
		fxr_dir = os.path.join(root, "host", "fxr")
		if not os.path.isdir(fxr_dir):
			continue
		versions = sorted(os.listdir(fxr_dir))
		for ver in reversed(versions):
			dll = os.path.join(fxr_dir, ver, "hostfxr.dll")
			if os.path.isfile(dll):
				return os.path.abspath(root), os.path.abspath(dll)
	return None, None


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--roslyn-dir", required=True)
	parser.add_argument("--repo-root", required=True)
	parser.add_argument("--out", required=True)
	args = parser.parse_args()

	roslyn_dir = os.path.abspath(args.roslyn_dir)
	repo_root = os.path.abspath(args.repo_root)
	out_path = os.path.abspath(args.out)

	roslyn_dll = os.path.join(roslyn_dir, "ShalltearRoslyn.dll")
	runtime_config = os.path.join(roslyn_dir, "ShalltearRoslyn.runtimeconfig.json")
	if not os.path.isfile(roslyn_dll) or not os.path.isfile(runtime_config):
		print(f"error: missing ShalltearRoslyn.dll in {roslyn_dir}", file=sys.stderr)
		return 1

	dotnet_root, hostfxr_dll = find_hostfxr(roslyn_dir)
	if not hostfxr_dll:
		print("error: could not locate hostfxr.dll", file=sys.stderr)
		return 1

	hostfxr = ctypes.CDLL(hostfxr_dll)

	init_fn = hostfxr.hostfxr_initialize_for_runtime_config
	init_fn.argtypes = [
		ctypes.c_wchar_p,
		ctypes.POINTER(HostfxrInitializeParameters),
		ctypes.POINTER(ctypes.c_void_p),
	]
	init_fn.restype = ctypes.c_int32

	get_delegate_fn = hostfxr.hostfxr_get_runtime_delegate
	get_delegate_fn.argtypes = [
		ctypes.c_void_p,
		ctypes.c_int32,
		ctypes.POINTER(ctypes.c_void_p),
	]
	get_delegate_fn.restype = ctypes.c_int32

	close_fn = hostfxr.hostfxr_close
	close_fn.argtypes = [ctypes.c_void_p]
	close_fn.restype = ctypes.c_int32

	params = HostfxrInitializeParameters()
	params.size = ctypes.sizeof(HostfxrInitializeParameters)
	params.host_path = roslyn_dll
	params.dotnet_root = dotnet_root

	ctx = ctypes.c_void_p()
	rc = init_fn(runtime_config, ctypes.byref(params), ctypes.byref(ctx))
	if rc < 0 or not ctx.value:
		print(f"error: hostfxr_initialize_for_runtime_config failed ({rc:#x})", file=sys.stderr)
		return 1

	load_asm_ptr = ctypes.c_void_p()
	# hdt_load_assembly_and_get_function_pointer = 5
	rc = get_delegate_fn(ctx, 5, ctypes.byref(load_asm_ptr))
	close_fn(ctx)
	if rc < 0 or not load_asm_ptr.value:
		print(f"error: hostfxr_get_runtime_delegate failed ({rc:#x})", file=sys.stderr)
		return 1

	LOAD_ASM_FN = ctypes.WINFUNCTYPE(
		ctypes.c_int32,
		ctypes.c_wchar_p,
		ctypes.c_wchar_p,
		ctypes.c_wchar_p,
		ctypes.c_void_p,
		ctypes.c_void_p,
		ctypes.POINTER(ctypes.c_void_p),
	)
	load_asm = LOAD_ASM_FN(load_asm_ptr.value)

	fn_ptr = ctypes.c_void_p()
	unmanaged_callers_only = ctypes.c_void_p(-1)
	rc = load_asm(
		roslyn_dll,
		"Shalltear.Roslyn.RoslynTranspilerExports, ShalltearRoslyn",
		"GenerateEngineStubsCache",
		unmanaged_callers_only,
		None,
		ctypes.byref(fn_ptr),
	)
	if rc < 0 or not fn_ptr.value:
		print(f"error: failed to resolve GenerateEngineStubsCache ({rc:#x})", file=sys.stderr)
		return 1

	GEN_FN = ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.c_char_p, ctypes.c_char_p)
	gen_fn = GEN_FN(fn_ptr.value)

	ok = gen_fn(repo_root.encode("utf-8"), out_path.encode("utf-8"))
	if ok != 1:
		print("error: GenerateEngineStubsCache returned failure", file=sys.stderr)
		return 1

	return 0


if __name__ == "__main__":
	sys.exit(main())
