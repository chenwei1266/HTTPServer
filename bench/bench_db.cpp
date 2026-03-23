// bench_db.cpp - Database connection pool benchmark with gradual load
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <mutex>

using namespace std;
using namespace chrono;

struct LoadStats {
    atomic<uint64_t> success{0};
    atomic<uint64_t> failed{0};
    vector<uint64_t> latencies;
    mutex latency_mutex;
};

class HttpClient {
    int sock = -1;
    string host;
    int port;
    string session_cookie;

public:
    HttpClient(const string& h, int p) : host(h), port(p) {}

    ~HttpClient() {
        if (sock >= 0) close(sock);
    }

    bool connect() {
        if (sock >= 0) {
            close(sock);
            sock = -1;
        }
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return false;

        int flag = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        return ::connect(sock, (sockaddr*)&addr, sizeof(addr)) == 0;
    }

    bool reconnect() {
        return connect();
    }

    bool login(const string& username, const string& password) {
        if (!connect()) return false;

        string body = "{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}";
        string req = "POST /api/auth/login HTTP/1.1\r\n"
                     "Host: " + host + "\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: " + to_string(body.size()) + "\r\n"
                     "Connection: keep-alive\r\n"
                     "\r\n" + body;

        if (send(sock, req.c_str(), req.size(), 0) < 0) return false;

        char buf[4096];
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) return false;

        // Extract Set-Cookie header
        string response(buf, n);
        size_t cookie_pos = response.find("Set-Cookie: sessionId=");
        if (cookie_pos == string::npos) return false;

        cookie_pos += 22; // length of "Set-Cookie: sessionId="
        size_t cookie_end = response.find(";", cookie_pos);
        if (cookie_end == string::npos) cookie_end = response.find("\r", cookie_pos);

        session_cookie = response.substr(cookie_pos, cookie_end - cookie_pos);
        return !session_cookie.empty();
    }

    void set_session_cookie(const string& cookie) {
        session_cookie = cookie;
    }

    const string& get_session_cookie() const {
        return session_cookie;
    }

    bool get_conversations(uint64_t& latency_us) {
        string req = "GET /api/conversations HTTP/1.1\r\n"
                     "Host: " + host + "\r\n"
                     "Cookie: sessionId=" + session_cookie + "\r\n"
                     "Connection: keep-alive\r\n"
                     "\r\n";

        auto start = steady_clock::now();

        if (send(sock, req.c_str(), req.size(), 0) < 0) {
            return false;
        }

        char buf[8192];
        int n = recv(sock, buf, sizeof(buf), 0);

        auto end = steady_clock::now();
        latency_us = duration_cast<microseconds>(end - start).count();

        if (n <= 0) return false;

        return strstr(buf, "HTTP/1.1 200") != nullptr;
    }
};

void worker_thread(const string& host, int port, const string& session_cookie,
                   LoadStats& stats, atomic<bool>& stop_flag) {
    HttpClient client(host, port);

    if (!client.connect()) {
        return;
    }
    client.set_session_cookie(session_cookie);

    while (!stop_flag) {
        uint64_t latency;
        if (client.get_conversations(latency)) {
            stats.success++;
            lock_guard<mutex> lock(stats.latency_mutex);
            stats.latencies.push_back(latency);
        } else {
            stats.failed++;
            // Reconnect
            if (!client.reconnect()) break;
            client.set_session_cookie(session_cookie);
        }
    }
}

struct StageResult {
    int concurrency = 0;
    double qps = 0.0;
    uint64_t p95_us = 0;
    uint64_t p99_us = 0;
    double error_rate = 0.0;
};

StageResult run_load_test(const string& host, int port, const string& session_cookie,
                          int concurrency, int duration_sec) {
    LoadStats stats;
    atomic<bool> stop_flag{false};

    auto start = steady_clock::now();

    vector<thread> threads;
    for (int i = 0; i < concurrency; i++) {
        threads.emplace_back(worker_thread, host, port, session_cookie,
                            ref(stats), ref(stop_flag));
    }

    this_thread::sleep_for(seconds(duration_sec));
    stop_flag = true;

    for (auto& t : threads) {
        t.join();
    }

    auto end = steady_clock::now();
    double elapsed = duration_cast<milliseconds>(end - start).count() / 1000.0;

    // Calculate percentiles
    sort(stats.latencies.begin(), stats.latencies.end());
    uint64_t p95 = 0;
    uint64_t p99 = 0;
    if (!stats.latencies.empty()) {
        size_t idx95 = (size_t)(stats.latencies.size() * 0.95);
        size_t idx = (size_t)(stats.latencies.size() * 0.99);
        if (idx95 >= stats.latencies.size()) idx95 = stats.latencies.size() - 1;
        if (idx >= stats.latencies.size()) idx = stats.latencies.size() - 1;
        p95 = stats.latencies[idx95];
        p99 = stats.latencies[idx];
    }

    double qps = stats.success / elapsed;
    double error_rate = 100.0 * stats.failed / (stats.success + stats.failed + 0.001);

    StageResult r;
    r.concurrency = concurrency;
    r.qps = qps;
    r.p95_us = p95;
    r.p99_us = p99;
    r.error_rate = error_rate;
    return r;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        cerr << "Usage: " << argv[0] << " <host> <port> <username> <password> [--duration <sec>] [--stages <a,b,c>] [--csv-out <path>]\n";
        cerr << "Example: " << argv[0] << " 127.0.0.1 8080 testuser testpass --duration 30 --stages 10,50,100,200\n";
        return 1;
    }

    string host = argv[1];
    int port = atoi(argv[2]);
    string username = argv[3];
    string password = argv[4];
    int duration_sec = 30;
    vector<int> stages = {10, 50, 100, 200};
    string csv_out;

    for (int i = 5; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--duration" && i + 1 < argc) {
            duration_sec = atoi(argv[++i]);
        } else if (arg == "--stages" && i + 1 < argc) {
            stages.clear();
            string s = argv[++i];
            size_t start = 0;
            while (start < s.size()) {
                size_t comma = s.find(',', start);
                string part = s.substr(start, comma == string::npos ? string::npos : comma - start);
                if (!part.empty()) stages.push_back(atoi(part.c_str()));
                if (comma == string::npos) break;
                start = comma + 1;
            }
        } else if (arg == "--csv-out" && i + 1 < argc) {
            csv_out = argv[++i];
        }
    }

    // Login first to get session cookie
    HttpClient login_client(host, port);
    if (!login_client.login(username, password)) {
        cerr << "Login failed\n";
        return 1;
    }

    cerr << "=== DB Connection Pool Benchmark ===\n";
    cerr << "Target: " << host << ":" << port << "\n";
    cerr << "Logged in as: " << username << "\n";
    cerr << "Running gradual load test with stages: ";
    for (size_t i = 0; i < stages.size(); i++) {
        cerr << stages[i] << (i + 1 == stages.size() ? "" : ",");
    }
    cerr << "\n";
    cerr << "Each stage runs for " << duration_sec << " seconds\n\n";

    vector<StageResult> results;

    for (int concurrency : stages) {
        cerr << "Running stage: " << concurrency << " concurrent connections...\n";

        // Need to get a fresh session cookie for each stage
        HttpClient stage_client(host, port);
        if (!stage_client.login(username, password)) {
            cerr << "Re-login failed for stage " << concurrency << "\n";
            continue;
        }

        auto r = run_load_test(host, port, stage_client.get_session_cookie(), concurrency, duration_sec);
        results.push_back(r);
        cerr << "  qps=" << r.qps << ", p99_us=" << r.p99_us << ", error_rate=" << r.error_rate << "%\n";
    }

    cout << "concurrency,qps,p95_latency_us,p99_latency_us,error_rate_percent\n";
    for (const auto& r : results) {
        cout << r.concurrency << ","
             << r.qps << ","
             << r.p95_us << ","
             << r.p99_us << ","
             << r.error_rate << "\n";
    }

    if (!csv_out.empty()) {
        ofstream ofs(csv_out);
        if (!ofs.is_open()) {
            cerr << "Failed to write csv: " << csv_out << "\n";
            return 1;
        }
        ofs << "concurrency,qps,p95_latency_us,p99_latency_us,error_rate_percent\n";
        for (const auto& r : results) {
            ofs << r.concurrency << ","
                << r.qps << ","
                << r.p95_us << ","
                << r.p99_us << ","
                << r.error_rate << "\n";
        }
    }

    cerr << "\nBenchmark complete. Results saved in CSV format.\n";
    return 0;
}
