#include "tinyfile.h"
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
#include <unistd.h>

using namespace std;
namespace fs = std::filesystem;
using Clock = chrono::steady_clock;

typedef struct Job
{
    string name;
    vector<uint8_t> data; // file contents
}job_t;

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s (--file <path> | --files <list>) --state SYNC|ASYNC [--output_dir <dir>] [--freqs <weight>]\n", prog);
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

// Stops the clock first, so writing the output file is not counted in the CST
static bool report(const job_t &job, const char *mode, Clock::time_point start, const uint8_t *data, size_t len, const string &output_dir)
{
    double seconds = chrono::duration<double>(Clock::now() - start).count();
    printf("TINYFILE_CST|file=%s|mode=%s|seconds=%.6f\n", job.name.c_str(), mode, seconds);
    fflush(stdout);

    string out_path = (fs::path(output_dir) / (job.name + ".snappy")).string();
    ofstream out(out_path, ios::binary);
    out.write(reinterpret_cast<const char *>(data), len);
    if(!out)
    {
        fprintf(stderr, "cannot write %s\n", out_path.c_str());
        return false;
    }
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
        Clock::time_point start = Clock::now();
        if(TF_compress_sync(job.data.data(), job.data.size(), &out, &out_len) != tf_err::SUCCESS)
        {
            return false;
        }
        if(!report(job, "SYNC", start, out, out_len, output_dir)) ok = false;
        TF_free(out);
    }
    return ok;
}

// Submit every file, then poll until all results are back
static bool run_async(vector<job_t> &jobs, const string &output_dir)
{
    map<uint64_t, pair<job_t *, Clock::time_point>> pending;   // req_id -> (job, submit time)

    for(auto &job : jobs)
    {
        uint64_t req_id;
        Clock::time_point start = Clock::now();
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
        if(TF_check_async(&results, &count) != tf_err::SUCCESS) return false;

        for(size_t i = 0; i < count; i++)
        {
            auto it = pending.find(results[i].req_id);
            if(it == pending.end()) continue;
            if(!report(*it->second.first, "ASYNC", it->second.second, results[i].data, results[i].len, output_dir)) ok = false;
            pending.erase(it);
        }
        TF_free_results(results, count);

        // sleep if nothing ready
        if(count == 0) usleep(1000);
    }
    return ok;
}

int main(int argc, char *argv[])
{
    vector<string> paths;
    string state;
    string output_dir = "out";
    int freqs = 1;

    // passing arguments, works
    for(int i = 1; i < argc; i++)
    {
        string arg = argv[i];
        if(i + 1 >= argc)
        {
            fprintf(stderr, "Missing value for %s\n", arg.c_str());
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
                fprintf(stderr, "cannot read file list %s\n", val.c_str());
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
            freqs = atoi(val.c_str());
        }
        else
        {
            fprintf(stderr, "Unknown argument: %s\n", arg.c_str());
            usage(argv[0]);
            return 1;
        }
    }

    if(paths.empty() || (state != "SYNC" && state != "ASYNC") || freqs <= 0)
    {
        usage(argv[0]);
        return 1;
    }
    // temprarily... need freqs to be passed to TF_init() probably, ignoring it now
    (void)freqs;

    // load every input file now
    vector<job_t> jobs;
    bool ok = true;
    for(const string &p : paths)
    {
        job_t job;
        job.name = fs::path(p).filename().string();
        if(!read_file(p, job.data))
        {
            fprintf(stderr, "cannot read %s, skipping\n", p.c_str());
            ok = false;
            continue;
        }
        jobs.push_back(std::move(job));
    }

    error_code ec;
    fs::create_directories(output_dir, ec);
    if(ec)
    {
        fprintf(stderr, "cannot create output dir %s: %s\n", output_dir.c_str(), ec.message().c_str());
        return 1;
    }

    // init
    if(TF_init() != tf_err::SUCCESS)
    {
        fprintf(stderr, "cannot connect to the TinyFile service (is the server running?)\n");
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
