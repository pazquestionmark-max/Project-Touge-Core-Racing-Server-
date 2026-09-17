// SPDX-License-Identifier: MIT
import { describe, expect, it } from 'vitest';

import {
  LineFramer,
  MAX_CHAT_CHARS,
  MAX_MESSAGE_BYTES,
  PROTOCOL_VERSION,
  decodeLine,
  encodeLine,
  isServerToClient,
  truncateText,
} from '../src/protocol.js';

describe('envelope', () => {
  it('round trips', () => {
    const line = encodeLine('user_joined', 7, 1737072000123, { user: { unique_id: 'a=' } }, 'srv=');
    expect(line.endsWith('\n')).toBe(true);

    const decoded = decodeLine(line.trimEnd());
    expect(decoded.ok).toBe(true);
    if (!decoded.ok) return;
    expect(decoded.envelope.v).toBe(PROTOCOL_VERSION);
    expect(decoded.envelope.seq).toBe(7);
    expect(decoded.envelope.ts).toBe(1737072000123);
    expect(decoded.envelope.type).toBe('user_joined');
    expect(decoded.envelope.server).toBe('srv=');
  });

  it('omits absent optional envelope fields', () => {
    const line = encodeLine('heartbeat', 0, 1);
    expect(line).not.toContain('"server"');
    expect(line).not.toContain('"data"');
  });

  it('rejects malformed input as fatal for the stream', () => {
    for (const input of ['not json', '[1,2,3]', '"a string"', '42']) {
      const decoded = decodeLine(input);
      expect(decoded.ok).toBe(false);
      if (decoded.ok) return;
      expect(decoded.fatal).toBe(true);
    }
  });

  it('treats an unknown type as droppable, not fatal', () => {
    const decoded = decodeLine(JSON.stringify({ v: 1, seq: 1, ts: 0, type: 'from_the_future' }));
    expect(decoded.ok).toBe(false);
    if (decoded.ok) return;
    expect(decoded.reason).toBe('unknown_type');
    expect(decoded.fatal).toBe(false);
  });

  it('treats an unsupported version as droppable, not fatal', () => {
    const decoded = decodeLine(JSON.stringify({ v: 99, seq: 1, ts: 0, type: 'hello' }));
    expect(decoded.ok).toBe(false);
    if (decoded.ok) return;
    expect(decoded.reason).toBe('bad_version');
    expect(decoded.fatal).toBe(false);
  });

  it('ignores unknown fields', () => {
    const decoded = decodeLine(
      JSON.stringify({ v: 1, seq: 1, ts: 0, type: 'heartbeat', tomorrow: 123 }),
    );
    expect(decoded.ok).toBe(true);
  });

  it('rejects an oversized message', () => {
    const decoded = decodeLine('x'.repeat(MAX_MESSAGE_BYTES + 1));
    expect(decoded.ok).toBe(false);
    if (decoded.ok) return;
    expect(decoded.reason).toBe('too_large');
    expect(decoded.fatal).toBe(true);
  });

  it('knows the direction of every message type', () => {
    expect(isServerToClient('state_snapshot')).toBe(true);
    expect(isServerToClient('client_hello')).toBe(false);
    expect(isServerToClient('ping')).toBe(false);
  });
});

describe('framing', () => {
  it('splits complete lines', () => {
    const framer = new LineFramer();
    expect(framer.feed('a\nb\nc\n')).toEqual(['a', 'b', 'c']);
  });

  it('reassembles across chunks', () => {
    const framer = new LineFramer();
    expect(framer.feed('par')).toEqual([]);
    expect(framer.feed('tial mes')).toEqual([]);
    expect(framer.feed('sage\n')).toEqual(['partial message']);
  });

  it('tolerates CRLF and drops empty lines', () => {
    const framer = new LineFramer();
    expect(framer.feed('a\r\n\n\nb\r\n')).toEqual(['a', 'b']);
  });

  it('overflows while accumulating rather than after', () => {
    const framer = new LineFramer(64);
    framer.feed('x'.repeat(40));
    expect(framer.overflowed).toBe(false);
    framer.feed('x'.repeat(40));
    expect(framer.overflowed).toBe(true);
    expect(framer.pendingBytes).toBe(0);
  });

  it('keeps lines completed before an overflow', () => {
    const framer = new LineFramer(32);
    expect(framer.feed(`good\n${'x'.repeat(64)}`)).toEqual(['good']);
    expect(framer.overflowed).toBe(true);
  });

  it('refuses further input until reset', () => {
    const framer = new LineFramer(16);
    framer.feed('x'.repeat(32));
    expect(framer.feed('short\n')).toEqual([]);
    framer.reset();
    expect(framer.feed('short\n')).toEqual(['short']);
  });
});

describe('text clamping', () => {
  it('clamps to the protocol limit', () => {
    expect(truncateText('x'.repeat(5000)).length).toBe(MAX_CHAT_CHARS);
  });

  it('does not split surrogate pairs', () => {
    const text = '😀'.repeat(10);
    const clamped = truncateText(text, 3);
    expect(Array.from(clamped)).toHaveLength(3);
    expect(clamped).toBe('😀😀😀');
  });

  it('leaves short text untouched', () => {
    expect(truncateText('hello', 100)).toBe('hello');
  });
});
