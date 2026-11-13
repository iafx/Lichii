<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-License-Identifier: GPL-2.0-or-later
-->

<h2 align="center">⚠️ Lichii has no official website ⚠️</h2>

> [!CAUTION]
> **Lichii has no official website.** The only site run by this project is the **game compatibility website, [lichii.zortos.me](https://lichii.zortos.me)**, for compatibility reports only. Download Lichii **only** from this repository's [releases page](https://github.com/zortos293/Lichii/releases). Any other "Lichii" website or download (such as lichii.org) is not ours and may contain malware ([#545](https://github.com/zortos293/Lichii/issues/545)).

<p align="center">
  <img src="dist/branding/lichii_icon_with_logo.png" alt="Lichii" width="600">
</p>

# Lichii

Lichii is an experimental, open-source Nintendo Switch and Switch 2 emulator written in C++. It builds on Eglantine, Eden, yuzu and Sudachi, preserving their contributions while developing this fork's game compatibility and renderer fixes.

This is an independent fork, not an official release of those upstream projects. Lichii is not affiliated with Nintendo.

Visit the [Lichii game compatibility website](https://lichii.zortos.me) to find games and compare community compatibility reports.

## This fork: Lichii + HD Rumble

**English** · [Español](#este-fork-lichii--hd-rumble)

This fork adds HD rumble for the Steam Controller (2026), the Steam Deck, the Steam Controller (2015) and the Switch 2 Pro Controller / Joy-Con 2 to Lichii, plus several fixes. The other controllers use Lichii's own rumble and DualSense haptics. Everything lives on the **`renhdrum`** branch, on top of `nx2`.

### Downloads

GitHub Actions builds both systems; pick the latest green run and download it under **Artifacts** (each artifact comes zipped: extract it first).

- **Windows x64:** [Actions → lichii-renhdrum-windows](../../actions/workflows/renhdrum-windows.yml). Extract the zip inside too and run `lichii.exe`.
- **Linux x86_64** (Steam Deck included): [Actions → lichii-renhdrum-linux](../../actions/workflows/renhdrum-linux.yml). The AppImage needs glibc 2.39 or newer (SteamOS 3.6+, Ubuntu 24.04, Fedora 40, Arch); make it executable (`chmod +x`) and run it. The tar.gz uses your system's Qt 6.

### What vibrates and how

| Controller | What it does |
|---|---|
| Steam Controller (2026) | All four motors: each grip plays both of its side's bands as a real wave (an audio stream, like a Joy-Con's actuator), and the high band also plays on that side's trackpad as a tone at the game's frequency, kept out of the trackpads' resonances. The stream skips a little silence now and then so it never drifts behind the game |
| Steam Deck | One tone per side on the trackpad actuators, at the game's frequency |
| Steam Controller (2015) | One pulse train per side on the trackpad actuators; the pulse rhythm sets the frequency, the way it plays music |
| Switch 2 Pro Controller (USB) | Native HD rumble 2: each side to its own motor, both bands with the game's frequencies (about 87–345 Hz; frequencies outside that range are clamped). Joy-Con 2 over USB too, on Linux and Windows. SDL does not support them over Bluetooth yet; wherever SDL cannot play HD rumble on a connection, Lichii falls back to plain rumble |
| Everything else | Lichii's own: DualSense HD haptics over USB, its Joy-Con / Pro Controller driver, and SDL rumble |

### Settings

**Emulation → Configure → Controls → Vibration → Configure**, **HD Rumble** section (the controllers above):

- **Boost** (100–400 %): makes quiet and medium effects stronger. These send the game's amplitude as is, without the curve the standard rumble applies, so without a boost it can feel weak. 100 % changes nothing; strong effects never go past the controller's maximum.
- **Steam Controller / Steam Deck trackpads** (-20 to +6 dB, default +6): strength of the trackpad haptics; +6 dB is the most the firmware takes before its own limiting.
- **Steam Deck: layer both bands (experimental)**: each Deck trackpad motor plays one tone, so by default it carries the game's stronger band. With this on it plays the high band pulsed at the low band's frequency instead.

The per-player vibration strength (Controls → Vibration) goes up to 400 %: output is still capped at each controller's hardware maximum. Turning a player's vibration off turns its HD rumble off too.

### Changes compared to Lichii

- **Steam Controller (2026):** HD rumble on all four motors, with an SDL patch that lets the controller's haptic commands through.
- **Steam Deck and Steam Controller (2015):** HD rumble as one tone per side on their trackpad actuators, with an SDL patch for the Deck's haptic commands.
- **Switch 2 Pro Controller:** SDL drove it with two fixed frequencies and the same rumble on both motors; an SDL patch now lets Lichii send each motor both bands at the game's frequencies.
- SDL3 is built from source (3.4.14, the same version as Lichii's official builds) with those patches and linked statically, because the prebuilt SDL3 lacks them.
- **Fix, Steam Controller:** the controller stops vibrating after ~50 ms and SDL only resent rumble every 2 s, so any steady vibration cut out almost at once. It is now resent every 40 ms, as in the latest SDL.
- **Fix:** unsafe access to the Steam Controller while it disconnects.
- **Diagnostics:** while a game vibrates, the log shows every 5 s the range of frequencies and amplitudes it sends (`HD Rumble from game ...`).
- **CI:** [`renhdrum-windows.yml`](.github/workflows/renhdrum-windows.yml) and [`renhdrum-linux.yml`](.github/workflows/renhdrum-linux.yml) build Lichii for Windows x64 and Linux x86_64 (AppImage + tar.gz). By default they are PGO builds, made like Lichii's own `-pgo` releases (Windows: clang-cl, ThinLTO and `lichii-windows.profdata`; Linux: clang with `eglantine.profdata`); untick **pgo** under **Run workflow** for the plain MSVC/GCC build.

### Status

On Windows, the Switch 2 controllers go through libusb (`libusb-1.0.dll`, shipped next to `lichii.exe`). Close Steam while using one in Lichii: only one program can hold the controller. If Lichii does not see it, install the WinUSB driver once with [Zadig](https://zadig.akeo.ie/) on the controller's non-HID interface (Options → List All Devices); leave the HID interface's driver alone.

Experimental. Support for Valve's controllers is based on SDL's public protocol and may need tuning, for example if one side vibrates swapped. On the Steam Deck, Lichii must see the Deck's own controller: map the "Steam Deck" device in Lichii's controls rather than Steam's virtual gamepad. If something breaks, open an issue with the controller, whether it is wired or Bluetooth, and the log (**File → Open Lichii Folders → Log Folder**).

### Building

```
git clone --branch renhdrum https://github.com/<your-user>/Lichii.git
cmake -S Lichii -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Credits and licenses

- [Lichii](https://github.com/zortos293/Lichii) by zortos293 and the projects it derives from (Eglantine, Eden, yuzu, Sudachi): GPL.
- [SDL](https://github.com/libsdl-org/SDL): zlib.
- [libusb](https://github.com/libusb/libusb) (Windows builds): LGPL-2.1-or-later.
- Switch 2 rumble frequency encoding from [joycon2-dongle](https://github.com/MukiEX/joycon2-dongle) by MukiEX: GPL-3.0.
- The way the HD paths shape the game's amplitude follows [RenHDrum](https://github.com/trzz1214/RenHDrum)'s DualSense shaping (reimplemented; no RenHDrum code is used).

## Este fork: Lichii + HD Rumble

[English](#this-fork-lichii--hd-rumble) · **Español**

Este fork añade a Lichii vibración HD para el Steam Controller (2026), la Steam Deck, el Steam Controller (2015) y el Switch 2 Pro Controller / Joy-Con 2, más varias correcciones. El resto de mandos usan la vibración y los hápticos del DualSense propios de Lichii. Todo está en la rama **`renhdrum`**, encima de `nx2`.

### Descargas

GitHub Actions genera las builds de los dos sistemas: entra en la última ejecución en verde y descárgala en **Artifacts** (cada artifact viene comprimido: descomprímelo primero).

- **Windows x64:** [Actions → lichii-renhdrum-windows](../../actions/workflows/renhdrum-windows.yml). Descomprime también el zip de dentro y ejecuta `lichii.exe`.
- **Linux x86_64** (incluida la Steam Deck): [Actions → lichii-renhdrum-linux](../../actions/workflows/renhdrum-linux.yml). El AppImage necesita glibc 2.39 o más reciente (SteamOS 3.6+, Ubuntu 24.04, Fedora 40, Arch); hazlo ejecutable (`chmod +x`) y ábrelo. El tar.gz usa el Qt 6 de tu sistema.

### Qué vibra y cómo

| Mando | Qué hace |
|---|---|
| Steam Controller (2026) | Los cuatro motores: cada agarre toca las dos bandas de su lado como una onda de verdad (una señal de audio, como el actuador de un Joy-Con), y la banda aguda suena también en el trackpad de ese lado como un tono a la frecuencia del juego, fuera de las resonancias de los trackpads. La señal se salta un poco de silencio de vez en cuando para no ir quedándose atrás respecto al juego |
| Steam Deck | Un tono por lado en los actuadores de los trackpads, a la frecuencia del juego |
| Steam Controller (2015) | Un tren de pulsos por lado en los actuadores de los trackpads; el ritmo de los pulsos marca la frecuencia, igual que cuando "toca música" |
| Switch 2 Pro Controller (USB) | HD Rumble 2 nativo: cada lado a su motor, las dos bandas con las frecuencias del juego (unos 87–345 Hz; lo que se sale de ese rango queda en el extremo). También Joy-Con 2 por USB, en Linux y en Windows. SDL aún no los soporta por Bluetooth; si en alguna conexión SDL no puede reproducir la vibración HD, Lichii vuelve a la vibración normal |
| Los demás | Lo propio de Lichii: hápticos HD del DualSense por USB, su driver de Joy-Con / Pro Controller y el rumble de SDL |

### Ajustes

En **Emulación → Configurar → Controles → Vibración → Configurar**, sección **HD Rumble** (los mandos de arriba):

- **Boost** (100–400 %): refuerza las vibraciones suaves y medias. Estos mandos reciben la intensidad del juego tal cual, sin la curva que aplica el rumble normal, y sin refuerzo puede notarse floja. Con 100 % no cambia nada; los efectos fuertes nunca pasan del máximo del mando.
- **Steam Controller / Steam Deck trackpads** (-20 a +6 dB, por defecto +6): fuerza de los trackpads; +6 dB es lo máximo que admite el firmware antes de limitarlo él mismo.
- **Steam Deck: layer both bands (experimental)**: cada motor de los trackpads de la Deck toca un único tono, así que por defecto lleva la banda más fuerte del juego. Con esta opción toca la banda aguda pulsada a la frecuencia de la grave.

La intensidad de vibración por jugador (Controles → Vibración) llega al 400 %: la salida sigue limitada al máximo del hardware de cada mando. Si desactivas la vibración de un jugador, también se apaga su vibración HD.

### Cambios respecto a Lichii

- **Steam Controller (2026):** vibración HD en los cuatro motores. Incluye un parche de SDL que deja pasar al mando sus comandos de vibración.
- **Steam Deck y Steam Controller (2015):** vibración HD como un tono por lado en los actuadores de los trackpads, con un parche de SDL para los comandos hápticos de la Deck.
- **Switch 2 Pro Controller:** SDL lo movía con dos frecuencias fijas y la misma vibración en los dos motores; un parche de SDL permite ahora que Lichii mande a cada motor las dos bandas con las frecuencias del juego.
- SDL3 se compila desde el código fuente (3.4.14, la misma versión que las builds oficiales de Lichii) con esos parches y se enlaza en estático, porque la versión precompilada no los tiene.
- **Corrección, Steam Controller:** el mando corta la vibración a los ~50 ms y SDL solo la reenviaba cada 2 s, así que cualquier vibración constante se cortaba casi enseguida. Ahora se reenvía cada 40 ms, como en la versión más reciente de SDL.
- **Corrección:** acceso inseguro al Steam Controller durante la desconexión.
- **Diagnóstico:** mientras el juego vibra, el log muestra cada 5 s el rango de frecuencias e intensidades que envía (`HD Rumble from game ...`).
- **CI:** workflows [`renhdrum-windows.yml`](.github/workflows/renhdrum-windows.yml) y [`renhdrum-linux.yml`](.github/workflows/renhdrum-linux.yml), que compilan Lichii para Windows x64 y Linux x86_64 (AppImage + tar.gz). Por defecto son builds PGO, hechas como las versiones `-pgo` oficiales de Lichii (Windows: clang-cl, ThinLTO y `lichii-windows.profdata`; Linux: clang con `eglantine.profdata`); desmarca **pgo** en **Run workflow** para la build normal con MSVC/GCC.

### Estado

En Windows, los mandos de Switch 2 funcionan a través de libusb (`libusb-1.0.dll`, que va junto a `lichii.exe`). Cierra Steam mientras uses uno en Lichii: solo un programa puede controlar el mando a la vez. Si Lichii no lo ve, instala una vez el driver WinUSB con [Zadig](https://zadig.akeo.ie/) en la interfaz del mando que no es HID (Options → List All Devices); no cambies el driver de la interfaz HID.

Experimental. El soporte de los mandos de Valve se basa en el protocolo público de SDL y puede necesitar ajustes, por ejemplo si un lado vibra cambiado. En la Steam Deck, Lichii tiene que ver el mando propio de la Deck: en los controles de Lichii asigna el dispositivo "Steam Deck", no el mando virtual de Steam. Si algo falla, abre un issue con el mando, si va por cable o Bluetooth, y el log (**Archivo → Abrir carpetas de Lichii → Carpeta de logs**).

### Compilar

```
git clone --branch renhdrum https://github.com/<tu-usuario>/Lichii.git
cmake -S Lichii -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Créditos y licencias

- [Lichii](https://github.com/zortos293/Lichii), de zortos293 y los proyectos de los que deriva (Eglantine, Eden, yuzu, Sudachi): GPL.
- [SDL](https://github.com/libsdl-org/SDL): zlib.
- [libusb](https://github.com/libusb/libusb) (builds de Windows): LGPL-2.1-or-later.
- Codificación de frecuencias del rumble de Switch 2 de [joycon2-dongle](https://github.com/MukiEX/joycon2-dongle), de MukiEX: GPL-3.0.
- La forma en que las vías HD dan forma a la intensidad del juego sigue la del DualSense de [RenHDrum](https://github.com/trzz1214/RenHDrum) (reimplementada; no se usa código de RenHDrum).

## Downloads and setup

Download Lichii only from this repository's [releases page](https://github.com/zortos293/Lichii/releases). Lichii has no other download site: archives offered elsewhere, such as lichii.org, are not built by this project, and one has been reported to install malware ([#545](https://github.com/zortos293/Lichii/issues/545)).

On Windows, extract the entire build archive and run **lichii.exe**. Only use games and system files you have legally obtained. Games, keys and firmware are not included.

### Existing saves and settings

Lichii uses its own profile: `%APPDATA%\lichii` on Windows, and the `lichii` directories under `$XDG_DATA_HOME`, `$XDG_CONFIG_HOME` and `$XDG_CACHE_HOME` on Linux and macOS. Without XDG overrides, these are `~/.local/share/lichii`, `~/.config/lichii` and `~/.cache/lichii`. An existing portable `user/` directory still takes priority and is not imported or changed by the wizard.

On the first non-portable GUI launch, Lichii offers to import Eden and other detected emulator profiles. Back up `%APPDATA%\eden` first on Windows, or the old data, config and cache directories on Unix, and close the source emulator. Choose **Keep Old Data** to copy without changing the source, then check your saves and relaunch. Existing destination data is retained in backup directories rather than merged. Move also retains a source backup until you remove it yourself; Link deliberately shares settings and saves. Paths outside the imported data root remain shared and unchanged. An interrupted import offers recovery on the next launch. `eden-cli`, including `--help` and `--version`, refuses to initialize configuration until you import or explicitly choose **Start fresh** in the GUI.

## Compatibility and issue reports

Compatibility and performance vary by game, hardware and driver. Rendering errors, stutters and crashes remain possible. Passing automated tests does not establish that a game renders correctly, and this project makes no universal frame-rate guarantee.

Browse [community compatibility reports](https://lichii.zortos.me/reports) for results with each player's build and setup.

Desktop reporting requires a build containing [PR #303](https://github.com/zortos293/Lichii/pull/303) and is not available in build 88. In builds that include it, play and stop a Switch 2 game, then right-click that game and choose **Submit compatibility report**. Only the latest session in the current Lichii process is eligible.

Confirm the matching website catalog entry and review the editable machine, game, setup, and enabled add-on details prefilled where available. Check the public preview and give explicit consent before submitting. Nothing uploads automatically, and the native v1 reporter does not read or upload logs or files.

When [reporting an issue](https://github.com/zortos293/Eglantine/issues), include:

- The exact build, game version, CPU, GPU and driver version.
- Relevant settings and steps to reproduce the problem.
- A screenshot and the log from the affected session, after checking it for personal information.

Do not upload game files, keys, firmware or private account information.

## Building and contributing

See the [build guide](docs/Build.md), [user documentation](docs/user/README.md) and [coding guidelines](docs/Coding.md). Some inherited documentation and internal build target names still use upstream names. The GUI build target is `lichii`; its output is `lichii.exe` on Windows and `lichii` on other desktop platforms.

Keep changes focused and distinguish code-level checks from actual gameplay testing.

## Artwork
Art created by YOB (yobslab)
The supplied Lichii logo, icon and library artwork are in [dist/branding](dist/branding). Windows, Linux and macOS icon resources are generated from that artwork by [Generate-LichiiIcons.ps1](tools/Generate-LichiiIcons.ps1).

## Support Lichii

Lichii is free and stays free. If you want to help, you can donate in crypto. Donations help pay for the AI tools and build machines used to develop this open-source project faster.

**Solana (SOL)**
```text
JepEjfyWjGjhJvHSrqj52KtLjcbbbmUh1iGqerYBF4w
```

**Ethereum (ETH)**
```text
0x055ABa1b80Bc35C836Ce7E9F47c2b3d6C75b5534
```

**Bitcoin (BTC)**
```text
bc1qcvxdpf4qvdhqp8ee4s355cjmxrzsphsdwpl8w4
```

**BNB Smart Chain (BNB)**
```text
0x055ABa1b80Bc35C836Ce7E9F47c2b3d6C75b5534
```

**Polygon**
```text
0x055ABa1b80Bc35C836Ce7E9F47c2b3d6C75b5534
```

Send each coin on its matching network. Coins sent on the wrong network can't be recovered.

> [!NOTE]
> Donations are voluntary gifts and are non-refundable. A donation does not buy any product, service, perk, early access, exclusive build, priority support or say in the project. Every release stays free and public for everyone.

## Credits and license

Lichii inherits the work of the Eglantine, Eden, yuzu and Sudachi projects and their contributors, including Eglantine's original developer Camille LaVey. Original copyright notices and third-party licenses are retained.

See [LICENSE.txt](LICENSE.txt) and the source files for applicable license terms.
