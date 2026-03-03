# Unofficial Mazda Connect (tm) (*) Android Auto Headunit App

Unofficial port of Android Auto Headunit App to Mazda Connect CMU. The app makes extensive use of jni functions that were originally developed by Mike Reid as part of his Android app. The Mazda specific source code is under the /mazda folder.

(*) - Mazda and Mazda Connect are trademarks of Mazda NA

The goal of this fork is to update the code to modernize it, and to use libprotobuf instead of manually parsing the messages so that it will be easier to add more complete integration with car systems.

## Quick Start (Ubuntu)

Once Ubuntu is all set up this is the complete set of commands to take you from 0 to Android Auto in about 30-60 minutes:

```bash
cd ~
sudo apt-get install git adb bluetooth libbluetooth-dev tlp blueman bluemon bluez libsdl2-2.0-0 libsdl2-ttf-2.0-0 libportaudio2 gstreamer1.0-plugins-base-apps gstreamer1.0-plugins-bad gstreamer1.0-libav gstreamer1.0-alsa
sudo apt-get install libssl-dev libusb-1.0-0-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libsdl1.2-dev libgtk-3-dev libudev-dev libunwind-dev libsdl2-dev libgstreamer-plugins-bad1.0-dev protobuf-compiler libdbusxx
git clone --recursive //github.com/gartnera/headunit.git
sed -i 's/xvimagesink/ximagesink/g' ~/headunit/ubuntu/outputs.cpp # ONLY NEEDED if running on a virtual machine
cd ~/headunit/ubuntu
make clean && make
sudo ./headunit
```

Now you have the Android Auto Ubuntu Emulator installed. Every time you want to open the emulator back up you just enter the command:

```bash
sudo ~/headunit/ubuntu/headunit
```

To compile a headunit binary for use in the car run:

```bash
cd ~/headunit/mazda && make clean && make
```

I hope this will lead to more programmers who did not want to bother installing a new operating system or spend a lot of time figuring out how to set up the development environment and whatnot to contribute to the Android Auto project. I tried to make these instructions as simple, clear, and straightforward as possible but if there are any ambiguities or you have an improvement or suggestion for this tutorial shoot me an email. To all my fellow HaXors out there Happy Hacking!

### Notes

- You may need to manually reconnect your phone if it is disconnected by AA.
- You need to have your phone in MTP mode (you can try PTP as well) to make the adb connection properly.
- If you are having trouble connecting you may be using an incompatible Bluetooth receiver.
