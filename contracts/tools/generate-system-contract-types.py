#!/usr/bin/env python3
"""
Generate JSON Schema and TypeScript types from Wire system contract ABIs.

Gathers all sysio.*.abi files from <build-dir>/contracts/ and produces:
  - <output-dir>/schema/system-contracts.schema.json
  - <output-dir>/typescript/SysioContractTypes.ts

Type names are always PascalCase (e.g., SysioWrapTransactionHeaderType).
Member names follow the --style flag: snake_case (default) or camelCase.
"""
from __future__ import annotations

import json
import os
import re
import sys
from pathlib import Path

import click


# ---------------------------------------------------------------------------
# Naming helpers
# ---------------------------------------------------------------------------

def to_pascal(s: str) -> str:
    """Convert 'sysio.wrap', 'transaction_header', 'activateroa' to PascalCase."""
    s = re.sub(r'[._]', ' ', s)
    return ''.join(word.capitalize() for word in s.split())


def snake_to_camel(name: str) -> str:
    """Convert snake_case to camelCase."""
    parts = name.split('_')
    return parts[0] + ''.join(p.capitalize() for p in parts[1:])


def convert_member(name: str, style: str) -> str:
    """Convert a member name to the target style."""
    if style == 'camel':
        return snake_to_camel(name)
    return name  # snake_case is the ABI default


def member_variants(name: str, style: str) -> list[str]:
    """Return the list of member names to emit for a field.

    For 'both', returns [snake, camel] deduplicated (single-word names
    like 'ref' produce the same string for both styles).
    """
    if style == 'both':
        snake = name
        camel = snake_to_camel(name)
        if snake == camel:
            return [snake]
        return [snake, camel]
    return [convert_member(name, style)]


# Contracts whose deploy account differs from `sysio.<short>`.
# system + bios both deploy onto the privileged `sysio` account.
DEPLOY_ACCOUNT_OVERRIDES = {'system': 'sysio', 'bios': 'sysio'}


def short_contract_name(contract_name: str) -> str:
    """'sysio.epoch' -> 'epoch'; 'sysio.system' -> 'system'."""
    return contract_name.split('.', 1)[1] if '.' in contract_name else contract_name


def emit_contract_interface(abi: dict, contract_name: str, contract_prefix: str) -> list[str]:
    """Emit `export interface Sysio<Pascal>Contract { actions; tables }`.

    actions: action-name -> its `Action` struct interface (struct name == action name).
    tables:  table-name  -> its row-struct `Type` interface (row struct == table['type']).
    """
    lines = [
        f'/** {contract_name} - action + table surface for the typed contract client. */',
        f'export interface {contract_prefix}Contract {{',
        '  actions: {',
    ]
    for action in abi.get('actions', []):
        action_ts = contract_prefix + to_pascal(action['name']) + 'Action'
        lines.append(f'    {action["name"]}: {action_ts}')
    lines.append('  }')
    lines.append('  tables: {')
    for table in abi.get('tables', []):
        row_ts = contract_prefix + to_pascal(table['type']) + 'Type'
        lines.append(f'    {table["name"]}: {row_ts}')
    lines.append('  }')
    lines.append('}')
    lines.append('')
    return lines


# ---------------------------------------------------------------------------
# ABI type → JSON Schema
# ---------------------------------------------------------------------------

PRIMITIVE_SCHEMA = {
    'bool': {'type': 'boolean'},
    'int8': {'type': 'integer'}, 'uint8': {'type': 'integer'},
    'int16': {'type': 'integer'}, 'uint16': {'type': 'integer'},
    'int32': {'type': 'integer'}, 'uint32': {'type': 'integer'},
    'int64': {'type': 'integer'}, 'uint64': {'type': 'integer'},
    'int128': {'type': 'string'}, 'uint128': {'type': 'string'},
    'float32': {'type': 'number'}, 'float64': {'type': 'number'}, 'float128': {'type': 'string'},
    'name': {'type': 'string', 'pattern': '^[a-z1-5.]{1,13}$'},
    'string': {'type': 'string'},
    'bytes': {'type': 'string', 'description': 'hex-encoded bytes'},
    'checksum256': {'type': 'string', 'pattern': '^[a-f0-9]{64}$'},
    'checksum160': {'type': 'string'}, 'checksum512': {'type': 'string'},
    'public_key': {'type': 'string'}, 'signature': {'type': 'string'},
    'symbol': {'type': 'string'}, 'symbol_code': {'type': 'string'},
    'asset': {'type': 'string', 'pattern': r'^-?[0-9]+\.[0-9]+ [A-Z]+$'},
    'extended_asset': {'type': 'object'},
    'time_point': {'type': 'string', 'format': 'date-time'},
    'time_point_sec': {'type': 'string', 'format': 'date-time'},
    'block_timestamp_type': {'type': 'string'},
    'varuint32': {'type': 'integer'}, 'varint32': {'type': 'integer'},
}


def resolve_type(abi_type: str, type_aliases: dict) -> str:
    """Resolve type aliases."""
    while abi_type in type_aliases:
        abi_type = type_aliases[abi_type]
    return abi_type


def abi_type_to_schema(abi_type: str, structs_map: dict, type_aliases: dict,
                       enums_map: dict | None = None) -> dict:
    """Map an ABI type to a JSON Schema fragment."""
    abi_type = resolve_type(abi_type, type_aliases)
    if abi_type.endswith('[]'):
        return {'type': 'array', 'items': abi_type_to_schema(abi_type[:-2], structs_map, type_aliases, enums_map)}
    if abi_type.endswith('?'):
        return {'oneOf': [abi_type_to_schema(abi_type[:-1], structs_map, type_aliases, enums_map), {'type': 'null'}]}
    if abi_type in PRIMITIVE_SCHEMA:
        return dict(PRIMITIVE_SCHEMA[abi_type])
    if enums_map and abi_type in enums_map:
        return {'$ref': f'#/definitions/enum::{abi_type}'}
    if abi_type in structs_map:
        return {'$ref': f'#/definitions/{abi_type}'}
    return {'type': 'string', 'description': f'ABI type: {abi_type}'}


# ---------------------------------------------------------------------------
# ABI type → TypeScript
# ---------------------------------------------------------------------------

PRIMITIVE_TS = {
    'bool': 'boolean',
    'int8': 'number', 'uint8': 'number', 'int16': 'number', 'uint16': 'number',
    'int32': 'number', 'uint32': 'number',
    # int64-class values exceed Number.MAX_SAFE_INTEGER on the wire; the chain
    # RPC accepts either form, so consumers may pass a decimal string for
    # magnitudes past 2^53 (the wire truth) without casting.
    'int64': 'number | string', 'uint64': 'number | string',
    'int128': 'string', 'uint128': 'string',
    'float32': 'number', 'float64': 'number', 'float128': 'string',
    'name': 'string', 'string': 'string', 'bytes': 'string',
    'checksum256': 'string', 'checksum160': 'string', 'checksum512': 'string',
    'public_key': 'string', 'signature': 'string',
    'symbol': 'string', 'symbol_code': 'string',
    'asset': 'string', 'extended_asset': 'ExtendedAsset',
    'time_point': 'string', 'time_point_sec': 'string',
    'block_timestamp_type': 'string',
    'varuint32': 'number', 'varint32': 'number',
}


def abi_type_to_ts(abi_type: str, structs_map: dict, type_aliases: dict,
                   contract_prefix: str, enums_map: dict | None = None) -> str:
    """Map an ABI type to a TypeScript type string.

    Enum-typed fields emit the WIRE-SPELLING UNION ``Enum | keyof typeof Enum``:
    the ABI accepts (and table reads return) the proto member NAME spelling as
    well as the numeric value, so the generated type admits both — consumers
    assign ``SysioXChainkind.CHAIN_KIND_EVM`` or its ``"CHAIN_KIND_EVM"``
    spelling without casts (see enums-are-first-class).
    """
    abi_type = resolve_type(abi_type, type_aliases)
    if abi_type.endswith('[]'):
        inner = abi_type_to_ts(abi_type[:-2], structs_map, type_aliases, contract_prefix, enums_map)
        # A union inner type must parenthesize: `(A | B)[]`, not `A | B[]`.
        return (f'({inner})[]' if '|' in inner else inner + '[]')
    if abi_type.endswith('?'):
        return abi_type_to_ts(abi_type[:-1], structs_map, type_aliases, contract_prefix, enums_map) + ' | null'
    if abi_type in PRIMITIVE_TS:
        return PRIMITIVE_TS[abi_type]
    if enums_map and abi_type in enums_map:
        enum_base = abi_type[:-2] if abi_type.endswith('_t') else abi_type
        enum_name = contract_prefix + to_pascal(enum_base) + ('Type' if abi_type.endswith('_t') else '')
        return f'{enum_name} | keyof typeof {enum_name}'
    if abi_type in structs_map:
        return contract_prefix + to_pascal(abi_type) + 'Type'
    return 'unknown'


# ---------------------------------------------------------------------------
# Main generation
# ---------------------------------------------------------------------------

def generate(abi_files: list[str], output_dir: str, style: str) -> None:
    """Process all ABIs and write JSON Schema + TypeScript output."""
    os.makedirs(os.path.join(output_dir, 'schema'), exist_ok=True)
    os.makedirs(os.path.join(output_dir, 'typescript'), exist_ok=True)

    all_definitions: dict = {}
    contract_registry: list[tuple[str, str, list[str], list[str]]] = []   # (name, prefix, action_names, table_names)
    ts_lines: list[str] = [
        '// noinspection SpellCheckingInspection',
        '/**',
        ' * Auto-generated TypeScript types for Wire system contracts.',
        ' * Generated by generate-system-contract-types.py',
        ' *',
        f' * Property naming style: {style}',
        ' */',
        '',
        '/** Extended asset type. */',
        'export interface ExtendedAsset {',
        f'  {convert_member("quantity", style)}: string',
        f'  {convert_member("contract", style)}: string',
        '}',
        '',
    ]

    for abi_path in abi_files:
        with open(abi_path) as f:
            abi = json.load(f)

        contract_name = os.path.basename(abi_path).replace('.abi', '')
        contract_prefix = to_pascal(contract_name)

        structs_map = {s['name']: s for s in abi.get('structs', [])}
        action_names = {a['name'] for a in abi.get('actions', [])}
        type_aliases = {t['new_type_name']: t['type'] for t in abi.get('types', [])}

        enums_map = {e['name']: e for e in abi.get('enums', [])}

        ts_lines.append(f'// ── {contract_name} ──')
        ts_lines.append('')

        # Enums
        for enum_def in abi.get('enums', []):
            ename = enum_def['name']
            values = enum_def.get('values', [])
            underlying = enum_def.get('type', 'uint8')

            # Strip trailing _t and append Type (chain_kind_t -> ChainKindType)
            enum_base = ename[:-2] if ename.endswith('_t') else ename
            ts_enum_name = contract_prefix + to_pascal(enum_base) + ('Type' if ename.endswith('_t') else '')
            schema_key = f'{contract_name}::enum::{ename}'

            # JSON Schema — integer enum with const descriptions
            enum_values = [v['value'] for v in values]
            enum_names = [v['name'] for v in values]
            schema_def: dict = {
                'type': 'integer',
                'enum': enum_values,
                'description': f'Enum {ename} ({underlying}): ' + ', '.join(
                    f'{v["name"]}={v["value"]}' for v in values
                ),
            }
            all_definitions[schema_key] = schema_def

            # TypeScript — const enum
            ts_lines.append(f'/** {contract_name}::{ename} (enum, {underlying}) */')
            ts_lines.append(f'export enum {ts_enum_name} {{')
            # Strip enum base name prefix from value names
            # e.g. chain_kind_t value "chain_kind_ethereum" -> "ethereum"
            enum_prefix = enum_base + '_'
            for v in values:
                vname = v['name']
                if vname.startswith(enum_prefix):
                    vname = vname[len(enum_prefix):]
                ts_lines.append(f'  {vname} = {v["value"]},')
            ts_lines.append('}')
            ts_lines.append('')

        for struct in abi.get('structs', []):
            sname = struct['name']
            fields = struct.get('fields', [])
            suffix = 'Action' if sname in action_names else 'Type'

            # Type name is always PascalCase regardless of --style
            ts_name = contract_prefix + to_pascal(sname) + suffix
            schema_key = f'{contract_name}::{sname}'

            # JSON Schema
            properties = {}
            required = []
            for field in fields:
                ftype = field['type']
                schema_val = abi_type_to_schema(ftype, structs_map, type_aliases, enums_map)
                resolved = resolve_type(ftype, type_aliases)
                for member in member_variants(field['name'], style):
                    properties[member] = dict(schema_val)
                    # In 'both' mode all properties are optional
                    if style != 'both' and not resolved.endswith('?'):
                        required.append(member)

            schema_def: dict = {'type': 'object', 'properties': properties}
            if required:
                schema_def['required'] = required
            all_definitions[schema_key] = schema_def

            # TypeScript
            kind_label = 'action' if sname in action_names else 'type'
            ts_lines.append(f'/** {contract_name}::{sname} ({kind_label}) */')
            ts_lines.append(f'export interface {ts_name} {{')
            for field in fields:
                ftype = field['type']
                resolved = resolve_type(ftype, type_aliases)
                ts_type = abi_type_to_ts(ftype, structs_map, type_aliases, contract_prefix, enums_map)
                for member in member_variants(field['name'], style):
                    # In 'both' mode every property is optional
                    if style == 'both':
                        optional = '?'
                    else:
                        optional = '?' if resolved.endswith('?') else ''
                    ts_lines.append(f'  {member}{optional}: {ts_type}')
            ts_lines.append('}')
            ts_lines.append('')

        # ── typed-client surface: Sysio<Pascal>Contract (actions + tables) ──
        ts_lines.extend(emit_contract_interface(abi, contract_name, contract_prefix))
        contract_registry.append((contract_name, contract_prefix,
            [a['name'] for a in abi.get('actions', [])], [t['name'] for t in abi.get('tables', [])]))

    # ── contract registry: SysioContractName + Account + Mapping + Definitions ──
    ts_lines.append('// ── contract registry ──')
    ts_lines.append('')
    ts_lines.append('/** Short contract name - key into SysioContractMapping / SysioContractAccount. */')
    ts_lines.append('export enum SysioContractName {')
    for contract_name, *_ in contract_registry:
        short = short_contract_name(contract_name)
        ts_lines.append(f'  {short} = "{short}",')
    ts_lines.append('}')
    ts_lines.append('')
    ts_lines.append('/** On-chain account per contract (default sysio.<name>; system/bios -> sysio). */')
    ts_lines.append('export const SysioContractAccount: Record<SysioContractName, string> = {')
    for contract_name, *_ in contract_registry:
        short = short_contract_name(contract_name)
        account = DEPLOY_ACCOUNT_OVERRIDES.get(short, contract_name)
        ts_lines.append(f'  [SysioContractName.{short}]: "{account}",')
    ts_lines.append('}')
    ts_lines.append('')
    ts_lines.append('/** Each contract -> its action+table surface, for the typed contract client. */')
    ts_lines.append('export interface SysioContractMapping {')
    for contract_name, contract_prefix, *_ in contract_registry:
        short = short_contract_name(contract_name)
        ts_lines.append(f'  [SysioContractName.{short}]: {contract_prefix}Contract')
    ts_lines.append('}')
    ts_lines.append('')
    ts_lines.append('/** Runtime action/table names + account per contract (the typed client validates against this). */')
    ts_lines.append('export interface SysioContractDefinition<Name extends SysioContractName> {')
    ts_lines.append('  readonly name: Name')
    ts_lines.append('  readonly account: string')
    ts_lines.append('  readonly actions: ReadonlyArray<string>')
    ts_lines.append('  readonly tables: ReadonlyArray<string>')
    ts_lines.append('}')
    ts_lines.append('export const SysioContractDefinitions: {')
    ts_lines.append('  readonly [Name in SysioContractName]: SysioContractDefinition<Name>')
    ts_lines.append('} = {')
    for contract_name, _prefix, action_names, table_names in contract_registry:
        short = short_contract_name(contract_name)
        account = DEPLOY_ACCOUNT_OVERRIDES.get(short, contract_name)
        actions_arr = ', '.join(f'"{a}"' for a in action_names)
        tables_arr = ', '.join(f'"{t}"' for t in table_names)
        ts_lines.append(f'  [SysioContractName.{short}]: {{ name: SysioContractName.{short}, account: "{account}", '
                        f'actions: [{actions_arr}], tables: [{tables_arr}] }},')
    ts_lines.append('}')
    ts_lines.append('')

    # Write JSON Schema
    schema = {
        '$schema': 'https://json-schema.org/draft/2020-12/schema',
        'title': 'Wire System Contract Types',
        'description': f'JSON Schema for all Wire system contract ABI types (style: {style})',
        'definitions': all_definitions,
    }
    schema_path = os.path.join(output_dir, 'schema', 'system-contracts.schema.json')
    with open(schema_path, 'w') as f:
        json.dump(schema, f, indent=2)
    click.echo(f'Written: {schema_path} ({len(all_definitions)} definitions)')

    # Write TypeScript
    ts_path = os.path.join(output_dir, 'typescript', 'SysioContractTypes.ts')
    with open(ts_path, 'w') as f:
        f.write('\n'.join(ts_lines) + '\n')
    iface_count = sum(1 for line in ts_lines if line.startswith('export interface'))
    click.echo(f'Written: {ts_path} ({iface_count} interfaces)')


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

@click.command()
@click.option('-B', '--build-dir', required=True, type=click.Path(exists=True, file_okay=False),
              help='Path to wire-sysio build directory.')
@click.option('-O', '--output-dir', required=True, type=click.Path(),
              help='Output directory for generated types.')
@click.option('-P', '--style', type=click.Choice(['snake', 'camel', 'both'], case_sensitive=False),
              default='snake', show_default=True,
              help='Property naming style for type members. "both" emits snake + camel (all optional).')
@click.option('-f', '--force', is_flag=True, default=False,
              help='Overwrite if output directory is not empty.')
def main(build_dir: str, output_dir: str, style: str, force: bool) -> None:
    """Generate JSON Schema and TypeScript types from Wire system contract ABIs."""
    # Check output dir
    if os.path.isdir(output_dir) and os.listdir(output_dir) and not force:
        raise click.ClickException(
            f'Output directory is not empty: {output_dir} (use --force to overwrite)')

    # Gather ABIs
    contracts_dir = os.path.join(build_dir, 'contracts')
    abi_files = sorted(
        str(p) for p in Path(contracts_dir).glob('sysio.*/sysio.*.abi')
        if '.bad.' not in p.name  # skip known bad ABIs like sysio.token.bad.abi
    )

    if not abi_files:
        raise click.ClickException(f'No sysio.*.abi files found in {contracts_dir}/')

    click.echo(f'Found {len(abi_files)} ABI files:')
    for f in abi_files:
        click.echo(f'  {f}')

    generate(abi_files, output_dir, style)

    click.echo()
    click.echo('Generation complete:')
    click.echo(f'  Schema:     {output_dir}/schema/system-contracts.schema.json')
    click.echo(f'  TypeScript: {output_dir}/typescript/SysioContractTypes.ts')
    click.echo(f'  Style:      {style}')


if __name__ == '__main__':
    main()
