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
    ariane.inc - server side of Ariane Plus (Ariane SA-MP edition), for open.mp and SA-MP servers.

    Include it in a gamemode or filterscript (after <open.mp> or <a_samp> and <streamer>) to get:
      - the live map: Ariane's "Sync to server" writes scriptfiles/ariane_live.txt; this include rebuilds
        the map whenever it changes (objects, materials, removed buildings)
      - "Show in game": Ariane asks the server to move players to the editor's view
      - "Focus editor": your script moves Ariane's camera to a player or a point
      - functions to query and control all of the above, and callbacks to react to it
      - Ariane_CreateObject / Ariane_RemoveBuilding / Ariane_SetObjectMaterial(Text): export a map from
        Ariane in the wrapper format with the prefix "Ariane_" and it compiles against this include

    Needs the streamer plugin. Everything starts by itself (OnGameModeInit / OnFilterScriptInit and
    OnPlayerConnect are hooked). To start it yourself instead:
        #define ARIANE_NO_HOOKS
        #include <ariane>
        ... Ariane_Start(); in your init, Ariane_Stop(); in your exit,
            Ariane_PlayerConnected(playerid); in OnPlayerConnect,
            Ariane_PlayerSpawned(playerid); first in OnPlayerSpawn and Ariane_PlayerSpawnDone(playerid);
            at its end, Ariane_PlayerDisconnected(playerid); in OnPlayerDisconnect.

    Settings (#define before the include): ARIANE_MAX_OBJECTS (10000), ARIANE_MAX_REMOVALS (2000),
    ARIANE_LIVE_FILE ("ariane_live.txt").

    ---------------------------------------------------------------- callbacks (all optional)

    public OnArianeMapUpdate(objects, materials, removals)
        The live map was rebuilt (server start and every sync from Ariane).

    public OnArianeObjectCreate(STREAMER_TAG_OBJECT:objectid, modelid, index)
        Called for each live object as it's created: tag doors, add labels, make things usable.
        index is its position in the map (0 .. Ariane_GetObjectCount() - 1).

    public OnArianeShowInGame(playerid, Float:x, Float:y, Float:z)
        A player is about to be moved to the editor's view. Return 0 to leave them where they are.

    public Ariane_CanFollow(playerid)
        Your own permission check, used when Ariane's "Show in game moves" is set to
        "Players the server script picks". Return 1 for players who may be moved.

    public OnArianeFreecam(playerid, bool:enabled)
        A player started or stopped the freecam.

    ---------------------------------------------------------------- functions

    Ariane_SetPollInterval(ms)              how often the live file is checked (default 3000)
    Ariane_SetDrawDistance(Float:distance)  stream and draw distance of live objects (default 300.0)
    Ariane_SetLiveWorld(worldid)            live objects meant for every world go to this one (-1 = all)
    Ariane_EnableLive(bool:enable)          pause / resume live updates (the current map stays)
    Ariane_EnableShowInGame(bool:enable)    ignore / follow Ariane's Show in game requests
    Ariane_Reload()                         rebuild the live map from the file now
    Ariane_ClearLive()                      destroy the live objects (removed buildings stay removed)

    Ariane_GetObjectCount()
    STREAMER_TAG_OBJECT:Ariane_GetObject(index)        INVALID_STREAMER_ID if out of range
    Ariane_GetObjectModel(index)
    Ariane_GetObjectIndex(STREAMER_TAG_OBJECT:objectid)  -1 if it isn't a live map object
    bool:Ariane_IsMapObject(STREAMER_TAG_OBJECT:objectid)
    Ariane_GetRemovalCount()                live + code removals
    bool:Ariane_GetRemoval(index, &modelid, &Float:x, &Float:y, &Float:z, &Float:radius)
    Ariane_ApplyRemovals(playerid)          RemoveBuildingForPlayer for every removal (done on connect)
    Ariane_GetRevision(dest[], size = sizeof dest)   the sync revision the live map was built from

    Ariane_TeleportPlayer(playerid, Float:x, Float:y, Float:z, Float:angle = 0.0, interior = 0, world = 0)
                                            streamer-aware move (objects load before the player lands)
    bool:Ariane_GetEditorView(&Float:x, &Float:y, &Float:z, &Float:angle)   last Show in game spot
    bool:Ariane_SendToEditorView(playerid)  move a player to that spot
    Ariane_FocusEditor(playerid)            move Ariane's camera to this player (or their freecam)
    Ariane_FocusEditorAt(Float:x, Float:y, Float:z)  ... or to a point
        (Ariane follows these when SA-MP > "Follow the server's focus requests" is on)

    Freecam - fly around freely, for mapping and checking a map in game:
    bool:Ariane_FreecamStart(playerid)      starts at the player's current camera
    bool:Ariane_FreecamStop(playerid, bool:landHere = false)
                                            back to where they were, or land at the camera
    bool:Ariane_IsInFreecam(playerid)
    bool:Ariane_GetFreecamPos(playerid, &Float:x, &Float:y, &Float:z)
    Ariane_SetFreecamSpeed(Float:metresPerSecond)   default 20; sprint x4, walk x0.25
    bool:Ariane_IsFreecamRespawn(playerid)  true during the OnPlayerSpawn that ends a freecam
                                            (skip your own spawn logic for it)
        Keys: W/S along the view, A/D sideways, jump up, crouch down, sprint faster, walk slower.
        The player is spectating while flying (hidden, not hittable). Leaving respawns them where they
        were (or at the camera); on open.mp their spawn info is put back afterwards. Show in game
        moves a flying player's camera instead of the player.

    Wrapper export (SA-MP > Export Pawn..., format "wrapper", prefix "Ariane_"):
    Ariane_CreateObject(modelid, Float:x, Float:y, Float:z, Float:rx, Float:ry, Float:rz, worldid = -1, interiorid = -1)
    Ariane_RemoveBuilding(modelid, Float:x, Float:y, Float:z, Float:radius)    every player, now and later
    Ariane_SetObjectMaterial(STREAMER_TAG_OBJECT:objectid, index, modelid, const txd[], const texture[], color = 0)
    Ariane_SetObjectMaterialText(STREAMER_TAG_OBJECT:objectid, index, const text[], size, const font[],
        fontsize, bold, fontcolor, backcolor, align)

    Removed buildings can't be brought back for players already connected: a building you restore in
    Ariane reappears for them after they reconnect.
*/

#if defined _ariane_included
    #endinput
#endif
#define _ariane_included

#if !defined _streamer_included
    #include <streamer>
#endif

#if !defined ARIANE_MAX_OBJECTS
    #define ARIANE_MAX_OBJECTS      (10000)
#endif
#if !defined ARIANE_MAX_REMOVALS
    #define ARIANE_MAX_REMOVALS     (2000)
#endif
#if !defined ARIANE_LIVE_FILE
    #define ARIANE_LIVE_FILE        "ariane_live.txt"
#endif
#define ARIANE_GOTO_FILE            "ariane_goto.txt"
#define ARIANE_FOCUS_FILE           "ariane_focus.txt"
#define ARIANE_GOTO_MS              (1000)

forward OnArianeMapUpdate(objects, materials, removals);
forward OnArianeObjectCreate(STREAMER_TAG_OBJECT:objectid, modelid, index);
forward OnArianeShowInGame(playerid, Float:x, Float:y, Float:z);
forward Ariane_CanFollow(playerid);
forward Ariane_OnPollTimer();
forward Ariane_OnGotoTimer();

enum E_ARIANE_REMOVAL { ariModel, Float:ariX, Float:ariY, Float:ariZ, Float:ariRadius }

static ari_Rev[24];
static ari_GotoSeq[24];
static STREAMER_TAG_OBJECT:ari_Objects[ARIANE_MAX_OBJECTS];
static ari_ObjectModel[ARIANE_MAX_OBJECTS];
static ari_ObjectCount;
static ari_Removals[ARIANE_MAX_REMOVALS][E_ARIANE_REMOVAL];
static ari_RemovalCount;
static ari_CodeRemovals[ARIANE_MAX_REMOVALS][E_ARIANE_REMOVAL];
static ari_CodeRemovalCount;
static ari_PollTimer = -1;
static ari_GotoTimer = -1;
static ari_PollMs = 3000;
static Float:ari_DrawDistance = 300.0;
static ari_LiveWorld = -1;
static bool:ari_Live = true;
static bool:ari_ShowInGame = true;
static bool:ari_Started;
static bool:ari_HaveView;
static Float:ari_ViewX, Float:ari_ViewY, Float:ari_ViewZ, Float:ari_ViewA, ari_ViewInterior, ari_ViewWorld;
static ari_FocusCounter;

#if !defined ARIANE_FREECAM_SPEED
    #define ARIANE_FREECAM_SPEED    (20.0)
#endif
#if defined _INC_open_mp
    #define ARI_KEYTAG KEY:
#else
    #define ARI_KEYTAG _:
#endif
forward OnArianeFreecam(playerid, bool:enabled);
forward Ariane_OnFreecamTimer();
static ari_FcObject[MAX_PLAYERS] = { -1, ... };
static bool:ari_FcMoving[MAX_PLAYERS];
static bool:ari_FcRespawn[MAX_PLAYERS];
static Float:ari_FcBack[MAX_PLAYERS][4];
static ari_FcBackInterior[MAX_PLAYERS], ari_FcBackWorld[MAX_PLAYERS];
static ari_FcTimer = -1;
static Float:ari_FcSpeed = ARIANE_FREECAM_SPEED;
#if defined GetSpawnInfo
static ari_FcSpawnTeam[MAX_PLAYERS], ari_FcSpawnSkin[MAX_PLAYERS];
static Float:ari_FcSpawn[MAX_PLAYERS][4];
#endif

// ---------------------------------------------------------------- line parsing

// Copies the idx-th space separated word of src into dest.
static stock bool:ari_Word(const src[], idx, dest[], size = sizeof (dest))
{
    new pos = 0, len = strlen(src), cur = 0;
    dest[0] = '\0';
    while (pos < len)
    {
        while (pos < len && src[pos] == ' ') pos++;
        if (pos >= len) break;
        new start = pos;
        while (pos < len && src[pos] != ' ') pos++;
        if (cur == idx)
        {
            strmid(dest, src, start, pos, size);
            return true;
        }
        cur++;
    }
    return false;
}

// Copies everything from the idx-th word to the end of the line.
static stock bool:ari_Rest(const src[], idx, dest[], size = sizeof (dest))
{
    new pos = 0, len = strlen(src), cur = 0;
    dest[0] = '\0';
    while (pos < len)
    {
        while (pos < len && src[pos] == ' ') pos++;
        if (pos >= len) break;
        if (cur == idx)
        {
            strmid(dest, src, pos, len, size);
            return true;
        }
        while (pos < len && src[pos] != ' ') pos++;
        cur++;
    }
    return false;
}

static stock ari_Int(const line[], idx)
{
    new tmp[16];
    ari_Word(line, idx, tmp);
    return strval(tmp);
}

static stock Float:ari_Float(const line[], idx)
{
    new tmp[24];
    ari_Word(line, idx, tmp);
    return floatstr(tmp);
}

// "0xAARRGGBB" or a decimal number
static stock ari_Hex(const line[], idx)
{
    new tmp[16];
    if (!ari_Word(line, idx, tmp)) return 0;
    if (tmp[0] == '0' && (tmp[1] == 'x' || tmp[1] == 'X'))
    {
        new v = 0;
        for (new i = 2; tmp[i] != '\0'; i++)
        {
            new c = tmp[i], d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = (v << 4) | d;
        }
        return v;
    }
    return strval(tmp);
}

static stock ari_Strip(line[])
{
    new len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = '\0';
}

// strcmp that treats "" as different from anything (SA-MP's strcmp says "" equals every string)
static stock bool:ari_Same(const a[], const b[])
{
    if (a[0] == '\0' || b[0] == '\0') return a[0] == b[0];
    return strcmp(a, b) == 0;
}

// ---------------------------------------------------------------- live map

stock Ariane_ApplyRemovals(playerid)
{
    for (new i = 0; i < ari_CodeRemovalCount; i++)
        RemoveBuildingForPlayer(playerid, ari_CodeRemovals[i][ariModel], ari_CodeRemovals[i][ariX], ari_CodeRemovals[i][ariY], ari_CodeRemovals[i][ariZ], ari_CodeRemovals[i][ariRadius]);
    for (new i = 0; i < ari_RemovalCount; i++)
        RemoveBuildingForPlayer(playerid, ari_Removals[i][ariModel], ari_Removals[i][ariX], ari_Removals[i][ariY], ari_Removals[i][ariZ], ari_Removals[i][ariRadius]);
}

stock Ariane_ClearLive()
{
    for (new i = 0; i < ari_ObjectCount; i++)
        if (IsValidDynamicObject(ari_Objects[i])) DestroyDynamicObject(ari_Objects[i]);
    ari_ObjectCount = 0;
}

// Reads the live file; rebuilds when its revision changed (or always, when force is set).
static stock ari_LoadLive(bool:force)
{
    if (!fexist(ARIANE_LIVE_FILE)) return 0;
    new File:f = fopen(ARIANE_LIVE_FILE, io_read);
    if (!f) return 0;

    new line[512], rev[24];
    if (!fread(f, line))
    {
        fclose(f);
        return 0;
    }
    ari_Strip(line);
    ari_Word(line, 1, rev);
    if (strcmp(line, "rev ", false, 4) || rev[0] == '\0' || (!force && ari_Same(rev, ari_Rev)))
    {
        fclose(f);
        return 0;
    }
    format(ari_Rev, sizeof (ari_Rev), "%s", rev);

    Ariane_ClearLive();
    ari_RemovalCount = 0;
    new bool:perObject = funcidx("OnArianeObjectCreate") != -1;
    new skipped = 0, materials = 0, STREAMER_TAG_OBJECT:last = STREAMER_TAG_OBJECT:INVALID_STREAMER_ID;
    while (fread(f, line))
    {
        ari_Strip(line);
        if (line[1] != ' ') continue;
        switch (line[0])
        {
            case 'O':
            {
                last = STREAMER_TAG_OBJECT:INVALID_STREAMER_ID;
                new model = ari_Int(line, 1);
                if (model == 0 || ari_ObjectCount >= ARIANE_MAX_OBJECTS) { skipped++; continue; }
                new world = ari_Int(line, 8), interior = ari_Int(line, 9);
                if (world == -1) world = ari_LiveWorld;
                last = CreateDynamicObject(model, ari_Float(line, 2), ari_Float(line, 3), ari_Float(line, 4),
                    ari_Float(line, 5), ari_Float(line, 6), ari_Float(line, 7),
                    world, interior == 0 ? -1 : interior, -1, ari_DrawDistance, ari_DrawDistance);
                new index = ari_ObjectCount++;
                ari_Objects[index] = last;
                ari_ObjectModel[index] = model;
                if (perObject) CallLocalFunction("OnArianeObjectCreate", "iii", _:last, model, index);
            }
            case 'M':
            {
                if (last == STREAMER_TAG_OBJECT:INVALID_STREAMER_ID) { skipped++; continue; }
                new txd[32], texture[64];
                ari_Word(line, 3, txd);
                ari_Word(line, 4, texture);
                SetDynamicObjectMaterial(last, ari_Int(line, 1), ari_Int(line, 2), txd, texture, ari_Hex(line, 5));
                materials++;
            }
            case 'T':
            {
                if (last == STREAMER_TAG_OBJECT:INVALID_STREAMER_ID) { skipped++; continue; }
                new font[32], raw[256], text[256], n = 0;
                ari_Word(line, 8, font);
                for (new i = 0; font[i] != '\0'; i++) if (font[i] == '_') font[i] = ' ';
                ari_Rest(line, 9, raw);
                for (new i = 0; raw[i] != '\0' && n < sizeof (text) - 1; i++)
                {
                    if (raw[i] == '\\' && raw[i + 1] == 'n') { text[n++] = '\n'; i++; }
                    else if (raw[i] == '\\' && raw[i + 1] == '\\') { text[n++] = '\\'; i++; }
                    else text[n++] = raw[i];
                }
                text[n] = '\0';
                SetDynamicObjectMaterialText(last, ari_Int(line, 1), text, ari_Int(line, 2), font, ari_Int(line, 3),
                    ari_Int(line, 4), ari_Hex(line, 5), ari_Hex(line, 6), ari_Int(line, 7));
                materials++;
            }
            case 'R':
            {
                if (ari_RemovalCount >= ARIANE_MAX_REMOVALS) { skipped++; continue; }
                new r = ari_RemovalCount++;
                ari_Removals[r][ariModel] = ari_Int(line, 1);
                ari_Removals[r][ariX] = ari_Float(line, 2);
                ari_Removals[r][ariY] = ari_Float(line, 3);
                ari_Removals[r][ariZ] = ari_Float(line, 4);
                ari_Removals[r][ariRadius] = ari_Float(line, 5);
            }
        }
    }
    fclose(f);

    for (new i = 0; i < MAX_PLAYERS; i++)
        if (IsPlayerConnected(i)) Ariane_ApplyRemovals(i);

    printf("[ariane] map updated: %d objects (%d materials), %d removed buildings%s",
        ari_ObjectCount, materials, ari_RemovalCount, skipped ? (" - some lines skipped (limits or unknown models)") : (""));
    CallLocalFunction("OnArianeMapUpdate", "iii", ari_ObjectCount, materials, ari_RemovalCount);
    return 1;
}

stock Ariane_Reload()
{
    return ari_LoadLive(true);
}

public Ariane_OnPollTimer()
{
    if (ari_Live) ari_LoadLive(false);
    return 1;
}

// ---------------------------------------------------------------- show in game / focus editor

stock Ariane_TeleportPlayer(playerid, Float:x, Float:y, Float:z, Float:angle = 0.0, interior = 0, world = 0)
{
    if (!IsPlayerConnected(playerid)) return 0;
    SetPlayerInterior(playerid, interior);
    SetPlayerVirtualWorld(playerid, world);
    Streamer_UpdateEx(playerid, x, y, z, world, interior);
    if (IsPlayerInAnyVehicle(playerid) && GetPlayerState(playerid) == PLAYER_STATE_DRIVER)
    {
        new v = GetPlayerVehicleID(playerid);
        SetVehiclePos(v, x, y, z);
        SetVehicleZAngle(v, angle);
        LinkVehicleToInterior(v, interior);
        SetVehicleVirtualWorld(v, world);
    }
    else
    {
        SetPlayerPos(playerid, x, y, z);
        SetPlayerFacingAngle(playerid, angle);
    }
    SetCameraBehindPlayer(playerid);
    return 1;
}

stock bool:Ariane_GetEditorView(&Float:x, &Float:y, &Float:z, &Float:angle)
{
    if (!ari_HaveView) return false;
    x = ari_ViewX; y = ari_ViewY; z = ari_ViewZ; angle = ari_ViewA;
    return true;
}

stock bool:Ariane_SendToEditorView(playerid)
{
    if (!ari_HaveView) return false;
    if (playerid >= 0 && playerid < MAX_PLAYERS && ari_FcObject[playerid] != -1)
    {
        // flying: move the camera there, a little above the ground point
        StopPlayerObject(playerid, ari_FcObject[playerid]);
        ari_FcMoving[playerid] = false;
        SetPlayerObjectPos(playerid, ari_FcObject[playerid], ari_ViewX, ari_ViewY, ari_ViewZ + 2.0);
        return true;
    }
    return Ariane_TeleportPlayer(playerid, ari_ViewX, ari_ViewY, ari_ViewZ, ari_ViewA, ari_ViewInterior, ari_ViewWorld) != 0;
}

static stock bool:ari_ShouldFollow(playerid, const who[], const arg[])
{
    if (!strcmp(who, "all")) return true;
    if (!strcmp(who, "player"))
    {
        if (arg[0] == '\0') return false;
        new bool:isId = true;
        for (new i = 0; arg[i] != '\0'; i++) if (arg[i] < '0' || arg[i] > '9') isId = false;
        if (isId) return playerid == strval(arg);
        new name[MAX_PLAYER_NAME + 1];
        GetPlayerName(playerid, name, sizeof name);
        return strcmp(name, arg, true) == 0;
    }
    // your own permission check (0 when no script defines Ariane_CanFollow)
    if (!strcmp(who, "script")) return CallRemoteFunction("Ariane_CanFollow", "i", playerid) != 0;
    return !!IsPlayerAdmin(playerid);
}

// "goto <seq> <x> <y> <z> <angle> <interior> <world> <who> [player]"; returns false if there's no request
static stock bool:ari_ReadGoto(line[], size, seq[], seqSize)
{
    line[0] = '\0';
    seq[0] = '\0';
    if (!fexist(ARIANE_GOTO_FILE)) return false;
    new File:f = fopen(ARIANE_GOTO_FILE, io_read);
    if (!f) return false;
    new ok = fread(f, line, size);
    fclose(f);
    if (!ok) return false;
    ari_Strip(line);
    if (strcmp(line, "goto ", false, 5)) return false;
    ari_Word(line, 1, seq, seqSize);
    return seq[0] != '\0';
}

public Ariane_OnGotoTimer()
{
    new line[160], seq[24];
    if (!ari_ReadGoto(line, sizeof line, seq, sizeof seq) || ari_Same(seq, ari_GotoSeq)) return 1;
    format(ari_GotoSeq, sizeof (ari_GotoSeq), "%s", seq);
    ari_ViewX = ari_Float(line, 2); ari_ViewY = ari_Float(line, 3); ari_ViewZ = ari_Float(line, 4);
    ari_ViewA = ari_Float(line, 5); ari_ViewInterior = ari_Int(line, 6); ari_ViewWorld = ari_Int(line, 7);
    ari_HaveView = true;
    if (!ari_ShowInGame) return 1;
    new who[16], arg[MAX_PLAYER_NAME + 1], moved = 0;
    if (!ari_Word(line, 8, who)) format(who, sizeof who, "rcon");
    ari_Word(line, 9, arg);
    new bool:askScript = funcidx("OnArianeShowInGame") != -1;
    for (new i = 0; i < MAX_PLAYERS; i++)
    {
        if (!IsPlayerConnected(i) || !ari_ShouldFollow(i, who, arg)) continue;
        if (askScript && !CallLocalFunction("OnArianeShowInGame", "ifff", i, ari_ViewX, ari_ViewY, ari_ViewZ)) continue;
        Ariane_SendToEditorView(i);
        SendClientMessage(i, 0xFFCC66FF, "[Ariane] Moved you to the editor's view.");
        moved++;
    }
    printf("[ariane] show in game (%s%s%s): %.1f %.1f %.1f - moved %d player(s)", who, arg[0] ? (" ") : (""), arg,
        ari_ViewX, ari_ViewY, ari_ViewZ, moved);
    return 1;
}

stock Ariane_FocusEditorAt(Float:x, Float:y, Float:z)
{
    new File:f = fopen(ARIANE_FOCUS_FILE, io_write);
    if (!f) return 0;
    new line[96];
    format(line, sizeof line, "focus %d%d %.3f %.3f %.3f\r\n", gettime(), ++ari_FocusCounter % 1000, x, y, z);
    fwrite(f, line);
    fclose(f);
    return 1;
}

stock Ariane_FocusEditor(playerid)
{
    new Float:x, Float:y, Float:z;
    if (playerid >= 0 && playerid < MAX_PLAYERS && ari_FcObject[playerid] != -1)
        GetPlayerObjectPos(playerid, ari_FcObject[playerid], x, y, z);
    else if (!GetPlayerPos(playerid, x, y, z))
        return 0;
    return Ariane_FocusEditorAt(x, y, z);
}

// ---------------------------------------------------------------- freecam

stock bool:Ariane_IsInFreecam(playerid)
{
    return playerid >= 0 && playerid < MAX_PLAYERS && ari_FcObject[playerid] != -1;
}

stock bool:Ariane_IsFreecamRespawn(playerid)
{
    return playerid >= 0 && playerid < MAX_PLAYERS && ari_FcRespawn[playerid];
}

stock Ariane_SetFreecamSpeed(Float:metresPerSecond)
{
    ari_FcSpeed = metresPerSecond > 0.5 ? metresPerSecond : 0.5;
}

stock bool:Ariane_GetFreecamPos(playerid, &Float:x, &Float:y, &Float:z)
{
    if (!Ariane_IsInFreecam(playerid)) return false;
    GetPlayerObjectPos(playerid, ari_FcObject[playerid], x, y, z);
    return true;
}

stock bool:Ariane_FreecamStart(playerid)
{
    if (!IsPlayerConnected(playerid) || Ariane_IsInFreecam(playerid)) return false;
    new Float:x, Float:y, Float:z;
    GetPlayerCameraPos(playerid, x, y, z);
    GetPlayerPos(playerid, ari_FcBack[playerid][0], ari_FcBack[playerid][1], ari_FcBack[playerid][2]);
    GetPlayerFacingAngle(playerid, ari_FcBack[playerid][3]);
    ari_FcBackInterior[playerid] = GetPlayerInterior(playerid);
    ari_FcBackWorld[playerid] = GetPlayerVirtualWorld(playerid);
    #if defined GetSpawnInfo
        GetSpawnInfo(playerid, ari_FcSpawnTeam[playerid], ari_FcSpawnSkin[playerid],
            ari_FcSpawn[playerid][0], ari_FcSpawn[playerid][1], ari_FcSpawn[playerid][2], ari_FcSpawn[playerid][3]);
    #endif
    new obj = CreatePlayerObject(playerid, 19300, x, y, z, 0.0, 0.0, 0.0);
    if (obj == INVALID_OBJECT_ID) return false;
    ari_FcObject[playerid] = obj;
    ari_FcMoving[playerid] = false;
    TogglePlayerSpectating(playerid, true);
    AttachCameraToPlayerObject(playerid, obj);
    Streamer_ToggleCameraUpdate(playerid, 1);	// stream objects around the camera, not the parked player
    if (ari_FcTimer == -1) ari_FcTimer = SetTimer("Ariane_OnFreecamTimer", 50, true);
    CallLocalFunction("OnArianeFreecam", "ii", playerid, 1);
    return true;
}

static stock ari_FreecamEnd(playerid)
{
    if (ari_FcObject[playerid] != -1) DestroyPlayerObject(playerid, ari_FcObject[playerid]);
    ari_FcObject[playerid] = -1;
    ari_FcMoving[playerid] = false;
    new any = false;
    for (new i = 0; i < MAX_PLAYERS; i++) if (ari_FcObject[i] != -1) { any = true; break; }
    if (!any && ari_FcTimer != -1)
    {
        KillTimer(ari_FcTimer);
        ari_FcTimer = -1;
    }
}

stock bool:Ariane_FreecamStop(playerid, bool:landHere = false)
{
    if (!Ariane_IsInFreecam(playerid)) return false;
    new Float:x = ari_FcBack[playerid][0], Float:y = ari_FcBack[playerid][1], Float:z = ari_FcBack[playerid][2];
    if (landHere) GetPlayerObjectPos(playerid, ari_FcObject[playerid], x, y, z);
    ari_FreecamEnd(playerid);
    Streamer_ToggleCameraUpdate(playerid, 0);
    ari_FcBack[playerid][0] = x; ari_FcBack[playerid][1] = y; ari_FcBack[playerid][2] = z;
    ari_FcRespawn[playerid] = true;
    // leaving spectate respawns the player: spawn them where they should land
    SetSpawnInfo(playerid, GetPlayerTeam(playerid), GetPlayerSkin(playerid), x, y, z, ari_FcBack[playerid][3]);
    Streamer_UpdateEx(playerid, x, y, z, ari_FcBackWorld[playerid], ari_FcBackInterior[playerid]);
    TogglePlayerSpectating(playerid, false);
    CallLocalFunction("OnArianeFreecam", "ii", playerid, 0);
    return true;
}

// the spawn that ends a freecam: put the player where they belong, restore their spawn point
static stock ari_FreecamSpawned(playerid)
{
    if (!ari_FcRespawn[playerid]) return;
    SetPlayerInterior(playerid, ari_FcBackInterior[playerid]);
    SetPlayerVirtualWorld(playerid, ari_FcBackWorld[playerid]);
    SetPlayerPos(playerid, ari_FcBack[playerid][0], ari_FcBack[playerid][1], ari_FcBack[playerid][2]);
    SetPlayerFacingAngle(playerid, ari_FcBack[playerid][3]);
    SetCameraBehindPlayer(playerid);
    #if defined GetSpawnInfo
        SetSpawnInfo(playerid, ari_FcSpawnTeam[playerid], ari_FcSpawnSkin[playerid],
            ari_FcSpawn[playerid][0], ari_FcSpawn[playerid][1], ari_FcSpawn[playerid][2], ari_FcSpawn[playerid][3]);
    #endif
}

public Ariane_OnFreecamTimer()
{
    for (new i = 0; i < MAX_PLAYERS; i++)
    {
        new obj = ari_FcObject[i];
        if (obj == -1) continue;
        if (!IsPlayerConnected(i)) { ari_FcObject[i] = -1; continue; }
        new ARI_KEYTAG keys, ud, lr;
        GetPlayerKeys(i, keys, ud, lr);
        new Float:fx, Float:fy, Float:fz;
        GetPlayerCameraFrontVector(i, fx, fy, fz);
        new Float:dx = 0.0, Float:dy = 0.0, Float:dz = 0.0;
        if (ud < 0) { dx += fx; dy += fy; dz += fz; }
        else if (ud > 0) { dx -= fx; dy -= fy; dz -= fz; }
        if (lr < 0) { dx -= fy; dy += fx; }          // left
        else if (lr > 0) { dx += fy; dy -= fx; }     // right
        if (keys & KEY_JUMP) dz += 1.0;
        if (keys & KEY_CROUCH) dz -= 1.0;
        new Float:len = floatsqroot(dx * dx + dy * dy + dz * dz);
        if (len < 0.01)
        {
            if (ari_FcMoving[i]) StopPlayerObject(i, obj);
            ari_FcMoving[i] = false;
            continue;
        }
        new Float:speed = ari_FcSpeed;
        if (keys & KEY_SPRINT) speed *= 4.0;
        if (keys & KEY_WALK) speed *= 0.25;
        new Float:x, Float:y, Float:z;
        GetPlayerObjectPos(i, obj, x, y, z);
        MovePlayerObject(i, obj, x + dx / len * 1000.0, y + dy / len * 1000.0, z + dz / len * 1000.0, speed);
        ari_FcMoving[i] = true;
    }
    return 1;
}

// ---------------------------------------------------------------- queries and settings

stock Ariane_SetPollInterval(ms)
{
    ari_PollMs = ms < 250 ? 250 : ms;
    if (ari_PollTimer != -1)
    {
        KillTimer(ari_PollTimer);
        ari_PollTimer = SetTimer("Ariane_OnPollTimer", ari_PollMs, true);
    }
}

stock Ariane_SetDrawDistance(Float:distance) { ari_DrawDistance = distance; }
stock Ariane_SetLiveWorld(worldid) { ari_LiveWorld = worldid; }
stock Ariane_EnableLive(bool:enable) { ari_Live = enable; }
stock Ariane_EnableShowInGame(bool:enable) { ari_ShowInGame = enable; }

stock Ariane_GetObjectCount() { return ari_ObjectCount; }

stock STREAMER_TAG_OBJECT:Ariane_GetObject(index)
{
    if (index < 0 || index >= ari_ObjectCount) return STREAMER_TAG_OBJECT:INVALID_STREAMER_ID;
    return ari_Objects[index];
}

stock Ariane_GetObjectModel(index)
{
    if (index < 0 || index >= ari_ObjectCount) return 0;
    return ari_ObjectModel[index];
}

stock Ariane_GetObjectIndex(STREAMER_TAG_OBJECT:objectid)
{
    for (new i = 0; i < ari_ObjectCount; i++)
        if (ari_Objects[i] == objectid) return i;
    return -1;
}

stock bool:Ariane_IsMapObject(STREAMER_TAG_OBJECT:objectid)
{
    return Ariane_GetObjectIndex(objectid) != -1;
}

stock Ariane_GetRemovalCount() { return ari_RemovalCount + ari_CodeRemovalCount; }

stock bool:Ariane_GetRemoval(index, &modelid, &Float:x, &Float:y, &Float:z, &Float:radius)
{
    if (index < 0) return false;
    if (index < ari_CodeRemovalCount)
    {
        modelid = ari_CodeRemovals[index][ariModel];
        x = ari_CodeRemovals[index][ariX]; y = ari_CodeRemovals[index][ariY]; z = ari_CodeRemovals[index][ariZ];
        radius = ari_CodeRemovals[index][ariRadius];
        return true;
    }
    index -= ari_CodeRemovalCount;
    if (index >= ari_RemovalCount) return false;
    modelid = ari_Removals[index][ariModel];
    x = ari_Removals[index][ariX]; y = ari_Removals[index][ariY]; z = ari_Removals[index][ariZ];
    radius = ari_Removals[index][ariRadius];
    return true;
}

stock Ariane_GetRevision(dest[], size = sizeof dest)
{
    format(dest, size, "%s", ari_Rev);
}

// ---------------------------------------------------------------- wrapper export functions

stock STREAMER_TAG_OBJECT:Ariane_CreateObject(modelid, Float:x, Float:y, Float:z, Float:rx, Float:ry, Float:rz, worldid = -1, interiorid = -1)
{
    return CreateDynamicObject(modelid, x, y, z, rx, ry, rz, worldid, interiorid == 0 ? -1 : interiorid, -1, ari_DrawDistance, ari_DrawDistance);
}

stock Ariane_RemoveBuilding(modelid, Float:x, Float:y, Float:z, Float:radius)
{
    if (ari_CodeRemovalCount >= ARIANE_MAX_REMOVALS) return 0;
    new r = ari_CodeRemovalCount++;
    ari_CodeRemovals[r][ariModel] = modelid;
    ari_CodeRemovals[r][ariX] = x;
    ari_CodeRemovals[r][ariY] = y;
    ari_CodeRemovals[r][ariZ] = z;
    ari_CodeRemovals[r][ariRadius] = radius;
    for (new i = 0; i < MAX_PLAYERS; i++)
        if (IsPlayerConnected(i)) RemoveBuildingForPlayer(i, modelid, x, y, z, radius);
    return 1;
}

stock Ariane_SetObjectMaterial(STREAMER_TAG_OBJECT:objectid, index, modelid, const txd[], const texture[], color = 0)
{
    return SetDynamicObjectMaterial(objectid, index, modelid, txd, texture, color);
}

stock Ariane_SetObjectMaterialText(STREAMER_TAG_OBJECT:objectid, index, const text[], size, const font[], fontsize, bold, fontcolor, backcolor, align)
{
    return SetDynamicObjectMaterialText(objectid, index, text, size, font, fontsize, bold, fontcolor, backcolor, align);
}

// ---------------------------------------------------------------- start / stop

stock Ariane_Start()
{
    if (ari_Started) return 0;
    ari_Started = true;
    ari_LoadLive(true);
    // a Show in game request written before the server started is not acted on
    new line[160];
    ari_ReadGoto(line, sizeof line, ari_GotoSeq, sizeof ari_GotoSeq);
    ari_PollTimer = SetTimer("Ariane_OnPollTimer", ari_PollMs, true);
    ari_GotoTimer = SetTimer("Ariane_OnGotoTimer", ARIANE_GOTO_MS, true);
    print("[ariane] watching scriptfiles/" ARIANE_LIVE_FILE " and " ARIANE_GOTO_FILE);
    return 1;
}

stock Ariane_Stop()
{
    if (!ari_Started) return 0;
    ari_Started = false;
    if (ari_PollTimer != -1) KillTimer(ari_PollTimer);
    if (ari_GotoTimer != -1) KillTimer(ari_GotoTimer);
    ari_PollTimer = ari_GotoTimer = -1;
    Ariane_ClearLive();
    return 1;
}

stock Ariane_PlayerConnected(playerid)
{
    if (playerid >= 0 && playerid < MAX_PLAYERS)
    {
        ari_FcObject[playerid] = -1;
        ari_FcRespawn[playerid] = false;
    }
    Ariane_ApplyRemovals(playerid);
}

// ARIANE_NO_HOOKS: call from OnPlayerSpawn (first thing) and OnPlayerDisconnect
stock Ariane_PlayerSpawned(playerid)
{
    if (playerid < 0 || playerid >= MAX_PLAYERS) return;
    ari_FreecamSpawned(playerid);
}

stock Ariane_PlayerSpawnDone(playerid)
{
    if (playerid >= 0 && playerid < MAX_PLAYERS) ari_FcRespawn[playerid] = false;
}

stock Ariane_PlayerDisconnected(playerid)
{
    if (playerid < 0 || playerid >= MAX_PLAYERS) return;
    if (ari_FcObject[playerid] != -1) ari_FreecamEnd(playerid);
    ari_FcRespawn[playerid] = false;
}

// ---------------------------------------------------------------- hooks (ALS)

#if !defined ARIANE_NO_HOOKS

#if defined FILTERSCRIPT
public OnFilterScriptInit()
{
    Ariane_Start();
    #if defined Ariane_OnFilterScriptInit
        return Ariane_OnFilterScriptInit();
    #else
        return 1;
    #endif
}
#if defined _ALS_OnFilterScriptInit
    #undef OnFilterScriptInit
#else
    #define _ALS_OnFilterScriptInit
#endif
#define OnFilterScriptInit Ariane_OnFilterScriptInit
#if defined Ariane_OnFilterScriptInit
    forward Ariane_OnFilterScriptInit();
#endif

public OnFilterScriptExit()
{
    Ariane_Stop();
    #if defined Ariane_OnFilterScriptExit
        return Ariane_OnFilterScriptExit();
    #else
        return 1;
    #endif
}
#if defined _ALS_OnFilterScriptExit
    #undef OnFilterScriptExit
#else
    #define _ALS_OnFilterScriptExit
#endif
#define OnFilterScriptExit Ariane_OnFilterScriptExit
#if defined Ariane_OnFilterScriptExit
    forward Ariane_OnFilterScriptExit();
#endif

#else

public OnGameModeInit()
{
    Ariane_Start();
    #if defined Ariane_OnGameModeInit
        return Ariane_OnGameModeInit();
    #else
        return 1;
    #endif
}
#if defined _ALS_OnGameModeInit
    #undef OnGameModeInit
#else
    #define _ALS_OnGameModeInit
#endif
#define OnGameModeInit Ariane_OnGameModeInit
#if defined Ariane_OnGameModeInit
    forward Ariane_OnGameModeInit();
#endif

public OnGameModeExit()
{
    Ariane_Stop();
    #if defined Ariane_OnGameModeExit
        return Ariane_OnGameModeExit();
    #else
        return 1;
    #endif
}
#if defined _ALS_OnGameModeExit
    #undef OnGameModeExit
#else
    #define _ALS_OnGameModeExit
#endif
#define OnGameModeExit Ariane_OnGameModeExit
#if defined Ariane_OnGameModeExit
    forward Ariane_OnGameModeExit();
#endif

#endif

public OnPlayerConnect(playerid)
{
    Ariane_PlayerConnected(playerid);
    #if defined Ariane_OnPlayerConnect
        return Ariane_OnPlayerConnect(playerid);
    #else
        return 1;
    #endif
}
#if defined _ALS_OnPlayerConnect
    #undef OnPlayerConnect
#else
    #define _ALS_OnPlayerConnect
#endif
#define OnPlayerConnect Ariane_OnPlayerConnect
#if defined Ariane_OnPlayerConnect
    forward Ariane_OnPlayerConnect(playerid);
#endif

public OnPlayerSpawn(playerid)
{
    Ariane_PlayerSpawned(playerid);
    #if defined Ariane_OnPlayerSpawn
        new ret = Ariane_OnPlayerSpawn(playerid);
        Ariane_PlayerSpawnDone(playerid);
        return ret;
    #else
        Ariane_PlayerSpawnDone(playerid);
        return 1;
    #endif
}
#if defined _ALS_OnPlayerSpawn
    #undef OnPlayerSpawn
#else
    #define _ALS_OnPlayerSpawn
#endif
#define OnPlayerSpawn Ariane_OnPlayerSpawn
#if defined Ariane_OnPlayerSpawn
    forward Ariane_OnPlayerSpawn(playerid);
#endif

public OnPlayerDisconnect(playerid, reason)
{
    Ariane_PlayerDisconnected(playerid);
    #if defined Ariane_OnPlayerDisconnect
        return Ariane_OnPlayerDisconnect(playerid, reason);
    #else
        return 1;
    #endif
}
#if defined _ALS_OnPlayerDisconnect
    #undef OnPlayerDisconnect
#else
    #define _ALS_OnPlayerDisconnect
#endif
#define OnPlayerDisconnect Ariane_OnPlayerDisconnect
#if defined Ariane_OnPlayerDisconnect
    forward Ariane_OnPlayerDisconnect(playerid, reason);
#endif

#endif
