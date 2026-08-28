#!/usr/bin/env python3
"""Assert the Solana OPP outpost IDL stub still matches the generated program IDL.

``tests/fixtures/solana-idl-opp-outpost-stub.json`` is a hand-maintained mirror
of the Solana program's generated Anchor IDL. The relay's packet-budget tests
measure transaction sizes against that stub, so a stub that has fallen behind
the real program lets those tests stay green while production transactions
overflow Solana's 1232-byte packet limit -- precisely the failure the
``dispatch_limit`` incident produced, where adding one 4-byte argument pushed a
full ``epoch_in`` data chunk to 1234 bytes.

The comparison is a SUBSET check: the stub may omit program surface it does not
need, but everything it *does* declare must match the generated IDL exactly --
instruction discriminators, argument names and types in order, account lists in
order with their ``writable``/``signer``/``optional`` flags, account
discriminators, and type definitions.

The generated IDL lives in the sibling ``wire-solana`` repository and is a build
artifact of its Anchor workspace. When it is absent -- a standalone wire-sysio
checkout, or a wire-solana that has not been built -- the check SKIPS and says
exactly what went unverified. It never fails for a missing sibling and never
passes silently as though it had verified something.

The wire-solana checkout is resolved from ``--solana-path`` first, then the
``WIRE_SOLANA_PATH`` environment variable, then the sibling layout.

Exit codes:
    0 -- every declaration in the stub matched, or the check skipped (see above)
    1 -- at least one divergence between the stub and the generated IDL
    2 -- an input is missing or unreadable: the stub is absent, or either the
         stub or the generated IDL is not valid JSON
"""

import argparse
import json
import os
import sys
from pathlib import Path

#: Program name the stub mirrors. The standalone ``opp_outpost`` program was
#: folded into ``liqsol_core``, which now hosts the whole OPP outpost surface.
GENERATED_IDL_RELATIVE = Path("target") / "idl" / "liqsol_core.json"

#: Default location of the sibling wire-solana checkout, relative to the
#: wire-sysio source root.
DEFAULT_SOLANA_RELATIVE = Path("..") / "wire-solana"


def parse_arguments():
    """Parse the stub and sibling-repository paths."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--stub",
        required=True,
        type=Path,
        help="path to solana-idl-opp-outpost-stub.json",
    )
    parser.add_argument(
        "--solana-path",
        type=Path,
        default=None,
        help="wire-solana checkout holding the generated IDL "
        "(falls back to env WIRE_SOLANA_PATH, then the sibling layout)",
    )
    return parser.parse_args()


def resolve_solana_path(argument, stub_path):
    """Resolve the wire-solana checkout.

    Precedence is explicit argument, then ``WIRE_SOLANA_PATH``, then the sibling
    layout. The argument MUST win: the ctest registration passes the correct
    checkout explicitly, while several build-system workflows export
    ``WIRE_SOLANA_PATH`` job-wide. If the environment took precedence, any job
    whose variable points at an unbuilt checkout would silently turn this gate
    into a permanent green no-op via the skip path.
    """
    if argument is not None:
        return argument
    override = os.environ.get("WIRE_SOLANA_PATH")
    if override:
        return Path(override)
    # tests/fixtures/<stub> -> the wire-sysio source root is two levels up.
    return stub_path.resolve().parent.parent.parent / DEFAULT_SOLANA_RELATIVE


def load_json(path, label):
    """Read and parse a JSON input, or exit 2 with a diagnostic.

    A malformed input is an unreadable input: the check cannot verify anything,
    which is neither a clean pass nor a divergence. Reporting it as a bare
    ``JSONDecodeError`` traceback would leave the reader to work out which of
    the two files failed and where -- a traceback is not a diagnostic.
    """
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError as error:
        print(
            f"FAILED: {label} at {path} is not valid JSON "
            f"(line {error.lineno}, column {error.colno}): {error.msg}",
            file=sys.stderr,
        )
        sys.exit(2)
    except OSError as error:
        print(f"FAILED: cannot read {label} at {path}: {error}", file=sys.stderr)
        sys.exit(2)


def skip(reason):
    """Report an unverified run loudly and exit successfully."""
    print("=" * 78)
    print("SKIPPED: solana_idl_stub_consistency_test did NOT verify anything.")
    print(reason)
    print("The stub was NOT checked against the generated IDL in this run.")
    print("=" * 78)
    sys.exit(0)


def without_docs(value):
    """Strip ``docs`` entries recursively.

    Anchor emits doc comments into the generated IDL; the stub does not carry
    them. They describe a declaration, they are not part of its wire shape, so
    comparing them would report prose edits as drift.
    """
    if isinstance(value, dict):
        return {
            key: without_docs(inner)
            for key, inner in value.items()
            if key != "docs"
        }
    if isinstance(value, list):
        return [without_docs(item) for item in value]
    return value


def accounts_of(instruction):
    """Return an instruction's account list as comparable tuples, in order."""
    return [
        (
            account.get("name"),
            bool(account.get("writable", False)),
            bool(account.get("signer", False)),
            bool(account.get("optional", False)),
        )
        for account in instruction.get("accounts", [])
    ]


def args_of(instruction):
    """Return an instruction's argument list as comparable tuples, in order."""
    return [
        (argument.get("name"), json.dumps(argument.get("type"), sort_keys=True))
        for argument in instruction.get("args", [])
    ]


def by_name(entries):
    """Index a list of IDL entries by their ``name`` field."""
    return {entry.get("name"): entry for entry in entries}


def compare_instructions(stub, generated, divergences):
    """Compare every instruction the stub declares against the generated IDL."""
    generated_instructions = by_name(generated.get("instructions", []))
    for instruction in stub.get("instructions", []):
        name = instruction.get("name")
        found = generated_instructions.get(name)
        if found is None:
            divergences.append(
                f"instruction '{name}': declared in the stub but ABSENT from the "
                f"generated IDL"
            )
            continue
        if instruction.get("discriminator") != found.get("discriminator"):
            divergences.append(
                f"instruction '{name}': discriminator "
                f"{instruction.get('discriminator')} != generated "
                f"{found.get('discriminator')}"
            )
        stub_args, generated_args = args_of(instruction), args_of(found)
        if stub_args != generated_args:
            divergences.append(
                f"instruction '{name}': args differ\n"
                f"      stub      ({len(stub_args)}): {stub_args}\n"
                f"      generated ({len(generated_args)}): {generated_args}"
            )
        stub_accounts, generated_accounts = accounts_of(instruction), accounts_of(found)
        if stub_accounts != generated_accounts:
            divergences.append(
                f"instruction '{name}': accounts differ "
                f"(name, writable, signer, optional)\n"
                f"      stub      ({len(stub_accounts)}): {stub_accounts}\n"
                f"      generated ({len(generated_accounts)}): {generated_accounts}"
            )


def compare_accounts(stub, generated, divergences):
    """Compare every account the stub declares against the generated IDL."""
    generated_accounts = by_name(generated.get("accounts", []))
    for account in stub.get("accounts", []):
        name = account.get("name")
        found = generated_accounts.get(name)
        if found is None:
            divergences.append(
                f"account '{name}': declared in the stub but ABSENT from the "
                f"generated IDL"
            )
            continue
        if account.get("discriminator") != found.get("discriminator"):
            divergences.append(
                f"account '{name}': discriminator {account.get('discriminator')} "
                f"!= generated {found.get('discriminator')}"
            )


def compare_struct_fields(name, stub_type, generated_type, divergences, omissions):
    """Subset-match a struct's fields against the generated definition.

    A stub struct may omit fields it does not need, but every field it DOES
    declare must carry the generated type and keep the generated relative
    order -- Borsh is positional, so a renamed, retyped or reordered field
    silently shifts every decode offset after it. Omissions are recorded for
    the report rather than failed, so a deliberate reduction stays visible
    instead of passing unnoticed.
    """
    generated_fields = generated_type.get("fields", [])
    generated_types_by_name = {
        field.get("name"): json.dumps(without_docs(field.get("type")), sort_keys=True)
        for field in generated_fields
    }
    generated_order = [field.get("name") for field in generated_fields]

    matched_positions = []
    for field in stub_type.get("fields", []):
        field_name = field.get("name")
        stub_field_type = json.dumps(without_docs(field.get("type")), sort_keys=True)
        if field_name not in generated_types_by_name:
            divergences.append(
                f"type '{name}': field '{field_name}' is declared in the stub but "
                f"ABSENT from the generated IDL"
            )
            continue
        if stub_field_type != generated_types_by_name[field_name]:
            divergences.append(
                f"type '{name}': field '{field_name}' type differs\n"
                f"      stub     : {stub_field_type}\n"
                f"      generated: {generated_types_by_name[field_name]}"
            )
        matched_positions.append(generated_order.index(field_name))

    if matched_positions != sorted(matched_positions):
        declared = [field.get("name") for field in stub_type.get("fields", [])]
        divergences.append(
            f"type '{name}': declared fields are OUT OF ORDER relative to the "
            f"generated IDL -- Borsh is positional, so this shifts every "
            f"subsequent decode offset\n"
            f"      stub order     : {declared}\n"
            f"      generated order: {generated_order}"
        )

    omitted = [
        field_name
        for field_name in generated_order
        if field_name not in {f.get("name") for f in stub_type.get("fields", [])}
    ]
    if omitted:
        trailing = generated_order[len(generated_order) - len(omitted):] == omitted
        omissions.append((name, omitted, trailing))


def compare_types(stub, generated, divergences, omissions):
    """Compare every type definition the stub declares against the generated IDL."""
    generated_types = by_name(generated.get("types", []))
    for type_def in stub.get("types", []):
        name = type_def.get("name")
        found = generated_types.get(name)
        if found is None:
            divergences.append(
                f"type '{name}': declared in the stub but ABSENT from the "
                f"generated IDL"
            )
            continue
        stub_type = without_docs(type_def.get("type")) or {}
        generated_type = without_docs(found.get("type")) or {}
        if stub_type.get("kind") != generated_type.get("kind"):
            divergences.append(
                f"type '{name}': kind {stub_type.get('kind')} != generated "
                f"{generated_type.get('kind')}"
            )
            continue
        if stub_type.get("kind") == "struct":
            compare_struct_fields(name, stub_type, generated_type, divergences, omissions)
            continue
        # Enums (and anything else) compare whole: dropping a variant shifts
        # every later discriminant, so there is no meaningful subset of one.
        stub_shape = json.dumps(stub_type, sort_keys=True)
        generated_shape = json.dumps(generated_type, sort_keys=True)
        if stub_shape != generated_shape:
            divergences.append(
                f"type '{name}': definition differs\n"
                f"      stub     : {stub_shape}\n"
                f"      generated: {generated_shape}"
            )


def report_omissions(omissions):
    """Print the fields the stub leaves out of the types it declares.

    A reduced type is permitted -- the stub only needs the fields its consumers
    read -- but it is never silent. A reduction whose omitted fields are not a
    trailing suffix cannot be Borsh-decoded as a prefix of the real account, so
    it is called out separately.
    """
    if not omissions:
        return
    print("\nOMITTED FIELDS (allowed, reported so the reduction stays visible):")
    for name, omitted, trailing in omissions:
        print(f"  type '{name}' omits {len(omitted)}: {omitted}")
        if not trailing:
            print(
                "      ^ these are NOT a trailing suffix, so this type is not a "
                "decodable prefix of the real account: Borsh offsets diverge at "
                "the first omitted field."
            )


def compare_address(stub, generated, divergences):
    """Compare the declared program address."""
    if stub.get("address") != generated.get("address"):
        divergences.append(
            f"program address: stub {stub.get('address')} != generated "
            f"{generated.get('address')}"
        )


def main():
    """Run the subset comparison and report every divergence found."""
    arguments = parse_arguments()

    if not arguments.stub.is_file():
        print(f"FAILED: stub not found at {arguments.stub}", file=sys.stderr)
        return 2
    stub = load_json(arguments.stub, "the stub")

    solana_path = resolve_solana_path(arguments.solana_path, arguments.stub)
    generated_path = solana_path / GENERATED_IDL_RELATIVE
    if not generated_path.is_file():
        skip(
            f"Generated IDL not found at {generated_path}.\n"
            f"It is a build artifact of the sibling wire-solana Anchor workspace; "
            f"build it (anchor build) or set WIRE_SOLANA_PATH to a checkout that "
            f"has one."
        )
    generated = load_json(generated_path, "the generated IDL")

    print(f"stub     : {arguments.stub}")
    print(f"generated: {generated_path}")

    divergences = []
    omissions = []
    compare_address(stub, generated, divergences)
    compare_instructions(stub, generated, divergences)
    compare_accounts(stub, generated, divergences)
    compare_types(stub, generated, divergences, omissions)

    report_omissions(omissions)

    declared = (
        f"{len(stub.get('instructions', []))} instructions, "
        f"{len(stub.get('accounts', []))} accounts, "
        f"{len(stub.get('types', []))} types"
    )
    if not divergences:
        print(f"OK: every declaration in the stub matches the generated IDL ({declared}).")
        return 0

    print(f"\nFAILED: {len(divergences)} divergence(s) between the stub and the "
          f"generated IDL ({declared} declared).\n")
    for index, divergence in enumerate(divergences, start=1):
        print(f"  {index}. {divergence}")
    print(
        "\nThe stub is a mirror of the generated IDL. Refresh the diverging "
        "entries from\n"
        f"{generated_path}, or delete entries the stub no longer needs -- the "
        "relay's\npacket-budget measurements are only meaningful while the two agree."
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
