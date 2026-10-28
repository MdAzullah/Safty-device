# CLAUDE.md

@AGENTS.md

## Claude Code specifics

- **Environment:** Windows 11. Use the PowerShell tool for ESP-IDF (`idf.py` needs the EIM activation script, see AGENTS.md). Bash (Git Bash) is fine for file inspection and Node. `python` is not on PATH; use the ESP-IDF venv Python at `C:\Espressif\tools\python\v6.0.1\venv\Scripts\python.exe` (it includes `pyserial`).
- **Serial port:** the board enumerates as `Silicon Labs CP210x USB to UART Bridge (COM4)`. Check it exists before flashing (`Get-CimInstance Win32_PnPEntity | Where-Object Name -match '\(COM\d+\)'`). "Access is denied" means another program (usually the VS Code ESP-IDF monitor) holds the port; ask the user to close it rather than retrying.
- **Reading device logs:** opening the port with DTR/RTS toggled resets the ESP32. To observe without resetting, open with `dtr=False, rts=False` set before `open()`.
- **Flashing** is reversible but changes the user's hardware state; confirm the board is connected and that the user expects it. A partition-table change requires `idf.py flash` (full), not app-only flashing.
- **Firebase checks:** prefer `node tools/check-device.mjs`. Reads require a guardian account; never store or echo the user's password, Wi-Fi credentials, or phone numbers in files, commits, or summaries.
- **Website verification:** serve `website/` locally and use `?demo` for UI checks; headless Edge (`C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe --headless=new --screenshot`) works for screenshots. Desktop Edge has a ~500 px minimum width; test phone layouts inside a 390 px iframe.
- Put scratch scripts and screenshots in the session scratchpad, not the repo.
- Do not commit unless asked. The working tree may contain large uncommitted changes (see `docs/CHANGELOG.md`).
