# Third-party notices

The original application, panel adapter, build helpers and generated synthetic
feedback tones in this repository are provided under the root MIT license.
Individual third-party files retain their own notices and licenses.

## libfvad / WebRTC voice activity detection

`device/third_party/libfvad` contains the source revision identified by its
`upstream.json`. Its BSD three-clause license is in `LICENSE`; preserve that
file, `AUTHORS`, `PATENTS`, and the original source notices when redistributing.
The patent grant and its conditions are recorded in `PATENTS`.

## Xiaozhi protocol

This client implements the documented Xiaozhi WebSocket protocol. The Xiaozhi
ESP32 project is available at <https://github.com/78/xiaozhi-esp32> under its own
MIT license. This repository is an independent Linux client; it does not
include an ESP32 firmware image or imply upstream endorsement.

## Platform libraries supplied by the user

The application uses public ALSA, Opus and json-c interfaces and standard Linux
and C runtime interfaces. This repository does not distribute the device's
runtime libraries, Linux kernel, bootloader, device tree, root filesystem or
toolchain. When obtaining or redistributing those components, retain their
individual licenses and satisfy their corresponding source obligations.

The small opaque ABI declarations are interface adapters, not copies of a
vendor audio implementation. Their relevant upstream API references are
recorded in the source comments.

## Xiaomi components are not distributed

Local wake-word detection and the optional original microphone processing
adapter load libraries and models already present in the user's original,
read-only system filesystem. Those proprietary libraries, model data and
factory recordings are not included and are not covered by this project's
MIT license. Hardware ownership is not a license to redistribute these assets.

The three generated cue headers are original integer-waveform synthesis,
reproducible with `python3 scripts/generate_prompts.py --check`. They contain no
factory sound recordings, personal speech, TTS cache or voice clone. The image
builder also generates original boot and wake tones. If a user privately
replaces these with recordings from their own device, those recordings must
remain local and must not be added to the repository.
