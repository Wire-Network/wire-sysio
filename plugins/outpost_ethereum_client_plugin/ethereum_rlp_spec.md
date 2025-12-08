# Ethereum RLP Spec

## [Definition](https://ethereum.org/developers/docs/data-structures-and-encoding/rlp/?utm_source=chatgpt.com#definition)

The RLP encoding function takes in an item. An item is defined as follows：

-   a string (i.e., byte array) is an item
-   a list of items is an item
-   a positive integer is an item

For example, all of the following are items:

-   an empty string;
-   the string containing the word "cat";
-   a list containing any number of strings;
-   and a more complex data structures like `["cat", ["puppy", "cow"], "horse", [[]], "pig", [""], "sheep"]`.
-   the number `100`

Note that in the context of the rest of this page, 'string' means "a certain number of bytes of binary data"; no special encodings are used, and no knowledge about the content of the strings is implied (except as required by the rule against non-minimal positive integers).

RLP encoding is defined as follows:

-   For a positive integer, it is converted to the shortest byte array whose big-endian interpretation is the integer, and then encoded as a string according to the rules below.
-   For a single byte whose value is in the `[0x00, 0x7f]` (decimal `[0, 127]`) range, that byte is its own RLP encoding.
-   Otherwise, if a string is 0-55 bytes long, the RLP encoding consists of a single byte with value **0x80** (dec. 128) plus the length of the string followed by the string. The range of the first byte is thus `[0x80, 0xb7]` (dec. `[128, 183]`).
-   If a string is more than 55 bytes long, the RLP encoding consists of a single byte with value **0xb7** (dec. 183) plus the length in bytes of the length of the string in binary form, followed by the length of the string, followed by the string. For example, a 1024 byte long string would be encoded as `\xb9\x04\x00` (dec. `185, 4, 0`) followed by the string. Here, `0xb9` (183 + 2 = 185) as the first byte, followed by the 2 bytes `0x0400` (dec. 1024) that denote the length of the actual string. The range of the first byte is thus `[0xb8, 0xbf]` (dec. `[184, 191]`).
-   If a string is 2^64 bytes long, or longer, it may not be encoded.
-   If the total payload of a list (i.e., the combined length of all its items being RLP encoded) is 0-55 bytes long, the RLP encoding consists of a single byte with value **0xc0** plus the length of the payload followed by the concatenation of the RLP encodings of the items. The range of the first byte is thus `[0xc0, 0xf7]` (dec. `[192, 247]`).
-   If the total payload of a list is more than 55 bytes long, the RLP encoding consists of a single byte with value **0xf7** plus the length in bytes of the length of the payload in binary form, followed by the length of the payload, followed by the concatenation of the RLP encodings of the items. The range of the first byte is thus `[0xf8, 0xff]` (dec. `[248, 255]`).

In code, this is:
```python3

def rlp_encode(input):
    if isinstance(input,str):
        if len(input) == 1 and ord(input) < 0x80:
            return input
        return encode_length(len(input), 0x80) + input
    elif isinstance(input, list):
        output = ''
        for item in input:
            output += rlp_encode(item)
        return encode_length(len(output), 0xc0) + output

def encode_length(L, offset):
    if L < 56:
        return chr(L + offset)
    elif L < 256**8:
        BL = to_binary(L)
        return chr(len(BL) + offset + 55) + BL
    raise Exception("input too long")

def to_binary(x):
    if x == 0:
        return ''
    return to_binary(int(x / 256)) + chr(x % 256)


```
## [Examples](https://ethereum.org/developers/docs/data-structures-and-encoding/rlp/?utm_source=chatgpt.com#examples)

-   the string "dog" = \[ 0x83, 'd', 'o', 'g' \]
-   the list \[ "cat", "dog" \] = `[ 0xc8, 0x83, 'c', 'a', 't', 0x83, 'd', 'o', 'g' ]`
-   the empty string ('null') = `[ 0x80 ]`
-   the empty list = `[ 0xc0 ]`
-   the integer 0 = `[ 0x80 ]`
-   the byte '\\x00' = `[ 0x00 ]`
-   the byte '\\x0f' = `[ 0x0f ]`
-   the bytes '\\x04\\x00' = `[ 0x82, 0x04, 0x00 ]`
-   the [set theoretical representationopens in a new tab](http://en.wikipedia.org/wiki/Set-theoretic_definition_of_natural_numbers) of three, `[ [], [[]], [ [], [[]] ] ] = [ 0xc7, 0xc0, 0xc1, 0xc0, 0xc3, 0xc0, 0xc1, 0xc0 ]`
-   the string "Lorem ipsum dolor sit amet, consectetur adipisicing elit" = `[ 0xb8, 0x38, 'L', 'o', 'r', 'e', 'm', ' ', ... , 'e', 'l', 'i', 't' ]`