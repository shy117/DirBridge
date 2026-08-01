#include "terminal/TerminalBrokerProtocol.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace dirbridge::terminal::broker;

int failures = 0;

void check(bool condition, const char *label)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (!condition)
    {
        ++failures;
    }
}

} // namespace

int main()
{
    std::vector<Frame> expected = {
        {FrameType::Start, 7, 1, {'s', 'p', 'e', 'c'}},
        {FrameType::AuthSecret, 7, 2, {'s', 'e', 'c', 'r', 'e', 't'}},
        {FrameType::Input, 7, 3, {'e', 'x', 'i', 't', '\n'}},
    };
    std::vector<std::uint8_t> bytes;
    for (const Frame &frame : expected)
    {
        const auto encoded = encodeFrame(frame);
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    }

    std::vector<Frame> decoded;
    std::string error;
    check(decodeFrames(bytes, decoded, error), "decode merged broker frames");
    check(
        decoded.size() == expected.size()
            && decoded[1].type == FrameType::AuthSecret
            && decoded[1].payload == expected[1].payload,
        "preserve broker frame type and payload");
    check(
        validateCommandSequence(decoded, error),
        "accept Start, AuthSecret, Input sequence");

    auto repeatedSecret = decoded;
    repeatedSecret.push_back(
        {FrameType::AuthSecret, 7, 4, {'x'}});
    check(
        !validateCommandSequence(repeatedSecret, error),
        "reject repeated AuthSecret");

    auto lateSecret = decoded;
    lateSecret[1] = {FrameType::Input, 7, 2, {'x'}};
    lateSecret[2] = {FrameType::AuthSecret, 7, 3, {'x'}};
    check(
        !validateCommandSequence(lateSecret, error),
        "reject AuthSecret after input");

    std::vector<std::uint8_t> oversized(MaximumSecretPayloadSize + 1, 'x');
    check(
        encodeFrame({FrameType::AuthSecret, 7, 2, oversized}).empty(),
        "reject oversized secret before serialization");

    auto badMagic = bytes;
    badMagic.front() ^= 0xffU;
    check(!decodeFrames(badMagic, decoded, error), "reject invalid magic");

    auto truncated = bytes;
    truncated.pop_back();
    check(!decodeFrames(truncated, decoded, error), "reject truncated payload");

    std::cout << "[SUMMARY] failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
