#include "TLSBackend.h"

namespace Shalltear::Net::TLSBackend {

	namespace {

		constexpr const char* UNSUPPORTED_MESSAGE = "raw TLS streams are unsupported on Web (use https:// or wss://)";

	} // anonymous namespace

	bool IsAvailable() {
		return false;
	}

	Session* CreateClient(const str&, str& error) {
		error = UNSUPPORTED_MESSAGE;
		return nullptr;
	}

	Session* CreateServer(const TLSIdentityData&, str& error) {
		error = UNSUPPORTED_MESSAGE;
		return nullptr;
	}

	void Destroy(Session*) {
	}

	Step Advance(Session*, Vector<u8>&, Vector<u8>&, Vector<u8>&, str& error) {
		error = UNSUPPORTED_MESSAGE;
		return Step::Failed;
	}

	bool Encrypt(Session*, std::span<const u8>, Vector<u8>&, str& error) {
		error = UNSUPPORTED_MESSAGE;
		return false;
	}

	void Shutdown(Session*, Vector<u8>&) {
	}

	bool PeerCertificateHash(Session*, CertificateHash&) {
		return false;
	}

	bool ValidatePeer(Session*, const str&, str& error) {
		error = UNSUPPORTED_MESSAGE;
		return false;
	}

	bool LoadPKCS12(std::span<const u8>, std::string_view, TLSIdentityData&, str& error) {
		error = UNSUPPORTED_MESSAGE;
		return false;
	}

} // namespace Shalltear::Net::TLSBackend
