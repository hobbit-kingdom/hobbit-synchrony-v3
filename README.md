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

## License

[BSD 3-Clause license](https://opensource.org/licenses/BSD-3-Clause).
