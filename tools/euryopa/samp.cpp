// SA-MP edition of Ariane
//
// Adds an "SA-MP layer" on top of the normal map editor:
//  - objects placed while SA-MP mode is on go into the layer, never into game IPLs
//  - deleting (or moving) a stock building becomes RemoveBuildingForPlayer on export
//  - import CreateObject / CreateDynamicObject / RemoveBuildingForPlayer code
//    (.pwn / .inc / .txt) and MTA:SA .map files
//  - export Pawn (custom Map_* helpers, streamer or native objects)
//  - "Sync to server" writes scriptfiles/ariane_live.txt, which the custom
//    gamemode polls and rebuilds live
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

static GameFile *gSampFile;
static int gSampIplCounter;
static char gSampServerDir[1024];
static char gSampMapName[64] = "ariane_map";
static int gSampExportFormat;		// 0 custom, 1 streamer, 2 native
static bool gSampAutoInc = true;	// also write gamemodes/wasteland/maps/<name>.inc on sync
static int gRotMode = -1;		// how a matrix quaternion maps onto ObjectInst::m_rotation
static bool gSettingsLoaded;
static bool gOpenExport, gOpenServer;

struct SampRawObject { int model; float x, y, z, rx, ry, rz; int world, interior; };
struct SampRawRemoval { int model; float x, y, z, radius; };

// objects Ariane can't render (SA-MP-only models) and removals that matched nothing:
// kept so export doesn't silently drop them
static std::vector<SampRawObject> gRawObjects;
static std::vector<SampRawRemoval> gRawRemovals;
static std::map<ObjectInst*, int> gInstWorld;	// virtual world per layer object (-1 = all)

static const float DEG2RAD = 3.14159265358979f / 180.0f;

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

struct ImportStats { int objects, raw, removals, removedInsts, rawRemovals, skipped, scaled; };

static void
AddObject(ImportStats *st, int model, float x, float y, float z, float rx, float ry, float rz, int world, int interior)
{
	if(SampCreateInstance(model, x, y, z, rx, ry, rz, world, interior))
		st->objects++;
	else{
		SampRawObject o = { model, x, y, z, rx, ry, rz, world, interior };
		gRawObjects.push_back(o);
		st->raw++;
	}
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

static void
ImportPawn(std::string &text, ImportStats *st)
{
	StripComments(text);
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
		size_t k = j;
		while(k < text.size() && isspace((unsigned char)text[k])) k++;
		if(call == nil || k >= text.size() || text[k] != '('){
			i = j - 1;
			continue;
		}
		size_t after = ParseArgs(text, k + 1, args);
		i = after - 1;
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

	ImportStats st = {};
	if(HasExtCi(path, ".map"))
		ImportMtaMap(text, &st);
	else
		ImportPawn(text, &st);

	log("SA-MP import %s: %d objects, %d kept unrendered, %d removals (%d buildings hidden, %d unmatched), %d skipped, %d scaled\n",
		path, st.objects, st.raw, st.removals, st.removedInsts, st.rawRemovals, st.skipped, st.scaled);
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
			inst->UpdateMatrix();
			SampRawObject o;
			o.model = inst->m_objectId;
			o.x = inst->m_translation.x; o.y = inst->m_translation.y; o.z = inst->m_translation.z;
			GetMatrixEuler(inst->m_matrix, &o.rx, &o.ry, &o.rz);
			std::map<ObjectInst*, int>::iterator it = gInstWorld.find(inst);
			o.world = it == gInstWorld.end() ? -1 : it->second;
			o.interior = inst->m_area;
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
}

static void
SanitizeName(char *s)
{
	for(; *s; s++)
		if(!(isalnum((unsigned char)*s) || *s == '_')) *s = '_';
}

static bool
WritePawn(const char *path, const char *mapName, int format, const ExportLists &l)
{
	FILE *f = fopen(path, "w");
	if(f == nil)
		return false;
	char name[64];
	strncpy(name, mapName, sizeof(name));
	name[sizeof(name)-1] = '\0';
	SanitizeName(name);
	fprintf(f, "// Exported by Ariane (SA-MP edition): map '%s' - %d objects, %d removed buildings\n",
		name, (int)l.objects.size(), (int)l.removals.size());
	if(format == 0){
		fprintf(f, "stock Map_%s()\n{\n", name);
		for(const SampRawRemoval &r : l.removals)
			fprintf(f, "    Map_RemoveBuilding(%d, %.4f, %.4f, %.4f, %.2f);\n", r.model, r.x, r.y, r.z, r.radius);
		for(const SampRawObject &o : l.objects)
			fprintf(f, "    Map_CreateObject(%d, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, %d, %d);\n",
				o.model, o.x, o.y, o.z, o.rx, o.ry, o.rz, o.world, o.interior);
		fprintf(f, "    return %d;\n}\n", (int)l.objects.size());
	}else{
		fprintf(f, "// --- objects: call from OnGameModeInit\nstock CreateMap_%s()\n{\n", name);
		for(const SampRawObject &o : l.objects){
			if(format == 1)
				fprintf(f, "    CreateDynamicObject(%d, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, %d, %d);\n",
					o.model, o.x, o.y, o.z, o.rx, o.ry, o.rz, o.world, o.interior < 0 ? -1 : o.interior);
			else
				fprintf(f, "    CreateObject(%d, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, 300.0);\n",
					o.model, o.x, o.y, o.z, o.rx, o.ry, o.rz);
		}
		fprintf(f, "}\n\n// --- removed buildings: call from OnPlayerConnect\nstock RemoveMap_%s(playerid)\n{\n", name);
		for(const SampRawRemoval &r : l.removals)
			fprintf(f, "    RemoveBuildingForPlayer(playerid, %d, %.4f, %.4f, %.4f, %.2f);\n", r.model, r.x, r.y, r.z, r.radius);
		fprintf(f, "}\n");
	}
	fclose(f);
	return true;
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
		else if(strncmp(line, "autoinc=", 8) == 0) gSampAutoInc = atoi(line + 8) != 0;
		else if(strncmp(line, "protect=", 8) == 0) gSampProtectGameFiles = atoi(line + 8) != 0;
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

bool
SampSyncToServer(void)
{
	LoadSettings();
	if(gSampServerDir[0] == '\0'){
		gOpenServer = true;
		return false;
	}
	ExportLists l;
	CollectExport(&l);

	char path[1200], tmp[1200];
	JoinPath(path, sizeof(path), gSampServerDir, "scriptfiles/ariane_live.txt");
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if(f == nil){
		Toast(TOAST_SAVE, "Sync failed: can't write %s (is the server folder right?)", path);
		return false;
	}
	fprintf(f, "rev %u\n", (unsigned)time(nil));
	for(const SampRawRemoval &r : l.removals)
		fprintf(f, "R %d %.4f %.4f %.4f %.2f\n", r.model, r.x, r.y, r.z, r.radius);
	for(const SampRawObject &o : l.objects)
		fprintf(f, "O %d %.4f %.4f %.4f %.4f %.4f %.4f %d %d\n", o.model, o.x, o.y, o.z, o.rx, o.ry, o.rz, o.world, o.interior);
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
		snprintf(file, sizeof(file), "gamemodes/wasteland/maps/%s.inc", name);
		JoinPath(inc, sizeof(inc), gSampServerDir, file);
		WritePawn(inc, name, 0, l);
	}
	Toast(TOAST_SAVE, "Synced %d objects, %d removals to the server%s", (int)l.objects.size(), (int)l.removals.size(),
		l.moved ? " (moved buildings exported as remove + copy)" : "");
	return true;
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
		if(SampIsLayerInst(inst) && !inst->m_isDeleted) n++;
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
}

void
SampMenu(void)
{
	LoadSettings();
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
	ImGui::Separator();
#ifdef _WIN32
	if(ImGui::MenuItem("Import SA-MP / MTA map...")){
		char path[1024] = "";
		if(FileDialog(false, path, sizeof(path), "Maps (*.pwn;*.inc;*.txt;*.map)\0*.pwn;*.inc;*.txt;*.map;*.p\0All files\0*.*\0", nil))
			SampImportFile(path);
	}
#endif
	if(ImGui::MenuItem("Export Pawn...")) gOpenExport = true;
	if(ImGui::MenuItem("Sync to server", nil, false, true)) SampSyncToServer();
	if(ImGui::MenuItem("Server folder...")) gOpenServer = true;
	ImGui::Separator();
	if(ImGui::MenuItem("Clear SA-MP layer")) ClearLayer();
	ImGui::EndMenu();
}

void
SampPopups(void)
{
	if(gOpenExport){ ImGui::OpenPopup("SA-MP export"); gOpenExport = false; }
	if(gOpenServer){ ImGui::OpenPopup("SA-MP server"); gOpenServer = false; }

	if(ImGui::BeginPopupModal("SA-MP export", nil, ImGuiWindowFlags_AlwaysAutoResize)){
		ImGui::InputText("Map name", gSampMapName, sizeof(gSampMapName));
		const char *formats[] = { "custom (Map_CreateObject / Map_RemoveBuilding)", "Streamer (CreateDynamicObject)", "Native (CreateObject)" };
		ImGui::Combo("Format", &gSampExportFormat, formats, 3);
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
		ImGui::Checkbox("Also write gamemodes/wasteland/maps/<name>.inc on sync", &gSampAutoInc);
		if(ImGui::Button("Save", ImVec2(120, 0))){
			SaveSettings();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if(ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}
