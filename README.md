NORTHSTAR RADIO

Plugin for GTA VC that recreates the whole radio system from scratch, allowing you to add infinite amount of new radio stations, assign new default stations to vehicles and play ambient radio from traffic. Created with AI (Claude).

Current version: 1.0

CHANGELOG

Version 1.0
- Stronger suppression of the original radio: the stock Vice City radio and its on-screen station name are now fully disabled, so the game's radio can no longer play or switch underneath the plugin — including via mouse scroll, the radio key, and the controller button.
- Station name display fixed: it now scales with the game resolution, is slightly larger, and no longer shows a black box behind it when other on-screen text is present.
- Radio volume now matches the game: the new radio follows the menu's Radio Volume slider correctly (re-calibrated so it is no longer roughly twice as loud, and fully silent at zero). The radio pauses while the menu is open and the new level applies when you return to the game.
- New [NORADIO] section: list vehicle model IDs that should have no radio at all — no music, no police radio, no story announcements, and no ambient radio from that model in traffic. Takes precedence over [VEHICLES].
- Story radio announcements restored: the hurricane / bridge-status bulletins now play through the new radio system at the right story moments.
- Mission radio changes honored: missions that set a specific car-radio station now switch the new radio to that station.
- Original Vice City stations now start at the top of the broadcast on a fresh new game, while added/custom stations keep the randomized "already playing" start.
- New [STARTOFFSET] section: open a station at a specific point in its broadcast (for example Flash FM on Billie Jean, like the original intro). Applied to the new-game intro only — loading a save resumes the radio naturally.
- Fixed a bug where the radio could play over the loading screen when loading a save (and could resume a stale station before the player was back in a vehicle).
- Added a Visual Studio project file (NorthstarRadio.vcxproj) so the plugin can be built from source out of the box.
- The radio now goes silent during cutscenes and scripted mission scenes — including a cutscene triggered while you're driving — and resumes when the scene ends.
- The [STARTOFFSET] intro song (e.g. Flash FM on Billie Jean) now plays reliably no matter how long the intro cutscenes run, and continues forward after a radio announcement instead of resetting to the start.
- Audio ducking: the radio now dims while mission characters speak (mission dialogue) and during the "mission passed" jingle, then smoothly returns to full volume — like the original game.
- Fixed the [STARTOFFSET] intro song replaying when loading a saved game — the intro cue now applies only to a genuinely new game (even across a full game restart), and loaded saves always resume the radio naturally.
- Fixed an occasional crash when entering or changing vehicles (and when knocked off a motorcycle), caused by reading the vehicle before the game had finished seating the player in it.
- Vehicles now remember their station for as long as they stay loaded: leave a car or get knocked off a bike, come back any time later, and the same station is still playing (it previously re-randomized after about 5 seconds).
- A different/new vehicle now always starts on its own station (its [VEHICLES] assignment or a random one) instead of briefly carrying over the previous vehicle's station — how close the cars are no longer matters.
- New [SETTINGS] RadioAutoTune option: set to 1 to make the station you're listening to follow you into every vehicle you enter, regardless of distance (off by default).
- Fixed the original Vice City stations restarting from the top of the broadcast after quitting and loading a save — they now resume at a natural spot, and only open at the top on a genuine new game.
- New [SETTINGS] ScriptIntegration option: set to 0 to run the core radio without hooking the game's mission script (no story announcements, mission-radio changes, or ducking) — for total-conversion mods with a custom main.scm where that integration would otherwise crash on missions.
- The default controller button for changing station is now LEFT SHOULDER (L1 / LB) — Vice City's native radio button — instead of D-PAD LEFT.
- Fixed interiors that play their own ambient music/radio (clubs, shops, etc.) being silenced by the radio suppression — they now play normally while you're on foot inside an interior, with the radio still fully suppressed in the open world and in vehicles.

Version 0.9
- Initial release.

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
11. You can disable the radio entirely for specific vehicles through the [NORADIO] section of INI file. List one numeric vehicle model id per line (same id list as [VEHICLES]). Those vehicles will have no radio at all — no music, no police radio, no story announcements, and no ambient radio in traffic. This takes precedence over the [VEHICLES] section.
12. You can make a station open at a specific point in its broadcast through the [STARTOFFSET] section of INI file. Format: "Station Name | M:SS" (or plain seconds). The station will start from that position each launch instead of from the beginning. This is useful for recreating the original VC behaviour where the intro opens Flash FM on a specific song.
13. You can make the radio follow you between vehicles with the RadioAutoTune option in the [SETTINGS] section. With RadioAutoTune = 1, whatever station you are listening to keeps playing when you get into any other vehicle, no matter the distance. With RadioAutoTune = 0 (default), each different/new vehicle uses its own assigned station or a random one, and the same vehicle still remembers its own station when you return to it.
14. You can turn off the in-game script integration with the ScriptIntegration option in the [SETTINGS] section. With ScriptIntegration = 0 the plugin stops hooking the game's mission script, so you lose the story radio announcements, mission-forced station changes and audio ducking, but the rest of the radio works normally. This is meant for total-conversion mods that use a custom main.scm and crash when the script is hooked. Default is 1 (full integration).

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

Instagram: https://www.instagram.com/northstar.games.studio

TikTok: https://www.tiktok.com/@northstar.games

Discord: https://discord.com/invite/HbeMpFxXc6

Website: https://northstargames.tilda.ws/
