// message_t: the layout is timestamp, title, content (strings with a 32-bit big-endian length).
// The same bytes are checked in python/tests/test_message_t.py.

#include "raccoon/message_t.hpp"

#include <cstdio>
#include <vector>

int main()
{
    const std::vector<uint8_t> golden = {
        0, 0, 0, 0, 0, 0, 0, 1,
        0, 0, 0, 2, 'A', 'b',
        0, 0, 0, 3, 0xc3, 0xa9, '!',
    };
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", what); }
    };

    raccoon::message_t message;
    message.timestamp = 1;
    message.title = "Ab";
    message.content = "\xc3\xa9!";
    std::vector<uint8_t> bytes(static_cast<size_t>(message.encoded_size()));
    check(message.encoded_size() == static_cast<int>(golden.size()), "encoded size");
    check(message.encode(bytes.data(), static_cast<int>(bytes.size())) == static_cast<int>(golden.size()), "encode");
    check(bytes == golden, "bytes equal the layout of the other languages");

    raccoon::message_t back;
    check(back.decode(golden.data(), static_cast<int>(golden.size())) == static_cast<int>(golden.size()), "decode");
    check(back.timestamp == 1 && back.title == "Ab" && back.content == message.content, "decoded fields");

    check(back.decode(golden.data(), static_cast<int>(golden.size()) - 1) == -1, "a cut-off frame is refused");
    check(message.encode(bytes.data(), 10) == -1, "a buffer that is too small is refused");

    return failures == 0 ? 0 : 1;
}
