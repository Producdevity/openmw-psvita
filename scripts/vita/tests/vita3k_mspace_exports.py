#!/usr/bin/env python3
"""Compile the actual mspace exports against the pinned host allocator."""
import argparse
import pathlib
import re
import subprocess
import tempfile


def function(source, name):
    match = re.search(r"^EXPORT\([^,\n]+,\s*" + re.escape(name) + r"(?:,|\))", source, re.MULTILINE)
    if match is None:
        raise ValueError(f"Missing export: {name}")
    start = match.start()
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=pathlib.Path, required=True,
                        help="Patched Vita3K checkout including its pinned dlmalloc submodule")
    parser.add_argument("--probe-expansion", action="store_true")
    parser.add_argument("--reproduce-stub", action="store_true")
    args = parser.parse_args()
    root = args.source_root.resolve()
    source = (root / "vita3k/modules/SceLibKernel/SceLibKernel.cpp").read_text()
    names = ("Create", "Destroy", "Malloc", "Calloc", "Memalign", "Realloc", "Free", "MallocUsableSize", "MallocStats", "MallocStatsFast")
    exports = "\n".join(function(source, "sceClibMspace" + name) for name in names)
    start = source.index("struct SceClibMspaceStats {")
    exports = source[start:source.index("};", start) + 2] + "\n" + exports
    if args.reproduce_stub:
        # Preserve the old return expression while supplying the now-declared ABI argument.
        exports = exports.replace("return static_cast<SceSize>(mspace_usable_size(address.get(emuenv.mem)));", "return UNIMPLEMENTED();")
    preamble = r'''
#include <dlmalloc.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
using SceSize = uint32_t;
struct MemState {};
struct EmuEnv { MemState mem; struct { std::mutex mutex; } kernel; };
template <typename T> struct Ptr {
    T* value;
    Ptr(T* value) : value(value) {}
    Ptr(T* value, MemState&) : value(value) {}
    T* get(MemState&) const { return value; }
    explicit operator bool() const { return value != nullptr; }
};
#define TRACY_FUNC(...) ((void)0)
#define EXPORT(type, name, ...) type name(EmuEnv& emuenv, ##__VA_ARGS__)
#define UNIMPLEMENTED() 0
#define CALL_EXPORT(name, ...) name(emuenv, ##__VA_ARGS__)
#define LOG_WARN_ONCE(...) ((void)0)
'''
    test = r'''
int main(int argc, char**) {
    EmuEnv env;
    constexpr size_t capacity = 1024 * 1024;
    std::vector<unsigned char> first(capacity), second(capacity);
    auto a = sceClibMspaceCreate(env, first.data(), capacity);
    auto b = sceClibMspaceCreate(env, second.data(), capacity);
    assert(a.value && b.value);
    auto initial = mspace_mallinfo(a.value);
    assert(initial.uordblks > 0);
    assert(initial.uordblks + initial.fordblks == capacity);
    auto check_stats = [&] {
        struct Guarded { uint32_t before; SceClibMspaceStats stats; uint32_t after; };
        Guarded normal = {0x12345678, {}, 0x87654321}, fast = normal;
        memset(&normal.stats, 0xcd, sizeof(normal.stats));
        memset(&fast.stats, 0xef, sizeof(fast.stats));
        sceClibMspaceMallocStats(env, a, &normal.stats);
        sceClibMspaceMallocStatsFast(env, a, &fast.stats);
        assert(normal.before == 0x12345678 && normal.after == 0x87654321);
        assert(fast.before == 0x12345678 && fast.after == 0x87654321);
        assert(memcmp(&normal.stats, &fast.stats, sizeof(normal.stats)) == 0);
        assert(normal.stats.capacity == capacity && normal.stats.unk == capacity);
        assert(normal.stats.peak_in_use == 0); // Explicitly unsupported by this diagnostic adapter.
        const auto info = mspace_mallinfo(a.value);
        assert(normal.stats.current_in_use == info.uordblks);
        assert(normal.stats.capacity - normal.stats.current_in_use == info.fordblks);
    };
    check_stats();
    if (argc > 1) {
        auto p = sceClibMspaceMalloc(env, a, capacity * 2);
        bool outside = p.value && (reinterpret_cast<uintptr_t>(p.value) < reinterpret_cast<uintptr_t>(first.data()) ||
            reinterpret_cast<uintptr_t>(p.value) >= reinterpret_cast<uintptr_t>(first.data() + capacity));
        printf("capacity=%zu footprint=%zu allocation_outside_guest_pool=%d\n", capacity, mspace_footprint(a.value), outside);
        if (p.value) sceClibMspaceFree(env, a, p);
        sceClibMspaceDestroy(env, a);
        sceClibMspaceDestroy(env, b);
        return 0;
    }
    assert(sceClibMspaceMallocUsableSize(env, Ptr<void>(nullptr)) == 0);
    for (uint32_t size : {1, 7, 16, 17, 127, 1024, 4096, 65536}) {
        auto p = sceClibMspaceMalloc(env, a, size);
        assert(p.value);
        auto usable = sceClibMspaceMallocUsableSize(env, p);
        assert(usable >= size);
        assert(usable == mspace_usable_size(p.value));
        check_stats();
        memset(p.value, 0x5a, size);
        auto q = sceClibMspaceRealloc(env, a, p, size * 2);
        assert(q.value);
        assert(sceClibMspaceMallocUsableSize(env, q) >= size * 2);
        check_stats();
        for (uint32_t i = 0; i < size; ++i) assert(static_cast<unsigned char*>(q.value)[i] == 0x5a);
        auto r = sceClibMspaceRealloc(env, a, q, size);
        assert(r.value);
        check_stats();
        for (uint32_t i = 0; i < size; ++i) assert(static_cast<unsigned char*>(r.value)[i] == 0x5a);
        // vitaGL's cross-pool realloc fallback depends on the usable-size export.
        auto copy = sceClibMspaceMalloc(env, b, size * 2);
        memset(copy.value, 0, size * 2);
        memcpy(copy.value, r.value, std::min<uint32_t>(sceClibMspaceMallocUsableSize(env, r), size * 2));
        for (uint32_t i = 0; i < size; ++i) assert(static_cast<unsigned char*>(copy.value)[i] == 0x5a);
        sceClibMspaceFree(env, a, r);
        sceClibMspaceFree(env, b, copy);
        check_stats();
        auto zeroed = sceClibMspaceCalloc(env, a, size, 2);
        assert(zeroed.value && sceClibMspaceMallocUsableSize(env, zeroed) >= size * 2);
        check_stats();
        for (uint32_t i = 0; i < size * 2; ++i) assert(static_cast<unsigned char*>(zeroed.value)[i] == 0);
        sceClibMspaceFree(env, a, zeroed);
        auto aligned = sceClibMspaceMemalign(env, a, 256, size);
        assert(aligned.value && reinterpret_cast<uintptr_t>(aligned.value) % 256 == 0);
        assert(sceClibMspaceMallocUsableSize(env, aligned) >= size);
        check_stats();
        sceClibMspaceFree(env, a, aligned);
        assert(mspace_mallinfo(a.value).uordblks == initial.uordblks);
        check_stats();
    }
    auto contents = sceClibMspaceMalloc(env, a, 4096);
    memset(contents.value, 0x68, 4096);
    auto failed = sceClibMspaceRealloc(env, a, contents, capacity * 2);
    assert(!failed.value && mspace_footprint(a.value) == capacity);
    for (size_t i = 0; i < 4096; ++i) assert(static_cast<unsigned char*>(contents.value)[i] == 0x68);
    check_stats();
    sceClibMspaceFree(env, a, contents);
    assert(mspace_mallinfo(a.value).uordblks == initial.uordblks);
    check_stats();
    auto exercise = [&] {
        for (int i = 0; i < 200; ++i) {
            auto p = sceClibMspaceMalloc(env, a, 128);
            assert(p.value && sceClibMspaceMallocUsableSize(env, p) >= 128);
            SceClibMspaceStats stats = {};
            sceClibMspaceMallocStatsFast(env, a, &stats);
            assert(stats.capacity == capacity && stats.current_in_use <= capacity);
            sceClibMspaceFree(env, a, p);
        }
    };
    std::thread t1(exercise), t2(exercise);
    t1.join();
    t2.join();
    assert(mspace_mallinfo(a.value).uordblks == initial.uordblks);
    check_stats();
    sceClibMspaceFree(env, a, Ptr<void>(nullptr));
    sceClibMspaceDestroy(env, a);
    sceClibMspaceDestroy(env, b);
    puts("mspace export tests passed (host allocator, not firmware equivalence)");
}
'''
    with tempfile.TemporaryDirectory(prefix="vita3k-mspace-") as temp:
        temp = pathlib.Path(temp)
        cpp = temp / "mspace_test.cpp"
        cpp.write_text(preamble + exports + test)
        common = ["clang++", "-std=c++17", "-g", "-O1", "-DONLY_MSPACES=1", "-DUSE_LOCK=0",
                  "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                  "-I", str(root / "external/dlmalloc")]
        # The pinned allocator computes align_as_chunk from null. Keep every other
        # sanitizer enabled there; the export test itself retains pointer-overflow.
        subprocess.run(common + ["-fno-sanitize=pointer-overflow", "-c", str(root / "external/dlmalloc/dlmalloc.cc"),
                                  "-o", str(temp / "dlmalloc.o")], check=True)
        subprocess.run(common + [str(cpp), str(temp / "dlmalloc.o"), "-o", str(temp / "test")], check=True)
        subprocess.run([str(temp / "test")] + (["probe"] if args.probe_expansion else []), check=True)


if __name__ == "__main__":
    main()
