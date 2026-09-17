#!/usr/bin/env node
// SPDX-License-Identifier: MIT
/**
 * `tsro` — configuration tooling and the protocol test harness.
 *
 * Everything here runs outside the game and outside TeamSpeak. It cannot change a live overlay;
 * it reads and writes configuration files and, for `mock`, speaks the plugin's side of the wire.
 */
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';

import { CONFIG_VERSION, RULES, parseAndValidate, type Issue } from './config-schema.js';
import { MockPlugin, defaultEndpoint } from './mock-plugin.js';
import { SCENARIOS, runScenario } from './scenario.js';

const USAGE = `tsro — TeamSpeak ReShade Overlay tooling

Usage:
  tsro validate <file...>              Check configuration files and report every problem
  tsro migrate <file> [--out <file>]   Upgrade a configuration to the current version
  tsro format <file> [--out <file>]    Rewrite a configuration canonically
  tsro diff <a> <b>                    Show which settings differ between two configurations
  tsro profiles [--dir <dir>]          List saved profiles
  tsro show <file> [--section <name>]  Print a configuration, or one section of it
  tsro mock [options]                  Run a mock plugin the overlay can connect to
  tsro schema                          Print the validation rules as JSON

Options for 'mock':
  --endpoint <name>      Pipe name or socket path (default: the overlay's default)
  --scenario <name>      ${Object.keys(SCENARIOS).join(', ')}
  --heartbeat <ms>       Heartbeat interval (default 2000)
  --quiet                Only report errors

Exit codes: 0 success, 1 validation problems found, 2 usage error.
`;

interface Args {
  positional: string[];
  flags: Map<string, string | boolean>;
}

function parseArgs(argv: string[]): Args {
  const positional: string[] = [];
  const flags = new Map<string, string | boolean>();
  for (let i = 0; i < argv.length; i += 1) {
    const argument = argv[i]!;
    if (!argument.startsWith('--')) {
      positional.push(argument);
      continue;
    }
    const name = argument.slice(2);
    const next = argv[i + 1];
    if (next !== undefined && !next.startsWith('--')) {
      flags.set(name, next);
      i += 1;
    } else {
      flags.set(name, true);
    }
  }
  return { positional, flags };
}

function severityPrefix(issue: Issue): string {
  switch (issue.severity) {
    case 'error': return 'error  ';
    case 'warning': return 'warning';
    case 'info': return 'note   ';
  }
}

function reportIssues(file: string, issues: Issue[]): number {
  if (issues.length === 0) {
    console.log(`${file}: ok`);
    return 0;
  }
  console.log(`${file}:`);
  for (const issue of issues) {
    const where = issue.path.length > 0 ? ` ${issue.path}:` : '';
    console.log(`  ${severityPrefix(issue)}${where} ${issue.message}`);
  }
  return issues.some((issue) => issue.severity === 'error') ? 1 : 0;
}

function defaultConfigDir(): string {
  if (process.platform === 'win32') {
    const appData = process.env.APPDATA;
    return appData ? path.join(appData, 'TeamSpeakReShadeOverlay') : '';
  }
  const xdg = process.env.XDG_CONFIG_HOME;
  if (xdg) return path.join(xdg, 'tsro');
  return path.join(os.homedir(), '.config', 'tsro');
}

function readOrExit(file: string): string {
  try {
    return fs.readFileSync(file, 'utf8');
  } catch (error) {
    console.error(`cannot read ${file}: ${(error as Error).message}`);
    process.exit(2);
  }
}

function commandValidate(args: Args): number {
  if (args.positional.length === 0) {
    console.error('validate: give at least one file');
    return 2;
  }
  let worst = 0;
  for (const file of args.positional) {
    const result = parseAndValidate(readOrExit(file));
    worst = Math.max(worst, reportIssues(file, result.issues));
    if (result.newerThanSupported) {
      console.log('  note    written by a newer version; unknown settings were preserved');
    }
  }
  return worst;
}

function commandMigrate(args: Args): number {
  const [file] = args.positional;
  if (!file) {
    console.error('migrate: give a file');
    return 2;
  }
  const result = parseAndValidate(readOrExit(file));
  if (result.usedDefaults) {
    console.error(`${file}: could not be parsed; nothing was written`);
    return 1;
  }
  if (result.newerThanSupported) {
    // Refuse rather than silently downgrading a file this build does not fully understand.
    console.error(
      `${file}: written by a newer version (config_version above ${CONFIG_VERSION}); ` +
        'refusing to rewrite it',
    );
    return 1;
  }
  const output = (args.flags.get('out') as string | undefined) ?? file;
  fs.writeFileSync(output, `${JSON.stringify(result.config, null, 2)}\n`, 'utf8');
  console.log(
    result.migrated
      ? `${file}: migrated to version ${CONFIG_VERSION} -> ${output}`
      : `${file}: already at version ${CONFIG_VERSION}; rewrote -> ${output}`,
  );
  reportIssues(file, result.issues);
  return 0;
}

function commandFormat(args: Args): number {
  const [file] = args.positional;
  if (!file) {
    console.error('format: give a file');
    return 2;
  }
  const result = parseAndValidate(readOrExit(file));
  if (result.usedDefaults) {
    console.error(`${file}: could not be parsed`);
    return 1;
  }
  const output = (args.flags.get('out') as string | undefined) ?? file;
  fs.writeFileSync(output, `${JSON.stringify(result.config, null, 2)}\n`, 'utf8');
  console.log(`${file} -> ${output}`);
  return 0;
}

function flatten(value: unknown, prefix = ''): Map<string, unknown> {
  const out = new Map<string, unknown>();
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    out.set(prefix, value);
    return out;
  }
  for (const [key, child] of Object.entries(value as Record<string, unknown>)) {
    const childPath = prefix.length > 0 ? `${prefix}.${key}` : key;
    for (const [k, v] of flatten(child, childPath)) out.set(k, v);
  }
  return out;
}

function commandDiff(args: Args): number {
  const [left, right] = args.positional;
  if (!left || !right) {
    console.error('diff: give two files');
    return 2;
  }
  const a = flatten(parseAndValidate(readOrExit(left)).config);
  const b = flatten(parseAndValidate(readOrExit(right)).config);
  const keys = [...new Set([...a.keys(), ...b.keys()])].sort();

  let differences = 0;
  for (const key of keys) {
    const valueA = a.get(key);
    const valueB = b.get(key);
    if (JSON.stringify(valueA) === JSON.stringify(valueB)) continue;
    differences += 1;
    console.log(`${key}`);
    console.log(`  ${left}: ${valueA === undefined ? '(unset)' : JSON.stringify(valueA)}`);
    console.log(`  ${right}: ${valueB === undefined ? '(unset)' : JSON.stringify(valueB)}`);
  }
  console.log(differences === 0 ? 'identical' : `${differences} setting(s) differ`);
  return 0;
}

function commandProfiles(args: Args): number {
  const dir = (args.flags.get('dir') as string | undefined) ?? defaultConfigDir();
  const profileDir = path.join(dir, 'profiles');
  if (!fs.existsSync(profileDir)) {
    console.log(`no profiles found in ${profileDir}`);
    return 0;
  }
  const entries = fs
    .readdirSync(profileDir)
    .filter((name) => name.endsWith('.json'))
    .sort();
  if (entries.length === 0) {
    console.log(`no profiles found in ${profileDir}`);
    return 0;
  }
  console.log(`profiles in ${profileDir}:`);
  let worst = 0;
  for (const entry of entries) {
    const full = path.join(profileDir, entry);
    const result = parseAndValidate(fs.readFileSync(full, 'utf8'));
    const problems = result.issues.filter((issue) => issue.severity !== 'info').length;
    const size = fs.statSync(full).size;
    console.log(
      `  ${path.basename(entry, '.json').padEnd(24)} ${String(size).padStart(7)} bytes` +
        (problems > 0 ? `  (${problems} problem(s))` : ''),
    );
    if (result.usedDefaults) worst = 1;
  }

  const mapping = path.join(dir, 'game-profiles.json');
  if (fs.existsSync(mapping)) {
    try {
      const parsed = JSON.parse(fs.readFileSync(mapping, 'utf8')) as Record<string, unknown>;
      const games = (parsed.games ?? {}) as Record<string, unknown>;
      const names = Object.keys(games);
      if (names.length > 0) {
        console.log('per-game profiles:');
        for (const name of names.sort()) console.log(`  ${name} -> ${String(games[name])}`);
      }
    } catch {
      console.log('per-game profile mapping could not be read');
      worst = 1;
    }
  }
  return worst;
}

function commandShow(args: Args): number {
  const [file] = args.positional;
  if (!file) {
    console.error('show: give a file');
    return 2;
  }
  const result = parseAndValidate(readOrExit(file));
  const section = args.flags.get('section') as string | undefined;
  const value = section ? (result.config as Record<string, unknown>)[section] : result.config;
  if (section && value === undefined) {
    console.error(`no section named '${section}'`);
    return 1;
  }
  console.log(JSON.stringify(value, null, 2));
  return 0;
}

async function commandMock(args: Args): Promise<number> {
  const quiet = args.flags.get('quiet') === true;
  const scenarioName = (args.flags.get('scenario') as string | undefined) ?? 'idle';
  const scenario = SCENARIOS[scenarioName];
  if (!scenario) {
    console.error(`unknown scenario '${scenarioName}'. Available: ${Object.keys(SCENARIOS).join(', ')}`);
    return 2;
  }
  const heartbeat = Number(args.flags.get('heartbeat') ?? 2000);

  const plugin = new MockPlugin({
    endpoint: args.flags.get('endpoint') as string | undefined,
    heartbeatMs: Number.isFinite(heartbeat) ? heartbeat : 2000,
    onLog: quiet ? undefined : (message) => console.log(`[mock] ${message}`),
  });

  try {
    await plugin.start();
  } catch (error) {
    console.error(`could not listen on ${defaultEndpoint(args.flags.get('endpoint') as string)}: ${(error as Error).message}`);
    return 1;
  }

  console.log(`mock plugin listening on ${plugin.endpoint}`);
  console.log(`scenario: ${scenario.name} — ${scenario.description}`);
  console.log('This is a test harness. It is not TeamSpeak and produces no real voice data.');
  console.log('Press Ctrl+C to stop.');

  const stop = runScenario(plugin, scenario, (step) => {
    if (!quiet) console.log(`[mock] ${step.description}`);
  });

  await new Promise<void>((resolve) => {
    const shutdown = () => {
      stop();
      void plugin.stop().then(resolve);
    };
    process.on('SIGINT', shutdown);
    process.on('SIGTERM', shutdown);
  });
  return 0;
}

function commandSchema(): number {
  // Emitting the rules makes the two schema implementations comparable by machine, which is what
  // the parity test uses.
  console.log(JSON.stringify({ config_version: CONFIG_VERSION, rules: RULES }, null, 2));
  return 0;
}

async function main(): Promise<void> {
  const [, , command, ...rest] = process.argv;
  const args = parseArgs(rest);

  if (!command || command === '--help' || command === '-h' || command === 'help') {
    console.log(USAGE);
    process.exit(command ? 0 : 2);
  }

  let code = 0;
  switch (command) {
    case 'validate': code = commandValidate(args); break;
    case 'migrate': code = commandMigrate(args); break;
    case 'format': code = commandFormat(args); break;
    case 'diff': code = commandDiff(args); break;
    case 'profiles': code = commandProfiles(args); break;
    case 'show': code = commandShow(args); break;
    case 'mock': code = await commandMock(args); break;
    case 'schema': code = commandSchema(); break;
    default:
      console.error(`unknown command '${command}'\n`);
      console.log(USAGE);
      code = 2;
  }
  process.exit(code);
}

void main();
