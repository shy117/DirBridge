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
        {FrameType::Resize, 7, 3, {120, 0, 40, 0}},
        {FrameType::Input, 7, 4, {'e', 'x', 'i', 't', '\n'}},
        {FrameType::Close, 7, 5, {}},
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
        "accept Start, AuthSecret, Resize, Input, Close sequence");

    auto repeatedSecret = decoded;
    repeatedSecret.resize(2);
    repeatedSecret.push_back(
        {FrameType::AuthSecret, 7, 3, {'x'}});
    check(
        !validateCommandSequence(repeatedSecret, error),
        "reject repeated AuthSecret");

    auto lateSecret = decoded;
    lateSecret.resize(3);
    lateSecret[1] = {FrameType::Resize, 7, 2, {80, 0, 24, 0}};
    lateSecret[2] = {FrameType::AuthSecret, 7, 3, {'x'}};
    check(
        !validateCommandSequence(lateSecret, error),
        "reject AuthSecret after runtime command");

    auto afterClose = decoded;
    afterClose.push_back({FrameType::Input, 7, 6, {'x'}});
    check(
        !validateCommandSequence(afterClose, error),
        "reject command after Close");

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
