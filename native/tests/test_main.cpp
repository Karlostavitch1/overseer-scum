// Minimal native unit tests (no framework dependency). Run via ctest.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "engine/command_queue.h"
#include "util/hook.h"
#include "util/pattern.h"

namespace {

int g_failures = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (!(expr)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                         #expr);                                                 \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

using namespace overseer;

void TestPatternParse() {
    auto p = pattern::Parse("48 8B ?? ? e8");
    CHECK(p.has_value());
    CHECK(p->size() == 5);
    CHECK(p->bytes[0] == 0x48 && p->bytes[1] == 0x8B && p->bytes[4] == 0xE8);
    CHECK(p->mask[0] && p->mask[1] && !p->mask[2] && !p->mask[3] && p->mask[4]);

    CHECK(!pattern::Parse("").has_value());
    CHECK(!pattern::Parse("?? ??").has_value());
    CHECK(!pattern::Parse("4").has_value());
    CHECK(!pattern::Parse("ZZ").has_value());
    CHECK(!pattern::Parse("488B").has_value());
}

void TestPatternFind() {
    const uint8_t data[] = {0x00, 0x48, 0x8B, 0x05, 0x10, 0x20, 0x30, 0x40, 0xE8, 0x48, 0x8B, 0x05};
    auto p = pattern::Parse("48 8B 05 ?? ?? ?? ?? E8");
    CHECK(pattern::Find(data, *p) == data + 1);

    auto leadingWildcard = pattern::Parse("?? 8B 05");
    CHECK(pattern::Find(data, *leadingWildcard) == data + 1);

    auto all = pattern::FindAll(data, *pattern::Parse("48 8B 05"));
    CHECK(all.size() == 2);
    CHECK(all.size() == 2 && all[1] == data + 9);

    auto missing = pattern::Parse("DE AD BE EF");
    CHECK(pattern::Find(data, *missing) == nullptr);

    // Pattern longer than the haystack.
    CHECK(pattern::Find(std::span<const uint8_t>(data, 2), *p) == nullptr);
}

void TestResolveRelative() {
    // mov rax, [rip+0x10] at offset 0, 7 bytes long -> target = 7 + 0x10
    uint8_t code[32] = {0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00};
    CHECK(pattern::ResolveRelative(code, 3, 7) == code + 7 + 0x10);

    // call with negative displacement
    uint8_t call[16] = {};
    call[8] = 0xE8;
    int32_t disp = -8;
    std::memcpy(call + 9, &disp, 4);
    CHECK(pattern::ResolveRelative(call + 8, 1, 5) == call + 5);
}

void TestCommandQueue() {
    engine::CommandQueue queue(3);
    std::vector<std::string> done;
    auto cb = [&](int64_t, engine::CommandStatus, const std::string& out) { done.push_back(out); };

    CHECK(queue.Push({1, "a", cb}));
    CHECK(queue.Push({2, "b", cb}));
    CHECK(queue.Push({3, "c", cb}));
    CHECK(!queue.Push({4, "d", cb}));  // full
    CHECK(queue.Size() == 3);

    size_t ran = queue.Drain(
        [](engine::PendingCommand& c) { c.callback(c.requestId, engine::CommandStatus::Ok, c.command); },
        2);
    CHECK(ran == 2);
    CHECK(done.size() == 2 && done[0] == "a" && done[1] == "b");

    queue.FailAll(engine::CommandStatus::Failed, "shutdown");
    CHECK(done.size() == 3 && done[2] == "shutdown");
    CHECK(queue.Size() == 0);
}

void TestCommandQueueThreads() {
    engine::CommandQueue queue(100000);
    std::atomic<int> completed{0};
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t) {
        producers.emplace_back([&, t] {
            for (int i = 0; i < 1000; ++i) {
                queue.Push({t * 1000 + i, "x",
                            [&](int64_t, engine::CommandStatus, const std::string&) { ++completed; }});
            }
        });
    }
    for (auto& p : producers) p.join();
    while (queue.Drain([](engine::PendingCommand& c) {
        c.callback(c.requestId, engine::CommandStatus::Ok, "");
    }) > 0) {
    }
    CHECK(completed == 4000);
}

__declspec(noinline) int AddOne(int x) { return x + 1; }
using AddOneFn = int (*)(int);
AddOneFn g_originalAddOne = nullptr;
int AddOneDetour(int x) { return g_originalAddOne(x) * 10; }

void TestHook() {
    std::string error;
    // volatile pointer stops the compiler from inlining/constant-folding the call.
    AddOneFn volatile fn = &AddOne;
    CHECK(fn(1) == 2);
    CHECK(hook::Install(&AddOne, &AddOneDetour, &g_originalAddOne, error));
    if (!error.empty()) std::fprintf(stderr, "hook error: %s\n", error.c_str());
    CHECK(fn(1) == 20);
    hook::RemoveAll();
    CHECK(fn(1) == 2);
}

}  // namespace

int main() {
    TestPatternParse();
    TestPatternFind();
    TestResolveRelative();
    TestCommandQueue();
    TestCommandQueueThreads();
    TestHook();
    if (g_failures == 0) {
        std::printf("all native tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
}
