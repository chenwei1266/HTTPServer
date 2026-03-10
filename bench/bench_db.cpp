// bench_db.cpp - Database connection pool benchmark with gradual load
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <algorithm>
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
                   int duration_sec, LoadStats& stats, atomic<bool>& stop_flag) {
    HttpClient client(host, port);

    if (!client.connect()) {
        return;
    }

    while (!stop_flag) {
        uint64_t latency;
        if (client.get_conversations(latency)) {
            stats.success++;
            lock_guard<mutex> lock(stats.latency_mutex);
            stats.latencies.push_back(latency);
        } else {
            stats.failed++;
            // Reconnect
            client = HttpClient(host, port);
            if (!client.connect()) break;
        }
    }
}

void run_load_test(const string& host, int port, const string& session_cookie,
                   int concurrency, int duration_sec) {
    LoadStats stats;
    atomic<bool> stop_flag{false};

    auto start = steady_clock::now();

    vector<thread> threads;
    for (int i = 0; i < concurrency; i++) {
        threads.emplace_back(worker_thread, host, port, session_cookie,
                            duration_sec, ref(stats), ref(stop_flag));
    }

    this_thread::sleep_for(seconds(duration_sec));
    stop_flag = true;

    for (auto& t : threads) {
        t.join();
    }

    auto end = steady_clock::now();
    double elapsed = duration_cast<milliseconds>(end - start).count() / 1000.0;

    // Calculate P99
    sort(stats.latencies.begin(), stats.latencies.end());
    uint64_t p99 = 0;
    if (!stats.latencies.empty()) {
        size_t idx = (size_t)(stats.latencies.size() * 0.99);
        if (idx >= stats.latencies.size()) idx = stats.latencies.size() - 1;
        p99 = stats.latencies[idx];
    }

    double qps = stats.success / elapsed;
    double error_rate = 100.0 * stats.failed / (stats.success + stats.failed + 0.001);

    // Output CSV row
    cout << concurrency << ","
         << qps << ","
         << p99 << ","
         << error_rate << "\n";
}

int main(int argc, char** argv) {
    if (argc < 5) {
        cerr << "Usage: " << argv[0] << " <host> <port> <username> <password>\n";
        cerr << "Example: " << argv[0] << " 127.0.0.1 8080 testuser testpass\n";
        return 1;
    }

    string host = argv[1];
    int port = atoi(argv[2]);
    string username = argv[3];
    string password = argv[4];

    // Login first to get session cookie
    HttpClient login_client(host, port);
    if (!login_client.login(username, password)) {
        cerr << "Login failed\n";
        return 1;
    }

    cerr << "=== DB Connection Pool Benchmark ===\n";
    cerr << "Target: " << host << ":" << port << "\n";
    cerr << "Logged in as: " << username << "\n";
    cerr << "Running gradual load test: 10 -> 50 -> 100 -> 200 concurrency\n";
    cerr << "Each stage runs for 30 seconds\n\n";

    // CSV header
    cout << "concurrency,qps,p99_latency_us,error_rate_percent\n";

    // Gradual load stages
    vector<int> stages = {10, 50, 100, 200};
    for (int concurrency : stages) {
        cerr << "Running stage: " << concurrency << " concurrent connections...\n";

        // Need to get a fresh session cookie for each stage
        HttpClient stage_client(host, port);
        if (!stage_client.login(username, password)) {
            cerr << "Re-login failed for stage " << concurrency << "\n";
            continue;
        }

        run_load_test(host, port, "", concurrency, 30);
    }

    cerr << "\nBenchmark complete. Results saved in CSV format.\n";
    return 0;
}
