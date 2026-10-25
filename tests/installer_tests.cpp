/*
Module: rtl8125.tests.installer_policy
Purpose: Check exact identity, firmware-family policy, reboot proof and path boundaries.
Created: 2026-10-24
Architecture: Portable user-mode policy checks; no simulated OS install claims.
*/
#include "../src/installer/model.hpp"
#include <cstdio>
using namespace rtl8125::installer;
namespace { unsigned count{},failed{};
void check(bool value) { ++count; if (!value) { ++failed; std::fprintf(stderr,"Installer check %u failed\n",count); } }
}
int main()
{
    check(identify({L"PCI\\VEN_10EC&DEV_8125&SUBSYS_7E271462&REV_05"})==Card::rtl8125b);
    check(identify({L"pci\\ven_10ec&dev_8125&subsys_7e271462&rev_05"})==Card::rtl8125b);
    for (const auto* id : {L"PCI\\VEN_10EC&DEV_8125",L"PCI\\VEN_10EC&DEV_8125&REV_05",
            L"PCI\\VEN_10EC&DEV_8125&SUBSYS_00001462&REV_05",L"PCI\\VEN_10EC&DEV_8125&SUBSYS_7E271462&REV_06"}) {
        check(identify({id})==Card::rtl8125);
    }
    for (const auto* id : {L"",L"USB\\VEN_10EC&DEV_8125",L"PCI\\VEN_10EC&DEV_8139",L"PCI\\VEN_10EC&DEV_81250",
            L"PCI\\VEN_10EC&DEV_8125B",L"PCI\\VEN_10EC",L"PCI\\CC_0200"}) { check(identify({id})==Card::unrelated); }
    check(std::wstring(card_name(Card::rtl8125b))==L"RTL8125B");
    check(std::wstring(card_name(Card::rtl8125))==L"RTL8125");
    for (const auto* image : {L"system32\\drivers\\ndis.sys",
            L"\\SystemRoot\\System32\\drivers\\NDIS.sys", L"%SystemRoot%\\system32\\drivers\\ndis.sys",
            L"C:\\ReactOS\\system32\\drivers\\ndis.sys", L"\\??\\C:\\ReactOS\\system32\\drivers\\ndis.sys"}) {
        check(boot_ndis_identity(image,L"C:\\ReactOS",0,1,50,200));
    }
    for (const auto* image : {L"ndis.sys", L"system32\\ndis.sys", L"system32\\drivers\\..\\ndis.sys",
            L"D:\\other\\ndis.sys", L"system32\\drivers\\ndis.sys.extra", L""}) {
        check(!boot_ndis_identity(image,L"C:\\ReactOS",0,1,50,200));
    }
    check(!boot_ndis_identity(L"system32\\drivers\\ndis.sys",L"C:\\ReactOS",1,1,50,200));
    check(!boot_ndis_identity(L"system32\\drivers\\ndis.sys",L"C:\\ReactOS",0,2,50,200));
    for (const auto stamp : std::array<std::uint64_t,3>{{0,200,201}}) {
        check(!boot_ndis_identity(L"system32\\drivers\\ndis.sys",L"C:\\ReactOS",0,1,stamp,200));
    }
    check(!boot_ndis_identity(L"system32\\drivers\\ndis.sys",L"C:\\ReactOS",0,1,50,0));
    check(!boot_ndis_identity(L"system32\\drivers\\ndis.sys",L"ReactOS",0,1,50,200));
    CompatibilityEvidence good{true,true,true,true,true,200,100,50};
    check(compatibility(good)==Compatibility::ready);
    for (unsigned bit=0;bit<5;++bit) {
        auto value=good;
        switch(bit) { case 0:value.reactos_i386=false;break; case 1:value.administrator=false;break;
            case 2:value.loaded_path_matches=false;break; case 3:value.approved_hash=false;break; case 4:value.journal_hash_matches=false;break; }
        check(compatibility(value)!=Compatibility::ready);
    }
    for (auto observed : std::array<std::uint64_t,4>{{0,200,201,UINT64_MAX}}) { auto value=good; value.observed_boot_time=observed; check(compatibility(value)==Compatibility::reboot_required); }
    auto copied=good; copied.file_write_time=201; check(compatibility(copied)==Compatibility::reboot_required);
    copied.boot_time=0; check(compatibility(copied)!=Compatibility::ready);
    for (const auto* path : {L"",L".",L"..",L"../rtl.sys",L"C:rtl.sys",L"x\\rtl.sys",L"x/rtl.sys",L"x.",L"x ",L"a\n",L"a:b"}) { check(!safe_component(path)); }
    check(!safe_component(std::wstring(L"abc\0def",7)));
    check(safe_component(L"rtl8125.sys.before"));
    for (const auto& vector : {std::pair<std::string,std::string>{"","e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
            {"abc","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
            {std::string(1000000,'a'),"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}}) {
        Sha256 hash;
        for (const auto byte : vector.first) { const auto value=static_cast<std::uint8_t>(byte); hash.update(&value,1); }
        check(hash.finish()==vector.second);
    }
    std::printf("Installer policy: %u checks, %u failures\n",count,failed);
    return failed==0?0:1;
}
