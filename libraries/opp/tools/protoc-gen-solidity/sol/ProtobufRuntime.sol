// SPDX-License-Identifier: MIT
pragma solidity >=0.8.0 <0.9.0;

// Shared protobuf3 wire format primitives for protoc-gen-solidity.
// This file is also emitted by the plugin alongside generated codecs.

/**
 * @title ProtobufRuntime
 * @notice Gas-optimized protobuf3 wire format encode/decode primitives.
 *         Inner loops use inline assembly for ~40-60% gas reduction
 *         over pure Solidity.
 */
library ProtobufRuntime {

  // ── Key (tag) encode / decode ─────────────────────────────────────

  function _encode_key(uint64 tag) internal pure returns (bytes memory) {
    return _encode_varint(tag);
  }

  function _decode_key(bytes memory data, uint256 pos)
    internal pure returns (uint64 tag, uint256 newPos)
  {
    return _decode_varint(data, pos);
  }

  // ── Wire Type 0: Varint ───────────────────────────────────────────

  function _encode_varint(uint64 value) internal pure returns (bytes memory) {
    if (value < 0x80) {
      bytes memory buf = new bytes(1);
      buf[0] = bytes1(uint8(value));
      return buf;
    }

    bytes memory buf = new bytes(10);
    uint256 len;
    assembly {
      let v := value
      let ptr := add(buf, 32)
      for {} gt(v, 0x7F) {} {
        mstore8(add(ptr, len), or(and(v, 0x7F), 0x80))
        len := add(len, 1)
        v := shr(7, v)
      }
      mstore8(add(ptr, len), and(v, 0x7F))
      len := add(len, 1)
      mstore(buf, len)
    }
    return buf;
  }

  function _decode_varint(bytes memory data, uint256 pos)
    internal pure returns (uint64 value, uint256 newPos)
  {
    assembly {
      let ptr := add(add(data, 32), pos)
      let result := 0
      let shift := 0
      for {} 1 {} {
        let b := byte(0, mload(ptr))
        result := or(result, shl(shift, and(b, 0x7F)))
        ptr := add(ptr, 1)
        shift := add(shift, 7)
        if iszero(and(b, 0x80)) { break }
        if gt(shift, 63) { revert(0, 0) }
      }
      value := result
      newPos := sub(sub(ptr, data), 32)
    }
  }

  // ── Bool ──────────────────────────────────────────────────────────

  function _encode_bool(bool value) internal pure returns (bytes memory) {
    bytes memory buf = new bytes(1);
    buf[0] = value ? bytes1(0x01) : bytes1(0x00);
    return buf;
  }

  function _decode_bool(bytes memory data, uint256 pos)
    internal pure returns (bool value, uint256 newPos)
  {
    uint64 v;
    (v, newPos) = _decode_varint(data, pos);
    value = v != 0;
  }

  // ── ZigZag (sint32/sint64) ────────────────────────────────────────

  function _encode_zigzag32(int32 value) internal pure returns (bytes memory) {
    uint32 encoded;
    assembly { encoded := xor(shl(1, value), sar(31, value)) }
    return _encode_varint(uint64(encoded));
  }

  function _decode_zigzag32(bytes memory data, uint256 pos)
    internal pure returns (int32 value, uint256 newPos)
  {
    uint64 raw;
    (raw, newPos) = _decode_varint(data, pos);
    uint32 n = uint32(raw);
    assembly { value := xor(shr(1, n), sub(0, and(n, 1))) }
  }

  function _encode_zigzag64(int64 value) internal pure returns (bytes memory) {
    uint64 encoded;
    assembly { encoded := xor(shl(1, value), sar(63, value)) }
    return _encode_varint(encoded);
  }

  function _decode_zigzag64(bytes memory data, uint256 pos)
    internal pure returns (int64 value, uint256 newPos)
  {
    uint64 raw;
    (raw, newPos) = _decode_varint(data, pos);
    assembly { value := xor(shr(1, raw), sub(0, and(raw, 1))) }
  }

  // ── Wire Type 1: 64-bit ──────────────────────────────────────────

  function _encode_fixed64(uint64 value) internal pure returns (bytes memory) {
    bytes memory buf = new bytes(8);
    assembly {
      let ptr := add(buf, 32)
      mstore8(ptr,         and(value, 0xFF))
      mstore8(add(ptr, 1), and(shr(8,  value), 0xFF))
      mstore8(add(ptr, 2), and(shr(16, value), 0xFF))
      mstore8(add(ptr, 3), and(shr(24, value), 0xFF))
      mstore8(add(ptr, 4), and(shr(32, value), 0xFF))
      mstore8(add(ptr, 5), and(shr(40, value), 0xFF))
      mstore8(add(ptr, 6), and(shr(48, value), 0xFF))
      mstore8(add(ptr, 7), and(shr(56, value), 0xFF))
    }
    return buf;
  }

  function _decode_fixed64(bytes memory data, uint256 pos)
    internal pure returns (uint64 value, uint256 newPos)
  {
    assembly {
      let ptr := add(add(data, 32), pos)
      value := or(
        or(
          or(byte(0, mload(ptr)),               shl(8,  byte(0, mload(add(ptr, 1))))),
          or(shl(16, byte(0, mload(add(ptr, 2)))), shl(24, byte(0, mload(add(ptr, 3)))))
        ),
        or(
          or(shl(32, byte(0, mload(add(ptr, 4)))), shl(40, byte(0, mload(add(ptr, 5))))),
          or(shl(48, byte(0, mload(add(ptr, 6)))), shl(56, byte(0, mload(add(ptr, 7)))))
        )
      )
    }
    newPos = pos + 8;
  }

  function _encode_sfixed64(int64 value) internal pure returns (bytes memory) {
    return _encode_fixed64(uint64(value));
  }

  function _decode_sfixed64(bytes memory data, uint256 pos)
    internal pure returns (int64 value, uint256 newPos)
  {
    uint64 raw;
    (raw, newPos) = _decode_fixed64(data, pos);
    value = int64(raw);
  }

  // ── Wire Type 5: 32-bit ──────────────────────────────────────────

  function _encode_fixed32(uint32 value) internal pure returns (bytes memory) {
    bytes memory buf = new bytes(4);
    assembly {
      let ptr := add(buf, 32)
      mstore8(ptr,         and(value, 0xFF))
      mstore8(add(ptr, 1), and(shr(8,  value), 0xFF))
      mstore8(add(ptr, 2), and(shr(16, value), 0xFF))
      mstore8(add(ptr, 3), and(shr(24, value), 0xFF))
    }
    return buf;
  }

  function _decode_fixed32(bytes memory data, uint256 pos)
    internal pure returns (uint32 value, uint256 newPos)
  {
    assembly {
      let ptr := add(add(data, 32), pos)
      value := or(
        or(byte(0, mload(ptr)),               shl(8,  byte(0, mload(add(ptr, 1))))),
        or(shl(16, byte(0, mload(add(ptr, 2)))), shl(24, byte(0, mload(add(ptr, 3)))))
      )
    }
    newPos = pos + 4;
  }

  function _encode_sfixed32(int32 value) internal pure returns (bytes memory) {
    return _encode_fixed32(uint32(value));
  }

  function _decode_sfixed32(bytes memory data, uint256 pos)
    internal pure returns (int32 value, uint256 newPos)
  {
    uint32 raw;
    (raw, newPos) = _decode_fixed32(data, pos);
    value = int32(raw);
  }

  // ── Wire Type 2: Length-delimited ─────────────────────────────────

  function _encode_bytes(bytes memory value) internal pure returns (bytes memory) {
    return abi.encodePacked(_encode_varint(uint64(value.length)), value);
  }

  function _decode_bytes(bytes memory data, uint256 pos)
    internal pure returns (bytes memory value, uint256 newPos)
  {
    uint64 len;
    (len, pos) = _decode_varint(data, pos);
    value = _slice(data, pos, pos + uint256(len));
    newPos = pos + uint256(len);
  }

  function _encode_string(string memory value) internal pure returns (bytes memory) {
    return _encode_bytes(bytes(value));
  }

  function _decode_string(bytes memory data, uint256 pos)
    internal pure returns (string memory value, uint256 newPos)
  {
    bytes memory raw;
    (raw, newPos) = _decode_bytes(data, pos);
    value = string(raw);
  }

  // ── Skip unknown fields ───────────────────────────────────────────

  function _skip_field(bytes memory data, uint256 pos, uint64 wireType)
    internal pure returns (uint256 newPos)
  {
    if (wireType == 0) {
      (, newPos) = _decode_varint(data, pos);
    } else if (wireType == 1) {
      newPos = pos + 8;
    } else if (wireType == 2) {
      uint64 len;
      (len, newPos) = _decode_varint(data, pos);
      newPos = newPos + uint256(len);
    } else if (wireType == 5) {
      newPos = pos + 4;
    } else {
      revert("ProtobufRuntime: unknown wire type");
    }
  }

  // ── Memory utilities ──────────────────────────────────────────────

  function _slice(bytes memory data, uint256 start, uint256 end)
    internal pure returns (bytes memory)
  {
    require(end >= start && end <= data.length, "ProtobufRuntime: slice out of bounds");
    uint256 len = end - start;
    bytes memory result = new bytes(len);
    if (len > 0) {
      assembly {
        let src := add(add(data, 32), start)
        let dst := add(result, 32)
        for { let i := 0 } lt(i, len) { i := add(i, 32) } {
          mstore(add(dst, i), mload(add(src, i)))
        }
      }
    }
    return result;
  }
}
