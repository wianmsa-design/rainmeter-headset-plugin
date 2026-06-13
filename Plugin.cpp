#include <Windows.h>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <regex>

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

// Numeric value returned by Update():
//   >= 0  : live battery percentage (headset on)
//   -2.0  : charging
//   -3.0  : off / unavailable / never seen
//
// GetString() returns: "Charging", "Off", or the percentage as a number string
// GetLastKnown() returns the last valid reading for display when off/charging

struct Measure
{
    std::wstring headsetControlPath = L"C:\\Tools\\HeadsetControl\\headsetcontrol.exe";
    int updateIntervalSeconds = 300;

    std::mutex lock;
    double batteryLevel = -3.0;
    double lastKnownLevel = -1.0;
    std::wstring batteryString = L"Off";

    std::thread pollThread;
    std::atomic<bool> running{ false };

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
            batteryLevel = -3.0;
            batteryString = L"Off";
            return;
        }

        // Extract level first - present in both charging and active states
        std::wregex levelRegex(L"\"level\"\\s*:\\s*(\\d+)");
        std::wsmatch match;
        if (std::regex_search(output, match, levelRegex))
        {
            lastKnownLevel = (double)std::stoi(match[1].str());
        }

        if (output.find(L"BATTERY_CHARGING") != std::wstring::npos)
        {
            batteryLevel = -2.0;
            batteryString = L"Charging";
            return;
        }

        if (output.find(L"BATTERY_UNAVAILABLE") != std::wstring::npos)
        {
            batteryLevel = -3.0;
            batteryString = L"Off";
            return;
        }

        if (lastKnownLevel >= 0.0 && std::regex_search(output, match, levelRegex))
        {
            batteryLevel = lastKnownLevel;
            batteryString = match[1].str();
        }
        else
        {
            batteryLevel = -3.0;
            batteryString = L"Off";
        }
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
            running = true;
            pollThread = std::thread(&Measure::PollLoop, this);
        }
    }

    void Stop()
    {
        running = false;
        if (pollThread.joinable())
            pollThread.join();
    }

    double GetValue()
    {
        std::lock_guard<std::mutex> guard(lock);
        // Always return last known level as numeric so bar/conditions work
        // State is communicated via GetString
        return lastKnownLevel >= 0.0 ? lastKnownLevel : 0.0;
    }

    double GetState()
    {
        std::lock_guard<std::mutex> guard(lock);
        return batteryLevel;
    }

    std::wstring GetStr()
    {
        std::lock_guard<std::mutex> guard(lock);
        return batteryString;
    }
};

extern "C" __declspec(dllexport)
void Initialize(void** data, LPRAINMETER rm)
{
    LoadRainmeterFunctions();
    Measure* m = new Measure();
    *data = m;
}

extern "C" __declspec(dllexport)
void Reload(void* data, LPRAINMETER rm, double* maxValue)
{
    Measure* m = (Measure*)data;

    if (RmReadString)
        m->headsetControlPath = (LPCWSTR)RmReadString(rm, L"HeadsetControlPath",
            L"C:\\Tools\\HeadsetControl\\headsetcontrol.exe", FALSE);

    if (RmReadFormula)
        m->updateIntervalSeconds = (int)RmReadFormula(rm, L"UpdateInterval", 300.0);

    *maxValue = 100.0;
    m->Start();
}

extern "C" __declspec(dllexport)
double Update(void* data)
{
    return ((Measure*)data)->GetValue();
}

static thread_local std::wstring tl_string;

extern "C" __declspec(dllexport)
LPCWSTR GetString(void* data)
{
    tl_string = ((Measure*)data)->GetStr();
    return tl_string.c_str();
}

extern "C" __declspec(dllexport)
void Finalize(void* data)
{
    Measure* m = (Measure*)data;
    m->Stop();
    delete m;
}
