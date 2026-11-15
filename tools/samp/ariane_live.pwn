/*
    MIT License

    Copyright (c) 2026 NikolasOG (Ariane Plus)

    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
    associated documentation files (the "Software"), to deal in the Software without restriction,
    including without limitation the rights to use, copy, modify, merge, publish, distribute,
    sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all copies or
    substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
    NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
    NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
    OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
    CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

    This file is MIT-licensed (unlike the GPL editor) so gamemodes and filterscripts can include it
    freely, open or closed source.
*/

/*
    Ariane Plus live link - ready-made filterscript for any open.mp / SA-MP server.

    Everything it does comes from ariane.inc (the server side of Ariane): the live map that follows
    Ariane's "Sync to server", Ariane's "Show in game", a fly-around freecam, and commands for RCON admins:
        /arianefreecam   fly around (W/S/A/D along the view, jump up, crouch down,
                         sprint faster, walk slower); again to land where you are looking from
        /arianefocus     move Ariane's camera to where you are (or where your freecam is)
        /arianeview      go to the last spot Ariane showed in game

    Setup
      1. Copy ariane.inc to your compiler's include folder (qawno/include or pawno/include) and this
         file to filterscripts/. Compile it (needs the streamer plugin include).
      2. Add "ariane_live" to the filterscripts in config.json (open.mp) or server.cfg (SA-MP), and
         load the streamer plugin.
      3. In Ariane: SA-MP > Server folder... (the folder with scriptfiles/), then Sync to server.

    To use Ariane from your own gamemode instead (callbacks, queries, your own permission check),
    #include <ariane> there - see the top of ariane.inc.
*/

#define FILTERSCRIPT

#include <open.mp>
#include <streamer>
#include <ariane>

public OnFilterScriptInit()
{
    print("[ariane_live] loaded - /arianefreecam, /arianefocus and /arianeview for RCON admins");
    return 1;
}

public OnPlayerCommandText(playerid, cmdtext[])
{
    if (!strcmp(cmdtext, "/arianefreecam", true))
    {
        if (!IsPlayerAdmin(playerid)) return 0;
        if (Ariane_IsInFreecam(playerid))
            Ariane_FreecamStop(playerid, true);
        else if (Ariane_FreecamStart(playerid))
            SendClientMessage(playerid, 0xFFCC66FF, "[Ariane] Freecam: W/S/A/D move, jump up, crouch down, sprint fast, walk slow. /arianefreecam to land.");
        return 1;
    }
    if (!strcmp(cmdtext, "/arianefocus", true))
    {
        if (!IsPlayerAdmin(playerid)) return 0;
        if (Ariane_FocusEditor(playerid))
            SendClientMessage(playerid, 0xFFCC66FF, "[Ariane] Asked the editor to look here.");
        return 1;
    }
    if (!strcmp(cmdtext, "/arianeview", true))
    {
        if (!IsPlayerAdmin(playerid)) return 0;
        if (!Ariane_SendToEditorView(playerid))
            SendClientMessage(playerid, 0xFFCC66FF, "[Ariane] Use Show in game (Ctrl+Shift+T) in the editor first.");
        return 1;
    }
    return 0;
}
