import {
  useState,
  useCallback,
  createContext,
  useContext,
  ReactNode
} from "react"

interface ToastMessage {
  id: number
  text: string
  color: "success" | "danger" | "warning"
}

interface ToastContextType {
  show: (text: string, color?: ToastMessage["color"]) => void
}

const ToastContext = createContext<ToastContextType>({ show: () => {} })

export function useToast() {
  return useContext(ToastContext)
}

let nextId = 0

export function ToastProvider({ children }: { children: ReactNode }) {
  const [toasts, setToasts] = useState<ToastMessage[]>([])

  const show = useCallback(
    (text: string, color: ToastMessage["color"] = "success") => {
      console.log("Toast:", text)
      const id = nextId++
      setToasts(prev => [...prev, { id, text, color }])
      setTimeout(() => {
        setToasts(prev => prev.filter(t => t.id !== id))
      }, 4000)
    },
    []
  )

  return (
    <ToastContext.Provider value={{ show }}>
      {children}
      <div className="toast-container">
        {toasts.map(t => (
          <div key={t.id} className={`toast toast-${t.color}`}>
            {t.text}
          </div>
        ))}
      </div>
    </ToastContext.Provider>
  )
}
