<div align="center">
  <img src="https://github.com/user-attachments/assets/b6934858-21ce-4cbe-a3e8-6a7338df5d27" alt="Fast MIDI Jam Logo">
</div>

# Fast MIDI Jam

A low-latency MIDI remote jamming service over UDP.

## Use Case

Connect and play MIDI instruments together with others in real-time over a local network or the internet.

## Downloads

[![Release](https://img.shields.io/github/release/serifpersia/fast-midi-jam.svg?style=flat-square)](https://github.com/serifpersia/fast-midi-jam/releases)

## Remote Usage

To connect clients and a server over the internet, the server must be externally accessible. You can achieve this in two ways:

### 1. Port Forwarding
- Configure your router to forward UDP port `5000` to the internal IP address of the machine running the server.
- Clients connect using your public IP address and the forwarded port. Find your public IP by searching "what is my ip" on Google.

### 2. Service Tunneling (e.g., playit.gg, zrok)
- Use a tunneling service like playit.gg zrok or pinggy(all free).
- Configure the service to tunnel UDP traffic on your chosen port (e.g., `5000`).
- The service provides a public IP address and port for clients to use.

## Build Instructions

### Windows
1. **Requirements:**
   - CMake
   - MinGW
   - Boost (added to PATH)

2. **Clone Repository:**
   ```bash
   git clone https://github.com/serifpersia/fast-midi-jam
   cd fast-midi-jam```
3. **Build:**
   ```bash
    build.bat
  

### Linux
1. **Requirements:**
```bash
cmake g++ unzip libasound2-dev libjack-dev libboost-dev
```
2. **Clone Repository and Build:**
```bash
git clone https://github.com/serifpersia/fast-midi-jam
cd fast-midi-jam
sudo chmod +x build.sh
sudo ./build.sh both (for amd64)
sudo ./build.sh both arm64 (for arm64)
* Building only server or client is also possible with sudo ./build.sh server/client amd64/arm64
```
### Running

1. **Start Server:**
```bash
build/MidiJamServer.exe
sudo chmod +x MidiJamServer
./build/MidiJamServer
```
2. **Start Client:**
```bash
build/MidiJamClient.exe
sudo chmod +x MidiJamClient
./build/MidiJamClient
```

*Linux users need to install libasound2-dev or libjack-dev packages to be able to run the client
Debug Mode: Add the ```-debug``` flag for verbose logging:

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
