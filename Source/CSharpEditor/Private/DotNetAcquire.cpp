#include "DotNetAcquire.h"

#include "SHA512.h"
#include "SystemTools.h"

#include "Shalltear/Core/FileSystem.h"
#include "Shalltear/Core/Memory.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>

namespace Shalltear::CSharpEditor {

	namespace {

		namespace fs = std::filesystem;

		constexpr u64 MEGABYTE = 1024ull * 1024ull;

		constexpr std::string_view NuGetConfig =
			"<configuration>\n"
			"\t<packageSources>\n"
			"\t\t<clear />\n"
			"\t\t<add key=\"nuget.org\" value=\"https://api.nuget.org/v3/index.json\" />\n"
			"\t</packageSources>\n"
			"\t<fallbackPackageFolders>\n"
			"\t\t<clear />\n"
			"\t</fallbackPackageFolders>\n"
			"</configuration>\n";

		str RestoreProject(const DotNetToolchain::Pin& pin) {
			return std::format(
				"<Project Sdk=\"Microsoft.NET.Sdk\">\n"
				"\t<PropertyGroup>\n"
				"\t\t<TargetFramework>{}</TargetFramework>\n"
				"\t\t<OutputType>Library</OutputType>\n"
				"\t\t<PublishAot>true</PublishAot>\n"
				"\t\t<NativeLib>Shared</NativeLib>\n"
				"\t\t<DisableTransitiveFrameworkReferenceDownloads>true</DisableTransitiveFrameworkReferenceDownloads>\n"
				"\t\t<DisableImplicitNuGetFallbackFolder>true</DisableImplicitNuGetFallbackFolder>\n"
				"\t</PropertyGroup>\n"
				"</Project>\n",
				pin.targetFramework);
		}

		str FileNameOf(const str& url) {
			const size_t slash = url.find_last_of('/');
			return slash == str::npos ? url : url.substr(slash + 1);
		}

		u64 ParseContentLength(const Vector<str>& lines) {
			constexpr std::string_view Header = "content-length:";
			u64 length = 0;
			for (const str& line : lines) {
				if (line.size() <= Header.size()) {
					continue;
				}
				const bool matches = std::equal(Header.begin(), Header.end(), line.begin(), [](char expected, char found) {
					return expected == static_cast<char>(std::tolower(static_cast<unsigned char>(found)));
				});
				if (!matches) {
					continue;
				}
				size_t start = Header.size();
				while (start < line.size() && line[start] == ' ') {
					++start;
				}
				u64 parsed = 0;
				if (std::from_chars(line.data() + start, line.data() + line.size(), parsed).ec == std::errc{}) {
					length = parsed;
				}
			}
			return length;
		}

	} // anonymous namespace

	DotNetAcquire::DotNetAcquire(AcquireOptions options) : _options(std::move(options)) {
		if (_options.curl.empty()) {
			_options.curl = FindSystemTool("curl");
		}
		if (_options.tar.empty()) {
			_options.tar = FindSystemTool("tar");
		}
		const fs::path cache(_options.cacheRoot);
		_installed = (cache / _options.pin.sdkVersion).generic_string();
		_work = (cache / (_options.pin.sdkVersion + ".partial")).generic_string();
		_archive = (fs::path(_work) / FileNameOf(_options.pin.archiveURL)).generic_string();
		_staged = (fs::path(_work) / "sdk").generic_string();
		_packages = DotNetToolchain::GetPackagesRoot(_options.cacheRoot, _options.pin);
	}

	DotNetAcquire::~DotNetAcquire() {
		if (!IsFinished() && _status.step != AcquireStep::Idle) {
			Cancel();
		}
		if (_hasher.joinable()) {
			_hasher.join();
		}
	}

	bool DotNetAcquire::IsFinished() const {
		return _status.step == AcquireStep::Done || _status.step == AcquireStep::Failed || _status.step == AcquireStep::Cancelled;
	}

	void DotNetAcquire::RemoveWork() {
		std::error_code ec;
		fs::remove_all(fs::path(_work), ec);
	}

	void DotNetAcquire::RemovePackages() {
		std::error_code ec;
		fs::remove_all(fs::path(_packages), ec);
	}

	str DotNetAcquire::TakeLastLine() {
		_child.DrainLines(_lines);
		str last;
		for (const str& line : _lines) {
			if (!line.empty()) {
				last = line;
			}
		}
		_lines.clear();
		return last;
	}

	void DotNetAcquire::Fail(str message, bool keepWork) {
		_child.Kill();
		if (!keepWork) {
			RemoveWork();
		}
		_status.error = std::move(message);
		_status.step = AcquireStep::Failed;
	}

	void DotNetAcquire::Start() {
		if (_status.step != AcquireStep::Idle) {
			return;
		}
		_status = {};
		RemoveWork();
		std::error_code ec;
		fs::create_directories(fs::path(_work), ec);
		if (ec) {
			Fail(std::format("couldn't create '{}' ({})", _work, ec.message()));
			return;
		}
		if (DotNetToolchain::HoldsPin(_installed, _options.pin)) {
			BeginRestore();
			return;
		}
		_status.step = AcquireStep::Sizing;
		const str command = std::format("{} --silent --head --location --fail {}", QuoteCommandArgument(_options.curl),
										QuoteCommandArgument(_options.pin.archiveURL));
		if (!_child.Start(command, _work)) {
			Fail(std::format("curl was not found. Download {} -> extract it into '{}'", _options.pin.archiveURL, _installed));
		}
	}

	void DotNetAcquire::BeginDownload() {
		_status.step = AcquireStep::Downloading;
		_status.bytesDone = 0;
		const str command = std::format("{} --silent --show-error --location --fail --output {} {}", QuoteCommandArgument(_options.curl),
										QuoteCommandArgument(_archive), QuoteCommandArgument(_options.pin.archiveURL));
		if (!_child.Start(command, _work)) {
			Fail(std::format("curl was not found. Download {} -> extract it into '{}'", _options.pin.archiveURL, _installed));
		}
	}

	bool DotNetAcquire::RetryDownload() {
		if (_downloadRetried) {
			return false;
		}
		_downloadRetried = true;
		++_status.retries;
		std::error_code ec;
		fs::remove(fs::path(_archive), ec);
		BeginDownload();
		return true;
	}

	void DotNetAcquire::BeginVerify() {
		_status.step = AcquireStep::Verifying;
		_hashed = false;
		_hasher = std::thread([this] {
			_hash = HashFile(_archive);
			_hashed = true;
		});
	}

	void DotNetAcquire::BeginExtract() {
		_status.step = AcquireStep::Extracting;
		std::error_code ec;
		fs::create_directories(fs::path(_staged), ec);
		const str command =
			std::format("{} -xf {} -C {}", QuoteCommandArgument(_options.tar), QuoteCommandArgument(_archive), QuoteCommandArgument(_staged));
		if (!_child.Start(command, _work)) {
			Fail(std::format("tar was not found. Extract '{}' into '{}'", _archive, _installed), true);
		}
	}

	void DotNetAcquire::BeginRestore() {
		_status.step = AcquireStep::Restoring;
		std::error_code ec;
		fs::create_directories(fs::path(_packages), ec);
		const fs::path project = fs::path(_work) / "restore" / "Player.csproj";
		fs::create_directories(project.parent_path(), ec);
		if (!FileSystem::WriteTextFile(Path(project.generic_string()), RestoreProject(_options.pin)) ||
			!FileSystem::WriteTextFile(Path((project.parent_path() / "nuget.config").generic_string()), str(NuGetConfig))) {
			Fail(std::format("couldn't write '{}'", project.generic_string()));
			return;
		}
		const str command = _options.restoreCommand
								? _options.restoreCommand(_installed, _packages)
								: std::format("{} restore {} -p:RuntimeIdentifier={} --packages {} -v quiet",
											  QuoteCommandArgument(DotNetToolchain::GetMuxerPath(_installed)), QuoteCommandArgument(project.generic_string()),
											  _options.pin.hostRid, QuoteCommandArgument(_packages));
		if (!_child.Start(command, project.parent_path().generic_string())) {
			Fail(std::format("couldn't start '{}'", DotNetToolchain::GetMuxerPath(_installed)));
		}
	}

	void DotNetAcquire::FinishRestore(bool restored, const str& detail) {
		str error;
		if (restored && VerifyPacks(_packages, _options.pin, error)) {
			RemoveWork();
			_status.step = AcquireStep::Done;
			return;
		}
		if (!restored && !_restoreRetried) {
			_restoreRetried = true;
			++_status.retries;
			RemovePackages();
			BeginRestore();
			return;
		}
		RemovePackages();
		Fail(restored ? error : std::format("native build packages couldn't be restored ({}). Reopen the project to retry", detail));
	}

	void DotNetAcquire::Pump() {
		i32 exitCode = 0;
		switch (_status.step) {
		case AcquireStep::Sizing:
			_child.DrainLines(_lines);
			if (!_child.TryFinish(exitCode)) {
				return;
			}
			_child.DrainLines(_lines);
			_status.bytesTotal = exitCode == 0 ? ParseContentLength(_lines) : 0;
			_lines.clear();
			BeginDownload();
			return;
		case AcquireStep::Downloading: {
			std::error_code ec;
			const std::uintmax_t size = fs::file_size(fs::path(_archive), ec);
			_status.bytesDone = ec ? _status.bytesDone : static_cast<u64>(size);
			if (!_child.TryFinish(exitCode)) {
				return;
			}
			const str detail = TakeLastLine();
			if (exitCode != 0) {
				if (!RetryDownload()) {
					Fail(std::format(".NET SDK {} download failed ({}). Check the connection, reopen the project", _options.pin.sdkVersion,
									 detail.empty() ? std::format("curl exit {}", exitCode) : detail));
				}
				return;
			}
			BeginVerify();
			return;
		}
		case AcquireStep::Verifying:
			if (!_hashed) {
				return;
			}
			_hasher.join();
			if (_hash != _options.pin.archiveSHA512) {
				if (!RetryDownload()) {
					Fail(std::format(".NET SDK {} archive has SHA-512 {}, expected {}. Reopen the project to download it again", _options.pin.sdkVersion,
									 _hash.empty() ? str("(unreadable)") : _hash, _options.pin.archiveSHA512));
				}
				return;
			}
			BeginExtract();
			return;
		case AcquireStep::Extracting: {
			if (!_child.TryFinish(exitCode)) {
				return;
			}
			const str detail = TakeLastLine();
			if (exitCode != 0) {
				Fail(std::format(".NET SDK {} archive couldn't be extracted ({})", _options.pin.sdkVersion,
								 detail.empty() ? std::format("tar exit {}", exitCode) : detail));
				return;
			}
			if (!DotNetToolchain::HoldsPin(_staged, _options.pin)) {
				Fail(std::format(".NET SDK archive has no SDK {} with runtime {} inside", _options.pin.sdkVersion, _options.pin.runtimeVersion));
				return;
			}
			std::error_code ec;
			fs::remove_all(fs::path(_installed), ec);
			fs::rename(fs::path(_staged), fs::path(_installed), ec);
			if (ec) {
				Fail(std::format("couldn't move .NET SDK into '{}' ({})", _installed, ec.message()));
				return;
			}
			BeginRestore();
			return;
		}
		case AcquireStep::Restoring: {
			if (!_child.TryFinish(exitCode)) {
				return;
			}
			const str detail = TakeLastLine();
			FinishRestore(exitCode == 0, detail.empty() ? std::format("dotnet exit {}", exitCode) : detail);
			return;
		}
		default:
			return;
		}
	}

	void DotNetAcquire::Cancel() {
		if (IsFinished() || _status.step == AcquireStep::Idle) {
			return;
		}
		const bool restoring = _status.step == AcquireStep::Restoring;
		_child.Kill();
		if (_hasher.joinable()) {
			_hasher.join();
		}
		RemoveWork();
		if (restoring) {
			RemovePackages();
		}
		_status.step = AcquireStep::Cancelled;
	}

	bool DotNetAcquire::Run() {
		Start();
		while (!IsFinished()) {
			Pump();
			if (!IsFinished()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		}
		return _status.step == AcquireStep::Done;
	}

	str DescribeAcquireStep(const AcquireStatus& status, const DotNetToolchain::Pin& pin) {
		switch (status.step) {
		case AcquireStep::Sizing:
		case AcquireStep::Downloading:
			if (status.bytesTotal > 0) {
				return std::format("Downloading .NET SDK {} ({} of {} MB)", pin.sdkVersion, status.bytesDone / MEGABYTE, status.bytesTotal / MEGABYTE);
			}
			return std::format("Downloading .NET SDK {} ({} MB)", pin.sdkVersion, status.bytesDone / MEGABYTE);
		case AcquireStep::Verifying:
			return std::format("Checking .NET SDK {}", pin.sdkVersion);
		case AcquireStep::Extracting:
			return std::format("Extracting .NET SDK {}", pin.sdkVersion);
		case AcquireStep::Restoring:
			return "Restoring native build packages";
		case AcquireStep::Done:
			return std::format(".NET SDK {} is ready", pin.sdkVersion);
		case AcquireStep::Failed:
			return status.error;
		case AcquireStep::Cancelled:
			return "Download cancelled";
		default:
			return {};
		}
	}

	bool VerifyPacks(const str& packagesRoot, const DotNetToolchain::Pin& pin, str& outError) {
		std::error_code listed;
		const fs::directory_iterator ids(fs::path(packagesRoot), listed);
		if (listed) {
			outError = std::format("couldn't read '{}' ({})", packagesRoot, listed.message());
			return false;
		}
		std::error_code ec;
		for (const fs::directory_entry& id : ids) {
			if (!id.is_directory(ec)) {
				continue;
			}
			for (const fs::directory_entry& version : fs::directory_iterator(id.path(), ec)) {
				if (!version.is_directory(ec)) {
					continue;
				}
				for (const fs::directory_entry& file : fs::directory_iterator(version.path(), ec)) {
					if (file.path().extension() != ".nupkg") {
						continue;
					}
					const str name = id.path().filename().generic_string() + "/" + version.path().filename().generic_string();
					const auto pinned = std::find_if(pin.packs.begin(), pin.packs.end(),
													 [&name](const DotNetToolchain::PinnedPack& pack) { return pack.name == name; });
					if (pinned == pin.packs.end()) {
						outError = std::format("package '{}' is not expected (not used)", name);
						return false;
					}
					const str hash = HashFile(file.path().generic_string());
					if (hash.empty()) {
						outError = std::format("package '{}' couldn't be read ('{}')", name, file.path().generic_string());
						return false;
					}
					if (hash != pinned->sha512) {
						outError = std::format("package '{}' has SHA-512 {}, expected {}. Reopen the project to restore it again", name, hash, pinned->sha512);
						return false;
					}
				}
			}
		}
		return true;
	}

} // namespace Shalltear::CSharpEditor
