import { execFile } from "node:child_process";
import { promisify } from "node:util";

const run = promisify(execFile);

export const BINARY = process.env.FAN_CONTROL_BINARY ?? "/usr/local/bin/fan_control";
const SUDO = "/usr/bin/sudo";
const SETUP = "Run scripts/install.sh from the macos-fan-control repo.";

export interface Fan {
  index: number;
  actual: number;
  minimum: number;
  maximum: number;
  target: number;
  percent: number;
  forced: boolean;
}

export interface Temperatures {
  cpu: number;
  cpuSensors: number;
  gpu: number;
  gpuSensors: number;
}

export interface FanStatus {
  fanCount: number;
  fans: Fan[];
  temperatures?: Temperatures;
}

export class FanControlError extends Error {}

function fail(error: unknown): FanControlError {
  const failure = error as { code?: string; stderr?: string; message?: string };
  if (failure.code === "ENOENT") return new FanControlError(`${BINARY} is not installed. ${SETUP}`);
  const stderr = (failure.stderr ?? "").trim();
  if (stderr.includes("password is required")) return new FanControlError(`sudo needs a password, the NOPASSWD rule is missing. ${SETUP}`);
  return new FanControlError(stderr || failure.message || "fan_control failed");
}

async function exec(args: string[], root: boolean): Promise<string> {
  try {
    const { stdout } = root ? await run(SUDO, ["-n", BINARY, ...args]) : await run(BINARY, args);
    return stdout;
  } catch (error) {
    throw fail(error);
  }
}

export async function readStatus(temps = false): Promise<FanStatus> {
  const args = temps ? ["status", "--json", "--temps"] : ["status", "--json"];
  return JSON.parse(await exec(args, false)) as FanStatus;
}

export const setPercent = (percent: number) => exec(["set", String(Math.round(percent))], true);
export const setMaximum = () => exec(["max"], true);
export const setAutomatic = () => exec(["auto"], true);
export const runGuard = (ceilingCelsius?: number) =>
  exec(ceilingCelsius === undefined ? ["guard"] : ["guard", "--max-temp", String(ceilingCelsius)], true);

export const rpm = (value: number) => `${Math.round(value).toLocaleString()} rpm`;
export const mode = (fans: Fan[]) => (fans.every((fan) => fan.forced) ? "Forced" : fans.some((fan) => fan.forced) ? "Mixed" : "Auto");
export const average = (fans: Fan[]) => (fans.length ? fans.reduce((sum, fan) => sum + fan.percent, 0) / fans.length : 0);
export const estimate = (fan: Fan, percent: number) => fan.minimum + ((fan.maximum - fan.minimum) * percent) / 100;
