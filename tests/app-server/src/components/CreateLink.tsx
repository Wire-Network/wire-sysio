import { useState } from "react"
import { WireChain } from "../types"
import { getApi } from "../services/chain"
import { retrievePubKey } from "../services/key"
import { createLinkTransaction } from "../services/transaction"
import { useToast } from "./Toast"
import { shortenAddress } from "../services/wallet"

interface Props {
  chain: WireChain
  username: string
  address: string
  onComplete: () => void
  onBack: () => void
}

export default function CreateLink({
  chain,
  username,
  address,
  onComplete,
  onBack
}: Props) {
  const [done, setDone] = useState(false)
  const [loading, setLoading] = useState(false)
  const { show } = useToast()

  async function handleCreateLink() {
    setLoading(true)
    try {
      let pubKey = await retrievePubKey(address)
      if (pubKey.startsWith("0x")) pubKey = pubKey.slice(2)

      const api = getApi(chain)
      const result = await createLinkTransaction(api, chain, username, pubKey)

      if (result?.processed) {
        setDone(true)
        show("Link created successfully!", "success")
      } else {
        throw new Error("Transaction not processed")
      }
    } catch (e: any) {
      show(e.message || "Failed to create link", "danger")
    } finally {
      setLoading(false)
    }
  }

  const linkVisual = (connected: boolean) => (
    <div className={`link-visual${connected ? " connected" : ""}`}>
      <span>@ {username}</span>
      <span className="connector">
        {connected ? "--- linked ---" : "--- link ---"}
      </span>
      <span className="mono">{shortenAddress(address)}</span>
    </div>
  )

  if (!done) {
    return (
      <div className="card">
        <div className="step-title">Create Link</div>
        <div className="step-subtitle">
          Officially link your Web3 wallet to your WNS account
        </div>

        <p
          style={{ fontSize: 14, color: "var(--text-muted)", marginBottom: 16 }}
        >
          By clicking confirm you agree to link the following wallet with your
          account.
        </p>

        {linkVisual(false)}

        <button
          className="btn-primary btn-full"
          onClick={handleCreateLink}
          disabled={loading}
        >
          {loading ? <span className="spinner" /> : "Create Link"}
        </button>

        <button
          className="btn-secondary btn-full"
          style={{ marginTop: 8 }}
          onClick={onBack}
          disabled={loading}
        >
          Back
        </button>
      </div>
    )
  }

  return (
    <div className="card">
      <div className="step-title">Create Link Complete</div>

      <p style={{ fontSize: 14, color: "var(--text-muted)", marginBottom: 16 }}>
        Your WNS account and wallet have been successfully linked.
      </p>

      {linkVisual(true)}

      <button className="btn-success btn-full" onClick={onComplete}>
        Done
      </button>
    </div>
  )
}
