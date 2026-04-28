// Shared protobuf3 wire format primitives for protoc-gen-solana.
// This file is also emitted by the plugin alongside generated codecs.
//
// Optimized for Solana's compute budget: minimal allocations,
// no unnecessary copies, and efficient varint handling.

use std::fmt;

// ── Error type ───────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub enum DecodeError {
    BufferOverflow,
    InvalidVarint,
    UnknownWireType(u64),
    InvalidData(&'static str),
}

impl fmt::Display for DecodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            DecodeError::BufferOverflow => write!(f, "protobuf: buffer overflow"),
            DecodeError::InvalidVarint => write!(f, "protobuf: invalid varint"),
            DecodeError::UnknownWireType(wt) => write!(f, "protobuf: unknown wire type {}", wt),
            DecodeError::InvalidData(msg) => write!(f, "protobuf: {}", msg),
        }
    }
}

// ── Key (tag) encode / decode ────────────────────────────────────────

#[inline]
pub fn encode_key(buf: &mut Vec<u8>, tag: u64) {
    encode_varint(buf, tag);
}

#[inline]
pub fn decode_key(data: &[u8], pos: usize) -> Result<(u64, usize), DecodeError> {
    decode_varint(data, pos)
}

// ── Wire Type 0: Varint ──────────────────────────────────────────────

#[inline]
pub fn encode_varint(buf: &mut Vec<u8>, mut value: u64) {
    loop {
        if value < 0x80 {
            buf.push(value as u8);
            return;
        }
        buf.push(((value & 0x7F) | 0x80) as u8);
        value >>= 7;
    }
}

#[inline]
pub fn decode_varint(data: &[u8], mut pos: usize) -> Result<(u64, usize), DecodeError> {
    let mut result: u64 = 0;
    let mut shift: u32 = 0;
    loop {
        if pos >= data.len() {
            return Err(DecodeError::BufferOverflow);
        }
        let b = data[pos];
        pos += 1;
        result |= ((b & 0x7F) as u64) << shift;
        if b & 0x80 == 0 {
            return Ok((result, pos));
        }
        shift += 7;
        if shift > 63 {
            return Err(DecodeError::InvalidVarint);
        }
    }
}

// ── Bool ─────────────────────────────────────────────────────────────

#[inline]
pub fn encode_bool(buf: &mut Vec<u8>, value: bool) {
    buf.push(if value { 1 } else { 0 });
}

#[inline]
pub fn decode_bool(data: &[u8], pos: usize) -> Result<(bool, usize), DecodeError> {
    let (v, new_pos) = decode_varint(data, pos)?;
    Ok((v != 0, new_pos))
}

// ── ZigZag (sint32/sint64) ───────────────────────────────────────────

#[inline]
pub fn encode_zigzag32(buf: &mut Vec<u8>, value: i32) {
    let encoded = ((value << 1) ^ (value >> 31)) as u32;
    encode_varint(buf, encoded as u64);
}

#[inline]
pub fn decode_zigzag32(data: &[u8], pos: usize) -> Result<(i32, usize), DecodeError> {
    let (raw, new_pos) = decode_varint(data, pos)?;
    let n = raw as u32;
    let value = ((n >> 1) as i32) ^ (-((n & 1) as i32));
    Ok((value, new_pos))
}

#[inline]
pub fn encode_zigzag64(buf: &mut Vec<u8>, value: i64) {
    let encoded = ((value << 1) ^ (value >> 63)) as u64;
    encode_varint(buf, encoded);
}

#[inline]
pub fn decode_zigzag64(data: &[u8], pos: usize) -> Result<(i64, usize), DecodeError> {
    let (raw, new_pos) = decode_varint(data, pos)?;
    let value = ((raw >> 1) as i64) ^ (-((raw & 1) as i64));
    Ok((value, new_pos))
}

// ── Wire Type 1: 64-bit (little-endian) ─────────────────────────────

#[inline]
pub fn encode_fixed64(buf: &mut Vec<u8>, value: u64) {
    buf.extend_from_slice(&value.to_le_bytes());
}

#[inline]
pub fn decode_fixed64(data: &[u8], pos: usize) -> Result<(u64, usize), DecodeError> {
    if pos + 8 > data.len() {
        return Err(DecodeError::BufferOverflow);
    }
    let value = u64::from_le_bytes(data[pos..pos + 8].try_into().unwrap());
    Ok((value, pos + 8))
}

#[inline]
pub fn encode_sfixed64(buf: &mut Vec<u8>, value: i64) {
    encode_fixed64(buf, value as u64);
}

#[inline]
pub fn decode_sfixed64(data: &[u8], pos: usize) -> Result<(i64, usize), DecodeError> {
    let (raw, new_pos) = decode_fixed64(data, pos)?;
    Ok((raw as i64, new_pos))
}

// ── Wire Type 5: 32-bit (little-endian) ─────────────────────────────

#[inline]
pub fn encode_fixed32(buf: &mut Vec<u8>, value: u32) {
    buf.extend_from_slice(&value.to_le_bytes());
}

#[inline]
pub fn decode_fixed32(data: &[u8], pos: usize) -> Result<(u32, usize), DecodeError> {
    if pos + 4 > data.len() {
        return Err(DecodeError::BufferOverflow);
    }
    let value = u32::from_le_bytes(data[pos..pos + 4].try_into().unwrap());
    Ok((value, pos + 4))
}

#[inline]
pub fn encode_sfixed32(buf: &mut Vec<u8>, value: i32) {
    encode_fixed32(buf, value as u32);
}

#[inline]
pub fn decode_sfixed32(data: &[u8], pos: usize) -> Result<(i32, usize), DecodeError> {
    let (raw, new_pos) = decode_fixed32(data, pos)?;
    Ok((raw as i32, new_pos))
}

// ── Wire Type 2: Length-delimited ────────────────────────────────────

#[inline]
pub fn encode_bytes(buf: &mut Vec<u8>, value: &[u8]) {
    encode_varint(buf, value.len() as u64);
    buf.extend_from_slice(value);
}

#[inline]
pub fn decode_bytes(data: &[u8], pos: usize) -> Result<(Vec<u8>, usize), DecodeError> {
    let (len, pos) = decode_varint(data, pos)?;
    let len = len as usize;
    if pos + len > data.len() {
        return Err(DecodeError::BufferOverflow);
    }
    Ok((data[pos..pos + len].to_vec(), pos + len))
}

#[inline]
pub fn encode_string(buf: &mut Vec<u8>, value: &str) {
    encode_bytes(buf, value.as_bytes());
}

#[inline]
pub fn decode_string(data: &[u8], pos: usize) -> Result<(String, usize), DecodeError> {
    let (raw, new_pos) = decode_bytes(data, pos)?;
    String::from_utf8(raw)
        .map(|s| (s, new_pos))
        .map_err(|_| DecodeError::InvalidData("invalid UTF-8 in string field"))
}

// ── Skip unknown fields ──────────────────────────────────────────────

#[inline]
pub fn skip_field(data: &[u8], pos: usize, wire_type: u64) -> Result<usize, DecodeError> {
    match wire_type {
        0 => {
            // Varint: skip until MSB is clear
            let (_, new_pos) = decode_varint(data, pos)?;
            Ok(new_pos)
        }
        1 => {
            // 64-bit: skip 8 bytes
            if pos + 8 > data.len() {
                return Err(DecodeError::BufferOverflow);
            }
            Ok(pos + 8)
        }
        2 => {
            // Length-delimited: read length, skip that many bytes
            let (len, new_pos) = decode_varint(data, pos)?;
            let end = new_pos + len as usize;
            if end > data.len() {
                return Err(DecodeError::BufferOverflow);
            }
            Ok(end)
        }
        5 => {
            // 32-bit: skip 4 bytes
            if pos + 4 > data.len() {
                return Err(DecodeError::BufferOverflow);
            }
            Ok(pos + 4)
        }
        _ => Err(DecodeError::UnknownWireType(wire_type)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_varint_roundtrip() {
        for &val in &[0u64, 1, 127, 128, 255, 300, 16384, u64::MAX] {
            let mut buf = Vec::new();
            encode_varint(&mut buf, val);
            let (decoded, pos) = decode_varint(&buf, 0).unwrap();
            assert_eq!(decoded, val);
            assert_eq!(pos, buf.len());
        }
    }

    #[test]
    fn test_bool_roundtrip() {
        for &val in &[true, false] {
            let mut buf = Vec::new();
            encode_bool(&mut buf, val);
            let (decoded, _) = decode_bool(&buf, 0).unwrap();
            assert_eq!(decoded, val);
        }
    }

    #[test]
    fn test_zigzag32_roundtrip() {
        for &val in &[0i32, 1, -1, 2, -2, i32::MAX, i32::MIN] {
            let mut buf = Vec::new();
            encode_zigzag32(&mut buf, val);
            let (decoded, _) = decode_zigzag32(&buf, 0).unwrap();
            assert_eq!(decoded, val);
        }
    }

    #[test]
    fn test_zigzag64_roundtrip() {
        for &val in &[0i64, 1, -1, 2, -2, i64::MAX, i64::MIN] {
            let mut buf = Vec::new();
            encode_zigzag64(&mut buf, val);
            let (decoded, _) = decode_zigzag64(&buf, 0).unwrap();
            assert_eq!(decoded, val);
        }
    }

    #[test]
    fn test_fixed64_roundtrip() {
        for &val in &[0u64, 1, 0xDEAD_BEEF, u64::MAX] {
            let mut buf = Vec::new();
            encode_fixed64(&mut buf, val);
            let (decoded, _) = decode_fixed64(&buf, 0).unwrap();
            assert_eq!(decoded, val);
        }
    }

    #[test]
    fn test_fixed32_roundtrip() {
        for &val in &[0u32, 1, 0xDEAD, u32::MAX] {
            let mut buf = Vec::new();
            encode_fixed32(&mut buf, val);
            let (decoded, _) = decode_fixed32(&buf, 0).unwrap();
            assert_eq!(decoded, val);
        }
    }

    #[test]
    fn test_string_roundtrip() {
        for val in &["", "hello", "hello world 🌍"] {
            let mut buf = Vec::new();
            encode_string(&mut buf, val);
            let (decoded, _) = decode_string(&buf, 0).unwrap();
            assert_eq!(&decoded, val);
        }
    }

    #[test]
    fn test_bytes_roundtrip() {
        for val in &[vec![], vec![1u8, 2, 3], vec![0xFF; 300]] {
            let mut buf = Vec::new();
            encode_bytes(&mut buf, val);
            let (decoded, _) = decode_bytes(&buf, 0).unwrap();
            assert_eq!(&decoded, val);
        }
    }

    #[test]
    fn test_skip_field() {
        // Varint
        let mut buf = Vec::new();
        encode_varint(&mut buf, 300);
        let new_pos = skip_field(&buf, 0, 0).unwrap();
        assert_eq!(new_pos, buf.len());

        // Fixed64
        let mut buf = Vec::new();
        encode_fixed64(&mut buf, 42);
        let new_pos = skip_field(&buf, 0, 1).unwrap();
        assert_eq!(new_pos, 8);

        // Length-delimited
        let mut buf = Vec::new();
        encode_string(&mut buf, "hello");
        let new_pos = skip_field(&buf, 0, 2).unwrap();
        assert_eq!(new_pos, buf.len());

        // Fixed32
        let mut buf = Vec::new();
        encode_fixed32(&mut buf, 42);
        let new_pos = skip_field(&buf, 0, 5).unwrap();
        assert_eq!(new_pos, 4);
    }
}
