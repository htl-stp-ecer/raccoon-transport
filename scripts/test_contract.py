"""Tests for scripts/contract.py. Run: python3 -m unittest scripts/test_contract.py"""
import importlib.util
import re
import shutil
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "contract.py"
ROOT = SCRIPT.parents[1]


def load():
    spec = importlib.util.spec_from_file_location("contract_under_test", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Tree:
    """A scratch copy of the C++ sources the contract is read from."""

    def __init__(self):
        self.dir = Path(tempfile.mkdtemp())
        shutil.copytree(ROOT / "messages" / "include", self.dir / "messages" / "include")
        (self.dir / "cpp" / "include" / "raccoon").mkdir(parents=True)
        for name in ("Channels.h", "raccoon_ring.h"):
            shutil.copy(ROOT / "cpp" / "include" / "raccoon" / name, self.dir / "cpp" / "include" / "raccoon" / name)
        self.module = load()
        self.module.MESSAGES_DIR = self.dir / "messages" / "include" / "raccoon"
        self.module.CHANNELS_H = self.dir / "cpp" / "include" / "raccoon" / "Channels.h"
        self.module.RING_H = self.dir / "cpp" / "include" / "raccoon" / "raccoon_ring.h"

    def hash(self):
        return self.module.contract_hash(self.module.build_contract())

    def edit(self, relative, old, new, count=1):
        path = self.dir / relative
        text = path.read_text()
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, count))

    assertIn = unittest.TestCase().assertIn

    def close(self):
        shutil.rmtree(self.dir, ignore_errors=True)


SCALAR = "messages/include/raccoon/scalar_f_t.hpp"
CHANNELS = "cpp/include/raccoon/Channels.h"
RING = "cpp/include/raccoon/raccoon_ring.h"


class HashTests(unittest.TestCase):
    def setUp(self):
        self.tree = Tree()
        self.addCleanup(self.tree.close)
        self.base = self.tree.hash()

    def test_hash_is_short_hex(self):
        self.assertRegex(self.base, r"^[0-9a-f]{16}$")

    def test_same_sources_same_hash(self):
        self.assertEqual(self.base, self.tree.hash())

    def test_comments_and_whitespace_do_not_change_it(self):
        self.tree.edit(SCALAR, "int64_t timestamp = 0;", "// the time\n        int64_t   timestamp = 0;   /* ns */")
        self.assertEqual(self.base, self.tree.hash())

    def test_default_values_do_not_change_it(self):
        self.tree.edit(SCALAR, "float value = 0.0f;", "float value = 1.5f;")
        self.assertEqual(self.base, self.tree.hash())

    def test_a_new_field_changes_it(self):
        self.tree.edit(SCALAR, "float value = 0.0f;", "float value = 0.0f;\n        int32_t extra = 0;")
        self.assertNotEqual(self.base, self.tree.hash())

    def test_a_changed_field_type_changes_it(self):
        self.tree.edit(SCALAR, "float value", "double value")
        self.assertNotEqual(self.base, self.tree.hash())

    def test_field_order_changes_it(self):
        self.tree.edit(SCALAR, "int64_t timestamp = 0;\n        float value = 0.0f;", "float value = 0.0f;\n        int64_t timestamp = 0;")
        self.assertNotEqual(self.base, self.tree.hash())

    def test_a_new_channel_changes_it(self):
        self.tree.edit(CHANNELS, 'constexpr auto GYRO = "raccoon/gyro/value";',
                       'constexpr auto GYRO = "raccoon/gyro/value";\n    constexpr auto NEW_ONE = "raccoon/new/one";')
        self.assertNotEqual(self.base, self.tree.hash())

    def test_channel_declaration_order_does_not_change_it(self):
        text = (self.tree.dir / CHANNELS).read_text()
        a = '    constexpr auto GYRO = "raccoon/gyro/value";\n'
        b = '    constexpr auto ACCELEROMETER = "raccoon/accel/value";\n'
        self.assertIn(a + b, text)  # adjacent in the real header
        (self.tree.dir / CHANNELS).write_text(text.replace(a + b, b + a))
        self.assertEqual(self.base, self.tree.hash())

    def test_ring_version_changes_it(self):
        self.tree.edit(RING, "#define RRB_VERSION    3u", "#define RRB_VERSION    4u")
        self.assertNotEqual(self.base, self.tree.hash())

    def test_unsupported_type_is_an_error(self):
        self.tree.edit(SCALAR, "float value", "long double value")
        with self.assertRaises(self.tree.module.ContractError):
            self.tree.hash()


class RepoTests(unittest.TestCase):
    """The checked-in generated files agree with the sources and each other."""

    def setUp(self):
        self.module = load()
        self.contract = self.module.build_contract()
        self.digest = self.module.contract_hash(self.contract)

    def test_generated_files_are_current(self):
        for path, content in self.module.generated_files(self.contract).items():
            self.assertTrue(path.exists(), f"{path} missing, run scripts/contract.py --write")
            self.assertEqual(content, path.read_text(), f"{path} is stale, run scripts/contract.py --write")

    def test_every_language_carries_the_same_hash(self):
        cpp = re.search(r'kHash = "([0-9a-f]+)"', self.module.CPP_OUT.read_text()).group(1)
        py = re.search(r'CONTRACT_HASH = "([0-9a-f]+)"', self.module.PY_OUT.read_text()).group(1)
        dart = re.search(r"raccoonContractHash = '([0-9a-f]+)'", self.module.DART_OUT.read_text()).group(1)
        self.assertEqual({self.digest}, {cpp, py, dart})

    def test_python_and_dart_messages_match_cpp(self):
        self.assertEqual([], self.module.check_drift(self.contract))

    def test_python_package_exposes_the_hash(self):
        import sys

        sys.path.insert(0, str(ROOT / "python"))
        try:
            import raccoon_transport as rt
        finally:
            sys.path.pop(0)
        self.assertEqual(self.digest, rt.contract_hash())
        self.assertEqual(f"RACCOON_CONTRACT_V1={self.digest}", rt.CONTRACT_MARKER)


if __name__ == "__main__":
    unittest.main()
