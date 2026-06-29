# Hobbit Sycnhorny v3

The Hobbit Synchrony is a mod that brings Multiplayer to The Hobbit 2003 Game. Play the game with your friends with up to 8 players or even more! The gameplay is still work in progress and subject to change. The mod is still in the testing and in the works - new features are being worked on.

Download here: https://github.com/hobbit-kingdom/hobbit-synchrony-v3/releases

## Video Showcase
[<img width="50%" alt="Gameplay showcase" src="https://github.com/user-attachments/assets/d02c8aa2-772a-41de-bc9f-2c95044876f5" />](https://www.youtube.com/watch?v=4Yf1T2Yr1dc)


## Installation

Go to the [releases](https://github.com/hobbit-kingdom/hobbit-synchrony-v3/releases) section and download the .zip. Copy the contents of the .zip to the root of your Game folder. Then follow the README.TXT provided with the mod to run the multiplayer.

## Join our Discord
[<img width="2048" height="512" alt="banner 3" src="https://github.com/user-attachments/assets/e346271c-95f9-4f28-982a-3b98206df26f" />](https://discord.gg/uuMsVFX397)

# For Developers

## Skin Sync

The multiplayer layer now supports a GUID-bound skin registry:

- each server-assigned fake Bilbo GUID maps to a fixed Bilbo slot from `FAKE_BILBO_GUID.txt`
- that slot uses a fixed texture filename such as `bilb1[d].xbmp`, `bilb2[d].xbmp`, etc
- synced skins are installed into `common\props`
- the client can upload one skin file, and the server relays it using the canonical slot filename for that GUID 256kb size MAX

To enable it for a client, copy `skin_config.example.txt` to `skin_config.txt` and point `file_path` at your texture file.

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
