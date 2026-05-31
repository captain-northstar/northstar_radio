NORTHSTAR RADIO

Plugin for GTA VC that recreates the whole radio system from scratch, allowing you to add infinite amount of new radio stations, assign new default stations to vehicles and play ambient radio from traffic. Created with AI (Claude).

HOW TO INSTALL

1. IMPORTANT: Download Ultimate ASI Loader (https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) and install it. The plugin will not work without it.
2. IMPORTANT: Download bass.dll (https://www.un4seen.com) and bass_fx.dll (https://www.un4seen.com/bass.html#addons), put them in the root folder of your GTA Vice City game. I cannot distribute these files, but plugin will not work without them.
3. Put NorthstarRadio.asi and NorthstarRadio.ini into your root GTA Vice City folder or your "scripts" folder.
4. Enjoy.

HOW TO SET UP

1.  You can assign new buttons to change radio station through [CONTROLS] section of INI file. You can find the full list of key ids here: https://asawicki.info/nosense/doc/devices/keyboard/key_codes.html
2.  You can assign controller buttons as well, according to this list:

0  — disabled
1 — DPAD UP
2 — DPAD DOWN
4 — DPAD LEFT
8 — DPAD RIGHT
16 — START
32 — BACK
64 — LEFT THUMB
128 — RIGHT THUMB
256 — LEFT SHOULDER
512 — RIGHT SHOULDER
4096 — A
8192 — B
16384 — X
32768 — Y

3. Mouse Scroll works by default both for changing to the next station and the previous one.
4. You can add new stations both in ADF and MP3 formats through [STATIONS] section of INI file. Just put the name of the station in the quotes and then path to the file from the root folder. For example, instead of "GTA - Vice City\Audio\WILD.adf" you should put "Audio\WILD.adf". The plugin is not able to find a file if it's not in the Vice City root folder.
5. The plugin is able to read both MP3 and ADF files. Please notice, that ADF files are decoded and loaded into memory, which uses more RAM than MP3. If you want to reduce memory usage, convert ADF files to MP3 and put the new path in INI file.
9. You can assign new default stations to vehicles through [VEHICLES] section of INI file. You should use vehicle's id from this list: https://gtamods.com/wiki/List_of_vehicles_(VC). Use the same name for radio station that you used in [STATIONS] section. If vehicle does not have an assigned station, the station will be picked at random. You can assign multiple stations to one vehicle, it will be chosen randomly on the fly.
10. You can turn Ambient Radio on and off through [SETTINGS] section of INI file. Only the cars listed in [VEHICLES] section will have ambient radio. Ambient radio turns off every time the vehicle is encountered, but only if it's required at a certain distance. If a vehicle spawned too close, you will not hear the ambient radio. It is by design.

HOW TO CODE

If you want to edit the code and build it from source, you're going to need the following:
1. Visual Studio 2019 or later
2. plugin-sdk for Vice City — https://github.com/DK22Pac/plugin-sdk
3. bass.lib + bass.h — from https://www.un4seen.com
4. bass_fx.lib + bass_fx.h — from https://www.un4seen.com/bass.html#addons
5. ASI loader for GTA VC ([e.g. Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases))
Linker flag required: /SAFESEH:NO (due to bass_fx.lib)

HOW IT ALL WORKS

- Turns off original radio.
- Turns off switching radio button.
- Every time the game is loaded, picks a number at random and adds 1 to it every millisecond, imitating the timestamp of the radio.
- If player sits in the car, picks a radio according to [VEHICLES] section of INI file and plays radio from the timestamp.
- MP3 PLAYER is never picked at random, because plugin only picks stations from INI file
- Detects if MP3 folder has any files, adds MP3 PLAYER station and plays songs in random order if it's chosen.
- Detects Music Volume that player set in the menu and plays the music accordingly.
- Pauses the music if player pauses the game.
- If AmbientRadio is turned on, detects which vehicles are loaded, checks if they are in [VEHICLES] section and plays the radio accordingly.
- Music volume depends on the player's settings and the distance.
- Sound is muffled through bass_fx.dll.
- If vehicles are spawned too close to the player, doesn't play the ambient radio.

HOW TO DISTRIBUTE

It's an open source project done with the help of AI. Please use it for your business and pleasure, but always credit me and never charge any money for it. You can edit the code according to your needs.

HOW TO CONTACT ME

GitHub: https://github.com/captain-northstar
EMail: northstar.games.studio@gmail.com
YouTube: https://www.youtube.com/@Northstar_Games_Studio
Instagram: instagram.com/northstar.games.studio
TikTok: tiktok.com/@northstar.games
Discord: discord.gg/HbeMpFxXc6
Website: northstargames.tilda.ws
