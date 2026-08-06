// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import {
    UnderwriteIntentCommit,
    UnderwriteIntentCommitCodec
} from "@wireio/opp-solidity-models/contracts/sysio/opp/attestations/Attestations.sol";
import {
    ChainAddress,
    ChainKind,
    ChainKindLib,
    WireAccount
} from "@wireio/opp-solidity-models/contracts/sysio/opp/types/Types.sol";

/// Executes the generated production Solidity codec against the sealed UIC
/// records owned by wire-sysio's cross-generator compatibility gate.
contract UICCrossGeneratorTest {
    bytes private constant EVM_ADDRESS =
        hex"9965507d1a55bcc2695c58ba16fb37d819b0a4dc";
    bytes private constant EVM_SIGNATURE =
        hex"0020dca5027647a8282688d535cc783057ece6a950450c63cefafa007ac95688ee2608525965898277231aee8ee079065a8da3cd886a34e2326ae6143b074be787fb";
    bytes private constant EVM_FULL =
        hex"0a0d0a0b756e6465727772697465721218080212149965507d1a55bcc2695c58ba16fb37d819b0a4dc18ac022a420020dca5027647a8282688d535cc783057ece6a950450c63cefafa007ac95688ee2608525965898277231aee8ee079065a8da3cd886a34e2326ae6143b074be787fb30ad0238ae0240af02";

    bytes private constant SVM_ADDRESS =
        hex"17cb79fb2b4120f2b1ec65e4198d6e08b28e813feb01e4a400839b85e18080ce";
    bytes private constant SVM_SIGNATURE =
        hex"04962daf636c40b2b7236987387b2d04b2cafb7dd1da8711ae48afb0e3d3baad12311911aecac80f2a05cb4f364a35fe1437c74aeee5b7d51b448ecc758e374091b0a6817ce27ea8df7adc5491f116c0248d7470a819e22d241e5768151371be0e";
    bytes private constant SVM_FULL =
        hex"0a0d0a0b756e64657277726974657212240803122017cb79fb2b4120f2b1ec65e4198d6e08b28e813feb01e4a400839b85e18080ce18ac022a6104962daf636c40b2b7236987387b2d04b2cafb7dd1da8711ae48afb0e3d3baad12311911aecac80f2a05cb4f364a35fe1437c74aeee5b7d51b448ecc758e374091b0a6817ce27ea8df7adc5491f116c0248d7470a819e22d241e5768151371be0e30ad0238ae0240af02";

    function testGeneratedSolidityEncoderMatchesSealedEvmVector() public pure {
        _assertConstructsAndReencodes(
            ChainKindLib.CHAIN_KIND_EVM, EVM_ADDRESS, EVM_SIGNATURE, EVM_FULL
        );
    }

    function testGeneratedSolidityEncoderMatchesSealedSvmVector() public pure {
        _assertConstructsAndReencodes(
            ChainKindLib.CHAIN_KIND_SVM, SVM_ADDRESS, SVM_SIGNATURE, SVM_FULL
        );
    }

    function testGeneratedSolidityDecoderRejectsMalformedUint64Varints() public {
        _assertDecodeReverts(hex"1880");
        _assertDecodeReverts(hex"1880808080808080808002");
    }

    function decodeAndReencode(bytes memory encoded) external pure returns (bytes memory) {
        return UnderwriteIntentCommitCodec.encode(
            UnderwriteIntentCommitCodec.decode(encoded)
        );
    }

    function _assertConstructsAndReencodes(
        ChainKind kind,
        bytes memory callerAddress,
        bytes memory signature,
        bytes memory expected
    ) private pure {
        UnderwriteIntentCommit memory uic = UnderwriteIntentCommit({
            uwAccount: WireAccount({name: "underwriter"}),
            uwExtChainAddr: ChainAddress({kind: kind, address_: callerAddress}),
            uwRequestId: 300,
            signature: signature,
            tokenCode: 301,
            chainCode: 302,
            reserveCode: 303
        });
        _assertBytesEqual(UnderwriteIntentCommitCodec.encode(uic), expected);
        _assertBytesEqual(
            UnderwriteIntentCommitCodec.encode(
                UnderwriteIntentCommitCodec.decode(expected)
            ),
            expected
        );
    }

    function _assertDecodeReverts(bytes memory malformed) private {
        (bool success,) = address(this).call(
            abi.encodeCall(this.decodeAndReencode, (malformed))
        );
        require(!success, "malformed varint decoded successfully");
    }

    function _assertBytesEqual(bytes memory actual, bytes memory expected) private pure {
        require(actual.length == expected.length, "encoded length mismatch");
        require(keccak256(actual) == keccak256(expected), "encoded bytes mismatch");
    }
}
