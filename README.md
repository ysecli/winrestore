# winrestore

An x64dbg plugin that remembers where the debugged program's window was and puts it back there when you restart debugging, on the same monitor, at the same size.

> [!NOTE]
> Project is heavily vibe coded, but nonetheless works. I will occasionally go through and edit some things but for the most part, it's working fine enough.

## Usage

1. Build `conset.slnx` (x64). The build copies `winrestore.dp64` into x64dbg's `plugins` folder.
2. Restart x64dbg.
3. Start debugging, then move and resize the console where you want it. It opens there on every restart.

Everything else is under **Plugins → winrestore**:

- **Profiles...**: save fixed window positions, either for one program or as the global default.
- **Restore this process to default (this session)**: stop moving this program's window until x64dbg closes.
- **Enabled**: turn the plugin on or off.
