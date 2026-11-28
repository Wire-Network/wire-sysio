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
        recursive = self.structs(
            {
                "name": "node",
                "base": "",
                "fields": [{"name": "flag", "type": "bool"}, {"name": "next", "type": "node?"}],
            }
        )
        with self.assertRaises(ValueError):
            GENERATOR.boolean_descriptor("node", recursive, {}, {})

        variants = {"choice": {"name": "choice", "types": ["uint64", "bool"]}}
        with self.assertRaises(ValueError):
            GENERATOR.boolean_descriptor("choice", {}, {}, variants)

    def test_fixed_size_arrays_are_described_as_arrays(self) -> None:
        """Describe `T[N]` like `T[]`, which the node also decodes to a JSON array, including through an alias."""
        structs = self.structs(
            {"name": "inner", "base": "", "fields": [{"name": "ok", "type": "bool"}]},
            {
                "name": "row",
                "base": "",
                "fields": [
                    {"name": "flags", "type": "bool[2]"},
                    {"name": "inners", "type": "inner[3]?"},
                    {"name": "pairs", "type": "pair[]"},
                    {"name": "counts", "type": "uint64[4]"},
                ],
            },
        )
        aliases = {"pair": "bool[2]"}
        abi = {"tables": [{"name": "rows", "type": "row"}]}

        descriptors = GENERATOR.table_boolean_descriptors(abi, structs, aliases, {})

        bool_array = {"kind": GENERATOR.ARRAY_KIND, "items": self.BOOLEAN}
        self.assertEqual(
            {
                "rows": {
                    "kind": GENERATOR.STRUCT_KIND,
                    "fields": {
                        "flags": bool_array,
                        "inners": {
                            "kind": GENERATOR.ARRAY_KIND,
                            "items": {"kind": GENERATOR.STRUCT_KIND, "fields": {"ok": self.BOOLEAN}},
                        },
                        "pairs": {"kind": GENERATOR.ARRAY_KIND, "items": bool_array},
                    },
                }
            },
            descriptors,
        )

    def test_fixed_size_array_bools_in_variants_are_refused(self) -> None:
        """Refuse a variant alternative holding a fixed-size `bool` array, as for any other bool in a variant."""
        structs = self.structs({"name": "row", "base": "", "fields": [{"name": "value", "type": "choice"}]})
        variants = {"choice": {"name": "choice", "types": ["uint64", "bool[2]"]}}
        abi = {"tables": [{"name": "rows", "type": "row"}]}

        with self.assertRaisesRegex(ValueError, r"bool\[2\]"):
            GENERATOR.table_boolean_descriptors(abi, structs, {}, variants)

    def test_duplicate_field_names_take_the_last_declaration(self) -> None:
        """Describe a field name shared across a base chain by its last declaration, which the JSON row keeps."""
        abi = {"tables": [{"name": "rows", "type": "row"}]}

        bool_base = self.structs(
            {"name": "row_base", "base": "", "fields": [{"name": "value", "type": "bool"}]},
            {"name": "row", "base": "row_base", "fields": [{"name": "value", "type": "uint8"}]},
        )
        self.assertEqual({}, GENERATOR.table_boolean_descriptors(abi, bool_base, {}, {}))

        bool_derived = self.structs(
            {"name": "row_base", "base": "", "fields": [{"name": "value", "type": "uint8"}]},
            {"name": "row", "base": "row_base", "fields": [{"name": "value", "type": "bool"}]},
        )
        self.assertEqual(
            {"rows": {"kind": GENERATOR.STRUCT_KIND, "fields": {"value": self.BOOLEAN}}},
            GENERATOR.table_boolean_descriptors(abi, bool_derived, {}, {}),
        )

        same_struct = self.structs(
            {
                "name": "row",
                "base": "",
                "fields": [{"name": "value", "type": "bool"}, {"name": "value", "type": "uint8"}],
            }
        )
        self.assertEqual({}, GENERATOR.table_boolean_descriptors(abi, same_struct, {}, {}))

        same_extension = self.structs(
            {"name": "row_base", "base": "", "fields": [{"name": "value", "type": "bool"}]},
            {"name": "row", "base": "row_base", "fields": [{"name": "value", "type": "bool$"}]},
        )
        self.assertEqual(
            {"rows": {"kind": GENERATOR.STRUCT_KIND, "fields": {"value": self.BOOLEAN}}},
            GENERATOR.table_boolean_descriptors(abi, same_extension, {}, {}),
        )

        # An absent extension leaves the base's bool in the row, a present one its uint8.
        differing_extension = self.structs(
            {"name": "row_base", "base": "", "fields": [{"name": "value", "type": "bool"}]},
            {"name": "row", "base": "row_base", "fields": [{"name": "value", "type": "uint8$"}]},
        )
        with self.assertRaisesRegex(ValueError, "binary extension field value"):
            GENERATOR.table_boolean_descriptors(abi, differing_extension, {}, {})

    def test_recursive_types_without_bools_are_skipped(self) -> None:
        """Describe rows that reach a recursive variant or struct holding no bool, and refuse a bool past the cycle."""
        structs = self.structs(
            {"name": "entry", "base": "", "fields": [{"name": "children", "type": "choice[]"}]},
            {
                "name": "node",
                "base": "",
                "fields": [{"name": "value", "type": "uint64"}, {"name": "next", "type": "node?"}],
            },
            {
                "name": "row",
                "base": "",
                "fields": [
                    {"name": "flag", "type": "bool"},
                    {"name": "choice", "type": "choice"},
                    {"name": "entry", "type": "entry"},
                    {"name": "node", "type": "node"},
                ],
            },
        )
        recursive_variant = {"choice": {"name": "choice", "types": ["uint64", "choice[]"]}}
        self.assertIsNone(GENERATOR.boolean_descriptor("choice", {}, {}, recursive_variant))

        variants = {"choice": {"name": "choice", "types": ["choice[]", "uint64", "entry"]}}
        abi = {"tables": [{"name": "rows", "type": "row"}]}

        self.assertEqual(
            {"rows": {"kind": GENERATOR.STRUCT_KIND, "fields": {"flag": self.BOOLEAN}}},
            GENERATOR.table_boolean_descriptors(abi, structs, {}, variants),
        )

        # The cyclic alternative comes first; the alternatives after it are still inspected.
        bool_after_cycle = {"choice": {"name": "choice", "types": ["choice[]", "bool"]}}
        with self.assertRaisesRegex(ValueError, "alternative bool"):
            GENERATOR.boolean_descriptor("choice", {}, {}, bool_after_cycle)

        # A variant alternative that cycles back to the row holds the row's bool.
        back_to_row = {"choice": {"name": "choice", "types": ["uint64", "row"]}}
        with self.assertRaisesRegex(ValueError, "alternative row"):
            GENERATOR.table_boolean_descriptors(abi, structs, {}, back_to_row)

    def test_zero_length_fixed_arrays_hold_no_bools(self) -> None:
        """Treat `T[0]`, which the node always decodes to `[]`, as holding no bool in descriptors and variants."""
        abi = {"tables": [{"name": "rows", "type": "row"}]}
        flag_only = {"rows": {"kind": GENERATOR.STRUCT_KIND, "fields": {"flag": self.BOOLEAN}}}

        variant_row = self.structs(
            {
                "name": "row",
                "base": "",
                "fields": [{"name": "value", "type": "choice"}, {"name": "flag", "type": "bool"}],
            }
        )
        variants = {"choice": {"name": "choice", "types": ["uint64", "bool[0]"]}}
        self.assertEqual(flag_only, GENERATOR.table_boolean_descriptors(abi, variant_row, {}, variants))

        recursive_row = self.structs(
            {
                "name": "row",
                "base": "",
                "fields": [{"name": "next", "type": "row[0]"}, {"name": "flag", "type": "bool"}],
            }
        )
        self.assertEqual(flag_only, GENERATOR.table_boolean_descriptors(abi, recursive_row, {}, {}))

        fields_row = self.structs(
            {"name": "inner", "base": "", "fields": [{"name": "ok", "type": "bool"}]},
            {
                "name": "row",
                "base": "",
                "fields": [
                    {"name": "flags", "type": "bool[0]"},
                    {"name": "padded", "type": "bool[00]"},
                    {"name": "inners", "type": "inner[0]?"},
                    {"name": "aliased", "type": "no_flags"},
                    {"name": "flag", "type": "bool"},
                ],
            },
        )
        aliases = {"no_flags": "bool[0]"}
        self.assertEqual(flag_only, GENERATOR.table_boolean_descriptors(abi, fields_row, aliases, {}))

    def test_shadowed_declarations_are_not_inspected(self) -> None:
        """Skip a field declaration that a later required declaration of its name always replaces in the row."""
        abi = {"tables": [{"name": "rows", "type": "row"}]}
        variants = {"choice": {"name": "choice", "types": ["uint64", "bool"]}}

        shadowed_variant = self.structs(
            {"name": "row_base", "base": "", "fields": [{"name": "value", "type": "choice"}]},
            {"name": "row", "base": "row_base", "fields": [{"name": "value", "type": "uint8"}]},
        )
        self.assertEqual({}, GENERATOR.table_boolean_descriptors(abi, shadowed_variant, {}, variants))

        # A struct used as a variant alternative is inspected the same way.
        shadowed_alternative = self.structs(
            {"name": "alt_base", "base": "", "fields": [{"name": "value", "type": "bool"}]},
            {"name": "alt", "base": "alt_base", "fields": [{"name": "value", "type": "uint8"}]},
            {"name": "row", "base": "", "fields": [{"name": "value", "type": "alt_choice"}]},
        )
        alt_variants = {"alt_choice": {"name": "alt_choice", "types": ["uint64", "alt"]}}
        self.assertEqual({}, GENERATOR.table_boolean_descriptors(abi, shadowed_alternative, {}, alt_variants))

        shadowed_recursion = self.structs(
            {"name": "row_base", "base": "", "fields": [{"name": "next", "type": "row?"}]},
            {
                "name": "row",
                "base": "row_base",
                "fields": [{"name": "next", "type": "uint64"}, {"name": "flag", "type": "bool"}],
            },
        )
        self.assertEqual(
            {"rows": {"kind": GENERATOR.STRUCT_KIND, "fields": {"flag": self.BOOLEAN}}},
            GENERATOR.table_boolean_descriptors(abi, shadowed_recursion, {}, {}),
        )

    def test_binary_extension_fallbacks_are_inspected(self) -> None:
        """Inspect an earlier declaration that an absent trailing binary extension field leaves in the row."""
        abi = {"tables": [{"name": "rows", "type": "row"}]}
        variants = {"choice": {"name": "choice", "types": ["uint64", "bool"]}}

        fallback_variant = self.structs(
            {"name": "row_base", "base": "", "fields": [{"name": "value", "type": "choice"}]},
            {"name": "row", "base": "row_base", "fields": [{"name": "value", "type": "uint8$"}]},
        )
        with self.assertRaisesRegex(ValueError, "alternative bool"):
            GENERATOR.table_boolean_descriptors(abi, fallback_variant, {}, variants)

        fallback_alternative = self.structs(
            {"name": "alt_base", "base": "", "fields": [{"name": "value", "type": "bool"}]},
            {"name": "alt", "base": "alt_base", "fields": [{"name": "value", "type": "uint8$"}]},
            {"name": "row", "base": "", "fields": [{"name": "value", "type": "alt_choice"}]},
        )
        alt_variants = {"alt_choice": {"name": "alt_choice", "types": ["uint64", "alt"]}}
        with self.assertRaisesRegex(ValueError, "alternative alt"):
            GENERATOR.table_boolean_descriptors(abi, fallback_alternative, {}, alt_variants)

        # Every declaration an absent extension can expose must agree.
        agreeing_chain = self.structs(
            {"name": "row_base", "base": "", "fields": [{"name": "value", "type": "bool"}]},
            {"name": "row_mid", "base": "row_base", "fields": [{"name": "value", "type": "bool?$"}]},
            {"name": "row", "base": "row_mid", "fields": [{"name": "value", "type": "bool$"}]},
        )
        self.assertEqual(
            {"rows": {"kind": GENERATOR.STRUCT_KIND, "fields": {"value": self.BOOLEAN}}},
            GENERATOR.table_boolean_descriptors(abi, agreeing_chain, {}, {}),
        )
        disagreeing_chain = self.structs(
            {"name": "row_base", "base": "", "fields": [{"name": "value", "type": "uint8"}]},
            {"name": "row_mid", "base": "row_base", "fields": [{"name": "value", "type": "bool$"}]},
            {"name": "row", "base": "row_mid", "fields": [{"name": "value", "type": "bool$"}]},
        )
        with self.assertRaisesRegex(ValueError, "binary extension field value"):
            GENERATOR.table_boolean_descriptors(abi, disagreeing_chain, {}, {})

        # A later required declaration replaces every earlier one, binary extensions included.
        required_last = self.structs(
            {"name": "row_base", "base": "", "fields": [{"name": "value", "type": "choice"}]},
            {"name": "row_mid", "base": "row_base", "fields": [{"name": "value", "type": "bool$"}]},
            {"name": "row", "base": "row_mid", "fields": [{"name": "value", "type": "uint8"}]},
        )
        self.assertEqual({}, GENERATOR.table_boolean_descriptors(abi, required_last, {}, variants))

        # An extension followed by a required field is never absent from a decoded row, so it shadows.
        extension_then_required = self.structs(
            {"name": "row_base", "base": "", "fields": [{"name": "value", "type": "choice"}]},
            {
                "name": "row",
                "base": "row_base",
                "fields": [{"name": "value", "type": "uint8$"}, {"name": "count", "type": "uint64"}],
            },
        )
        self.assertEqual({}, GENERATOR.table_boolean_descriptors(abi, extension_then_required, {}, variants))

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
