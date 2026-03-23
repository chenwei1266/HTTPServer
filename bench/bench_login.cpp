// bench_login.cpp - Login QPS benchmark with latency percentiles
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

using namespace std;
using namespace chrono;

struct Stats {
    atomic<uint64_t> success{0};
    atomic<uint64_t> failed{0};
    atomic<uint64_t> network_failed{0};
    atomic<uint64_t> http_non_200{0};
    vector<uint64_t> latencies;
};

// Simple HTTP client with Keep-Alive
class HttpClient {
    int sock = -1;
    string host;
    int port;

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

        // Enable TCP_NODELAY for lower latency
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

    bool post_login(const string& username, const string& password, uint64_t& latency_us,
                    bool& network_error, int& http_status) {
        string body = "{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}";
        string req = "POST /api/auth/login HTTP/1.1\r\n"
                     "Host: " + host + "\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: " + to_string(body.size()) + "\r\n"
                     "Connection: keep-alive\r\n"
                     "\r\n" + body;

        auto start = steady_clock::now();

        if (send(sock, req.c_str(), req.size(), 0) < 0) {
            network_error = true;
            http_status = 0;
            return false;
        }

        char buf[4096];
        int n = recv(sock, buf, sizeof(buf), 0);

        auto end = steady_clock::now();
        latency_us = duration_cast<microseconds>(end - start).count();

        if (n <= 0) {
            network_error = true;
            http_status = 0;
            return false;
        }

        network_error = false;
        http_status = 0;

        // Parse HTTP status
        string resp(buf, n);
        auto p = resp.find("HTTP/1.1 ");
        if (p != string::npos && p + 12 <= resp.size()) {
            http_status = atoi(resp.substr(p + 9, 3).c_str());
        }
        return http_status == 200;
    }
};

void worker_thread(const string& host, int port, int requests_per_thread,
                   const string& username, const string& password, Stats& stats) {
    HttpClient client(host, port);

    if (!client.connect()) {
        stats.failed += requests_per_thread;
        stats.network_failed += requests_per_thread;
        return;
    }

    for (int i = 0; i < requests_per_thread; i++) {
        uint64_t latency;
        bool network_error = false;
        int http_status = 0;
        if (client.post_login(username, password, latency, network_error, http_status)) {
            stats.success++;
            stats.latencies.push_back(latency);
        } else {
            stats.failed++;
            if (network_error) stats.network_failed++;
            else stats.http_non_200++;
            // Reconnect on failure
            if (!client.reconnect()) break;
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 7) {
        cerr << "Usage: " << argv[0] << " <host> <port> <threads> <requests_per_thread> <username> <password> [--csv-out <path>]\n";
        cerr << "Example: " << argv[0] << " 127.0.0.1 8080 10 1000 testuser testpass\n";
        return 1;
    }

    string host = argv[1];
    int port = atoi(argv[2]);
    int num_threads = atoi(argv[3]);
    int requests_per_thread = atoi(argv[4]);
    string username = argv[5];
    string password = argv[6];
    string csv_out;

    for (int i = 7; i + 1 < argc; i++) {
        if (string(argv[i]) == "--csv-out") {
            csv_out = argv[i + 1];
            i++;
        }
    }

    cout << "=== Login Benchmark ===\n";
    cout << "Target: " << host << ":" << port << "\n";
    cout << "Threads: " << num_threads << "\n";
    cout << "Requests per thread: " << requests_per_thread << "\n";
    cout << "Total requests: " << num_threads * requests_per_thread << "\n\n";

    Stats stats;
    stats.latencies.reserve(num_threads * requests_per_thread);

    auto start = steady_clock::now();

    vector<thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(worker_thread, host, port, requests_per_thread,
                            username, password, ref(stats));
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = steady_clock::now();
    double elapsed = duration_cast<milliseconds>(end - start).count() / 1000.0;

    // Calculate percentiles
    sort(stats.latencies.begin(), stats.latencies.end());

    auto percentile = [&](double p) -> uint64_t {
        if (stats.latencies.empty()) return 0;
        size_t idx = (size_t)(stats.latencies.size() * p);
        if (idx >= stats.latencies.size()) idx = stats.latencies.size() - 1;
        return stats.latencies[idx];
    };

    if (elapsed <= 0.0) elapsed = 0.001;

    cout << "=== Results ===\n";
    cout << "Elapsed: " << elapsed << " seconds\n";
    cout << "Success: " << stats.success << "\n";
    cout << "Failed: " << stats.failed << "\n";
    cout << "  - Network failed: " << stats.network_failed << "\n";
    cout << "  - HTTP non-200: " << stats.http_non_200 << "\n";
    cout << "QPS: " << (stats.success / elapsed) << "\n\n";

    cout << "=== Latency (microseconds) ===\n";
    cout << "P50: " << percentile(0.50) << " us\n";
    cout << "P95: " << percentile(0.95) << " us\n";
    cout << "P99: " << percentile(0.99) << " us\n";
    cout << "Max: " << (stats.latencies.empty() ? 0 : stats.latencies.back()) << " us\n";

    if (!csv_out.empty()) {
        ofstream ofs(csv_out);
        if (!ofs.is_open()) {
            cerr << "Failed to write csv: " << csv_out << "\n";
            return 1;
        }
        ofs << "metric,value\n";
        ofs << "elapsed_sec," << elapsed << "\n";
        ofs << "success," << stats.success << "\n";
        ofs << "failed," << stats.failed << "\n";
        ofs << "network_failed," << stats.network_failed << "\n";
        ofs << "http_non_200," << stats.http_non_200 << "\n";
        ofs << "qps," << (stats.success / elapsed) << "\n";
        ofs << "p50_us," << percentile(0.50) << "\n";
        ofs << "p95_us," << percentile(0.95) << "\n";
        ofs << "p99_us," << percentile(0.99) << "\n";
        ofs << "max_us," << (stats.latencies.empty() ? 0 : stats.latencies.back()) << "\n";
    }

    return 0;
}
