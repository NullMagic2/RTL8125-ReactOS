/*
Module: rtl8125.tests.installer_preview
Purpose: Execute real preview/Wine denial paths without permitting native installation.
Created: 2026-10-24
Architecture: Wine-only user-mode fixture linked to the actual setup backend.
*/
#include "../src/installer/backend.hpp"
#include <cstdio>
using namespace rtl8125::installer;
int main()
{
    if(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"wine_get_version")==nullptr) {
        std::puts("This preview fixture only runs in isolated Wine.");return 77;
    }
    unsigned checks{},failures{};
    const auto check=[&](bool value){++checks;if(!value){++failures;}};
    const auto preview=inspect(true);
    check(preview.preview&&preview.devices.size()==1&&preview.devices[0].card==Card::rtl8125b);
    check(!prepare_compatibility(preview).success);
    const auto result=install(preview,preview.devices[0],L"",L"",nullptr,nullptr);
    check(result.success&&result.preview&&!result.reboot);
    check(remove(preview,preview.devices[0]).preview);
    check(restore(preview,preview.devices[0]).preview);
    const auto actual=inspect(false);
    check(!actual.preview&&actual.state==Compatibility::unsupported_os);
    check(!prepare_compatibility(actual).success);
    check(!install(actual,preview.devices[0],L"",L"",nullptr,nullptr).success);
    check(!remove(actual,preview.devices[0]).success);
    check(!restore(actual,preview.devices[0]).success);
    HKEY key{};
    const auto status=RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"Software\\RTL8125ReactOS\\Installer",0,KEY_QUERY_VALUE,&key);
    if(key!=nullptr){RegCloseKey(key);}
    check(status==ERROR_FILE_NOT_FOUND);
    std::printf("Actual installer preview/Wine denial: %u checks, %u failures; no native install was attempted\n",checks,failures);
    return failures==0?0:1;
}
