import { useState, useEffect, useCallback } from "react"
import { API } from "@wireio/sdk-core"
import { WireChain, AuthLink } from "../types"
import { getApi } from "../services/chain"
import * as walletService from "../services/wallet"
import { getAccount } from "../services/account"
import { checkLinkStatus } from "../services/link"

interface Props {
  chain: WireChain
  onProceedToLink: (username: string, address: string) => void
}

export default function WalletConnect({ chain, onProceedToLink }: Props) {
  const [accounts, setAccounts] = useState<string[]>([])
  const [selectedAddress, setSelectedAddress] = useState<string | null>(null)
  const [wireAccount, setWireAccount] = useState<API.v1.AccountObject | null>(
    null
  )
  const [wireUsername, setWireUsername] = useState("jon.eth.link")
  const [noAccount, setNoAccount] = useState(false)
  const [link, setLink] = useState<AuthLink | null>(null)
  const [loading, setLoading] = useState(false)

  const lookupAccount = useCallback(
    async (username: string) => {
      if (!username.trim()) return
      setLoading(true)
      try {
        const api = getApi(chain)
        const acct = await getAccount(api, username)
        if (acct) {
          setWireAccount(acct)
          setNoAccount(false)
          const linkResult = await checkLinkStatus(
            api,
            chain.contractAccount,
            username
          )
          setLink(linkResult)
        } else {
          setWireAccount(null)
          setNoAccount(true)
          setLink(null)
        }
      } catch {
        setNoAccount(true)
        setWireAccount(null)
        setLink(null)
      } finally {
        setLoading(false)
      }
    },
    [chain]
  )

  useEffect(() => {
    return walletService.onAccountsChanged(accts => {
      if (accts.length === 0) {
        setAccounts([])
        setSelectedAddress(null)
        setWireAccount(null)
        setLink(null)
      } else {
        setAccounts(accts)
        if (selectedAddress && !accts.includes(selectedAddress)) {
          setSelectedAddress(null)
          setWireAccount(null)
          setLink(null)
        }
      }
    })
  }, [selectedAddress])

  async function connect() {
    try {
      const accts = await walletService.requestAccounts()
      setAccounts(accts)
    } catch (e: any) {
      alert(e.message)
    }
  }

  function disconnect() {
    setAccounts([])
    setSelectedAddress(null)
    setWireAccount(null)
    setWireUsername("")
    setLink(null)
    setNoAccount(false)
  }

  // Not connected
  if (accounts.length === 0) {
    return (
      <div className="card" style={{ textAlign: "center" }}>
        <p style={{ marginBottom: 16, color: "var(--text-muted)" }}>
          Connect your MetaMask wallet to get started
        </p>
        <button className="btn-primary" onClick={connect}>
          Connect MetaMask
        </button>
      </div>
    )
  }

  // Connected but no account selected
  if (!selectedAddress) {
    return (
      <div className="card">
        <h3 style={{ marginBottom: 12 }}>Select an Account</h3>
        {accounts.map(addr => (
          <button
            key={addr}
            className="account-btn"
            onClick={() => setSelectedAddress(addr)}
          >
            {walletService.shortenAddress(addr)}
          </button>
        ))}
      </div>
    )
  }

  // Account selected
  return (
    <div className="card">
      <h3 className="mono" style={{ marginBottom: 12 }}>
        {walletService.shortenAddress(selectedAddress)}
      </h3>

      <div className="section">
        <h4>Wire Username</h4>
        <div style={{ display: "flex", gap: 8, marginTop: 8 }}>
          <input
            type="text"
            autoFocus
            autoComplete="on"
            placeholder="e.g. myaccount"
            value={wireUsername}
            onChange={e => setWireUsername(e.target.value.toLowerCase().trim())}
            onKeyDown={e => {
              if (e.key === "Enter") lookupAccount(wireUsername)
            }}
            style={{
              flex: 1,
              background: "var(--surface-light)",
              color: "var(--text)",
              border: "1px solid var(--border)",
              borderRadius: "var(--radius)",
              padding: "8px 12px",
              fontSize: 14,
              fontFamily: "monospace",
              outline: "none"
            }}
          />
          <button
            className="btn-primary"
            onClick={() => lookupAccount(wireUsername)}
            disabled={!wireUsername.trim() || loading}
          >
            {loading ? <span className="spinner" /> : "Lookup"}
          </button>
        </div>
      </div>

      {!loading && noAccount && (
        <div className="section">
          <span className="badge badge-danger">Account Not Found</span>
          <p style={{ marginTop: 8, color: "var(--text-muted)", fontSize: 14 }}>
            No Wire account found for &quot;{wireUsername}&quot;.
          </p>
        </div>
      )}

      {!loading && wireAccount && wireUsername && (
        <>
          <div className="section">
            <span className="badge badge-success">Account Found</span>
            <div style={{ marginTop: 12 }}>
              <div className="detail-row">
                <span className="label">Wire Username</span>
                <span>{wireUsername}</span>
              </div>
              <div className="detail-row">
                <span className="label">Created</span>
                <span>
                  {new Date(
                    wireAccount.created.toString()
                  ).toLocaleDateString()}
                </span>
              </div>
              <div className="detail-row">
                <span className="label">CPU Available</span>
                <span>
                  {Number(
                    wireAccount.cpu_limit.available.toString()
                  ).toLocaleString()}{" "}
                  us
                </span>
              </div>
              <div className="detail-row">
                <span className="label">NET Available</span>
                <span>
                  {Number(
                    wireAccount.net_limit.available.toString()
                  ).toLocaleString()}{" "}
                  bytes
                </span>
              </div>
            </div>
          </div>

          <div className="section">
            <h4>Link Status</h4>
            {link ? (
              <>
                <span className="badge badge-success">Linked</span>
                <p
                  style={{
                    marginTop: 8,
                    color: "var(--text-muted)",
                    fontSize: 14
                  }}
                >
                  Your Ethereum address is linked to your Wire account.
                </p>
                <div style={{ marginTop: 8 }}>
                  <div className="detail-row">
                    <span className="label">Chain Kind</span>
                    <span>{link.chain_kind}</span>
                  </div>
                  <div className="detail-row">
                    <span className="label">Address</span>
                    <span className="mono" style={{ fontSize: 12 }}>
                      {link.address}
                    </span>
                  </div>
                </div>
              </>
            ) : (
              <>
                <span className="badge badge-warning">No Link</span>
                <p
                  style={{
                    marginTop: 8,
                    color: "var(--text-muted)",
                    fontSize: 14
                  }}
                >
                  No link between your Ethereum address and Wire account.
                </p>
                <button
                  className="btn-success"
                  style={{ marginTop: 12 }}
                  onClick={() => onProceedToLink(wireUsername, selectedAddress)}
                >
                  Create Link
                </button>
              </>
            )}
          </div>
        </>
      )}

      <div className="actions">
        <button
          className="btn-secondary"
          onClick={() => {
            setSelectedAddress(null)
            setWireAccount(null)
            setLink(null)
            setNoAccount(false)
            setWireUsername("")
          }}
        >
          Change Account
        </button>
        <button className="btn-danger" onClick={disconnect}>
          Disconnect
        </button>
      </div>
    </div>
  )
}
