// Fuzz harness for the hand-rolled HTTP intake path — the largest
// memory-safety risk in the project (docs/security.md "Parser
// robustness", standing rule 5).
//
// Targets, both fed the same untrusted bytes:
//   http_framing::analyze()      — body framing / smuggling decisions
//   RequestHandler::parseRequest() — request line, headers, body split
//
// Two drivers from one FuzzOneInput():
//   -DFUZZING_LIBFUZZER  → LLVMFuzzerTestOneInput, coverage-guided (clang)
//   default              → standalone; replays corpus files given as
//                          argv, otherwise runs a seeded mutation loop
//                          so it works under GCC and inside CTest.
//
// Build the sanitized version with -DENABLE_SANITIZERS=ON; the point of
// this harness is to feed ASan/UBSan, not to assert on its own.

#include "http_server.h"
#include "request_handler.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Invariants the parser must hold for *any* input it accepts. A crash is
// a finding, but so is a silent contract violation — these are the
// properties the rest of the engine (routing, HMAC, storage) relies on.
void checkParseInvariants(const std::string& raw, const HttpRequest& req) {
    // The query string is stripped from the path, never left dangling.
    assert(req.path.find('?') == std::string::npos);

    // Header keys are lowercased on parse; signature-header lookups in
    // Phase 1 depend on this holding for every accepted request.
    for (const auto& kv : req.headers) {
        for (unsigned char c : kv.first) {
            assert(!(c >= 'A' && c <= 'Z'));
        }
        // A key is never empty and never contains the separator.
        assert(!kv.first.empty());
        assert(kv.first.find(':') == std::string::npos);
    }

    // The body is a suffix of the input, byte for byte: bodies are
    // opaque buffers and must survive parsing unmodified (PRD media
    // rule). This is the property the binary-safety fix restored.
    assert(req.body.size() <= raw.size());
    assert(raw.compare(raw.size() - req.body.size(), req.body.size(), req.body) == 0);
}

void checkFramingInvariants(const std::string& data, http_framing::Result r,
                            size_t header_end, size_t content_length) {
    if (r != http_framing::Result::kOk) return;

    // A decided framing must point at a real terminator inside the data.
    assert(header_end + 4 <= data.size());
    assert(data.compare(header_end, 4, "\r\n\r\n") == 0);

    // Caps hold, and the total length cannot overflow size_t — the
    // server computes header_end + 4 + content_length to size its read.
    assert(header_end <= http_limits::kMaxHeaderBytes);
    assert(content_length <= http_limits::kMaxBodyBytes);
    assert(header_end + 4 + content_length >= header_end);

    // Framing is stable: analyze() is pure, so appending body bytes must
    // not change the decision the server already acted on. A parser that
    // re-decides mid-request is exactly how smuggling bugs happen.
    std::string extended = data + std::string(8, 'x');
    size_t he2 = 0, cl2 = 0;
    assert(http_framing::analyze(extended, he2, cl2) == r);
    assert(he2 == header_end && cl2 == content_length);
}

} // namespace


extern "C" int FuzzOneInput(const uint8_t* data, size_t size) {
    std::string raw(reinterpret_cast<const char*>(data), size);

    size_t header_end = 0, content_length = 0;
    http_framing::Result r = http_framing::analyze(raw, header_end, content_length);
    checkFramingInvariants(raw, r, header_end, content_length);

    RequestHandler handler;
    HttpRequest request;
    if (handler.parseRequest(raw, request)) {
        checkParseInvariants(raw, request);
        // Routing runs on attacker-controlled paths too; no route is
        // registered, so this exercises the 404/405 scan.
        handler.handleRequest(request);
    }
    return 0;
}


#ifdef FUZZING_LIBFUZZER

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return FuzzOneInput(data, size);
}

#else

namespace {

// xorshift64* — deterministic across machines and runs, so a failure
// found in CI reproduces locally from the seed alone.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1DULL;
    }
    size_t below(size_t n) { return n ? static_cast<size_t>(next() % n) : 0; }
};

const char* const kSeeds[] = {
    "GET /health HTTP/1.1\r\nHost: x\r\n\r\n",
    "POST /webhook HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello",
    "POST /webhook HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 44\r\n\r\nhello",
    "POST /webhook HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
    "POST /webhook?a=1&b=2 HTTP/1.1\r\nContent-Type: application/json\r\n"
    "X-Hub-Signature-256: sha256=deadbeef\r\nContent-Length: 2\r\n\r\n{}",
    "POST http://127.0.0.1:8080/webhook HTTP/1.1\r\nContent-Length: 0\r\n\r\n",
    "GET / HTTP/1.1\r\n\r\n",
    "\r\n\r\n",
};
const size_t kSeedCount = sizeof(kSeeds) / sizeof(kSeeds[0]);

// Byte-level mutations, biased toward the structural characters the
// parser keys on (CR, LF, colon, space) — random bytes alone almost
// never produce a request that gets past the request line.
const char kInteresting[] = {'\r', '\n', ':', ' ', '\t', '?', '&', '=', '/',
                             '\0', '\x7f', '\xff', '0', '9', '-', ',', '.'};

std::string mutate(const std::string& in, Rng& rng) {
    std::string out = in;
    size_t rounds = 1 + rng.below(6);
    for (size_t i = 0; i < rounds; ++i) {
        if (out.empty()) {
            out.push_back(kInteresting[rng.below(sizeof(kInteresting))]);
            continue;
        }
        switch (rng.below(6)) {
            case 0:  // overwrite with an interesting byte
                out[rng.below(out.size())] = kInteresting[rng.below(sizeof(kInteresting))];
                break;
            case 1:  // overwrite with an arbitrary byte
                out[rng.below(out.size())] = static_cast<char>(rng.next() & 0xff);
                break;
            case 2:  // delete a byte
                out.erase(rng.below(out.size()), 1);
                break;
            case 3:  // insert a byte
                out.insert(rng.below(out.size() + 1), 1,
                           kInteresting[rng.below(sizeof(kInteresting))]);
                break;
            case 4: {  // splice in a chunk of another seed
                const std::string other = kSeeds[rng.below(kSeedCount)];
                size_t at = rng.below(other.size() + 1);
                size_t len = rng.below(other.size() - at + 1);
                out.insert(rng.below(out.size() + 1), other.substr(at, len));
                break;
            }
            default:  // truncate
                out.resize(rng.below(out.size() + 1));
                break;
        }
        if (out.size() > 64 * 1024) out.resize(64 * 1024);
    }
    return out;
}

int replayFile(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "fuzz: cannot open %s\n", path);
        return 1;
    }
    std::string bytes;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.append(buf, n);
    std::fclose(f);
    FuzzOneInput(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    std::printf("fuzz: replayed %s (%zu bytes) with no failure\n", path, bytes.size());
    return 0;
}

} // namespace

// Usage:
//   fuzz_parse_request                 bounded run (CTest default)
//   fuzz_parse_request <n> [seed]      n iterations, optional RNG seed
//   fuzz_parse_request <file>...       replay saved corpus/crash inputs
int main(int argc, char* argv[]) {
    if (argc > 1 && std::strtoul(argv[1], nullptr, 10) == 0) {
        int rc = 0;
        for (int i = 1; i < argc; ++i) rc |= replayFile(argv[i]);
        return rc;
    }

    size_t iterations = (argc > 1) ? std::strtoul(argv[1], nullptr, 10) : 50000;
    uint64_t seed = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 0x5EED1234ULL;
    Rng rng(seed);

    // Every seed goes through unmutated first, so the corpus doubles as
    // a smoke test of the happy paths.
    for (size_t i = 0; i < kSeedCount; ++i) {
        const std::string s = kSeeds[i];
        FuzzOneInput(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    for (size_t i = 0; i < iterations; ++i) {
        std::string input = mutate(kSeeds[rng.below(kSeedCount)], rng);
        // Occasionally stack mutations to reach deeper states.
        if ((rng.next() & 3) == 0) input = mutate(input, rng);
        FuzzOneInput(reinterpret_cast<const uint8_t*>(input.data()), input.size());
    }

    std::printf("fuzz_parse_request: %zu iterations, seed %llu, no failures\n",
                iterations, static_cast<unsigned long long>(seed));
    return 0;
}

#endif
