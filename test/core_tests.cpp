#include "config.hpp"
#include "frameprotocol.hpp"
#include "msgdedup.hpp"

#include <cstdlib>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

static void expect(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

static void testFrameProtocol()
{
    const std::string payload = R"({"msgid":1,"name":"测试"})";
    const std::string frame = chatserver::protocol::encodeFrame(payload);
    expect(frame.size() == chatserver::protocol::FRAME_HEADER_LEN + payload.size(), "frame size mismatch");
    expect(chatserver::protocol::decodeFrameLength(frame.data()) == payload.size(), "frame length decode mismatch");
    expect(frame.substr(chatserver::protocol::FRAME_HEADER_LEN) == payload, "frame payload mismatch");
    expect(!chatserver::protocol::isValidPayloadLength(0), "empty payload must be invalid");
    expect(!chatserver::protocol::isValidPayloadLength(chatserver::protocol::FRAME_MAX_PAYLOAD + 1), "oversized payload must be invalid");

    bool emptyRejected = false;
    try
    {
        chatserver::protocol::encodeFrame("");
    }
    catch (const std::length_error &)
    {
        emptyRejected = true;
    }
    expect(emptyRejected, "empty payload was not rejected");
}

static void testDedup()
{
    MsgDedup dedup(2, 120);
    expect(!dedup.isDuplicate("a"), "first a must be new");
    expect(dedup.isDuplicate("a"), "second a must be duplicate");
    expect(!dedup.isDuplicate("b"), "first b must be new");
    expect(!dedup.isDuplicate("c"), "first c must be new");
    expect(dedup.isDuplicate("b"), "b must still be cached");
    // Inserting c evicted a, which was then the least-recently-used entry.
    expect(!dedup.isDuplicate("a"), "evicted a must be new");
    dedup.forget("a");
    expect(!dedup.isDuplicate("a"), "forgotten a must be accepted again");
    expect(!dedup.isDuplicate(""), "empty id must not deduplicate");
    expect(!dedup.isDuplicate(""), "empty id must remain non-deduplicated");
}

static void testConcurrentDedup()
{
    MsgDedup dedup(128, 120);
    std::atomic<int> firstSeen{0};
    std::vector<std::thread> workers;
    workers.reserve(32);

    for (int i = 0; i < 32; ++i)
    {
        workers.emplace_back([&dedup, &firstSeen]() {
            if (!dedup.isDuplicate("same-message-id"))
            {
                ++firstSeen;
            }
        });
    }
    for (auto &worker : workers)
    {
        worker.join();
    }

    expect(firstSeen == 1, "concurrent duplicate check must accept exactly one message");
}

static void testConfig()
{
    unsetenv("CHAT_TEST_VALUE");
    expect(chatserver::config::envOr("CHAT_TEST_VALUE", "fallback") == "fallback", "environment fallback failed");
    setenv("CHAT_TEST_VALUE", "configured", 1);
    expect(chatserver::config::envOr("CHAT_TEST_VALUE", "fallback") == "configured", "environment override failed");

    setenv("CHAT_TEST_PORT", "7000", 1);
    expect(chatserver::config::envIntOr("CHAT_TEST_PORT", 6000) == 7000, "integer environment override failed");
    setenv("CHAT_TEST_PORT", "invalid", 1);
    expect(chatserver::config::envIntOr("CHAT_TEST_PORT", 6000) == 6000, "invalid integer fallback failed");
}

int main()
{
    testFrameProtocol();
    testDedup();
    testConcurrentDedup();
    testConfig();
    std::cout << "chat_core_tests: all assertions passed" << std::endl;
    return 0;
}
