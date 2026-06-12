#include <Windows.h>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <regex>
#include <sstream>

// Rainmeter API types
typedef void* LPRAINMETER;
typedef void* (*RmReadStringFunc)(LPRAINMETER rm, LPCWSTR option, LPCWSTR defValue, BOOL replaceMeasures);
typedef double (*RmReadFormulaFunc)(LPRAINMETER rm, LPCWSTR option, double defValue);
typedef void (*RmLogFunc)(LPRAINMETER rm, int level, LPCWSTR message);

// Load Rainmeter functions dynamically
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

struct Measure
{
    std::wstring headsetControlPath = L"C:\\Tools\\HeadsetControl\\headsetcontrol.exe";
    int updateIntervalSeconds = 300;

    std::mutex lock;
    double batteryLevel = -1.0;
    std::wstring batteryString = L"N/A";

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

        // Convert UTF-8 output to wstring
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
            batteryLevel = -1.0;
            batteryString = L"Error";
            return;
        }

        if (output.find(L"BATTERY_CHARGING") != std::wstring::npos)
        {
            batteryLevel = -2.0;
            batteryString = L"Charging";
            return;
        }

        if (output.find(L"BATTERY_UNAVAILABLE") != std::wstring::npos)
        {
            batteryLevel = -1.0;
            batteryString = L"N/A";
            return;
        }

        // Extract "level": <number>
        std::wregex levelRegex(L"\"level\"\\s*:\\s*(\\d+)");
        std::wsmatch match;
        if (std::regex_search(output, match, levelRegex))
        {
            int level = std::stoi(match[1].str());
            batteryLevel = (double)level;
            batteryString = match[1].str();
        }
        else
        {
            batteryLevel = -1.0;
            batteryString = L"N/A";
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
        return batteryLevel >= 0.0 ? batteryLevel : 0.0;
    }

    std::wstring GetStr()
    {
        std::lock_guard<std::mutex> guard(lock);
        return batteryString;
    }
};

// ---- Rainmeter plugin exports ----

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
