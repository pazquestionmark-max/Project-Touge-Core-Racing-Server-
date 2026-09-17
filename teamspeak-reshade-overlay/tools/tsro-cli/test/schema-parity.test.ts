// SPDX-License-Identifier: MIT
/**
 * Cross-validates the two schema implementations against each other.
 *
 * The overlay validates configuration in C++; this CLI validates it in TypeScript. Two
 * implementations of the same rules will drift apart unless something checks, and a user whose
 * file the CLI calls valid but the overlay repairs (or vice versa) has been lied to. These tests
 * run the real `tsro-config` binary and compare its behaviour with this package's validator.
 *
 * When the binary has not been built the suite skips rather than failing, so `npm test` works on
 * a machine that has not run CMake. CI builds it first, so the comparison always runs there.
 */
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

import {
  CONFIG_VERSION,
  RULES,
  isChannelOverrideKey,
  normaliseColor,
  parseAndValidate,
  validate,
} from '../src/config-schema.js';
import { MAX_CHAT_CHARS, MAX_MESSAGE_BYTES, MAX_USERS_PER_SNAPSHOT } from '../src/protocol.js';

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '..', '..', '..');

function findBinary(): string | undefined {
  const candidates = [
    path.join(repoRoot, 'build', 'tools', 'config-tool', 'tsro-config'),
    path.join(repoRoot, 'build', 'tools', 'config-tool', 'Release', 'tsro-config.exe'),
    path.join(repoRoot, 'build', 'tools', 'config-tool', 'tsro-config.exe'),
  ];
  return candidates.find((candidate) => fs.existsSync(candidate));
}

const binary = findBinary();
const describeIfBuilt = binary ? describe : describe.skip;

function run(...args: string[]): string {
  return execFileSync(binary!, args, { encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 });
}

interface Probe {
  config_version: number;
  ranges: Record<string, { min: number; max: number }>;
  limits: { max_message_bytes: number; max_chat_chars: number; max_users_per_snapshot: number };
}

describeIfBuilt('schema parity with the C++ implementation', () => {
  it('agrees on the configuration version', () => {
    const probe = JSON.parse(run('probe')) as Probe;
    expect(probe.config_version).toBe(CONFIG_VERSION);
  });

  it('agrees on the protocol limits', () => {
    const probe = JSON.parse(run('probe')) as Probe;
    expect(probe.limits.max_message_bytes).toBe(MAX_MESSAGE_BYTES);
    expect(probe.limits.max_chat_chars).toBe(MAX_CHAT_CHARS);
    expect(probe.limits.max_users_per_snapshot).toBe(MAX_USERS_PER_SNAPSHOT);
  });

  it('agrees on every numeric range it can probe', () => {
    const probe = JSON.parse(run('probe')) as Probe;
    const compared: string[] = [];

    for (const [field, range] of Object.entries(probe.ranges)) {
      const rule = RULES[field];
      // Only fields both implementations constrain can be compared; a field the TypeScript side
      // passes through is not a disagreement, it is simply not validated here.
      if (!rule || rule.kind !== 'number') continue;
      compared.push(field);
      expect(range.min, `${field} minimum`).toBeCloseTo(rule.min, 4);
      expect(range.max, `${field} maximum`).toBeCloseTo(rule.max, 4);
    }
    // Guard against this test silently comparing nothing if the probe output changes shape.
    expect(compared.length).toBeGreaterThanOrEqual(15);
  });

  it('accepts the C++ defaults without a single repair', () => {
    // The strongest statement of agreement: the shipped defaults must be clean under the other
    // implementation's rules too.
    const defaults = run('defaults');
    const result = parseAndValidate(defaults);
    expect(result.usedDefaults).toBe(false);
    expect(result.issues).toEqual([]);
  });

  it('produces a document the C++ validator also accepts', () => {
    const defaults = JSON.parse(run('defaults')) as Record<string, unknown>;
    const validated = validate(defaults);
    const file = path.join(os.tmpdir(), `tsro-parity-${process.pid}.json`);
    fs.writeFileSync(file, JSON.stringify(validated.config, null, 2), 'utf8');
    try {
      const output = run('validate', file);
      expect(output).toContain('ok');
    } finally {
      fs.unlinkSync(file);
    }
  });

  it('agrees that the same broken file is broken', () => {
    const file = path.join(os.tmpdir(), `tsro-parity-bad-${process.pid}.json`);
    fs.writeFileSync(
      file,
      JSON.stringify({
        config_version: 1,
        appearance: { font_size: 100000, accent: 'not a colour' },
        general: { scale: -5, enabled: 'yes' },
        logging: { level: 'loud' },
        channel_overrides: { '42': { title_color: '#FF0000' } },
      }),
      'utf8',
    );
    try {
      let cppOutput = '';
      try {
        cppOutput = run('validate', file);
      } catch (error) {
        cppOutput = String((error as { stdout?: string }).stdout ?? '');
      }
      // Both must flag the same four things: a clamped size, a bad colour, a bad enum, and a
      // channel override key that is not server-scoped.
      expect(cppOutput).toContain('appearance.font_size');
      expect(cppOutput).toContain('appearance.accent');
      expect(cppOutput).toContain('logging.level');
      expect(cppOutput).toContain('channel_overrides.42');

      const result = parseAndValidate(fs.readFileSync(file, 'utf8'));
      const paths = result.issues.map((issue) => issue.path);
      expect(paths).toContain('appearance.font_size');
      expect(paths).toContain('appearance.accent');
      expect(paths).toContain('logging.level');
      expect(paths).toContain('channel_overrides.42');
    } finally {
      fs.unlinkSync(file);
    }
  });

  it('agrees that a colour normalises the same way', () => {
    const file = path.join(os.tmpdir(), `tsro-parity-colour-${process.pid}.json`);
    fs.writeFileSync(
      file,
      JSON.stringify({ config_version: 1, appearance: { accent: '#f80' } }),
      'utf8',
    );
    try {
      const normalised = JSON.parse(run('normalise', file)) as Record<string, unknown>;
      const appearance = normalised.appearance as Record<string, unknown>;
      expect(appearance.accent).toBe(normaliseColor('#f80'));
      expect(appearance.accent).toBe('#FF8800FF');
    } finally {
      fs.unlinkSync(file);
    }
  });
});

describe('the TypeScript validator on its own', () => {
  it('never throws, whatever it is handed', () => {
    for (const input of ['', 'null', '[]', '"text"', '{', '{"a":', 'true', '0']) {
      expect(() => parseAndValidate(input)).not.toThrow();
    }
  });

  it('preserves keys it does not recognise', () => {
    const result = validate({ config_version: 1, from_the_future: { nested: [1, 2, 3] } });
    expect(result.config.from_the_future).toEqual({ nested: [1, 2, 3] });
  });

  it('flags a document from a newer version without downgrading it', () => {
    const result = validate({ config_version: 999, tomorrow: true });
    expect(result.newerThanSupported).toBe(true);
    expect(result.config.tomorrow).toBe(true);
    expect(result.config.config_version).toBe(999);
  });

  it('clamps out-of-range numbers', () => {
    const result = validate({ config_version: 1, general: { scale: 1000 } });
    const general = result.config.general as Record<string, unknown>;
    expect(general.scale).toBe(4);
    expect(result.issues.some((issue) => issue.path === 'general.scale')).toBe(true);
  });

  it('removes wrongly typed values so the default applies', () => {
    const result = validate({ config_version: 1, general: { enabled: 'yes' } });
    const general = result.config.general as Record<string, unknown>;
    expect(general).not.toHaveProperty('enabled');
  });

  it('enforces cross-field constraints', () => {
    const result = validate({
      config_version: 1,
      chat: { history_size: 5, max_visible_messages: 40 },
      integration: { reconnect_initial_ms: 2000, reconnect_max_ms: 100 },
    });
    const chat = result.config.chat as Record<string, unknown>;
    const integration = result.config.integration as Record<string, unknown>;
    expect(chat.max_visible_messages).toBe(5);
    expect(integration.reconnect_max_ms).toBe(2000);
  });

  it('requires channel override keys to be server-scoped', () => {
    expect(isChannelOverrideKey('srvUID=:42')).toBe(true);
    expect(isChannelOverrideKey('42')).toBe(false);
    expect(isChannelOverrideKey('srvUID=:')).toBe(false);
    expect(isChannelOverrideKey(':42')).toBe(false);
    expect(isChannelOverrideKey('srvUID=:abc')).toBe(false);
  });

  it('normalises every accepted colour form', () => {
    expect(normaliseColor('#f80')).toBe('#FF8800FF');
    expect(normaliseColor('f80')).toBe('#FF8800FF');
    expect(normaliseColor('#FF8800')).toBe('#FF8800FF');
    expect(normaliseColor('#FF880080')).toBe('#FF880080');
    expect(normaliseColor('#f808')).toBe('#FF880088');
    expect(normaliseColor('#12345')).toBeUndefined();
    expect(normaliseColor('nope')).toBeUndefined();
  });
});
