// Safe, staged auto-tuner for the 4e18 Level Zero Goldbach verifier.
// The supervisor never initializes Level Zero. Every measurement is made by
// a fresh worker process, with a deadline, complete output capture, and a
// health gate after failures.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <poll.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Config {
    uint64_t segment = 0; // Integer span consumed by verifier.cpp (about segment/2 evens).
    uint32_t split = 0;   // Prime-list index; larger values increase tile-walk work.
};

struct Sample {
    Config cfg;
    bool pass = false;
    bool timeout = false;
    bool device_lost = false;
    bool signaled = false;
    int exit_code = -1;
    double seconds = 0.0;
    double rate = 0.0;
    std::string detail;
};

struct Point {
    Config cfg;
    std::vector<Sample> samples;
    bool stable = false;
    double median = 0.0;
    double worst = 0.0;
    double cv_percent = 0.0;
    double rate = 0.0;
    double score = 0.0;
};

struct Options {
    uint64_t target = 4'000'000'000'000'000'000ull;
    uint64_t window = 8'000'000'000ull;
    std::string worker = "./build/goldbach_l0";
    std::string spv = "kernels/goldbach_bmg.spv";
    std::string cache = "base_primes.cache";
    std::string csv = "tuning-4e18.csv";
    int runs = 3;
    int soak_runs = 7;
    int timeout_seconds = 180;
    int cooldown_seconds = 15;
    double max_cv_percent = 3.0;
    uint32_t max_split = 200'000;
    bool quick = false;
};

uint64_t safe_start(const Options& o) {
    uint64_t start = o.target > o.window ? o.target - o.window : 4;
    start = std::max<uint64_t>(start, 4);
    return start & ~uint64_t(1);
}

std::string compact(std::string text) {
    for (char& c : text) if (c == '\n' || c == '\r' || c == ',') c = ' ';
    constexpr size_t cap = 300;
    if (text.size() > cap) text = text.substr(text.size() - cap);
    return text;
}

bool parse_double_after(const std::string& text, const std::string& key, double& value) {
    const size_t pos = text.find(key);
    if (pos == std::string::npos) return false;
    char* end = nullptr;
    value = std::strtod(text.c_str() + pos + key.size(), &end);
    return end != text.c_str() + pos + key.size() && std::isfinite(value);
}

Sample run_worker(const Options& o, Config cfg) {
    Sample sample;
    sample.cfg = cfg;

    int fd[2];
    if (pipe(fd) != 0) {
        sample.detail = std::string("pipe: ") + std::strerror(errno);
        return sample;
    }

    const std::string target = std::to_string(o.target);
    const std::string start = std::to_string(safe_start(o));
    const std::string segment = std::to_string(cfg.segment);
    const std::string split = std::to_string(cfg.split);
    const pid_t pid = fork();
    if (pid < 0) {
        close(fd[0]); close(fd[1]);
        sample.detail = std::string("fork: ") + std::strerror(errno);
        return sample;
    }
    if (pid == 0) {
        setpgid(0, 0);
        close(fd[0]);
        dup2(fd[1], STDOUT_FILENO);
        dup2(fd[1], STDERR_FILENO);
        close(fd[1]);
        execl(o.worker.c_str(), o.worker.c_str(),
              "--limit", target.c_str(), "--start", start.c_str(),
              "--segment", segment.c_str(), "--split", split.c_str(),
              "--spv", o.spv.c_str(), "--prime-cache", o.cache.c_str(),
              static_cast<char*>(nullptr));
        std::fprintf(stderr, "exec %s failed: %s\n", o.worker.c_str(), std::strerror(errno));
        _exit(127);
    }

    close(fd[1]);
    setpgid(pid, pid);
    std::string output;
    int status = 0;
    bool exited = false;
    const auto deadline = Clock::now() + std::chrono::seconds(o.timeout_seconds);
    while (!exited) {
        pollfd pfd{fd[0], POLLIN | POLLHUP, 0};
        (void)poll(&pfd, 1, 200);
        if (pfd.revents & (POLLIN | POLLHUP)) {
            char buf[4096];
            const ssize_t n = read(fd[0], buf, sizeof(buf));
            if (n > 0) output.append(buf, size_t(n));
        }
        const pid_t done = waitpid(pid, &status, WNOHANG);
        exited = done == pid;
        if (!exited && Clock::now() >= deadline) {
            sample.timeout = true;
            kill(-pid, SIGTERM);
            const auto grace = Clock::now() + std::chrono::seconds(2);
            while (Clock::now() < grace && waitpid(pid, &status, WNOHANG) == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (waitpid(pid, &status, WNOHANG) == 0) kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            exited = true;
        }
    }
    for (;;) {
        char buf[4096];
        const ssize_t n = read(fd[0], buf, sizeof(buf));
        if (n <= 0) break;
        output.append(buf, size_t(n));
    }
    close(fd[0]);

    std::string lower = output;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    sample.device_lost = lower.find("device lost") != std::string::npos ||
                         lower.find("engine reset") != std::string::npos ||
                         lower.find("gpu hang") != std::string::npos;
    sample.signaled = WIFSIGNALED(status);
    sample.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    const bool report_pass = output.find("result         : PASS") != std::string::npos;
    const bool got_time = parse_double_after(output, "gpu time       : ", sample.seconds);
    parse_double_after(output, "evens/second   : ", sample.rate);
    sample.pass = !sample.timeout && !sample.device_lost && !sample.signaled &&
                  sample.exit_code == 0 && report_pass && got_time && sample.seconds > 0.0;
    if (!sample.pass) sample.detail = compact(output.empty() ? "no worker output" : output);
    return sample;
}

std::string classification(const Sample& s) {
    if (s.timeout) return "timeout";
    if (s.device_lost) return "device_lost";
    if (s.signaled) return "signal";
    if (!s.pass) return "fail";
    return "pass";
}

void ensure_csv(const Options& o) {
    if (std::filesystem::exists(o.csv) && std::filesystem::file_size(o.csv) > 0) return;
    std::ofstream f(o.csv, std::ios::app);
    f << "timestamp,stage,segment_span,split_index,sample,status,seconds,evens_per_second,detail\n";
}

void log_sample(const Options& o, const std::string& stage, int index, const Sample& s) {
    ensure_csv(o);
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%FT%TZ", std::gmtime(&now));
    std::ofstream f(o.csv, std::ios::app);
    f << stamp << ',' << stage << ',' << s.cfg.segment << ',' << s.cfg.split << ','
      << index << ',' << classification(s) << ',' << s.seconds << ',' << s.rate
      << ',' << compact(s.detail) << '\n';
}

Point measure_point(const Options& o, const std::string& stage, Config cfg, int runs) {
    Point point;
    point.cfg = cfg;
    std::printf("  %-9s span=%-11llu (~%-10llu evens) split=%-7u ", stage.c_str(),
                (unsigned long long)cfg.segment,
                (unsigned long long)((cfg.segment + 1) / 2), cfg.split);
    std::fflush(stdout);
    for (int i = 0; i < runs; ++i) {
        Sample sample = run_worker(o, cfg);
        log_sample(o, stage, i + 1, sample);
        point.samples.push_back(sample);
        if (!sample.pass) {
            std::printf("%s\n", classification(sample).c_str());
            if (!sample.detail.empty()) std::printf("    %s\n", sample.detail.c_str());
            return point;
        }
        std::printf(".");
        std::fflush(stdout);
    }

    std::vector<double> times;
    for (const Sample& s : point.samples) times.push_back(s.seconds);
    std::sort(times.begin(), times.end());
    point.median = times[times.size() / 2];
    point.worst = times.back();
    const double mean = std::accumulate(times.begin(), times.end(), 0.0) / times.size();
    double variance = 0.0;
    for (double v : times) variance += (v - mean) * (v - mean);
    if (times.size() > 1) variance /= double(times.size() - 1);
    point.cv_percent = mean > 0.0 ? 100.0 * std::sqrt(variance) / mean : 100.0;
    std::vector<double> rates;
    for (const Sample& s : point.samples) rates.push_back(s.rate);
    std::sort(rates.begin(), rates.end());
    point.rate = rates[rates.size() / 2];
    if (point.rate <= 0.0) {
        const double evens = double((o.target - safe_start(o)) / 2 + 1);
        point.rate = evens / point.median;
    }
    point.stable = point.cv_percent <= o.max_cv_percent;
    point.score = point.stable ? point.rate / (1.0 + point.cv_percent / 100.0) : 0.0;
    std::printf(" median %.3fs worst %.3fs CV %.2f%%  %.2f G/s%s\n",
                point.median, point.worst, point.cv_percent, point.rate / 1e9,
                point.stable ? "" : "  VARIANCE-FAIL");
    return point;
}

bool failed_hard(const Point& p) {
    return std::any_of(p.samples.begin(), p.samples.end(), [](const Sample& s) {
        return s.timeout || s.device_lost || s.signaled;
    });
}

const Point* best_point(const std::vector<Point>& points) {
    const Point* best = nullptr;
    for (const Point& p : points)
        if (p.stable && (!best || p.score > best->score)) best = &p;
    return best;
}

bool health_gate(const Options& o, Config baseline) {
    std::printf("\n  Cooling down for %d seconds, then checking the conservative baseline...\n",
                o.cooldown_seconds);
    if (o.cooldown_seconds > 0) std::this_thread::sleep_for(std::chrono::seconds(o.cooldown_seconds));
    const Point health = measure_point(o, "health", baseline, 1);
    return health.stable;
}

void usage() {
    std::printf(
        "usage: goldbach_tuner [options]\n"
        "  --target N          target upper bound (default 4e18)\n"
        "  --window N          top-range integer span (default 8e9)\n"
        "  --runs N            samples per tuning point (default 3)\n"
        "  --soak-runs N       final stability samples (default 7)\n"
        "  --timeout N         per-worker deadline in seconds (default 180)\n"
        "  --cooldown N        post-failure cooldown seconds (default 15)\n"
        "  --max-cv PERCENT    maximum timing CV (default 3.0)\n"
        "  --max-split N       moderate-stage safety ceiling (default 1000000)\n"
        "  --worker FILE       verifier executable\n"
        "  --spv FILE          matched, known-good SPIR-V module\n"
        "  --cache FILE        shared base-prime cache\n"
        "  --out FILE          append-only CSV journal\n"
        "  --quick             one sample per point, three-run soak\n");
}

template <typename T>
bool next_value(int& i, int argc, char** argv, T& out) {
    if (++i >= argc) return false;
    std::stringstream ss(argv[i]);
    ss >> out;
    return bool(ss) && ss.eof();
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        bool ok = true;
        if (a == "--target") ok = next_value(i, argc, argv, o.target);
        else if (a == "--window") ok = next_value(i, argc, argv, o.window);
        else if (a == "--runs") ok = next_value(i, argc, argv, o.runs);
        else if (a == "--soak-runs" || a == "--soak") ok = next_value(i, argc, argv, o.soak_runs);
        else if (a == "--timeout") ok = next_value(i, argc, argv, o.timeout_seconds);
        else if (a == "--cooldown") ok = next_value(i, argc, argv, o.cooldown_seconds);
        else if (a == "--max-cv") ok = next_value(i, argc, argv, o.max_cv_percent);
        else if (a == "--max-split") ok = next_value(i, argc, argv, o.max_split);
        else if (a == "--worker") { if (++i < argc) o.worker = argv[i]; else ok = false; }
        else if (a == "--spv") { if (++i < argc) o.spv = argv[i]; else ok = false; }
        else if (a == "--cache") { if (++i < argc) o.cache = argv[i]; else ok = false; }
        else if (a == "--out") { if (++i < argc) o.csv = argv[i]; else ok = false; }
        else if (a == "--quick") o.quick = true;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); return 2; }
        if (!ok) { std::fprintf(stderr, "invalid or missing value for %s\n", a.c_str()); return 2; }
    }
    if (o.target < 4 || o.window < 8 || o.window >= o.target || o.runs < 1 ||
        o.soak_runs < 1 || o.timeout_seconds < 1 || o.max_split < 78'498) {
        std::fprintf(stderr, "invalid tuning bounds\n");
        return 2;
    }
    if (o.quick) { o.runs = 1; o.soak_runs = std::min(o.soak_runs, 3); }
    if (!std::filesystem::exists(o.worker) || !std::filesystem::exists(o.spv)) {
        std::fprintf(stderr, "worker or SPIR-V file does not exist\n");
        return 2;
    }

    const uint64_t max_effective_segment = std::max<uint64_t>(o.window / 4, 2);
    const Config baseline{std::min<uint64_t>(250'000'000, max_effective_segment), 2'000};
    std::printf("=== Goldbach 4e18 stability tuner ===\n");
    std::printf("target=%llu  window=[%llu,%llu]  runs=%d  timeout=%ds\n",
                (unsigned long long)o.target, (unsigned long long)safe_start(o),
                (unsigned long long)o.target, o.runs, o.timeout_seconds);
    std::printf("Safety: isolated workers; hard failures stop escalation; CSV=%s\n\n", o.csv.c_str());

    std::printf("Stage 0 — health and conservative calibration\n");
    Point probe = measure_point(o, "probe", baseline, std::max(1, o.runs));
    if (!probe.stable) {
        std::fprintf(stderr, "Baseline is not stable. Stop here; recover the GPU/driver before tuning.\n");
        return 1;
    }

    std::printf("\nStage 1 — EASY: segment-span knee (low risk)\n");
    std::vector<uint64_t> segments = {125'000'000, 250'000'000, 500'000'000,
                                      1'000'000'000, 2'000'000'000, 4'000'000'000};
    segments.erase(std::remove_if(segments.begin(), segments.end(), [&](uint64_t v) {
        return v > max_effective_segment;
    }), segments.end());
    if (std::find(segments.begin(), segments.end(), baseline.segment) == segments.end())
        segments.push_back(baseline.segment);
    std::sort(segments.begin(), segments.end());
    segments.erase(std::unique(segments.begin(), segments.end()), segments.end());

    std::vector<Point> easy;
    for (uint64_t segment : segments) {
        Point p = measure_point(o, "easy", {segment, baseline.split}, o.runs);
        easy.push_back(p);
        if (!p.stable) {
            if (!health_gate(o, baseline) || failed_hard(p)) {
                std::fprintf(stderr, "Unsafe boundary reached. Tuning stopped without further escalation.\n");
                return 1;
            }
            break;
        }
    }
    const Point* easy_best = best_point(easy);
    if (!easy_best) { std::fprintf(stderr, "No stable easy-stage point.\n"); return 1; }

    std::printf("\nStage 2 — MODERATE: approach sieve-split boundary gradually\n");
    std::vector<uint32_t> splits = {2'000, 3'000, 4'000, 6'000, 8'000, 12'000,
                                    16'000, 20'000, 40'000};
    splits.erase(std::remove_if(splits.begin(), splits.end(), [&](uint32_t v) {
        return v > o.max_split;
    }), splits.end());
    std::vector<Point> moderate;
    int regressions = 0;
    double best_score_seen = 0.0;
    for (uint32_t split : splits) {
        Point p = measure_point(o, "moderate", {easy_best->cfg.segment, split}, o.runs);
        moderate.push_back(p);
        if (!p.stable) {
            if (!health_gate(o, baseline) || failed_hard(p)) {
                std::fprintf(stderr, "Unsafe split boundary reached. Tuning stopped.\n");
                return 1;
            }
            break;
        }
        if (p.score > best_score_seen) { best_score_seen = p.score; regressions = 0; }
        else if (++regressions >= 2) break;
    }
    const Point* moderate_best = best_point(moderate);
    if (!moderate_best) moderate_best = easy_best;

    std::printf("\nStage 3 — FINE: local segment/split neighborhood\n");
    std::set<std::pair<uint64_t, uint32_t>> fine_configs;
    for (double sf : {0.75, 1.0, 1.25}) {
        const uint64_t segment = std::min<uint64_t>(max_effective_segment,
            std::max<uint64_t>(2, uint64_t(double(moderate_best->cfg.segment) * sf)));
        for (double pf : {0.8, 1.0, 1.2}) {
            const uint32_t split = std::min<uint32_t>(o.max_split,
                std::max<uint32_t>(2'000, uint32_t(double(moderate_best->cfg.split) * pf)));
            fine_configs.emplace(segment, split);
        }
    }
    std::vector<Point> fine;
    for (const auto& [segment, split] : fine_configs) {
        Point p = measure_point(o, "fine", {segment, split}, o.runs);
        fine.push_back(p);
        if (!p.stable) {
            if (!health_gate(o, baseline) || failed_hard(p)) {
                std::fprintf(stderr, "Fine-stage instability detected. Tuning stopped.\n");
                return 1;
            }
        }
    }
    const Point* fine_best = best_point(fine);
    const Point winner = fine_best ? *fine_best : *moderate_best;

    std::printf("\nStage 4 — SOAK: every run must pass and remain low-variance\n");
    Point soak = measure_point(o, "soak", winner.cfg, o.soak_runs);
    if (!soak.stable) {
        (void)health_gate(o, baseline);
        std::fprintf(stderr, "Winner failed soak. Recommended result is the conservative baseline.\n");
        std::printf("RECOMMENDED: --segment %llu --split %u (baseline)\n",
                    (unsigned long long)baseline.segment, baseline.split);
        return 1;
    }

    Config recommended = soak.cfg;
    const Config backed_off{std::max<uint64_t>(baseline.segment, soak.cfg.segment * 3 / 4),
                            std::max<uint32_t>(baseline.split, soak.cfg.split * 4 / 5)};
    if (backed_off.segment != soak.cfg.segment || backed_off.split != soak.cfg.split) {
        Point safe = measure_point(o, "backoff", backed_off, std::max(2, o.runs));
        if (safe.stable && soak.score < safe.score * 1.02) recommended = safe.cfg;
    }

    std::printf("\n=== STABLE RESULT ===\n");
    std::printf("fastest    : --segment %llu --split %u (%.2f G evens/s, CV %.2f%%)\n",
                (unsigned long long)soak.cfg.segment, soak.cfg.split,
                soak.rate / 1e9, soak.cv_percent);
    std::printf("recommended: --segment %llu --split %u\n",
                (unsigned long long)recommended.segment, recommended.split);
    std::printf("journal    : %s\n", o.csv.c_str());
    return 0;
}
