#include "TestsPCH.h"

#ifndef SHALLTEAR_SHARED

#include "PackageBindings.h"

#include <stdexcept>

using namespace Shalltear;
using namespace Shalltear::CSharpImpl;

namespace {

	str Refusal(std::string_view name, u64 layoutHash, i32 size) {
		try {
			PackageBindings::Find(name, layoutHash, size);
		} catch (const std::runtime_error& error) {
			return error.what();
		}
		return "found";
	}

} // anonymous namespace

TEST(CSharpPackageBindingsTest, TableFoundByNameUntilRemoved) {
	const u32 before = PackageBindings::GetCount();
	int first = 0;
	int second = 0;
	PackageBindings::Add("Probe", &first, 16, 0xabcdu);
	PackageBindings::Add("Other", &second, 8, 0x1234u);
	EXPECT_EQ(PackageBindings::GetCount(), before + 2);
	EXPECT_EQ(PackageBindings::Find("Probe", 0xabcdu, 16), &first);
	EXPECT_EQ(PackageBindings::Find("Other", 0x1234u, 8), &second);
	EXPECT_EQ(PackageBindings::GetName(before), "Probe");

	PackageBindings::Add("Probe", &second, 16, 0xabcdu);
	EXPECT_EQ(PackageBindings::GetCount(), before + 2);
	EXPECT_EQ(PackageBindings::Find("Probe", 0xabcdu, 16), &second);

	PackageBindings::Remove("Probe");
	PackageBindings::Remove("Other");
	EXPECT_EQ(PackageBindings::GetCount(), before);
	EXPECT_EQ(Refusal("Probe", 0xabcdu, 16), "Shalltear.Probe has no bindings in this engine. Enable the package");
}

TEST(CSharpPackageBindingsTest, MismatchNamesBothLayouts) {
	int table = 0;
	PackageBindings::Add("Probe", &table, 16, 0xabcdu);
	EXPECT_EQ(Refusal("Probe", 0xabceu, 16), "Shalltear.Probe doesn't match this engine (engine layout 000000000000abcd size 16, assembly layout "
											 "000000000000abce size 16). Rebuild the engine");
	EXPECT_EQ(Refusal("Probe", 0xabcdu, 24), "Shalltear.Probe doesn't match this engine (engine layout 000000000000abcd size 16, assembly layout "
											 "000000000000abcd size 24). Rebuild the engine");
	PackageBindings::Remove("Probe");
}

TEST(CSharpPackageBindingsTest, ListenerSlotFollowsItsName) {
	const i32 first = PackageBindings::ReserveListenerSlot("ProbeEvents");
	const i32 second = PackageBindings::ReserveListenerSlot("OtherEvents");
	EXPECT_NE(first, second);
	EXPECT_GE(first, 0);
	EXPECT_LT(second, 32);
	EXPECT_EQ(PackageBindings::ReserveListenerSlot("ProbeEvents"), first);
	EXPECT_EQ(PackageBindings::ReserveListenerSlot("OtherEvents"), second);
}

#endif
