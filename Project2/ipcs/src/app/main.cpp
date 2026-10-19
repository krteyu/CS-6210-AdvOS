#include "tinyfile.h"
#include "debug.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

typedef struct Job
{
    string name;
    vector<uint8_t> data; // file contents
}job_t;

static void usage(const char *prog)
{
    gt_debug_logs(stderr, "Usage: %s (--file <path> | --files <list>) --state SYNC|ASYNC [--output_dir <dir>] [--freqs <weight>]\n", prog);
}

static bool read_file(const string &path, vector<uint8_t> &out)
{
    ifstream in(path, ios::binary);
    if(!in) return false;
    out.assign(istreambuf_iterator<char>(in), istreambuf_iterator<char>());
    return true;
}

// --files: one path per line, blank lines skipped
static bool read_file_list(const string &list_path, vector<string> &out)
{
    ifstream in(list_path);
    if(!in) return false;
    string line;
    while(getline(in, line))
    {
        while(!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if(!line.empty()) out.push_back(line);
    }
    return true;
}

// get current time
static double now_seconds()
{
    return chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count();
}

// Prints the CST line, then saves the result (after the clock stops)
static bool report(const job_t &job, const char *mode, double start, const uint8_t *data, size_t len, const string &output_dir)
{
    double seconds = now_seconds() - start;
    printf("TINYFILE_CST|file=%s|mode=%s|seconds=%.6f\n", job.name.c_str(), mode, seconds);
    fflush(stdout);

    string out_path = output_dir + "/" + job.name + ".snappy";
    FILE *f = fopen(out_path.c_str(), "wb");
    if(f == nullptr)
    {
        gt_debug_logs(stderr, "cannot write %s\n", out_path.c_str());
        return false;
    }
    fwrite(data, 1, len, f);
    fclose(f);
    return true;
}

// One blocking call per file
static bool run_sync(vector<job_t> &jobs, const string &output_dir)
{
    bool ok = true;
    for(auto &job : jobs)
    {
        uint8_t *out;
        size_t out_len;
        double start = now_seconds();
        if(TF_compress_sync(job.data.data(), job.data.size(), &out, &out_len) != tf_err::SUCCESS)
        {
            return false;
        }
        if(!report(job, "SYNC", start, out, out_len, output_dir)) ok = false;
        TF_free(out);
    }
    return ok;
}

static bool run_async(vector<job_t> &jobs, const string &output_dir)
{
    map<uint64_t, pair<job_t *, double>> pending;   // req_id->(job, submit time)

    for(auto &job : jobs)
    {
        uint64_t req_id;
        double start = now_seconds();
        if(TF_compress_async(job.data.data(), job.data.size(), &req_id) != tf_err::SUCCESS)
        {
            return false;
        }
        pending[req_id] = { &job, start };
    }

    bool ok = true;
    while(!pending.empty())
    {
        TF_result *results;
        size_t count;
        // sleeps until at least one result is done
        if(TF_wait_async(&results, &count) != tf_err::SUCCESS) return false;

        // empty only if the library has nothing in flight, so the rest will never come
        if(count == 0) return false;

        for(size_t i = 0; i < count; i++)
        {
            auto it = pending.find(results[i].req_id);
            if(it == pending.end()) continue;
            if(!report(*it->second.first, "ASYNC", it->second.second, results[i].data, results[i].len, output_dir)) ok = false;
            pending.erase(it);
        }
        TF_free_results(results, count);
    }
    return ok;
}

int main(int argc, char *argv[])
{
    vector<string> paths;
    string state;
    string output_dir = "out";
    uint32_t freqs = 1;

    // passing arguments, works
    for(int i = 1; i < argc; i++)
    {
        string arg = argv[i];
        if(i + 1 >= argc)
        {
            gt_debug_logs(stderr, "Missing value for %s\n", arg.c_str());
            usage(argv[0]);
            return 1;
        }
        string val = argv[++i];

        if(arg == "--file")
        {
            paths.push_back(val);
        }
        else if(arg == "--files")
        {
            if(!read_file_list(val, paths))
            {
                gt_debug_logs(stderr, "cannot read file list %s\n", val.c_str());
                return 1;
            }
        }
        else if(arg == "--state")
        {
            state = val;
        }
        else if(arg == "--output_dir")
        {
            output_dir = val;
        }
        else if(arg == "--freqs")
        {
            freqs = static_cast<uint32_t>(atoi(val.c_str()));
        }
        else
        {
            gt_debug_logs(stderr, "Unknown argument: %s\n", arg.c_str());
            usage(argv[0]);
            return 1;
        }
    }

    if(paths.empty() || (state != "SYNC" && state != "ASYNC") || freqs <= 0)
    {
        usage(argv[0]);
        return 1;
    }

    // load every input file now
    vector<job_t> jobs;
    bool ok = true;
    for(const string &p : paths)
    {
        job_t job;
        job.name = fs::path(p).filename().string();
        if(!read_file(p, job.data))
        {
            gt_debug_logs(stderr, "cannot read %s, skipping\n", p.c_str());
            ok = false;
            continue;
        }
        jobs.push_back(std::move(job));
    }

    error_code ec;
    fs::create_directories(output_dir, ec);
    if(ec)
    {
        gt_debug_logs(stderr, "cannot create output dir %s: %s\n", output_dir.c_str(), ec.message().c_str());
        return 1;
    }

    // init
    if(TF_init(freqs) != tf_err::SUCCESS)
    {
        gt_debug_logs(stderr, "cannot connect to the TinyFile service (is the server running?)\n");
        return 1;
    }

    // run the jobs
    bool run_ok;
    if(state == "SYNC")
        run_ok = run_sync(jobs, output_dir);
    else
        run_ok = run_async(jobs, output_dir);

    if(!run_ok) ok = false;

    TF_cleanup();

    if(ok) return 0;

    return 1;
}
