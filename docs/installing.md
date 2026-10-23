# Installing

```{index} installing, download, packages
```

Every release on
[GitHub](https://github.com/pvanek/slonisko/releases) has packages for
macOS (Apple silicon and Intel), Windows and Linux (Flatpak), and a
`SHA256SUMS` file listing their checksums.

The macOS and Windows packages are not signed with a certificate from Apple
or Microsoft. Both cost money every year, and slonisko is free. The first
time you run it, the system warns you that it does not know who made the
program. The steps below get past that warning once; after that the program
starts like any other.

If you want to be sure a package is the one the release built, compare its
checksum with the one in `SHA256SUMS`:

```sh
shasum -a 256 Slonisko-*.dmg                    # macOS
sha256sum Slonisko-*.flatpak                    # Linux
```

```powershell
Get-FileHash Slonisko-*-setup.exe               # Windows, PowerShell
```

## macOS

```{index} macOS, Gatekeeper, quarantine, xattr
```

Download the disk image for your processor: `macos-arm64` for Apple
silicon, `macos-x86_64` for Intel. Open it and drag *Slonisko* to
*Applications*.

The browser marks every downloaded file as quarantined, and macOS will not
open a quarantined program that Apple has not notarised. On the first
attempt it says that it could not verify Slonisko is free of malware, or,
on some versions, that Slonisko is damaged and should be moved to the
trash. Nothing is damaged. Remove the quarantine mark in Terminal:

```sh
xattr -d com.apple.quarantine /Applications/Slonisko.app
```

and open Slonisko as usual. Use the path you actually copied it to, if that
is somewhere other than *Applications*.

If you would rather not use Terminal, try to open Slonisko once and close
the warning. Then go to *System Settings → Privacy & Security*, scroll to
*Security*, and click *Open Anyway* next to the message about Slonisko.
macOS asks for your password and opens the program. Since macOS 15, this is
the only way without Terminal: Control-clicking the program and choosing
*Open* no longer skips the check.

## Windows

```{index} Windows, SmartScreen, installer
```

There are two packages:

- `Slonisko-<version>-setup.exe` installs slonisko for you alone, without
  administrator rights, or for everyone on the computer.
- `Slonisko-<version>-win64.zip` is the same program in a folder, to unpack
  anywhere and run from there.

When you start the installer, Microsoft Defender SmartScreen may say that
Windows protected your PC. Click *More info*, then *Run anyway*.

Windows also marks files unpacked from a downloaded zip, so the first start
of `slonisko.exe` from the zip can show the same warning. To avoid it,
right-click the zip before you unpack it, choose *Properties*, tick
*Unblock* and click *OK*. In PowerShell this does the same:

```powershell
Unblock-File Slonisko-*-win64.zip
```

## Linux

```{index} Linux, Flatpak
```

The Flatpak bundle needs the KDE runtime from Flathub. If Flathub is not
set up yet, add it, then install the bundle for your user:

```sh
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user Slonisko-<version>.flatpak
flatpak run cz.yarpen.slonisko
```

To build slonisko from source instead, see {doc}`development/building`.
