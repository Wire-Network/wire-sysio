#[path = "generated/protobuf_runtime.rs"]
mod protobuf_runtime;
#[path = "generated/sysio/mod.rs"]
mod sysio;

#[cfg(test)]
mod tests {
    use super::sysio::opp::attestations::attestations::UnderwriteIntentCommit;
    use super::sysio::opp::types::types::{ChainAddress, ChainKind, WireAccount};
    struct Vector {
        kind: ChainKind,
        address_hex: &'static str,
        signature_hex: &'static str,
        full_hex: &'static str,
    }

    fn decode_hex(encoded: &str) -> Vec<u8> {
        assert_eq!(encoded.len() % 2, 0);
        encoded
            .as_bytes()
            .chunks_exact(2)
            .map(|pair| {
                let digits = core::str::from_utf8(pair).expect("golden hex is UTF-8");
                u8::from_str_radix(digits, 16).expect("golden hex is valid")
            })
            .collect()
    }

    #[test]
    fn generated_rust_encoder_constructs_and_reencodes_sealed_uic_vectors() {
        let vectors = [
            Vector {
                kind: ChainKind::ChainKindEvm,
                address_hex: "9965507d1a55bcc2695c58ba16fb37d819b0a4dc",
                signature_hex: "0020dca5027647a8282688d535cc783057ece6a950450c63cefafa007ac95688ee2608525965898277231aee8ee079065a8da3cd886a34e2326ae6143b074be787fb",
                full_hex: "0a0d0a0b756e6465727772697465721218080212149965507d1a55bcc2695c58ba16fb37d819b0a4dc18ac022a420020dca5027647a8282688d535cc783057ece6a950450c63cefafa007ac95688ee2608525965898277231aee8ee079065a8da3cd886a34e2326ae6143b074be787fb30ad0238ae0240af02",
            },
            Vector {
                kind: ChainKind::ChainKindSvm,
                address_hex: "17cb79fb2b4120f2b1ec65e4198d6e08b28e813feb01e4a400839b85e18080ce",
                signature_hex: "04962daf636c40b2b7236987387b2d04b2cafb7dd1da8711ae48afb0e3d3baad12311911aecac80f2a05cb4f364a35fe1437c74aeee5b7d51b448ecc758e374091b0a6817ce27ea8df7adc5491f116c0248d7470a819e22d241e5768151371be0e",
                full_hex: "0a0d0a0b756e64657277726974657212240803122017cb79fb2b4120f2b1ec65e4198d6e08b28e813feb01e4a400839b85e18080ce18ac022a6104962daf636c40b2b7236987387b2d04b2cafb7dd1da8711ae48afb0e3d3baad12311911aecac80f2a05cb4f364a35fe1437c74aeee5b7d51b448ecc758e374091b0a6817ce27ea8df7adc5491f116c0248d7470a819e22d241e5768151371be0e30ad0238ae0240af02",
            },
        ];

        for vector in vectors {
            let expected = decode_hex(vector.full_hex);
            let signed = UnderwriteIntentCommit {
                uw_account: WireAccount {
                    name: "underwriter".into(),
                },
                uw_ext_chain_addr: ChainAddress {
                    kind: vector.kind,
                    address: decode_hex(vector.address_hex),
                },
                uw_request_id: 300,
                signature: decode_hex(vector.signature_hex),
                token_code: 301,
                chain_code: 302,
                reserve_code: 303,
            };

            let encoded = signed.encode();
            assert_eq!(encoded, expected);
            let decoded = UnderwriteIntentCommit::decode(&encoded)
                .expect("sealed UIC must decode with the generated Rust model");
            assert_eq!(decoded, signed);
            assert_eq!(decoded.encode(), encoded);
        }
    }

    #[test]
    fn generated_rust_decoder_rejects_malformed_uint64_varints() {
        assert!(UnderwriteIntentCommit::decode(&decode_hex("1880")).is_err());
    }
}
