// Mapping styles: user-curated object sets ("wasteland", "farm", ...) that
// agents prefer when they are asked to map in that style.
//
// One JSON file per style in <Ariane data dir>/styles/<slug>.json (next to
// samp_settings.ini). The agent tools (tools/agent/styles.py, arianectl styles,
// the style_* MCP tools) read and write the same files; this window notices
// their changes within a second.
//
//	{ "format": "ariane-style-1", "name": "...", "description": "...",
//	  "models": [ {"id": 12957, "name": "...", "role": "vehicle", "weight": 1, "notes": "..."} ],
//	  "textures": ["rusty_metal"],            (texture preset names, see tools/agent/materials.py)
//	  "avoid": [ {"id": 1225, "name": "...", "reason": "explosive"} ],
//	  "notes": "..." }

#include "euryopa.h"
#include "icons.h"
#include <string>
#include <vector>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

bool gShowStylesWindow;

struct StyleModel {
	int id;
	std::string name, role, notes;
	float weight;
};
struct StyleAvoid {
	int id;
	std::string name, reason;
};
struct MappingStyle {
	std::string name, description, notes, file;
	std::vector<StyleModel> models;
	std::vector<std::string> textures;
	std::vector<StyleAvoid> avoid;
};

static std::vector<MappingStyle> gStyles;
static std::string gStylesSignature;	// file names + sizes + mtimes of the last load
static double gStylesLastScan = -100.0;
static std::string gCurrentStyleName;
static char gStylesError[512];

static const char *kStyleRoles[] = {
	"", "wall", "shelter", "ruin", "prop", "debris", "vehicle", "vegetation", "ground", "light", "sign", "other"
};
enum { NUM_STYLE_ROLES = sizeof(kStyleRoles) / sizeof(kStyleRoles[0]) };

// pending "new style" popup (opened from a context menu: add these models once it exists)
static bool gOpenNewStylePopup;
static std::vector<int> gNewStyleModels;
static std::string gNewStyleRole;
static bool gOpenRenamePopup, gOpenDeletePopup;

// ------------------------------------------------------------------ tiny JSON

struct JVal {
	enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
	double num = 0.0;
	bool b = false;
	std::string str;
	std::vector<JVal> arr;
	std::vector<std::pair<std::string, JVal>> obj;

	const JVal *get(const char *key) const {
		if(type != OBJ) return nil;
		for(const auto &kv : obj)
			if(kv.first == key) return &kv.second;
		return nil;
	}
	std::string getStr(const char *key, const char *def = "") const {
		const JVal *v = get(key);
		return v && v->type == STR ? v->str : std::string(def);
	}
	double getNum(const char *key, double def) const {
		const JVal *v = get(key);
		return v && v->type == NUM ? v->num : def;
	}
};

struct JParser {
	const char *p;
	bool ok = true;

	void ws(void) { while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; }
	bool fail(void) { ok = false; return false; }

	static void putUtf8(std::string &out, unsigned cp) {
		if(cp < 0x80) out += (char)cp;
		else if(cp < 0x800){ out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
		else if(cp < 0x10000){ out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
		else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
		       out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
	}
	bool hex4(unsigned *cp) {
		unsigned v = 0;
		for(int i = 0; i < 4; i++){
			char c = *p++;
			v <<= 4;
			if(c >= '0' && c <= '9') v |= c - '0';
			else if(c >= 'a' && c <= 'f') v |= c - 'a' + 10;
			else if(c >= 'A' && c <= 'F') v |= c - 'A' + 10;
			else return fail();
		}
		*cp = v;
		return true;
	}
	bool string(std::string &out) {
		if(*p != '"') return fail();
		p++;
		while(*p && *p != '"'){
			if(*p == '\\'){
				p++;
				char c = *p++;
				switch(c){
				case 'n': out += '\n'; break;
				case 't': out += '\t'; break;
				case 'r': out += '\r'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'u': {
					unsigned cp;
					if(!hex4(&cp)) return false;
					if(cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u'){
						p += 2;
						unsigned lo;
						if(!hex4(&lo)) return false;
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					}
					putUtf8(out, cp);
					break;
				}
				case 0: return fail();
				default: out += c; break;	// \" \\ \/
				}
			}else
				out += *p++;
		}
		if(*p != '"') return fail();
		p++;
		return true;
	}
	bool value(JVal &v, int depth = 0) {
		if(depth > 32) return fail();
		ws();
		if(*p == '{'){
			v.type = JVal::OBJ;
			p++; ws();
			if(*p == '}'){ p++; return true; }
			for(;;){
				ws();
				std::string key;
				if(!string(key)) return false;
				ws();
				if(*p++ != ':') return fail();
				v.obj.emplace_back(key, JVal());
				if(!value(v.obj.back().second, depth + 1)) return false;
				ws();
				if(*p == ','){ p++; continue; }
				if(*p == '}'){ p++; return true; }
				return fail();
			}
		}
		if(*p == '['){
			v.type = JVal::ARR;
			p++; ws();
			if(*p == ']'){ p++; return true; }
			for(;;){
				v.arr.emplace_back();
				if(!value(v.arr.back(), depth + 1)) return false;
				ws();
				if(*p == ','){ p++; continue; }
				if(*p == ']'){ p++; return true; }
				return fail();
			}
		}
		if(*p == '"'){ v.type = JVal::STR; return string(v.str); }
		if(strncmp(p, "true", 4) == 0){ v.type = JVal::BOOL; v.b = true; p += 4; return true; }
		if(strncmp(p, "false", 5) == 0){ v.type = JVal::BOOL; p += 5; return true; }
		if(strncmp(p, "null", 4) == 0){ p += 4; return true; }
		char *end;
		v.num = strtod(p, &end);
		if(end == p) return fail();
		v.type = JVal::NUM;
		p = end;
		return true;
	}
};

static void
jsonString(std::string &out, const std::string &s)
{
	out += '"';
	for(unsigned char c : s){
		if(c == '"') out += "\\\"";
		else if(c == '\\') out += "\\\\";
		else if(c == '\n') out += "\\n";
		else if(c == '\t') out += "\\t";
		else if(c == '\r') out += "\\r";
		else if(c < 0x20){
			char buf[8];
			snprintf(buf, sizeof(buf), "\\u%04x", c);
			out += buf;
		}else
			out += (char)c;
	}
	out += '"';
}

// ------------------------------------------------------------------ files

// Same rule as styles.py slug(): lowercase a-z 0-9 - _, other runs -> '_', trimmed, max 48.
static std::string
styleSlug(const std::string &name)
{
	std::string s;
	bool pendingSep = false;
	for(unsigned char c : name){
		c = (unsigned char)tolower(c);
		bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
		if(!keep){ pendingSep = true; continue; }
		if(pendingSep && !s.empty()) s += '_';
		pendingSep = false;
		s += (char)c;
	}
	while(!s.empty() && s.front() == '_') s.erase(s.begin());
	while(!s.empty() && s.back() == '_') s.pop_back();
	if(s.size() > 48) s.resize(48);
	return s.empty() ? std::string("style") : s;
}

static bool
stylesDir(char *dir, size_t size)
{
	if(!GetArianeDataPath(dir, size, "styles"))
		return false;
#ifdef _WIN32
	CreateDirectoryA(dir, nil);
#else
	mkdir(dir, 0755);
#endif
	return true;
}

static void
listStyleFiles(std::vector<std::string> &names, std::string &signature)
{
	char dir[1024];
	if(!stylesDir(dir, sizeof(dir)))
		return;
#ifdef _WIN32
	char pattern[1100];
	snprintf(pattern, sizeof(pattern), "%s\\*.json", dir);
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA(pattern, &fd);
	if(h == INVALID_HANDLE_VALUE) return;
	do {
		if((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.')
			continue;
		names.push_back(fd.cFileName);
		char buf[64];
		snprintf(buf, sizeof(buf), "|%lu|%lu|%lu", (unsigned long)fd.nFileSizeLow,
		         (unsigned long)fd.ftLastWriteTime.dwLowDateTime, (unsigned long)fd.ftLastWriteTime.dwHighDateTime);
		signature += fd.cFileName;
		signature += buf;
	} while(FindNextFileA(h, &fd));
	FindClose(h);
#else
	DIR *d = opendir(dir);
	if(!d) return;
	struct dirent *ent;
	while((ent = readdir(d)) != nil){
		size_t len = strlen(ent->d_name);
		if(ent->d_name[0] == '.' || len < 6 || strcmp(ent->d_name + len - 5, ".json") != 0)
			continue;
		char path[1400];
		snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
		struct stat st;
		if(stat(path, &st) != 0) continue;
		names.push_back(ent->d_name);
		char buf[64];
		snprintf(buf, sizeof(buf), "|%ld|%ld", (long)st.st_size, (long)st.st_mtime);
		signature += ent->d_name;
		signature += buf;
	}
	closedir(d);
#endif
	std::sort(names.begin(), names.end());
}

static bool
readStyleFile(const char *path, const std::string &file, MappingStyle &style)
{
	FILE *f = fopen(path, "rb");
	if(f == nil) return false;
	std::string text;
	char buf[4096];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
	fclose(f);

	JVal root;
	JParser parser;
	parser.p = text.c_str();
	if(text.size() >= 3 && (unsigned char)text[0] == 0xEF) parser.p += 3;	// BOM
	if(!parser.value(root) || root.type != JVal::OBJ){
		log("Mapping styles: can't parse %s\n", path);
		return false;
	}
	style.file = file;
	style.name = root.getStr("name", file.substr(0, file.size() - 5).c_str());
	style.description = root.getStr("description");
	style.notes = root.getStr("notes");
	if(const JVal *models = root.get("models")){
		for(const JVal &m : models->arr){
			StyleModel sm;
			if(m.type == JVal::NUM){ sm.id = (int)m.num; sm.weight = 1.0f; }
			else if(m.type == JVal::OBJ && m.get("id")){
				sm.id = (int)m.getNum("id", -1);
				sm.name = m.getStr("name");
				sm.role = m.getStr("role");
				sm.notes = m.getStr("notes");
				sm.weight = (float)m.getNum("weight", 1.0);
			}else continue;
			bool dup = false;
			for(const StyleModel &o : style.models) dup |= o.id == sm.id;
			if(!dup) style.models.push_back(sm);
		}
	}
	if(const JVal *tex = root.get("textures"))
		for(const JVal &t : tex->arr)
			if(t.type == JVal::STR) style.textures.push_back(t.str);
	if(const JVal *avoid = root.get("avoid")){
		for(const JVal &a : avoid->arr){
			StyleAvoid sa;
			if(a.type == JVal::NUM) sa.id = (int)a.num;
			else if(a.type == JVal::OBJ && a.get("id")){
				sa.id = (int)a.getNum("id", -1);
				sa.name = a.getStr("name");
				sa.reason = a.getStr("reason");
			}else continue;
			style.avoid.push_back(sa);
		}
	}
	return true;
}

static void
loadStyles(void)
{
	std::vector<std::string> names;
	std::string signature;
	listStyleFiles(names, signature);
	gStylesSignature = signature;
	gStyles.clear();
	char dir[1024];
	if(!stylesDir(dir, sizeof(dir)))
		return;
	for(const std::string &name : names){
		char path[1400];
		snprintf(path, sizeof(path), "%s/%s", dir, name.c_str());
		MappingStyle style;
		if(readStyleFile(path, name, style))
			gStyles.push_back(style);
	}
	std::sort(gStyles.begin(), gStyles.end(), [](const MappingStyle &a, const MappingStyle &b){
		return a.name < b.name;
	});
}

// Re-read the folder when a file appeared, vanished or changed (agents edit it too).
static void
refreshStyles(bool force)
{
	double now = ImGui::GetTime();
	if(!force && now - gStylesLastScan < 1.0)
		return;
	gStylesLastScan = now;
	std::vector<std::string> names;
	std::string signature;
	listStyleFiles(names, signature);
	if(force || signature != gStylesSignature)
		loadStyles();
}

static std::string
styleJson(const MappingStyle &s)
{
	std::string out = "{\n \"format\": \"ariane-style-1\",\n \"name\": ";
	jsonString(out, s.name);
	out += ",\n \"description\": ";
	jsonString(out, s.description);
	out += ",\n \"models\": [";
	for(size_t i = 0; i < s.models.size(); i++){
		const StyleModel &m = s.models[i];
		char buf[64];
		out += i ? ",\n  {\"id\": " : "\n  {\"id\": ";
		snprintf(buf, sizeof(buf), "%d", m.id);
		out += buf;
		out += ", \"name\": "; jsonString(out, m.name);
		out += ", \"role\": "; jsonString(out, m.role);
		snprintf(buf, sizeof(buf), ", \"weight\": %g", m.weight);
		out += buf;
		out += ", \"notes\": "; jsonString(out, m.notes);
		out += "}";
	}
	out += s.models.empty() ? "],\n \"textures\": [" : "\n ],\n \"textures\": [";
	for(size_t i = 0; i < s.textures.size(); i++){
		if(i) out += ", ";
		jsonString(out, s.textures[i]);
	}
	out += "],\n \"avoid\": [";
	for(size_t i = 0; i < s.avoid.size(); i++){
		char buf[32];
		snprintf(buf, sizeof(buf), "%d", s.avoid[i].id);
		out += i ? ",\n  {\"id\": " : "\n  {\"id\": ";
		out += buf;
		out += ", \"name\": "; jsonString(out, s.avoid[i].name);
		out += ", \"reason\": "; jsonString(out, s.avoid[i].reason);
		out += "}";
	}
	out += s.avoid.empty() ? "],\n \"notes\": " : "\n ],\n \"notes\": ";
	jsonString(out, s.notes);
	out += "\n}\n";
	return out;
}

// Write <slug>.json atomically (temp file + rename) so agents never read half a file.
static bool
saveStyle(MappingStyle &s)
{
	char dir[1024];
	if(!stylesDir(dir, sizeof(dir))){
		snprintf(gStylesError, sizeof(gStylesError), "No Ariane data folder");
		return false;
	}
	std::string file = styleSlug(s.name) + ".json";
	char path[1400], tmp[1400];
	snprintf(path, sizeof(path), "%s/%s", dir, file.c_str());
	snprintf(tmp, sizeof(tmp), "%s/.tmp-editor-%s", dir, file.c_str());
	std::string text = styleJson(s);
	FILE *f = fopen(tmp, "wb");
	if(f == nil){
		snprintf(gStylesError, sizeof(gStylesError), "Can't write %s", tmp);
		return false;
	}
	bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
	ok = fclose(f) == 0 && ok;
#ifdef _WIN32
	ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
	ok = ok && rename(tmp, path) == 0;
#endif
	if(!ok){
		remove(tmp);
		snprintf(gStylesError, sizeof(gStylesError), "Can't save %s", path);
		return false;
	}
	// the file name follows the name: drop the old file after a rename
	if(!s.file.empty() && s.file != file){
		char old[1400];
		snprintf(old, sizeof(old), "%s/%s", dir, s.file.c_str());
		remove(old);
	}
	s.file = file;
	gStylesError[0] = '\0';
	return true;
}

static void
saveAndReload(MappingStyle &s)
{
	std::string name = s.name;
	saveStyle(s);
	loadStyles();
	gCurrentStyleName = name;
}

static MappingStyle *
findStyle(const std::string &name)
{
	for(MappingStyle &s : gStyles)
		if(s.name == name) return &s;
	return nil;
}

static bool
styleNameTaken(const std::string &name, const MappingStyle *except)
{
	std::string slug = styleSlug(name);
	for(const MappingStyle &s : gStyles)
		if(&s != except && styleSlug(s.name) == slug) return true;
	return false;
}

static int
findModel(const MappingStyle &s, int id)
{
	for(int i = 0; i < (int)s.models.size(); i++)
		if(s.models[i].id == id) return i;
	return -1;
}

static bool
isAvoided(const MappingStyle &s, int id)
{
	for(const StyleAvoid &a : s.avoid)
		if(a.id == id) return true;
	return false;
}

// Adds (or re-roles) models; returns how many were new. Avoid-list models are skipped.
static int
addModelsToStyle(MappingStyle &s, const std::vector<int> &ids, const char *role)
{
	int added = 0, skipped = 0;
	for(int id : ids){
		ObjectDef *obj = GetObjectDef(id);
		if(obj == nil) continue;
		if(isAvoided(s, id)){ skipped++; continue; }
		int i = findModel(s, id);
		if(i < 0){
			StyleModel m;
			m.id = id;
			m.name = obj->m_name;
			m.role = role ? role : "";
			m.weight = 1.0f;
			s.models.push_back(m);
			added++;
		}else if(role && role[0])
			s.models[i].role = role;
	}
	std::string name = s.name;
	saveAndReload(s);
	if(skipped)
		Toast(TOAST_SPAWN, "%d model(s) skipped: on the avoid list of \"%s\"", skipped, name.c_str());
	else if(added)
		Toast(TOAST_SPAWN, "Added %d model(s) to style \"%s\"", added, name.c_str());
	else
		Toast(TOAST_SPAWN, "Updated style \"%s\"", name.c_str());
	return added;
}

static void
selectedInstanceModels(std::vector<int> &ids)
{
	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst == nil || inst->m_isDeleted) continue;
		bool dup = false;
		for(int id : ids) dup |= id == inst->m_objectId;
		if(!dup) ids.push_back(inst->m_objectId);
	}
}

// ------------------------------------------------------------------ add-to-style menus

// Menu body: one submenu per style (pick a role), plus "New style...".
static void
styleTargetsMenu(const std::vector<int> &ids)
{
	refreshStyles(false);
	if(gStyles.empty())
		ImGui::TextDisabled("No styles yet");
	for(MappingStyle &s : gStyles){
		int inStyle = 0;
		for(int id : ids) inStyle += findModel(s, id) >= 0;
		char label[256];
		snprintf(label, sizeof(label), "%s%s###style_%s", s.name.c_str(),
		         inStyle == (int)ids.size() && inStyle ? "  " ICON_FA_STAR : "", s.file.c_str());
		if(ImGui::BeginMenu(label)){
			for(int r = 0; r < NUM_STYLE_ROLES; r++){
				if(ImGui::MenuItem(r == 0 ? "(no role)" : kStyleRoles[r])){
					addModelsToStyle(s, ids, kStyleRoles[r]);
					ImGui::EndMenu();
					return;	// gStyles was reloaded
				}
			}
			if(inStyle){
				ImGui::Separator();
				if(ImGui::MenuItem(ids.size() == 1 ? "Remove from this style" : "Remove these from this style")){
					for(int id : ids){
						int i = findModel(s, id);
						if(i >= 0) s.models.erase(s.models.begin() + i);
					}
					saveAndReload(s);
					ImGui::EndMenu();
					return;
				}
			}
			ImGui::EndMenu();
		}
	}
	ImGui::Separator();
	if(ImGui::MenuItem(ICON_FA_PLUS " New style...")){
		gNewStyleModels = ids;
		gNewStyleRole.clear();
		gOpenNewStylePopup = true;
	}
}

void
StylesObjectMenu(int objectId)
{
	if(objectId < 0) return;
	if(ImGui::BeginMenu("Add to style")){
		std::vector<int> ids(1, objectId);
		styleTargetsMenu(ids);
		ImGui::EndMenu();
	}
}

void
StylesObjectButton(int objectId)
{
	if(objectId < 0) return;
	if(ImGui::Button("Add to style"))
		ImGui::OpenPopup("##AddToStylePopup");
	ImGui::SetItemTooltip("Add this model to a mapping style (curated object set that agents use\n"
	                      "when asked to map in that style). Manage styles in Window > Mapping Styles.");
	if(ImGui::BeginPopup("##AddToStylePopup")){
		std::vector<int> ids(1, objectId);
		styleTargetsMenu(ids);
		ImGui::EndPopup();
	}
}

void
StylesSelectionMenu(void)
{
	std::vector<int> ids;
	selectedInstanceModels(ids);
	if(ImGui::BeginMenu("Add selection to style", !ids.empty())){
		ImGui::TextDisabled("%d model(s) from the selected objects", (int)ids.size());
		styleTargetsMenu(ids);
		ImGui::EndMenu();
	}
	if(ids.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Select objects in the world to add their models to a mapping style.");
}

// ------------------------------------------------------------------ window

static void
placeModel(int id)
{
	if(gBrushMode) ExitBrushMode();
	if(gPrefabPlaceMode) ExitPrefabPlaceMode();
	SetSpawnObjectId(id);
	RequestObject(id);
	int lod = GetLodForObject(id);
	if(lod >= 0) RequestObject(lod);
	gPlaceMode = true;
}

static void
uiStylePopups(void)
{
	static char nameBuf[128];
	static char descBuf[512];
	if(gOpenNewStylePopup){
		nameBuf[0] = descBuf[0] = '\0';
		ImGui::OpenPopup("New mapping style");
		gOpenNewStylePopup = false;
	}
	if(ImGui::BeginPopupModal("New mapping style", nil, ImGuiWindowFlags_AlwaysAutoResize)){
		if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		ImGui::InputText("Name", nameBuf, sizeof(nameBuf));
		ImGui::InputText("Description", descBuf, sizeof(descBuf));
		if(!gNewStyleModels.empty())
			ImGui::TextDisabled("Starts with %d model(s)", (int)gNewStyleModels.size());
		bool taken = styleNameTaken(nameBuf, nil);
		if(taken) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "A style with that name already exists");
		ImGui::BeginDisabled(nameBuf[0] == '\0' || taken);
		if(ImGui::Button("Create", ImVec2(120, 0))){
			MappingStyle s;
			s.name = nameBuf;
			s.description = descBuf;
			saveStyle(s);
			loadStyles();
			gCurrentStyleName = s.name;
			if(MappingStyle *made = findStyle(s.name))
				if(!gNewStyleModels.empty())
					addModelsToStyle(*made, gNewStyleModels, gNewStyleRole.c_str());
			gNewStyleModels.clear();
			gShowStylesWindow = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if(ImGui::Button("Cancel", ImVec2(120, 0))){
			gNewStyleModels.clear();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	MappingStyle *cur = findStyle(gCurrentStyleName);
	if(gOpenRenamePopup && cur){
		snprintf(nameBuf, sizeof(nameBuf), "%s", cur->name.c_str());
		ImGui::OpenPopup("Rename mapping style");
	}
	gOpenRenamePopup = false;
	if(ImGui::BeginPopupModal("Rename mapping style", nil, ImGuiWindowFlags_AlwaysAutoResize)){
		if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
		ImGui::InputText("Name", nameBuf, sizeof(nameBuf), ImGuiInputTextFlags_AutoSelectAll);
		bool taken = cur && styleNameTaken(nameBuf, cur);
		if(taken) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "A style with that name already exists");
		ImGui::BeginDisabled(cur == nil || nameBuf[0] == '\0' || taken);
		if(ImGui::Button("Rename", ImVec2(120, 0))){
			cur->name = nameBuf;
			saveAndReload(*cur);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if(ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if(gOpenDeletePopup && cur) ImGui::OpenPopup("Delete mapping style?");
	gOpenDeletePopup = false;
	if(ImGui::BeginPopupModal("Delete mapping style?", nil, ImGuiWindowFlags_AlwaysAutoResize)){
		ImGui::Text("Delete the style \"%s\" (%d models)?", cur ? cur->name.c_str() : "",
		            cur ? (int)cur->models.size() : 0);
		ImGui::TextDisabled("The file is removed from the styles folder.");
		if(ImGui::Button("Delete", ImVec2(120, 0)) && cur){
			char dir[1024], path[1400];
			if(stylesDir(dir, sizeof(dir))){
				snprintf(path, sizeof(path), "%s/%s", dir, cur->file.c_str());
				remove(path);
			}
			gCurrentStyleName.clear();
			loadStyles();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if(ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

static bool
inputStdString(const char *label, std::string &value, bool multiline = false, float height = 0.0f)
{
	char buf[2048];
	snprintf(buf, sizeof(buf), "%s", value.c_str());
	if(multiline)
		ImGui::InputTextMultiline(label, buf, sizeof(buf), ImVec2(-1, height));
	else
		ImGui::InputText(label, buf, sizeof(buf));
	if(ImGui::IsItemActive() || ImGui::IsItemDeactivatedAfterEdit())
		value = buf;
	return ImGui::IsItemDeactivatedAfterEdit();
}

static void
uiStyleModels(MappingStyle &s)
{
	static int roleFilter = -1;	// -1 = all
	int browserId = GetSpawnObjectId();
	ObjectDef *browserObj = browserId >= 0 ? GetObjectDef(browserId) : nil;
	ImGui::BeginDisabled(browserObj == nil);
	char addLabel[128];
	snprintf(addLabel, sizeof(addLabel), ICON_FA_PLUS " Browser selection%s%s",
	         browserObj ? ": " : "", browserObj ? browserObj->m_name : "");
	if(ImGui::Button(addLabel)){
		std::vector<int> ids(1, browserId);
		addModelsToStyle(s, ids, roleFilter > 0 ? kStyleRoles[roleFilter] : "");
		return;
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Add the model selected in the Object Browser.");
	ImGui::SameLine();
	std::vector<int> selIds;
	selectedInstanceModels(selIds);
	ImGui::BeginDisabled(selIds.empty());
	char selLabel[64];
	snprintf(selLabel, sizeof(selLabel), ICON_FA_PLUS " Selected objects (%d)", (int)selIds.size());
	if(ImGui::Button(selLabel)){
		addModelsToStyle(s, selIds, roleFilter > 0 ? kStyleRoles[roleFilter] : "");
		return;
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("Add the models of the objects selected in the world.");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(110.0f);
	if(ImGui::BeginCombo("Role##filter", roleFilter < 0 ? "all" : roleFilter == 0 ? "(no role)" : kStyleRoles[roleFilter])){
		if(ImGui::Selectable("all", roleFilter < 0)) roleFilter = -1;
		for(int r = 0; r < NUM_STYLE_ROLES; r++)
			if(ImGui::Selectable(r == 0 ? "(no role)" : kStyleRoles[r], roleFilter == r)) roleFilter = r;
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip("Show only models with this role (new models get it too).");

	ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
		ImGuiTableFlags_ScrollY;
	const float thumb = 56.0f;
	if(!ImGui::BeginTable("##StyleModels", 5, flags, ImVec2(0, ImGui::GetContentRegionAvail().y)))
		return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthFixed, thumb + 150.0f);
	ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, 100.0f);
	ImGui::TableSetupColumn("Weight", ImGuiTableColumnFlags_WidthFixed, 60.0f);
	ImGui::TableSetupColumn("Notes");
	ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60.0f);
	ImGui::TableHeadersRow();

	int removeIndex = -1;
	bool dirty = false;
	for(int i = 0; i < (int)s.models.size(); i++){
		StyleModel &m = s.models[i];
		if(roleFilter >= 0 && m.role != kStyleRoles[roleFilter]) continue;
		ImGui::PushID(m.id);
		ImGui::TableNextRow(0, thumb + 4.0f);
		ImGui::TableSetColumnIndex(0);
		ObjectDef *obj = GetObjectDef(m.id);
		rw::Texture *tex = obj ? GetObjectThumbnailTexture(m.id) : nil;
		ImVec2 pos = ImGui::GetCursorScreenPos();
		if(ImGui::InvisibleButton("##thumb", ImVec2(thumb, thumb)) && obj){
			SetSpawnObjectId(m.id);
			RequestObject(m.id);
		}
		ImDrawList *dl = ImGui::GetWindowDrawList();
		if(tex && tex->raster)
			dl->AddImage((void*)(intptr_t)tex, pos, ImVec2(pos.x + thumb, pos.y + thumb), ImVec2(0, 1), ImVec2(1, 0));
		else
			dl->AddRectFilled(pos, ImVec2(pos.x + thumb, pos.y + thumb), ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
		if(browserId == m.id)
			dl->AddRect(pos, ImVec2(pos.x + thumb, pos.y + thumb), ImGui::GetColorU32(ImGuiCol_HeaderActive), 3.0f, 0, 2.0f);
		if(ImGui::IsItemHovered())
			ImGui::SetTooltip("%d  %s\nClick: select in the Object Browser", m.id, obj ? obj->m_name : m.name.c_str());
		ImGui::SameLine();
		ImGui::BeginGroup();
		ImGui::Text("%d", m.id);
		if(obj) ImGui::TextDisabled("%s", obj->m_name);
		else ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%s (not loaded)", m.name.c_str());
		ImGui::EndGroup();

		ImGui::TableSetColumnIndex(1);
		ImGui::SetNextItemWidth(-1);
		if(ImGui::BeginCombo("##role", m.role.empty() ? "(no role)" : m.role.c_str())){
			for(int r = 0; r < NUM_STYLE_ROLES; r++)
				if(ImGui::Selectable(r == 0 ? "(no role)" : kStyleRoles[r], m.role == kStyleRoles[r])){
					m.role = kStyleRoles[r];
					dirty = true;
				}
			ImGui::EndCombo();
		}
		ImGui::TableSetColumnIndex(2);
		ImGui::SetNextItemWidth(-1);
		ImGui::DragFloat("##weight", &m.weight, 0.05f, 0.0f, 10.0f, "%.2f");
		if(ImGui::IsItemDeactivatedAfterEdit()) dirty = true;
		ImGui::SetItemTooltip("How often agents pick this model (relative; 0 = never).");
		ImGui::TableSetColumnIndex(3);
		ImGui::SetNextItemWidth(-1);
		if(inputStdString("##notes", m.notes)) dirty = true;
		ImGui::TableSetColumnIndex(4);
		if(ImGui::SmallButton("Place") && obj) placeModel(m.id);
		if(ImGui::SmallButton(ICON_FA_TRASH)) removeIndex = i;
		ImGui::SetItemTooltip("Remove from this style");
		ImGui::PopID();
	}
	ImGui::EndTable();
	if(removeIndex >= 0){
		s.models.erase(s.models.begin() + removeIndex);
		dirty = true;
	}
	if(dirty) saveAndReload(s);
}

static void
uiStyleTextures(MappingStyle &s)
{
	ImGui::TextWrapped("Texture presets agents should prefer for this style (names from the agent "
	                   "material presets, e.g. rusty_metal, corrugated_metal, concrete).");
	static char texBuf[64];
	ImGui::SetNextItemWidth(200.0f);
	bool enter = ImGui::InputText("##newtex", texBuf, sizeof(texBuf), ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();
	if((ImGui::Button(ICON_FA_PLUS " Add") || enter) && texBuf[0]){
		bool dup = false;
		for(const std::string &t : s.textures) dup |= t == texBuf;
		if(!dup){
			s.textures.push_back(texBuf);
			texBuf[0] = '\0';
			saveAndReload(s);
			return;
		}
	}
	int remove = -1;
	for(int i = 0; i < (int)s.textures.size(); i++){
		ImGui::PushID(i);
		if(ImGui::SmallButton(ICON_FA_XMARK)) remove = i;
		ImGui::SameLine();
		ImGui::TextUnformatted(s.textures[i].c_str());
		ImGui::PopID();
	}
	if(remove >= 0){
		s.textures.erase(s.textures.begin() + remove);
		saveAndReload(s);
	}
}

static void
uiStyleAvoid(MappingStyle &s)
{
	ImGui::TextWrapped("Models agents must never use for this style (e.g. explosive props). "
	                   "They can't be added to the style either.");
	static int avoidId = -1;
	static char reasonBuf[128];
	ImGui::SetNextItemWidth(90.0f);
	ImGui::InputInt("##avoidid", &avoidId, 0, 0);
	ImGui::SameLine();
	if(ImGui::SmallButton("Browser selection") && GetSpawnObjectId() >= 0)
		avoidId = GetSpawnObjectId();
	ImGui::SameLine();
	ImGui::SetNextItemWidth(160.0f);
	ImGui::InputTextWithHint("##reason", "reason", reasonBuf, sizeof(reasonBuf));
	ImGui::SameLine();
	ObjectDef *obj = avoidId >= 0 ? GetObjectDef(avoidId) : nil;
	ImGui::BeginDisabled(obj == nil || isAvoided(s, avoidId));
	if(ImGui::Button(ICON_FA_PLUS " Avoid")){
		StyleAvoid a;
		a.id = avoidId;
		a.name = obj->m_name;
		a.reason = reasonBuf;
		s.avoid.push_back(a);
		int i = findModel(s, avoidId);
		if(i >= 0) s.models.erase(s.models.begin() + i);
		reasonBuf[0] = '\0';
		ImGui::EndDisabled();
		saveAndReload(s);
		return;
	}
	ImGui::EndDisabled();
	if(obj) { ImGui::SameLine(); ImGui::TextDisabled("%s", obj->m_name); }

	int remove = -1;
	for(int i = 0; i < (int)s.avoid.size(); i++){
		ImGui::PushID(i);
		if(ImGui::SmallButton(ICON_FA_XMARK)) remove = i;
		ImGui::SameLine();
		ImGui::Text("%d  %s", s.avoid[i].id, s.avoid[i].name.c_str());
		if(!s.avoid[i].reason.empty()){
			ImGui::SameLine();
			ImGui::TextDisabled("- %s", s.avoid[i].reason.c_str());
		}
		ImGui::PopID();
	}
	if(remove >= 0){
		s.avoid.erase(s.avoid.begin() + remove);
		saveAndReload(s);
	}
}

void
StylesShortcuts(bool allowEditorShortcuts)
{
	if(allowEditorShortcuts && CPad::IsCtrlDown() && CPad::IsShiftDown() && CPad::IsKeyJustDown('M'))
		gShowStylesWindow ^= 1;
}

void
StylesWindow(void)
{
	uiStylePopups();
	if(!gShowStylesWindow)
		return;
	refreshStyles(false);

	ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_FirstUseEver);
	if(!ImGui::Begin(ICON_FA_CUBE " Mapping Styles", &gShowStylesWindow)){
		ImGui::End();
		return;
	}
	if(findStyle(gCurrentStyleName) == nil && !gStyles.empty())
		gCurrentStyleName = gStyles[0].name;

	ImGui::BeginChild("##StyleList", ImVec2(170, 0), true);
	for(MappingStyle &s : gStyles){
		char label[256];
		snprintf(label, sizeof(label), "%s (%d)###%s", s.name.c_str(), (int)s.models.size(), s.file.c_str());
		if(ImGui::Selectable(label, s.name == gCurrentStyleName))
			gCurrentStyleName = s.name;
		if(!s.description.empty()) ImGui::SetItemTooltip("%s", s.description.c_str());
	}
	if(gStyles.empty())
		ImGui::TextWrapped("No styles yet. Create one, then add models from the Object Browser "
		                   "(right-click a model) or from selected objects.");
	ImGui::Separator();
	if(ImGui::Button(ICON_FA_PLUS " New", ImVec2(-1, 0))){
		gNewStyleModels.clear();
		gOpenNewStylePopup = true;
	}
	MappingStyle *cur = findStyle(gCurrentStyleName);
	ImGui::BeginDisabled(cur == nil);
	if(ImGui::Button("Rename", ImVec2(-1, 0))) gOpenRenamePopup = true;
	if(ImGui::Button(ICON_FA_TRASH " Delete", ImVec2(-1, 0))) gOpenDeletePopup = true;
	ImGui::EndDisabled();
	if(ImGui::Button(ICON_FA_ROTATE " Reload", ImVec2(-1, 0))) refreshStyles(true);
	ImGui::SetItemTooltip("Styles are re-read automatically when agents change them.");
	ImGui::EndChild();
	ImGui::SameLine();

	ImGui::BeginChild("##StyleBody", ImVec2(0, 0), false);
	if(gStylesError[0]) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "%s", gStylesError);
	if(cur){
		ImGui::TextColored(ImVec4(0.85f, 0.90f, 1.0f, 1.0f), "%s", cur->name.c_str());
		ImGui::SameLine();
		ImGui::TextDisabled("styles/%s", cur->file.c_str());
		ImGui::SetNextItemWidth(-90.0f);
		if(inputStdString("Description", cur->description)){ saveAndReload(*cur); cur = findStyle(gCurrentStyleName); }
		if(cur){
			ImGui::SetNextItemWidth(-90.0f);
			if(inputStdString("Rules/notes", cur->notes)){ saveAndReload(*cur); cur = findStyle(gCurrentStyleName); }
			ImGui::SetItemTooltip("Free text agents read with the style (e.g. 'nothing on roads, dark colours').");
		}
		if(cur && ImGui::BeginTabBar("##StyleTabs")){
			char tabLabel[64];
			snprintf(tabLabel, sizeof(tabLabel), "Models (%d)###models", (int)cur->models.size());
			if(ImGui::BeginTabItem(tabLabel)){
				uiStyleModels(*cur);
				ImGui::EndTabItem();
			}
			cur = findStyle(gCurrentStyleName);
			snprintf(tabLabel, sizeof(tabLabel), "Textures (%d)###textures", cur ? (int)cur->textures.size() : 0);
			if(cur && ImGui::BeginTabItem(tabLabel)){
				uiStyleTextures(*cur);
				ImGui::EndTabItem();
			}
			cur = findStyle(gCurrentStyleName);
			snprintf(tabLabel, sizeof(tabLabel), "Avoid (%d)###avoid", cur ? (int)cur->avoid.size() : 0);
			if(cur && ImGui::BeginTabItem(tabLabel)){
				uiStyleAvoid(*cur);
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}
	ImGui::EndChild();
	ImGui::End();
}
