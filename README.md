## mpc boot thingy

this thingy will allow various MPC software like custom firmware and custom applications to work without breaking eachother.
you can install applications and choose between them easily from the menu.

there are other enhancements this provides, but ehh...they're there... :)

[![Watch the video](https://www.youtube.com/watch?v=0PymkOFAauc)](https://www.youtube.com/watch?v=0PymkOFAauc)

## apps you can try now

**how to install apps:**
> make sure boot thingy is installed, plug in your MPC Sample to your computer via USB. install your app from the boot thingy web tool: 
- [http://mpc-sample.local:8080](http://mpc-sample.local:8080)
- [http://192.168.155.1:8080](http://192.168.155.1:8080)

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

