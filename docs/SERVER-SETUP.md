# Server-side setup — everything the gateway watch needs

This is the complete "Hermes side" of the project: the platform plugin, the
config that routes watch turns to the right agent profile, the toolset binding
that gives those turns hands, and the optional environment keys. Everything here
lives server-side (the machine running Hermes); the watch itself only needs the
firmware from [BUILD.md](BUILD.md).

## 1. Install the plugin

`gateway/` in this repo **is** the server-side component (a Hermes "gadget"
platform plugin, MIT). Copy it into your Hermes `plugins/` directory:

```bash
cp -r gateway ~/.hermes/plugins/gadget
```

The hub listens on `ws://0.0.0.0:8765/gadget` on your LAN. Devices store that
host at pairing time — give the machine a fixed LAN IP or DNS name.

## 2. Platform config (config.yaml)

```yaml
platforms:
  gadget:
    enabled: true
    host: 0.0.0.0
    port: 8765
    path: /gadget
    # Optional: send a device's turns to the profile that should own them.
    # Key = device id (as enrolled), value = Hermes profile name.
    device_profiles:
      <your-device-id>: <profile>
    # Optional: the watch's agent ring commits an agent name per turn; map each
    # name to a Hermes profile. Unknown names fall through → device_profiles → owner.
    agent_profiles:
      venus: venus
      oracle: oracle
```

Then give the routed profile's brain **tools** — a platform whose toolset id is
not registered binds ZERO tools (the turn answers in prose and never acts):

```yaml
platform_toolsets:
  gadget: [web, search, browser, file, terminal, vision, memory, todo, skills,
           tts, session_search, code_execution]
```

(The gadget platform is a Hermes Agent plugin platform; check `hermes config`
for the current mechanism on your version — see the Hermes docs.)

## 3. Pairing

```text
hermes gadget list          # device shows up as pending after first contact
hermes pairing approve gadget <code-from-device-screen>
```

Full flow: [GATEWAY.md](GATEWAY.md).

## 4. Agent prerequisites (STT / TTS)

Voice turns are STT → agent → TTS on the host. Local whisper already installed?
Hermes picks it up; otherwise set `GROQ_API_KEY` etc. Config lives in the
ROUTED profile's `config.yaml` (per-profile `stt:`/`tts:` blocks apply per turn).

## 5. Optional env keys (.env)

| Key | Purpose |
|-----|---------|
| `GOOGLE_HOME_CMD` | Custom Google Home helper command (see below) |
| `HASS_URL` | Home Assistant URL if you bridge voice → HA |
| STT/TTS provider keys | whichever providers you enable (`GROQ_API_KEY`, …) |

## 6. Google Home (the "turn off the coffee machine" demo)

The watch's channel prompt resolves its Google Home command at import time:

1. `GOOGLE_HOME_CMD` if set — anything your agent can run via the `terminal`
   tool; it gets the user's spoken command appended as one quoted argument.
2. Otherwise it auto-resolves `$HERMES_HOME/skills/smarthome/google-home-control/scripts/assist.py`
   (any profile's skills dir) against `$HERMES_HOME/tools/google-home-venv/bin/python`.

The helper script + OAuth flow (`reauth.py`) come from the
`google-home-control` skill — see that skill's own docs for device OAuth. Once
resolved, voice turns like "turn off the coffee machine" execute immediately
and the watch speaks Google's confirmation back.

## 7. What the server sends (and the watch now shows)

Prompt/approval frames (`prompt{id,title,text,ttl_s}` → the watch's APPROVAL
card with **A YES / B NO / hold-B later**), status ticker frames (a tool-progress
line under the ring), notices (with `level` for error styling), and turn
lifecycle frames are all documented frame-by-frame in [PROTOCOL.md](PROTOCOL.md).