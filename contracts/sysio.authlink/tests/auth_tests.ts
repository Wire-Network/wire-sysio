import { config } from "dotenv";
config();

import { expect } from "chai";
import { TransactResult } from "eosjs/dist/eosjs-api-interfaces";
import chain from "./chain";
import { sha256 } from "eosjs/dist/eosjs-key-conversions";
import { error } from "console";
import { keccak256 } from 'ethers';
import { ec as EC } from "elliptic";
const ec = new EC('secp256k1');


/**
 * Note: This test suite is designed to be run against a local testnet. Actions ran in this test suite will affect the state of the contract. So you'll have to clear any necessary tables before running the tests again.
 * 
 * How to use: 
 * - Set testnet endpoint in .env file.
 * - Set private key in .env file. ( EOSIO Studio uses one key for all accounts, makes this very easy.)
 * - Run local testnet. ( Docker / EOSIO Studio )
 * - Make sure contract is deployed to local testnet. (build / deploy scripts included, just need to adjust the chain 'localhost:8888' to whatever you are using.)
 * - Make sure contract is initialized.
 * - Make sure all account names below are created on the test net. 'daniel' 'nick' 'josh' 'settle.wns'
 * - Run tests with 'npm run test-suite'
 */
describe(" ----- Running Auth.msg Test Suite ----- ", async () => {
    const contract = 'auth.msg';
    const user1 = 'daniel';
    const user2 = 'nick';
    const blacklisted_wire_user = 'josh';
    const blacklisted_eth_user = [82, 237, 16, 145, 157, 180, 207, 203, 3, 20, 181, 62, 170, 253, 137, 47, 24, 84, 214, 234];

    // Before running new set of tests clear all tables before running tests.
    before(async () => {
        console.log(" * Clearing all tables before running tests.")
        await chain.pushTransaction({
            account: contract,
            name: 'clearlinks',
            actor: contract,
            permission: "active",
            data: {}
        });
        console.log(" * Tables clear for new round of testing.")
    });

    beforeEach(async () => {
        // Anything that needs to be reset / occur before each test.
    });

    describe("Create new Link", async () => {
        describe("Successes", async () => {
            it("Should Successfully create Link.", async () => {
                // Private Key, Nonce, Account Name
                let msg_info = createMsgDigest(
                    '0x52b19a2ded13f4496167895285266d3924b41bba3b35063b40321e807caa6442',
                    5,
                    'daniel'
                )

                // Transaction data
                let data = {
                    sig: msg_info.wire_sig,
                    msg_digest: msg_info.msg_digest.slice(2),
                    nonce: 5,
                    account_name: "daniel"
                };

                await chain.pushTransaction({
                    account: 'auth.msg',
                    name: 'createlink',
                    actor: 'daniel',
                    permission: 'active',
                    data
                }).catch((error) => {
                    console.log('Push ERROR', error);
                })

                const addr_checksum = get_checksum(msg_info.eth_addr.slice(2));

                // Now check the table to see if the link was created.
                await chain.getRows({
                    code: contract,                    
                    scope: contract,                   
                    table: "links",                   
                    index_position: 3,  
                    lower_bound: addr_checksum,
                    upper_bound: addr_checksum,
                    key_type: "sha256" 
                }).then((result) => {
                    // console.log({msg_info});
                    // console.log(result.rows);
                    expect(result.rows[0].account_name == msg_info.account_name, "Account name does not match.").to.be.true;

                    expect(result.rows[0].nonce == msg_info.nonce, "Nonce does not match.").to.be.true;

                    expect(result.rows[0].eth_address == msg_info.eth_addr.slice(2), "Eth address does not match.").to.be.true;

                    // TODO: Write an expect for the user having the 'auth.ext' permission with the respective pub key.
                });
            });
        });

        describe("Failures", async () => {
            it("Should fail where account_name != caller.", async () => {
                // Private Key, Nonce, Account Name
                let msg_info = createMsgDigest(
                    '0x52b19a2ded13f4496167895285266d3924b41bba3b35063b40321e807caa6442',
                    6,
                    'daniel'
                )

                // Transaction data
                let data = {
                    sig: msg_info.wire_sig,
                    msg_digest: msg_info.msg_digest.slice(2),
                    nonce: 5,
                    account_name: "nick"
                };

                await chain.pushTransaction({
                    account: 'auth.msg',
                    name: 'createlink',
                    actor: 'daniel',
                    permission: 'active',
                    data
                }).catch((error) => {
                    // console.log(error);
                    expect(error == "Error: missing authority of " + data.account_name).to.be.true;
                })
            });

            it("Should fail with invalid signature lengths.", async () => {
                // Private Key, Nonce, Account Name
                let msg_info = createMsgDigest(
                    '0x52b19a2ded13f4496167895285266d3924b41bba3b35063b40321e807caa6442',
                    6,
                    'daniel'
                )

                // Transaction data
                let data = {
                    sig: msg_info.wire_sig.slice(1),
                    msg_digest: msg_info.msg_digest.slice(2),
                    nonce: 5,
                    account_name: "daniel"
                };

                await chain.pushTransaction({
                    account: 'auth.msg',
                    name: 'createlink',
                    actor: 'daniel',
                    permission: 'active',
                    data
                }).catch((error) => {
                    expect(error == "Signature must be 66 bytes long.").to.be.true;
                })
            });

            it("Should fail with 'invalid digest' if passed in digest doesn't match the digest created from the signature.", async () => { 
                // Private Key, Nonce, Account Name
                let msg_info = createMsgDigest(
                    '0x52b19a2ded13f4496167895285266d3924b41bba3b35063b40321e807caa6442',
                    5,
                    'daniel'
                )

                // Transaction data
                let data = {
                    sig: msg_info.wire_sig,
                    msg_digest: msg_info.msg_digest.slice(2),
                    nonce: 5,
                    account_name: "nick"
                };

                await chain.pushTransaction({
                    account: 'auth.msg',
                    name: 'createlink',
                    actor: 'nick',
                    permission: 'active',
                    data
                }).catch((error) => {
                    expect(error == "Invalid Message Digest, does not match recreated value.").to.be.true;
                })
            });
            
            it("Should fail with 'odd number of hex digits' in digest.", async () => {
                 // Private Key, Nonce, Account Name
                 let msg_info = createMsgDigest(
                    '0x52b19a2ded13f4496167895285266d3924b41bba3b35063b40321e807caa6442',
                    6,
                    'daniel'
                )

                // Transaction data
                let data = {
                    sig: msg_info.wire_sig,
                    msg_digest: msg_info.msg_digest.slice(3),
                    nonce: 5,
                    account_name: "daniel"
                };

                await chain.pushTransaction({
                    account: 'auth.msg',
                    name: 'createlink',
                    actor: 'daniel',
                    permission: 'active',
                    data
                }).catch((error) => {
                    // console.log(error);
                    expect(error == "Error: Odd number of hex digits").to.be.true;
                })
            });

            it("Should fail with 'expected hex string' if 0x not sliced from digest.", async () => {
                 // Private Key, Nonce, Account Name
                 let msg_info = createMsgDigest(
                    '0x52b19a2ded13f4496167895285266d3924b41bba3b35063b40321e807caa6442',
                    6,
                    'daniel'
                )

                // Transaction data
                let data = {
                    sig: msg_info.wire_sig,
                    msg_digest: msg_info.msg_digest,
                    nonce: 5,
                    account_name: "daniel"
                };

                await chain.pushTransaction({
                    account: 'auth.msg',
                    name: 'createlink',
                    actor: 'daniel',
                    permission: 'active',
                    data
                }).catch((error) => {
                    // console.log(error);
                    expect(error == "Error: Expected hex string").to.be.true;
                })
            });

            it("Should fail with 'binary data has incorrect size' if less than 32 bytes.", async () => {
                 // Private Key, Nonce, Account Name
                 let msg_info = createMsgDigest(
                    '0x52b19a2ded13f4496167895285266d3924b41bba3b35063b40321e807caa6442',
                    6,
                    'daniel'
                )

                // Transaction data
                let data = {
                    sig: msg_info.wire_sig,
                    msg_digest: msg_info.msg_digest.slice(4),
                    nonce: 5,
                    account_name: "daniel"
                };

                await chain.pushTransaction({
                    account: 'auth.msg',
                    name: 'createlink',
                    actor: 'daniel',
                    permission: 'active',
                    data
                }).catch((error) => {
                    // console.log(error);
                    expect(error == "Error: Binary data has incorrect size").to.be.true;
                })
            });

            it("Should fail with duplicate eth address assignments.", async () => {
                // Private Key, Nonce, Account Name
                let msg_info = createMsgDigest(
                    '0x52b19a2ded13f4496167895285266d3924b41bba3b35063b40321e807caa6442',
                    5,
                    'nick'
                )

                // Transaction data
                let data = {
                    sig: msg_info.wire_sig,
                    msg_digest: msg_info.msg_digest.slice(2),
                    nonce: 5,
                    account_name: "nick"
                };

                await chain.pushTransaction({
                    account: 'auth.msg',
                    name: 'createlink',
                    actor: 'nick',
                    permission: 'active',
                    data
                }).catch((error) => {
                    expect(error == "Ethereum address already linked to a different account.").to.be.true;
                })
            });
        });
    });
});

function createMsgDigest(
    private_key: string,
    nonce: number,
    account_name: string
) {
    const eth_prefix_hex = '19457468657265756d205369676e6564204d6573736167653a0a3332';
    const compressedKey = getCompressedPublicKeyFromPrivateKey(private_key);
    const message = compressedKey + nonce + account_name;
    const messageHash = keccak256(new Uint8Array(Buffer.from(message)));
    const pre_hash_digest_hex = eth_prefix_hex + messageHash.slice(2);
    const pre_hash_digest_uc = new Uint8Array(Buffer.from(pre_hash_digest_hex, 'hex'));
    const msg_digest = keccak256(pre_hash_digest_uc);

    const eth_sig = directSignHash(private_key, msg_digest);

    const wire_sig = getWireSigFromEth(eth_sig.sig);

    return {
        message,
        messageHash,
        msg_digest,
        pre_hash_digest_hex,
        pre_hash_digest_uc,
        compressedKey,
        nonce,
        account_name,
        eth_addr: eth_sig.eth_addr,
        eth_sig: eth_sig.sig,
        wire_sig
    }
}

function getCompressedPublicKeyFromPrivateKey(privateKey: string): string {
    // Ensure the private key doesn't have the '0x' prefix
    if (privateKey.startsWith('0x')) {
        privateKey = privateKey.slice(2);
    }

    const keyPair = ec.keyFromPrivate(privateKey);
    const publicKey = keyPair.getPublic(true, 'hex');  // Get the compressed form

    return publicKey;
}

function directSignHash(privateKey: string, hash: string): {
    sig: string,
    eth_addr: string
} {
    if (privateKey.startsWith('0x')) {
        privateKey = privateKey.slice(2);
    }

    if (hash.startsWith('0x')) {
        hash = hash.slice(2);
    }

    const keyPair = ec.keyFromPrivate(privateKey);
    const signature = keyPair.sign(hash, 'hex');

    // Extract Ethereum address from the keyPair
    const publicKey = keyPair.getPublic('hex').slice(2);  // Remove the '04' prefix (uncompressed format)
    const pubKeyHash = keccak256(Buffer.from(publicKey, 'hex'));
    const address = '0x' + pubKeyHash.slice(-40);  // Last 20 bytes as Ethereum address

    // Convert r, s, and recovery param into the Ethereum signature format
    let r = signature.r.toString(16).padStart(64, '0');
    let s = signature.s.toString(16).padStart(64, '0');
    let v = (signature.recoveryParam || 0) + 27;  // 27 or 28

    return {sig: '0x' + r + s + v.toString(16), eth_addr: address};
}

function getWireSigFromEth(sig: string) {
    // manually removed the 0x in the beginning
    if (sig.length == 132) sig = sig.slice(2);
    else if (sig.length !== 130) throw new Error('Invalid signature length');

    // the ethereum signature is in the order of : r,s,v
    // the wire signature is in the order of : v,r,s
    const w_r = sig.slice(0, 64);
    const w_s = sig.slice(64, 128);
    const w_v = (parseInt(sig.slice(128, 130), 16) + 4).toString(16);
    const wire_sig_for_contract = '00' + w_v + w_r + w_s;
    return new Uint8Array(Buffer.from(wire_sig_for_contract, 'hex'));
}

function get_checksum(address: string) {
    const buffer = Buffer.from(address, 'hex');
    return Buffer.from(sha256(buffer)).toString('hex');
}