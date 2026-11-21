"""Tests for scripts/channels.py and channels.yml."""
import importlib.util
import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("channels_tool", ROOT / "scripts" / "channels.py")
tool = importlib.util.module_from_spec(spec)
sys.modules["channels_tool"] = tool
spec.loader.exec_module(tool)

SIZES = {"scalar_i32_t": 12, "scalar_f_t": 12, "string_t": None, "vector3f_t": 20}


def expand(*entries):
    return tool.expand({"format": 1, "channels": list(entries)}, SIZES)


def entry(**fields):
    base = {"name": "a/b/state/c", "type": "scalar_i32_t", "doc": "x"}
    base.update(fields)
    return base


class TableFileTests(unittest.TestCase):
    def setUp(self):
        self.contract = json.loads((ROOT / "contract" / "contract.json").read_text(encoding="utf-8"))
        self.document = tool.parse_table((ROOT / "channels.yml").read_text(encoding="utf-8"))

    def test_parser_agrees_with_a_yaml_library(self):
        try:
            import yaml
        except ImportError:
            self.skipTest("PyYAML is not installed")
        expected = yaml.safe_load((ROOT / "channels.yml").read_text(encoding="utf-8"))
        self.assertEqual(self.document, expected)

    def test_every_channel_is_valid(self):
        channels, _ = tool.expand(self.document, tool.type_sizes(self.contract))
        names = [c.name for c in channels]
        self.assertEqual(len(names), len(set(names)))
        self.assertGreater(len(names), 100)

    def test_generated_files_are_up_to_date(self):
        channels, leaves = tool.expand(self.document, tool.type_sizes(self.contract))
        for path, render in tool.GENERATED:
            with self.subTest(path.name):
                self.assertEqual(render(channels, leaves), path.read_text(encoding="utf-8"))

    def test_generated_python_names_every_channel_by_its_path(self):
        module_spec = importlib.util.spec_from_file_location("channel_table_generated", tool.PY_MODULE)
        generated = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(generated)
        channels = generated.channels
        self.assertEqual(channels.hardware.imu.state.GYRO, "hardware/imu/state/gyro")
        self.assertEqual(channels.hardware.latch.cmd.ENGAGED, "hardware/latch/cmd/engaged")
        self.assertEqual(channels.messages.ERROR, "messages/error")
        self.assertEqual(channels.camera.state.FRAME, "camera/state/frame")
        self.assertEqual(channels.hardware.motor.cmd.power(2), "hardware/motor/2/cmd/power")
        self.assertEqual(channels.hardware.digital.state.value(10), "hardware/digital/10/state/value")
        for bad_port in (4, -1, True, "1", None):
            with self.assertRaises(ValueError):
                channels.hardware.motor.cmd.power(bad_port)
        with self.assertRaises(ValueError):
            channels.hardware.digital.state.value(11)

    def test_generated_python_and_dart_cover_every_table_channel(self):
        parsed, _ = tool.expand(self.document, tool.type_sizes(self.contract))
        module_spec = importlib.util.spec_from_file_location("channel_table_generated2", tool.PY_MODULE)
        generated = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(generated)

        def collect(cls, found):
            for name, value in vars(cls).items():
                if name.startswith("_"):
                    continue
                if isinstance(value, type):
                    collect(value, found)
                elif isinstance(value, str):
                    found.add(value)
                elif isinstance(value, staticmethod):
                    for port in range(0, 11):
                        try:
                            found.add(value.__func__(port))
                        except ValueError:
                            pass
            return found

        names = collect(generated.channels, set())
        self.assertEqual(names, {c.name for c in parsed})

    def test_generated_dart_is_well_formed(self):
        text = tool.DART_MODULE.read_text(encoding="utf-8")
        self.assertEqual(text.count("{"), text.count("}"))
        defined = set(re.findall(r"^class (\w+) \{", text, re.M))
        used = set(re.findall(r"const (_\w+)\(\)", text)) - {"_Channels"} | {"_Channels"}
        self.assertLessEqual(used, defined)
        self.assertIn("channels.hardware.motor.cmd.power(port)", text)
        self.assertIn("String get gyro => 'hardware/imu/state/gyro';", text)
        self.assertIn("String power(int port) => _port('hardware/motor/{port}/cmd/power', port, 0, 3);", text)

    def test_fixed_sizes_follow_the_message_types(self):
        sizes = tool.type_sizes(self.contract)
        self.assertEqual(sizes["scalar_i8_t"], 9)
        self.assertEqual(sizes["scalar_i32_t"], 12)
        self.assertEqual(sizes["scalar_f_t"], 12)
        self.assertEqual(sizes["vector3f_t"], 20)
        self.assertEqual(sizes["quaternion_t"], 24)
        self.assertEqual(sizes["kinematics_config_t"], 136)
        self.assertIsNone(sizes["string_t"])
        self.assertIsNone(sizes["cam_frame_t"])

    def test_commands_that_must_arrive_are_reliable(self):
        channels, _ = tool.expand(self.document, tool.type_sizes(self.contract))
        by_name = {c.name: c for c in channels}
        for name in ("hardware/latch/cmd/engaged", "hardware/chassis/cmd/stop", "hardware/motor/2/cmd/reset_position"):
            self.assertTrue(by_name[name].reliable, name)
        for name in ("hardware/chassis/cmd/velocity", "hardware/watchdog/cmd/heartbeat", "hardware/imu/state/gyro"):
            self.assertFalse(by_name[name].reliable, name)

    def test_state_channels_are_retained_and_events_are_not(self):
        channels, _ = tool.expand(self.document, tool.type_sizes(self.contract))
        by_name = {c.name: c for c in channels}
        self.assertTrue(by_name["hardware/odometry/state/pos_x"].retained)
        self.assertFalse(by_name["hardware/motor/0/cmd/power"].retained)
        self.assertFalse(by_name["messages/error"].retained)
        self.assertFalse(by_name["camera/response"].retained)


class ExpandTests(unittest.TestCase):
    def test_ports_replace_the_placeholder(self):
        channels, ranges = expand(entry(name="m/{port}/state/p", ports="0..2"))
        self.assertEqual([c.name for c in channels], ["m/0/state/p", "m/1/state/p", "m/2/state/p"])
        self.assertEqual([c.port for c in channels], [0, 1, 2])
        self.assertEqual((ranges[0].low, ranges[0].high, ranges[0].first_index), (0, 2, 0))

    def test_defaults(self):
        channels, _ = expand(entry())
        channel = channels[0]
        self.assertEqual((channel.slots, channel.payload, channel.fixed, channel.reliable), (64, 12, True, False))
        self.assertTrue(channel.retained)

    def test_variable_type_takes_its_limit(self):
        channels, _ = expand(entry(type="string_t", max_payload="64KiB"))
        self.assertEqual((channels[0].payload, channels[0].fixed), (64 * 1024, False))

    def test_slot_stride_is_a_multiple_of_a_cache_line(self):
        self.assertEqual(tool.slot_stride(12), 64)
        self.assertEqual(tool.slot_stride(48), 64)
        self.assertEqual(tool.slot_stride(49), 128)
        self.assertEqual(tool.slot_stride(256), 320)

    def test_rejects_what_does_not_belong(self):
        cases = {
            "unknown type": entry(type="nope_t"),
            "variable type without a limit": entry(type="string_t"),
            "fixed type with a limit": entry(max_payload="1KiB"),
            "limit without a unit": entry(type="string_t", max_payload=256),
            "port range without placeholder": entry(ports="0..3"),
            "placeholder without port range": entry(name="m/{port}/c"),
            "descending ports": entry(name="m/{port}/c", ports="3..0"),
            "bad character": entry(name="a/b c"),
            "empty segment": entry(name="a//b"),
            "too few slots": entry(slots=1),
            "unknown field": entry(color="red"),
            "missing doc": {"name": "a/b", "type": "scalar_i32_t"},
            "reliable that is not a boolean": entry(reliable="yes"),
            "placeholder inside a segment": entry(name="m/p{port}/c", ports="0..1"),
        }
        for label, bad in cases.items():
            with self.subTest(label):
                with self.assertRaises(tool.TableError):
                    expand(bad)

    def test_rejects_names_that_clash_in_a_language(self):
        def tree(*names):
            channels, leaves = expand(*[entry(name=n) for n in names])
            return tool.build_tree(leaves)
        for label, names in {
            "a channel below another channel": ("a/b", "a/b/c"),
            "a segment that is a reserved word": ("a/class/c",),
            "a segment that starts with a digit": ("a/0/c",),
            "two names that are one in Dart": ("a/b_c/x", "a/bC/y"),
        }.items():
            with self.subTest(label):
                with self.assertRaises(tool.TableError):
                    tree(*names)

    def test_a_port_range_is_a_function_below_the_path_without_the_port(self):
        _, leaves = expand(entry(name="m/{port}/cmd/p", ports="0..3"), entry(name="m/{port}/state/p", ports="0..3"))
        tree = tool.build_tree(leaves)
        self.assertEqual(list(tree.children["m"].children), ["cmd", "state"])
        self.assertEqual(tree.children["m"].children["cmd"].children["p"].leaf.high, 3)

    def test_rejects_duplicates(self):
        with self.assertRaises(tool.TableError):
            expand(entry(), entry())


class ParserTests(unittest.TestCase):
    def test_commas_and_colons_inside_quotes_stay_in_the_text(self):
        document = tool.parse_table('format: 1\nchannels:\n  - {name: a/b, type: scalar_i32_t, doc: "x: y, z", slots: 4}\n')
        self.assertEqual(document["channels"][0], {"name": "a/b", "type": "scalar_i32_t", "doc": "x: y, z", "slots": 4})

    def test_comments_and_blank_lines_are_ignored(self):
        document = tool.parse_table("# c\n\nformat: 1\nchannels:\n  # c\n  - {name: a, type: t, doc: d, reliable: true}\n")
        self.assertEqual(document["channels"], [{"name": "a", "type": "t", "doc": "d", "reliable": True}])

    def test_unreadable_line_names_the_line(self):
        with self.assertRaises(tool.TableError) as raised:
            tool.parse_table("format: 1\nchannels:\n  - [not, a, mapping]\n")
        self.assertIn(":3:", str(raised.exception))


if __name__ == "__main__":
    unittest.main()
