## mpc boot thingy

this thingy will allow various MPC software like custom firmware and custom applications to work without breaking eachother.
you can install applications and choose between them easily from the menu.

there are other enhancements this provides, but ehh...they're there... :)

## install boot thingy

there are several ways to install boot thingy.

* USB
   - recommended
   - automatically installs boot thingy to your SD card
* SD card
   - manual install
   - copy autoboot directory to SD card root
* SSH
   - ssh will only work on an existing boot thingy install
   - easy to install for devs iterating on an app

### usb installer

this installs boot thingy to the SD card...connect your MPC Sample to your computer via USB, then run:

```
./setup.sh --usb
```

on your MPC Sample, it will ask if you want to install boot thingy...after install, it will automatically reboot the device and boot thingy is ready to go

### SD card install

download the release zip and extract it...there will be a folder called `autoboot` that you need to copy to SD:

1. mount your SD card to your computer.
2. copy the `autoboot` directory in the release zip to the root of your sd card.
3. plug the sd card into your MPC Sample.

> if your MPC Sample is already running, it will automatically boot into boot thingy without a reboot

### SSH

if you have an existing boot thingy install, you can update the boot thingy with ssh:

```
./setup.sh --ssh
```