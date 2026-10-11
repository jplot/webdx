// Translates every .bin shader of a directory to WGSL: sm3_wgsl_corpus <in-dir> <out-dir>
// (SHADOW=1: every 2D sampler compared). Host build: c++ -std=c++17 -Id3d9 d3d9/sm3_wgsl.cpp tests/sm3_wgsl_corpus.cpp
#include "sm3_wgsl.h"

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s <in-dir> <out-dir>\n", argv[0]); return 2; }
    DIR *dir = opendir(argv[1]);
    if (!dir) { perror(argv[1]); return 2; }
    int total = 0, failed = 0;
    while (dirent *e = readdir(dir)) {
        std::string name = e->d_name;
        if (name.size() < 5 || name.substr(name.size() - 4) != ".bin") continue;
        FILE *f = fopen((std::string(argv[1]) + "/" + name).c_str(), "rb");
        std::vector<uint32_t> tokens(1 << 16);
        size_t n = fread(tokens.data(), 4, tokens.size(), f);
        fclose(f);
        tokens.resize(n);
        Sm3WgslOptions options;
        if (getenv("SHADOW")) options.shadowMask = TranslateSm3ToWgsl(tokens.data(), options).sampler2DMask;
        Sm3WgslResult r = TranslateSm3ToWgsl(tokens.data(), options);
        ++total;
        if (!r.ok) { ++failed; printf("%s: %s\n", name.c_str(), r.error.c_str()); continue; }
        FILE *out = fopen((std::string(argv[2]) + "/" + name.substr(0, name.size() - 4) + ".wgsl").c_str(), "wb");
        fwrite(r.wgsl.data(), 1, r.wgsl.size(), out);
        fclose(out);
    }
    closedir(dir);
    printf("%d shaders, %d failed\n", total, failed);
    return failed ? 1 : 0;
}
