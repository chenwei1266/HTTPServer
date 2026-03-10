// bench_login.cpp - Login QPS benchmark with latency percentiles
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

using namespace std;
using namespace chrono;

struct Stats {
    atomic<uint64_t> success{0};
    atomic<uint64_t> failed{0};
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

    bool post_login(const string& username, const string& password, uint64_t& latency_us) {
        string body = "{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}";
        string req = "POST /api/auth/login HTTP/1.1\r\n"
                     "Host: " + host + "\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: " + to_string(body.size()) + "\r\n"
                     "Connection: keep-alive\r\n"
                     "\r\n" + body;

        auto start = steady_clock::now();

        if (send(sock, req.c_str(), req.size(), 0) < 0) {
            return false;
        }

        char buf[4096];
        int n = recv(sock, buf, sizeof(buf), 0);

        auto end = steady_clock::now();
        latency_us = duration_cast<microseconds>(end - start).count();

        if (n <= 0) return false;

        // Check for HTTP 200
        return strstr(buf, "HTTP/1.1 200") != nullptr;
    }
};

void worker_thread(const string& host, int port, int requests_per_thread,
                   const string& username, const string& password, Stats& stats) {
    HttpClient client(host, port);

    if (!client.connect()) {
        stats.failed += requests_per_thread;
        return;
    }

    for (int i = 0; i < requests_per_thread; i++) {
        uint64_t latency;
        if (client.post_login(username, password, latency)) {
            stats.success++;
            stats.latencies.push_back(latency);
        } else {
            stats.failed++;
            // Reconnect on failure
            client = HttpClient(host, port);
            if (!client.connect()) break;
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 6) {
        cerr << "Usage: " << argv[0] << " <host> <port> <threads> <requests_per_thread> <username> <password>\n";
        cerr << "Example: " << argv[0] << " 127.0.0.1 8080 10 1000 testuser testpass\n";
        return 1;
    }

    string host = argv[1];
    int port = atoi(argv[2]);
    int num_threads = atoi(argv[3]);
    int requests_per_thread = atoi(argv[4]);
    string username = argv[5];
    string password = argv[6];

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

    cout << "=== Results ===\n";
    cout << "Elapsed: " << elapsed << " seconds\n";
    cout << "Success: " << stats.success << "\n";
    cout << "Failed: " << stats.failed << "\n";
    cout << "QPS: " << (stats.success / elapsed) << "\n\n";

    cout << "=== Latency (microseconds) ===\n";
    cout << "P50: " << percentile(0.50) << " us\n";
    cout << "P95: " << percentile(0.95) << " us\n";
    cout << "P99: " << percentile(0.99) << " us\n";
    cout << "Max: " << (stats.latencies.empty() ? 0 : stats.latencies.back()) << " us\n";

    return 0;
}
