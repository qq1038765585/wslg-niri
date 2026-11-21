# wslg-niri
Let WSLG use the Niri compositor

---
### Requirements

- Windows with WSL support
- WSL version supporting custom distributions

---
### Install

##### 1. Download the latest VHD package. Example:
```
system.zip
```

##### 2. Extract the downloaded archive. After extraction, you should get:
```
system.vhd
```

##### 3. Open **WSL Settings**.
<img width="754" height="409" alt="image" src="https://github.com/user-attachments/assets/2881adb1-4d69-43bb-a654-f9c1b3d6418f" />

##### 4. Go to`Developer -> Custom system distro` then select the extracted `system.vhd` file, example:
<img width="1284" height="791" alt="image" src="https://github.com/user-attachments/assets/b5912fb7-8602-40a0-a947-037b5c1040ff" />

##### 5. Enter the system distribution version using a terminal:
```
wsl --system -u wslg
```
<img width="640" height="201" alt="image" src="https://github.com/user-attachments/assets/cbacec9d-1cee-4c6a-8377-333990c79a10" />

##### 6. Copy the default configuration file:
```
cp ~/.config/niri/config.kdl /mnt/c/Users/[your user name]/niriconfig.kdl
```
<img width="1581" height="213" alt="image" src="https://github.com/user-attachments/assets/41aa4db4-2129-4863-a12a-2abd20275c49" />

## Configuration
For configuration instructions, please refer to (https://niri-wm.github.io/niri/Configuration:-Introduction)

configuration file path:
```
C:\ProgramData\Microsoft\WSL\niriconfig.kdl or C:\Users\[your user name]\niriconfig.kdl
```

```
Please configure the shortcut key to open the terminal as follows:

binds {
    Mod+T hotkey-overlay-title="Open a Terminal: foot" { spawn-sh "/mnt/c/Windows/System32/wsl.exe -- NIRI_SOCKET=$NIRI_SOCKET foot"; }
    ...
}
```

```
Please configure the shortcut keys to switch monitors:

binds {
    Mod+F11 { switch-next-monitor; }
    ...
}
```

```
Similar configurations can prevent WSL for auto shutdown:
spawn-sh-at-startup "/mnt/c/Windows/System32/wsl.exe -- swaybg -c 333333"
```

---
### VHD Permission Issue

If there's this error: WsL/Service/CreateInstance/CreateVm/HCS/E_ACCESSDENIED

1. Locate the VHD file.

2. Open file properties.

3. Add the `Users` group.

4. Grant read and execute permissions.

5. Restart WSL and try again.

example:

<img width="363" height="450" alt="image" src="https://github.com/user-attachments/assets/25c1f8ba-a522-4b76-be57-fa5ec156a317" />

---
## References

The project is based on or inspired by:
- freerdp  
  https://github.com/FreeRDP/FreeRDP
- niri  
  https://github.com/niri-wm/niri

thank you:
- wslg  
  https://github.com/microsoft/wslg
