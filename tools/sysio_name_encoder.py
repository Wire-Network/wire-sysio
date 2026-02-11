#!/usr/bin/env python3
import click


def convert_name_to_value(name: str) -> int:
    def value_of_char(c: str):
        assert len(c) == 1
        if 'a' <= c <= 'z':
            return ord(c) - ord('a') + 6
        if '1' <= c <= '5':
            return ord(c) - ord('1') + 1
        if c == '.':
            return 0
        raise ValueError("invalid SYSIO name: character '{0}' is not allowed".format(c))

    name_length = len(name)

    if name_length > 13:
        raise ValueError("invalid SYSIO name: cannot exceed 13 characters")

    thirteen_char_value = 0
    if name_length == 13:
        thirteen_char_value = value_of_char(name[12])
        if thirteen_char_value > 15:
            raise ValueError("invalid SYSIO name: 13th character cannot be letter past j")

    # truncate/extend to at exactly 12 characters since the 13th character is handled differently
    normalized_name = name[:12].ljust(12, '.')

    def convert_to_value(s: str) -> int:
        value = 0
        for c in s:
            value <<= 5
            value |= value_of_char(c)

        return value

    return (convert_to_value(normalized_name) << 4) | thirteen_char_value


@click.command()
@click.argument('name', type=str,
                required=True)
def main(name):
    """NAME to be converted to numeric value, 12 character max, [a-z\\.]+

    NAME is the SYSIO name to convert to numeric value.
    """
    print(convert_name_to_value(name))


if __name__ == "__main__":
    main()
