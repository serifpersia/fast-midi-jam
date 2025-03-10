# Fast MIDI Jam

A low-latency MIDI remote jamming service over UDP, designed for Windows.

## Use Case

Connect and play MIDI instruments together with others in real-time over a local network or internet.

## Downloads

[![Release](https://img.shields.io/github/release/[YOUR_GITHUB_USERNAME]/[YOUR_REPO_NAME].svg?style=flat-square)](https://github.com/serifpersia/fast-midi-jam/releases)

## Build Instructions

1.  **Requirements:**
    *   CMake
    *   MinGW
    *   Boost (added to PATH)

2.  **Clone Repository:**
    ```bash
    git clone [repository_url]
    cd [fast-midi-jam]
    ```

3.  **Run `build.bat`:** Downloads RtMidi, generates build files, and compiles.

## Running

1.  **Configuration:**
    *   **Server:** `build\MidiJamServer.exe` prompts for port/timeout if `server_config.cfg` is missing.
    *   **Client:** `build\MidiJamClient.exe` prompts for settings if `client_config.cfg` is missing.

2.  **Start Server:** `build\MidiJamServer.exe`

3.  **Start Clients:** `build\MidiJamClient.exe`

## Configuration Files

*   **`server_config.cfg`:**
    ```
    port=5000
    inactivity_timeout=30
    ```

*   **`client_config.cfg`:**
    ```
    server_ip=127.0.0.1
    server_port=5000
    nickname=MyMidiClient
    midi_in_port=0
    midi_out_port=1
    midi_channel=3
    second_midi_port_open=false
    second_midi_in_port=2
    ```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
