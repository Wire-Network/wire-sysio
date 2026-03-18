import { ethers } from "ethers"
import * as wallet from "./wallet"

const PUB_KEY_MSG = "Retrieve Public Key"
const STORAGE_KEY = "authex-pub-keys"

export async function retrievePubKey(address: string): Promise<string> {
  const stored = getStoredPubKey(address)
  if (stored) return stored

  const sig = await wallet.signMessage(PUB_KEY_MSG)
  const msgHash = ethers.hashMessage(PUB_KEY_MSG)
  const pubKey = ethers.SigningKey.recoverPublicKey(msgHash, sig)

  storePubKey(address, pubKey)
  return pubKey
}

function getStoredPubKey(address: string): string | null {
  const keys = JSON.parse(localStorage.getItem(STORAGE_KEY) || "{}")
  return keys[address] || null
}

function storePubKey(address: string, pubKey: string): void {
  const keys = JSON.parse(localStorage.getItem(STORAGE_KEY) || "{}")
  keys[address] = pubKey
  localStorage.setItem(STORAGE_KEY, JSON.stringify(keys))
}
