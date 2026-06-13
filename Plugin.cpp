#include <Windows.h>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <regex>
#include <fstream>
#include <shlobj.h>

typedef void* LPRAINMETER;
typedef void* (*RmReadStringFunc)(LPRAINMETER rm, LPCWSTR option, LPCWSTR defValue, BOOL replaceMeasures);
typedef double (*RmReadFormulaFunc)(LPRAINMETER rm, LPCWSTR option, double defValue);
typedef void (*RmLogFunc)(LPRAINMETER rm, int level, LPCWSTR message);

static HMODULE g_rainmeter = nullptr;
static RmReadStringFunc RmReadString = nullptr;
static RmReadFormulaFunc RmReadFormula = nullptr;
static RmLogFunc RmLog = nullptr;

static void LoadRainmeterFunctions()
{
    if (g_rainmeter) return;
    g_rainmeter = GetModuleHandleW(L"Rainmeter.dll");
    if (g_rainmeter)
    {
        RmReadString = (RmReadStringFunc)GetProcAddress(g_rainmeter, "RmReadString");
        RmReadFormula = (RmReadFormulaFunc)GetProcAddress(g_rainmeter, "RmReadFormula");
        RmLog = (RmLogFunc)GetProcAddress(g_rainmeter, "RmLog");
    }
}

static std::wstring GetCachePath()
{
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path)))
        return std::wstring(path) + L"\\Rainmeter\\HeadsetBatteryCache.txt";
    return L"C:\\HeadsetBatteryCache.txt";
}

static double LoadCachedLevel()
{
    std::wifstream f(GetCachePath());
    if (f.is_open())
    {
        double val = -1.0;
        f >> val;
        if (val >= 0.0 && val <= 100.0)
            return val;
    }
    return -1.0;
}

static void SaveCachedLevel(double level)
{
    if (level < 0.0 || level > 100.0) return;
    std::wofstream f(GetCachePath());
    if (f.is_open())
        f << (int)level;
}

// Shared state across all instances
struct SharedState
{
    std::mutex lock;
    double lastKnownLevel = -1.0;  // last valid battery %
    int state = 0;                  // 0=off, 1=charging, 2=live

    std::wstring headsetControlPath = L"C:\\Tools\\HeadsetControl\\headsetcontrol.exe";
    int updateIntervalSeconds = 300;

    std::thread pollThread;
    std::atomic<bool> running{ false };
    std::atomic<int> refCount{ 0 };

    static std::wstring RunProcess(const std::wstring& exe, const std::wstring& args)
    {
        std::wstring cmdLine = L"\"" + exe + L"\" " + args;
        SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
        HANDLE hReadPipe, hWritePipe;
        if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
            return L"";
        SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.hStdOutput = hWritePipe;
        si.hStdError = hWritePipe;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        std::vector<wchar_t> cmd(cmdLine.begin(), cmdLine.end());
        cmd.push_back(0);
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            CloseHandle(hReadPipe);
            CloseHandle(hWritePipe);
            return L"";
        }
        CloseHandle(hWritePipe);
        std::string output;
        char buf[4096];
        DWORD bytesRead;
        while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0)
        {
            buf[bytesRead] = 0;
            output += buf;
        }
        WaitForSingleObject(pi.hProcess, 8000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hReadPipe);
        int wlen = MultiByteToWideChar(CP_UTF8, 0, output.c_str(), -1, nullptr, 0);
        std::wstring result(wlen, 0);
        MultiByteToWideChar(CP_UTF8, 0, output.c_str(), -1, &result[0], wlen);
        return result;
    }

    void QueryHeadset()
    {
        std::wstring output = RunProcess(headsetControlPath, L"-o json -b");
        std::lock_guard<std::mutex> guard(lock);

        if (output.empty())
        {
            state = 0;
            return;
        }

        // Extract level first - present in charging and active states
        std::wregex levelRegex(L"\"level\"\\s*:\\s*(\\d+)");
        std::wsmatch match;
        if (std::regex_search(output, match, levelRegex))
        {
            double newLevel = (double)std::stoi(match[1].str());
            lastKnownLevel = newLevel;
            SaveCachedLevel(newLevel);
        }

        if (output.find(L"BATTERY_CHARGING") != std::wstring::npos)
        {
            state = 1;
            return;
        }

        if (output.find(L"BATTERY_UNAVAILABLE") != std::wstring::npos)
        {
            state = 0;
            return;
        }

        if (lastKnownLevel >= 0.0)
            state = 2;
        else
            state = 0;
    }

    void PollLoop()
    {
        while (running)
        {
            QueryHeadset();
            for (int i = 0; i < updateIntervalSeconds * 10 && running; i++)
                Sleep(100);
        }
    }

    void Start()
    {
        if (!running)
        {
            lastKnownLevel = LoadCachedLevel();
            running = true;
            pollThread = std::thread(&SharedState::PollLoop, this);
        }
    }

    void Stop()
    {
        running = false;
        if (pollThread.joinable())
            pollThread.join();
    }
};

static SharedState* g_shared = nullptr;
static std::mutex g_sharedMutex;

// Measure types
// "State"   -> numeric: 0=off, 1=charging, 2=live  string: "Off", "Charging", "Live"
// "Level"   -> numeric: last known battery %        string: "87" etc
// (default) -> numeric: last known battery %        string: "Off"/"Charging"/"87"

enum MeasureType { TYPE_DEFAULT, TYPE_STATE, TYPE_LEVEL };

struct Measure
{
    MeasureType type = TYPE_DEFAULT;
};

extern "C" __declspec(dllexport)
void Initialize(void** data, LPRAINMETER rm)
{
    LoadRainmeterFunctions();

    std::lock_guard<std::mutex> guard(g_sharedMutex);
    if (!g_shared)
        g_shared = new SharedState();
    g_shared->refCount++;

    Measure* m = new Measure();
    *data = m;
}

extern "C" __declspec(dllexport)
void Reload(void* data, LPRAINMETER rm, double* maxValue)
{
    Measure* m = (Measure*)data;

    if (RmReadString)
    {
        std::wstring typeStr = (LPCWSTR)RmReadString(rm, L"Type", L"", FALSE);
        if (typeStr == L"State") m->type = TYPE_STATE;
        else if (typeStr == L"Level") m->type = TYPE_LEVEL;
        else m->type = TYPE_DEFAULT;
    }

    {
        std::lock_guard<std::mutex> guard(g_sharedMutex);
        if (RmReadString)
            g_shared->headsetControlPath = (LPCWSTR)RmReadString(rm, L"HeadsetControlPath",
                L"C:\\Tools\\HeadsetControl\\headsetcontrol.exe", FALSE);
        if (RmReadFormula)
            g_shared->updateIntervalSeconds = (int)RmReadFormula(rm, L"UpdateInterval", 300.0);
        g_shared->Start();
    }

    *maxValue = 100.0;
}

extern "C" __declspec(dllexport)
double Update(void* data)
{
    Measure* m = (Measure*)data;
    std::lock_guard<std::mutex> guard(g_shared->lock);

    if (m->type == TYPE_STATE)
        return (double)g_shared->state;

    // TYPE_DEFAULT and TYPE_LEVEL both return last known level
    return g_shared->lastKnownLevel >= 0.0 ? g_shared->lastKnownLevel : 0.0;
}

static thread_local std::wstring tl_string;

extern "C" __declspec(dllexport)
LPCWSTR GetString(void* data)
{
    Measure* m = (Measure*)data;
    std::lock_guard<std::mutex> guard(g_shared->lock);

    if (m->type == TYPE_STATE)
    {
        if (g_shared->state == 0) tl_string = L"Off";
        else if (g_shared->state == 1) tl_string = L"Charging";
        else tl_string = L"Live";
        return tl_string.c_str();
    }

    if (m->type == TYPE_LEVEL)
    {
        wchar_t buf[16];
        swprintf(buf, 16, L"%d", (int)(g_shared->lastKnownLevel >= 0.0 ? g_shared->lastKnownLevel : 0.0));
        tl_string = buf;
        return tl_string.c_str();
    }

    // TYPE_DEFAULT
    if (g_shared->state == 0) tl_string = L"Off";
    else if (g_shared->state == 1) tl_string = L"Charging";
    else
    {
        wchar_t buf[16];
        swprintf(buf, 16, L"%d", (int)(g_shared->lastKnownLevel >= 0.0 ? g_shared->lastKnownLevel : 0.0));
        tl_string = buf;
    }
    return tl_string.c_str();
}

extern "C" __declspec(dllexport)
void Finalize(void* data)
{
    Measure* m = (Measure*)data;
    delete m;

    std::lock_guard<std::mutex> guard(g_sharedMutex);
    if (g_shared && --g_shared->refCount <= 0)
    {
        g_shared->Stop();
        delete g_shared;
        g_shared = nullptr;
    }
}
