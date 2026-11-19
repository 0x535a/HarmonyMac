/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Disabled by default. At most 1024 scopes/process/module, no image capture.
// Record wall time separately from this thread's CPU time (worker CPU is not
// attributed here). File creation cannot follow or replace an existing path.
class AuroraPerfTrace {
    struct Point { const char *name; uint64_t begin, wall, cpu; };
    struct Writer {
        std::mutex lock;
        FILE *file = nullptr;
        bool attempted = false;
        unsigned rows = 0;
        ~Writer() { if (file) std::fclose(file); }
    };
    static bool enabled()
    {
        static bool value = [] { const char *s=std::getenv("AURORA_PERF_TRACE");return s && !std::strcmp(s,"1"); }();
        return value;
    }
    static uint64_t clock(clockid_t id)
    {
        timespec t{};if(clock_gettime(id,&t))return 0;
        return uint64_t(t.tv_sec)*1000000000+uint64_t(t.tv_nsec);
    }
    static Writer &writer() { static Writer value;return value; }
    bool active_ = false;
    const char *component_;
    uint64_t last_=0,cpu_=0,serial_=0;
    uintptr_t object_=0;
    int width_=0,height_=0,result_=-1;
    unsigned count_=0;
    Point points_[24]{};
public:
    explicit AuroraPerfTrace(const char *component) : component_(component)
    {
        if(!enabled())return;
        int saved=errno;
        uint64_t now=clock(CLOCK_MONOTONIC);
        static uint64_t first=now;
        static uint64_t delay=[] {
            const char *s=std::getenv("AURORA_PERF_DELAY_MS");
            if(!s || !*s)return uint64_t(0);
            char *end=nullptr;unsigned long value=std::strtoul(s,&end,10);
            return end && !*end && value<=300000 ? uint64_t(value)*1000000 : uint64_t(0);
        }();
        if(now<first || now-first<delay){errno=saved;return;}
        static std::atomic<unsigned> scopes{0};
        unsigned n=scopes.fetch_add(1);
        if(n>=1024){errno=saved;return;}
        active_=true;serial_=n+1;
        last_=now;cpu_=clock(CLOCK_THREAD_CPUTIME_ID);errno=saved;
    }
    void object(const void *identity,int width,int height)
    { object_=reinterpret_cast<uintptr_t>(identity);width_=width;height_=height; }
    void mark(const char *name)
    {
        if(!active_ || count_>=24)return;
        int saved=errno;uint64_t now=clock(CLOCK_MONOTONIC),cpu=clock(CLOCK_THREAD_CPUTIME_ID);
        if(now<last_ || cpu<cpu_){active_=false;errno=saved;return;}
        points_[count_++]={name,last_,now-last_,cpu-cpu_};last_=now;cpu_=cpu;errno=saved;
    }
    void event(const char *name,uint64_t stamp)
    { if(active_ && count_<24 && stamp)points_[count_++]={name,stamp,0,0}; }
    void result(int value) { result_=value; }
    ~AuroraPerfTrace()
    {
        if(!active_)return;
        int saved=errno;mark("return");
        auto &w=writer();std::lock_guard<std::mutex> lock(w.lock);
        if(!w.attempted) {
            w.attempted=true;
            char path[256];
            const char *directory = std::getenv("AURORA_PERF_TRACE_DIR");
            const int size = std::snprintf(path, sizeof(path), "%s/aurora-perf-%d.csv",
                directory && *directory ? directory : "/data/local/tmp", getpid());
            int fd = size > 0 && static_cast<size_t>(size) < sizeof(path)
                ? open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600) : -1;
            if(fd>=0)w.file=fdopen(fd,"w");
            if(fd>=0 && !w.file)close(fd);
            if(w.file) {
                std::setvbuf(w.file,nullptr,_IOFBF,32768);
                std::fprintf(w.file,"pid,component,scope,object,width,height,stage,begin_ns,wall_ns,thread_cpu_ns,result\n");
            }
        }
        if(w.file) {
            for(unsigned i=0;i<count_;++i) {
                const auto &p=points_[i];
                std::fprintf(w.file,"%d,%s,%llu,%llx,%d,%d,%s,%llu,%llu,%llu,%d\n",getpid(),component_,
                    (unsigned long long)serial_,(unsigned long long)object_,width_,height_,p.name,
                    (unsigned long long)p.begin,(unsigned long long)p.wall,(unsigned long long)p.cpu,result_);
                ++w.rows;
            }
            // Each completed scope is visible to CLI collectors; never fsync
            // every frame or send timings into HiLog/console flood paths.
            std::fflush(w.file);
        }
        errno=saved;
    }
};
