import tracer from "tracer"

let _level = "info"

function createLogger() {
  return tracer.colorConsole({
    level: _level,
    format: "{{timestamp}} [{{title}}] {{file}}:{{line}} — {{message}}",
    dateformat: "HH:MM:ss.L",
    transport: function (data) {
      process.stderr.write(data.output + "\n")
    }
  })
}

export let log = createLogger()

export function setLogLevel(level: string): void {
  const validLevels = ["log", "trace", "debug", "info", "warn", "error"]
  if (validLevels.includes(level)) {
    _level = level
    log = createLogger()
  }
}
