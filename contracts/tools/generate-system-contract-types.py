#!/usr/bin/env python3
"""
Generate JSON Schema and TypeScript types from Wire system contract ABIs.

Gathers all sysio.*.abi files from <build-dir>/contracts/ and produces:
  - <output-dir>/schema/system-contracts.schema.json
  - <output-dir>/typescript/SystemContractTypes.ts

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
    'int64': 'number', 'uint64': 'number',
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
    """Map an ABI type to a TypeScript type string."""
    abi_type = resolve_type(abi_type, type_aliases)
    if abi_type.endswith('[]'):
        return abi_type_to_ts(abi_type[:-2], structs_map, type_aliases, contract_prefix, enums_map) + '[]'
    if abi_type.endswith('?'):
        return abi_type_to_ts(abi_type[:-1], structs_map, type_aliases, contract_prefix, enums_map) + ' | null'
    if abi_type in PRIMITIVE_TS:
        return PRIMITIVE_TS[abi_type]
    if enums_map and abi_type in enums_map:
        enum_base = abi_type[:-2] if abi_type.endswith('_t') else abi_type
        return contract_prefix + to_pascal(enum_base) + ('Type' if abi_type.endswith('_t') else '')
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
    ts_path = os.path.join(output_dir, 'typescript', 'SystemContractTypes.ts')
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
    click.echo(f'  TypeScript: {output_dir}/typescript/SystemContractTypes.ts')
    click.echo(f'  Style:      {style}')


if __name__ == '__main__':
    main()
