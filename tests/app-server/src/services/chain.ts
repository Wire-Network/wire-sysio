import { APIClient, FetchProvider } from "@wireio/sdk-core"
import { WireChain } from "../types"

const CHAINS: WireChain[] = [
  {
    name: "Local",
    id: "local",
    endpoint: "http://localhost:8888",
    namespace: "sysio",
    coreSymbol: "SYS",
    contractAccount: "sysio.authex"
  }
]

const STORAGE_KEY = "authex-selected-chain"

export function getChains(): WireChain[] {
  return CHAINS
}

export function getSelectedChain(): WireChain {
  const stored = localStorage.getItem(STORAGE_KEY)
  if (stored) {
    const found = CHAINS.find(c => c.id === stored)
    if (found) return found
  }
  return CHAINS[0]
}

export function selectChain(id: string): WireChain {
  localStorage.setItem(STORAGE_KEY, id)
  return CHAINS.find(c => c.id === id) || CHAINS[0]
}

export function getApi(chain: WireChain): APIClient {
  return new APIClient({
    provider: new FetchProvider(chain.endpoint)
  })
}
