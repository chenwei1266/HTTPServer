// bench_sse.cpp - SSE concurrent connections and TTFT benchmark
#include <iostream>
#include <vector>
#include <chrono>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <algorithm>

using namespace std;
using namespace chrono;

struct ConnState {
    int fd;
    steady_clock::time_point start_time;
    steady_clock::time_point first_token_time;
    bool headers_sent = false;
    bool first_token_received = false;
    bool done = false;
    bool error = false;
    string buffer;
};

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int create_connection(const string& host, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    set_nonblocking(sock);

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    connect(sock, (sockaddr*)&addr, sizeof(addr)); // Non-blocking, may return EINPROGRESS
    return sock;
}

bool send_sse_request(int fd, const string& host) {
    string body = R"({"messages":[{"role":"user","content":"Hello"}],"model":"claude-sonnet-4-6"})";
    string req = "POST /api/chat/stream HTTP/1.1\r\n"
                 "Host: " + host + "\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: " + to_string(body.size()) + "\r\n"
                 "Accept: text/event-stream\r\n"
                 "Connection: keep-alive\r\n"
                 "\r\n" + body;

    ssize_t sent = 0;
    while (sent < (ssize_t)req.size()) {
        ssize_t n = send(fd, req.c_str() + sent, req.size() - sent, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            return false;
        }
        sent += n;
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        cerr << "Usage: " << argv[0] << " <host> <port> <num_connections>\n";
        cerr << "Example: " << argv[0] << " 127.0.0.1 8080 100\n";
        return 1;
    }

    string host = argv[1];
    int port = atoi(argv[2]);
    int num_conns = atoi(argv[3]);

    cout << "=== SSE Benchmark ===\n";
    cout << "Target: " << host << ":" << port << "\n";
    cout << "Connections: " << num_conns << "\n\n";

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        cerr << "epoll_create1 failed\n";
        return 1;
    }

    vector<ConnState> conns(num_conns);
    int connected = 0;
    int failed_connect = 0;

    // Create all connections
    for (int i = 0; i < num_conns; i++) {
        conns[i].fd = create_connection(host, port);
        if (conns[i].fd < 0) {
            failed_connect++;
            continue;
        }

        conns[i].start_time = steady_clock::now();

        epoll_event ev{};
        ev.events = EPOLLOUT | EPOLLIN | EPOLLET;
        ev.data.ptr = &conns[i];
        epoll_ctl(epfd, EPOLL_CTL_ADD, conns[i].fd, &ev);
    }

    cout << "Connections created: " << (num_conns - failed_connect) << "\n";
    cout << "Failed to create: " << failed_connect << "\n\n";

    // Event loop
    epoll_event events[256];
    int active = num_conns - failed_connect;

    while (active > 0) {
        int n = epoll_wait(epfd, events, 256, 5000);
        if (n < 0) break;
        if (n == 0) {
            cout << "Timeout waiting for events\n";
            break;
        }

        for (int i = 0; i < n; i++) {
            ConnState* conn = (ConnState*)events[i].data.ptr;

            if (events[i].events & EPOLLOUT && !conn->headers_sent) {
                if (send_sse_request(conn->fd, host)) {
                    conn->headers_sent = true;
                } else {
                    conn->error = true;
                    active--;
                }
            }

            if (events[i].events & EPOLLIN) {
                char buf[4096];
                ssize_t nr = recv(conn->fd, buf, sizeof(buf), 0);

                if (nr <= 0) {
                    if (!conn->done) {
                        conn->error = true;
                        active--;
                    }
                    continue;
                }

                conn->buffer.append(buf, nr);

                // Check for first data token
                if (!conn->first_token_received) {
                    if (conn->buffer.find("data:") != string::npos) {
                        conn->first_token_time = steady_clock::now();
                        conn->first_token_received = true;
                    }
                }

                // Check for [DONE]
                if (conn->buffer.find("[DONE]") != string::npos) {
                    conn->done = true;
                    active--;
                }
            }

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                if (!conn->done) {
                    conn->error = true;
                    active--;
                }
            }
        }
    }

    // Calculate statistics
    int success = 0;
    int disconnected = 0;
    vector<uint64_t> ttfts;
    vector<uint64_t> total_times;

    for (auto& conn : conns) {
        if (conn.fd < 0) continue;

        if (conn.done) {
            success++;
            if (conn.first_token_received) {
                uint64_t ttft = duration_cast<milliseconds>(
                    conn.first_token_time - conn.start_time).count();
                ttfts.push_back(ttft);
            }
        } else if (conn.error) {
            disconnected++;
        }

        close(conn.fd);
    }

    sort(ttfts.begin(), ttfts.end());

    auto percentile = [&](const vector<uint64_t>& v, double p) -> uint64_t {
        if (v.empty()) return 0;
        size_t idx = (size_t)(v.size() * p);
        if (idx >= v.size()) idx = v.size() - 1;
        return v[idx];
    };

    cout << "=== Results ===\n";
    cout << "Successful completions: " << success << "\n";
    cout << "Disconnected mid-stream: " << disconnected << "\n";
    cout << "Connection success rate: " << (100.0 * success / num_conns) << "%\n\n";

    if (!ttfts.empty()) {
        cout << "=== TTFT (Time To First Token, ms) ===\n";
        cout << "P50: " << percentile(ttfts, 0.50) << " ms\n";
        cout << "P95: " << percentile(ttfts, 0.95) << " ms\n";
        cout << "P99: " << percentile(ttfts, 0.99) << " ms\n";
        cout << "Max: " << ttfts.back() << " ms\n";
    }

    close(epfd);
    return 0;
}
