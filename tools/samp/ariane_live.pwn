/*
    Ariane live link - filterscript for any open.mp / SA-MP server.

    Ariane (SA-MP edition) "Sync to server" writes scriptfiles/ariane_live.txt.
    This filterscript polls that file and rebuilds the map whenever it changes,
    so edits made in Ariane appear on the running server within a few seconds.

    Setup
      1. Copy this file to filterscripts/, compile it (needs the streamer plugin
         include), add "ariane_live" to the filterscripts list in config.json
         (open.mp) or server.cfg (SA-MP), and load the streamer plugin.
      2. In Ariane: SA-MP > Server folder... (the folder with scriptfiles/).
      3. SA-MP > Sync to server (Ctrl+Shift+S).

    File format (one entry per line)
      rev <number>                                            changes on every sync
      R <model> <x> <y> <z> <radius>                          RemoveBuildingForPlayer
      O <model> <x> <y> <z> <rx> <ry> <rz> <world> <interior> object (model < 0 = custom model)
      M <index> <model> <txd> <texture> <ARGB>                material of the object above
      T <index> <size> <fontsize> <bold> <fontARGB> <backARGB> <align> <font> <text>
                                                              material text of the object above
                                                              (font: '_' = space, text: \n = new line)

    Removed buildings can't be restored for players already connected: a
    building you bring back reappears for them after they reconnect.
*/

#define FILTERSCRIPT

#include <open.mp>
#include <streamer>

#define AL_FILE             "ariane_live.txt"
#define AL_POLL_MS          (3000)
#define AL_MAX_OBJECTS      (10000)
#define AL_MAX_REMOVALS     (2000)
#define AL_DRAW_DISTANCE    (300.0)

new gRev[24];
new gObjects[AL_MAX_OBJECTS];
new gObjectCount;
new gPollTimer = -1;

enum E_REMOVAL { rModel, Float:rX, Float:rY, Float:rZ, Float:rRadius }
new gRemovals[AL_MAX_REMOVALS][E_REMOVAL];
new gRemovalCount;

// ---------------------------------------------------------------- line parsing

// Copies the idx-th space separated word of src into dest.
static bool:AL_Word(const src[], idx, dest[], size = sizeof (dest))
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
static bool:AL_Rest(const src[], idx, dest[], size = sizeof (dest))
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

static AL_Int(const line[], idx)
{
    new tmp[16];
    AL_Word(line, idx, tmp);
    return strval(tmp);
}

static Float:AL_Float(const line[], idx)
{
    new tmp[24];
    AL_Word(line, idx, tmp);
    return floatstr(tmp);
}

// "0xAARRGGBB" or a decimal number
static AL_Hex(const line[], idx)
{
    new tmp[16];
    if (!AL_Word(line, idx, tmp)) return 0;
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

static AL_Strip(line[])
{
    new len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = '\0';
}

// ---------------------------------------------------------------- map

static AL_ApplyRemovals(playerid)
{
    for (new i = 0; i < gRemovalCount; i++)
        RemoveBuildingForPlayer(playerid, gRemovals[i][rModel], gRemovals[i][rX], gRemovals[i][rY], gRemovals[i][rZ], gRemovals[i][rRadius]);
}

static AL_Clear()
{
    for (new i = 0; i < gObjectCount; i++)
        if (IsValidDynamicObject(gObjects[i])) DestroyDynamicObject(gObjects[i]);
    gObjectCount = 0;
    gRemovalCount = 0;
}

forward AL_Poll();
public AL_Poll()
{
    if (!fexist(AL_FILE)) return 1;
    new File:f = fopen(AL_FILE, io_read);
    if (!f) return 1;

    new line[512], rev[24];
    if (!fread(f, line))
    {
        fclose(f);
        return 1;
    }
    AL_Strip(line);
    AL_Word(line, 1, rev);
    if (strcmp(line, "rev ", false, 4) || rev[0] == '\0' || !strcmp(rev, gRev))
    {
        fclose(f);
        return 1;
    }
    format(gRev, sizeof (gRev), "%s", rev);

    AL_Clear();
    new skipped = 0, materials = 0, last = INVALID_STREAMER_ID;
    while (fread(f, line))
    {
        AL_Strip(line);
        if (line[1] != ' ') continue;
        switch (line[0])
        {
            case 'O':
            {
                last = INVALID_STREAMER_ID;
                new model = AL_Int(line, 1);
                if (model == 0 || gObjectCount >= AL_MAX_OBJECTS) { skipped++; continue; }
                new interior = AL_Int(line, 9);
                last = CreateDynamicObject(model, AL_Float(line, 2), AL_Float(line, 3), AL_Float(line, 4),
                    AL_Float(line, 5), AL_Float(line, 6), AL_Float(line, 7),
                    AL_Int(line, 8), interior == 0 ? -1 : interior, -1, AL_DRAW_DISTANCE, AL_DRAW_DISTANCE);
                gObjects[gObjectCount++] = last;
            }
            case 'M':
            {
                if (last == INVALID_STREAMER_ID) { skipped++; continue; }
                new txd[32], texture[64];
                AL_Word(line, 3, txd);
                AL_Word(line, 4, texture);
                SetDynamicObjectMaterial(last, AL_Int(line, 1), AL_Int(line, 2), txd, texture, AL_Hex(line, 5));
                materials++;
            }
            case 'T':
            {
                if (last == INVALID_STREAMER_ID) { skipped++; continue; }
                new font[32], raw[256], text[256], n = 0;
                AL_Word(line, 8, font);
                for (new i = 0; font[i] != '\0'; i++) if (font[i] == '_') font[i] = ' ';
                AL_Rest(line, 9, raw);
                for (new i = 0; raw[i] != '\0' && n < sizeof (text) - 1; i++)
                {
                    if (raw[i] == '\\' && raw[i + 1] == 'n') { text[n++] = '\n'; i++; }
                    else if (raw[i] == '\\' && raw[i + 1] == '\\') { text[n++] = '\\'; i++; }
                    else text[n++] = raw[i];
                }
                text[n] = '\0';
                SetDynamicObjectMaterialText(last, AL_Int(line, 1), text, AL_Int(line, 2), font, AL_Int(line, 3),
                    AL_Int(line, 4), AL_Hex(line, 5), AL_Hex(line, 6), AL_Int(line, 7));
                materials++;
            }
            case 'R':
            {
                if (gRemovalCount >= AL_MAX_REMOVALS) { skipped++; continue; }
                new r = gRemovalCount++;
                gRemovals[r][rModel] = AL_Int(line, 1);
                gRemovals[r][rX] = AL_Float(line, 2);
                gRemovals[r][rY] = AL_Float(line, 3);
                gRemovals[r][rZ] = AL_Float(line, 4);
                gRemovals[r][rRadius] = AL_Float(line, 5);
            }
        }
    }
    fclose(f);

    for (new i = 0; i < MAX_PLAYERS; i++)
        if (IsPlayerConnected(i)) AL_ApplyRemovals(i);

    printf("[ariane_live] map updated: %d objects (%d materials), %d removed buildings%s",
        gObjectCount, materials, gRemovalCount, skipped ? (" - some lines skipped (limits or unknown models)") : (""));
    return 1;
}

// ---------------------------------------------------------------- callbacks

public OnFilterScriptInit()
{
    gPollTimer = SetTimer("AL_Poll", AL_POLL_MS, true);
    AL_Poll();
    print("[ariane_live] watching scriptfiles/" AL_FILE);
    return 1;
}

public OnFilterScriptExit()
{
    if (gPollTimer != -1) KillTimer(gPollTimer);
    AL_Clear();
    return 1;
}

public OnPlayerConnect(playerid)
{
    AL_ApplyRemovals(playerid);
    return 1;
}
