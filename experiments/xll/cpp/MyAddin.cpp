#include <windows.h>
#include <libloaderapi.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <sstream>

#include "external/EXCELSDK/include/XLCALL.H"
#include "generated/rpc_generated.h" 
#include "logger.h"
#include "PascalString.h"
#include "shm/DirectHost.h" 

#pragma comment(lib, "user32.lib")

using namespace shm;

// --- Globals ---
static DirectHost* g_directHost = nullptr;
static PROCESS_INFORMATION g_goServerProcess = {0};
static std::atomic<uint32_t> g_thread_idx_counter{0};
static thread_local uint32_t g_tls_thread_idx = 0xFFFFFFFF;

// --- Function Prototypes ---
static bool LaunchGoServer();
static void ShutdownGoServer();
static std::wstring GetXllPath();

// --- XLL Interface Functions ---
extern "C" __declspec(dllexport) int WINAPI xlAutoOpen(void) {
    std::wstring xllPath = GetXllPath();
    std::wstring logDir = xllPath.substr(0, xllPath.find_last_of(L"\/"));
    Logger::GetInstance().Initialize(logDir + L"\\MyXLL_ZeroCopy.log");
    LOG(L"xlAutoOpen called.");

    static XLOPER12 xDll;
    Excel12(xlGetName, 0, 1, (LPXLOPER12)&xDll);

    Excel12(xlfRegister, 0, 8, (LPXLOPER12)&xDll, TempStr12(L"MYADD"), TempStr12(L"BB$"),
            TempStr12(L"MYADD"), TempStr12(L"Num1,Num2"), TempStr12(L"1"),
            TempStr12(L"MyCategory"), TempStr12(L""),
            TempStr12(L"Adds two numbers via Go server"));

    Excel12(xlfRegister, 0, 7, (LPXLOPER12)&xDll, TempStr12(L"MYRAND"),
            TempStr12(L"B$"), TempStr12(L"MYRAND"), TempStr12(L"    "),
            TempStr12(L"1"), TempStr12(L"MyCategory"),
            TempStr12(L"Returns a random number from Go server"));
    
    Excel12(xlFree, 0, 1, &xDll);

    g_directHost = new DirectHost();
    if (!g_directHost->Init("ZeroCopyAddInShm", std::thread::hardware_concurrency())) {
        LOG(L"Failed to initialize DirectHost shared memory.");
        delete g_directHost;
        g_directHost = nullptr;
        return 0;
    }
    LOG(L"DirectHost initialized.");

    if (!LaunchGoServer()) {
        ShutdownGoServer(); // Clean up partial initialization
        MessageBoxW(NULL, L"Failed to launch Go server.", L"MyXLL", MB_ICONERROR);
        return 0;
    }

    LOG(L"xlAutoOpen completed successfully.");
    return 1;
}

extern "C" __declspec(dllexport) int WINAPI xlAutoClose(void) {
    LOG(L"xlAutoClose called.");
    ShutdownGoServer();

    if (g_directHost) {
        g_directHost->Shutdown();
        delete g_directHost;
        g_directHost = nullptr;
        LOG(L"DirectHost shut down.");
    }

    // It's good practice to unregister, although Excel does it on close.
    static XLOPER12 xDll;
    Excel12(xlGetName, 0, 1, &xDll);
    Excel12(xlfUnregister, 0, 1, TempStr12(L"MYADD"));
    Excel12(xlfUnregister, 0, 1, TempStr12(L"MYRAND"));
    Excel12(xlFree, 0, 1, &xDll);

    return 1;
}

extern "C" __declspec(dllexport) LPXLOPER12 WINAPI xlAddInManagerInfo(LPXLOPER12 pxAction) {
    static XLOPER12 xInfo;
    if (pxAction->xltype == xltypeInt && pxAction->val.w == 1) {
        xInfo.xltype = xltypeStr;
        xInfo.val.str = TempStr12(L"\020My Zero-Copy Add-in");
    } else {
        xInfo.xltype = xltypeErr;
        xInfo.val.err = xlerrValue;
    }
    return &xInfo;
}

// --- Custom Functions ---

LPXLOPER12 WINAPI MYADD(double num1, double num2) {
    static thread_local XLOPER12 xResult;
    xResult.xltype = xltypeErr;
    xResult.val.err = xlerrNA;

    if (!g_directHost) { return &xResult; }

    try {
        auto slot = g_directHost->GetZeroCopySlot();
        if (!slot.IsValid()) {
            LOG(L"MYADD: Failed to acquire Zero-Copy slot.");
            return &xResult;
        }

        flatbuffers::FlatBufferBuilder builder(0, slot.GetReqBuffer(), slot.GetMaxReqSize());
        
        auto addReq = MyIPC::CreateAddRequest(builder, num1, num2);
        
        MyIPC::RequestBuilder req_builder(builder);
        req_builder.add_body_type(MyIPC::RequestBody_AddRequest);
        req_builder.add_body(addReq.Union());
        auto req_offset = req_builder.Finish();

        MyIPC::RootBuilder root_builder(builder);
        root_builder.add_message_type(MyIPC::Message_Request);
        root_builder.add_message(req_offset.Union());
        auto root_offset = root_builder.Finish();

        builder.Finish(root_offset);

        slot.SendFlatBuffer(builder.GetSize());

        auto* respRoot = flatbuffers::GetRoot<MyIPC::Root>(slot.GetRespBuffer());
        if (respRoot && respRoot->message_type() == MyIPC::Message_Response) {
            auto* response = respRoot->message_as_Response();
            if (response && response->body_type() == MyIPC::ResponseBody_AddResponse) {
                auto* addResp = response->body_as_AddResponse();
                xResult.xltype = xltypeNum;
                xResult.val.num = addResp->result();
            }
        }
    } catch (const std::exception& e) {
        std::string err = e.what();
        LOG(L"MYADD: Exception - " + std::wstring(err.begin(), err.end()));
    }
    return &xResult;
}

LPXLOPER12 WINAPI MYRAND(void) {
    static thread_local XLOPER12 xResult;
    xResult.xltype = xltypeErr;
    xResult.val.err = xlerrNA;

    if (!g_directHost) { return &xResult; }

    try {
        auto slot = g_directHost->GetZeroCopySlot();
        if (!slot.IsValid()) {
            LOG(L"MYRAND: Failed to acquire Zero-Copy slot.");
            return &xResult;
        }

        flatbuffers::FlatBufferBuilder builder(0, slot.GetReqBuffer(), slot.GetMaxReqSize());

        auto randReq = MyIPC::CreateRandRequest(builder);

        MyIPC::RequestBuilder req_builder(builder);
        req_builder.add_body_type(MyIPC::RequestBody_RandRequest);
        req_builder.add_body(randReq.Union());
        auto req_offset = req_builder.Finish();

        MyIPC::RootBuilder root_builder(builder);
        root_builder.add_message_type(MyIPC::Message_Request);
        root_builder.add_message(req_offset.Union());
        auto root_offset = root_builder.Finish();

        builder.Finish(root_offset);
        
        slot.SendFlatBuffer(builder.GetSize());
        
        const auto* respRoot = flatbuffers::GetRoot<MyIPC::Root>(slot.GetRespBuffer());
        if (respRoot && respRoot->message_type() == MyIPC::Message_Response) {
            const auto* response = respRoot->message_as_Response();
            if (response && response->body_type() == MyIPC::ResponseBody_RandResponse) {
                const auto* randResp = response->body_as_RandResponse();
                xResult.xltype = xltypeNum;
                xResult.val.num = randResp->result();
            }
        }
    } catch (const std::exception& e) {
        std::string err = e.what();
        LOG(L"MYRAND: Exception - " + std::wstring(err.begin(), err.end()));
    }
    return &xResult;
}


// --- Helper Implementations ---

static std::wstring GetXLLPath() {
    wchar_t path[MAX_PATH] = {0};
    HMODULE hModule = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&GetXLLPath, &hModule)) {
        GetModuleFileNameW(hModule, path, MAX_PATH);
    }
    return std::wstring(path);
}

static bool LaunchGoServer() {
    std::wstring xllPath = GetXLLPath();
    std::wstring dirPath = xllPath.substr(0, xllPath.find_last_of(L"\/"));
    std::wstring serverPath = dirPath + L"\\go-server.exe";

    STARTUPINFOW si = { sizeof(si) };
    
    std::wstring cmdLine = L"\"" + serverPath + L"\" " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(std::thread::hardware_concurrency());

    LOG(L"Launching Go server with cmd: " + cmdLine);

    if (!CreateProcessW(NULL, &cmdLine[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dirPath.c_str(), &si, &g_goServerProcess)) {
        LOG(L"CreateProcessW for Go server failed. Error: " + std::to_wstring(GetLastError()));
        return false;
    }
    
    CloseHandle(g_goServerProcess.hThread);
    LOG(L"Go server launched. PID: " + std::to_wstring(g_goServerProcess.dwProcessId));
    return true;
}

static void ShutdownGoServer() {
    if (g_goServerProcess.hProcess) {
        LOG(L"Terminating Go server process.");
        TerminateProcess(g_goServerProcess.hProcess, 0);
        CloseHandle(g_goServerProcess.hProcess);
        g_goServerProcess = {0};
    }
}
