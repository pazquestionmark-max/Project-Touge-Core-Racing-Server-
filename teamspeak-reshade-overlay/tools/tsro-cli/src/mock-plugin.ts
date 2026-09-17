// SPDX-License-Identifier: MIT
/**
 * A protocol server that speaks the plugin's side of the wire without TeamSpeak.
 *
 * Two purposes, both genuine:
 *   1. The ReShade add-on can be exercised in a real game with no TeamSpeak client running,
 *      which is how rendering, layout and animation get tested at speed.
 *   2. It is an independent implementation, so the C++ server is validated against something
 *      other than itself.
 *
 * This is a development and test tool. It is not part of the shipped overlay, and the overlay
 * has no code path that fabricates TeamSpeak data.
 */
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import fs from 'node:fs';

import {
  HEARTBEAT_INTERVAL_MS,
  LineFramer,
  PROTOCOL_MAX,
  PROTOCOL_MIN,
  type ChatCategory,
  type WireChannel,
  type WireServer,
  type WireSnapshot,
  type WireUser,
  decodeLine,
  encodeLine,
  isServerToClient,
  truncateText,
} from './protocol.js';

export interface ChatSubscription {
  channel: boolean;
  server: boolean;
  private: boolean;
}

interface Session {
  socket: net.Socket;
  framer: LineFramer;
  seq: number;
  subscription: ChatSubscription;
  wantSpeaking: boolean;
  handshaken: boolean;
  id: number;
}

export interface MockPluginOptions {
  endpoint?: string;
  heartbeatMs?: number;
  pluginVersion?: string;
  capabilities?: string[];
  onLog?: (message: string) => void;
}

/** The default endpoint, matching the C++ default_endpoint on each platform. */
export function defaultEndpoint(override?: string): string {
  if (override && override.length > 0) {
    if (process.platform === 'win32' && !override.startsWith('\\\\')) {
      return `\\\\.\\pipe\\${override}`;
    }
    return override;
  }
  if (process.platform === 'win32') return '\\\\.\\pipe\\tsro.v1';
  const base = process.env.XDG_RUNTIME_DIR ?? process.env.TMPDIR ?? os.tmpdir();
  return path.join(base, 'tsro.v1.sock');
}

export class MockPlugin {
  private server: net.Server | undefined;
  private readonly sessions = new Set<Session>();
  private heartbeatTimer: NodeJS.Timeout | undefined;
  private readonly startedAt = Date.now();
  private nextSessionId = 1;
  private nextChatId = 1;

  private serverState: WireServer = {
    handler_id: 1,
    unique_id: 'mock-server-uid=',
    name: 'Mock TeamSpeak',
  };
  private channel: WireChannel = {
    id: 42,
    name: 'Racing #1',
    parent_id: 7,
    parent_name: 'Games',
    path: 'Games/Racing #1',
    topic: '',
  };
  private users: WireUser[] = [];
  private selfUniqueId = 'mock-me=';
  private connection: 'disconnected' | 'connecting' | 'connected' = 'connected';

  constructor(private readonly options: MockPluginOptions = {}) {
    this.users = [
      { client_id: 1, unique_id: 'mock-me=', nickname: 'You', is_self: true,
        input_muted: false, output_muted: false, input_hardware: true, output_hardware: true },
      { client_id: 2, unique_id: 'mock-alice=', nickname: 'Alice',
        input_muted: false, output_muted: false, channel_commander: true, talk_power: 75 },
      { client_id: 3, unique_id: 'mock-bob=', nickname: 'Bob',
        input_muted: true, output_muted: false, talk_power: 50 },
    ];
  }

  private log(message: string): void {
    this.options.onLog?.(message);
  }

  get endpoint(): string {
    return defaultEndpoint(this.options.endpoint);
  }

  get clientCount(): number {
    return this.sessions.size;
  }

  async start(): Promise<void> {
    const endpoint = this.endpoint;
    if (process.platform !== 'win32' && fs.existsSync(endpoint)) {
      // A stale socket file from a crashed run must not block startup.
      fs.unlinkSync(endpoint);
    }

    this.server = net.createServer((socket) => this.accept(socket));
    await new Promise<void>((resolve, reject) => {
      this.server!.once('error', reject);
      this.server!.listen(endpoint, () => {
        this.server!.removeListener('error', reject);
        resolve();
      });
    });
    if (process.platform !== 'win32') {
      // Owner-only, matching the Windows pipe DACL.
      fs.chmodSync(endpoint, 0o600);
    }

    this.heartbeatTimer = setInterval(
      () => this.heartbeat(),
      this.options.heartbeatMs ?? HEARTBEAT_INTERVAL_MS,
    );
    this.log(`listening on ${endpoint}`);
  }

  async stop(): Promise<void> {
    if (this.heartbeatTimer) clearInterval(this.heartbeatTimer);
    this.heartbeatTimer = undefined;
    for (const session of [...this.sessions]) session.socket.destroy();
    this.sessions.clear();
    if (this.server) {
      await new Promise<void>((resolve) => this.server!.close(() => resolve()));
      this.server = undefined;
    }
  }

  private accept(socket: net.Socket): void {
    socket.setNoDelay(true);
    const session: Session = {
      socket,
      framer: new LineFramer(),
      seq: 0,
      subscription: { channel: false, server: false, private: false },
      wantSpeaking: true,
      handshaken: false,
      id: this.nextSessionId++,
    };
    this.sessions.add(session);
    this.log(`client ${session.id} connected`);

    // Greet, then send a full snapshot, so a client is usable from its first frame.
    this.send(session, 'hello', {
      protocol_min: PROTOCOL_MIN,
      protocol_max: PROTOCOL_MAX,
      plugin_version: this.options.pluginVersion ?? '1.0.0-mock',
      ts_client_version: 'mock',
      plugin_api_version: 26,
      capabilities: this.options.capabilities ?? [
        'chat', 'whisper_incoming', 'commander', 'priority_speaker', 'recording', 'away',
        'talk_power', 'locally_muted', 'speaker_mute_independent', 'hardware_state', 'country',
      ],
    });
    this.sendSnapshot(session);

    socket.on('data', (chunk) => this.onData(session, chunk));
    socket.on('error', () => this.drop(session));
    socket.on('close', () => this.drop(session));
  }

  private drop(session: Session): void {
    if (!this.sessions.delete(session)) return;
    session.socket.destroy();
    this.log(`client ${session.id} disconnected`);
  }

  private onData(session: Session, chunk: Buffer): void {
    const lines = session.framer.feed(chunk.toString('utf8'));
    if (session.framer.overflowed) {
      this.log(`client ${session.id} sent an oversized frame; closing`);
      this.drop(session);
      return;
    }
    for (const line of lines) {
      const decoded = decodeLine(line);
      if (!decoded.ok) {
        if (decoded.fatal) {
          this.log(`client ${session.id}: ${decoded.message}; closing`);
          this.drop(session);
          return;
        }
        continue;
      }
      const { envelope } = decoded;
      if (isServerToClient(envelope.type)) continue;
      this.handle(session, envelope.type, envelope.data ?? {});
    }
  }

  private handle(session: Session, type: string, data: Record<string, unknown>): void {
    switch (type) {
      case 'client_hello': {
        const protocol = typeof data.protocol === 'number' ? data.protocol : 0;
        if (protocol < PROTOCOL_MIN || protocol > PROTOCOL_MAX) {
          this.send(session, 'error', {
            code: 'unsupported_version',
            message: `client requested v${protocol}; this plugin supports ${PROTOCOL_MIN}..${PROTOCOL_MAX}`,
            fatal: true,
          });
          setTimeout(() => this.drop(session), 50);
          return;
        }
        session.handshaken = true;
        this.log(`client ${session.id} handshake complete (${String(data.client ?? 'unknown')})`);
        break;
      }
      case 'configuration_updated': {
        // Absent means off: a client must opt in explicitly, so a partial configuration can
        // never switch private-message forwarding on.
        const chat = (data.chat ?? {}) as Record<string, unknown>;
        session.subscription = {
          channel: chat.channel === true,
          server: chat.server === true,
          private: chat.private === true,
        };
        session.wantSpeaking = data.want_speaking_events !== false;
        break;
      }
      case 'request_snapshot':
        this.sendSnapshot(session);
        break;
      case 'ping':
        this.send(session, 'heartbeat', {
          uptime_ms: Date.now() - this.startedAt,
          connected_clients: this.sessions.size,
          nonce: typeof data.nonce === 'number' ? data.nonce : 0,
        });
        break;
      default:
        break;
    }
  }

  private send(session: Session, type: string, data?: Record<string, unknown>): void {
    if (session.socket.destroyed) return;
    const line = encodeLine(
      type as Parameters<typeof encodeLine>[0],
      session.seq++,
      Date.now(),
      data,
      this.serverState.unique_id,
    );
    session.socket.write(line);
  }

  private broadcast(type: string, data?: Record<string, unknown>): void {
    for (const session of [...this.sessions]) {
      if (type === 'speaking_changed' && !session.wantSpeaking) continue;
      this.send(session, type, data);
    }
  }

  snapshot(): WireSnapshot {
    return {
      connection: this.connection,
      server: this.serverState,
      channel: this.channel,
      self_unique_id: this.selfUniqueId,
      users: this.users,
    };
  }

  private sendSnapshot(session: Session): void {
    this.send(session, 'state_snapshot', this.snapshot() as unknown as Record<string, unknown>);
  }

  broadcastSnapshot(): void {
    this.broadcast('state_snapshot', this.snapshot() as unknown as Record<string, unknown>);
  }

  // --- scripted scenario actions -------------------------------------------------------------

  addUser(user: WireUser): void {
    this.users = [...this.users.filter((u) => u.unique_id !== user.unique_id), user];
    this.broadcast('user_joined', { user, cause: 'moved' });
  }

  removeUser(uniqueId: string): void {
    const user = this.users.find((u) => u.unique_id === uniqueId);
    if (!user) return;
    this.users = this.users.filter((u) => u.unique_id !== uniqueId);
    this.broadcast('user_left', { user, cause: 'moved' });
  }

  setTalking(uniqueId: string, talking: boolean, whisper = false): void {
    const user = this.users.find((u) => u.unique_id === uniqueId);
    if (!user) return;
    user.talking = talking;
    user.whispering_to_me = talking && whisper;
    this.broadcast('speaking_changed', {
      client_id: user.client_id,
      unique_id: uniqueId,
      talking,
      ...(whisper ? { whisper: true } : {}),
    });
  }

  setMute(uniqueId: string, input: boolean | undefined, output: boolean | undefined): void {
    const user = this.users.find((u) => u.unique_id === uniqueId);
    if (!user) return;
    if (input !== undefined) user.input_muted = input;
    if (output !== undefined) user.output_muted = output;
    this.broadcast('mute_changed', {
      client_id: user.client_id,
      unique_id: uniqueId,
      ...(input !== undefined ? { input_muted: input } : {}),
      ...(output !== undefined ? { output_muted: output } : {}),
    });
  }

  setCommander(uniqueId: string, commander: boolean): void {
    const user = this.users.find((u) => u.unique_id === uniqueId);
    if (!user) return;
    user.channel_commander = commander;
    this.broadcast('commander_changed', {
      client_id: user.client_id,
      unique_id: uniqueId,
      channel_commander: commander,
    });
  }

  switchChannel(name: string, id: number): void {
    const previous = { ...this.channel };
    this.channel = { ...this.channel, id, name, path: `${this.channel.parent_name}/${name}` };
    this.broadcast('channel_changed', {
      from: previous,
      to: this.channel,
      user_count: this.users.length,
    });
    this.broadcastSnapshot();
  }

  setConnection(state: 'disconnected' | 'connecting' | 'connected', reason = 'user'): void {
    this.connection = state;
    if (state !== 'connected') this.users = [];
    this.broadcast('connection_changed', { connection: state, reason });
    this.broadcastSnapshot();
  }

  /** Chat is delivered only to sessions that subscribed to the category. */
  sendChat(category: ChatCategory, senderName: string, text: string, senderUid = ''): void {
    const message = {
      id: this.nextChatId++,
      category,
      sender_unique_id: senderUid,
      sender_name: senderName,
      channel_name: this.channel.name,
      text: truncateText(text),
      timestamp_ms: Date.now(),
    };
    for (const session of [...this.sessions]) {
      if (!session.subscription[category === 'private' ? 'private' : category]) continue;
      this.send(session, 'chat_message', message);
    }
  }

  private heartbeat(): void {
    this.broadcast('heartbeat', {
      uptime_ms: Date.now() - this.startedAt,
      connected_clients: this.sessions.size,
    });
  }
}
