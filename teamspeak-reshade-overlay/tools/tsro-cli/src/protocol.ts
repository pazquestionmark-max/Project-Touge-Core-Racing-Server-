// SPDX-License-Identifier: MIT
/**
 * The IPC protocol, in TypeScript.
 *
 * This is a second, independent implementation of the wire format defined in docs/protocol.md.
 * Having two means the C++ implementation is validated against something other than itself: the
 * mock plugin here speaks the protocol to the real add-on, and the round-trip tests check that
 * both sides agree about framing, envelopes and limits.
 */

export const PROTOCOL_VERSION = 1;
export const PROTOCOL_MIN = 1;
export const PROTOCOL_MAX = 1;

/** Must match shared/include/tsro/protocol.hpp; the limit tests assert they do. */
export const MAX_MESSAGE_BYTES = 65536;
export const MAX_CHAT_CHARS = 1024;
export const MAX_USERS_PER_SNAPSHOT = 512;
export const HEARTBEAT_INTERVAL_MS = 2000;

export const SERVER_MESSAGE_TYPES = [
  'hello',
  'state_snapshot',
  'connection_changed',
  'channel_changed',
  'user_joined',
  'user_left',
  'user_updated',
  'speaking_changed',
  'mute_changed',
  'commander_changed',
  'whisper_changed',
  'chat_message',
  'heartbeat',
  'error',
] as const;

export const CLIENT_MESSAGE_TYPES = [
  'client_hello',
  'configuration_updated',
  'request_snapshot',
  'ping',
] as const;

export type ServerMessageType = (typeof SERVER_MESSAGE_TYPES)[number];
export type ClientMessageType = (typeof CLIENT_MESSAGE_TYPES)[number];
export type MessageType = ServerMessageType | ClientMessageType;

export interface Envelope {
  v: number;
  seq: number;
  ts: number;
  type: MessageType;
  server?: string;
  data?: Record<string, unknown>;
}

export type ConnectionState = 'disconnected' | 'connecting' | 'connected';
export type ChatCategory = 'channel' | 'server' | 'private';

/**
 * A user as it appears on the wire. Optional fields are *absent* when TeamSpeak could not supply
 * them — which is different from false, and consumers must treat it as "hide the indicator".
 */
export interface WireUser {
  client_id: number;
  unique_id: string;
  nickname: string;
  display_name?: string;
  talking?: boolean;
  whispering_to_me?: boolean;
  is_self?: boolean;
  input_muted?: boolean;
  output_muted?: boolean;
  input_hardware?: boolean;
  output_hardware?: boolean;
  input_deactivated?: boolean;
  away?: boolean;
  away_message?: string;
  recording?: boolean;
  channel_commander?: boolean;
  priority_speaker?: boolean;
  is_talker?: boolean;
  has_avatar?: boolean;
  locally_muted?: boolean;
  talk_power?: number;
  country?: string;
}

export interface WireChannel {
  id: number;
  name: string;
  parent_id?: number;
  parent_name?: string;
  path?: string;
  topic?: string;
}

export interface WireServer {
  handler_id: number;
  unique_id: string;
  name: string;
}

export interface WireSnapshot {
  connection: ConnectionState;
  server: WireServer;
  channel: WireChannel;
  self_unique_id: string;
  users: WireUser[];
}

export interface DecodeSuccess {
  ok: true;
  envelope: Envelope;
}

export interface DecodeFailure {
  ok: false;
  reason:
    | 'not_json'
    | 'not_object'
    | 'missing_field'
    | 'bad_version'
    | 'unknown_type'
    | 'too_large';
  /** True when the failure means the peer is malformed enough to warrant closing the stream. */
  fatal: boolean;
  message: string;
}

export type DecodeResult = DecodeSuccess | DecodeFailure;

const ALL_TYPES: ReadonlySet<string> = new Set<string>([
  ...SERVER_MESSAGE_TYPES,
  ...CLIENT_MESSAGE_TYPES,
]);

export function isServerToClient(type: string): boolean {
  return (SERVER_MESSAGE_TYPES as readonly string[]).includes(type);
}

/** Encodes one envelope as a wire line, terminator included. */
export function encodeLine(
  type: MessageType,
  seq: number,
  ts: number,
  data?: Record<string, unknown>,
  serverUid?: string,
): string {
  const envelope: Envelope = { v: PROTOCOL_VERSION, seq, ts, type };
  if (serverUid) envelope.server = serverUid;
  if (data) envelope.data = data;
  return `${JSON.stringify(envelope)}\n`;
}

/**
 * Decodes one line. Mirrors the C++ decoder's distinctions exactly: an unknown type or an
 * unsupported version is droppable, whereas malformed JSON or an oversized frame means the
 * stream itself cannot be trusted.
 */
export function decodeLine(line: string): DecodeResult {
  if (Buffer.byteLength(line, 'utf8') > MAX_MESSAGE_BYTES) {
    return { ok: false, reason: 'too_large', fatal: true, message: 'message exceeds the limit' };
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(line);
  } catch {
    return { ok: false, reason: 'not_json', fatal: true, message: 'not valid JSON' };
  }
  if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
    return { ok: false, reason: 'not_object', fatal: true, message: 'envelope is not an object' };
  }
  const record = parsed as Record<string, unknown>;
  if (typeof record.v !== 'number' || typeof record.type !== 'string') {
    return {
      ok: false,
      reason: 'missing_field',
      fatal: false,
      message: "envelope missing 'v' or 'type'",
    };
  }
  if (record.v < PROTOCOL_MIN || record.v > PROTOCOL_MAX) {
    return {
      ok: false,
      reason: 'bad_version',
      fatal: false,
      message: `unsupported protocol version ${record.v}`,
    };
  }
  if (!ALL_TYPES.has(record.type)) {
    // Forward compatibility: a newer peer may send types we do not know. Dropped, not fatal.
    return {
      ok: false,
      reason: 'unknown_type',
      fatal: false,
      message: `unknown message type '${record.type}'`,
    };
  }
  const envelope: Envelope = {
    v: record.v,
    seq: typeof record.seq === 'number' ? record.seq : 0,
    ts: typeof record.ts === 'number' ? record.ts : 0,
    type: record.type as MessageType,
  };
  if (typeof record.server === 'string') envelope.server = record.server;
  if (typeof record.data === 'object' && record.data !== null && !Array.isArray(record.data)) {
    envelope.data = record.data as Record<string, unknown>;
  }
  return { ok: true, envelope };
}

/**
 * Newline framing with the size cap enforced *while accumulating*, so a peer that never sends a
 * terminator cannot grow the buffer without bound. Mirrors LineFramer in C++.
 */
export class LineFramer {
  private pending = '';
  private failed = false;

  constructor(private readonly maxBytes: number = MAX_MESSAGE_BYTES) {}

  get overflowed(): boolean {
    return this.failed;
  }

  get pendingBytes(): number {
    return Buffer.byteLength(this.pending, 'utf8');
  }

  reset(): void {
    this.pending = '';
    this.failed = false;
  }

  /** Returns the complete lines found. Throws nothing; check `overflowed` after each call. */
  feed(chunk: string): string[] {
    if (this.failed) return [];
    const lines: string[] = [];
    let rest = chunk;

    while (rest.length > 0) {
      const index = rest.indexOf('\n');
      if (index === -1) {
        if (Buffer.byteLength(this.pending + rest, 'utf8') > this.maxBytes) {
          this.pending = '';
          this.failed = true;
          return lines;
        }
        this.pending += rest;
        return lines;
      }
      const candidate = this.pending + rest.slice(0, index);
      if (Buffer.byteLength(candidate, 'utf8') > this.maxBytes) {
        this.pending = '';
        this.failed = true;
        return lines;
      }
      // Tolerate CRLF, and treat an empty line as keep-alive padding rather than a message.
      const line = candidate.endsWith('\r') ? candidate.slice(0, -1) : candidate;
      if (line.length > 0) lines.push(line);
      this.pending = '';
      rest = rest.slice(index + 1);
    }
    return lines;
  }
}

/** Clamps text to at most `maxChars` code points without splitting a surrogate pair. */
export function truncateText(text: string, maxChars: number = MAX_CHAT_CHARS): string {
  const points = Array.from(text);
  return points.length <= maxChars ? text : points.slice(0, maxChars).join('');
}
