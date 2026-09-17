// SPDX-License-Identifier: MIT
/**
 * Drives the mock plugin over a real socket with a hand-written client, so the server side of
 * the protocol is exercised end to end rather than only unit-tested.
 */
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { afterEach, describe, expect, it } from 'vitest';

import { LineFramer, decodeLine, encodeLine, type Envelope } from '../src/protocol.js';
import { MockPlugin } from '../src/mock-plugin.js';

let counter = 0;
function uniqueEndpoint(): string {
  counter += 1;
  return path.join(os.tmpdir(), `tsro-mock-${process.pid}-${counter}.sock`);
}

/** A minimal overlay client: connects, handshakes, and records everything it receives. */
class TestClient {
  readonly received: Envelope[] = [];
  private readonly framer = new LineFramer();
  private socket: net.Socket | undefined;
  private seq = 0;

  async connect(endpoint: string): Promise<void> {
    this.socket = net.createConnection(endpoint);
    await new Promise<void>((resolve, reject) => {
      this.socket!.once('connect', resolve);
      this.socket!.once('error', reject);
    });
    this.socket.on('data', (chunk) => {
      for (const line of this.framer.feed(chunk.toString('utf8'))) {
        const decoded = decodeLine(line);
        if (decoded.ok) this.received.push(decoded.envelope);
      }
    });
  }

  send(type: string, data: Record<string, unknown>): void {
    this.socket?.write(
      encodeLine(type as Parameters<typeof encodeLine>[0], this.seq++, Date.now(), data),
    );
  }

  handshake(chat: { channel: boolean; server: boolean; private: boolean }): void {
    this.send('client_hello', { protocol: 1, client: 'test', client_version: '1.0.0' });
    this.send('configuration_updated', { chat, want_speaking_events: true });
  }

  of(type: string): Envelope[] {
    return this.received.filter((envelope) => envelope.type === type);
  }

  close(): void {
    this.socket?.destroy();
  }
}

async function waitFor(predicate: () => boolean, timeoutMs = 3000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
  expect(predicate(), 'condition was not met before the timeout').toBe(true);
}

const cleanup: Array<() => Promise<void> | void> = [];
afterEach(async () => {
  for (const fn of cleanup.reverse()) await fn();
  cleanup.length = 0;
});

async function startPair(chat = { channel: false, server: false, private: false }) {
  const endpoint = uniqueEndpoint();
  const plugin = new MockPlugin({ endpoint, heartbeatMs: 50 });
  await plugin.start();
  cleanup.push(() => plugin.stop());

  const client = new TestClient();
  await client.connect(endpoint);
  cleanup.push(() => client.close());
  client.handshake(chat);

  await waitFor(() => client.of('state_snapshot').length > 0);
  return { plugin, client };
}

describe('mock plugin', () => {
  it('greets with hello and a snapshot', async () => {
    const { client } = await startPair();
    const hello = client.of('hello')[0];
    expect(hello).toBeDefined();
    expect(hello!.seq).toBe(0);
    expect(hello!.data?.plugin_api_version).toBe(26);

    const snapshot = client.of('state_snapshot')[0]!;
    const data = snapshot.data as Record<string, unknown>;
    expect(data.connection).toBe('connected');
    expect((data.users as unknown[]).length).toBe(3);
  });

  it('advertises only capabilities the plugin API provides', async () => {
    const { client } = await startPair();
    const capabilities = client.of('hello')[0]!.data!.capabilities as string[];
    expect(capabilities).toContain('commander');
    expect(capabilities).toContain('whisper_incoming');
    expect(capabilities).toContain('speaker_mute_independent');
    expect(capabilities).not.toContain('whisper_outgoing');
    expect(capabilities).not.toContain('avatar_images');
  });

  it('numbers messages sequentially without gaps', async () => {
    const { plugin, client } = await startPair();
    plugin.setTalking('mock-alice=', true);
    plugin.setTalking('mock-alice=', false);
    await waitFor(() => client.of('speaking_changed').length === 2);

    const sequences = client.received.map((envelope) => envelope.seq);
    for (let i = 1; i < sequences.length; i += 1) {
      expect(sequences[i]).toBe(sequences[i - 1]! + 1);
    }
  });

  it('delivers joins and leaves', async () => {
    const { plugin, client } = await startPair();
    plugin.addUser({ client_id: 9, unique_id: 'mock-carol=', nickname: 'Carol' });
    await waitFor(() => client.of('user_joined').length === 1);

    plugin.removeUser('mock-carol=');
    await waitFor(() => client.of('user_left').length === 1);
  });

  it('keeps microphone and speaker mute independent on the wire', async () => {
    const { plugin, client } = await startPair();
    plugin.setMute('mock-alice=', undefined, true);
    await waitFor(() => client.of('mute_changed').length === 1);

    const data = client.of('mute_changed')[0]!.data as Record<string, unknown>;
    expect(data.output_muted).toBe(true);
    // An omitted field means "unchanged", not "false".
    expect(data).not.toHaveProperty('input_muted');
  });

  it('withholds chat entirely until a category is subscribed', async () => {
    const { plugin, client } = await startPair({ channel: false, server: false, private: false });
    plugin.sendChat('channel', 'Alice', 'channel message');
    plugin.sendChat('private', 'Bob', 'private message');
    plugin.sendChat('server', 'Server', 'server message');
    await new Promise((resolve) => setTimeout(resolve, 200));
    expect(client.of('chat_message')).toHaveLength(0);
  });

  it('delivers only the subscribed category', async () => {
    const { plugin, client } = await startPair({ channel: true, server: false, private: false });
    plugin.sendChat('private', 'Bob', 'must not arrive');
    plugin.sendChat('server', 'Server', 'must not arrive either');
    plugin.sendChat('channel', 'Alice', 'this one should');
    await waitFor(() => client.of('chat_message').length === 1);
    await new Promise((resolve) => setTimeout(resolve, 150));

    const messages = client.of('chat_message');
    expect(messages).toHaveLength(1);
    expect((messages[0]!.data as Record<string, unknown>).text).toBe('this one should');
  });

  it('clamps overlong chat before sending', async () => {
    const { plugin, client } = await startPair({ channel: true, server: false, private: false });
    plugin.sendChat('channel', 'Alice', 'x'.repeat(9000));
    await waitFor(() => client.of('chat_message').length === 1);

    const text = (client.of('chat_message')[0]!.data as Record<string, unknown>).text as string;
    expect(text.length).toBeLessThanOrEqual(1024);
  });

  it('answers a snapshot request', async () => {
    const { client } = await startPair();
    const before = client.of('state_snapshot').length;
    client.send('request_snapshot', { reason: 'manual' });
    await waitFor(() => client.of('state_snapshot').length > before);
  });

  it('echoes a ping nonce so latency can be measured', async () => {
    const { client } = await startPair();
    client.send('ping', { nonce: 4242 });
    await waitFor(() =>
      client.of('heartbeat').some((e) => (e.data as Record<string, unknown>).nonce === 4242),
    );
  });

  it('sends heartbeats unprompted', async () => {
    const { client } = await startPair();
    await waitFor(() => client.of('heartbeat').length >= 2, 2000);
  });

  it('rejects an unsupported protocol version and closes', async () => {
    const endpoint = uniqueEndpoint();
    const plugin = new MockPlugin({ endpoint, heartbeatMs: 1000 });
    await plugin.start();
    cleanup.push(() => plugin.stop());

    const client = new TestClient();
    await client.connect(endpoint);
    cleanup.push(() => client.close());
    client.send('client_hello', { protocol: 99, client: 'from-the-future' });

    await waitFor(() => client.of('error').length === 1);
    const error = client.of('error')[0]!.data as Record<string, unknown>;
    expect(error.code).toBe('unsupported_version');
    expect(error.fatal).toBe(true);
  });

  it('drops a client that sends an oversized frame, and keeps serving others', async () => {
    const endpoint = uniqueEndpoint();
    const plugin = new MockPlugin({ endpoint, heartbeatMs: 50 });
    await plugin.start();
    cleanup.push(() => plugin.stop());

    const good = new TestClient();
    await good.connect(endpoint);
    cleanup.push(() => good.close());
    good.handshake({ channel: false, server: false, private: false });
    await waitFor(() => good.of('state_snapshot').length > 0);

    const bad = net.createConnection(endpoint);
    await new Promise<void>((resolve) => bad.once('connect', () => resolve()));
    bad.write('x'.repeat(70000)); // no newline, ever
    await waitFor(() => plugin.clientCount <= 1, 3000);

    plugin.addUser({ client_id: 12, unique_id: 'mock-dave=', nickname: 'Dave' });
    await waitFor(() => good.of('user_joined').length === 1);
    bad.destroy();
  });

  it('clears the roster when the connection is reported lost', async () => {
    const { plugin, client } = await startPair();
    plugin.setConnection('disconnected', 'connection_lost');
    await waitFor(() => client.of('connection_changed').length === 1);

    const snapshots = client.of('state_snapshot');
    const last = snapshots[snapshots.length - 1]!.data as Record<string, unknown>;
    expect(last.connection).toBe('disconnected');
    expect((last.users as unknown[]).length).toBe(0);
  });

  it('serves several clients independently', async () => {
    const endpoint = uniqueEndpoint();
    const plugin = new MockPlugin({ endpoint, heartbeatMs: 50 });
    await plugin.start();
    cleanup.push(() => plugin.stop());

    const a = new TestClient();
    const b = new TestClient();
    await a.connect(endpoint);
    await b.connect(endpoint);
    cleanup.push(() => { a.close(); b.close(); });

    // Only A subscribes to chat; B must not receive any.
    a.handshake({ channel: true, server: false, private: false });
    b.handshake({ channel: false, server: false, private: false });
    await waitFor(() => a.of('state_snapshot').length > 0 && b.of('state_snapshot').length > 0);

    plugin.sendChat('channel', 'Alice', 'for A only');
    await waitFor(() => a.of('chat_message').length === 1);
    await new Promise((resolve) => setTimeout(resolve, 150));
    expect(b.of('chat_message')).toHaveLength(0);

    // Non-chat events reach both.
    plugin.setCommander('mock-bob=', true);
    await waitFor(() =>
      a.of('commander_changed').length === 1 && b.of('commander_changed').length === 1,
    );
  });
});
