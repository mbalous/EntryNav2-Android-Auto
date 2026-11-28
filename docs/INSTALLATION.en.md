# Installation

English · [Français](INSTALLATION.md)

> [!NOTE]
> This is the English translation of [INSTALLATION.md](INSTALLATION.md). The commands are the same in both (a test
> checks it); if the explanations ever differ, the French page is the reference. The menu entries are given as iDrive
> shows them in English.

Installation is done by hand, over SSH, in a few commands. Nothing is replaced: two folders are added to the head
unit's system, which is writable only while they are copied. Every step was rehearsed with the car's BusyBox.

> [!CAUTION]
> Car parked, ignition on, for the whole procedure. Do not switch the ignition off while the system is writable
> (step 3). When in doubt, restarting the head unit puts the system back to read-only.

## You need

- A compatible **EntryNav2** head unit ([compatibility](COMPATIBILITE.md), in French).
- A PC with an SSH client: Windows 10 or 11 (built-in OpenSSH client), Linux or macOS.
- An **ENET cable** between the car's OBD socket and the PC. The PC's network adapter stays on automatic addressing.
- The head unit's **root password**. It is not provided with this project.
- The archive `entrynav2-android-auto-<version>-armhf.tar` of a
  [published version](https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases), whose SHA-256 matches the
  `.sha256` file published next to it, or one made by `tools/package_release.py`
  ([building](../README.md#building-from-source)). Up to 0.12.0-rc.6, the archive and its folder were called
  `entry-aad-<version>`: for those versions, replace `entrynav2-android-auto` with `entry-aad` in the commands.

The version number is written only twice: in the `scp` command of step 1 and on the `V=` line of step 2. Put the one
of the archive you downloaded. `169.254.1.0` is the reference car's address on the ENET cable: replace it with your
head unit's if it differs.

This page describes the current version. Each archive holds its own `INSTALLATION.md`: for an older version, follow
the one in its archive (in 0.10.0, the supervisor is called `evo-trial`, not `aa-supervisor`).

## 1. Copy the archive to the car

From the PC, in the archive's folder:

```sh
scp entrynav2-android-auto-0.13.0-rc.12-armhf.tar root@169.254.1.0:/tmp/
ssh root@169.254.1.0
```

All the following commands are typed in that SSH session, on the car. `/tmp` is in memory: nothing is written to the
flash memory yet.

## 2. Check the car

Nothing is changed during this step.

```sh
V=0.13.0-rc.12
cd /tmp && rm -rf entrynav2-android-auto-$V && tar -xf entrynav2-android-auto-$V-armhf.tar && cd entrynav2-android-auto-$V
md5sum -c MD5SUMS
head -n 1 /etc/version
df -k /
```

The `V=` line holds for the whole SSH session, step 3 included: after reconnecting, type it again.

Every file must answer `OK`, and at least 16 MB must be free (`Available` column, in KB). Then the head unit's loader
checks that every library and every function used exists, without running anything, and `aa-supervisor --compat`
rehearses the start of Android Auto:

```sh
L=/usr/local/lib:/opt/hmi/lib:/usr/lib:/lib
for f in opt/aa/entry-aad opt/aa/aa-supervisor opt/hmi/data/ro/Plugins/EntryAad/libEntryAad.so; do
    LD_TRACE_LOADED_OBJECTS=1 LD_WARN=yes LD_BIND_NOW=yes LD_LIBRARY_PATH=$L /lib/ld-linux-armhf.so.3 ./$f 2>&1 |
        grep -E 'not found|undefined symbol|not defined in file'
done
LD_LIBRARY_PATH=$L XDG_RUNTIME_DIR=/run/system/0 WAYLAND_DISPLAY=wayland-0 HOME=/tmp \
    busybox timeout -t 60 ./opt/aa/aa-supervisor --compat
```

- The loop must print **nothing**: each line shows a function or a library this car does not have.
- `aa-supervisor --compat` answers one line per point: `@ok|…` passed, `@warn|…` doubtful, `@block|…` **blocking**. A
  single `@block` line: do not install. What each point means is in [compatibility](COMPATIBILITE.md) (in French).
- Exception: a `@block` on `alps`, `uuid` or `spp` (detail `wireless only`) only prevents wireless. Installing is still
  possible in [USB](USB.md) mode (in French), to choose before the first *Start Android Auto*.

That result, with the first line of `/etc/version`, makes a good
[compatibility report](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/new/choose) for a car not listed yet.

## 3. Install

If an earlier version is running, stop it first from its iDrive menu (*Stop Android Auto*). Also note whether
`/opt/hmi/data/ro/Plugins` already exists (`ls -d /opt/hmi/data/ro/Plugins`): if not, the installation creates it, and
only the uninstallation of a head unit where it did not exist before may remove it.

```sh
mount -o remount,rw /
mkdir -p /opt/hmi/data/ro/Plugins/EntryAad /opt/aa
rm -f /opt/hmi/data/ro/Plugins/EntryAad/* /opt/aa/*
cp opt/hmi/data/ro/Plugins/EntryAad/* /opt/hmi/data/ro/Plugins/EntryAad/
cp opt/aa/* /opt/aa/
chmod 755 /opt/aa/entry-aad /opt/aa/aa-supervisor /opt/hmi/data/ro/Plugins/EntryAad/libEntryAad.so
touch /opt/aa/.installed-by-entryaad /opt/hmi/data/ro/Plugins/EntryAad/.installed-by-entryaad
sync
mount -o remount,ro /
cd / && md5sum -c /tmp/entrynav2-android-auto-$V/MD5SUMS
```

- `rm -f` before copying: iDrive keeps the old app in memory until it restarts, and a file deleted then copied again
  is never rewritten under its feet.
- The `.installed-by-entryaad` file marks each folder as added by this project.
- The last `md5sum -c` reads the installed files back: each must answer `OK`.
- If `mount -o remount,ro /` fails, the restart that follows puts the system back to read-only.

Optional, the car told to the phone: the model and year it shows, and the fuel. Without this file, it shows the series
coded in the car (for instance "G20" or "F56", "iDrive" if the coding does not say), 2019 and petrol: the head unit
does not know the model's name. The brand, BMW or MINI, comes from iDrive's coding and is not set here. The driver's
side, for which Android Auto lays out its screen, also comes from the car's coding (see below to force it).

Replace `MODELE` and `ANNEE` with the car's model and year (for instance `320d` and `2020`, or `Cooper S` and `2019`);
24 characters at most for the model: unaccented letters, digits, spaces, `-`, `.` and `_`. Replace `CARBURANT` with
`petrol` or `diesel`; any other value keeps petrol.

```sh
mkdir -p /var/local/data/AA && printf 'model=MODELE\nyear=ANNEE\nfuel=CARBURANT\n' > /var/local/data/AA/vehicle
```

If the service log says `driver side not read` (coding unreadable) or reports the wrong side, a `side=left` (left-hand
drive) or `side=right` (right-hand drive) line added to this file wins over the coding.

Then restart the head unit:

```sh
rm -rf /tmp/entrynav2-android-auto-$V /tmp/entrynav2-android-auto-$V-armhf.tar
reboot
```

## 4. First start

On iDrive: *Media*, then *Android Auto*, then *Start Android Auto*. The first time, pair the phone with the car over
Bluetooth if it is not paired yet, and accept the car on the phone: Android Auto then joins the car's Wi-Fi by
itself. The menu's *Start automatically* box starts Android Auto with iDrive.

Android Auto can also use a USB cable: tick *USB cable instead of wireless* in the same menu, see [USB](USB.md) (in
French).

## Updating

### From a USB stick, without a PC

An installation whose Android Auto menu has the *Update from USB stick* row (from 0.13.0-rc.10) updates from the
archive of a published version, without a PC or SSH. Car parked, ignition on.

1. Download the archive `entrynav2-android-auto-<version>-armhf.tar` of a
   [published version](https://github.com/Jiraiya43/EntryNav2-Android-Auto/releases) and copy it **as is**, without
   extracting it, to the root of a USB stick (FAT32, exFAT or NTFS). The name may change a little ("(1)" added by the
   browser), but must start with `entrynav2-android-auto-` and end with `.tar`. With several archives on the stick,
   the most recent valid one is taken.
2. Plug the stick in, then *Media*, *Android Auto*, and *Update from USB stick* in the menu. The row shows *Reading
   the USB stick...* and checks everything without installing anything (only its report is written, see below): the
   archive's signature, each file's checksum, that each program is made for this head unit (ARM, the car's loader,
   whole file), and, through the head unit's loader as in step 2, that every library and every function of the new
   programs exists on this car.
3. The row then says what a second press installs: *Install the update*, *Install the older version* (older than the
   installed one) or *Reinstall this version* (the car's files are no longer those of this version). Press again
   within 30 seconds: exactly the checked archive is installed, even if another one is added to the stick meanwhile.
   Never during a call or in reverse: the restart would cut the call or the camera.
4. Android Auto stops if it is running. The row shows *Installing: keep ignition on* for a few seconds, then
   *Installed: iDrive restarting*: the head unit restarts by itself and comes back with the new version. The settings
   in `/var/local/data/AA` are kept. If the installation is refused without having changed anything, Android Auto,
   stopped for it, starts again.

| The row says | Meaning |
|---|---|
| *Already up to date* | The car has exactly the archive's files. |
| *No USB stick found*, *No update on the USB stick* | No stick, or no `entrynav2-android-auto-…-armhf.tar` archive at its root. |
| *Update not valid* | Damaged archive, older than the USB-stick update itself, or not signed by the project (see below). |
| *Update not for this car* | A new program is not made for this head unit, or the head unit's loader cannot find a library or a function it needs: nothing is installed. |
| *Update failed: nothing changed* | Refused before the first file was replaced: not enough space, system refusing to write, checked archive removed from the stick before the second press… |
| *Not during a call or in reverse* | The restart would cut the call or the camera: try again afterwards. |
| *Finish the update* | An update was cut while the files were being swapped and can be finished: press again, the stick is not needed. |
| *Incomplete: install it again* | Only part of it may be in place (power cut, interrupted installation): update again with the stick. Android Auto stays stopped until then. |
| *Busy: try again shortly* | Android Auto did not stop within 45 s, or another update is running. |
| *Installation not recognised* | The folders lack the `.installed-by-entryaad` file of step 3: nothing is touched. |
| *Installed: restart the iDrive* | Installed, but the head unit did not restart by itself within a minute: hold iDrive's on/off button for about 30 seconds. |

Each run writes what it found and did to `EntryAA-update.txt`, at the root of the stick when it accepts writing, and
to `/var/local/data/AA/update-report.txt`, which *Copy logs to USB stick* also copies: attach it to an issue.

What protects the car:

- Only an archive signed by the project is installed: the archive's `UPDATE` list gives the SHA-256 of each installed
  file, and `UPDATE.sig` signs it (RSA, 3072 bits). A home-made archive (`tools/package_release.py --unsigned`)
  installs over SSH only.
- Nothing is written before the checks are over, done again on the archive read back at the second press. The new
  files are first written next to the old ones, read back from the flash memory and checked; then each replaces the
  old one at once, by a rename: each file is always the old or the new one, never a mix of both.
- Just before the replacements, the signed list is kept on the data partition. If the power is cut during the
  replacements, some files are new and others still old: the next update finishes the replacements, with or without
  the stick. Cut before, the old version stays whole.
- The system becomes writable when the files start being written. It can only go back to read-only once the replaced
  files iDrive keeps open (its app) are released: it therefore stays writable until the restart that follows, a few
  seconds later, which puts it back to read-only.
- Only `/opt/aa` and `/opt/hmi/data/ro/Plugins/EntryAad` are touched. The files the new version no longer has are
  removed there, as step 3 does.

### Over SSH

Do steps 1 to 3 again with the new archive, after stopping Android Auto from its menu. The settings in
`/var/local/data/AA` are kept. Step 3 empties `/opt/aa` before copying: a program renamed between two versions
(`evo-trial`, which became `aa-supervisor` after 0.10.0) does not stay on the head unit. This is the way to go once,
to move from a version without the *Update from USB stick* row to a version that has it.

If a USB-stick update was cut before, its journal is no longer useful: remove it during step 3,
`rm -f /var/local/data/AA/update-pending`.

## Uninstalling

Stop Android Auto from its menu, then, over SSH:

```sh
mount -o remount,rw /
rm -rf /opt/hmi/data/ro/Plugins/EntryAad /opt/aa
sync
mount -o remount,ro /
rm -rf /var/local/data/AA
reboot
```

If `/opt/hmi/data/ro/Plugins` did not exist before the installation and is empty, `rmdir /opt/hmi/data/ro/Plugins`
removes it too (before `sync`). Keeping `/var/local/data/AA` keeps the settings for a later installation.

## Troubleshooting

| Symptom | What to do |
|---|---|
| No Android Auto row in *Media* | Check step 3 (`md5sum -c`), then restart the head unit: iDrive reads its apps only at start. |
| iDrive restarts several times right after loading it | The app no longer starts at the third try within 15 minutes and iDrive comes back without it; it starts again by itself at an iDrive start 15 minutes later. To enable it at once after a fix: `rm -f /var/local/data/AA/app.disable /var/local/data/AA/app-guard`, then restart. |
| Wireless: the phone, paired with the car, never starts Android Auto | Pair the phone again while Android Auto is running (*Start Android Auto* first). A phone paired while the service was not running keeps a list of the car's services without Android Auto ([#72](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/72)). |
| Android Auto does not start | Tick *Keep logs* in the menu ([maintenance](MAINTENANCE.md), in French), reproduce, press *Copy logs to USB stick*, then open an [issue](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/new/choose) with the logs. |
| The system stayed writable | `mount -o remount,ro /`, or restart the head unit. |
| iDrive's screen no longer responds | Hold iDrive's on/off button for about 30 seconds to restart the head unit. |
