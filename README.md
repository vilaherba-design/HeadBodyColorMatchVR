Head Body Color Match 1.1.0 - Fallout 4 VR port (VR port 4)
============================================================

Original: mrtony - https://www.nexusmods.com/fallout4/mods/110802 (GPL-3.0 with the modding exception in EXCEPTIONS).
This port: Fallout 4 VR 1.2.72 only, F4SE VR + VR Address Library for F4SEVR required. Built on CommonLibF4VR
(github.com/ArthurHub/CommonLibF4VR @ 9057052). The complete source of this port ships with it, under the same licence: F4SE/Plugins/HeadBodyColorMatch_source.zip,
and at https://github.com/vilaherba-design/HeadBodyColorMatchVR (with CommonLibF4VR as a submodule).

What changed from 1.1.0 (search "VR:" in src/main.cpp):
- Addresses: the flat IDs, resolved through version-1-2-72-0.csv (AttachSkinnedObject 0x1CBC00, skin colour 0x5A03A0,
  re-tint 0x669700, main loop 0xD81860, Main::Update 0xD83DE0, Reset3D 0xDDAD60). Every prologue was checked in an
  unpacked Fallout4VR.exe 1.2.72; the plugin still checks them at run time and skips a hook that does not match.
- TESObjectLoadedEvent's event source getter has no VR ID either: 0x42B8D0, from F4SE VR's GameEvents.h (VR port 2;
  port 1 aborted the game at startup on the missing ID 416662).
- F4SEPlugin_Query accepts the runtime F4SE VR reports, 1.10.138, as well as 1.2.72 (VR port 4).
- CommonLibF4VR's NiRTTI ID for BSGeometry is wrong in VR (resolves to 0x3884F18); the real one, 0x5C62A30, is used (VR port 4;
  port 3 loaded and installed its hooks but never found a skin-tint shape). The log folder is created after F4SE init.
- Actor::OnHeadInitialized has no VR ID: it is read from Actor's vtable, slot 0x98 (0xE0A140 in 1.2.72).
- The skin tint material is 0x10 smaller in VR: tintColor is at 0xB0, not 0xC0.
- The reload key (iReloadKey) is polled from the keyboard instead of a MenuControls handler (no VR ID for it).
- Log: Documents\My Games\Fallout4VR\F4SE\HeadBodyColorMatch.log (debug level).

Build (Windows, VS 2022, vcpkg with VCPKG_ROOT set):
  git clone --recursive https://github.com/vilaherba-design/HeadBodyColorMatchVR
  .\build.ps1 -Src <this folder>
CommonLibF4VR (MIT, github.com/ArthurHub/CommonLibF4VR) is the lib/CommonLibF4VR submodule at 9057052; vcpkg ports:
rapidcsv, rsm-mmio, spdlog, xbyak (baseline b4a3d891).
