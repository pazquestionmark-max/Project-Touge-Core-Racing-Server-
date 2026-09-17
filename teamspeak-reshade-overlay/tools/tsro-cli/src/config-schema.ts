// SPDX-License-Identifier: MIT
/**
 * The configuration schema, in TypeScript.
 *
 * Deliberately a second implementation of the rules in shared/src/config.cpp, not a wrapper
 * around it. The schema-parity test loads a configuration produced by the C++ build and asserts
 * this validator agrees about defaults, ranges and migration, so the two cannot drift apart
 * without a test failing.
 */

export const CONFIG_VERSION = 1;

export type Severity = 'info' | 'warning' | 'error';

export interface Issue {
  severity: Severity;
  path: string;
  message: string;
}

export interface ValidationResult {
  /** The repaired document. Validation always produces something usable. */
  config: Record<string, unknown>;
  issues: Issue[];
  migrated: boolean;
  newerThanSupported: boolean;
  usedDefaults: boolean;
}

interface NumberRule {
  kind: 'number';
  min: number;
  max: number;
  integer?: boolean;
}
interface BooleanRule {
  kind: 'boolean';
}
interface StringRule {
  kind: 'string';
  maxLength: number;
}
interface ColorRule {
  kind: 'color';
}
interface EnumRule {
  kind: 'enum';
  values: readonly string[];
}
type Rule = NumberRule | BooleanRule | StringRule | ColorRule | EnumRule;

export const ANCHORS = [
  'top_left', 'top_center', 'top_right',
  'center_left', 'center', 'center_right',
  'bottom_left', 'bottom_center', 'bottom_right',
] as const;
export const ALIGNS = ['left', 'center', 'right'] as const;
export const ICONS = [
  'none', 'dot', 'circle', 'ring', 'square', 'diamond', 'triangle', 'star', 'chevron',
  'microphone', 'microphone_muted', 'speaker', 'speaker_muted', 'moon', 'record', 'crown',
  'whisper', 'bars',
] as const;
export const EASINGS = [
  'linear', 'ease_in', 'ease_out', 'ease_in_out', 'ease_out_back', 'ease_out_elastic',
] as const;
export const OVERFLOW_MODES = ['clip', 'ellipsis', 'wrap', 'shrink', 'scroll'] as const;
export const USER_SORTS = ['channel_order', 'alphabetical', 'talk_power', 'speaking_first'] as const;
export const SPEAKING_ANIMATIONS = ['none', 'color_fade', 'pulse', 'glow', 'border_sweep'] as const;
export const CHAT_ORDERS = ['newest_bottom', 'newest_top'] as const;
export const STACK_DIRECTIONS = ['down', 'up'] as const;
export const LOG_LEVELS = ['trace', 'debug', 'info', 'warn', 'error', 'off'] as const;

/**
 * Rules for the fields with a constrained range or vocabulary, keyed by dotted path. Fields not
 * listed are passed through unchanged; that is deliberate, so a configuration written by a newer
 * build survives a round trip through this tool intact.
 */
export const RULES: Readonly<Record<string, Rule>> = {
  'general.enabled': { kind: 'boolean' },
  'general.show_when_disconnected': { kind: 'boolean' },
  'general.show_when_plugin_unavailable': { kind: 'boolean' },
  'general.master_opacity': { kind: 'number', min: 0, max: 1 },
  'general.scale': { kind: 'number', min: 0.25, max: 4 },
  'general.profile_name': { kind: 'string', maxLength: 64 },
  'general.auto_profile_by_executable': { kind: 'boolean' },

  'appearance.font_size': { kind: 'number', min: 6, max: 96 },
  'appearance.icon_size': { kind: 'number', min: 2, max: 96 },
  'appearance.row_height': { kind: 'number', min: 6, max: 160 },
  'appearance.row_spacing': { kind: 'number', min: 0, max: 64 },
  'appearance.padding_x': { kind: 'number', min: 0, max: 128 },
  'appearance.padding_y': { kind: 'number', min: 0, max: 128 },
  'appearance.corner_radius': { kind: 'number', min: 0, max: 32 },
  'appearance.panel_border_thickness': { kind: 'number', min: 0, max: 12 },
  'appearance.text_shadow_offset': { kind: 'number', min: 0, max: 8 },
  'appearance.panel_background': { kind: 'color' },
  'appearance.panel_border': { kind: 'color' },
  'appearance.text_shadow_color': { kind: 'color' },
  'appearance.text_default': { kind: 'color' },
  'appearance.text_secondary': { kind: 'color' },
  'appearance.accent': { kind: 'color' },
  'appearance.show_panel_background': { kind: 'boolean' },
  'appearance.text_shadow': { kind: 'boolean' },

  'channel_title.font_scale': { kind: 'number', min: 0.2, max: 6 },
  'channel_title.opacity': { kind: 'number', min: 0, max: 1 },
  'channel_title.icon': { kind: 'enum', values: ICONS },
  'channel_title.format': { kind: 'string', maxLength: 200 },
  'channel_title.parent_format': { kind: 'string', maxLength: 200 },
  'channel_title.disconnected_text': { kind: 'string', maxLength: 120 },

  'user_list.sort': { kind: 'enum', values: USER_SORTS },
  'user_list.name_overflow': { kind: 'enum', values: OVERFLOW_MODES },
  'user_list.max_name_width': { kind: 'number', min: 20, max: 2000 },
  'user_list.min_font_scale': { kind: 'number', min: 0.2, max: 1 },
  'user_list.max_visible_users': { kind: 'number', min: 1, max: 512, integer: true },
  'user_list.indicator_gap': { kind: 'number', min: 0, max: 64 },
  'user_list.local_user_color': { kind: 'color' },

  'notifications.max_visible': { kind: 'number', min: 1, max: 20, integer: true },
  'notifications.stack': { kind: 'enum', values: STACK_DIRECTIONS },
  'notifications.spacing': { kind: 'number', min: 0, max: 64 },
  'notifications.width': { kind: 'number', min: 80, max: 2000 },
  'notifications.min_height': { kind: 'number', min: 8, max: 400 },
  'notifications.suppress_after_connect_ms': { kind: 'number', min: 0, max: 60000, integer: true },

  'chat.order': { kind: 'enum', values: CHAT_ORDERS },
  'chat.max_visible_messages': { kind: 'number', min: 1, max: 50, integer: true },
  'chat.history_size': { kind: 'number', min: 1, max: 500, integer: true },
  'chat.max_message_length': { kind: 'number', min: 1, max: 1024, integer: true },
  'chat.retention_seconds': { kind: 'number', min: 0, max: 86400, integer: true },
  'chat.width': { kind: 'number', min: 80, max: 4000 },
  'chat.font_scale': { kind: 'number', min: 0.2, max: 6 },
  'chat.show_private_messages': { kind: 'boolean' },

  'animation.speaking': { kind: 'enum', values: SPEAKING_ANIMATIONS },
  'animation.speaking_attack_ms': { kind: 'number', min: 0, max: 5000, integer: true },
  'animation.speaking_release_ms': { kind: 'number', min: 0, max: 5000, integer: true },
  'animation.speaking_pulse_hz': { kind: 'number', min: 0.1, max: 20 },
  'animation.speaking_pulse_depth': { kind: 'number', min: 0, max: 1 },
  'animation.state_easing': { kind: 'enum', values: EASINGS },
  'animation.state_transition_ms': { kind: 'number', min: 0, max: 5000, integer: true },
  'animation.list_reorder_ms': { kind: 'number', min: 0, max: 5000, integer: true },
  'animation.idle_after_ms': { kind: 'number', min: 1000, max: 3600000, integer: true },
  'animation.idle_opacity': { kind: 'number', min: 0, max: 1 },

  'integration.pipe_name': { kind: 'string', maxLength: 200 },
  'integration.reconnect_initial_ms': { kind: 'number', min: 50, max: 60000, integer: true },
  'integration.reconnect_max_ms': { kind: 'number', min: 100, max: 300000, integer: true },
  'integration.stale_after_ms': { kind: 'number', min: 1000, max: 120000, integer: true },
  'integration.ping_interval_ms': { kind: 'number', min: 1000, max: 600000, integer: true },

  'logging.level': { kind: 'enum', values: LOG_LEVELS },
  'logging.max_file_kb': { kind: 'number', min: 16, max: 102400, integer: true },
  'logging.include_message_content': { kind: 'boolean' },
};

const COLOR_PATTERN = /^#?(?:[0-9a-fA-F]{3,4}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/;

export function isColor(value: unknown): value is string {
  return typeof value === 'string' && COLOR_PATTERN.test(value);
}

/** Expands any accepted colour form to the canonical #RRGGBBAA, matching Color::to_hex. */
export function normaliseColor(value: string): string | undefined {
  if (!isColor(value)) return undefined;
  const hex = value.startsWith('#') ? value.slice(1) : value;
  const double = (c: string) => c + c;
  let rgba: string;
  if (hex.length === 3) rgba = hex.split('').map(double).join('') + 'FF';
  else if (hex.length === 4) rgba = hex.split('').map(double).join('');
  else if (hex.length === 6) rgba = hex + 'FF';
  else rgba = hex;
  return `#${rgba.toUpperCase()}`;
}

function getPath(root: Record<string, unknown>, path: string): unknown {
  let current: unknown = root;
  for (const part of path.split('.')) {
    if (typeof current !== 'object' || current === null) return undefined;
    current = (current as Record<string, unknown>)[part];
  }
  return current;
}

function setPath(root: Record<string, unknown>, path: string, value: unknown): void {
  const parts = path.split('.');
  let current = root;
  for (let i = 0; i < parts.length - 1; i += 1) {
    const key = parts[i] as string;
    const next = current[key];
    if (typeof next !== 'object' || next === null || Array.isArray(next)) return;
    current = next as Record<string, unknown>;
  }
  current[parts[parts.length - 1] as string] = value;
}

function deletePath(root: Record<string, unknown>, path: string): void {
  const parts = path.split('.');
  let current: Record<string, unknown> = root;
  for (let i = 0; i < parts.length - 1; i += 1) {
    const next = current[parts[i] as string];
    if (typeof next !== 'object' || next === null) return;
    current = next as Record<string, unknown>;
  }
  delete current[parts[parts.length - 1] as string];
}

/** The key shape for per-channel overrides: "<server_unique_id>:<channel_id>". */
export function isChannelOverrideKey(key: string): boolean {
  const separator = key.lastIndexOf(':');
  if (separator <= 0 || separator === key.length - 1) return false;
  return /^\d+$/.test(key.slice(separator + 1));
}

/**
 * Validates and repairs a configuration document. Never throws and never fails: out-of-range
 * values are clamped, wrong types are dropped back to the build's default, and every repair is
 * reported. Unknown keys are preserved.
 */
export function validate(input: unknown): ValidationResult {
  const issues: Issue[] = [];
  const add = (severity: Severity, path: string, message: string) =>
    issues.push({ severity, path, message });

  if (typeof input !== 'object' || input === null || Array.isArray(input)) {
    add('error', '', 'document is not an object; using defaults');
    return {
      config: { config_version: CONFIG_VERSION },
      issues,
      migrated: false,
      newerThanSupported: false,
      usedDefaults: true,
    };
  }

  const config = structuredClone(input) as Record<string, unknown>;
  let migrated = false;
  let newerThanSupported = false;

  const version = config.config_version;
  if (typeof version !== 'number') {
    add('info', 'config_version', 'missing; assuming version 1');
    config.config_version = CONFIG_VERSION;
  } else if (version > CONFIG_VERSION) {
    add(
      'warning',
      'config_version',
      'written by a newer version; unknown settings are preserved but not applied',
    );
    newerThanSupported = true;
  } else if (version < CONFIG_VERSION) {
    // Each step upgrades exactly one version, matching the C++ migrate().
    add('info', 'config_version', `migrated from version ${version}`);
    config.config_version = CONFIG_VERSION;
    migrated = true;
  }

  for (const [path, rule] of Object.entries(RULES)) {
    const value = getPath(config, path);
    if (value === undefined) continue;

    switch (rule.kind) {
      case 'boolean':
        if (typeof value !== 'boolean') {
          add('warning', path, 'expected a boolean; removed so the default applies');
          deletePath(config, path);
        }
        break;
      case 'number': {
        if (typeof value !== 'number' || !Number.isFinite(value)) {
          add('warning', path, 'expected a number; removed so the default applies');
          deletePath(config, path);
          break;
        }
        let repaired = value;
        if (repaired < rule.min || repaired > rule.max) {
          add('info', path, 'out of range; clamped');
          repaired = Math.min(rule.max, Math.max(rule.min, repaired));
        }
        if (rule.integer) repaired = Math.round(repaired);
        if (repaired !== value) setPath(config, path, repaired);
        break;
      }
      case 'string':
        if (typeof value !== 'string') {
          add('warning', path, 'expected a string; removed so the default applies');
          deletePath(config, path);
        } else if (Array.from(value).length > rule.maxLength) {
          add('info', path, 'too long; truncated');
          setPath(config, path, Array.from(value).slice(0, rule.maxLength).join(''));
        }
        break;
      case 'color': {
        const normalised = typeof value === 'string' ? normaliseColor(value) : undefined;
        if (normalised === undefined) {
          add('warning', path, 'not a valid colour; removed so the default applies');
          deletePath(config, path);
        } else if (normalised !== value) {
          setPath(config, path, normalised);
        }
        break;
      }
      case 'enum':
        if (typeof value !== 'string' || !rule.values.includes(value)) {
          add('warning', path, 'unrecognised value; removed so the default applies');
          deletePath(config, path);
        }
        break;
    }
  }

  // Cross-field constraints, matching Config::clamp.
  const history = getPath(config, 'chat.history_size');
  const visible = getPath(config, 'chat.max_visible_messages');
  if (typeof history === 'number' && typeof visible === 'number' && visible > history) {
    add('info', 'chat.max_visible_messages', 'cannot exceed history_size; lowered');
    setPath(config, 'chat.max_visible_messages', history);
  }
  const initial = getPath(config, 'integration.reconnect_initial_ms');
  const maximum = getPath(config, 'integration.reconnect_max_ms');
  if (typeof initial === 'number' && typeof maximum === 'number' && maximum < initial) {
    add('info', 'integration.reconnect_max_ms', 'was below reconnect_initial_ms; raised to match');
    setPath(config, 'integration.reconnect_max_ms', initial);
  }

  // Per-channel overrides must be server-scoped: a bare channel id would apply one server's
  // theme to another server's channel.
  const channelOverrides = config.channel_overrides;
  if (typeof channelOverrides === 'object' && channelOverrides !== null) {
    for (const key of Object.keys(channelOverrides as Record<string, unknown>)) {
      if (!isChannelOverrideKey(key)) {
        add(
          'warning',
          `channel_overrides.${key}`,
          "key must be '<server_unique_id>:<channel_id>'; entry dropped",
        );
        delete (channelOverrides as Record<string, unknown>)[key];
      }
    }
  }

  const userOverrides = config.user_overrides;
  if (typeof userOverrides === 'object' && userOverrides !== null) {
    for (const key of Object.keys(userOverrides as Record<string, unknown>)) {
      if (key.length === 0 || key.length > 128) {
        add('warning', 'user_overrides', 'entry with an implausible identity key was dropped');
        delete (userOverrides as Record<string, unknown>)[key];
      }
    }
  }

  return { config, issues, migrated, newerThanSupported, usedDefaults: false };
}

/** Parses text and validates it. Malformed JSON is reported rather than thrown. */
export function parseAndValidate(text: string): ValidationResult {
  try {
    return validate(JSON.parse(text));
  } catch (error) {
    return {
      config: { config_version: CONFIG_VERSION },
      issues: [
        { severity: 'error', path: '', message: `could not be parsed: ${(error as Error).message}` },
      ],
      migrated: false,
      newerThanSupported: false,
      usedDefaults: true,
    };
  }
}
