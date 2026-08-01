# The Hobbit Synchorny v3

The Hobbit Synchrony is a mod that brings Multiplayer to The Hobbit 2003 Game. Play the game with your friends with up to 8 players or even more! 

Download here: https://github.com/hobbit-kingdom/hobbit-synchrony-v3/releases

## Trailer & Video Showcase
[<img width="1920" height="1080" alt="Multiplayer Trailer" src="https://github.com/user-attachments/assets/5711c620-69f8-4c8d-97f8-c2781f9a3ec7" />](https://youtu.be/O65o7NI17v0)

[<img width="50%" alt="Gameplay showcase" src="https://github.com/user-attachments/assets/d02c8aa2-772a-41de-bc9f-2c95044876f5" />](https://www.youtube.com/watch?v=4Yf1T2Yr1dc)


## Installation

Go to the [releases](https://github.com/hobbit-kingdom/hobbit-synchrony-v3/releases) section and download the .zip. Then follow the README.TXT inside the .zip archive to setup & run the multiplayer.

## Join our Discord
[<img width="2048" height="512" alt="banner 3" src="https://github.com/user-attachments/assets/e346271c-95f9-4f28-982a-3b98206df26f" />](https://discord.gg/uuMsVFX397)

# For Developers

## Skin Sync

The multiplayer layer now supports a GUID-bound skin registry:

- each server-assigned fake Bilbo GUID maps to a fixed Bilbo slot from `FAKE_BILBO_GUID.txt`
- that slot uses a fixed texture filename such as `bilb1[d].xbmp`, `bilb2[d].xbmp`, etc
- synced skins are installed into `common\props`
- the client can upload one skin file, and the server relays it using the canonical slot filename for that GUID 256kb size MAX

To enable it for a client, copy `synchrony_config.example.txt` to `synchrony_config.txt` and point
`file_path` at your texture file.

## Client settings — `synchrony_config.txt`

One key/value file holds everything the client needs: server address, logging, skin and player
profile. It is searched for in the current folder, the game exe folder and the DLL folder, in that
order. The old name `skin_config.txt` is still accepted in the same places, so an existing install
keeps working, but a `synchrony_config.txt` anywhere wins over it.

```txt
server_ip=203.0.113.25
debug=0
enabled=true
file_path=common\props\my_custom_skin.xbmp
```

`server_ip` replaces `config.txt`. **Both the client and `server.exe` read it** — the server takes
`bind_ip` and `server_ip` (or `public_ip`) from the same file, so a host keeps one file instead of two.
Setting only `server_ip` also sets the bind address, matching what a single-line `config.txt` used to do.

Resolution order for the address: `synchrony_config.txt` (current folder, then next to the exe/dll),
then `skin_config.txt` in the same places, then `config.txt`. The first file that provides a key wins,
and the two keys resolve independently. Both programs print which file they ended up using.

`name`, `status` and `damage` also live here; the `/name`, `/status` and `/damage` chat commands write
them back, preserving every other key in the file.

## Console logging

By default the console stays quiet - it prints the startup lines, the host prompt and any connection
error, and nothing else.

```txt
debug=1
```

turns the development output back on: every NPC that gets resolved, every stone thrown, every chest,
trigger, pickup and switch replicated, the nickname/status/chat packet traces, and yojimbo's own
per-packet logging (which is otherwise limited to errors).

The file is searched for in the current folder, the game exe folder and the DLL folder, in that order.

## yojimbo

Network library used for this project

https://github.com/mas-bandwidth/yojimbo

## Secure Connect Tokens

The multiplayer transport now uses Yojimbo's secure `Client::Connect` flow instead of `InsecureConnect`.

- The server owns the private key in `server_private_key.txt`. If the file does not exist, the server generates it on first launch.
- Clients request a short-lived connect token from the server over `TCP 40001`, then join the game over `UDP 40000`.
- Do not ship `server_private_key.txt` with the client build.

`config.txt` still supports the old single-line format for local testing:

```txt
127.0.0.1
```

For a dedicated host, you can now use key/value settings:

```txt
bind_ip=0.0.0.0
public_ip=203.0.113.25
```

`bind_ip` is what the server listens on locally. `public_ip` is the address written into issued connect tokens and should be the address clients actually use.

## License

[BSD 3-Clause license](https://opensource.org/licenses/BSD-3-Clause).
