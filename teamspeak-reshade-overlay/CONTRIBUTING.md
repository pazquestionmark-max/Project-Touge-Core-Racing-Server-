# Contributing

## Ground rules

These exist because breaking them produces bugs that are invisible in review and painful in a
game:

1. **Never block the render thread or a TeamSpeak callback.** Both run in someone else's process.
   Queue work; do not wait on it.
2. **Never invent an API capability.** If TeamSpeak or ReShade does not expose something, say so
   in `docs/protocol.md` §6 or `docs/compatibility.md` and implement the closest honest
   alternative. A guessed value rendered confidently is worse than a hidden indicator.
3. **Absent is not false.** A state we cannot determine stays `std::nullopt` and is omitted from
   the wire, so the renderer hides the indicator.
4. **No third-party dependency in `shared/`.** It is loaded into a game and a voice client.
5. **Chat is private by default.** Any change touching chat must preserve: categories filtered in
   the plugin, private messages off unless explicitly enabled, content never logged by default.
6. **Validate every input.** Anything from a pipe or a configuration file is untrusted: bound its
   size, check its type, clamp its range.

## Building and testing

```bash
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
scripts/check-windows-sources.sh
cd tools/tsro-cli && npm install && npm test
```

Everything except the final Windows link runs on Linux. Use it — the MinGW cross-check has
already caught real bugs (a non-copyable type passed by value, a missing include, and the ImGui
docking-branch requirement) that would otherwise have surfaced only on a Windows machine.

## Where code belongs

| If it is... | It goes in |
|---|---|
| platform-independent logic | `shared/` — and it must have tests |
| TeamSpeak-specific | `teamspeak-plugin/` |
| drawing or the settings UI | `reshade-integration/` |
| a TeamSpeak SDK constant | `teamspeak-plugin/src/ts_query_ts3.cpp` only |

Keeping SDK constants in one file is what lets the rest of the plugin build and be tested without
the SDK. Do not spread them.

## Tests

* Logic in `shared/` needs a test in `tests/`. It runs on every push.
* Protocol changes need updating in **both** implementations (`shared/src/protocol.cpp` and
  `tools/tsro-cli/src/protocol.ts`); the parity tests will catch it if you forget one.
* Schema changes need the same treatment for `config.cpp` and `config-schema.ts`.
* Rendering cannot be unit-tested, but layout can: `compute_layout` is pure, and that is where
  resolution, anchor and overflow behaviour is verified.

Add tests that would fail if the behaviour regressed, not tests that restate the implementation.

## Style

`.clang-format` for C++ (Google base, 4 spaces, 100 columns); TypeScript follows the tsconfig's
strict settings. Comments should explain why a decision was made, especially where the obvious
approach was rejected. Do not comment what the code already says.

## Protocol and configuration changes

* Additive changes (a new optional field, message type or capability) do **not** bump the version:
  receivers ignore what they do not know.
* Bump `kProtocolVersion` only when a change would make an older receiver misinterpret an
  existing field.
* Bump `kConfigVersion` and add a `migrate_vN_to_vN+1` step for any change that would make an
  existing file load incorrectly. Never make an old configuration fail to load.
