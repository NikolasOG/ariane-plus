// SA-MP edition of Ariane
//
// Adds an "SA-MP layer" on top of the normal map editor:
//  - objects placed while SA-MP mode is on go into the layer, never into game IPLs
//  - deleting (or moving) a stock building becomes RemoveBuildingForPlayer on export
//  - import CreateObject / CreateDynamicObject / RemoveBuildingForPlayer code
//    (.pwn / .inc / .txt) and MTA:SA .map files
//  - export Pawn: streamer (CreateDynamicObject), native open.mp / SA-MP
//    (CreateObject) or your own wrapper functions (<prefix>CreateObject...)
//  - "Sync to server" writes scriptfiles/ariane_live.txt, which a gamemode or
//    filterscript polls and rebuilds live (see tools/samp/ariane_live.pwn)
//
// SA-MP object rotation is GTA's CMatrix::SetRotate(x, y, z) order:
//   M = Rz(rz) * Rx(rx) * Ry(ry), columns = right, forward, up.

#include "euryopa.h"
#include <string>
#include <vector>
#include <map>
#include <math.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

// ------------------------------------------------------------------ state

bool gSampProtectGameFiles = true;
bool gSampAllowWorldRemoval;			// pick world buildings outside the opened IPL, to remove them
static bool gSampAskWorldRemoval = true;	// confirm before removing world buildings
static bool gOpenConfirmWorldRemoval;
// Show in game: which players the server moves (written into scriptfiles/ariane_goto.txt)
enum { GOTO_RCON, GOTO_SCRIPT, GOTO_PLAYER, GOTO_ALL };
static int gSampGotoWho = GOTO_RCON;
static char gSampGotoPlayer[32];
static const char *kGotoModes[] = { "rcon", "script", "player", "all" };
// server -> editor: a script calls Ariane_FocusEditor(At) (ariane.inc), Ariane moves its camera there
static bool gSampFollowFocus = true;
// SA-MP layer objects draw at least this far (0 = the model's own draw distance). 300 m is the streamer
// plugin's default stream distance, so the map looks the way a player near it sees it.
float gSampLayerDrawDist = 300.0f;

static GameFile *gSampFile;
static int gSampIplCounter;
static char gSampServerDir[1024];
static char gSampMapName[64] = "ariane_map";
enum { SAMP_FORMAT_WRAPPER, SAMP_FORMAT_STREAMER, SAMP_FORMAT_NATIVE };
static int gSampExportFormat = SAMP_FORMAT_STREAMER;
static char gSampWrapPrefix[24] = "Map_";	// wrapper format: <prefix>CreateObject / RemoveBuilding / SetObjectMaterial
static bool gSampAutoInc = true;	// also write <server>/<incdir>/<name>.inc on sync
static char gSampIncDir[256] = "gamemodes/maps";
static int gRotMode = -1;		// how a matrix quaternion maps onto ObjectInst::m_rotation
static bool gSettingsLoaded;
static bool gOpenExport, gOpenServer;

// SetDynamicObjectMaterial / SetDynamicObjectMaterialText on one material slot
struct SampMaterial
{
	int index;
	bool text;
	int model;			// texture: model id SA-MP loads the txd through (the txd name wins here)
	std::string txd, tex;
	uint32 color;			// ARGB, 0 = keep the model's colour
	std::string str, font;		// text
	int size, fontSize, bold, align;
	uint32 fontColor, backColor;
};

struct SampRawObject { int model; float x, y, z, rx, ry, rz; int world, interior; std::vector<SampMaterial> mats; };
struct SampRawRemoval { int model; float x, y, z, radius; };

// objects Ariane can't render (SA-MP-only models) and removals that matched nothing:
// kept so export doesn't silently drop them
static std::vector<SampRawObject> gRawObjects;
static std::vector<SampRawRemoval> gRawRemovals;
static std::map<ObjectInst*, int> gInstWorld;	// virtual world per layer object (-1 = all)
static std::map<ObjectInst*, std::vector<SampMaterial>> gInstMaterials;	// material overrides per layer object

static const float DEG2RAD = 3.14159265358979f / 180.0f;
static void LoadSettings(void);

// ------------------------------------------------------------------ material overrides
//
// Texture overrides are drawn by swapping the material's texture and colour
// around the instance's render call (materials are shared by every instance of
// a model). Text materials are drawn as their background colour - the text
// itself is only rendered in game.

static void
PutMaterial(std::vector<SampMaterial> &list, const SampMaterial &m)
{
	for(SampMaterial &e : list)
		if(e.index == m.index){ e = m; return; }
	list.push_back(m);
}

static std::map<std::string, rw::Texture*> gMatTexCache;

static rw::Texture*
ResolveMaterialTexture(const SampMaterial &m)
{
	if(m.text || m.tex.empty() || rw::strncmp_ci(m.tex.c_str(), "none", 5) == 0)
		return nil;
	std::string key = m.txd + "/" + m.tex + "/" + std::to_string(m.model);
	std::map<std::string, rw::Texture*>::iterator it = gMatTexCache.find(key);
	if(it != gMatTexCache.end())
		return it->second;
	rw::Texture *t = nil;
	int slot = m.model < 0 ? -1 : FindTxdSlot(m.txd.c_str());
	if(slot < 0){
		ObjectDef *def = GetObjectDef(SampResolveModel(m.model));
		if(def) slot = def->m_txdSlot;
	}
	if(slot >= 0){
		if(!IsTxdLoaded(slot))
			LoadTxd(slot);
		t = FindTxdTexture(slot, m.tex.c_str());
	}
	if(t == nil)
		log("SA-MP material: texture %s/%s (model %d) not found\n", m.txd.c_str(), m.tex.c_str(), m.model);
	gMatTexCache[key] = t;
	return t;
}

struct SampMatBackup { rw::Material *mat; rw::Texture *tex; rw::RGBA color; };
struct SampGeoBackup { rw::Geometry *geo; uint32 flags; };
static std::vector<SampMatBackup> gMatBackups;
static std::vector<SampGeoBackup> gGeoBackups;

static void
ApplyMaterialsToGeometry(rw::Geometry *g, const std::vector<SampMaterial> &mats)
{
	if(g == nil)
		return;
	bool colour = false;
	for(const SampMaterial &m : mats){
		if(m.index < 0 || m.index >= g->matList.numMaterials)
			continue;
		rw::Material *mat = g->matList.materials[m.index];
		if(mat == nil)
			continue;
		SampMatBackup b = { mat, mat->texture, mat->color };
		gMatBackups.push_back(b);
		uint32 c = m.color;
		if(m.text){
			mat->texture = nil;
			// transparent background: show a faint panel in the font colour
			c = (m.backColor >> 24) ? m.backColor : ((m.fontColor & 0x00FFFFFF) | 0x80000000);
		}else{
			rw::Texture *t = ResolveMaterialTexture(m);
			if(t) mat->texture = t;
		}
		if(c != 0){
			mat->color.red = c >> 16 & 0xFF;
			mat->color.green = c >> 8 & 0xFF;
			mat->color.blue = c & 0xFF;
			mat->color.alpha = c >> 24 & 0xFF;
			colour = true;
		}
	}
	if(colour){
		SampGeoBackup gb = { g, g->flags };
		gGeoBackups.push_back(gb);
		g->flags |= rw::Geometry::MODULATE;
	}
}

void
SampPushInstMaterials(ObjectInst *inst)
{
	if(gInstMaterials.empty() || inst == nil || inst->m_rwObject == nil)
		return;
	std::map<ObjectInst*, std::vector<SampMaterial>>::iterator it = gInstMaterials.find(inst);
	if(it == gInstMaterials.end() || it->second.empty())
		return;
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	if(obj == nil)
		return;
	if(obj->m_type == ObjectDef::ATOMIC)
		ApplyMaterialsToGeometry(((rw::Atomic*)inst->m_rwObject)->geometry, it->second);
	else{
		rw::Clump *clump = (rw::Clump*)inst->m_rwObject;
		FORLIST(lnk, clump->atomics)
			ApplyMaterialsToGeometry(rw::Atomic::fromClump(lnk)->geometry, it->second);
	}
}

void
SampPopInstMaterials(void)
{
	for(size_t i = gMatBackups.size(); i-- > 0; ){
		gMatBackups[i].mat->texture = gMatBackups[i].tex;
		gMatBackups[i].mat->color = gMatBackups[i].color;
	}
	for(size_t i = gGeoBackups.size(); i-- > 0; )
		gGeoBackups[i].geo->flags = gGeoBackups[i].flags;
	gMatBackups.clear();
	gGeoBackups.clear();
}

static std::string
JsonStr(const std::string &s)
{
	std::string out = "\"";
	for(char c : s){
		if(c == '"' || c == '\\'){ out += '\\'; out += c; }
		else if(c == '\n') out += "\\n";
		else if((unsigned char)c < 0x20) out += ' ';
		else out += c;
	}
	return out + "\"";
}

static std::string
MaterialJson(const SampMaterial &m)
{
	char b[160];
	std::string s;
	if(m.text){
		snprintf(b, sizeof(b), "{\"index\":%d,\"kind\":\"text\",\"size\":%d,\"font_size\":%d,\"bold\":%d,"
			"\"font_color\":\"0x%08X\",\"back_color\":\"0x%08X\",\"align\":%d,\"font\":",
			m.index, m.size, m.fontSize, m.bold, m.fontColor, m.backColor, m.align);
		s = b + JsonStr(m.font) + ",\"text\":" + JsonStr(m.str) + "}";
	}else{
		snprintf(b, sizeof(b), "{\"index\":%d,\"kind\":\"texture\",\"model\":%d,\"color\":\"0x%08X\",\"txd\":",
			m.index, m.model, m.color);
		s = b + JsonStr(m.txd) + ",\"texture\":" + JsonStr(m.tex) + "}";
	}
	return s;
}

std::string
SampMaterialsJson(ObjectInst *inst)
{
	std::map<ObjectInst*, std::vector<SampMaterial>>::iterator it = gInstMaterials.find(inst);
	if(it == gInstMaterials.end() || it->second.empty())
		return "";
	std::string s = "[";
	for(size_t i = 0; i < it->second.size(); i++){
		if(i) s += ",";
		s += MaterialJson(it->second[i]);
	}
	return s + "]";
}

bool
SampSetMaterial(ObjectInst *inst, int index, int model, const char *txd, const char *tex, uint32 color)
{
	if(inst == nil || index < 0 || index > 15)
		return false;
	SampMaterial m = {};
	m.index = index; m.text = false; m.model = model; m.txd = txd ? txd : "none"; m.tex = tex ? tex : "none";
	m.color = color;
	PutMaterial(gInstMaterials[inst], m);
	inst->m_isDirty = true;
	return true;
}

bool
SampSetMaterialText(ObjectInst *inst, int index, const char *text, int size, const char *font, int fontSize,
	int bold, uint32 fontColor, uint32 backColor, int align)
{
	if(inst == nil || index < 0 || index > 15)
		return false;
	SampMaterial m = {};
	m.index = index; m.text = true; m.str = text ? text : ""; m.size = size; m.font = font && font[0] ? font : "Arial";
	m.fontSize = fontSize; m.bold = bold; m.fontColor = fontColor; m.backColor = backColor; m.align = align;
	PutMaterial(gInstMaterials[inst], m);
	inst->m_isDirty = true;
	return true;
}

int
SampClearMaterials(ObjectInst *inst, int index)
{
	std::map<ObjectInst*, std::vector<SampMaterial>>::iterator it = gInstMaterials.find(inst);
	if(it == gInstMaterials.end())
		return 0;
	int n = 0;
	std::vector<SampMaterial> &list = it->second;
	for(size_t i = list.size(); i-- > 0; )
		if(index < 0 || list[i].index == index){ list.erase(list.begin() + i); n++; }
	if(list.empty())
		gInstMaterials.erase(it);
	return n;
}

bool
SampCopyMaterials(ObjectInst *from, ObjectInst *to)
{
	std::map<ObjectInst*, std::vector<SampMaterial>>::iterator it = gInstMaterials.find(from);
	if(it == gInstMaterials.end() || to == nil)
		return false;
	gInstMaterials[to] = it->second;
	return true;
}

// Material slots of a model: index, texture, colour - plus every texture of its txd
std::string
SampModelMaterialsJson(int model)
{
	ObjectDef *obj = GetObjectDef(model);
	if(obj == nil)
		return "";
	RequestObject(model);
	LoadAllRequestedObjects();
	TxdDef *td = GetTxdDef(obj->m_txdSlot);
	std::string s = "\"model\":" + std::to_string(model) + ",\"name\":" + JsonStr(obj->m_name) +
		",\"txd\":" + JsonStr(td ? td->name : "") + ",\"materials\":[";
	int n = 0;
	auto addGeometry = [&](rw::Geometry *g){
		if(g == nil) return;
		for(int i = 0; i < g->matList.numMaterials; i++){
			rw::Material *m = g->matList.materials[i];
			char b[200];
			snprintf(b, sizeof(b), "%s{\"index\":%d,\"color\":\"0x%02X%02X%02X%02X\",\"texture\":", n ? "," : "", i,
				m ? m->color.alpha : 0, m ? m->color.red : 0, m ? m->color.green : 0, m ? m->color.blue : 0);
			s += b;
			s += JsonStr(m && m->texture ? m->texture->name : "") + "}";
			n++;
		}
	};
	if(obj->m_type == ObjectDef::ATOMIC){
		if(obj->m_atomics[0]) addGeometry(obj->m_atomics[0]->geometry);
	}else if(obj->m_clump){
		FORLIST(lnk, obj->m_clump->atomics){
			addGeometry(rw::Atomic::fromClump(lnk)->geometry);
			break;		// SA-MP indexes the first geometry's materials
		}
	}
	s += "],\"txd_textures\":" + SampTxdTexturesJson(td ? td->name : "");
	return s;
}

std::string
SampTxdTexturesJson(const char *txdName)
{
	int slot = txdName && txdName[0] ? FindTxdSlot(txdName) : -1;
	if(slot < 0)
		return "null";
	if(!IsTxdLoaded(slot))
		LoadTxd(slot);
	TxdDef *td = GetTxdDef(slot);
	std::string s = "[";
	int n = 0;
	if(td && td->txd){
		FORLIST(lnk, td->txd->textures){
			rw::Texture *t = rw::Texture::fromDict(lnk);
			if(t == nil) continue;
			if(n++) s += ",";
			s += JsonStr(t->name);
		}
	}
	return s + "]";
}

// --- material text in files: escapes \n, \\ and keeps everything on one line
static std::string
EscapeLine(const std::string &s)
{
	std::string out;
	for(char c : s){
		if(c == '\\') out += "\\\\";
		else if(c == '\n') out += "\\n";
		else if(c == '\r') continue;
		else out += c;
	}
	return out;
}

static std::string
UnescapeLine(const char *s)
{
	std::string out;
	for(; *s; s++){
		if(*s == '\\' && s[1] == 'n'){ out += '\n'; s++; }
		else if(*s == '\\' && s[1] == '\\'){ out += '\\'; s++; }
		else out += *s;
	}
	return out;
}

static std::string
PawnString(const std::string &s)
{
	std::string out = "\"";
	for(char c : s){
		if(c == '"' || c == '\\'){ out += '\\'; out += c; }
		else if(c == '\n') out += "\\n";
		else out += c;
	}
	return out + "\"";
}

// live file lines: M <index> <model> <txd> <texture> <ARGB hex>
//                  T <index> <size> <fontsize> <bold> <fontcolor> <backcolor> <align> <font, '_' for spaces> <text>
static void
WriteMaterialLines(FILE *f, const std::vector<SampMaterial> &mats)
{
	for(const SampMaterial &m : mats){
		if(m.text){
			std::string font = m.font;
			for(char &c : font) if(c == ' ') c = '_';
			fprintf(f, "T %d %d %d %d 0x%08X 0x%08X %d %s %s\n", m.index, m.size, m.fontSize, m.bold,
				m.fontColor, m.backColor, m.align, font.c_str(), EscapeLine(m.str).c_str());
		}else
			fprintf(f, "M %d %d %s %s 0x%08X\n", m.index, m.model, m.txd.c_str(), m.tex.c_str(), m.color);
	}
}

static bool
ParseMaterialLine(const char *line, SampMaterial *m)
{
	*m = SampMaterial();
	if(line[0] == 'M' && line[1] == ' '){
		char txd[64], tex[96];
		unsigned int color;
		if(sscanf(line + 2, "%d %d %63s %95s %x", &m->index, &m->model, txd, tex, &color) != 5)
			return false;
		m->text = false; m->txd = txd; m->tex = tex; m->color = color;
		return true;
	}
	if(line[0] == 'T' && line[1] == ' '){
		char font[64];
		unsigned int fc, bc;
		int used = 0;
		if(sscanf(line + 2, "%d %d %d %d %x %x %d %63s %n", &m->index, &m->size, &m->fontSize, &m->bold,
		          &fc, &bc, &m->align, font, &used) < 8)
			return false;
		for(char *c = font; *c; c++) if(*c == '_') *c = ' ';
		std::string rest = used > 0 ? UnescapeLine(line + 2 + used) : "";
		while(!rest.empty() && (rest.back() == '\n' || rest.back() == '\r')) rest.pop_back();
		m->text = true; m->font = font; m->fontColor = fc; m->backColor = bc; m->str = rest;
		return true;
	}
	return false;
}

// ------------------------------------------------------------------ layer

GameFile*
SampGetLayerFile(void)
{
	if(gSampFile == nil){
		static char name[] = "samp_layer.ipl";
		gSampFile = NewGameFile(name);
	}
	return gSampFile;
}

bool
SampIsLayerFile(const char *name)
{
	return gSampFile && name && strcmp(name, gSampFile->name) == 0;
}

bool
SampIsLayerInst(ObjectInst *inst)
{
	return inst && gSampFile && inst->m_file == gSampFile;
}

bool
SampModeActive(void)
{
	return gSampProtectGameFiles;
}

bool
SampBlockGameSave(const char *action)
{
	if(!gSampProtectGameFiles)
		return false;
	Toast(TOAST_SAVE, "%s blocked: SA-MP mode protects your game files. Use SA-MP > Export or Sync.", action);
	return true;
}

// ------------------------------------------------------------------ SA-MP models
//
// The SA-MP client ships its extra objects (IDs 18631+) in <game>/SAMP/:
// SAMP.ide + SAMP.img (models/textures), SAMPCOL.img (collisions) and
// CUSTOM.ide + custom.img. The open.mp launcher copies them into the game
// folder. Loading them before InitCdImages() registers them like stock models.

static bool gSampModelsLoaded;

bool
SampModelsLoaded(void)
{
	return gSampModelsLoaded;
}

static bool
GameFileExists(const char *path)
{
	FILE *f = fopen_ci(path, "rb");
	if(f == nil)
		return false;
	fclose(f);
	return true;
}

static char gSampModelDir[1024];

// Where to find SAMP.ide/SAMP.img: the game folder first, then the open.mp
// launcher's own copy (%LOCALAPPDATA%\mp.open.launcher\samp\shared\SAMP).
static bool
FindSampModelDir(char *dir, size_t size)
{
	if(GameFileExists("SAMP/SAMP.ide") && GameFileExists("SAMP/SAMP.img")){
		snprintf(dir, size, "SAMP");
		return true;
	}
#ifdef _WIN32
	char local[MAX_PATH];
	DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof(local));
	if(n > 0 && n < sizeof(local)){
		char ide[1200], img[1200];
		snprintf(dir, size, "%s\\mp.open.launcher\\samp\\shared\\SAMP", local);
		snprintf(ide, sizeof(ide), "%s\\SAMP.ide", dir);
		snprintf(img, sizeof(img), "%s\\SAMP.img", dir);
		if(GameFileExists(ide) && GameFileExists(img))
			return true;
	}
#endif
	dir[0] = '\0';
	return false;
}

const char*
SampModelDir(void)
{
	return gSampModelDir;
}

void
SampLoadDefinitions(void)
{
	if(!isSA())
		return;
	if(!FindSampModelDir(gSampModelDir, sizeof(gSampModelDir))){
		log("SA-MP: SAMP.ide + SAMP.img not found (game SAMP/ folder or open.mp launcher folder) - "
		    "SA-MP models won't be drawn. Run the open.mp launcher once to download them.\n");
		return;
	}
	char path[1200];
	snprintf(path, sizeof(path), "%s\\SAMP.img", gSampModelDir);
	AddCdImage(path);
	snprintf(path, sizeof(path), "%s\\SAMPCOL.img", gSampModelDir);
	if(GameFileExists(path))
		AddCdImage(path);
	snprintf(path, sizeof(path), "%s\\custom.img", gSampModelDir);
	if(GameFileExists(path))
		AddCdImage(path);
	// Tag the definitions with their own file so the Object Browser's IDE tab
	// lists them as SAMP.ide, not as the last GTA IDE that was loaded.
	static char sampIdeName[] = "SAMP\\SAMP.ide";
	static char customIdeName[] = "SAMP\\CUSTOM.ide";
	snprintf(path, sizeof(path), "%s\\SAMP.ide", gSampModelDir);
	FileLoader::currentFile = NewGameFile(sampIdeName);
	FileLoader::LoadObjectTypes(path);
	snprintf(path, sizeof(path), "%s\\CUSTOM.ide", gSampModelDir);
	if(GameFileExists(path)){
		FileLoader::currentFile = NewGameFile(customIdeName);
		FileLoader::LoadObjectTypes(path);
	}
	gSampModelsLoaded = true;
	log("SA-MP: loaded SA-MP object definitions from %s\n", gSampModelDir);
}



// ------------------------------------------------------------------ rotation maths

static void
EulerToAxes(float rx, float ry, float rz, rw::V3d *right, rw::V3d *fwd, rw::V3d *up)
{
	float sx = sinf(rx*DEG2RAD), cx = cosf(rx*DEG2RAD);
	float sy = sinf(ry*DEG2RAD), cy = cosf(ry*DEG2RAD);
	float sz = sinf(rz*DEG2RAD), cz = cosf(rz*DEG2RAD);
	*right = rw::makeV3d(cz*cy - sz*sx*sy, cz*sx*sy + sz*cy, -sy*cx);
	*fwd   = rw::makeV3d(-sz*cx, cz*cx, sx);
	*up    = rw::makeV3d(sz*sx*cy + cz*sy, sz*sy - cz*sx*cy, cy*cx);
}

static float
wrapDeg(float a)
{
	while(a < 0.0f) a += 360.0f;
	while(a >= 360.0f) a -= 360.0f;
	if(fabsf(a - 360.0f) < 0.0005f || fabsf(a) < 0.0005f) a = 0.0f;
	return a;
}

static void
AxesToEuler(const rw::V3d &right, const rw::V3d &fwd, const rw::V3d &up, float *rx, float *ry, float *rz)
{
	float s = fwd.z;
	if(s > 1.0f) s = 1.0f;
	if(s < -1.0f) s = -1.0f;
	float x = asinf(s);
	float y, z;
	if(cosf(x) > 0.0001f){
		z = atan2f(-fwd.x, fwd.y);
		y = atan2f(-right.z, up.z);
	}else{
		y = 0.0f;
		z = atan2f(right.y, right.x);
	}
	*rx = wrapDeg(x / DEG2RAD);
	*ry = wrapDeg(y / DEG2RAD);
	*rz = wrapDeg(z / DEG2RAD);
}

// Standard rotation-matrix -> quaternion (matrix columns = right, forward, up).
static rw::Quat
AxesToQuat(const rw::V3d &r, const rw::V3d &f, const rw::V3d &u)
{
	float m00 = r.x, m10 = r.y, m20 = r.z;
	float m01 = f.x, m11 = f.y, m21 = f.z;
	float m02 = u.x, m12 = u.y, m22 = u.z;
	float t = m00 + m11 + m22;
	float w, x, y, z;
	if(t > 0.0f){
		float s = sqrtf(t + 1.0f) * 2.0f;
		w = 0.25f * s; x = (m21 - m12) / s; y = (m02 - m20) / s; z = (m10 - m01) / s;
	}else if(m00 > m11 && m00 > m22){
		float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
		w = (m21 - m12) / s; x = 0.25f * s; y = (m01 + m10) / s; z = (m02 + m20) / s;
	}else if(m11 > m22){
		float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
		w = (m02 - m20) / s; x = (m01 + m10) / s; y = 0.25f * s; z = (m12 + m21) / s;
	}else{
		float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
		w = (m10 - m01) / s; x = (m02 + m20) / s; y = (m12 + m21) / s; z = 0.25f * s;
	}
	return rw::makeQuat(w, x, y, z);
}

static float
axisError(const rw::Matrix &m, const rw::V3d &r, const rw::V3d &f, const rw::V3d &u)
{
	return length(sub(m.right, r)) + length(sub(m.up, f)) + length(sub(m.at, u));
}

// Works out (once) whether ObjectInst stores the quaternion or its conjugate,
// by round-tripping a known rotation through ObjectInst::UpdateMatrix.
static void
CalibrateRotation(void)
{
	if(gRotMode >= 0)
		return;
	static ObjectInst probe;
	rw::V3d r, f, u;
	EulerToAxes(31.0f, -17.0f, 123.0f, &r, &f, &u);
	rw::Quat q = AxesToQuat(r, f, u);
	probe.m_translation = rw::makeV3d(0.0f, 0.0f, 0.0f);

	probe.m_rotation = conj(q);
	probe.UpdateMatrix();
	float e0 = axisError(probe.m_matrix, r, f, u);

	probe.m_rotation = q;
	probe.UpdateMatrix();
	float e1 = axisError(probe.m_matrix, r, f, u);

	gRotMode = e0 <= e1 ? 0 : 1;
	log("SA-MP: rotation calibration conj=%.5f direct=%.5f -> mode %d\n", e0, e1, gRotMode);
	if((gRotMode == 0 ? e0 : e1) > 0.01f)
		log("SA-MP: WARNING rotation round-trip error is high, rotations may be wrong\n");
}

static void
SetInstEuler(ObjectInst *inst, float rx, float ry, float rz)
{
	CalibrateRotation();
	rw::V3d r, f, u;
	EulerToAxes(rx, ry, rz, &r, &f, &u);
	rw::Quat q = AxesToQuat(r, f, u);
	inst->m_rotation = gRotMode == 0 ? conj(q) : q;
}

static void
GetMatrixEuler(const rw::Matrix &m, float *rx, float *ry, float *rz)
{
	AxesToEuler(m.right, m.up, m.at, rx, ry, rz);
}

// ------------------------------------------------------------------ creating layer objects

static ObjectInst*
SampCreateInstance(int model, float x, float y, float z, float rx, float ry, float rz, int world, int interior)
{
	model = SampResolveModel(model);
	ObjectDef *obj = GetObjectDef(model);
	if(obj == nil)
		return nil;

	GameFile *file = SampGetLayerFile();
	ObjectInst *inst = AddInstance();
	inst->m_objectId = model;
	inst->m_area = interior < 0 ? 0 : interior & 0xFF;
	inst->m_translation = rw::makeV3d(x, y, z);
	SetInstEuler(inst, rx, ry, rz);
	inst->m_lodId = -1;
	inst->m_lod = nil;
	inst->m_numChildren = 0;
	inst->m_file = file;
	inst->m_imageIndex = -1;
	inst->m_binInstIndex = -1;
	inst->m_iplIndex = ++gSampIplCounter;
	SetInstIplFilterKey(inst, file->name);
	inst->m_isAdded = true;
	inst->m_isDirty = true;
	inst->m_savedStateValid = false;
	inst->m_wasSavedDeleted = false;
	inst->m_gameEntityExists = false;
	StampChangeSeq(inst);

	if(obj->m_isBigBuilding)
		inst->SetupBigBuilding();
	inst->UpdateMatrix();
	if(!obj->IsLoaded()){
		RequestObject(model);
		LoadAllRequestedObjects();
	}
	inst->CreateRwObject();
	if(obj->m_colModel)
		InsertInstIntoSectors(inst);
	else{
		CPtrList *list = inst->m_isBigBuilding ? &outOfBoundsSector.bigbuildings : &outOfBoundsSector.buildings;
		list->InsertItem(inst);
	}
	if(world != -1)
		gInstWorld[inst] = world;
	return inst;
}

static bool
IsStockInst(ObjectInst *inst)
{
	return inst && inst->m_file && !SampIsLayerInst(inst) && inst->m_savedStateValid;
}

// A game building (deleting it means a RemoveBuildingForPlayer), as opposed to an object of the
// SA-MP layer, an agent's scene or another map opened from disk.
bool
SampIsWorldBuilding(ObjectInst *inst)
{
	if(!IsStockInst(inst) || inst->m_file->name == nil || IsExternalIplMapLogicalPath(inst->m_file->name))
		return false;
	return !(IsIplMapDocumentOpen() && IsIplMapDocumentExternal() && IsInstInIplMapDocument(inst));
}

// Selectable although outside the opened IPL: removal only (moves skip it, see FindOrAddLinkedTransformGroup)
bool
SampCanPickWorldBuilding(ObjectInst *inst)
{
	return gSampProtectGameFiles && gSampAllowWorldRemoval && !IsInstInIplMapDocument(inst) &&
	       SampIsWorldBuilding(inst);
}

static void
SetWorldRemoval(bool allow)
{
	gSampAllowWorldRemoval = allow;
	if(!allow){
		// drop picked world buildings: they'd sit in the selection, uneditable
		std::vector<ObjectInst*> picked;
		for(CPtrNode *p = selection.first; p; p = p->next)
			if(!IsInstInIplMapDocument((ObjectInst*)p->item))
				picked.push_back((ObjectInst*)p->item);
		for(size_t i = 0; i < picked.size(); i++)
			picked[i]->Deselect();
	}
	Toast(TOAST_SELECTION, allow ?
		"World buildings can be selected and removed (exported as RemoveBuildingForPlayer)" :
		"World buildings outside the opened IPL are locked again");
}

static void
CountSelectionForDelete(int *total, int *world)
{
	*total = *world = 0;
	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted)
			continue;
		(*total)++;
		if(gSampProtectGameFiles && SampIsWorldBuilding(inst))
			(*world)++;
	}
}

static void
DoDeleteSelection(void)
{
	int total, world;
	CountSelectionForDelete(&total, &world);
	if(total == 0)
		return;
	DeleteSelected(true);
	if(world == 0)
		Toast(TOAST_DELETE, "Deleted %d instance(s)", total < MAX_BATCH_OBJECTS ? total : MAX_BATCH_OBJECTS);
	else if(world == total)
		Toast(TOAST_DELETE, "Removed %d world building(s): exported as RemoveBuildingForPlayer", world);
	else
		Toast(TOAST_DELETE, "Deleted %d object(s) and removed %d world building(s)", total - world, world);
}

void
SampDeleteSelection(void)
{
	int total, world;
	CountSelectionForDelete(&total, &world);
	if(total == 0)
		return;
	if(world > 0 && gSampAskWorldRemoval)
		gOpenConfirmWorldRemoval = true;
	else
		DoDeleteSelection();
}

// Applies a RemoveBuildingForPlayer to the loaded world. Returns instances hidden.
static int
ApplyRemoval(int model, float x, float y, float z, float radius)
{
	int n = 0;
	rw::V3d c = rw::makeV3d(x, y, z);
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!IsStockInst(inst) || inst->m_isDeleted)
			continue;
		if(model != -1 && inst->m_objectId != model)
			continue;
		if(length(sub(inst->m_savedTranslation, c)) > radius)
			continue;
		inst->Delete();
		n++;
	}
	return n;
}

// ------------------------------------------------------------------ import

static void
StripComments(std::string &s)
{
	std::string out;
	out.reserve(s.size());
	bool inStr = false;
	for(size_t i = 0; i < s.size(); i++){
		char c = s[i];
		if(inStr){
			out += c;
			if(c == '\\' && i + 1 < s.size()){ out += s[++i]; continue; }
			if(c == '"') inStr = false;
			continue;
		}
		if(c == '"'){ inStr = true; out += c; continue; }
		if(c == '/' && i + 1 < s.size() && s[i+1] == '/'){
			while(i < s.size() && s[i] != '\n') i++;
			out += '\n';
			continue;
		}
		if(c == '/' && i + 1 < s.size() && s[i+1] == '*'){
			i += 2;
			while(i + 1 < s.size() && !(s[i] == '*' && s[i+1] == '/')) i++;
			i++;
			continue;
		}
		out += c;
	}
	s.swap(out);
}

// Splits "a, b(c, d), e)" into top-level args; returns position after ')'.
static size_t
ParseArgs(const std::string &s, size_t pos, std::vector<std::string> &args)
{
	args.clear();
	std::string cur;
	int depth = 0;
	for(; pos < s.size(); pos++){
		char c = s[pos];
		if(c == '(' || c == '{' || c == '['){ depth++; cur += c; continue; }
		if(c == ')' || c == '}' || c == ']'){
			if(depth == 0 && c == ')'){
				args.push_back(cur);
				return pos + 1;
			}
			depth--; cur += c; continue;
		}
		if(c == ',' && depth == 0){ args.push_back(cur); cur.clear(); continue; }
		cur += c;
	}
	return pos;
}

static bool
ArgNum(const std::vector<std::string> &a, size_t i, double *out)
{
	if(i >= a.size())
		return false;
	const char *s = a[i].c_str();
	while(*s && isspace((unsigned char)*s)) s++;
	char *end;
	double v = strtod(s, &end);
	if(end == s)
		return false;
	while(*end && (isspace((unsigned char)*end) || *end == 'f' || *end == 'F')) end++;
	if(*end != '\0')
		return false;
	*out = v;
	return true;
}

struct CallLayout { const char *name; int offset; bool removal; int worldArg; int interiorArg; };

static const CallLayout gCalls[] = {
	{ "CreateDynamicObjectEx",	0, false, -1, -1 },
	{ "CreateDynamicObject",	0, false, 7, 8 },
	{ "Map_CreateObject",		0, false, 7, 8 },
	{ "CreatePlayerObject",		1, false, -1, -1 },
	{ "CreateObject",		0, false, -1, -1 },
	{ "RemoveBuildingForPlayer",	1, true, -1, -1 },
	{ "Map_RemoveBuilding",		0, true, -1, -1 },
};

struct ImportStats { int objects, raw, removals, removedInsts, rawRemovals, skipped, scaled, materials; };

// the object material calls in imported code apply to (Texture Studio style:
// tmpobjid = CreateDynamicObject(...); SetDynamicObjectMaterial(tmpobjid, ...))
static ObjectInst *gLastImportInst;
static int gLastImportRaw = -1;

static void
AddObject(ImportStats *st, int model, float x, float y, float z, float rx, float ry, float rz, int world, int interior)
{
	ObjectInst *inst = SampCreateInstance(model, x, y, z, rx, ry, rz, world, interior);
	if(inst){
		st->objects++;
		gLastImportInst = inst;
		gLastImportRaw = -1;
	}else{
		SampRawObject o = { model, x, y, z, rx, ry, rz, world, interior };
		gRawObjects.push_back(o);
		st->raw++;
		gLastImportInst = nil;
		gLastImportRaw = (int)gRawObjects.size() - 1;
	}
}

static void
AddMaterialToLast(ImportStats *st, const SampMaterial &m)
{
	if(gLastImportInst)
		PutMaterial(gInstMaterials[gLastImportInst], m);
	else if(gLastImportRaw >= 0 && gLastImportRaw < (int)gRawObjects.size())
		PutMaterial(gRawObjects[gLastImportRaw].mats, m);
	else{
		st->skipped++;
		return;
	}
	st->materials++;
}

static void
AddRemoval(ImportStats *st, int model, float x, float y, float z, float r)
{
	st->removals++;
	int n = ApplyRemoval(model, x, y, z, r);
	st->removedInsts += n;
	if(n == 0){
		SampRawRemoval rr = { model, x, y, z, r };
		gRawRemovals.push_back(rr);
		st->rawRemovals++;
	}
}

// material calls: first argument is the object; layouts after it
//  kind 0: index, model, txd, texture, colour
//  kind 1: index, text, size, font, fontsize, bold, fontcolor, backcolor, align   (dynamic / Map_ order)
//  kind 2: text, index, size, font, fontsize, bold, fontcolor, backcolor, align   (native order)
struct MaterialCall { const char *name; int offset; int kind; };
static const MaterialCall gMaterialCalls[] = {
	{ "SetDynamicObjectMaterialText",	1, 1 },
	{ "SetDynamicObjectMaterial",		1, 0 },
	{ "Map_SetObjectMaterialText",		1, 1 },
	{ "Map_SetObjectMaterial",		1, 0 },
	{ "SetPlayerObjectMaterialText",	2, 2 },
	{ "SetPlayerObjectMaterial",		2, 0 },
	{ "SetObjectMaterialText",		1, 2 },
	{ "SetObjectMaterial",			1, 0 },
};

static std::string
ArgTrim(const std::vector<std::string> &a, size_t i)
{
	if(i >= a.size()) return "";
	size_t b = 0, e = a[i].size();
	while(b < e && isspace((unsigned char)a[i][b])) b++;
	while(e > b && isspace((unsigned char)a[i][e-1])) e--;
	return a[i].substr(b, e - b);
}

static bool
ArgString(const std::vector<std::string> &a, size_t i, std::string *out)
{
	std::string s = ArgTrim(a, i);
	if(s.size() < 2 || s[0] != '"' || s.back() != '"')
		return false;
	out->clear();
	for(size_t k = 1; k + 1 < s.size(); k++){
		if(s[k] == '\\' && k + 2 < s.size()){
			char n = s[++k];
			*out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
		}else
			*out += s[k];
	}
	return true;
}

// integers, hex colours (0xAARRGGBB, -1) and the SA-MP material constants
static bool
ArgInt(const std::vector<std::string> &a, size_t i, long long *out)
{
	std::string s = ArgTrim(a, i);
	if(s.empty()) return false;
	static const struct { const char *name; int value; } names[] = {
		{ "OBJECT_MATERIAL_SIZE_32x32", 10 }, { "OBJECT_MATERIAL_SIZE_64x32", 20 }, { "OBJECT_MATERIAL_SIZE_64x64", 30 },
		{ "OBJECT_MATERIAL_SIZE_128x32", 40 }, { "OBJECT_MATERIAL_SIZE_128x64", 50 }, { "OBJECT_MATERIAL_SIZE_128x128", 60 },
		{ "OBJECT_MATERIAL_SIZE_256x32", 70 }, { "OBJECT_MATERIAL_SIZE_256x64", 80 }, { "OBJECT_MATERIAL_SIZE_256x128", 90 },
		{ "OBJECT_MATERIAL_SIZE_256x256", 100 }, { "OBJECT_MATERIAL_SIZE_512x64", 110 }, { "OBJECT_MATERIAL_SIZE_512x128", 120 },
		{ "OBJECT_MATERIAL_SIZE_512x256", 130 }, { "OBJECT_MATERIAL_SIZE_512x512", 140 },
		{ "OBJECT_MATERIAL_TEXT_ALIGN_LEFT", 0 }, { "OBJECT_MATERIAL_TEXT_ALIGN_CENTER", 1 }, { "OBJECT_MATERIAL_TEXT_ALIGN_RIGHT", 2 },
		{ "true", 1 }, { "false", 0 },
	};
	for(auto &n : names)
		if(s == n.name){ *out = n.value; return true; }
	char *end;
	long long v = strtoll(s.c_str(), &end, 0);
	if(end == s.c_str() || *end != '\0'){
		// unsigned hex colours above 0x7FFFFFFF
		unsigned long long u = strtoull(s.c_str(), &end, 0);
		if(end == s.c_str() || *end != '\0') return false;
		v = (long long)u;
	}
	*out = v;
	return true;
}

static void
ImportMaterialCall(const MaterialCall *call, const std::vector<std::string> &args, ImportStats *st)
{
	int o = call->offset;
	SampMaterial m = SampMaterial();
	long long v[8];
	if(call->kind == 0){
		if(!ArgInt(args, o, &v[0]) || !ArgInt(args, o + 1, &v[1]) || !ArgString(args, o + 2, &m.txd) ||
		   !ArgString(args, o + 3, &m.tex)){
			st->skipped++;
			return;
		}
		v[2] = 0;
		ArgInt(args, o + 4, &v[2]);
		m.index = (int)v[0]; m.model = (int)v[1]; m.color = (uint32)v[2]; m.text = false;
	}else{
		int ti = call->kind == 1 ? o + 1 : o, ii = call->kind == 1 ? o : o + 1;
		long long def[] = { 0, 90, 0, 24, 1, (long long)0xFFFFFFFF, 0, 0 };
		if(!ArgString(args, ti, &m.str) || (args.size() > (size_t)ii && !ArgInt(args, ii, &v[0]))){
			st->skipped++;
			return;
		}
		if(args.size() <= (size_t)ii) v[0] = 0;
		for(int k = 1; k < 8; k++){
			v[k] = def[k];
			if(k == 2){ if(!ArgString(args, o + 3, &m.font)) m.font = "Arial"; continue; }
			ArgInt(args, o + 1 + k, &v[k]);
		}
		m.text = true; m.index = (int)v[0]; m.size = (int)v[1]; m.fontSize = (int)v[3]; m.bold = (int)v[4];
		m.fontColor = (uint32)v[5]; m.backColor = (uint32)v[6]; m.align = (int)v[7];
	}
	AddMaterialToLast(st, m);
}

static void
ImportPawn(std::string &text, ImportStats *st)
{
	StripComments(text);
	gLastImportInst = nil;
	gLastImportRaw = -1;
	std::vector<std::string> args;
	for(size_t i = 0; i < text.size(); i++){
		if(!(isalpha((unsigned char)text[i]) || text[i] == '_'))
			continue;
		if(i > 0 && (isalnum((unsigned char)text[i-1]) || text[i-1] == '_'))
			continue;
		size_t j = i;
		while(j < text.size() && (isalnum((unsigned char)text[j]) || text[j] == '_')) j++;
		std::string ident = text.substr(i, j - i);
		const CallLayout *call = nil;
		for(const CallLayout &c : gCalls)
			if(ident == c.name){ call = &c; break; }
		const MaterialCall *mcall = nil;
		if(call == nil)
			for(const MaterialCall &c : gMaterialCalls)
				if(ident == c.name){ mcall = &c; break; }
		size_t k = j;
		while(k < text.size() && isspace((unsigned char)text[k])) k++;
		if((call == nil && mcall == nil) || k >= text.size() || text[k] != '('){
			i = j - 1;
			continue;
		}
		size_t after = ParseArgs(text, k + 1, args);
		i = after - 1;
		if(mcall){
			ImportMaterialCall(mcall, args, st);
			continue;
		}
		double v[7];
		int o = call->offset;
		if(call->removal){
			bool ok = true;
			for(int n = 0; n < 5; n++) ok = ok && ArgNum(args, o + n, &v[n]);
			if(!ok){ st->skipped++; continue; }
			AddRemoval(st, (int)v[0], (float)v[1], (float)v[2], (float)v[3], (float)v[4]);
		}else{
			bool ok = true;
			for(int n = 0; n < 7; n++) ok = ok && ArgNum(args, o + n, &v[n]);
			if(!ok){ st->skipped++; continue; }
			double w = -1, in = 0;
			if(call->worldArg >= 0) ArgNum(args, call->worldArg, &w);
			if(call->interiorArg >= 0) ArgNum(args, call->interiorArg, &in);
			AddObject(st, (int)v[0], (float)v[1], (float)v[2], (float)v[3], (float)v[4], (float)v[5], (float)v[6], (int)w, (int)in);
		}
	}
}

static bool
XmlAttr(const std::string &tag, const char *name, double *out)
{
	std::string key = std::string(" ") + name + "=\"";
	size_t p = tag.find(key);
	if(p == std::string::npos){
		key[0] = '\t';
		p = tag.find(key);
		if(p == std::string::npos) return false;
	}
	*out = atof(tag.c_str() + p + key.size());
	return true;
}

static void
ImportMtaMap(const std::string &text, ImportStats *st)
{
	size_t pos = 0;
	while((pos = text.find('<', pos)) != std::string::npos){
		size_t end = text.find('>', pos);
		if(end == std::string::npos) break;
		std::string tag = text.substr(pos, end - pos);
		pos = end;
		double model, x, y, z, rx = 0, ry = 0, rz = 0, interior = 0, dim = -1, scale = 1, radius = 0.25, lod = -1;
		if(tag.compare(0, 8, "<object ") == 0 || tag.compare(0, 8, "<object\t") == 0){
			if(!XmlAttr(tag, "model", &model) || !XmlAttr(tag, "posX", &x) || !XmlAttr(tag, "posY", &y) || !XmlAttr(tag, "posZ", &z)){
				st->skipped++;
				continue;
			}
			XmlAttr(tag, "rotX", &rx); XmlAttr(tag, "rotY", &ry); XmlAttr(tag, "rotZ", &rz);
			XmlAttr(tag, "interior", &interior); XmlAttr(tag, "dimension", &dim);
			if(XmlAttr(tag, "scale", &scale) && fabs(scale - 1.0) > 0.001) st->scaled++;
			// MTA dimension 0 = the main world; keep it world-agnostic in SA-MP
			AddObject(st, (int)model, (float)x, (float)y, (float)z, (float)rx, (float)ry, (float)rz, dim <= 0 ? -1 : (int)dim, (int)interior);
		}else if(tag.compare(0, 18, "<removeWorldObject") == 0){
			if(!XmlAttr(tag, "model", &model) || !XmlAttr(tag, "posX", &x) || !XmlAttr(tag, "posY", &y) || !XmlAttr(tag, "posZ", &z)){
				st->skipped++;
				continue;
			}
			XmlAttr(tag, "radius", &radius);
			AddRemoval(st, (int)model, (float)x, (float)y, (float)z, (float)radius);
			if(XmlAttr(tag, "lodModel", &lod) && lod > 0)
				AddRemoval(st, (int)lod, (float)x, (float)y, (float)z, (float)radius);
		}
	}
}

static bool
HasExtCi(const char *path, const char *ext)
{
	size_t lp = strlen(path), le = strlen(ext);
	return lp >= le && rw::strncmp_ci(path + lp - le, ext, (int)le) == 0;
}

bool
SampCanImport(const char *path)
{
	return path && (HasExtCi(path, ".pwn") || HasExtCi(path, ".inc") || HasExtCi(path, ".p") ||
	                HasExtCi(path, ".txt") || HasExtCi(path, ".map"));
}

int
SampImportFile(const char *path)
{
	FILE *f = fopen(path, "rb");
	if(f == nil){
		Toast(TOAST_SPAWN, "SA-MP import: can't open %s", path);
		return 0;
	}
	std::string text;
	char buf[65536];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), f)) > 0)
		text.append(buf, n);
	fclose(f);

	LoadSettings();
	SampLoadCustomModels(nil);
	ImportStats st = {};
	if(HasExtCi(path, ".map"))
		ImportMtaMap(text, &st);
	else
		ImportPawn(text, &st);

	log("SA-MP import %s: %d objects, %d kept unrendered, %d removals (%d buildings hidden, %d unmatched), %d materials, %d skipped, %d scaled\n",
		path, st.objects, st.raw, st.removals, st.removedInsts, st.rawRemovals, st.materials, st.skipped, st.scaled);
	Toast(TOAST_SPAWN, "SA-MP import: %d objects, %d removals%s%s", st.objects + st.raw, st.removals,
		st.raw ? " (some SA-MP-only models aren't shown)" : "",
		st.scaled ? " - scaled MTA objects imported at normal size" : "");
	return st.objects + st.raw + st.removals;
}

// ------------------------------------------------------------------ export

struct ExportLists
{
	std::vector<SampRawObject> objects;
	std::vector<SampRawRemoval> removals;
	int moved;
};

static bool
ChangedTransform(ObjectInst *inst)
{
	rw::V3d d = sub(inst->m_translation, inst->m_savedTranslation);
	rw::Quat a = inst->m_rotation, b = inst->m_savedRotation;
	float dq = fabsf(a.x - b.x) + fabsf(a.y - b.y) + fabsf(a.z - b.z) + fabsf(a.w - b.w);
	return length(d) > 0.001f || dq > 0.0001f;
}

static void
CollectExport(ExportLists *out)
{
	out->moved = 0;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(SampIsLayerInst(inst)){
			if(inst->m_isDeleted)
				continue;
			// LOD companions Ariane adds for display: SA-MP objects have no LOD
			// system, a LOD exported as an object would render doubled/flicker
			if(inst->m_numChildren > 0)
				continue;
			inst->UpdateMatrix();
			SampRawObject o;
			o.model = SampExportModel(inst->m_objectId);
			o.x = inst->m_translation.x; o.y = inst->m_translation.y; o.z = inst->m_translation.z;
			GetMatrixEuler(inst->m_matrix, &o.rx, &o.ry, &o.rz);
			std::map<ObjectInst*, int>::iterator it = gInstWorld.find(inst);
			o.world = it == gInstWorld.end() ? -1 : it->second;
			o.interior = inst->m_area;
			std::map<ObjectInst*, std::vector<SampMaterial>>::iterator mt = gInstMaterials.find(inst);
			if(mt != gInstMaterials.end())
				o.mats = mt->second;
			out->objects.push_back(o);
			continue;
		}
		if(!IsStockInst(inst))
			continue;
		if(inst->m_isDeleted){
			SampRawRemoval r = { inst->m_objectId, inst->m_savedTranslation.x, inst->m_savedTranslation.y, inst->m_savedTranslation.z, 0.25f };
			out->removals.push_back(r);
		}else if(ChangedTransform(inst)){
			// SA-MP can't move stock buildings: remove the original, place a copy
			SampRawRemoval r = { inst->m_objectId, inst->m_savedTranslation.x, inst->m_savedTranslation.y, inst->m_savedTranslation.z, 0.25f };
			out->removals.push_back(r);
			inst->UpdateMatrix();
			SampRawObject o;
			o.model = inst->m_objectId;
			o.x = inst->m_translation.x; o.y = inst->m_translation.y; o.z = inst->m_translation.z;
			GetMatrixEuler(inst->m_matrix, &o.rx, &o.ry, &o.rz);
			o.world = -1;
			o.interior = inst->m_area;
			out->objects.push_back(o);
			out->moved++;
		}
	}
	out->objects.insert(out->objects.end(), gRawObjects.begin(), gRawObjects.end());
	out->removals.insert(out->removals.end(), gRawRemovals.begin(), gRawRemovals.end());
	// a removal loaded from the server file before its IPL streamed in is kept
	// raw, and deleting the building later adds it again: export each once
	std::vector<SampRawRemoval> unique;
	for(const SampRawRemoval &r : out->removals){
		bool dup = false;
		for(const SampRawRemoval &u : unique)
			if(u.model == r.model && fabsf(u.x - r.x) < 0.01f && fabsf(u.y - r.y) < 0.01f && fabsf(u.z - r.z) < 0.01f){
				dup = true;
				break;
			}
		if(!dup) unique.push_back(r);
	}
	out->removals.swap(unique);
}

static void
SanitizeName(char *s)
{
	for(; *s; s++)
		if(!(isalnum((unsigned char)*s) || *s == '_')) *s = '_';
}

static const char *gFormatNames[] = {
	"Wrapper functions (<prefix>CreateObject ... - defined in your script)",
	"Streamer plugin (CreateDynamicObject)",
	"Native open.mp / SA-MP (CreateObject)",
};

// One object (plus its material calls) in the chosen API; `assign` stores the id
// in tmpobjid when material calls follow (Texture Studio style).
static void
PawnObjectLines(std::string &out, const SampRawObject &o, int format, const char *indent)
{
	char b[512];
	const char *assign = o.mats.empty() ? "" : "tmpobjid = ";
	const char *pre = gSampWrapPrefix;
	if(format == SAMP_FORMAT_WRAPPER)
		snprintf(b, sizeof(b), "%s%s%sCreateObject(%d, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, %d, %d);\n",
			indent, assign, pre, o.model, o.x, o.y, o.z, o.rx, o.ry, o.rz, o.world, o.interior);
	else if(format == SAMP_FORMAT_STREAMER)
		snprintf(b, sizeof(b), "%s%sCreateDynamicObject(%d, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, %d, %d);\n",
			indent, assign, o.model, o.x, o.y, o.z, o.rx, o.ry, o.rz, o.world, o.interior < 0 ? -1 : o.interior);
	else
		snprintf(b, sizeof(b), "%s%sCreateObject(%d, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, 300.0);\n",
			indent, assign, o.model, o.x, o.y, o.z, o.rx, o.ry, o.rz);
	out += b;
	std::string setMat = format == SAMP_FORMAT_WRAPPER ? std::string(pre) + "SetObjectMaterial" :
	                     format == SAMP_FORMAT_STREAMER ? "SetDynamicObjectMaterial" : "SetObjectMaterial";
	for(const SampMaterial &m : o.mats){
		std::string line;
		if(!m.text){
			snprintf(b, sizeof(b), "(tmpobjid, %d, %d, ", m.index, m.model);
			line = setMat + b + PawnString(m.txd) + ", " + PawnString(m.tex);
			snprintf(b, sizeof(b), ", 0x%08X);", m.color);
			line += b;
		}else if(format == SAMP_FORMAT_NATIVE){
			snprintf(b, sizeof(b), ", %d, %d, ", m.index, m.size);
			line = "SetObjectMaterialText(tmpobjid, " + PawnString(m.str) + b + PawnString(m.font);
			snprintf(b, sizeof(b), ", %d, %d, 0x%08X, 0x%08X, %d);", m.fontSize, m.bold, m.fontColor, m.backColor, m.align);
			line += b;
		}else{
			snprintf(b, sizeof(b), "Text(tmpobjid, %d, ", m.index);
			line = setMat + b + PawnString(m.str);
			snprintf(b, sizeof(b), ", %d, ", m.size);
			line += b + PawnString(m.font);
			snprintf(b, sizeof(b), ", %d, %d, 0x%08X, 0x%08X, %d);", m.fontSize, m.bold, m.fontColor, m.backColor, m.align);
			line += b;
		}
		out += indent + line + "\n";
	}
}

static void
PawnRemovalLine(std::string &out, const SampRawRemoval &r, int format, const char *indent)
{
	char b[256];
	if(format == SAMP_FORMAT_WRAPPER)
		snprintf(b, sizeof(b), "%s%sRemoveBuilding(%d, %.4f, %.4f, %.4f, %.2f);\n", indent, gSampWrapPrefix,
			r.model, r.x, r.y, r.z, r.radius);
	else
		snprintf(b, sizeof(b), "%sRemoveBuildingForPlayer(playerid, %d, %.4f, %.4f, %.4f, %.2f);\n", indent,
			r.model, r.x, r.y, r.z, r.radius);
	out += b;
}

static std::string
PawnText(const char *mapName, int format, const ExportLists &l)
{
	char name[64], b[512];
	strncpy(name, mapName, sizeof(name));
	name[sizeof(name)-1] = '\0';
	SanitizeName(name);
	std::string out;
	snprintf(b, sizeof(b), "// Exported by Ariane (SA-MP edition): map '%s' - %d objects, %d removed buildings\n"
		"// Format: %s\n", name, (int)l.objects.size(), (int)l.removals.size(), gFormatNames[format]);
	out += b;
	bool anyMats = false;
	for(const SampRawObject &o : l.objects)
		if(!o.mats.empty()) anyMats = true;
	if(format == SAMP_FORMAT_WRAPPER){
		snprintf(b, sizeof(b), "// Needs %sCreateObject, %sRemoveBuilding%s in your script.\nstock CreateMap_%s()\n{\n",
			gSampWrapPrefix, gSampWrapPrefix, anyMats ? ", ...SetObjectMaterial(Text)" : "", name);
		out += b;
		if(anyMats) out += "    new tmpobjid;\n";
		for(const SampRawRemoval &r : l.removals) PawnRemovalLine(out, r, format, "    ");
		for(const SampRawObject &o : l.objects) PawnObjectLines(out, o, format, "    ");
		snprintf(b, sizeof(b), "    return %d;\n}\n", (int)l.objects.size());
		out += b;
	}else{
		snprintf(b, sizeof(b), "// --- objects: call from OnGameModeInit / OnFilterScriptInit\nstock CreateMap_%s()\n{\n", name);
		out += b;
		if(anyMats) out += "    new tmpobjid;\n";
		for(const SampRawObject &o : l.objects) PawnObjectLines(out, o, format, "    ");
		snprintf(b, sizeof(b), "}\n\n// --- removed buildings: call from OnPlayerConnect\nstock RemoveMap_%s(playerid)\n{\n", name);
		out += b;
		if(l.removals.empty()) out += "    #pragma unused playerid\n";
		for(const SampRawRemoval &r : l.removals) PawnRemovalLine(out, r, format, "    ");
		out += "}\n";
	}
	return out;
}

static bool
WritePawn(const char *path, const char *mapName, int format, const ExportLists &l)
{
	FILE *f = fopen(path, "w");
	if(f == nil)
		return false;
	std::string text = PawnText(mapName, format, l);
	fwrite(text.data(), 1, text.size(), f);
	fclose(f);
	return true;
}

// Selected objects (layer objects and stock buildings alike) as Pawn lines in
// the current export format - for pasting into a script.
int
SampCopySelectionAsPawn(void)
{
	std::string out;
	int n = 0;
	bool anyMats = false;
	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst == nil || inst->m_isDeleted)
			continue;
		inst->UpdateMatrix();
		SampRawObject o;
		o.model = SampExportModel(inst->m_objectId);
		o.x = inst->m_translation.x; o.y = inst->m_translation.y; o.z = inst->m_translation.z;
		GetMatrixEuler(inst->m_matrix, &o.rx, &o.ry, &o.rz);
		std::map<ObjectInst*, int>::iterator w = gInstWorld.find(inst);
		o.world = w == gInstWorld.end() ? -1 : w->second;
		o.interior = inst->m_area;
		std::map<ObjectInst*, std::vector<SampMaterial>>::iterator mt = gInstMaterials.find(inst);
		if(mt != gInstMaterials.end()) o.mats = mt->second;
		if(!o.mats.empty()) anyMats = true;
		PawnObjectLines(out, o, gSampExportFormat, "");
		n++;
	}
	if(n == 0){
		Toast(TOAST_COPY_PASTE, "Select objects first");
		return 0;
	}
	if(anyMats)
		out = "new tmpobjid;\n" + out;
	ImGui::SetClipboardText(out.c_str());
	Toast(TOAST_COPY_PASTE, "Copied %d object(s) as Pawn (%s)", n,
		gSampExportFormat == SAMP_FORMAT_WRAPPER ? "wrapper" : gSampExportFormat == SAMP_FORMAT_STREAMER ? "streamer" : "native");
	return n;
}

// ------------------------------------------------------------------ settings

static void
LoadSettings(void)
{
	if(gSettingsLoaded)
		return;
	gSettingsLoaded = true;
	char path[1024];
	if(!GetArianeDataPath(path, sizeof(path), "samp_settings.ini"))
		return;
	FILE *f = fopen(path, "r");
	if(f == nil)
		return;
	char line[1200];
	while(fgets(line, sizeof(line), f)){
		line[strcspn(line, "\r\n")] = '\0';
		if(strncmp(line, "server=", 7) == 0) strncpy(gSampServerDir, line + 7, sizeof(gSampServerDir) - 1);
		else if(strncmp(line, "map=", 4) == 0) strncpy(gSampMapName, line + 4, sizeof(gSampMapName) - 1);
		else if(strncmp(line, "format=", 7) == 0) gSampExportFormat = atoi(line + 7);
		else if(strncmp(line, "prefix=", 7) == 0) strncpy(gSampWrapPrefix, line + 7, sizeof(gSampWrapPrefix) - 1);
		else if(strncmp(line, "incdir=", 7) == 0) strncpy(gSampIncDir, line + 7, sizeof(gSampIncDir) - 1);
		else if(strncmp(line, "autoinc=", 8) == 0) gSampAutoInc = atoi(line + 8) != 0;
		else if(strncmp(line, "protect=", 8) == 0) gSampProtectGameFiles = atoi(line + 8) != 0;
		else if(strncmp(line, "worldremove=", 12) == 0) gSampAllowWorldRemoval = atoi(line + 12) != 0;
		else if(strncmp(line, "askremove=", 10) == 0) gSampAskWorldRemoval = atoi(line + 10) != 0;
		else if(strncmp(line, "gotowho=", 8) == 0){
			for(int m = 0; m < 4; m++)
				if(strcmp(line + 8, kGotoModes[m]) == 0) gSampGotoWho = m;
		}
		else if(strncmp(line, "gotoplayer=", 11) == 0) strncpy(gSampGotoPlayer, line + 11, sizeof(gSampGotoPlayer) - 1);
		else if(strncmp(line, "followfocus=", 12) == 0) gSampFollowFocus = atoi(line + 12) != 0;
		else if(strncmp(line, "drawdist=", 9) == 0){
			gSampLayerDrawDist = (float)atof(line + 9);
			if(gSampLayerDrawDist < 0.0f) gSampLayerDrawDist = 0.0f;
			if(gSampLayerDrawDist > 3000.0f) gSampLayerDrawDist = 3000.0f;
		}
	}
	fclose(f);
}

static void
SaveSettings(void)
{
	char path[1024];
	if(!GetArianeDataPath(path, sizeof(path), "samp_settings.ini"))
		return;
	FILE *f = fopen(path, "w");
	if(f == nil)
		return;
	fprintf(f, "server=%s\nmap=%s\nformat=%d\nautoinc=%d\nprotect=%d\n",
		gSampServerDir, gSampMapName, gSampExportFormat, gSampAutoInc ? 1 : 0, gSampProtectGameFiles ? 1 : 0);
	fprintf(f, "prefix=%s\nincdir=%s\n", gSampWrapPrefix, gSampIncDir);
	fprintf(f, "worldremove=%d\naskremove=%d\n", gSampAllowWorldRemoval ? 1 : 0, gSampAskWorldRemoval ? 1 : 0);
	fprintf(f, "gotowho=%s\ngotoplayer=%s\n", kGotoModes[gSampGotoWho], gSampGotoPlayer);
	fprintf(f, "followfocus=%d\n", gSampFollowFocus ? 1 : 0);
	fprintf(f, "drawdist=%.0f\n", gSampLayerDrawDist);
	fclose(f);
}

// ------------------------------------------------------------------ live sync

static void
JoinPath(char *dst, size_t size, const char *a, const char *b)
{
	size_t la = strlen(a);
	bool slash = la > 0 && (a[la-1] == '/' || a[la-1] == '\\');
	snprintf(dst, size, "%s%s%s", a, slash ? "" : "/", b);
}

static bool gOpenConfirmEmptySync, gOpenConfirmReload;
static int gPendingServerEntries;

static void
LiveFilePath(char *dst, size_t size)
{
	JoinPath(dst, size, gSampServerDir, "scriptfiles/ariane_live.txt");
}

// Number of object/removal lines in the server's live file, -1 if there is none.
static int
LiveFileEntries(void)
{
	char path[1200];
	LiveFilePath(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if(f == nil)
		return -1;
	char line[1200];
	int n = 0;
	while(fgets(line, sizeof(line), f))
		if((line[0] == 'O' || line[0] == 'R') && line[1] == ' ')
			n++;
	fclose(f);
	return n;
}

static void ClearLayer(void);
static int CountLayer(int *removed);

// Rebuilds the SA-MP layer from what the server is currently running
// (scriptfiles/ariane_live.txt, written by the last Sync).
int
SampLoadFromServer(void)
{
	LoadSettings();
	if(gSampServerDir[0] == '\0'){
		gOpenServer = true;
		return -1;
	}
	SampLoadCustomModels(nil);
	char path[1200];
	LiveFilePath(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if(f == nil){
		Toast(TOAST_SPAWN, "Nothing on the server yet (no %s)", path);
		return -1;
	}
	ClearLayer();
	ImportStats st = {};
	gLastImportInst = nil;
	gLastImportRaw = -1;
	char line[1200];
	while(fgets(line, sizeof(line), f)){
		int model, world, interior;
		float x, y, z, rx, ry, rz, radius;
		SampMaterial mat;
		if((line[0] == 'M' || line[0] == 'T') && ParseMaterialLine(line, &mat))
			AddMaterialToLast(&st, mat);
		else if(line[0] == 'O' &&
		   sscanf(line + 1, "%d %f %f %f %f %f %f %d %d", &model, &x, &y, &z, &rx, &ry, &rz, &world, &interior) == 9)
			AddObject(&st, model, x, y, z, rx, ry, rz, world, interior);
		else if(line[0] == 'R' &&
		        sscanf(line + 1, "%d %f %f %f %f", &model, &x, &y, &z, &radius) == 5)
			AddRemoval(&st, model, x, y, z, radius);
	}
	fclose(f);
	log("SA-MP: loaded from server: %d objects (%d not drawable), %d removals\n", st.objects + st.raw, st.raw, st.removals);
	Toast(TOAST_SPAWN, "Loaded the server's map: %d objects, %d removed buildings", st.objects + st.raw, st.removals);
	return st.objects + st.raw + st.removals;
}

static bool DoSyncToServer(bool force);

bool
SampSyncToServer(void)
{
	return DoSyncToServer(false);
}

static bool
DoSyncToServer(bool force)
{
	LoadSettings();
	if(gSampServerDir[0] == '\0'){
		gOpenServer = true;
		return false;
	}
	ExportLists l;
	CollectExport(&l);

	// Syncing an empty layer over a populated server map would wipe it -
	// almost always because Ariane was restarted and the map wasn't reloaded.
	if(!force && l.objects.empty() && l.removals.empty()){
		int onServer = LiveFileEntries();
		if(onServer > 0){
			gPendingServerEntries = onServer;
			gOpenConfirmEmptySync = true;
			return false;
		}
	}

	char path[1200], tmp[1200];
	LiveFilePath(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if(f == nil){
		Toast(TOAST_SAVE, "Sync failed: can't write %s (is the server folder right?)", path);
		return false;
	}
	fprintf(f, "rev %u\n", (unsigned)time(nil));
	for(const SampRawRemoval &r : l.removals)
		fprintf(f, "R %d %.4f %.4f %.4f %.2f\n", r.model, r.x, r.y, r.z, r.radius);
	for(const SampRawObject &o : l.objects){
		fprintf(f, "O %d %.4f %.4f %.4f %.4f %.4f %.4f %d %d\n", o.model, o.x, o.y, o.z, o.rx, o.ry, o.rz, o.world, o.interior);
		WriteMaterialLines(f, o.mats);
	}
	fclose(f);
	remove(path);
	if(rename(tmp, path) != 0){
		Toast(TOAST_SAVE, "Sync failed: couldn't replace %s", path);
		return false;
	}

	if(gSampAutoInc){
		char name[64], file[128], inc[1200];
		strncpy(name, gSampMapName, sizeof(name));
		name[sizeof(name)-1] = '\0';
		SanitizeName(name);
		snprintf(file, sizeof(file), "%s/%s.inc", gSampIncDir, name);
		JoinPath(inc, sizeof(inc), gSampServerDir, file);
		WritePawn(inc, name, gSampExportFormat, l);
	}
	Toast(TOAST_SAVE, "Synced %d objects, %d removals to the server%s", (int)l.objects.size(), (int)l.removals.size(),
		l.moved ? " (moved buildings exported as remove + copy)" : "");
	return true;
}

// Agent bridge: Export Pawn to a file. format: 0 wrapper, 1 streamer, 2 native, -1 = current setting.
bool
SampAgentExport(const char *path, int format, const char *mapName, const char *prefix, int *objects, int *removals,
	char *message, size_t messageSize)
{
	LoadSettings();
	if(format < 0) format = gSampExportFormat;
	if(format > SAMP_FORMAT_NATIVE){
		snprintf(message, messageSize, "format must be 0 (wrapper), 1 (streamer) or 2 (native)");
		return false;
	}
	char savedPrefix[sizeof(gSampWrapPrefix)];
	strcpy(savedPrefix, gSampWrapPrefix);
	if(prefix && prefix[0]){
		strncpy(gSampWrapPrefix, prefix, sizeof(gSampWrapPrefix) - 1);
		gSampWrapPrefix[sizeof(gSampWrapPrefix) - 1] = '\0';
	}
	ExportLists l;
	CollectExport(&l);
	*objects = (int)l.objects.size();
	*removals = (int)l.removals.size();
	bool ok = WritePawn(path, mapName && mapName[0] ? mapName : gSampMapName, format, l);
	strcpy(gSampWrapPrefix, savedPrefix);
	snprintf(message, messageSize, ok ? "exported %s format to %s" : "can't write %s format to %s",
		format == SAMP_FORMAT_WRAPPER ? "wrapper" : format == SAMP_FORMAT_STREAMER ? "streamer" : "native", path);
	return ok;
}

// Agent bridge entry point: sync without UI popups; returns false + message on refusal
bool
SampAgentSync(const char *mapName, bool force, int *objects, int *removals, char *message, size_t messageSize)
{
	LoadSettings();
	if(mapName && mapName[0]){
		strncpy(gSampMapName, mapName, sizeof(gSampMapName) - 1);
		gSampMapName[sizeof(gSampMapName) - 1] = '\0';
		SaveSettings();
	}
	ExportLists l;
	CollectExport(&l);
	*objects = (int)l.objects.size();
	*removals = (int)l.removals.size();
	if(gSampServerDir[0] == '\0'){
		snprintf(message, messageSize, "no server folder set (SA-MP > Server folder...)");
		return false;
	}
	if(!force && l.objects.empty() && l.removals.empty() && LiveFileEntries() > 0){
		snprintf(message, messageSize, "refusing to sync an empty layer over the server's map (pass force)");
		return false;
	}
	bool ok = DoSyncToServer(true);
	char path[1200];
	LiveFilePath(path, sizeof(path));
	snprintf(message, messageSize, ok ? "synced map '%s' to %s" : "sync failed for map '%s' (%s)", gSampMapName, path);
	return ok;
}

// ------------------------------------------------------------------ UI

#ifdef _WIN32
static bool
FileDialog(bool save, char *path, size_t size, const char *filter, const char *defExt)
{
	OPENFILENAMEA ofn;
	memset(&ofn, 0, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.lpstrFilter = filter;
	ofn.lpstrFile = path;
	ofn.nMaxFile = (DWORD)size;
	ofn.lpstrDefExt = defExt;
	ofn.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
	return save ? GetSaveFileNameA(&ofn) != 0 : GetOpenFileNameA(&ofn) != 0;
}
#endif

static int
CountLayer(int *removed)
{
	int n = 0, r = 0;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(SampIsLayerInst(inst) && !inst->m_isDeleted && inst->m_numChildren == 0) n++;
		else if(IsStockInst(inst) && inst->m_isDeleted) r++;
	}
	*removed = r + (int)gRawRemovals.size();
	return n + (int)gRawObjects.size();
}

static void
ClearLayer(void)
{
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(SampIsLayerInst(inst)) inst->Delete();
		else if(IsStockInst(inst) && inst->m_isDeleted) inst->Undelete();
	}
	gRawObjects.clear();
	gRawRemovals.clear();
	gInstWorld.clear();
	gInstMaterials.clear();
}

#ifdef _WIN32
static void
SampImportDialog(void)
{
	char path[1024] = "";
	if(FileDialog(false, path, sizeof(path), "Maps (*.pwn;*.inc;*.txt;*.map)\0*.pwn;*.inc;*.txt;*.map;*.p\0All files\0*.*\0", nil))
		SampImportFile(path);
}
#endif

static void
SampRequestReload(void)
{
	int removed = 0, objects = CountLayer(&removed);
	if(objects > 0 || removed > 0) gOpenConfirmReload = true;
	else SampLoadFromServer();
}

// SA-MP keyboard shortcuts (also listed under "SA-MP" in Window > Keyboard Shortcuts)
void
SampShortcuts(bool allowEditorShortcuts)
{
	if(!allowEditorShortcuts || !CPad::IsCtrlDown() || !CPad::IsShiftDown())
		return;
	if(CPad::IsKeyJustDown('S')) SampSyncToServer();
	if(CPad::IsKeyJustDown('L')) SampRequestReload();
	if(CPad::IsKeyJustDown('P')) gOpenExport = true;
	if(CPad::IsKeyJustDown('C')) SampCopySelectionAsPawn();
	if(CPad::IsKeyJustDown('T')) SampShowInGame();
	if(CPad::IsKeyJustDown('W') && gSampProtectGameFiles){
		SetWorldRemoval(!gSampAllowWorldRemoval);
		SaveSettings();
	}
#ifdef _WIN32
	if(CPad::IsKeyJustDown('O')) SampImportDialog();
#endif
}

void
SampMenu(void)
{
	LoadSettings();

	// First frame: pick up what the server is already running, so a fresh
	// Ariane doesn't start empty (and a sync doesn't wipe the server's map).
	static bool autoLoaded = false;
	if(!autoLoaded){
		autoLoaded = true;
		int r0 = 0;
		if(gSampServerDir[0] && CountLayer(&r0) == 0 && r0 == 0 && LiveFileEntries() > 0)
			SampLoadFromServer();
	}

	if(!ImGui::BeginMenu("SA-MP"))
		return;
	int removed = 0, objects = CountLayer(&removed);
	ImGui::TextDisabled("Layer: %d objects, %d removed buildings", objects, removed);
	if(gSampModelsLoaded)
		ImGui::TextDisabled("SA-MP models: loaded from SAMP/");
	else
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "SA-MP models: SAMP/ folder not found");
	if(!gRawObjects.empty())
		ImGui::TextDisabled("(%d objects use models Ariane can't draw - kept for export)", (int)gRawObjects.size());
	ImGui::Separator();
	if(ImGui::MenuItem("SA-MP mode (protect game files)", nil, gSampProtectGameFiles)){
		gSampProtectGameFiles = !gSampProtectGameFiles;
		SaveSettings();
	}
	if(ImGui::IsItemHovered())
		ImGui::SetTooltip("On: new objects go to the SA-MP layer, deleting/moving stock buildings\n"
		                  "becomes RemoveBuildingForPlayer, and Save/Hot Reload can't touch your IPLs.\n"
		                  "Turning it off and saving would write layer deletions into the game files.");
	if(ImGui::MenuItem("Allow removing world buildings", "Ctrl+Shift+W", gSampAllowWorldRemoval, gSampProtectGameFiles)){
		SetWorldRemoval(!gSampAllowWorldRemoval);
		SaveSettings();
	}
	if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Lets you select game buildings outside the opened IPL (for example while an agent's\n"
		                  "map is open) and remove them with Delete. They export as RemoveBuildingForPlayer;\n"
		                  "they can't be moved or edited, and no IPL file is changed. Needs SA-MP mode.");
	if(ImGui::MenuItem("Ask before removing world buildings", nil, gSampAskWorldRemoval, gSampProtectGameFiles)){
		gSampAskWorldRemoval = !gSampAskWorldRemoval;
		SaveSettings();
	}
	ImGui::Separator();
#ifdef _WIN32
	if(ImGui::MenuItem("Import SA-MP / MTA map...", "Ctrl+Shift+O"))
		SampImportDialog();
#endif
	if(ImGui::MenuItem("Export Pawn...", "Ctrl+Shift+P")) gOpenExport = true;
	if(ImGui::MenuItem("Copy selection as Pawn", "Ctrl+Shift+C", false, selection.first != nil)) SampCopySelectionAsPawn();
	if(ImGui::IsItemHovered())
		ImGui::SetTooltip("Copies the selected objects as Pawn code in the export format (%s).",
		                  gSampExportFormat == SAMP_FORMAT_WRAPPER ? "wrapper" : gSampExportFormat == SAMP_FORMAT_STREAMER ? "streamer" : "native");
	StylesSelectionMenu();
	ImGui::Separator();
	if(ImGui::MenuItem("Sync to server", "Ctrl+Shift+S")) SampSyncToServer();
	if(ImGui::MenuItem("Show in game", "Ctrl+Shift+T")) SampShowInGame();
	if(ImGui::IsItemHovered())
		ImGui::SetTooltip("Sync, then move players who are in game to this view (the camera's target).\n"
		                  "Who moves is set under 'Show in game moves'. Needs tools/samp/ariane_live.pwn\n"
		                  "or a gamemode that reads scriptfiles/ariane_goto.txt.");
	if(ImGui::BeginMenu("Show in game moves")){
		static const char *labels[] = { "Admins logged in with RCON", "Players the server script picks",
			"One player", "Everyone on the server" };
		static const char *tips[] = {
			"Players who typed /rcon login <password> in game. Works on any server\nwith tools/samp/ariane_live.pwn loaded.",
			"The server decides: a gamemode or filterscript defines\n  forward Ariane_CanFollow(playerid);\n"
			"  public Ariane_CanFollow(playerid) return <your admin check>;\nand players it returns 1 for are moved.",
			"Only the player with this name (or id).",
			"Every connected player. Only for a local test server." };
		for(int m = 0; m < 4; m++){
			if(ImGui::MenuItem(labels[m], nil, gSampGotoWho == m)){
				gSampGotoWho = m;
				SaveSettings();
			}
			if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tips[m]);
		}
		ImGui::SetNextItemWidth(180);
		if(ImGui::InputTextWithHint("##gotoplayer", "player name or id", gSampGotoPlayer, sizeof(gSampGotoPlayer),
		                            ImGuiInputTextFlags_CharsNoBlank)){
			if(gSampGotoPlayer[0]) gSampGotoWho = GOTO_PLAYER;
			SaveSettings();
		}
		ImGui::EndMenu();
	}
	if(ImGui::MenuItem("Follow the server's focus requests", nil, gSampFollowFocus)){
		gSampFollowFocus = !gSampFollowFocus;
		SaveSettings();
	}
	if(ImGui::IsItemHovered())
		ImGui::SetTooltip("When a server script calls Ariane_FocusEditor(playerid) or Ariane_FocusEditorAt(x, y, z)\n"
		                  "(ariane.inc; /arianefocus in the bundled filterscript), the camera flies there.");
	if(ImGui::BeginMenu("Draw distance")){
		SampDrawDistanceSlider(true);
		ImGui::EndMenu();
	}
	if(ImGui::MenuItem("Reload from server", "Ctrl+Shift+L")) SampRequestReload();
	if(ImGui::IsItemHovered())
		ImGui::SetTooltip("Replace the SA-MP layer with what the server is running now\n(scriptfiles/ariane_live.txt from the last sync).");
	if(ImGui::MenuItem("Server folder...")) gOpenServer = true;
	ImGui::Separator();
	if(ImGui::MenuItem("Removed buildings...")) SampOpenRemovedWindow();
	if(ImGui::IsItemHovered())
		ImGui::SetTooltip("Every game building the map removes, nearest first: fly to it or restore it.");
	if(ImGui::MenuItem("Checks...")) SampOpenChecksWindow();
	if(ImGui::IsItemHovered())
		ImGui::SetTooltip("Find floating objects, objects on roads or dirt tracks, explosive props,\n"
		                  "duplicates and object-limit problems.");
	ImGui::Separator();
	if(ImGui::MenuItem("Clear SA-MP layer")) ClearLayer();
	ImGui::EndMenu();
}

// Live draw distance controls: map (SA-MP layer) objects and the world multiplier.
// Shown in SA-MP > Draw distance and in the Render panel.
void
SampDrawDistanceSlider(bool withWorld, bool compact)
{
	static bool dirty;
	static const char *mapTip =
		"Map objects: objects of the SA-MP layer are drawn at least this far, so a whole map\n"
		"shows from a distance. 0 = each model's own draw distance (small props vanish early).\n"
		"300 m is the streamer plugin's default stream distance. Editor only: in game,\n"
		"the streamer and each object's draw distance decide.";
	static const char *worldTip =
		"World: multiplier on every model's draw distance (game buildings too).\n"
		"Higher keeps detailed buildings instead of their LODs further away; slower.";
	if(compact){
		ImGui::TextUnformatted("Draw");
		ImGui::SameLine();
	}
	ImGui::SetNextItemWidth(compact ? 120.0f : 220.0f);
	ImGui::SliderFloat(compact ? "##sampddbar" : "Map objects##sampdd", &gSampLayerDrawDist, 0.0f, 3000.0f,
	                   gSampLayerDrawDist < 1.0f ? (compact ? "map: model" : "model distance")
	                                             : (compact ? "map %.0f m" : "%.0f m"),
	                   ImGuiSliderFlags_Logarithmic);
	if(ImGui::IsItemEdited()) dirty = true;
	if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", mapTip);
	if(withWorld){
		if(compact) ImGui::SameLine();
		ImGui::SetNextItemWidth(compact ? 90.0f : 220.0f);
		ImGui::SliderFloat(compact ? "##sampddworldbar" : "World##sampddworld", &TheCamera.m_LODmult, 0.5f, 5.0f,
		                   compact ? "world x%.1f" : "x%.2f");
		if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s", worldTip);
	}
	if(dirty && !ImGui::IsAnyItemActive()){
		dirty = false;
		if(gSampLayerDrawDist < 1.0f) gSampLayerDrawDist = 0.0f;
		SaveSettings();
	}
}

void
SampPopups(void)
{
	if(gOpenExport){ ImGui::OpenPopup("SA-MP export"); gOpenExport = false; }
	if(gOpenServer){ ImGui::OpenPopup("SA-MP server"); gOpenServer = false; }
	if(gOpenConfirmEmptySync){ ImGui::OpenPopup("Sync an empty layer?"); gOpenConfirmEmptySync = false; }
	if(gOpenConfirmReload){ ImGui::OpenPopup("Reload from server?"); gOpenConfirmReload = false; }
	if(gOpenConfirmWorldRemoval){ ImGui::OpenPopup("Remove world buildings?"); gOpenConfirmWorldRemoval = false; }

	if(ImGui::BeginPopupModal("Remove world buildings?", nil, ImGuiWindowFlags_AlwaysAutoResize)){
		std::vector<ObjectInst*> world;
		int others = 0;
		for(CPtrNode *p = selection.first; p; p = p->next){
			ObjectInst *inst = (ObjectInst*)p->item;
			if(inst->m_isDeleted) continue;
			if(SampIsWorldBuilding(inst)) world.push_back(inst);
			else others++;
		}
		static bool dontAsk = false;
		if(world.empty())
			ImGui::CloseCurrentPopup();	// selection changed meanwhile
		ImGui::Text("Remove %d world building%s from the map?", (int)world.size(), world.size() == 1 ? "" : "s");
		ImGui::Spacing();
		for(size_t i = 0; i < world.size() && i < 8; i++){
			ObjectDef *obj = GetObjectDef(world[i]->m_objectId);
			ImGui::BulletText("%s (%d)  at %.1f, %.1f, %.1f", obj ? obj->m_name : "?", world[i]->m_objectId,
				world[i]->m_translation.x, world[i]->m_translation.y, world[i]->m_translation.z);
		}
		if(world.size() > 8)
			ImGui::TextDisabled("...and %d more", (int)world.size() - 8);
		if(others > 0)
			ImGui::Text("%d other selected object%s will be deleted as well.", others, others == 1 ? "" : "s");
		ImGui::Spacing();
		ImGui::TextDisabled("They're hidden here and exported as RemoveBuildingForPlayer, together with\n"
		                    "their far-away LOD models. Your game's IPL files are not changed.\n"
		                    "Ctrl+Z brings them back.");
		ImGui::Spacing();
		ImGui::Checkbox("Don't ask again", &dontAsk);
		ImGui::SetItemTooltip("Turn it back on in SA-MP > Ask before removing world buildings");
		ImGui::Spacing();
		if(ImGui::Button("Remove", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)){
			if(dontAsk){
				gSampAskWorldRemoval = false;
				SaveSettings();
			}
			dontAsk = false;
			DoDeleteSelection();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if(ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)){
			dontAsk = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if(ImGui::BeginPopupModal("Sync an empty layer?", nil, ImGuiWindowFlags_AlwaysAutoResize)){
		ImGui::Text("Your SA-MP layer is empty, but the server is running %d objects/removals.", gPendingServerEntries);
		ImGui::Text("Syncing now would wipe the server's map (and %s.inc).", gSampMapName);
		ImGui::Spacing();
		if(ImGui::Button("Load the server's map", ImVec2(200, 0))){
			SampLoadFromServer();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if(ImGui::Button("Wipe it anyway", ImVec2(140, 0))){
			DoSyncToServer(true);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if(ImGui::Button("Cancel", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if(ImGui::BeginPopupModal("Reload from server?", nil, ImGuiWindowFlags_AlwaysAutoResize)){
		ImGui::Text("Replace the current SA-MP layer with the server's map?");
		ImGui::TextDisabled("Anything you haven't synced yet will be lost.");
		ImGui::Spacing();
		if(ImGui::Button("Reload", ImVec2(120, 0))){
			SampLoadFromServer();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if(ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if(ImGui::BeginPopupModal("SA-MP export", nil, ImGuiWindowFlags_AlwaysAutoResize)){
		ImGui::InputText("Map name", gSampMapName, sizeof(gSampMapName));
		ImGui::Combo("Format", &gSampExportFormat, gFormatNames, 3);
		if(gSampExportFormat == SAMP_FORMAT_WRAPPER){
			ImGui::InputText("Function prefix", gSampWrapPrefix, sizeof(gSampWrapPrefix));
			ImGui::TextDisabled("Writes %sCreateObject(model, x, y, z, rx, ry, rz, world, interior),\n"
			                    "%sRemoveBuilding(model, x, y, z, radius), %sSetObjectMaterial(...) - define them in your script.",
			                    gSampWrapPrefix, gSampWrapPrefix, gSampWrapPrefix);
		}else if(gSampExportFormat == SAMP_FORMAT_NATIVE)
			ImGui::TextDisabled("open.mp and SA-MP share these natives (CreateObject, SetObjectMaterial, RemoveBuildingForPlayer).");
		if(ImGui::Button("Save as...", ImVec2(140, 0))){
			char path[1024];
			snprintf(path, sizeof(path), "%s.inc", gSampMapName);
			SanitizeName(path);
			size_t l = strlen(path);
			if(l > 4) memcpy(path + l - 4, ".inc", 4);
#ifdef _WIN32
			if(FileDialog(true, path, sizeof(path), "Pawn include (*.inc)\0*.inc\0Pawn (*.pwn)\0*.pwn\0", "inc"))
#endif
			{
				ExportLists lists;
				CollectExport(&lists);
				if(WritePawn(path, gSampMapName, gSampExportFormat, lists))
					Toast(TOAST_SAVE, "Exported %d objects, %d removals", (int)lists.objects.size(), (int)lists.removals.size());
				else
					Toast(TOAST_SAVE, "Export failed: can't write %s", path);
				SaveSettings();
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::SameLine();
		if(ImGui::Button("Cancel", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if(ImGui::BeginPopupModal("SA-MP server", nil, ImGuiWindowFlags_AlwaysAutoResize)){
		ImGui::Text("open.mp server folder (the one with omp-server.exe and scriptfiles/)");
		ImGui::InputText("##server", gSampServerDir, sizeof(gSampServerDir));
		ImGui::InputText("Map name", gSampMapName, sizeof(gSampMapName));
		ImGui::Checkbox("Also write a Pawn include on sync", &gSampAutoInc);
		if(gSampAutoInc){
			ImGui::InputText("Include folder (in the server folder)", gSampIncDir, sizeof(gSampIncDir));
			ImGui::Combo("Include format", &gSampExportFormat, gFormatNames, 3);
			ImGui::TextDisabled("Writes <server>/%s/%s.inc", gSampIncDir, gSampMapName);
		}
		if(ImGui::Button("Save", ImVec2(120, 0))){
			SaveSettings();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if(ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

// ------------------------------------------------------------------ custom models
//
// open.mp downloads custom models listed in <server>/models/artconfig.txt:
//   AddSimpleModel(virtualworld, baseid, newid, "file.dff", "file.txd");
//   AddSimpleModelTimed(virtualworld, baseid, newid, "file.dff", "file.txd", timeon, timeoff);
// newid is negative (-1000 .. -30000). Ariane registers each as a runtime
// object definition at a free internal id (30000+) that borrows the base
// model's collision and flags; layer objects keep the SA-MP id on export.

struct SampCustomModel
{
	int sampId, internalId, baseId, world;
	std::string dff, txd, dffPath, txdPath;
	bool ok;
	std::string error;
};
static std::vector<SampCustomModel> gCustomModels;
static std::map<int, int> gCustomToInternal, gInternalToCustom;
static std::map<int, std::string> gCustomDffPath;	// internal id -> dff path
static int gNextCustomInternal = 30000;
static char gCustomModelsDir[1024];
static GameFile *gArtconfigFile;

int
SampResolveModel(int model)
{
	if(model >= 0)
		return model;
	std::map<int, int>::iterator it = gCustomToInternal.find(model);
	return it == gCustomToInternal.end() ? -1 : it->second;
}

int
SampExportModel(int model)
{
	std::map<int, int>::iterator it = gInternalToCustom.find(model);
	return it == gInternalToCustom.end() ? model : it->second;
}

const char*
SampCustomModelDff(int internalId)
{
	if(gCustomDffPath.empty())
		return nil;
	std::map<int, std::string>::iterator it = gCustomDffPath.find(internalId);
	return it == gCustomDffPath.end() ? nil : it->second.c_str();
}

static bool
FileExistsAbs(const char *path)
{
	FILE *f = fopen(path, "rb");
	if(f == nil) return false;
	fclose(f);
	return true;
}

static void
BaseName(const std::string &file, char *out, size_t size)
{
	size_t s = file.find_last_of("/\\");
	std::string b = s == std::string::npos ? file : file.substr(s + 1);
	size_t dot = b.find_last_of('.');
	if(dot != std::string::npos) b = b.substr(0, dot);
	strncpy(out, b.c_str(), size - 1);
	out[size - 1] = '\0';
}

static bool
RegisterCustomModel(SampCustomModel &cm, int timeOn, int timeOff)
{
	ObjectDef *base = GetObjectDef(cm.baseId);
	if(base == nil){ cm.error = "base model unknown"; return false; }
	if(!FileExistsAbs(cm.dffPath.c_str())){ cm.error = "dff not found: " + cm.dffPath; return false; }
	if(!FileExistsAbs(cm.txdPath.c_str())){ cm.error = "txd not found: " + cm.txdPath; return false; }
	int id;
	std::map<int, int>::iterator known = gCustomToInternal.find(cm.sampId);
	if(known != gCustomToInternal.end())
		id = known->second;			// reload: keep the id placed objects use
	else{
		while(gNextCustomInternal < NUMOBJECTDEFS && GetObjectDef(gNextCustomInternal)) gNextCustomInternal++;
		if(gNextCustomInternal >= NUMOBJECTDEFS){ cm.error = "no free object ids"; return false; }
		id = gNextCustomInternal++;
	}
	ObjectDef *obj = GetObjectDef(id);
	if(obj == nil){
		obj = AddObjectDef(id);
		if(obj == nil){ cm.error = "could not allocate object definition"; return false; }
		// borrow the base model's flags, draw distance and collision
		memcpy(obj, base, sizeof(ObjectDef));
		obj->m_id = id;
		obj->m_type = ObjectDef::ATOMIC;
		obj->m_numAtomics = 1;
		obj->m_atomics[0] = obj->m_atomics[1] = obj->m_atomics[2] = nil;
		obj->m_drawDist[0] = base->GetLargestDrawDist();
		obj->m_clump = nil;
		obj->m_animname[0] = '\0';
		obj->m_animAsset = nil;
		obj->m_imageIndex = -1;
		obj->m_cantLoad = false;
		obj->m_hasPreRendered = false;
		obj->m_isBigBuilding = false;
		obj->m_isHidden = false;
		obj->m_relatedModel = obj->m_relatedTimeModel = nil;
		obj->m_effectIndex = -1;
		obj->m_numEffects = 0;
		obj->m_pedPathIndex = obj->m_carPathIndex = -1;
		obj->m_isTimed = timeOn != timeOff;
		obj->m_timeOn = timeOn;
		obj->m_timeOff = timeOff;
		char name[MODELNAMELEN];
		BaseName(cm.dff, name, sizeof(name));
		strncpy(obj->m_name, name, MODELNAMELEN);
		obj->m_name[MODELNAMELEN-1] = '\0';
		if(gArtconfigFile == nil){
			static char artName[] = "models\\artconfig.txt";
			gArtconfigFile = NewGameFile(artName);
		}
		obj->m_file = gArtconfigFile;
	}
	// own txd slot ("samp:" + name) so a stock txd with the same name isn't touched
	char txdBase[MODELNAMELEN], slotName[MODELNAMELEN];
	BaseName(cm.txd, txdBase, sizeof(txdBase));
	snprintf(slotName, sizeof(slotName), "samp:%s", txdBase);
	int slot = AddTxdSlot(slotName);
	if(!IsTxdLoaded(slot))
		LoadTxd(slot, cm.txdPath.c_str());
	obj->m_txdSlot = slot;
	gCustomDffPath[id] = cm.dffPath;
	gCustomToInternal[cm.sampId] = id;
	gInternalToCustom[id] = cm.sampId;
	cm.internalId = id;
	RequestObject(id);
	LoadAllRequestedObjects();
	if(obj->m_atomics[0] == nil){ cm.error = "dff did not load (not a simple single-atomic model?)"; return false; }
	return true;
}

// Reads <dir>/artconfig.txt (dir defaults to <server>/models). Returns models loaded.
int
SampLoadCustomModels(const char *dir)
{
	char path[1200];
	if(dir && dir[0])
		strncpy(gCustomModelsDir, dir, sizeof(gCustomModelsDir) - 1);
	else if(gSampServerDir[0])
		JoinPath(gCustomModelsDir, sizeof(gCustomModelsDir), gSampServerDir, "models");
	else
		return 0;
	JoinPath(path, sizeof(path), gCustomModelsDir, "artconfig.txt");
	FILE *f = fopen(path, "rb");
	if(f == nil){
		gCustomModels.clear();		// listing only; definitions already registered stay usable
		return 0;
	}
	std::string text;
	char buf[16384];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
	fclose(f);
	StripComments(text);
	gCustomModels.clear();
	int loaded = 0;
	std::vector<std::string> args;
	const char *calls[] = { "AddSimpleModelTimed", "AddSimpleModel" };
	size_t pos = 0;
	while(pos < text.size()){
		size_t best = std::string::npos;
		int which = -1;
		for(int c = 0; c < 2; c++){
			size_t p = text.find(calls[c], pos);
			if(p < best){ best = p; which = c; }
		}
		if(best == std::string::npos) break;
		size_t open = text.find('(', best);
		if(open == std::string::npos) break;
		pos = ParseArgs(text, open + 1, args);
		double v[3];
		SampCustomModel cm;
		cm.ok = false;
		cm.internalId = -1;
		std::string dff, txd;
		if(args.size() < 5 || !ArgNum(args, 0, &v[0]) || !ArgNum(args, 1, &v[1]) || !ArgNum(args, 2, &v[2]) ||
		   !ArgString(args, 3, &dff) || !ArgString(args, 4, &txd))
			continue;
		cm.world = (int)v[0]; cm.baseId = (int)v[1]; cm.sampId = (int)v[2];
		cm.dff = dff; cm.txd = txd;
		char p1[1200], p2[1200];
		JoinPath(p1, sizeof(p1), gCustomModelsDir, dff.c_str());
		JoinPath(p2, sizeof(p2), gCustomModelsDir, txd.c_str());
		cm.dffPath = p1; cm.txdPath = p2;
		double on = 0, off = 0;
		if(which == 0 && args.size() >= 7){ ArgNum(args, 5, &on); ArgNum(args, 6, &off); }
		cm.ok = RegisterCustomModel(cm, (int)on, (int)off);
		if(cm.ok) loaded++;
		else log("SA-MP custom model %d (%s): %s\n", cm.sampId, dff.c_str(), cm.error.c_str());
		gCustomModels.push_back(cm);
	}
	log("SA-MP: %d of %d custom models loaded from %s\n", loaded, (int)gCustomModels.size(), path);
	return loaded;
}

std::string
SampCustomModelsJson(void)
{
	std::string s = "\"dir\":" + JsonStr(gCustomModelsDir) + ",\"models\":[";
	for(size_t i = 0; i < gCustomModels.size(); i++){
		const SampCustomModel &cm = gCustomModels[i];
		char b[200];
		snprintf(b, sizeof(b), "%s{\"samp_id\":%d,\"model_id\":%d,\"base_id\":%d,\"world\":%d,\"ok\":%s,\"dff\":",
			i ? "," : "", cm.sampId, cm.internalId, cm.baseId, cm.world, cm.ok ? "true" : "false");
		s += b + JsonStr(cm.dff) + ",\"txd\":" + JsonStr(cm.txd);
		if(!cm.ok) s += ",\"error\":" + JsonStr(cm.error);
		s += "}";
	}
	return s + "]";
}

// ================================================================== SA-MP windows
//
// Removed Buildings (every RemoveBuildingForPlayer the map makes, with Go to / Restore), SA-MP Checks
// (common mapping mistakes, each with Go to / Select), Show in game, and the material editor in Object Info.

#include "icons.h"
#include <algorithm>
#include <unordered_map>
#include <chrono>
#include <set>

static bool gShowRemovedWindow, gShowChecksWindow;

void SampOpenRemovedWindow(void) { gShowRemovedWindow = true; }
void SampOpenChecksWindow(void) { gShowChecksWindow = true; }

static bool
ContainsNoCase(const char *hay, const char *needle)
{
	if(needle == nil || needle[0] == '\0') return true;
	if(hay == nil) return false;
	size_t n = strlen(needle);
	for(const char *h = hay; *h; h++){
		size_t i = 0;
		while(i < n && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) i++;
		if(i == n) return true;
	}
	return false;
}

static void
FlyTo(ObjectInst *inst, rw::V3d pos)
{
	if(inst && GetObjectDef(inst->m_objectId) && GetObjectDef(inst->m_objectId)->m_colModel)
		inst->JumpTo();
	else{
		TheCamera.setTarget(pos);
		TheCamera.setDistanceFromTarget(30.0f);
	}
}

// Put a removed game building back, with the far-away LOD its removal took along.
static void
RestoreWorldBuilding(ObjectInst *inst)
{
	if(inst == nil || !inst->m_isDeleted)
		return;
	inst->m_isDeleted = false;
	StampChangeSeq(inst);
	ObjectInst *lod = inst->m_lod;
	if(lod && lod->m_isDeleted && IsStockInst(lod)){
		lod->m_isDeleted = false;
		StampChangeSeq(lod);
	}
}

static void
RemovedBuildingsWindow(void)
{
	if(!gShowRemovedWindow)
		return;
	ImGui::SetNextWindowSize(ImVec2(620, 460), ImGuiCond_FirstUseEver);
	if(!ImGui::Begin(ICON_FA_TRASH " Removed Buildings", &gShowRemovedWindow)){
		ImGui::End();
		return;
	}
	static char filter[64] = "";
	static bool showLods = false;
	static std::set<ObjectInst*> picked;	// highlighted rows (removed buildings aren't in the viewport selection)
	static ObjectInst *anchor;			// shift-click range start
	static int pickedRaw = -1;
	struct Row { ObjectInst *inst; float dist; bool lod; };
	std::vector<Row> rows;
	int buildings = 0, lods = 0;
	rw::V3d cam = TheCamera.m_position;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(!inst->m_isDeleted || !IsStockInst(inst))
			continue;
		bool isLod = inst->m_numChildren > 0;
		if(isLod) lods++; else buildings++;
		if(isLod && !showLods)
			continue;
		ObjectDef *obj = GetObjectDef(inst->m_objectId);
		char idText[16];
		snprintf(idText, sizeof(idText), "%d", inst->m_objectId);
		if(filter[0] && !ContainsNoCase(obj ? obj->m_name : "", filter) && strstr(idText, filter) == nil)
			continue;
		Row r = { inst, length(sub(inst->m_translation, cam)), isLod };
		rows.push_back(r);
	}
	std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b){ return a.dist < b.dist; });
	// forget picks that were restored or filtered out
	std::vector<ObjectInst*> pickedRows;
	for(size_t i = 0; i < rows.size(); i++)
		if(picked.count(rows[i].inst))
			pickedRows.push_back(rows[i].inst);
	picked = std::set<ObjectInst*>(pickedRows.begin(), pickedRows.end());
	int n = (int)pickedRows.size();

	ImGui::Text("%d removed building%s", buildings, buildings == 1 ? "" : "s");
	ImGui::SameLine();
	ImGui::TextDisabled("(+%d LOD model%s, removed with them) - each exports as RemoveBuildingForPlayer",
		lods, lods == 1 ? "" : "s");

	// toolbar: acts on the highlighted rows
	ImGui::BeginDisabled(n == 0);
	if(ImGui::Button("Go to"))
		FlyTo(pickedRows[0], pickedRows[0]->m_translation);
	ImGui::SetItemTooltip("Fly to the first highlighted building (double-click a row does the same)");
	ImGui::SameLine();
	char restoreLabel[32];
	snprintf(restoreLabel, sizeof(restoreLabel), n > 1 ? "Restore %d" : "Restore", n);
	if(ImGui::Button(restoreLabel)){
		for(size_t i = 0; i < pickedRows.size(); i++)
			RestoreWorldBuilding(pickedRows[i]);
		Toast(TOAST_DELETE, "Restored %d building(s): they're no longer removed in the export", n);
		picked.clear();
	}
	ImGui::SetItemTooltip("Put the highlighted buildings back (and their LODs). Players already on the\n"
	                      "server only see them again after they reconnect.");
	ImGui::EndDisabled();
	ImGui::SameLine(0, 16);
	ImGui::SetNextItemWidth(200);
	ImGui::InputTextWithHint("##filter", "Filter by name or id", filter, sizeof(filter));
	ImGui::SameLine();
	ImGui::Checkbox("LOD models", &showLods);

	float footer = ImGui::GetTextLineHeightWithSpacing() * (gRawRemovals.empty() ? 1.2f : 2.6f);
	if(ImGui::BeginTable("##removed", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
	                     ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable,
	                     ImVec2(0, -footer))){
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed, 70);
		ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthFixed, 160);
		ImGui::TableHeadersRow();
		int clicked = -1;
		bool doubleClicked = false;
		ImGuiListClipper clipper;
		clipper.Begin((int)rows.size());
		while(clipper.Step())
			for(int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++){
				ObjectInst *inst = rows[i].inst;
				ObjectDef *obj = GetObjectDef(inst->m_objectId);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				char label[128];
				snprintf(label, sizeof(label), "%s (%d)%s##%p", obj ? obj->m_name : "?", inst->m_objectId,
					rows[i].lod ? "  LOD" : "", (void*)inst);
				if(ImGui::Selectable(label, picked.count(inst) != 0,
				                     ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)){
					clicked = i;
					doubleClicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
				}
				ImGui::TableNextColumn();
				ImGui::Text("%.0f m", rows[i].dist);
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%.1f, %.1f, %.1f", inst->m_translation.x, inst->m_translation.y, inst->m_translation.z);
			}
		ImGui::EndTable();
		if(clicked >= 0){
			ObjectInst *inst = rows[clicked].inst;
			ImGuiIO &io = ImGui::GetIO();
			if(io.KeyShift && anchor){
				int a = -1;
				for(int i = 0; i < (int)rows.size(); i++) if(rows[i].inst == anchor) a = i;
				if(a >= 0){
					if(!io.KeyCtrl) picked.clear();
					for(int i = a < clicked ? a : clicked; i <= (a < clicked ? clicked : a); i++)
						picked.insert(rows[i].inst);
				}
			}else if(io.KeyCtrl){
				if(picked.count(inst)) picked.erase(inst); else picked.insert(inst);
				anchor = inst;
			}else{
				picked.clear();
				picked.insert(inst);
				anchor = inst;
			}
			if(doubleClicked)
				FlyTo(inst, inst->m_translation);
		}
	}
	ImGui::TextDisabled("Click highlights, Ctrl+click adds, Shift+click a range, double-click flies there.");
	if(!gRawRemovals.empty()){
		ImGui::Text("%d imported removal%s match no building Ariane loaded (kept for export)",
			(int)gRawRemovals.size(), gRawRemovals.size() == 1 ? "" : "s");
		ImGui::SameLine();
		if(ImGui::SmallButton("Show"))
			ImGui::OpenPopup("##rawremovals");
		if(ImGui::BeginPopup("##rawremovals")){
			if(pickedRaw >= (int)gRawRemovals.size()) pickedRaw = -1;
			ImGui::BeginDisabled(pickedRaw < 0);
			if(ImGui::Button("Go to")){
				const SampRawRemoval &r = gRawRemovals[pickedRaw];
				rw::V3d p = { r.x, r.y, r.z };
				FlyTo(nil, p);
			}
			ImGui::SameLine();
			if(ImGui::Button("Drop")){
				gRawRemovals.erase(gRawRemovals.begin() + pickedRaw);
				pickedRaw = -1;
			}
			ImGui::SetItemTooltip("Stop exporting this removal");
			ImGui::EndDisabled();
			for(size_t i = 0; i < gRawRemovals.size(); i++){
				const SampRawRemoval &r = gRawRemovals[i];
				char label[128];
				snprintf(label, sizeof(label), "model %d at %.1f, %.1f, %.1f (radius %.1f)##raw%d",
					r.model, r.x, r.y, r.z, r.radius, (int)i);
				if(ImGui::Selectable(label, pickedRaw == (int)i, ImGuiSelectableFlags_DontClosePopups |
				                     ImGuiSelectableFlags_AllowDoubleClick)){
					pickedRaw = (int)i;
					if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)){
						rw::V3d p = { r.x, r.y, r.z };
						FlyTo(nil, p);
					}
				}
			}
			ImGui::EndPopup();
		}
	}
	ImGui::End();
}

// ------------------------------------------------------------------ checks

enum { CHK_FLOATING, CHK_ROAD, CHK_TRACK, CHK_EXPLOSIVE, CHK_DUPLICATE, CHK_LIMIT, CHK_DENSITY, CHK_NOTDRAWN, NUM_CHECKS };
static const char *kCheckNames[NUM_CHECKS] = {
	"Floating", "On a road", "On a dirt track", "Explosive prop", "Duplicate", "Object limit", "Streamer density", "Not drawn"
};
static const char *kCheckHelp[NUM_CHECKS] = {
	"Nothing under the object within 35 cm: it hangs in the air (or over water).",
	"Stands on tarmac: vehicles drive into it.",
	"Stands on a dirt track (DIRTTRACK surface).",
	"Explodes when shot or hit (1225 barrel, 1370 flame drum).",
	"Two copies of the same model at the same spot: they flicker (z-fighting) and cost an object slot.",
	"More objects than CreateObject allows on SA-MP (1000): export with the streamer format instead.",
	"More objects within 300 m than the streamer shows at once by default (500): distant ones pop in late.",
	"Objects with models Ariane can't draw: kept and exported as they are, but not checked.",
};
static bool gCheckEnabled[NUM_CHECKS] = { true, true, true, true, true, true, true, true };

struct CheckIssue { int kind; ObjectInst *inst; rw::V3d pos; std::string msg; };
static std::vector<CheckIssue> gIssues;
static bool gChecksRan;
static double gChecksMs;
static int gChecksObjects;

static bool
IsRoadSurface(int s) { return s >= 1 && s <= 3; }	// TARMAC, TARMAC_FUCKED, TARMAC_REALLYFUCKED (surfinfo.dat)
static bool
IsTrackSurface(int s) { return s == 27; }			// DIRTTRACK

static void
RunChecks(void)
{
	std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
	gIssues.clear();
	gChecksRan = true;
	// barriers and gate parts are meant to stand on roads and tracks
	static const int kRoadOk[] = { 1423, 976, 988, 969, 975, 1228, 1237, 1238, 1282, 1459, 3091 };
	// may hang in the air: ground slabs laid as the ground itself, and dressing that hugs walls (scaffolding...)
	static const int kFloatOk[] = { 18981, 19543, 19544, 19545, 19650, 19700, 19741,
		1426, 1436, 1464, 1465, 1466, 1467, 1469, 1519, 1521 };
	std::vector<ObjectInst*> objs;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(SampIsLayerInst(inst) && !inst->m_isDeleted && inst->m_numChildren == 0)
			objs.push_back(inst);
	}
	gChecksObjects = (int)objs.size();
	char msg[256];
	std::unordered_map<long long, std::vector<ObjectInst*>> cells;

	for(size_t i = 0; i < objs.size(); i++){
		ObjectInst *inst = objs[i];
		ObjectDef *obj = GetObjectDef(inst->m_objectId);
		const char *name = obj ? obj->m_name : "?";
		inst->UpdateMatrix();

		if(inst->m_objectId == 1225 || inst->m_objectId == 1370){
			snprintf(msg, sizeof(msg), "%s (%d) explodes when shot", name, inst->m_objectId);
			gIssues.push_back({ CHK_EXPLOSIVE, inst, inst->m_translation, msg });
		}

		// duplicates: same model within 5 cm and the same rotation
		long long key = ((long long)inst->m_objectId << 40) ^
			((long long)floorf(inst->m_translation.x * 20.0f) * 73856093LL) ^
			((long long)floorf(inst->m_translation.y * 20.0f) * 19349663LL) ^
			((long long)floorf(inst->m_translation.z * 20.0f) * 83492791LL);
		std::vector<ObjectInst*> &cell = cells[key];
		for(size_t k = 0; k < cell.size(); k++){
			ObjectInst *o = cell[k];
			rw::Quat a = inst->m_rotation, b = o->m_rotation;
			float dq = fabsf(a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w);
			if(length(sub(o->m_translation, inst->m_translation)) < 0.05f && dq > 0.9999f){
				snprintf(msg, sizeof(msg), "%s (%d) placed twice at the same spot", name, inst->m_objectId);
				gIssues.push_back({ CHK_DUPLICATE, inst, inst->m_translation, msg });
				break;
			}
		}
		cell.push_back(inst);

		if(obj == nil || obj->m_colModel == nil)
			continue;
		CColModel *col = obj->m_colModel;
		bool solid = col->numBoxes > 0 || col->numSpheres > 0 || col->numTriangles > 0;
		bool doFloat = gCheckEnabled[CHK_FLOATING] && solid &&
			std::find(std::begin(kFloatOk), std::end(kFloatOk), inst->m_objectId) == std::end(kFloatOk);
		bool doGround = isSA() && (gCheckEnabled[CHK_ROAD] || gCheckEnabled[CHK_TRACK]) &&
			std::find(std::begin(kRoadOk), std::end(kRoadOk), inst->m_objectId) == std::end(kRoadOk);
		if(!doFloat && !doGround)
			continue;

		// bottom of the collision box in the world, and five points under it (centre + corners pulled in 25%)
		CBox &bb = obj->m_colModel->boundingBox;
		rw::V3d corners[8], world[8];
		for(int c = 0; c < 8; c++){
			corners[c].x = c & 1 ? bb.max.x : bb.min.x;
			corners[c].y = c & 2 ? bb.max.y : bb.min.y;
			corners[c].z = c & 4 ? bb.max.z : bb.min.z;
		}
		rw::V3d::transformPoints(world, corners, 8, &inst->m_matrix);
		float minZ = world[0].z;
		for(int c = 1; c < 8; c++) minZ = fminf(minZ, world[c].z);
		rw::V3d local[5], samples[5];
		rw::V3d mid = { (bb.min.x + bb.max.x) * 0.5f, (bb.min.y + bb.max.y) * 0.5f, bb.min.z };
		local[0] = mid;
		for(int c = 0; c < 4; c++){
			local[c + 1].x = mid.x + ((c & 1 ? bb.max.x : bb.min.x) - mid.x) * 0.75f;
			local[c + 1].y = mid.y + ((c & 2 ? bb.max.y : bb.min.y) - mid.y) * 0.75f;
			local[c + 1].z = bb.min.z;
		}
		rw::V3d::transformPoints(samples, local, 5, &inst->m_matrix);

		if(doFloat){
			float bestGap = 1.0e30f;
			for(int s = 0; s < 5; s++){
				rw::V3d from = { samples[s].x, samples[s].y, minZ + 1.0f };
				float hz;
				if(SurfaceBelow(inst, from, false, &hz, nil, nil))
					bestGap = fminf(bestGap, minZ - hz);
			}
			if(bestGap > 0.35f){
				if(bestGap >= 1.0e29f)
					snprintf(msg, sizeof(msg), "%s (%d) has nothing below it", name, inst->m_objectId);
				else
					snprintf(msg, sizeof(msg), "%s (%d) floats %.2f m above what's below", name, inst->m_objectId, bestGap);
				gIssues.push_back({ CHK_FLOATING, inst, inst->m_translation, msg });
			}
		}
		if(doGround){
			int road = 0, track = 0;
			bool centreRoad = false, centreTrack = false;
			for(int s = 0; s < 5; s++){
				rw::V3d from = { samples[s].x, samples[s].y, minZ + 1.0f };
				int surface = -1;
				if(!SurfaceBelow(inst, from, true, nil, &surface, nil))
					continue;
				if(IsRoadSurface(surface)){ road++; if(s == 0) centreRoad = true; }
				if(IsTrackSurface(surface)){ track++; if(s == 0) centreTrack = true; }
			}
			if(gCheckEnabled[CHK_ROAD] && (centreRoad || road >= 2)){
				snprintf(msg, sizeof(msg), "%s (%d) stands on a road", name, inst->m_objectId);
				gIssues.push_back({ CHK_ROAD, inst, inst->m_translation, msg });
			}else if(gCheckEnabled[CHK_TRACK] && (centreTrack || track >= 2)){
				snprintf(msg, sizeof(msg), "%s (%d) stands on a dirt track", name, inst->m_objectId);
				gIssues.push_back({ CHK_TRACK, inst, inst->m_translation, msg });
			}
		}
	}

	ExportLists l;
	CollectExport(&l);
	int total = (int)l.objects.size();
	if(gSampExportFormat == SAMP_FORMAT_NATIVE && total > 1000){
		snprintf(msg, sizeof(msg), "%d objects, but SA-MP's CreateObject allows 1000 - use the streamer format", total);
		gIssues.push_back({ CHK_LIMIT, nil, TheCamera.m_target, msg });
	}
	if(gSampExportFormat != SAMP_FORMAT_NATIVE && objs.size() > 500){
		// densest spot: most objects within 300 m (2D)
		int best = 0;
		ObjectInst *bestInst = nil;
		for(size_t i = 0; i < objs.size(); i += objs.size() > 4000 ? 4 : 1){
			int n = 0;
			for(size_t k = 0; k < objs.size(); k++){
				float dx = objs[k]->m_translation.x - objs[i]->m_translation.x;
				float dy = objs[k]->m_translation.y - objs[i]->m_translation.y;
				if(dx*dx + dy*dy < 300.0f*300.0f) n++;
			}
			if(n > best){ best = n; bestInst = objs[i]; }
		}
		if(best > 500){
			snprintf(msg, sizeof(msg), "%d objects within 300 m of here: the streamer shows 500 at once by default", best);
			gIssues.push_back({ CHK_DENSITY, bestInst, bestInst->m_translation, msg });
		}
	}
	if(!gRawObjects.empty()){
		snprintf(msg, sizeof(msg), "%d object%s use models Ariane can't draw", (int)gRawObjects.size(),
			gRawObjects.size() == 1 ? "" : "s");
		rw::V3d p = { gRawObjects[0].x, gRawObjects[0].y, gRawObjects[0].z };
		gIssues.push_back({ CHK_NOTDRAWN, nil, p, msg });
	}
	gChecksMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

static void
ChecksWindow(void)
{
	if(!gShowChecksWindow)
		return;
	ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_FirstUseEver);
	if(!ImGui::Begin(ICON_FA_WRENCH " SA-MP Checks", &gShowChecksWindow)){
		ImGui::End();
		return;
	}
	if(!gChecksRan)
		RunChecks();
	if(ImGui::Button(ICON_FA_ROTATE " Run checks"))
		RunChecks();
	ImGui::SameLine();
	ImGui::TextDisabled("%d map objects checked in %.0f ms", gChecksObjects, gChecksMs);

	int counts[NUM_CHECKS] = {};
	for(size_t i = 0; i < gIssues.size(); i++) counts[gIssues[i].kind]++;
	static bool show[NUM_CHECKS] = { true, true, true, true, true, true, true, true };
	for(int k = 0; k < NUM_CHECKS; k++){
		if(k % 4) ImGui::SameLine();
		char label[64];
		snprintf(label, sizeof(label), "%s (%d)##chk%d", kCheckNames[k], counts[k], k);
		if(counts[k] > 0)
			ImGui::PushStyleColor(ImGuiCol_Text, k == CHK_NOTDRAWN ? ImVec4(0.7f, 0.7f, 0.7f, 1) : ImVec4(1.0f, 0.75f, 0.35f, 1));
		ImGui::Checkbox(label, &show[k]);
		if(counts[k] > 0)
			ImGui::PopStyleColor();
		ImGui::SetItemTooltip("%s", kCheckHelp[k]);
	}
	if(gIssues.empty()){
		ImGui::Spacing();
		ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "No problems found.");
		ImGui::End();
		return;
	}
	// toolbar: acts on the issues whose objects are selected
	std::vector<int> pickedIssues;
	for(size_t i = 0; i < gIssues.size(); i++)
		if(show[gIssues[i].kind] && gIssues[i].inst && !gIssues[i].inst->m_isDeleted && gIssues[i].inst->m_selected)
			pickedIssues.push_back((int)i);
	ImGui::BeginDisabled(pickedIssues.empty());
	if(ImGui::Button("Go to")){
		CheckIssue &it = gIssues[pickedIssues[0]];
		FlyTo(it.inst, it.pos);
	}
	ImGui::SetItemTooltip("Fly to the first selected problem (double-click a row does the same)");
	ImGui::EndDisabled();
	ImGui::SameLine();
	if(!pickedIssues.empty()) ImGui::Text("%d selected - Delete deletes them, G snaps them to the ground", (int)pickedIssues.size());
	else ImGui::TextDisabled("Click a problem to select its object");

	float footer = ImGui::GetTextLineHeightWithSpacing() * 1.2f;
	if(ImGui::BeginTable("##issues", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
	                     ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable, ImVec2(0, -footer))){
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Check", ImGuiTableColumnFlags_WidthFixed, 120);
		ImGui::TableSetupColumn("Problem", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();
		static int anchor = -1;
		int clicked = -1;
		bool doubleClicked = false;
		std::vector<int> visible;
		for(size_t i = 0; i < gIssues.size(); i++){
			CheckIssue &it = gIssues[i];
			if(!show[it.kind])
				continue;
			visible.push_back((int)i);
			bool gone = it.inst && it.inst->m_isDeleted;
			bool sel = it.inst && !gone && it.inst->m_selected;
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			char label[64];
			snprintf(label, sizeof(label), "%s##issue%d", kCheckNames[it.kind], (int)i);
			if(ImGui::Selectable(label, sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)){
				clicked = (int)i;
				doubleClicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
			}
			ImGui::TableNextColumn();
			if(gone) ImGui::TextDisabled("%s (deleted)", it.msg.c_str());
			else ImGui::TextWrapped("%s", it.msg.c_str());
		}
		ImGui::EndTable();
		if(clicked >= 0){
			CheckIssue &it = gIssues[clicked];
			bool gone = it.inst && it.inst->m_isDeleted;
			ImGuiIO &io = ImGui::GetIO();
			if(it.inst == nil || gone)
				FlyTo(nil, it.pos);	// nothing to select: just show the spot
			else if(io.KeyShift && anchor >= 0){
				int a = -1, b = -1;
				for(int v = 0; v < (int)visible.size(); v++){
					if(visible[v] == anchor) a = v;
					if(visible[v] == clicked) b = v;
				}
				if(a >= 0 && b >= 0){
					if(!io.KeyCtrl) ClearSelection();
					for(int v = a < b ? a : b; v <= (a < b ? b : a); v++){
						CheckIssue &o = gIssues[visible[v]];
						if(o.inst && !o.inst->m_isDeleted) o.inst->Select();
					}
				}
			}else if(io.KeyCtrl){
				if(it.inst->m_selected) it.inst->Deselect(); else it.inst->Select();
				anchor = clicked;
			}else{
				ClearSelection();
				it.inst->Select();
				anchor = clicked;
			}
			if(doubleClicked)
				FlyTo(gone ? nil : it.inst, it.pos);
		}
	}
	ImGui::TextDisabled("Click selects, Ctrl+click adds, Shift+click a range, double-click flies there.");
	ImGui::End();
}

// ------------------------------------------------------------------ show in game

// Sync, then ask the server to move admins to the editor's view (scriptfiles/ariane_goto.txt,
// read by tools/samp/ariane_live.pwn or a gamemode's own copy).
void
SampShowInGame(void)
{
	LoadSettings();
	if(gSampServerDir[0] == '\0'){
		gOpenServer = true;
		return;
	}
	if(!DoSyncToServer(false))
		return;
	rw::V3d target = TheCamera.m_target, ground;
	float z = target.z + 1.0f;
	if(GetGroundPlacementSurface(target, &ground, nil, false))
		z = ground.z + 1.0f;
	float dx = target.x - TheCamera.m_position.x, dy = target.y - TheCamera.m_position.y;
	float angle = atan2f(dy, dx) * 180.0f / 3.14159265f - 90.0f;	// SA-MP: 0 = north, counter-clockwise
	while(angle < 0.0f) angle += 360.0f;
	char path[1200];
	JoinPath(path, sizeof(path), gSampServerDir, "scriptfiles/ariane_goto.txt");
	FILE *f = fopen(path, "w");
	if(f == nil){
		Toast(TOAST_SAVE, "Could not write %s", path);
		return;
	}
	static unsigned counter;
	int who = gSampGotoWho;
	if(who == GOTO_PLAYER && gSampGotoPlayer[0] == '\0')
		who = GOTO_RCON;
	fprintf(f, "goto %u%03u %.3f %.3f %.3f %.1f 0 0 %s%s%s\n", (unsigned)time(nil), counter++ % 1000,
		target.x, target.y, z, angle, kGotoModes[who], who == GOTO_PLAYER ? " " : "", who == GOTO_PLAYER ? gSampGotoPlayer : "");
	fclose(f);
	static const char *whoText[] = { "RCON admins", "players your server script picks", "", "everyone" };
	if(who == GOTO_PLAYER)
		Toast(TOAST_SAVE, "Synced - moving %s to this view (needs the ariane_live script)", gSampGotoPlayer);
	else
		Toast(TOAST_SAVE, "Synced - moving %s to this view (needs the ariane_live script)", whoText[who]);
}

// ------------------------------------------------------------------ material editor (Object Info)

static ImVec4
ArgbToVec(uint32 c)
{
	return ImVec4(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f);
}

static uint32
VecToArgb(const ImVec4 &v)
{
	auto b = [](float f){ return (uint32)(fminf(fmaxf(f, 0.0f), 1.0f) * 255.0f + 0.5f); };
	return b(v.w) << 24 | b(v.x) << 16 | b(v.y) << 8 | b(v.z);
}

// a model that uses this txd: SetObjectMaterial loads the txd through it
static int
ModelForTxd(int slot)
{
	static std::map<int, int> cache;
	std::map<int, int>::iterator it = cache.find(slot);
	if(it != cache.end())
		return it->second;
	int found = -1;
	for(int i = 0; i < NUMOBJECTDEFS && found < 0; i++){
		ObjectDef *o = GetObjectDef(i);
		if(o && o->m_txdSlot == slot)
			found = i;
	}
	cache[slot] = found;
	return found;
}

static rw::Geometry*
FirstGeometry(ObjectDef *obj)
{
	if(obj == nil) return nil;
	if(obj->m_type == ObjectDef::ATOMIC)
		return obj->m_atomics[0] ? obj->m_atomics[0]->geometry : nil;
	if(obj->m_clump){
		FORLIST(lnk, obj->m_clump->atomics)
			return rw::Atomic::fromClump(lnk)->geometry;
	}
	return nil;
}

static const SampMaterial*
FindMaterial(ObjectInst *inst, int index)
{
	std::map<ObjectInst*, std::vector<SampMaterial>>::iterator it = gInstMaterials.find(inst);
	if(it == gInstMaterials.end()) return nil;
	for(size_t i = 0; i < it->second.size(); i++)
		if(it->second[i].index == index) return &it->second[i];
	return nil;
}

static rw::Texture*
TextureByName(const char *txd, const char *tex)
{
	if(txd == nil || tex == nil) return nil;
	int slot = FindTxdSlot(txd);
	if(slot < 0) return nil;
	if(!IsTxdLoaded(slot)) LoadTxd(slot);
	return FindTxdTexture(slot, tex);
}

void
SampMaterialsInspector(ObjectInst *inst)
{
	if(inst == nil)
		return;
	if(gSampProtectGameFiles && SampIsWorldBuilding(inst)){
		ImGui::TextDisabled("Game buildings can't be retextured in SA-MP. Place a copy of the\n"
		                    "model (it joins the SA-MP layer) and retexture that.");
		return;
	}
	ObjectDef *obj = GetObjectDef(inst->m_objectId);
	rw::Geometry *g = FirstGeometry(obj);
	if(g == nil){
		ImGui::TextDisabled("Model not loaded yet.");
		return;
	}
	static ObjectInst *copySource;
	static int pickSlot = -1;
	static char pickTxd[64];
	static ObjectInst *pickInst;
	int slots = g->matList.numMaterials < 16 ? g->matList.numMaterials : 16;
	ImGui::TextDisabled("Slot = SA-MP material index. Changes sync and export like any edit.");
	for(int i = 0; i < slots; i++){
		ImGui::PushID(i);
		rw::Material *m = g->matList.materials[i];
		const SampMaterial *sm = FindMaterial(inst, i);
		rw::Texture *shown = sm && !sm->text ? TextureByName(sm->txd.c_str(), sm->tex.c_str()) : nil;
		if(shown == nil && m) shown = m->texture;
		if(shown && shown->raster)
			ImGui::Image((void*)(intptr_t)shown, ImVec2(36, 36));
		else
			ImGui::Dummy(ImVec2(36, 36));
		ImGui::SameLine();
		ImGui::BeginGroup();
		if(sm && sm->text)
			ImGui::Text("%d  text \"%.40s\"", i, sm->str.c_str());
		else if(sm && sm->txd != "none")
			ImGui::Text("%d  %s / %s", i, sm->txd.c_str(), sm->tex.c_str());
		else
			ImGui::TextDisabled("%d  %s%s", i, m && m->texture ? m->texture->name : "(no texture)", sm ? "  (recoloured)" : "");
		if(ImGui::SmallButton("Texture...")){
			pickSlot = i;
			pickInst = inst;
			TxdDef *td = obj ? GetTxdDef(obj->m_txdSlot) : nil;
			if(sm && !sm->text && sm->txd != "none")
				snprintf(pickTxd, sizeof(pickTxd), "%s", sm->txd.c_str());
			else
				snprintf(pickTxd, sizeof(pickTxd), "%s", td ? td->name : "");
			ImGui::OpenPopup("Pick texture");
		}
		ImGui::SameLine();
		ImVec4 col = sm && !sm->text && sm->color ? ArgbToVec(sm->color) : ImVec4(1, 1, 1, 1);
		if(ImGui::ColorEdit4("##colour", &col.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)){
			if(sm && !sm->text)
				SampSetMaterial(inst, i, sm->model, sm->txd.c_str(), sm->tex.c_str(), VecToArgb(col));
			else
				SampSetMaterial(inst, i, -1, "none", "none", VecToArgb(col));
		}
		ImGui::SetItemTooltip("Material colour (ARGB). White with full alpha keeps the texture as it is.");
		if(sm){
			ImGui::SameLine();
			if(ImGui::SmallButton("Clear"))
				SampClearMaterials(inst, i);
		}
		ImGui::EndGroup();

		if(pickSlot == i && pickInst == inst && ImGui::BeginPopup("Pick texture")){
			ImGui::Text("Texture for slot %d", i);
			ImGui::SetNextItemWidth(200);
			ImGui::InputTextWithHint("TXD", "e.g. beach_sfs", pickTxd, sizeof(pickTxd));
			int slot = pickTxd[0] ? FindTxdSlot(pickTxd) : -1;
			if(slot < 0)
				ImGui::TextDisabled("Type a TXD name (this model's own TXD is filled in).");
			else{
				if(!IsTxdLoaded(slot)) LoadTxd(slot);
				TxdDef *td = GetTxdDef(slot);
				int model = ModelForTxd(slot);
				ImGui::BeginChild("##texgrid", ImVec2(440, 300));
				// as many 56 px tiles per row as fit (no clipped last column)
				ImGuiStyle &st = ImGui::GetStyle();
				float tileW = 56.0f + st.FramePadding.x * 2.0f + st.ItemSpacing.x;
				int cols = (int)((ImGui::GetContentRegionAvail().x + st.ItemSpacing.x) / tileW);
				if(cols < 1) cols = 1;
				int n = 0;
				if(td && td->txd){
					FORLIST(lnk, td->txd->textures){
						rw::Texture *t = rw::Texture::fromDict(lnk);
						if(t == nil || t->raster == nil) continue;
						if(n % cols) ImGui::SameLine();
						ImGui::PushID(n++);
						if(ImGui::ImageButton("##t", (void*)(intptr_t)t, ImVec2(56, 56))){
							uint32 keep = sm && !sm->text ? sm->color : 0;
							SampSetMaterial(inst, i, model >= 0 ? model : inst->m_objectId, pickTxd, t->name, keep);
							ImGui::CloseCurrentPopup();
						}
						ImGui::SetItemTooltip("%s", t->name);
						ImGui::PopID();
					}
				}
				if(n == 0)
					ImGui::TextDisabled("No textures in this TXD.");
				ImGui::EndChild();
			}
			ImGui::EndPopup();
		}
		ImGui::PopID();
	}
	ImGui::Spacing();
	if(ImGui::SmallButton("Copy materials"))
		copySource = gInstMaterials.count(inst) ? inst : nil;
	ImGui::SetItemTooltip("Remember this object's materials to paste onto others");
	ImGui::SameLine();
	if(ImGui::SmallButton("Paste to selection") && copySource && !copySource->m_isDeleted){
		int n = 0;
		for(CPtrNode *p = selection.first; p; p = p->next){
			ObjectInst *o = (ObjectInst*)p->item;
			if(o != copySource && !SampIsWorldBuilding(o) && SampCopyMaterials(copySource, o)){
				o->m_isDirty = true;
				n++;
			}
		}
		Toast(TOAST_SELECTION, "Pasted materials onto %d object(s)", n);
	}
	if(gInstMaterials.count(inst)){
		ImGui::SameLine();
		if(ImGui::SmallButton("Clear all"))
			SampClearMaterials(inst, -1);
	}
}

// ------------------------------------------------------------------ per-frame entry (gui.cpp)

// Server -> editor: scriptfiles/ariane_focus.txt holds "focus <seq> <x> <y> <z>". Checked once a second;
// a request already there when Ariane starts is not acted on.
static void
PollFocusRequests(void)
{
	static double next;
	static bool primed;
	static char lastSeq[32];
	double t = ImGui::GetTime();
	if(t < next)
		return;
	next = t + 1.0;
	LoadSettings();
	if(gSampServerDir[0] == '\0')
		return;
	char path[1200];
	JoinPath(path, sizeof(path), gSampServerDir, "scriptfiles/ariane_focus.txt");
	FILE *f = fopen(path, "r");
	char seq[32] = "";
	float x = 0.0f, y = 0.0f, z = 0.0f;
	bool ok = false;
	if(f){
		char line[256];
		if(fgets(line, sizeof(line), f))
			ok = sscanf(line, "focus %31s %f %f %f", seq, &x, &y, &z) == 4;
		fclose(f);
	}
	if(!primed){
		primed = true;
		snprintf(lastSeq, sizeof(lastSeq), "%s", ok ? seq : "");
		return;
	}
	if(!ok || strcmp(seq, lastSeq) == 0)
		return;
	snprintf(lastSeq, sizeof(lastSeq), "%s", seq);
	if(!gSampFollowFocus)
		return;
	rw::V3d p = { x, y, z };
	TheCamera.setTarget(p);
	TheCamera.setDistanceFromTarget(25.0f);
	Toast(TOAST_SELECTION, "Server: looking at %.1f, %.1f, %.1f", x, y, z);
}

void
SampWindows(void)
{
	RemovedBuildingsWindow();
	ChecksWindow();
	PollFocusRequests();
}
