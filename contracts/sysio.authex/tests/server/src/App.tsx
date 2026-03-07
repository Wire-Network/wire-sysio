import { useState, useCallback } from "react"
import { Routes, Route, useNavigate } from "react-router-dom"
import ChainSelector from "./components/ChainSelector"
import WalletConnect from "./components/WalletConnect"
import CreateLink from "./components/CreateLink"
import { ToastProvider } from "./components/Toast"
import { WireChain } from "./types"
import { getSelectedChain, selectChain } from "./services/chain"

export default function App() {
  const [chain, setChain] = useState<WireChain>(getSelectedChain())
  const [linkParams, setLinkParams] = useState<{
    username: string
    address: string
  } | null>(null)
  const navigate = useNavigate()

  const handleChainChange = useCallback((c: WireChain) => {
    selectChain(c.id)
    setChain(c)
  }, [])

  const handleProceedToLink = useCallback(
    (username: string, address: string) => {
      setLinkParams({ username, address })
      navigate("/create-link")
    },
    [navigate]
  )

  const handleLinkComplete = useCallback(() => {
    setLinkParams(null)
    navigate("/")
  }, [navigate])

  const handleLinkBack = useCallback(() => {
    setLinkParams(null)
    navigate("/")
  }, [navigate])

  return (
    <ToastProvider>
      <div className="app">
        <div className="app-header">
          <h1>AuthLink Test</h1>
          <ChainSelector selected={chain} onChange={handleChainChange} />
        </div>

        <Routes>
          <Route
            path="/"
            element={
              <WalletConnect
                chain={chain}
                onProceedToLink={handleProceedToLink}
              />
            }
          />
          <Route
            path="/create-link"
            element={
              linkParams ? (
                <CreateLink
                  chain={chain}
                  username={linkParams.username}
                  address={linkParams.address}
                  onComplete={handleLinkComplete}
                  onBack={handleLinkBack}
                />
              ) : (
                <div className="card">
                  <p>No account selected. Go back to connect.</p>
                  <button
                    className="btn-primary"
                    style={{ marginTop: 12 }}
                    onClick={() => navigate("/")}
                  >
                    Go Home
                  </button>
                </div>
              )
            }
          />
        </Routes>
      </div>
    </ToastProvider>
  )
}
