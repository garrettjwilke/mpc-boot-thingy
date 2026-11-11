## mpc boot thingy

this thingy will allow various MPC software like custom firmware and custom applications to work without breaking eachother.
you can install applications and choose between them easily from the menu.

there are other enhancements this provides, but ehh...they're there... :)

[![Watch the video](https://www.youtube.com/watch?v=0PymkOFAauc)](https://www.youtube.com/watch?v=0PymkOFAauc)

## apps you can try now

**how to install apps:**
> make sure boot thingy is installed. You can install apps in two ways:
>
> **Option 1: SD card drag-and-drop (no web server needed)**
> 1. copy your app `.zip` file(s) directly into `autorun/apps/` on the SD card (using an SD card reader or Boot Thingy's `USB storage (SD card)` mode).
> 2. boot up your MPC (or exit USB storage / reload the menu) — Boot Thingy installs the `.zip` files automatically and removes them once installed.
>
> **Option 2: Web tool (over USB)**
> plug in your MPC Sample to your computer via USB and install your app from the boot thingy web tool:
> - [http://mpc-sample.local:8080](http://mpc-sample.local:8080)
> - [http://192.168.155.1:8080](http://192.168.155.1:8080)

* [AM Mod](https://github.com/AlekseyMalykh/am-mod)
   - step sequencer on pads, chords, typing names, per-pad EQ, Opto compressor, and more.
* [SampQOL-OS](https://github.com/snarebes/SampQOL-OS.git)
   - many enhancements to the OS that improve overall functionality and UX.
* [GRIDZILLA](https://www.thesoundfactorynj.com/gridzilla)
   - modern daw-like sequencing UI/UX...super custom workflow.
* [mpc-minimod](https://github.com/ioma8/mpc-minimod)
   - autotrim samples
   - octave shift by half the grid
   - HPSS - stem split drums and melody
   - spectral chop 
* [am-mod](https://github.com/AlekseyMalykh/am-mod)
   - lots of custom workflows and updated UI
   - chord mode, key scale of sample, step sequencer update, search, resample fixes
*  [KAOSAMPLE public beta](https://github.com/omarhuertasllc-ops/kaosample-beta)
   - stems, looper, auto-kit, scales/chords/arp, sidechain duck + lots more
* [Loop Pad](https://github.com/larsoholm/LoopPad)
   - looper with synchronized tracks
   - turn your MPC Sample into a loop track recorder!
* [beat train](https://github.com/drose030/beat-train-mpc-sample)
   - drum/rhythm game for the mpc sample.
   - import your own music to play!
* [mpc-furnace](https://github.com/garrettjwilke/mpc-furnace)
   - furnace editor for MPC Sample.
   - create music for retro video game consoles.
* [mpc-gen1recomp](https://github.com/garrettjwilke/mpc-gen1recomp)
   - run the original pokemans games on your MPC Sample.
* [mpc-cavestory](https://github.com/garrettjwilke/mpc-cavestory)
   - play the original cavestory game on your MPC Sample
   - making music is not why you bought an MPC Sample, you bought it to play video games!
* [mpc-tic80](https://github.com/garrettjwilke/mpc-tic80)
   - tic80 software in your MPC Sample
   - way too many tic80 games to waste time on

## install

### usb install (recommended)

1. download the [zip here](https://github.com/garrettjwilke/mpc-boot-thingy/releases)
2. plug your mpc sample into your computer.
3. open [mpc-sample.local/testapp](http://mpc-sample.local/testapp) or [192.168.155.1/testapp](http://192.168.155.1/testapp)
4. add the zip and click on `Upload and run`
5. on the MPC, you can then install boot thingy.

> if you have an existing `autoboot`, the installer warns you first: continue to erase it and install boot thingy, or cancel to leave the card unchanged.

### manual SD card install

1. download the [zip here](https://github.com/garrettjwilke/mpc-boot-thingy/releases)
2. extract the zip...inside will be an `autoboot` folder.
3. copy the `autoboot` folder to your sd card.
   > if there is an existing `autoboot` folder, you need to either back it up or delete it first...
4. boot mpc sample with sd card instered

## install apps

### usb install on device method

1. download the app/mod you want..do not extract the zip
2. on the mpc boot thingy screen, press B1 for USB, then select "Install App (USB)"
3. a new usb flash drive will mount to your computer.
4. drag and drop the app zip file you downloaded to this directory
4. click B1 (exit) and boot thingy will install the app

### usb install webserver method (recommended)

1. download the app/mod you want..do not extract the zip
2. plug in your MPC sample and navigate to: [mpc-sample.local/testapp](http://mpc-sample.local/testapp) or [192.168.155.1/testapp](http://192.168.155.1/testapp)
3. click the "install app" button
4. choose your app zip

### manual app install via SD card

1. download the app/mod you want...do no extract the zip
2. mount your SD card to your computer
3. put your app zip in the SD card folder: `autorun/apps/ZIP_FILE_HERE.zip`
4. power on MPC Sample with SD card inserted and it will display that it is installing your app.
