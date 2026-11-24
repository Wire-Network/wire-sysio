"""Focused tests for deterministic system-contract type generation."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


GENERATOR_PATH = Path(__file__).parents[1] / "generate-sysio-contract-types.py"
GENERATOR_SPEC = importlib.util.spec_from_file_location("generate_sysio_contract_types", GENERATOR_PATH)
if GENERATOR_SPEC is None or GENERATOR_SPEC.loader is None:
    raise RuntimeError(f"Unable to load generator from {GENERATOR_PATH}")
GENERATOR = importlib.util.module_from_spec(GENERATOR_SPEC)
GENERATOR_SPEC.loader.exec_module(GENERATOR)


class GenerateSysioContractTypesTests(unittest.TestCase):
    """Verify optional fields and reproducible output from the ABI generator."""

    def setUp(self) -> None:
        """Create one representative ABI for each test."""
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)
        self.abi_path = self.root / "sysio.example.abi"
        self.abi_path.write_text(
            json.dumps(
                {
                    "version": "sysio::abi/1.2",
                    "types": [{"new_type_name": "extension_alias", "type": "uint64$"}],
                    "structs": [
                        {
                            "name": "example",
                            "base": "",
                            "fields": [
                                {"name": "required_value", "type": "uint64"},
                                {"name": "extension_value", "type": "extension_alias"},
                                {"name": "nullable_value", "type": "string?"},
                            ],
                        }
                    ],
                    "actions": [{"name": "example", "type": "example", "ricardian_contract": ""}],
                    "tables": [],
                }
            ),
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        """Remove generated fixtures."""
        self.temporary_directory.cleanup()

    def test_binary_extensions_and_optionals_are_not_required(self) -> None:
        """Emit `$` and `?` fields as optional while retaining their inner types."""
        output = self.root / "output"
        GENERATOR.generate([str(self.abi_path)], str(output), "snake")

        generated_types = (output / "typescript" / "SysioContractTypes.ts").read_text(encoding="utf-8")
        self.assertIn("required_value: number | string", generated_types)
        self.assertIn("extension_value?: number | string", generated_types)
        self.assertIn("nullable_value?: string | null", generated_types)
        self.assertTrue(generated_types.endswith("\n"))
        self.assertFalse(generated_types.endswith("\n\n"))

        schema = json.loads((output / "schema" / "system-contracts.schema.json").read_text(encoding="utf-8"))
        example_schema = schema["definitions"]["sysio.example::example"]
        self.assertEqual(["required_value"], example_schema["required"])

    def test_generation_is_byte_for_byte_deterministic(self) -> None:
        """Produce byte-identical TypeScript and schema outputs from identical ABIs."""
        first_output = self.root / "first"
        second_output = self.root / "second"
        GENERATOR.generate([str(self.abi_path)], str(first_output), "snake")
        GENERATOR.generate([str(self.abi_path)], str(second_output), "snake")

        for relative_path in (
            Path("typescript/SysioContractTypes.ts"),
            Path("schema/system-contracts.schema.json"),
        ):
            self.assertEqual(
                (first_output / relative_path).read_bytes(),
                (second_output / relative_path).read_bytes(),
            )



class TableBooleanDescriptorTests(unittest.TestCase):
    """Verify where generated table descriptors locate ABI `bool` values."""

    BOOLEAN = {"kind": GENERATOR.BOOLEAN_KIND}

    @staticmethod
    def structs(*definitions: dict) -> dict:
        """Index ABI struct definitions by name, as the generator does."""
        return {definition["name"]: definition for definition in definitions}

    def test_descriptors_cover_bases_optionals_and_struct_arrays(self) -> None:
        """Locate bools in a base struct, an optional field and an array of structs; skip tables without bools."""
        structs = self.structs(
            {"name": "row_base", "base": "", "fields": [{"name": "flag", "type": "bool"}]},
            {"name": "inner", "base": "", "fields": [{"name": "ok", "type": "bool"}, {"name": "n", "type": "uint64"}]},
            {
                "name": "row",
                "base": "row_base",
                "fields": [
                    {"name": "active", "type": "bool?"},
                    {"name": "items", "type": "inner[]"},
                    {"name": "count", "type": "uint64"},
                ],
            },
            {"name": "plain", "base": "", "fields": [{"name": "count", "type": "uint64"}]},
        )
        abi = {"tables": [{"name": "rows", "type": "row"}, {"name": "plain", "type": "plain"}]}

        descriptors = GENERATOR.table_boolean_descriptors(abi, structs, {}, {})

        self.assertEqual(
            {
                "rows": {
                    "kind": GENERATOR.STRUCT_KIND,
                    "fields": {
                        "flag": self.BOOLEAN,
                        "active": self.BOOLEAN,
                        "items": {
                            "kind": GENERATOR.ARRAY_KIND,
                            "items": {"kind": GENERATOR.STRUCT_KIND, "fields": {"ok": self.BOOLEAN}},
                        },
                    },
                }
            },
            descriptors,
        )

    def test_recursive_structs_and_variant_bools_are_refused(self) -> None:
        """Fail generation rather than emit a descriptor that would leave a bool unconverted."""
        recursive = self.structs({"name": "node", "base": "", "fields": [{"name": "next", "type": "node?"}]})
        with self.assertRaises(ValueError):
            GENERATOR.boolean_descriptor("node", recursive, {}, {})

        variants = {"choice": {"name": "choice", "types": ["uint64", "bool"]}}
        with self.assertRaises(ValueError):
            GENERATOR.boolean_descriptor("choice", {}, {}, variants)

    def test_generated_contract_definition_carries_table_booleans(self) -> None:
        """Emit the table descriptor on the contract definition."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        abi_path = root / "sysio.flags.abi"
        abi_path.write_text(
            json.dumps(
                {
                    "version": "sysio::abi/1.2",
                    "structs": [{"name": "flag_row", "base": "", "fields": [{"name": "set", "type": "bool"}]}],
                    "actions": [],
                    "tables": [{"name": "flags", "type": "flag_row", "index_type": "i64"}],
                }
            ),
            encoding="utf-8",
        )
        GENERATOR.generate([str(abi_path)], str(root / "output"), "snake")
        generated = (root / "output" / "typescript" / "SysioContractTypes.ts").read_text(encoding="utf-8")
        self.assertIn("tableBooleans:", generated)
        self.assertIn(f"{GENERATOR.BOOLEAN_DESCRIPTOR_KIND}.{GENERATOR.BOOLEAN_KIND}", generated)


if __name__ == "__main__":
    unittest.main()
