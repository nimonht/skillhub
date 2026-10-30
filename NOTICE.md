# Third-party notices

Skill Hub itself is MIT licensed ([LICENSE](LICENSE)). It includes the following third-party material.

| What | From | Licence |
|---|---|---|
| Fonts (Barlow, Barlow Condensed, Barlow Semi Condensed) | The Barlow Project Authors | SIL Open Font License 1.1, [fonts/OFL.txt](fonts/OFL.txt) |
| Agent catalog (`data/providers.json`): which agents exist and where each reads skills | [vercel-labs/skills](https://github.com/vercel-labs/skills), Copyright (c) 2026 Vercel, Inc. | MIT, [data/LICENSE-vercel-labs-skills](data/LICENSE-vercel-labs-skills) |
| Brand marks (`data/provider-icons.json`), mostly | [lobehub/lobe-icons](https://github.com/lobehub/lobe-icons), Copyright (c) 2023 LobeHub | MIT, [data/LICENSE-lobe-icons](data/LICENSE-lobe-icons) |
| Brand marks (`data/provider-icons.json`): Zed, Warp, Replit, OpenCode | [simple-icons](https://github.com/simple-icons/simple-icons) | CC0 1.0, [data/LICENSE-simple-icons](data/LICENSE-simple-icons) |
| JSON reader and writer (`third_party/yyjson`), compiled in | [ibireme/yyjson](https://github.com/ibireme/yyjson), Copyright (c) 2020 YaoYuan | MIT, [third_party/yyjson/LICENSE](third_party/yyjson/LICENSE) |
| Terminal emulator of the window (`third_party/libvterm`, 0.3.3), compiled in | [libvterm](https://www.leonerd.org.uk/code/libvterm/), Copyright (c) 2008 Paul Evans | MIT, [third_party/libvterm/LICENSE](third_party/libvterm/LICENSE) |
| Image reader of `skillhub fetch` (`third_party/stb/stb_image.h`, 2.30), compiled in | [nothings/stb](https://github.com/nothings/stb), Sean Barrett | MIT or public domain, at the end of the file |
| Event loop of `skillhub serve` (libuv 1.51.0): the system library on Linux, compiled in on macOS and Windows | [libuv/libuv](https://github.com/libuv/libuv), Copyright (c) 2015-present libuv project contributors | MIT, with notices for some of its files, [third_party/libuv/LICENSE](third_party/libuv/LICENSE) and [third_party/libuv/LICENSE-extra](third_party/libuv/LICENSE-extra) |

## Trademarks

The marks in `data/provider-icons.json` are logos and trademarks of their respective owners (Anthropic, OpenAI, Google,
Cursor, GitHub, Zed Industries, Warp, Replit and others). They are shown only so that each coding agent can be recognised in
the list of agents installed on your machine. Skill Hub is not affiliated with, endorsed by or sponsored by any of them.
The licences above cover the SVG files as redistributed by those collections; they do not grant any right to the marks.
If you are an owner and want a mark removed or changed, open an issue; deleting its entry from `data/provider-icons.json`
is all it takes (the agent then shows a two-letter tile).

The visual language of the interface is inspired by the MIT-licensed
[`endfield-design`](https://github.com/Statrue/endfield-design-skill) skill.
