<div align="center">
  
![image](https://github.com/user-attachments/assets/a3b4a0aa-9278-4eff-a0e1-5a684535a567)

</div>

# Fast MIDI Jam

A low-latency MIDI remote jamming service over UDP, designed for Windows(for now).

## Use Case

Connect and play MIDI instruments together with others in real-time over a local network or internet.

## Downloads

[![Release](https://img.shields.io/github/release/serifpersia/fast-midi-jam.svg?style=flat-square)](https://github.com/serifpersia/fast-midi-jam/releases)

## Remote Usage

To connect clients and a server over the internet, the server needs to be accessible from the outside. You can achieve this in two ways:

1.  **Port Forwarding:**
    *   Configure your router to forward the UDP port used by the server (5000) to the internal IP address of the machine running the server.
    *   Clients will then connect to your public IP address (and the forwarded port). You can find your public IP address by searching "what is my ip" on Google.

2.  **Service Tunneling (e.g., playit.gg, zrok(will test and report back):**
    *   Use a service like playit.gg (or similar) to create a tunnel to your server.
    *   Configure the service to tunnel UDP traffic on your chosen port.
    *   The service will provide you with a public IP address and port.
    *   Use the provided IP address and port

## Build Instructions

1.  **Requirements:**
    *   CMake
    *   MinGW
    *   Boost (added to PATH)

2.  **Clone Repository:**
    ```bash
    git clone https://github.com/serifpersia/fast-midi-jam
    cd fast-midi-jam
    ```

3.  **Run `build.bat`:** Downloads RtMidi, generates build files, and compiles.

## Running

1.  **Start Server:** `build\MidiJamServer.exe` 

2.  **Start Clients:** `build\MidiJamClient.exe`
* Both Server and Client exes can be ran in cmd or terminal with ```-debug``` arg for verbose log
## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
