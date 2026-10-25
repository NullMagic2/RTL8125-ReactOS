/*
Module: rtl8125.installer.wizard
Purpose: Native, legible device-specific setup with review, progress and recovery.
Created: 2026-10-24
Architecture: Win32/GDI user interface; backend owns actual installation and preview isolation.
*/
#include "backend.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <cwchar>

namespace rtl8125::installer {
namespace {
constexpr UINT progress_message=WM_APP+1, done_message=WM_APP+2;
constexpr int next_id=100,back_id=101,close_id=102,devices_id=103,browse_b_id=104,browse_a_id=105,
    prepare_id=106,refresh_id=107,details_id=108,remove_id=109,restore_id=110;
constexpr COLORREF navy=RGB(15,29,48), ink=RGB(24,37,53), muted=RGB(95,111,130), blue=RGB(31,104,235),
    pale=RGB(244,247,251), line=RGB(222,229,238), green=RGB(25,125,87), amber=RGB(170,101,17);
struct State {
    HWND window{},next{},back{},close{},devices{},browse_a{},browse_b{},prepare{},refresh{},details{},remove_button{},restore_button{};
    HFONT body{},small{},title{},heading{},button{};
    HBRUSH white{};
    int dpi=96,page{},selected{};
    unsigned progress{};
    bool preview{},busy{},preview_finish{};
    HANDLE thread{};
    Snapshot snapshot;
    Result result;
    std::wstring firmware_a,firmware_b,capture;
};
int scale(const State& state,int value) { return MulDiv(value,state.dpi,96); }
RECT rect(const State& state,int x,int y,int width,int height) { return {scale(state,x),scale(state,y),scale(state,x+width),scale(state,y+height)}; }
void fill(HDC dc,const RECT& area,COLORREF color)
{
    const auto brush=CreateSolidBrush(color);
    FillRect(dc,&area,brush);
    DeleteObject(brush);
}
void rounded(HDC dc,const RECT& area,COLORREF color,COLORREF border,int radius)
{
    const auto brush=CreateSolidBrush(color);
    const auto pen=CreatePen(PS_SOLID,1,border);
    const auto old_brush=SelectObject(dc,brush),old_pen=SelectObject(dc,pen);
    RoundRect(dc,area.left,area.top,area.right,area.bottom,radius,radius);
    SelectObject(dc,old_brush);SelectObject(dc,old_pen);
    DeleteObject(brush);DeleteObject(pen);
}
void text(HDC dc,const State& state,const std::wstring& value,int x,int y,int width,int height,
    HFONT font,COLORREF color=ink,UINT flags=DT_LEFT|DT_WORDBREAK|DT_NOPREFIX)
{
    auto area=rect(state,x,y,width,height);
    const auto old=SelectObject(dc,font);
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);
    DrawTextW(dc,value.c_str(),static_cast<int>(value.size()),&area,flags);
    SelectObject(dc,old);
}
HFONT font(int size,int weight,int dpi,const wchar_t* name=L"Segoe UI")
{
    return CreateFontW(-MulDiv(size,dpi,72),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,name);
}
bool has_device(const State& state) { return state.selected>=0 && static_cast<std::size_t>(state.selected)<state.snapshot.devices.size(); }
bool firmware_ready(const State& state,bool b)
{
    if (state.preview) { return true; }
    const auto& selected=b?state.firmware_b:state.firmware_a;
    return firmware_valid(selected.empty()?installed_firmware(state.snapshot,b):selected);
}
bool can_install(const State& state)
{
    return has_device(state) && state.snapshot.state==Compatibility::ready && state.snapshot.payload_valid &&
        firmware_ready(state,true) && (state.snapshot.devices[state.selected].card==Card::rtl8125b || firmware_ready(state,false));
}
void set_button(HWND control,const wchar_t* label,bool enabled=true)
{
    SetWindowTextW(control,label);EnableWindow(control,enabled);
}
void update(State& state)
{
    const bool device=has_device(state),requirements=state.page==1;
    const auto product=device?card_name(state.snapshot.devices[state.selected].card):L"RTL8125";
#if defined(RTL8125_TRUSTED_REACTOS_GUEST)
    const auto caption=std::wstring(product)+(state.preview?L" Setup — Preview":L" Setup — Controlled VM");
#else
    const auto caption=std::wstring(product)+(state.preview?L" Setup — Preview":L" Setup");
#endif
    SetWindowTextW(state.window,caption.c_str());
    ShowWindow(state.devices,state.page==0?SW_SHOW:SW_HIDE);
    ShowWindow(state.details,state.page==0&&device?SW_SHOW:SW_HIDE);
    ShowWindow(state.refresh,state.page==0?SW_SHOW:SW_HIDE);
    ShowWindow(state.browse_b,requirements?SW_SHOW:SW_HIDE);
    ShowWindow(state.browse_a,requirements&&device&&state.snapshot.devices[state.selected].card!=Card::rtl8125b?SW_SHOW:SW_HIDE);
    ShowWindow(state.prepare,requirements&&state.snapshot.state==Compatibility::reboot_required?SW_SHOW:SW_HIDE);
    ShowWindow(state.remove_button,requirements&&device&&_wcsicmp(state.snapshot.devices[state.selected].service.c_str(),L"Rtl8125")==0?SW_SHOW:SW_HIDE);
    const bool recovery_available=!state.preview&&device&&_wcsicmp(state.snapshot.devices[state.selected].service.c_str(),L"Rtl8125")==0&&state.snapshot.state==Compatibility::ready;
    ShowWindow(state.restore_button,((state.page==4&&!state.result.preview&&!state.result.success)||(requirements&&recovery_available))?SW_SHOW:SW_HIDE);
    EnableWindow(state.back,state.page>0&&state.page<3&&!state.busy);
    EnableWindow(state.close,!state.busy);
    set_button(state.close,state.page==4?L"Close":L"Cancel",!state.busy);
    if (state.page==0) { set_button(state.next,L"Continue",device); }
    if (state.page==1) { set_button(state.next,L"Review installation",can_install(state)); }
    if (state.page==2) { set_button(state.next,state.preview?L"Preview installation":L"Install driver",can_install(state)); }
    if (state.page==3) { set_button(state.next,L"Installing...",false); }
    if (state.page==4) { set_button(state.next,L"Done"); }
    InvalidateRect(state.window,nullptr,TRUE);
}
void enumerate(State& state)
{
    state.snapshot=inspect(state.preview);
    state.selected=state.snapshot.devices.empty()?-1:0;
    SendMessageW(state.devices,LB_RESETCONTENT,0,0);
    for (const auto& device:state.snapshot.devices) {
        const auto name=std::wstring(card_name(device.card))+L"   |   PCI Express Ethernet";
        SendMessageW(state.devices,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));
    }
    SendMessageW(state.devices,LB_SETCURSEL,state.selected,0);
    update(state);
}
void checkmark(HDC dc,const State& state,int x,int y,COLORREF color)
{
    const auto pen=CreatePen(PS_SOLID,std::max(1,scale(state,3)),color);
    const auto previous=SelectObject(dc,pen);
    MoveToEx(dc,scale(state,x),scale(state,y+8),nullptr);
    LineTo(dc,scale(state,x+6),scale(state,y+14));
    LineTo(dc,scale(state,x+18),scale(state,y));
    SelectObject(dc,previous);DeleteObject(pen);
}
void status(HDC dc,const State& state,int y,const wchar_t* label,const std::wstring& detail,bool ready,int progress_state=-1)
{
    rounded(dc,rect(state,290,y,700,72),RGB(255,255,255),line,scale(state,12));
    const auto background=ready?RGB(231,246,238):progress_state==0?RGB(241,244,248):progress_state==1?RGB(231,239,255):RGB(255,245,227);
    rounded(dc,rect(state,308,y+19,30,30),background,background,scale(state,30));
    if(ready){checkmark(dc,state,314,y+26,green);}
    else if(progress_state>=0){const auto center=rect(state,318,y+29,10,10);rounded(dc,center,progress_state==1?blue:RGB(183,195,209),progress_state==1?blue:RGB(183,195,209),scale(state,10));}
    else{text(dc,state,L"!",309,y+21,27,25,state.heading,amber,DT_CENTER|DT_SINGLELINE|DT_NOPREFIX);}
    text(dc,state,label,352,y+12,602,22,state.heading);
    text(dc,state,detail,352,y+38,610,24,state.small,muted,DT_LEFT|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
}
void paint(State& state,HDC dc)
{
    fill(dc,rect(state,0,0,1040,720),pale);
    for (int y=0;y<720;++y) {
        fill(dc,rect(state,0,y,238,1),RGB(15+y/100,29+y/60,48+y/40));
    }
    rounded(dc,rect(state,30,35,42,42),blue,blue,scale(state,12));
    text(dc,state,L"R",30,39,42,34,state.title,RGB(255,255,255),DT_CENTER|DT_SINGLELINE);
    text(dc,state,has_device(state)?card_name(state.snapshot.devices[state.selected].card):L"RTL8125",85,37,135,28,state.heading,RGB(255,255,255));
    text(dc,state,L"Driver setup",85,62,135,25,state.small,RGB(166,186,210));
    const wchar_t* steps[]={L"Find your card",L"Requirements",L"Review & install"};
    const int active=state.page==0?0:state.page==1?1:2;
    for (int step=0;step<3;++step) {
        const auto color=step==active?RGB(255,255,255):RGB(156,177,202);
        if (step==active) { rounded(dc,rect(state,18,150+step*64,202,51),RGB(41,61,86),RGB(41,61,86),scale(state,12)); }
        text(dc,state,std::to_wstring(step+1),32,162+step*64,22,30,state.heading,color);
        text(dc,state,steps[step],66,164+step*64,148,30,state.body,color);
    }
    // Code-native illustration of the controller and an Ethernet jack.
    const auto pen=CreatePen(PS_SOLID,scale(state,2),RGB(105,139,177));
    const auto old=SelectObject(dc,pen);
    const auto old_brush=SelectObject(dc,GetStockObject(NULL_BRUSH));
    auto board=rect(state,48,457,142,100);
    RoundRect(dc,board.left,board.top,board.right,board.bottom,scale(state,10),scale(state,10));
    auto chip=rect(state,67,477,46,43);Rectangle(dc,chip.left,chip.top,chip.right,chip.bottom);
    auto port=rect(state,135,480,32,38);Rectangle(dc,port.left,port.top,port.right,port.bottom);
    for (int pin=0;pin<5;++pin) {
        MoveToEx(dc,scale(state,72+pin*8),scale(state,525),nullptr);LineTo(dc,scale(state,72+pin*8),scale(state,536));
    }
    SelectObject(dc,old);SelectObject(dc,old_brush);DeleteObject(pen);
    const wchar_t* titles[]={L"Let’s get you connected.",L"A quick readiness check.",L"",L"Setting up your driver.",L"Setup complete."};
    if (state.page != 2) {
        text(dc,state,state.page==4&&!state.result.success?L"Setup needs your attention.":titles[state.page],290,77,710,58,state.title);
    }
    const wchar_t* descriptions[]={L"Choose the Ethernet card you want to set up.",L"We’ll check the system before changing your selected card.",
        L"Review the changes, then install when you’re ready.",L"Keep this window open while ReactOS configures your card.",L"Your setup result and recovery information are below."};
    text(dc,state,descriptions[state.page],292,state.page==2?77:139,700,42,state.body,muted);
    if (state.page==0) {
        text(dc,state,L"YOUR NETWORK CARD",292,197,640,22,state.small,muted);
        if (!has_device(state)) {
            text(dc,state,L"No RTL8125 card found",310,252,620,35,state.heading);
            text(dc,state,L"Connect a dedicated supported card to this ReactOS machine, then refresh.",310,297,620,56,state.body,muted);
        }
        rounded(dc,rect(state,290,395,700,145),RGB(255,255,255),line,scale(state,14));
        text(dc,state,L"Only the card you choose",313,419,640,28,state.heading);
        text(dc,state,L"Setup matches the actual PCI device and leaves other network adapters alone. Your card’s name will also appear correctly in Device Manager.",313,458,640,68,state.body,muted);
        if(!state.snapshot.issue.empty()&&(state.snapshot.state==Compatibility::unsupported_os||state.snapshot.state==Compatibility::administrator_required)) {
            text(dc,state,state.snapshot.issue,292,567,695,64,state.body,amber);
        }
    } else if (state.page==1) {
        status(dc,state,190,L"ReactOS i386 with administrator access",state.preview?L"Sample compatible system":state.snapshot.evidence.reactos_i386&&state.snapshot.evidence.administrator?L"Ready":L"Requires a ReactOS i386 administrator session",state.snapshot.evidence.reactos_i386&&state.snapshot.evidence.administrator);
        status(dc,state,274,L"Compatible network core",state.snapshot.state==Compatibility::ready?L"Approved image verified across a reboot":state.snapshot.state==Compatibility::reboot_required?L"Record this image, restart ReactOS, then reopen setup":L"A compatible patched ReactOS image is required",state.snapshot.state==Compatibility::ready);
        status(dc,state,358,L"Driver package",state.snapshot.payload_valid?L"Driver and device-specific INF verified":L"Keep setup beside the matching rtl8125.sys and rtl8125.inf",state.snapshot.payload_valid);
        const bool b=has_device(state)&&state.snapshot.devices[state.selected].card==Card::rtl8125b;
        status(dc,state,442,b?L"RTL8125B firmware":L"RTL8125A and RTL8125B firmware",b?(firmware_ready(state,true)?L"Uncompressed firmware is ready":L"Choose rtl8125b-2.fw from a trusted Realtek firmware package"):
            L"Choose rtl8125a-3.fw and rtl8125b-2.fw; PCI ID alone cannot select the MAC family",firmware_ready(state,true)&&(b||firmware_ready(state,false)));
        if (!state.snapshot.issue.empty()) { text(dc,state,state.snapshot.issue,292,607,695,44,state.small,muted); }
    } else if (state.page==2) {
        rounded(dc,rect(state,290,132,700,315),RGB(255,255,255),line,scale(state,14));
        const auto label=has_device(state)?card_name(state.snapshot.devices[state.selected].card):L"RTL8125";
        text(dc,state,label,316,156,640,40,state.title);
        text(dc,state,L"Device Manager name",318,205,310,25,state.small,muted);
        text(dc,state,label,684,205,280,25,state.heading);
        text(dc,state,L"Driver provider",318,247,310,25,state.small,muted);
        text(dc,state,L"RTL8125 ReactOS development",584,247,380,25,state.body);
        text(dc,state,L"Install scope",318,289,260,25,state.small,muted);
        text(dc,state,L"Selected adapter only",684,289,280,25,state.body);
        text(dc,state,L"Recovery",318,331,260,25,state.small,muted);
        text(dc,state,L"Original files and binding recorded",584,331,380,25,state.body);
#if defined(RTL8125_TRUSTED_REACTOS_GUEST)
        text(dc,state,L"Controlled VM build: this ReactOS version cannot protect registry journals. Use only in a trusted test guest.",318,385,640,50,state.small,muted);
#else
        text(dc,state,L"Setup includes an experimental driver. It does not replace core OS files or download firmware.",318,385,640,50,state.small,muted);
#endif
        text(dc,state,L"A recovery folder is saved before changes. You can restore a previous binding when supported, or recover the stopped guest from its saved files.",292,477,690,65,state.body,muted);
    } else if (state.page==3) {
        const wchar_t* labels[]={L"Verify system and package",L"Preserve files and prepare firmware",L"Bind the selected network card",L"Verify its name and device status"};
        for (unsigned index=0;index<4;++index) {
            status(dc,state,195+static_cast<int>(index)*86,labels[index],index+1<state.progress?L"Complete":index+1==state.progress?L"In progress…":L"Waiting",index+1<state.progress,index+1<state.progress?2:index+1==state.progress?1:0);
        }
    } else {
        rounded(dc,rect(state,290,196,700,290),RGB(255,255,255),line,scale(state,14));
        if(state.result.success){checkmark(dc,state,329,234,green);}else{text(dc,state,L"!",316,216,65,65,state.title,amber);}
        text(dc,state,state.result.preview?L"Preview complete":state.result.success?L"Your driver is configured":L"Check the details below",386,225,565,40,state.heading);
        text(dc,state,state.result.message,316,290,640,153,state.body,muted);
        if (!state.result.recovery.empty()) {
            text(dc,state,L"RECOVERY FILES",292,520,680,25,state.small,muted);
            text(dc,state,state.result.recovery,292,552,695,55,state.small,muted);
        }
    }
    fill(dc,rect(state,238,655,802,65),RGB(255,255,255));
    fill(dc,rect(state,238,655,802,1),line);
}
void browse(State& state,bool b)
{
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=state.window;
    dialog.lpstrTitle=b?L"Choose rtl8125b-2.fw":L"Choose rtl8125a-3.fw";
    dialog.lpstrFilter=b?L"RTL8125B firmware\0rtl8125b-2.fw\0\0":L"RTL8125A firmware\0rtl8125a-3.fw\0\0";
    dialog.lpstrFile=path;dialog.nMaxFile=MAX_PATH;dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) {
        if (!firmware_valid(path)) { MessageBoxW(state.window,L"Choose an uncompressed valid firmware file from a trusted package.",L"Firmware not ready",MB_OK|MB_ICONWARNING); return; }
        (b?state.firmware_b:state.firmware_a)=path;
        update(state);
    }
}
void report_progress(void* context,unsigned step)
{
    PostMessageW(static_cast<State*>(context)->window,progress_message,step,0);
}
DWORD WINAPI worker(void* context)
{
    auto& state=*static_cast<State*>(context);
    state.result=install(state.snapshot,state.snapshot.devices[state.selected],state.firmware_a,state.firmware_b,report_progress,&state);
    PostMessageW(state.window,done_message,0,0);
    return 0;
}
void capture(State& state)
{
    if (!state.preview||state.capture.empty()) { return; }
    RECT client{};GetClientRect(state.window,&client);
    const auto dc=GetDC(state.window),memory=CreateCompatibleDC(dc);
    const auto bitmap=CreateCompatibleBitmap(dc,client.right,client.bottom);
    const auto old=SelectObject(memory,bitmap);
    // Capture this visible client, including its native child controls.
    BitBlt(memory,0,0,client.right,client.bottom,dc,0,0,SRCCOPY);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=client.right;
    info.bmiHeader.biHeight=client.bottom;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    std::vector<BYTE> pixels(static_cast<std::size_t>(client.right)*client.bottom*4);
    SelectObject(memory,old);
    if (GetDIBits(dc,bitmap,0,client.bottom,pixels.data(),&info,DIB_RGB_COLORS)) {
        BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+static_cast<DWORD>(pixels.size());
        const auto file=CreateFileW(state.capture.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (file!=INVALID_HANDLE_VALUE) { DWORD wrote{};WriteFile(file,&header,sizeof(header),&wrote,nullptr);WriteFile(file,&info.bmiHeader,sizeof(info.bmiHeader),&wrote,nullptr);WriteFile(file,pixels.data(),static_cast<DWORD>(pixels.size()),&wrote,nullptr);CloseHandle(file); }
    }
    DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(state.window,dc);
}
HWND control(State& state,const wchar_t* type,const wchar_t* label,int id,int x,int y,int width,int height,DWORD style=0)
{
    const auto area=rect(state,x,y,width,height);
    const auto value=CreateWindowExW(0,type,label,WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,area.left,area.top,
        area.right-area.left,area.bottom-area.top,state.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(value,WM_SETFONT,reinterpret_cast<WPARAM>(state.body),TRUE);
    return value;
}
LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)
{
    auto* state=reinterpret_cast<State*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message==WM_NCCREATE) { state=static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);state->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state)); }
    if (state==nullptr) { return DefWindowProcW(window,message,wparam,lparam); }
    switch(message) {
    case WM_CREATE:
        state->body=font(12,FW_NORMAL,state->dpi);state->small=font(10,FW_NORMAL,state->dpi);
        state->title=font(25,FW_SEMIBOLD,state->dpi);state->heading=font(12,FW_SEMIBOLD,state->dpi);state->button=font(11,FW_SEMIBOLD,state->dpi);
        state->white=CreateSolidBrush(RGB(255,255,255));
        state->next=control(*state,L"BUTTON",L"Continue",next_id,794,672,195,35,BS_OWNERDRAW);
        state->back=control(*state,L"BUTTON",L"Back",back_id,648,672,128,35,BS_OWNERDRAW);
        state->close=control(*state,L"BUTTON",L"Cancel",close_id,291,672,118,35,BS_OWNERDRAW);
        state->devices=control(*state,L"LISTBOX",L"",devices_id,291,235,698,105,LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL|WS_BORDER);
        SendMessageW(state->devices,LB_SETITEMHEIGHT,0,scale(*state,40));
        state->refresh=control(*state,L"BUTTON",L"Refresh",refresh_id,870,351,118,32,BS_OWNERDRAW);
        state->details=control(*state,L"BUTTON",L"Hardware details",details_id,292,351,175,32,BS_OWNERDRAW);
        state->browse_b=control(*state,L"BUTTON",L"Choose B firmware",browse_b_id,788,528,200,32,BS_OWNERDRAW);
        state->browse_a=control(*state,L"BUTTON",L"Choose A firmware",browse_a_id,574,528,200,32,BS_OWNERDRAW);
        state->prepare=control(*state,L"BUTTON",L"Record image & restart later",prepare_id,291,568,337,32,BS_OWNERDRAW);
        state->remove_button=control(*state,L"BUTTON",L"Remove selected driver",remove_id,291,528,251,32,BS_OWNERDRAW);
        state->restore_button=control(*state,L"BUTTON",L"Restore previous binding",restore_id,291,602,281,34,BS_OWNERDRAW);
        enumerate(*state);
        if (state->preview&&state->page==4) { state->result={true,false,true,0,L"Preview complete. No driver, firmware or registry settings were changed.",{}};update(*state); }
        if (!state->capture.empty()) { SetTimer(window,9,600,nullptr); }
        return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT: { PAINTSTRUCT ps{};const auto dc=BeginPaint(window,&ps);paint(*state,dc);EndPaint(window,&ps);return 0; }
    case WM_PRINTCLIENT:paint(*state,reinterpret_cast<HDC>(wparam));return 0;
    case WM_CTLCOLORLISTBOX:SetBkColor(reinterpret_cast<HDC>(wparam),RGB(255,255,255));SetTextColor(reinterpret_cast<HDC>(wparam),ink);return reinterpret_cast<LRESULT>(state->white);
    case WM_DRAWITEM: {
        const auto& item=*reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        const bool primary=item.CtlID==next_id,disabled=(item.itemState&ODS_DISABLED)!=0,pressed=(item.itemState&ODS_SELECTED)!=0;
        const auto background=disabled?RGB(233,238,246):primary?(pressed?RGB(24,80,182):blue):RGB(255,255,255);
        rounded(item.hDC,item.rcItem,background,primary?background:line,scale(*state,10));
        wchar_t label[128]{};GetWindowTextW(item.hwndItem,label,128);
        auto area=item.rcItem;const auto old=SelectObject(item.hDC,state->button);
        SetBkMode(item.hDC,TRANSPARENT);SetTextColor(item.hDC,disabled?RGB(145,158,177):primary?RGB(255,255,255):ink);
        DrawTextW(item.hDC,label,-1,&area,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        if ((item.itemState&ODS_FOCUS)!=0) { InflateRect(&area,-4,-4);DrawFocusRect(item.hDC,&area); }
        SelectObject(item.hDC,old);return TRUE;
    }
    case WM_COMMAND:
        switch(LOWORD(wparam)) {
        case devices_id:if(HIWORD(wparam)==LBN_SELCHANGE){state->selected=static_cast<int>(SendMessageW(state->devices,LB_GETCURSEL,0,0));update(*state);}break;
        case refresh_id:enumerate(*state);break;
        case browse_a_id:browse(*state,false);break;
        case browse_b_id:browse(*state,true);break;
        case details_id:if(has_device(*state)){const auto& device=state->snapshot.devices[state->selected];std::wstring details=std::wstring(card_name(device.card))+L"\n\nManufacturer: Realtek\n\n";for(const auto& id:device.hardware_ids){details+=id+L"\n";}details+=L"\nInstance: "+device.instance;MessageBoxW(window,details.c_str(),L"Hardware details",MB_OK|MB_ICONINFORMATION);}break;
        case prepare_id:{const auto result=prepare_compatibility(state->snapshot);MessageBoxW(window,result.message.c_str(),L"Compatibility check",MB_OK|(result.success?MB_ICONINFORMATION:MB_ICONWARNING));enumerate(*state);break;}
        case remove_id:if(has_device(*state)&&MessageBoxW(window,L"Disable and attempt removal of only the selected RTL8125 driver? Shared files and recovery backups will be preserved.",L"Remove selected driver",MB_YESNO|MB_ICONQUESTION)==IDYES){state->result=remove(state->snapshot,state->snapshot.devices[state->selected]);state->page=4;update(*state);}break;
        case restore_id:if(has_device(*state)&&MessageBoxW(window,L"Stop this adapter and attempt to restore its recorded previous driver binding? Recovery files remain available.",L"Restore previous binding",MB_YESNO|MB_ICONQUESTION)==IDYES){state->result=restore(state->snapshot,state->snapshot.devices[state->selected]);update(*state);}break;
        case back_id:if(state->page>0&&state->page<3){--state->page;update(*state);}break;
        case next_id:
            if(state->page==4){SendMessageW(window,WM_CLOSE,0,0);break;}
            if(state->page==0&&has_device(*state)){state->page=1;update(*state);break;}
            if(state->page==1&&can_install(*state)){state->page=2;update(*state);break;}
            if(state->page==2&&can_install(*state)){state->page=3;state->progress=1;state->busy=true;update(*state);state->thread=CreateThread(nullptr,0,worker,state,0,nullptr);if(state->thread==nullptr){state->busy=false;state->page=4;state->result={false,false,false,GetLastError(),L"Setup could not start. Close it and try again.",{}};update(*state);}}break;
        case close_id:SendMessageW(window,WM_CLOSE,0,0);break;
        }return 0;
    case progress_message:state->progress=static_cast<unsigned>(wparam);InvalidateRect(window,nullptr,FALSE);return 0;
    case done_message:WaitForSingleObject(state->thread,INFINITE);CloseHandle(state->thread);state->thread=nullptr;state->busy=false;state->page=4;update(*state);return 0;
    case WM_TIMER:if(wparam==9){KillTimer(window,9);capture(*state);}return 0;
    case WM_CLOSE:if(!state->busy){DestroyWindow(window);}return 0;
    case WM_DESTROY:
        for(const auto value:{state->body,state->small,state->title,state->heading,state->button}){DeleteObject(value);}
        DeleteObject(state->white);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show)
{
    using namespace rtl8125::installer;
    State state;
    int argc{};
    auto** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argv!=nullptr){for(int index=1;index<argc;++index){const std::wstring arg=argv[index];if(arg==L"--preview"){state.preview=true;}
        if(arg.compare(0,15,L"--preview-page=")==0){const auto page=arg.substr(15);const wchar_t* names[]={L"device",L"requirements",L"review",L"progress",L"finish"};for(int value=0;value<5;++value){if(page==names[value]){state.page=value;}}}
        if(arg.compare(0,10,L"--capture=")==0){state.capture=arg.substr(10);}}
        LocalFree(argv);}
    if(!state.preview){state.page=0;state.capture.clear();}
    state.progress=state.page==3?3:0;
    const auto awareness=reinterpret_cast<BOOL (WINAPI*)()>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetProcessDPIAware"));
    if(awareness!=nullptr){awareness();}
    const auto dc=GetDC(nullptr);if(dc!=nullptr){state.dpi=GetDeviceCaps(dc,LOGPIXELSX);ReleaseDC(nullptr,dc);}
    state.dpi=std::max(96,std::min(state.dpi,192));
    RECT work{};
    if(!SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0)){work={0,0,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN)};}
    // Preserve the layout coherently on ReactOS's common 800x600 desktops;
    // include caption/frame space so the bottom actions remain reachable.
    while(state.dpi>48){auto fitted=rect(state,0,0,1040,720);AdjustWindowRect(&fitted,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE);
        if(fitted.right-fitted.left<=work.right-work.left-16&&fitted.bottom-fitted.top<=work.bottom-work.top-16){break;}--state.dpi;}
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_PROGRESS_CLASS};InitCommonControlsEx(&controls);
    WNDCLASSW type{};type.lpfnWndProc=procedure;type.hInstance=instance;type.lpszClassName=L"RTL8125SetupWindow";
    type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    if(!RegisterClassW(&type)){return 1;}
    auto area=rect(state,0,0,1040,720);AdjustWindowRect(&area,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,FALSE);
    const auto width=area.right-area.left,height=area.bottom-area.top;
    const auto window=CreateWindowExW(0,type.lpszClassName,state.preview?L"RTL8125B Setup — Preview":L"RTL8125 Network Driver Setup",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,work.left+(work.right-work.left-width)/2,
        work.top+(work.bottom-work.top-height)/2,width,height,nullptr,nullptr,instance,&state);
    if(window==nullptr){return 1;}
    ShowWindow(window,show);UpdateWindow(window);
    MSG message{};
    while(GetMessageW(&message,nullptr,0,0)>0){if(!IsDialogMessageW(window,&message)){TranslateMessage(&message);DispatchMessageW(&message);}}
    return static_cast<int>(message.wParam);
}
