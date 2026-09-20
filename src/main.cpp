#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <httplib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <signal.h>
#endif

using json = nlohmann::json;

std::atomic<bool> g_stop{false};

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT || signal == CTRL_SHUTDOWN_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}
#else
void posix_signal_handler(int) {
    g_stop = true;
}
#endif

struct MetricRecord {
    std::string time;
    std::string process_name;
    std::string window_title;
    bool user_active;
};

std::string current_time_string() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string get_hostname() {
    char buf[256] = {0};
#ifdef _WIN32
    DWORD size = sizeof(buf);
    if (GetComputerNameA(buf, &size)) {
        return std::string(buf, size);
    }
#else
    if (gethostname(buf, sizeof(buf)) == 0) {
        return std::string(buf);
    }
#endif
    return "unknown-host";
}

#ifdef _WIN32
std::string wide_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                                          nullptr, 0, nullptr, nullptr);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                        &str[0], size_needed, nullptr, nullptr);
    return str;
}

std::string get_process_name(DWORD pid) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return "unknown";
    wchar_t path[MAX_PATH] = {0};
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(hProcess, 0, path, &size)) {
        std::wstring full(path);
        size_t pos = full.find_last_of(L"\\/");
        std::wstring name = (pos == std::wstring::npos) ? full : full.substr(pos + 1);
        CloseHandle(hProcess);
        return wide_to_utf8(name);
    }
    CloseHandle(hProcess);
    return "unknown";
}

MetricRecord collect_metrics_win() {
    MetricRecord rec;
    rec.time = current_time_string();

    HWND hwnd = GetForegroundWindow();
    if (hwnd) {
        int len = GetWindowTextLengthW(hwnd);
        std::wstring title(len + 1, L'\0');
        GetWindowTextW(hwnd, &title[0], len + 1);
        title.resize(len);
        rec.window_title = wide_to_utf8(title);

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        rec.process_name = get_process_name(pid);
    } else {
        rec.window_title = "";
        rec.process_name = "";
    }

    LASTINPUTINFO lii;
    lii.cbSize = sizeof(LASTINPUTINFO);
    if (GetLastInputInfo(&lii)) {
        DWORD idle = GetTickCount() - lii.dwTime;
        rec.user_active = (idle < 5000);
    } else {
        rec.user_active = false;
    }
    return rec;
}
#else
MetricRecord collect_metrics_stub() {
    MetricRecord rec;
    rec.time = current_time_string();
    rec.process_name = "unknown";
    rec.window_title = "unknown";
    rec.user_active = false;
    return rec;
}
#endif

class SafeQueue {
public:
    void push(const MetricRecord& rec) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (queue_.size() >= max_size_) {
            queue_.pop_front();
        }
        queue_.push_back(rec);
        cv_.notify_one();
    }

    void push_all_front(const std::vector<MetricRecord>& records) {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto it = records.rbegin(); it != records.rend(); ++it) {
            if (queue_.size() >= max_size_) {
                queue_.pop_back();
            }
            queue_.push_front(*it);
        }
        cv_.notify_one();
    }

    bool wait_for_data_or_timeout(std::vector<MetricRecord>& out,
                                  std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mtx_);
        if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || stop_; })) {
            return false;
        }
        if (stop_ && queue_.empty()) return false;
        out.assign(queue_.begin(), queue_.end());
        queue_.clear();
        return !out.empty();
    }

    void flush_to_file(const std::string& filename) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (queue_.empty()) return;

        json j = json::array();
        for (const auto& rec : queue_) {
            j.push_back({
                {"time", rec.time},
                {"process_name", rec.process_name},
                {"window_title", rec.window_title},
                {"user_active", rec.user_active}
            });
        }
        std::ofstream ofs(filename);
        if (ofs) {
            ofs << j.dump(2);
        }
        queue_.clear();
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_ = true;
        cv_.notify_all();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<MetricRecord> queue_;
    const size_t max_size_ = 100;
    bool stop_ = false;
};

class Agent {
public:
    Agent(const std::string& host, int port, const std::string& agent_id)
        : host_(host), port_(port), agent_id_(agent_id) {}

    void start() {
        running_ = true;
        collector_thread_ = std::thread(&Agent::collector_loop, this);
        sender_thread_ = std::thread(&Agent::sender_loop, this);
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        queue_.stop();
        if (collector_thread_.joinable()) collector_thread_.join();
        if (sender_thread_.joinable()) sender_thread_.join();
        queue_.flush_to_file("backup.json");
    }

private:
    void collector_loop() {
        while (running_ && !g_stop) {
#ifdef _WIN32
            MetricRecord rec = collect_metrics_win();
#else
            MetricRecord rec = collect_metrics_stub();
#endif
            queue_.push(rec);

            for (int i = 0; i < 50 && running_ && !g_stop; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    void sender_loop() {
        httplib::Client cli(host_, port_);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(5, 0);

        while (running_ && !g_stop) {
            std::vector<MetricRecord> batch;
            if (!queue_.wait_for_data_or_timeout(batch, std::chrono::milliseconds(1000))) {
                continue;
            }
            if (batch.empty()) continue;

            json payload = json::array();
            for (const auto& rec : batch) {
                payload.push_back({
                    {"time", rec.time},
                    {"process_name", rec.process_name},
                    {"window_title", rec.window_title},
                    {"user_active", rec.user_active}
                });
            }

            json packet = {
                {"agent_id", agent_id_},
                {"timestamp", std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()},
                {"payload", payload}
            };

            auto res = cli.Post("/", packet.dump(), "application/json");
            if (res) {
                std::cout << "[sender] HTTP " << res->status
                          << " body=" << res->body
                          << " records=" << batch.size() << std::endl;
                if (res->status < 200 || res->status >= 300) {
                    queue_.push_all_front(batch);
                }
            } 
            else {
                std::cout << "[sender] FAILED: "
                          << httplib::to_string(res.error())
                          << " records=" << batch.size() << std::endl;
                queue_.push_all_front(batch);
                for (int i = 0; i < 50 && running_ && !g_stop; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }        
        }
    }

    std::string host_;
    int port_;
    std::string agent_id_;
    SafeQueue queue_;
    std::thread collector_thread_;
    std::thread sender_thread_;
    std::atomic<bool> running_{false};
};

int main() {
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    signal(SIGINT, posix_signal_handler);
    signal(SIGTERM, posix_signal_handler);
#endif

    std::string agent_id = get_hostname();
    Agent agent("127.0.0.1", 8080, agent_id);
    agent.start();

    std::cout << "Agent started. Press Ctrl+C to stop." << std::endl;
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "Stopping agent..." << std::endl;
    agent.stop();
    std::cout << "Agent stopped. Backup saved if needed." << std::endl;
    return 0;
}