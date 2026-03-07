import { ethers } from "ethers"

declare global {
  interface Window {
    ethereum?: any
  }
}

export function isMetaMaskInstalled(): boolean {
  return typeof window !== "undefined" && !!window.ethereum
}

export async function requestAccounts(): Promise<string[]> {
  if (!window.ethereum) throw new Error("MetaMask is not installed")
  return window.ethereum.request({ method: "eth_requestAccounts" })
}

function getProvider(): ethers.BrowserProvider {
  return new ethers.BrowserProvider(window.ethereum)
}

export async function signMessage(
  message: string | Uint8Array
): Promise<string> {
  const provider = getProvider()
  const signer = await provider.getSigner()
  return signer.signMessage(message)
}

export function shortenAddress(address: string): string {
  return `${address.slice(0, 6)}...${address.slice(-4)}`
}

export function onAccountsChanged(
  cb: (accounts: string[]) => void
): () => void {
  if (!window.ethereum) return () => {}
  window.ethereum.on("accountsChanged", cb)
  return () => window.ethereum.removeListener("accountsChanged", cb)
}
