import tracer from "tracer"

/**
 * Plugin logger — writes to stderr exclusively.
 * stdout is reserved for the CodeGeneratorResponse wire payload.
 */
export const log = tracer.colorConsole({
  level: "info",
  format: "{{timestamp}} [{{title}}] {{file}}:{{line}} — {{message}}",
  dateformat: "HH:MM:ss.L",
  transport: function (data) {
    process.stderr.write(data.output + "\n")
  }
})

/** Set log level at runtime (e.g. via plugin parameter) */
export function setLogLevel(level: string): void {
  const validLevels = ["log", "trace", "debug", "info", "warn", "error"]
  if (validLevels.includes(level)) {
    ;(log as any).setLevel(level)
  }
}
