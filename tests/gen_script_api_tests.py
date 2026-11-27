"""Exercise API refreshes without the research checkout or network access."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parent.parent


def prop(name, category="Primitive", kind="float", security="None"):
    return {"Name": name, "MemberType": "Property", "ValueType": {"Category": category, "Name": kind},
            "Security": {"Read": security, "Write": security}}


def cls(name, members=(), base="Instance", tags=()):
    return {"Name": name, "Superclass": base, "Members": list(members), "Tags": list(tags)}


def enum(name, **items):
    return {"Name": name, "Items": [{"Name": k, "Value": v} for k, v in items.items()]}


class ApiRefreshTest(unittest.TestCase):
    def generate(self, listing):
        with tempfile.TemporaryDirectory() as folder:
            folder = pathlib.Path(folder)
            (folder / "apidump").mkdir()
            baseline = {
                "Classes": [cls("Instance", base="<<<ROOT>>>"),
                            cls("Widget", [prop("Changed"), prop("Removed")]), cls("Retired"),
                            cls("StudioOnly")],
                "Enums": [enum("KeyCode", **{"None": 0}), enum("Existing", First=1), enum("RetiredEnum", Old=0)],
            }
            current = {
                "Classes": [cls("Instance", base="<<<ROOT>>>"),
                            cls("Widget", [prop("Changed", "Enum", "Existing", "RobloxScriptSecurity"),
                                           prop("Added"), prop("AbsentFromPlayer")]),
                            cls("StudioOnly"), cls("FutureService", tags=["Service", "NotCreatable"])],
                "Enums": [enum("KeyCode", **{"None": 0}), enum("Existing", First=1, Second=2),
                          enum("FutureEnum", New=7)],
            }
            reflection = {
                "Instance": {"base": None, "members": {}},
                "Widget": {"base": "Instance", "creatable": True,
                           "members": {"Changed": {"kind": "PropDescriptor", "type": "float"},
                                       "Removed": {"kind": "PropDescriptor", "type": "float"},
                                       "PrivatePlayerMember": {"kind": "PropDescriptor", "type": "bool"}}},
                "Retired": {"base": "Instance", "members": {}},
            }
            files = {"enums.json": {"KeyCode": {"None": 0}, "Existing": {"First": 1},
                                     "RetiredEnum": {"Old": 0}},
                     "reflection.json": reflection, "apidump/API-Dump-0.742.0.7421053.json": baseline,
                     "current.json": current}
            for name, data in files.items():
                (folder / name).write_text(json.dumps(data))
            (folder / "strings.txt").write_text(listing)
            output = folder / "RobloxApiData.cpp"
            run = subprocess.run([sys.executable, str(ROOT / "tools/gen_script_api.py"), str(folder),
                                  "--api-dump", str(folder / "current.json"),
                                  "--player-strings", str(folder / "strings.txt"), "--output", str(output)],
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            return output.read_text()

    def check_refresh(self, output):
        self.assertIn('{"Changed", MemberKind::Property, "Enum.Existing", 4, 4}', output)
        self.assertIn('{"Added", MemberKind::Property, "number"}', output)
        self.assertIn('{"PrivatePlayerMember", MemberKind::Property, "boolean"}', output)
        self.assertIn('{"Second", 2u}', output)
        self.assertIn('{"FutureEnum",', output)
        self.assertIn('{"FutureService", "Instance",', output)
        self.assertIn('ClassService}', output)
        for name in ("Removed", "Retired", "RetiredEnum", "StudioOnly", "AbsentFromPlayer"):
            self.assertNotIn('"' + name + '"', output)

    def test_plain_player_strings_refresh_types_security_members_and_enums(self):
        self.check_refresh(self.generate("Added\nFutureService\nFutureEnum\nStudioOnly\n"))

    def test_spelunk_player_strings_have_the_same_result(self):
        self.check_refresh(self.generate("0x1000|Added\n0x1008|FutureService\n0x1010|FutureEnum\n0x1020|StudioOnly\n"))


if __name__ == "__main__":
    unittest.main()
