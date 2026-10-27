#include "common/bankIoTrace.h"

#include <type_traits>

static void Check(bool ok) { if (!ok) { std::puts("BankIoTraceTests failed"); std::abort(); } }

int main(int argc, char** argv) {
	using Common::BankIoTrace;
	const bool enabled = argc == 2 && std::strcmp(argv[1], "enabled") == 0;
#ifdef _WIN32
	_putenv_s("KYTY_BANK_IO_TRACE", enabled ? "1" : "0");
#else
	setenv("KYTY_BANK_IO_TRACE", enabled ? "1" : "0", 1);
#endif
	const char* log = enabled ? "bank-io-enabled.log" : "bank-io-disabled.log";
	Check(std::freopen(log, "w+", stderr) != nullptr);
	Check(BankIoTrace::Matches("/app0/Media/StreamingAssets/Player/Young K.assets.BANK"));
	Check(!BankIoTrace::Matches("/app0/no.bank/asset") && !BankIoTrace::Matches("x.bank.tmp"));
	BankIoTrace open("open", -1);
	open.Start("/app0/missing.bank", 7);
	constexpr int error = static_cast<int>(0x80020002u);
	static_assert(std::is_same_v<decltype(open.Result(error)), int>);
	Check(open.Result(error) == error);
	BankIoTrace read("pread", 12, INT64_C(0x100000021), 128);
	read.Start("/app0/space and\nquote\".bank", 8);
	read.HostPath("C:\\test\\space.bank");
	read.Context(INT64_C(0x100000021), INT64_C(0x100000040));
	Check(read.Result(int64_t{31}) == 31); // EOF short read: keep position and size >4 GiB.
	BankIoTrace filtered("read", 13);
	filtered.Start("/app0/ignored.bin", 9);
	Check(!filtered.Active() && filtered.Result(42) == 42);
	std::fflush(stderr);
	std::rewind(stderr);
	char buffer[8192] {};
	const auto bytes = std::fread(buffer, 1, sizeof(buffer), stderr);
	const std::string text(buffer, bytes);
	if (enabled) {
		Check(text.find("[BANK_IO_TRACE] started") != std::string::npos);
		Check(text.find("id=1 phase=enter") != std::string::npos && text.find("id=1 phase=exit") != std::string::npos);
		Check(text.find("result=-2147352574") != std::string::npos);
		Check(text.find("pos=4294967329 size=4294967360 result=31") != std::string::npos);
		Check(text.find("space and\\nquote\\\".bank") != std::string::npos);
		Check(text.find("ignored.bin") == std::string::npos);
	} else { Check(text.empty() && !read.Active()); }
	std::puts("BankIoTraceTests passed");
	return 0;
}
