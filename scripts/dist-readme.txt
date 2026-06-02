Horizon CloudBeats
==================

Thanks for grabbing this! Horizon CloudBeats is a free, open-source mod
that drops a brand new station into Forza Horizon 6's radio dial. Feed it
music from Spotify, from any YouTube / YouTube Music link, from your own
Jellyfin server, or from a folder of files on your PC, and the game treats
the result like every other station: it ducks for menus, follows your
in-game volume slider, and fades on the loading screen.


Getting it running
~~~~~~~~~~~~~~~~~~~

Make sure FH6 isn't open first. Then drop the contents of this archive
straight into the folder that contains forzahorizon6.exe. Depending on
where you installed the game, that'll look like one of:

    Steam      ->  ...\steamapps\common\ForzaHorizon6
    Xbox app   ->  ...\XboxGames\Forza Horizon 6\Content

Let Windows overwrite when it asks. Heads-up: some antivirus tools flag
the bundled version.dll because of how the mod hooks into the game. If
yours quarantines it, add the FH6 folder to its exclusions and re-extract.

Once the files are in place, launch the game and head into
Settings > Audio. Two switches matter:

    Streamer Mode  ->  ON     (the new station only shows up with this on)
    Radio DJ       ->  OFF    (otherwise the in-game DJ talks over your
                                tracks)

Now cycle the radio stations in-game until you land on Horizon CloudBeats.
The mod only broadcasts while that station is the active one.


Opening the dashboard
~~~~~~~~~~~~~~~~~~~~~~

Everything is controlled from a small dashboard. You have two ways in:

  * Desktop app: just open HorizonCloudBeats.exe (included). It shows the
    dashboard in its own window and connects automatically once the game
    is running - no browser needed.

  * Browser: with the game running, open http://localhost:8420 on the
    same machine. From another device on the same network, use your PC's
    local IP instead (e.g. http://192.168.1.42:8420); run `ipconfig` in a
    Command Prompt to find it.


Your sources
~~~~~~~~~~~~

  * Spotify: paste any public playlist, album, or track link. No Premium
    and no login required - tracks are matched and streamed via YouTube.

  * YouTube / YouTube Music: paste a video, playlist, or YT Music link.

  * Jellyfin: stream playlists or favorites from your own Jellyfin server.
    Set the server URL, API key, user ID, and playlist under
    Settings > Jellyfin.

  * Local files: point it at a folder of MP3, FLAC, WAV, OGG, M4A, AAC,
    OPUS or WMA tracks (subfolders included). MP3 / FLAC / WAV / OGG play
    out of the box; the rest route through ffmpeg.

Spotify and YouTube share the same engine and need three free tools. Open
a Command Prompt and run:

    winget install yt-dlp.yt-dlp
    winget install Gyan.FFmpeg
    winget install DenoLand.Deno

Then restart the game. yt-dlp and ffmpeg can also be set manually from the
dashboard under Settings. For age-gated or private YouTube content, export
your browser cookies as a Netscape cookies.txt (use an extension like
"Get cookies.txt LOCALLY") and load it from Settings > YouTube Music.


In-game extras
~~~~~~~~~~~~~~

A few options live under Settings in the dashboard:

  * Race start action: on race begin, go to the next track, restart the
    current one, or leave it alone.
  * Quick station skip: tune the radio knob away and back within 1s to
    skip the current track.
  * Loudness normalization for consistent volume across tracks.
  * 5-band equalizer (60 Hz / 250 Hz / 1 kHz / 4 kHz / 12 kHz, +/-6 dB
    per band, applied at 48 kHz before audio reaches the game).
  * Optional now-playing banner on every track change.

Changes save the moment you make them - no need to restart the game.


Uninstalling
~~~~~~~~~~~~

Remove two things from the FH6 install folder: version.dll, and the
fh6-radio/ folder next to it. Then run "Verify integrity of game files"
(Steam) or "Repair" (Xbox app / MS Store) to restore the original assets.


About
~~~~~

Made by MyLuxy.

    Profile:  https://github.com/MyLuxy
    Project:  https://github.com/MyLuxy/fh6-cloudbeats-custom-radio

Unofficial fan project. Not affiliated with, endorsed by, or connected to
Turn 10 Studios, Playground Games, Xbox Game Studios, Microsoft, Google,
YouTube, Spotify, or Jellyfin (Jellyfin LLC). Forza Horizon and all other
names belong to their respective owners. Provided as-is, no warranty, use
at your own risk.


---------------------------------------------------------------------
Horizon CloudBeats is released under the GPLv3 license and is a fork of
FH6 Universal Radio by g0ldyy.

    Original project:  https://github.com/g0ldyy/fh6-universal-radio
    Original author:   https://github.com/g0ldyy  (g0ldyy)
---------------------------------------------------------------------
