Configuration files shown here are examples, not a working device identity.

Run `scripts/prepare_config.py` to create a new private directory. It asks for
Wi-Fi and optional upstream authentication interactively, without putting them
in shell history. Use the speaker's own Wi-Fi MAC. The local bridge receives a
new random token, separate from any token required by your Xiaozhi server.

Use the complete WebSocket URL provided by the selected server. Static query
parameters are allowed and remain private. Public servers require `wss://`;
plain `ws://` is restricted to an explicit private numeric IP. `wss://` also
requires a public CA bundle through `--ca-file`; TLS checks stay enabled.

All generated configuration files and firmware images contain private data.
Keep them outside the source tree and never publish them as release assets.
This repository distributes neither real credentials nor vendor firmware.
