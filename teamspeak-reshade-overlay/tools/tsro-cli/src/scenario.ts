// SPDX-License-Identifier: MIT
/** Scripted scenarios for the mock plugin, so overlay behaviour can be reproduced on demand. */
import type { MockPlugin } from './mock-plugin.js';

export interface Step {
  afterMs: number;
  description: string;
  run: (plugin: MockPlugin) => void;
}

export interface Scenario {
  name: string;
  description: string;
  steps: Step[];
  loop: boolean;
}

export const SCENARIOS: Record<string, Scenario> = {
  idle: {
    name: 'idle',
    description: 'A static channel with three people and no events.',
    steps: [],
    loop: false,
  },

  chatter: {
    name: 'chatter',
    description: 'People talking over each other, with mute toggles. Exercises the speaking '
      + 'envelope, list reordering and indicator transitions.',
    steps: [
      { afterMs: 600, description: 'Alice starts talking',
        run: (p) => p.setTalking('mock-alice=', true) },
      { afterMs: 1400, description: 'Bob starts talking too',
        run: (p) => p.setTalking('mock-bob=', true) },
      { afterMs: 900, description: 'Alice stops',
        run: (p) => p.setTalking('mock-alice=', false) },
      { afterMs: 700, description: 'Bob mutes his microphone',
        run: (p) => { p.setTalking('mock-bob=', false); p.setMute('mock-bob=', true, undefined); } },
      { afterMs: 1200, description: 'Bob unmutes and mutes his speakers instead',
        run: (p) => p.setMute('mock-bob=', false, true) },
      { afterMs: 1500, description: 'Bob unmutes his speakers',
        run: (p) => p.setMute('mock-bob=', false, false) },
    ],
    loop: true,
  },

  churn: {
    name: 'churn',
    description: 'People joining and leaving, including a very long nickname. Exercises join and '
      + 'leave notifications, overflow handling and the list cap.',
    steps: [
      { afterMs: 900, description: 'Carol joins',
        run: (p) => p.addUser({ client_id: 10, unique_id: 'mock-carol=', nickname: 'Carol',
          input_muted: false, output_muted: false }) },
      { afterMs: 800, description: 'A user with a very long name joins',
        run: (p) => p.addUser({ client_id: 11, unique_id: 'mock-long=',
          nickname: 'AnExtremelyLongTeamSpeakNicknameForTestingOverflowBehaviour',
          input_muted: false, output_muted: false }) },
      { afterMs: 1200, description: 'Carol is made Channel Commander',
        run: (p) => p.setCommander('mock-carol=', true) },
      { afterMs: 1500, description: 'The long-named user leaves',
        run: (p) => p.removeUser('mock-long=') },
      { afterMs: 1200, description: 'Carol leaves',
        run: (p) => p.removeUser('mock-carol=') },
    ],
    loop: true,
  },

  channels: {
    name: 'channels',
    description: 'Channel switching and connection loss. Exercises the channel title updating '
      + 'immediately, and that a lost connection clears the user list rather than leaving it '
      + 'stale on screen.',
    steps: [
      { afterMs: 2000, description: 'Switch to the Lobby',
        run: (p) => p.switchChannel('Lobby', 43) },
      { afterMs: 2500, description: 'Switch back to Racing #1',
        run: (p) => p.switchChannel('Racing #1', 42) },
      { afterMs: 2500, description: 'Connection lost',
        run: (p) => p.setConnection('disconnected', 'connection_lost') },
      { afterMs: 2500, description: 'Reconnecting',
        run: (p) => p.setConnection('connecting') },
      { afterMs: 1500, description: 'Reconnected',
        run: (p) => p.setConnection('connected') },
    ],
    loop: true,
  },

  chat: {
    name: 'chat',
    description: 'Messages in every category. Private messages are sent but only reach a client '
      + 'that explicitly subscribed, which is the privacy behaviour in action.',
    steps: [
      { afterMs: 1500, description: 'Channel message',
        run: (p) => p.sendChat('channel', 'Alice', 'Lights out in 30 seconds', 'mock-alice=') },
      { afterMs: 2000, description: 'Server message',
        run: (p) => p.sendChat('server', 'Server', 'Scheduled restart at 04:00') },
      { afterMs: 2000, description: 'Private message (delivered only if subscribed)',
        run: (p) => p.sendChat('private', 'Bob', 'you there?', 'mock-bob=') },
      { afterMs: 2000, description: 'A very long channel message',
        run: (p) => p.sendChat('channel', 'Alice', 'x'.repeat(600), 'mock-alice=') },
    ],
    loop: true,
  },

  stress: {
    name: 'stress',
    description: 'A high event rate: rapid speaking changes across many users. Used to check the '
      + 'overlay stays responsive and that queue overflow forces a resynchronisation rather than '
      + 'unbounded growth.',
    steps: Array.from({ length: 40 }, (_, index) => ({
      afterMs: 40,
      description: `rapid speaking change ${index + 1}`,
      run: (p: MockPlugin) => {
        const uid = index % 2 === 0 ? 'mock-alice=' : 'mock-bob=';
        p.setTalking(uid, index % 4 < 2);
      },
    })),
    loop: true,
  },
};

/** Runs a scenario, returning a function that stops it. */
export function runScenario(
  plugin: MockPlugin,
  scenario: Scenario,
  onStep?: (step: Step) => void,
): () => void {
  let stopped = false;
  let timer: NodeJS.Timeout | undefined;

  const step = (index: number): void => {
    if (stopped) return;
    if (index >= scenario.steps.length) {
      if (!scenario.loop || scenario.steps.length === 0) return;
      timer = setTimeout(() => step(0), 1500);
      return;
    }
    const current = scenario.steps[index]!;
    timer = setTimeout(() => {
      if (stopped) return;
      current.run(plugin);
      onStep?.(current);
      step(index + 1);
    }, current.afterMs);
  };

  step(0);
  return () => {
    stopped = true;
    if (timer) clearTimeout(timer);
  };
}
