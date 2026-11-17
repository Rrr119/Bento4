import os
import shutil
import struct
import subprocess
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
VIDEO_INPUT = REPO_ROOT / "Test/Data/video-h264-001.mp4"
AUDIO_INPUT = REPO_ROOT / "Test/Data/audio-aac-001.mp4"


def find_tool(name):
    binary_dir = os.environ.get("BENTO4_BIN")
    if binary_dir:
        candidate = Path(binary_dir) / name
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)

    candidate = shutil.which(name)
    if candidate:
        return candidate

    for pattern in (
        "cmakebuild/*/" + name,
        "Build/SCons/Targets/*/*/" + name,
        "Build/Targets/*/*/" + name,
    ):
        matches = sorted(REPO_ROOT.glob(pattern))
        for match in reversed(matches):
            if match.is_file() and os.access(match, os.X_OK):
                return str(match)
    return None


MP4REPAIR = find_tool("mp4repair")
MP4INFO = find_tool("mp4info")
MP4DUMP = find_tool("mp4dump")

pytestmark = pytest.mark.skipif(
    not all((MP4REPAIR, MP4INFO, MP4DUMP)),
    reason="mp4repair, mp4info and mp4dump must be built first",
)


def read_boxes(data, start=0, end=None):
    boxes = []
    if end is None:
        end = len(data)
    offset = start
    while offset + 8 <= end:
        size, atom_type = struct.unpack_from(">I4s", data, offset)
        header_size = 8
        if size == 1:
            if offset + 16 > end:
                break
            size = struct.unpack_from(">Q", data, offset + 8)[0]
            header_size = 16
        elif size == 0:
            size = end - offset
        if size < header_size or offset + size > end:
            break
        boxes.append((atom_type, offset, size, header_size))
        offset += size
    return boxes


def find_box_offset(data, atom_type):
    for box_type, offset, _size, _header_size in read_boxes(data):
        if box_type == atom_type:
            return offset
    raise AssertionError("atom %r not found" % atom_type)


def corrupt_moov(source, destination, damage):
    data = bytearray(source.read_bytes())
    moov_offset = find_box_offset(data, b"moov")

    if damage == "moov-small":
        struct.pack_into(">I", data, moov_offset, 100)
    elif damage == "moov-large":
        struct.pack_into(">I", data, moov_offset, 0x7FFFFFFF)
    elif damage == "moov-zero":
        struct.pack_into(">I", data, moov_offset, 0)
    elif damage == "moov-type":
        data[moov_offset + 4:moov_offset + 8] = b"junk"
    elif damage == "stsz-size":
        stsz_offset = data.find(b"stsz") - 4
        assert stsz_offset > 0
        struct.pack_into(">I", data, stsz_offset, 20)
    else:
        raise AssertionError("unknown damage: " + damage)

    destination.write_bytes(data)


def run_repair(source, output):
    subprocess.run(
        [MP4REPAIR, "--verbose", str(source), str(output)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert output.is_file()
    assert output.stat().st_size > 0


def mp4info(output):
    result = subprocess.run(
        [MP4INFO, str(output)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return result.stdout


def dump_track(source, track_id):
    subprocess.run(
        [MP4DUMP, "--track", str(track_id), str(source)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    dump_path = Path(str(source) + "." + str(track_id))
    assert dump_path.is_file()
    return dump_path.read_bytes()


@pytest.mark.parametrize(
    "damage",
    ["moov-small", "moov-large", "moov-zero", "moov-type", "stsz-size"],
)
def test_repair_recovers_moov_and_sample_tables(tmp_path, damage):
    damaged = tmp_path / ("damaged-" + damage + ".mp4")
    repaired = tmp_path / ("repaired-" + damage + ".mp4")
    corrupt_moov(VIDEO_INPUT, damaged, damage)

    run_repair(damaged, repaired)

    output = mp4info(repaired)
    assert "sample count: 54" in output
    assert "sample count: 78" in output
    assert "Coding:       avc1" in output
    assert "Coding:       mp4a" in output

    boxes = read_boxes(repaired.read_bytes())
    assert boxes[0][0] == b"ftyp"
    assert boxes[1][0] == b"moov"
    assert b"mdat" in [box[0] for box in boxes]


def test_repair_preserves_media_samples(tmp_path):
    damaged = tmp_path / "damaged.mp4"
    repaired = tmp_path / "repaired.mp4"
    original_copy = tmp_path / "original.mp4"
    shutil.copyfile(VIDEO_INPUT, original_copy)
    corrupt_moov(VIDEO_INPUT, damaged, "moov-small")

    run_repair(damaged, repaired)

    assert dump_track(original_copy, 1) == dump_track(repaired, 1)
    assert dump_track(original_copy, 2) == dump_track(repaired, 2)


def test_repair_audio_only_file(tmp_path):
    damaged = tmp_path / "audio-damaged.mp4"
    repaired = tmp_path / "audio-repaired.mp4"
    corrupt_moov(AUDIO_INPUT, damaged, "moov-zero")

    run_repair(damaged, repaired)

    output = mp4info(repaired)
    assert "sample count: 1294" in output
    assert "Coding:       mp4a" in output
