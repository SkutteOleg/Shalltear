#pragma once

#include "DotNetToolchain.h"

#include "Shalltear/Core/Containers.h"
#include "Shalltear/Core/Types.h"

#include "Shalltear/Editor/ChildProcess.h"

#include <atomic>
#include <functional>
#include <thread>

namespace Shalltear::CSharpEditor {

	enum class AcquireStep : u8 {
		Idle = 0,
		Sizing,
		Downloading,
		Verifying,
		Extracting,
		Restoring,
		Done,
		Failed,
		Cancelled
	};

	struct AcquireOptions {
		DotNetToolchain::Pin pin;
		str cacheRoot;
		str curl;
		str tar;
		std::function<str(const str& sdkRoot, const str& packagesRoot)> restoreCommand;
	};

	struct AcquireStatus {
		AcquireStep step = AcquireStep::Idle;
		u64 bytesDone = 0;
		u64 bytesTotal = 0;
		u32 retries = 0;
		str error;
	};

	class DotNetAcquire {
	public:
		explicit DotNetAcquire(AcquireOptions options);
		~DotNetAcquire();

		DotNetAcquire(const DotNetAcquire&) = delete;
		DotNetAcquire& operator=(const DotNetAcquire&) = delete;

		void Start();
		void Pump();
		void Cancel();

		bool Run();

		const AcquireStatus& GetStatus() const { return _status; }
		bool IsFinished() const;
		const str& GetInstalledRoot() const { return _installed; }
	private:
		void BeginDownload();
		void BeginVerify();
		void BeginExtract();
		void BeginRestore();
		void FinishRestore(bool restored, const str& detail);
		bool RetryDownload();
		void Fail(str message, bool keepWork = false);
		void RemoveWork();
		void RemovePackages();
		str TakeLastLine();

		AcquireOptions _options;
		AcquireStatus _status;
		ChildProcess _child;
		std::thread _hasher;
		std::atomic<bool> _hashed{false};
		str _hash;
		Vector<str> _lines{GetMemoryResource(MemTag::Editor)};
		str _work;
		str _archive;
		str _staged;
		str _installed;
		str _packages;
		bool _downloadRetried = false;
		bool _restoreRetried = false;
	};

	str DescribeAcquireStep(const AcquireStatus& status, const DotNetToolchain::Pin& pin);

	bool VerifyPacks(const str& packagesRoot, const DotNetToolchain::Pin& pin, str& outError);

} // namespace Shalltear::CSharpEditor
