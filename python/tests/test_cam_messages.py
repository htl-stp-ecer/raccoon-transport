"""Camera messages: a frame and the detections of that frame are separate messages that share a frame_id."""

from raccoon_transport.types.raccoon import cam_blob_t, cam_detections_t, cam_frame_t

# timestamp 1, frame_id 7, 2x1 pixels, the data bytes 0xff 0xd8.
FRAME = bytes.fromhex("0000000000000001" "0000000000000007" "00000002" "00000001" "00000002" "ffd8")
# timestamp 1, frame_id 7, 2x1 pixels, no detections.
DETECTIONS = bytes.fromhex("0000000000000001" "0000000000000007" "00000002" "00000001" "00000000")


def test_a_frame_is_timestamp_frame_id_size_and_data():
    frame = cam_frame_t(timestamp=1, frame_id=7, frame_width=2, frame_height=1, frame_data=b"\xff\xd8")
    assert frame.encode() == FRAME


def test_a_frame_decodes_to_what_was_encoded():
    frame = cam_frame_t.decode(FRAME)
    assert (frame.timestamp, frame.frame_id, frame.frame_width, frame.frame_height) == (1, 7, 2, 1)
    assert frame.frame_data == b"\xff\xd8"


def test_detections_carry_the_frame_id_of_their_frame():
    assert cam_detections_t(timestamp=1, frame_id=7, frame_width=2, frame_height=1).encode() == DETECTIONS
    assert cam_detections_t.decode(DETECTIONS).frame_id == 7


def test_detections_decode_with_their_blobs():
    blob = cam_blob_t(timestamp=1, label="cube", x=0.5, y=0.25, width=0.1, height=0.2, area=0.02, confidence=0.9, color=-16711936)
    message = cam_detections_t.decode(cam_detections_t(timestamp=2, frame_id=-3, frame_width=160, frame_height=120, detections=[blob]).encode())
    assert message.frame_id == -3
    assert [d.label for d in message.detections] == ["cube"]
    assert message.detections[0].color == -16711936
