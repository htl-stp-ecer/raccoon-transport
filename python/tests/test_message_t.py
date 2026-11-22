"""message_t: the title and text of a message for the user. The bytes are the same as in C++ and Dart."""

from raccoon_transport.types.raccoon import message_t

# timestamp 1, title "Ab", content "é!" (UTF-8: c3 a9 21); strings have a 32-bit big-endian length.
GOLDEN = bytes.fromhex("0000000000000001" "00000002" "4162" "00000003" "c3a921")


def test_the_layout_is_timestamp_title_content():
    assert message_t(timestamp=1, title="Ab", content="é!").encode() == GOLDEN


def test_decode_gives_back_what_was_encoded():
    message = message_t.decode(GOLDEN)
    assert (message.timestamp, message.title, message.content) == (1, "Ab", "é!")
    long_text = "line\n" * 5000
    assert message_t.decode(message_t(timestamp=-5, title="", content=long_text).encode()).content == long_text
