## mpc boot thingy

this thingy will allow various MPC software like custom firmware and custom applications to work without breaking eachother.
you can install applications and choose between them easily from the menu.

there are other enhancements this provides, but ehh...they're there...there :)

[![mpc sample boot thingy video](https://youtu.be/eMhiaCJpgGg)](https://youtu.be/eMhiaCJpgGg)

## USB Installer

You can install or update mpc-boot-thingy directly over USB without requiring an SD card reader or an existing SSH setup:

1. Connect your MPC to your computer with a USB-C cable and turn it on.
2. Run the USB Installer from your terminal:
   ```bash
   ./setup.sh --usb
   ```
3. Look at the MPC screen:
   - A dialog titled **USB Installer** will ask: **"Install boot thingy?"** (or **"Update boot thingy?"**).
   - If an existing autoboot setup is present on your SD card, it is automatically migrated into `autorun/apps/` so nothing is lost!
   - Press **Yes** (top right button / PLAY / Data Wheel Click) to install and reboot into mpc-boot-thingy.
   - Press **No** (top left button / STOP) to cancel without making changes.

## Manual / SD Card Setup

If you prefer installing directly to a mounted SD card via an SD reader:
```bash
./setup.sh --SD "/Volumes/MPC"
```

To add an app directly to the card:
```bash
./setup.sh --SD "/Volumes/MPC" --add /path/to/app --title "App Name"
```

## web page

Edit the page in `webserver/source`. `./setup.sh` builds that into `webserver/content` and copies the directory onto the card. The generated content is not committed.

```
cd webserver/source
npm install
npm run dev
```

`npm run dev` serves the page with sample boot entries so the layout can be checked without an MPC. `npm run dev:live` uses a server from `make web-host` on port 8080 instead.
