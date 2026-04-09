#Hobbit Sycnhorny v3

Description coming soon

## Skin Sync

The multiplayer layer now supports a GUID-bound skin registry:

- each server-assigned fake Bilbo GUID maps to a fixed Bilbo slot from `FAKE_BILBO_GUID.txt`
- that slot uses a fixed texture filename such as `bilb1[d].xbmp`, `bilb2[d].xbmp`, etc
- synced skins are installed into `common\props`
- the client can upload one skin file, and the server relays it using the canonical slot filename for that GUID 256kb size MAX

To enable it for a client, copy `skin_config.example.txt` to `skin_config.txt` and point `file_path` at your texture file.

##yojimbo

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
