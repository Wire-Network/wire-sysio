import { getChains } from "../services/chain"
import { WireChain } from "../types"

interface Props {
  selected: WireChain
  onChange: (chain: WireChain) => void
}

export default function ChainSelector({ selected, onChange }: Props) {
  const chains = getChains()

  return (
    <select
      value={selected.id}
      onChange={e => {
        const chain = chains.find(c => c.id === e.target.value)
        if (chain) onChange(chain)
      }}
    >
      {chains.map(c => (
        <option key={c.id} value={c.id}>
          {c.name} ({c.endpoint})
        </option>
      ))}
    </select>
  )
}
