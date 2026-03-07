import {
  ABI,
  Action,
  AnyAction,
  API,
  APIClient,
  APIError,
  getCompressedPublicKey,
  KeyType,
  Name,
  PermissionLevel,
  PrivateKey,
  PublicKey,
  Signature,
  SignedTransaction,
  Transaction
} from "@wireio/sdk-core"
import { ethers } from "ethers"
import * as wallet from "./wallet"
import { ChainKind, WireChain } from "../types"

/**
 * Convert an array of AnyAction objects to Action objects using the provided API client.
 * This function fetches the ABI for each unique account involved in the actions and uses it to convert the AnyAction to Action.
 * NOTE: This is necessary because the Action.from() method requires the ABI to properly parse the action data.
 *
 * @param api - The API client to use for fetching ABIs
 * @param actions - An array of AnyAction objects to convert
 * @returns A promise that resolves to an array of Action objects
 */
async function anyToAction(
  api: APIClient,
  actions: AnyAction[]
): Promise<Action[]> {
  const result: Action[] = []
  const abiCache = new Map<string, ABI>()

  for (const act of actions) {
    const account = act.account.toString()
    if (!abiCache.has(account)) {
      const resp = await api.v1.chain.get_abi(account)
      if (resp.abi) abiCache.set(account, ABI.from(resp.abi))
    }
    const abi = abiCache.get(account)
    result.push(abi ? Action.from(act, abi) : Action.from(act))
  }
  return result
}

export async function pushTransaction(
  api: APIClient,
  action: AnyAction | AnyAction[]
): Promise<API.v1.PushTransactionResponse> {
  try {
    const actions = await anyToAction(
      api,
      Array.isArray(action) ? action : [action]
    )
    actions.forEach(act => {
      act.authorization = [
        PermissionLevel.from({
          actor: Name.from("jon.eth.link"),
          permission: Name.from("active")
        })
      ]
    })
    const info = await api.v1.chain.get_info()
    const header = info.getTransactionHeader()
    const transaction = Transaction.from({
      ...header,
      actions
    })
    const { msgDigest } = transaction.signingDigest(info.chain_id)

    const privKey = PrivateKey.fromString(process.env.AUTHEX_TEST_PRIVATE_KEY!)
    const signature = privKey.signDigest(msgDigest)
    const signedTrx = SignedTransaction.from({
      ...transaction,

      signatures: [signature]
    })

    try {
      return await api.v1.chain.push_transaction(signedTrx)
    } catch (e: any) {
      console.error("Error pushing transaction (remote):", e)
      if (e instanceof APIError) {
        throw new Error(
          e.details[0]?.message?.replace(/Error:/g, "") || e.message
        )
      }
      throw e
    }
  } catch (e: any) {
    console.error("Error pushing transaction:", e)
    throw e
  }
}

/**
 * Build the message the contract expects:
 * <pubkey_string>|<username>|<chain_kind>|<nonce>|createlink auth
 * The contract uses pubkey_to_string which gives PUB_EM_<hex> format.
 * The contract constructs the message internally from the provided pubKey param,
 * so we just need to sign the same message.
 *
 * NOTE: We are working on a contract client code generation tool that will
 *   eliminate the need for manually constructing messages like this.
 *
 * @param api - API client to use for pushing the transaction
 * @param chain - The WireChain object containing chain details
 * @param username - The username to link
 * @param rawPubKey - The uncompressed public key in hex format (0x...)
 * @returns The response from the push_transaction API call
 * @throws Error if the transaction fails to push
 */
export async function createLinkTransaction(
  api: APIClient,
  chain: WireChain,
  username: string,
  rawPubKey: string
): Promise<API.v1.PushTransactionResponse> {
  const compressed = getCompressedPublicKey(rawPubKey)
  const nonce = Date.now()
  const chainKind = ChainKind.Ethereum
  const compressedHex = compressed.startsWith("0x")
    ? compressed.slice(2)
    : compressed
  const pubKeyObj = PublicKey.from({
    type: "EM",
    compressed: ethers.getBytes("0x" + compressedHex)
  })
  const pubKeyString = pubKeyObj.toHexString() // "PUB_EM_..."

  const msg = `${pubKeyString}|${username}|${chainKind}|${nonce}|createlink auth`

  const msgHash = ethers.keccak256(ethers.toUtf8Bytes(msg))
  const messageBytes = ethers.getBytes(msgHash)
  const ethSig = await wallet.signMessage(messageBytes)
  const wireSig = Signature.fromHex(ethSig, KeyType.EM)

  const actionData = {
    account: chain.contractAccount,
    name: "createlink",
    authorization: [{ actor: username, permission: "active" }],
    data: {
      chain_kind: chainKind,
      username,
      sig: wireSig,
      pubKey: pubKeyString,
      nonce
    }
  }
  console.log("actionData", actionData)
  return pushTransaction(api, actionData)
}
