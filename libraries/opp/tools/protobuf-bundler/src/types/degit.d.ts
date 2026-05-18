declare module "degit" {
  interface DegitOptions {
    cache?: boolean
    force?: boolean
    verbose?: boolean
  }

  interface DegitInfo {
    message: string
  }

  interface DegitEmitter {
    on(event: "info", handler: (info: DegitInfo) => void): void
    on(event: "warn", handler: (info: DegitInfo) => void): void
    clone(dest: string): Promise<void>
  }

  export default function degit(
    src: string,
    opts?: DegitOptions
  ): DegitEmitter
}
