// bench_sse.cpp - SSE concurrent connections benchmark with staged runs and CSV/JSON output
#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <cctype>
#include <fstream>
#include <sstream>
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
    int fd = -1;
    steady_clock::time_point start_time;
    steady_clock::time_point first_token_time;
    bool headers_sent = false;
    bool first_token_received = false;
    bool done = false;
    bool error = false;
    bool terminal = false;
    int status_code = 0;
    uint64_t token_events = 0;
    string buffer;
};

struct StageResult {
    int stage_connections = 0;
    int attempted = 0;
    int created = 0;
    int success = 0;
    int disconnected = 0;
    uint64_t token_events_total = 0;
    double elapsed_sec = 0.0;
    uint64_t ttft_p50_ms = 0;
    uint64_t ttft_p95_ms = 0;
    uint64_t ttft_p99_ms = 0;
    uint64_t ttft_max_ms = 0;
};

struct Options {
    string host;
    int port = 0;
    int num_connections = 0;
    vector<int> stages;
    string model = "claude-sonnet-4-6";
    bool enable_tools = false;
    int duration_sec = 60;
    string csv_out;
    string json_out;
    string path = "/api/chat/stream";
};

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int create_connection(const string& host, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    set_nonblocking(sock);

    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close(sock);
        return -1;
    }

    int rc = connect(sock, (sockaddr*)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(sock);
        return -1;
    }

    return sock;
}

static bool send_sse_request(int fd, const Options& opt) {
    string body =
        string("{\"messages\":[{\"role\":\"user\",\"content\":\"Hello\"}],") +
        "\"model\":\"" + opt.model + "\"," +
        "\"enable_tools\":" + (opt.enable_tools ? "true" : "false") +
        "}";

    string req = "POST " + opt.path + " HTTP/1.1\r\n"
                 "Host: " + opt.host + "\r\n"
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

static uint64_t percentile(const vector<uint64_t>& v, double p) {
    if (v.empty()) return 0;
    size_t idx = (size_t)(v.size() * p);
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

static void mark_terminal(ConnState* c, int& active) {
    if (!c->terminal) {
        c->terminal = true;
        active--;
    }
}

static void parse_lines(ConnState* conn) {
    size_t pos;
    while ((pos = conn->buffer.find('\n')) != string::npos) {
        string line = conn->buffer.substr(0, pos);
        conn->buffer.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (conn->status_code == 0 && line.rfind("HTTP/1.1 ", 0) == 0 && line.size() >= 12) {
            conn->status_code = atoi(line.substr(9, 3).c_str());
        }

        if (line.rfind("data: ", 0) == 0) {
            string data = line.substr(6);
            if (!conn->first_token_received && data != "[DONE]") {
                conn->first_token_time = steady_clock::now();
                conn->first_token_received = true;
            }
            if (data == "[DONE]") {
                conn->done = true;
            }
            if (data.find("\"token\"") != string::npos) {
                conn->token_events++;
            }
        }
    }
}

static StageResult run_stage(const Options& opt, int num_conns) {
    StageResult res;
    res.stage_connections = num_conns;
    res.attempted = num_conns;

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        cerr << "epoll_create1 failed\n";
        return res;
    }

    vector<ConnState> conns(num_conns);
    int failed_create = 0;

    for (int i = 0; i < num_conns; i++) {
        conns[i].fd = create_connection(opt.host, opt.port);
        if (conns[i].fd < 0) {
            failed_create++;
            continue;
        }

        conns[i].start_time = steady_clock::now();

        epoll_event ev{};
        ev.events = EPOLLOUT | EPOLLIN | EPOLLET | EPOLLERR | EPOLLHUP;
        ev.data.ptr = &conns[i];
        epoll_ctl(epfd, EPOLL_CTL_ADD, conns[i].fd, &ev);
    }

    res.created = num_conns - failed_create;

    int active = res.created;
    auto bench_start = steady_clock::now();

    epoll_event events[512];
    while (active > 0) {
        auto now = steady_clock::now();
        auto elapsed = duration_cast<seconds>(now - bench_start).count();
        if (elapsed > opt.duration_sec) {
            break;
        }

        int n = epoll_wait(epfd, events, 512, 1000);
        if (n < 0) break;
        if (n == 0) continue;

        for (int i = 0; i < n; i++) {
            ConnState* conn = (ConnState*)events[i].data.ptr;
            if (conn->terminal) continue;

            if ((events[i].events & EPOLLOUT) && !conn->headers_sent) {
                if (send_sse_request(conn->fd, opt)) {
                    conn->headers_sent = true;
                } else {
                    conn->error = true;
                    mark_terminal(conn, active);
                }
            }

            if (events[i].events & EPOLLIN) {
                char buf[4096];
                while (true) {
                    ssize_t nr = recv(conn->fd, buf, sizeof(buf), 0);
                    if (nr > 0) {
                        conn->buffer.append(buf, nr);
                        parse_lines(conn);
                        if (conn->done) {
                            mark_terminal(conn, active);
                            break;
                        }
                    } else if (nr == 0) {
                        if (!conn->done) conn->error = true;
                        mark_terminal(conn, active);
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        conn->error = true;
                        mark_terminal(conn, active);
                        break;
                    }
                }
            }

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                if (!conn->done) conn->error = true;
                mark_terminal(conn, active);
            }
        }
    }

    auto bench_end = steady_clock::now();
    res.elapsed_sec = duration_cast<milliseconds>(bench_end - bench_start).count() / 1000.0;

    vector<uint64_t> ttfts;
    for (auto& conn : conns) {
        if (conn.fd >= 0) {
            if (conn.done) {
                res.success++;
                if (conn.first_token_received) {
                    uint64_t ttft = duration_cast<milliseconds>(conn.first_token_time - conn.start_time).count();
                    ttfts.push_back(ttft);
                }
            } else if (conn.error) {
                res.disconnected++;
            }
            res.token_events_total += conn.token_events;
            close(conn.fd);
        }
    }

    sort(ttfts.begin(), ttfts.end());
    res.ttft_p50_ms = percentile(ttfts, 0.50);
    res.ttft_p95_ms = percentile(ttfts, 0.95);
    res.ttft_p99_ms = percentile(ttfts, 0.99);
    res.ttft_max_ms = ttfts.empty() ? 0 : ttfts.back();

    close(epfd);
    return res;
}

static bool parse_options(int argc, char** argv, Options& opt) {
    if (argc < 4) return false;

    opt.host = argv[1];
    opt.port = atoi(argv[2]);
    opt.num_connections = atoi(argv[3]);
    opt.stages.push_back(opt.num_connections);

    for (int i = 4; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--duration" && i + 1 < argc) {
            opt.duration_sec = atoi(argv[++i]);
        } else if (arg == "--stages" && i + 1 < argc) {
            opt.stages.clear();
            string s = argv[++i];
            size_t start = 0;
            while (start < s.size()) {
                size_t comma = s.find(',', start);
                string part = s.substr(start, comma == string::npos ? string::npos : comma - start);
                if (!part.empty()) opt.stages.push_back(atoi(part.c_str()));
                if (comma == string::npos) break;
                start = comma + 1;
            }
        } else if (arg == "--model" && i + 1 < argc) {
            opt.model = argv[++i];
        } else if (arg == "--enable-tools") {
            opt.enable_tools = true;
        } else if (arg == "--csv-out" && i + 1 < argc) {
            opt.csv_out = argv[++i];
        } else if (arg == "--json-out" && i + 1 < argc) {
            opt.json_out = argv[++i];
        } else if (arg == "--path" && i + 1 < argc) {
            opt.path = argv[++i];
        }
    }

    if (opt.stages.empty()) opt.stages.push_back(opt.num_connections);
    return true;
}

int main(int argc, char** argv) {
    Options opt;
    if (!parse_options(argc, argv, opt)) {
        cerr << "Usage: " << argv[0] << " <host> <port> <num_connections> [--stages <a,b,c>] [--duration <sec>]"
             << " [--model <name>] [--enable-tools] [--csv-out <path>] [--json-out <path>] [--path <uri>]\n";
        cerr << "Example: " << argv[0] << " 127.0.0.1 8080 100 --stages 100,500,1000 --duration 60 --csv-out out.csv\n";
        return 1;
    }

    cout << "=== SSE Benchmark ===\n";
    cout << "Target: " << opt.host << ":" << opt.port << "\n";
    cout << "Model: " << opt.model << ", enable_tools=" << (opt.enable_tools ? "true" : "false") << "\n";
    cout << "Stages: ";
    for (size_t i = 0; i < opt.stages.size(); i++) {
        cout << opt.stages[i] << (i + 1 == opt.stages.size() ? "" : ",");
    }
    cout << "\n";
    cout << "Stage timeout: " << opt.duration_sec << " sec\n\n";

    vector<StageResult> results;
    for (int stage : opt.stages) {
        cout << "[Stage] connections=" << stage << " ...\n";
        auto r = run_stage(opt, stage);
        results.push_back(r);

        double conn_success_rate = r.attempted == 0 ? 0.0 : (100.0 * r.success / r.attempted);
        double tokens_per_sec = r.elapsed_sec <= 0.0 ? 0.0 : (r.token_events_total / r.elapsed_sec);

        cout << "  success=" << r.success
             << ", disconnected=" << r.disconnected
             << ", conn_success=" << conn_success_rate << "%"
             << ", ttft_p95=" << r.ttft_p95_ms << "ms"
             << ", token_events/s=" << tokens_per_sec << "\n";
    }

    cout << "\n=== Summary (CSV) ===\n";
    cout << "stage_connections,attempted,created,success,disconnected,conn_success_rate,ttft_p50_ms,ttft_p95_ms,ttft_p99_ms,ttft_max_ms,token_events_total,token_events_per_sec,elapsed_sec\n";

    for (const auto& r : results) {
        double conn_success_rate = r.attempted == 0 ? 0.0 : (100.0 * r.success / r.attempted);
        double tokens_per_sec = r.elapsed_sec <= 0.0 ? 0.0 : (r.token_events_total / r.elapsed_sec);
        cout << r.stage_connections << ","
             << r.attempted << ","
             << r.created << ","
             << r.success << ","
             << r.disconnected << ","
             << conn_success_rate << ","
             << r.ttft_p50_ms << ","
             << r.ttft_p95_ms << ","
             << r.ttft_p99_ms << ","
             << r.ttft_max_ms << ","
             << r.token_events_total << ","
             << tokens_per_sec << ","
             << r.elapsed_sec << "\n";
    }

    if (!opt.csv_out.empty()) {
        ofstream ofs(opt.csv_out);
        if (!ofs.is_open()) {
            cerr << "Failed to write csv: " << opt.csv_out << "\n";
            return 1;
        }
        ofs << "stage_connections,attempted,created,success,disconnected,conn_success_rate,ttft_p50_ms,ttft_p95_ms,ttft_p99_ms,ttft_max_ms,token_events_total,token_events_per_sec,elapsed_sec\n";
        for (const auto& r : results) {
            double conn_success_rate = r.attempted == 0 ? 0.0 : (100.0 * r.success / r.attempted);
            double tokens_per_sec = r.elapsed_sec <= 0.0 ? 0.0 : (r.token_events_total / r.elapsed_sec);
            ofs << r.stage_connections << ","
                << r.attempted << ","
                << r.created << ","
                << r.success << ","
                << r.disconnected << ","
                << conn_success_rate << ","
                << r.ttft_p50_ms << ","
                << r.ttft_p95_ms << ","
                << r.ttft_p99_ms << ","
                << r.ttft_max_ms << ","
                << r.token_events_total << ","
                << tokens_per_sec << ","
                << r.elapsed_sec << "\n";
        }
    }

    if (!opt.json_out.empty()) {
        ofstream ofs(opt.json_out);
        if (!ofs.is_open()) {
            cerr << "Failed to write json: " << opt.json_out << "\n";
            return 1;
        }

        ofs << "{\n";
        ofs << "  \"target\": \"" << opt.host << ":" << opt.port << "\",\n";
        ofs << "  \"model\": \"" << opt.model << "\",\n";
        ofs << "  \"enable_tools\": " << (opt.enable_tools ? "true" : "false") << ",\n";
        ofs << "  \"results\": [\n";
        for (size_t i = 0; i < results.size(); i++) {
            const auto& r = results[i];
            double conn_success_rate = r.attempted == 0 ? 0.0 : (100.0 * r.success / r.attempted);
            double tokens_per_sec = r.elapsed_sec <= 0.0 ? 0.0 : (r.token_events_total / r.elapsed_sec);
            ofs << "    {\n";
            ofs << "      \"stage_connections\": " << r.stage_connections << ",\n";
            ofs << "      \"attempted\": " << r.attempted << ",\n";
            ofs << "      \"created\": " << r.created << ",\n";
            ofs << "      \"success\": " << r.success << ",\n";
            ofs << "      \"disconnected\": " << r.disconnected << ",\n";
            ofs << "      \"conn_success_rate\": " << conn_success_rate << ",\n";
            ofs << "      \"ttft_p50_ms\": " << r.ttft_p50_ms << ",\n";
            ofs << "      \"ttft_p95_ms\": " << r.ttft_p95_ms << ",\n";
            ofs << "      \"ttft_p99_ms\": " << r.ttft_p99_ms << ",\n";
            ofs << "      \"ttft_max_ms\": " << r.ttft_max_ms << ",\n";
            ofs << "      \"token_events_total\": " << r.token_events_total << ",\n";
            ofs << "      \"token_events_per_sec\": " << tokens_per_sec << ",\n";
            ofs << "      \"elapsed_sec\": " << r.elapsed_sec << "\n";
            ofs << "    }" << (i + 1 == results.size() ? "\n" : ",\n");
        }
        ofs << "  ]\n";
        ofs << "}\n";
    }

    return 0;
}
