// Named object groups: select, move and rotate objects as one, give them a name, duplicate,
// delete or save them as a prefab. Membership lives in memory (ObjectInst* -> group id) and in
// <data>/groups.txt, matched by model + position on load because instance ids change between
// sessions. Deleted members stay mapped so undo brings them back grouped; only live members are
// listed and saved.
#include "euryopa.h"
#include "groups.h"
#include "icons.h"
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

void OpenExportPrefabPopup(const char *suggestedName);	// gui.cpp

struct EditorGroup { int id; std::string name; };
struct PendingMember { int group; int model; rw::V3d pos; };

bool gGroupSelectWhole = true;

static std::vector<EditorGroup> gGroups;
static std::map<ObjectInst*, int> gMemberOf;
static std::vector<PendingMember> gPending;	// saved members not matched to an object yet
static int gNextGroupId = 1;
static bool gShowWindow, gLoaded;
static double gSaveAt = -1.0;
static double gNextHashCheck, gNextResolve;
static unsigned long long gPosHash;
static int gExpanding;				// selection hooks: don't recurse
static bool gOpenNameModal;
static char gNameBuf[64];
static int gRenameId;				// >0: the name modal renames this group
static char gFilter[64];

static double Now(void) { return ImGui::GetTime(); }
static void MarkDirty(void) { gSaveAt = Now() + 1.5; }

static EditorGroup*
FindGroup(int id)
{
	for(size_t i = 0; i < gGroups.size(); i++)
		if(gGroups[i].id == id) return &gGroups[i];
	return nil;
}

static EditorGroup*
FindGroupByName(const char *name)
{
	for(size_t i = 0; i < gGroups.size(); i++)
		if(gGroups[i].name == name) return &gGroups[i];
	return nil;
}

static bool
Editable(ObjectInst *inst)
{
	return inst && !inst->m_isDeleted && inst->m_numChildren == 0 && IsInstInIplMapDocument(inst);
}

static void
Members(int id, std::vector<ObjectInst*> &out, bool liveOnly)
{
	out.clear();
	for(std::map<ObjectInst*, int>::iterator it = gMemberOf.begin(); it != gMemberOf.end(); ++it)
		if(it->second == id && (!liveOnly || !it->first->m_isDeleted))
			out.push_back(it->first);
}

static int
LiveCount(int id)
{
	int n = 0;
	for(std::map<ObjectInst*, int>::iterator it = gMemberOf.begin(); it != gMemberOf.end(); ++it)
		if(it->second == id && !it->first->m_isDeleted) n++;
	return n;
}

static int
PendingCount(int id)
{
	int n = 0;
	for(size_t i = 0; i < gPending.size(); i++)
		if(gPending[i].group == id) n++;
	return n;
}

static std::string
UniqueName(const std::string &base, int ignoreId = 0)
{
	std::string name = base.empty() ? std::string("Group") : base;
	EditorGroup *g = FindGroupByName(name.c_str());
	if(g == nil || g->id == ignoreId)
		return name;
	for(int n = 2; ; n++){
		std::string tryName = name + " " + std::to_string(n);
		g = FindGroupByName(tryName.c_str());
		if(g == nil || g->id == ignoreId)
			return tryName;
	}
}

static int
AddGroup(const std::string &name)
{
	EditorGroup g;
	g.id = gNextGroupId++;
	g.name = name;
	gGroups.push_back(g);
	return g.id;
}

static void
DropGroup(int id)
{
	for(std::map<ObjectInst*, int>::iterator it = gMemberOf.begin(); it != gMemberOf.end(); )
		if(it->second == id) it = gMemberOf.erase(it); else ++it;
	for(size_t i = gPending.size(); i-- > 0; )
		if(gPending[i].group == id) gPending.erase(gPending.begin() + i);
	for(size_t i = 0; i < gGroups.size(); i++)
		if(gGroups[i].id == id){ gGroups.erase(gGroups.begin() + i); break; }
}

// groups with no members at all (live, deleted or still to be found)
static void
DropEmptyGroups(void)
{
	std::set<int> used;
	for(std::map<ObjectInst*, int>::iterator it = gMemberOf.begin(); it != gMemberOf.end(); ++it)
		used.insert(it->second);
	for(size_t i = 0; i < gPending.size(); i++)
		used.insert(gPending[i].group);
	for(size_t i = gGroups.size(); i-- > 0; )
		if(used.find(gGroups[i].id) == used.end())
			gGroups.erase(gGroups.begin() + i);
}

static bool
Bounds(int id, rw::V3d *centre, float *radius)
{
	std::vector<ObjectInst*> m;
	Members(id, m, true);
	if(m.empty())
		return false;
	rw::V3d mn = m[0]->m_translation, mx = mn;
	for(size_t i = 1; i < m.size(); i++){
		rw::V3d p = m[i]->m_translation;
		mn.x = fminf(mn.x, p.x); mn.y = fminf(mn.y, p.y); mn.z = fminf(mn.z, p.z);
		mx.x = fmaxf(mx.x, p.x); mx.y = fmaxf(mx.y, p.y); mx.z = fmaxf(mx.z, p.z);
	}
	*centre = scale(add(mn, mx), 0.5f);
	if(radius)
		*radius = length(sub(mx, mn)) * 0.5f;
	return true;
}

static int
CreateFromList(const std::string &name, std::vector<ObjectInst*> &list)
{
	std::vector<ObjectInst*> ok;
	for(size_t i = 0; i < list.size(); i++)
		if(Editable(list[i])) ok.push_back(list[i]);
	if(ok.empty())
		return 0;
	int id = AddGroup(UniqueName(name));
	for(size_t i = 0; i < ok.size(); i++)
		gMemberOf[ok[i]] = id;		// leaves any group it was in
	DropEmptyGroups();
	MarkDirty();
	return id;
}

static void
SelectionList(std::vector<ObjectInst*> &out)
{
	out.clear();
	for(CPtrNode *p = selection.first; p; p = p->next)
		out.push_back((ObjectInst*)p->item);
}

static void
SelectGroup(int id)
{
	std::vector<ObjectInst*> m;
	Members(id, m, true);
	ClearSelection();
	gExpanding++;
	for(size_t i = 0; i < m.size(); i++)
		m[i]->Select();
	gExpanding--;
}

// ------------------------------------------------------------------ selection hooks

void
GroupOnSelect(ObjectInst *inst)
{
	if(!gGroupSelectWhole || gExpanding)
		return;
	std::map<ObjectInst*, int>::iterator found = gMemberOf.find(inst);
	if(found == gMemberOf.end())
		return;
	int id = found->second;
	gExpanding++;
	for(std::map<ObjectInst*, int>::iterator it = gMemberOf.begin(); it != gMemberOf.end(); ++it)
		if(it->second == id && it->first != inst && !it->first->m_isDeleted && !it->first->m_selected)
			it->first->Select();
	gExpanding--;
}

void
GroupOnDeselect(ObjectInst *inst)
{
	if(!gGroupSelectWhole || gExpanding)
		return;
	std::map<ObjectInst*, int>::iterator found = gMemberOf.find(inst);
	if(found == gMemberOf.end())
		return;
	int id = found->second;
	gExpanding++;
	for(std::map<ObjectInst*, int>::iterator it = gMemberOf.begin(); it != gMemberOf.end(); ++it)
		if(it->second == id && it->first != inst && it->first->m_selected)
			it->first->Deselect();
	gExpanding--;
}

// ------------------------------------------------------------------ persistence

static unsigned long long
PositionHash(void)
{
	unsigned long long h = 1469598103934665603ULL;
	auto mix = [&h](unsigned long long v){ h ^= v; h *= 1099511628211ULL; };
	for(std::map<ObjectInst*, int>::iterator it = gMemberOf.begin(); it != gMemberOf.end(); ++it){
		ObjectInst *inst = it->first;
		mix((unsigned long long)it->second);
		mix((unsigned long long)(uintptr_t)inst);
		mix(inst->m_isDeleted ? 1 : 0);
		mix((unsigned long long)(long long)floorf(inst->m_translation.x * 100.0f));
		mix((unsigned long long)(long long)floorf(inst->m_translation.y * 100.0f));
		mix((unsigned long long)(long long)floorf(inst->m_translation.z * 100.0f));
	}
	for(size_t i = 0; i < gGroups.size(); i++){
		mix((unsigned long long)gGroups[i].id);
		for(size_t c = 0; c < gGroups[i].name.size(); c++) mix((unsigned char)gGroups[i].name[c]);
	}
	mix(gPending.size());
	return h;
}

static bool
GroupsPath(char *path, size_t size)
{
	return GetArianeDataPath(path, size, "groups.txt");
}

static void
LoadGroups(void)
{
	char path[1024];
	if(!GroupsPath(path, sizeof(path)))
		return;
	FILE *f = fopen(path, "r");
	if(f == nil)
		return;
	char line[512];
	int cur = 0;
	while(fgets(line, sizeof(line), f)){
		line[strcspn(line, "\r\n")] = '\0';
		if(strncmp(line, "group ", 6) == 0)
			cur = AddGroup(UniqueName(line + 6));
		else if(line[0] == 'm' && line[1] == ' ' && cur > 0){
			PendingMember pm;
			pm.group = cur;
			if(sscanf(line + 2, "%d %f %f %f", &pm.model, &pm.pos.x, &pm.pos.y, &pm.pos.z) == 4)
				gPending.push_back(pm);
		}
	}
	fclose(f);
	DropEmptyGroups();
	log("Groups: loaded %d group(s), %d member(s) to find\n", (int)gGroups.size(), (int)gPending.size());
}

static void
SaveGroups(void)
{
	char path[1024];
	if(!GroupsPath(path, sizeof(path)))
		return;
	FILE *f = fopen(path, "w");
	if(f == nil){
		log("Groups: can't write %s\n", path);
		return;
	}
	fprintf(f, "# Ariane editor groups: 'group <name>' then its members 'm <model> <x> <y> <z>'\n");
	fprintf(f, "# (matched by model + position when Ariane starts)\n");
	std::vector<ObjectInst*> m;
	for(size_t g = 0; g < gGroups.size(); g++){
		int id = gGroups[g].id;
		Members(id, m, true);
		int pending = PendingCount(id);
		if(m.empty() && pending == 0)
			continue;
		fprintf(f, "group %s\n", gGroups[g].name.c_str());
		for(size_t i = 0; i < m.size(); i++)
			fprintf(f, "m %d %.4f %.4f %.4f\n", m[i]->m_objectId,
				m[i]->m_translation.x, m[i]->m_translation.y, m[i]->m_translation.z);
		for(size_t i = 0; i < gPending.size(); i++)
			if(gPending[i].group == id)
				fprintf(f, "m %d %.4f %.4f %.4f\n", gPending[i].model,
					gPending[i].pos.x, gPending[i].pos.y, gPending[i].pos.z);
	}
	fclose(f);
}

static bool
HasDeletedMember(void)
{
	for(std::map<ObjectInst*, int>::iterator it = gMemberOf.begin(); it != gMemberOf.end(); ++it)
		if(it->first->m_isDeleted)
			return true;
	return false;
}

// Match saved members to objects: one pass over `instances`, bucketing only the models still pending.
// Also re-attaches a deleted member to a live twin (same model within 5 cm): a checkpoint restore or a
// layer reload replaces every object with a fresh copy, which must not empty the group.
static void
Resolve(void)
{
	std::vector<ObjectInst*> deleted;
	for(std::map<ObjectInst*, int>::iterator it = gMemberOf.begin(); it != gMemberOf.end(); ++it)
		if(it->first->m_isDeleted)
			deleted.push_back(it->first);
	if(gPending.empty() && deleted.empty())
		return;
	std::unordered_set<int> models;
	for(size_t i = 0; i < gPending.size(); i++)
		models.insert(gPending[i].model);
	for(size_t i = 0; i < deleted.size(); i++)
		models.insert(deleted[i]->m_objectId);
	std::unordered_map<int, std::vector<ObjectInst*>> candidates;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(models.find(inst->m_objectId) == models.end())
			continue;
		if(!Editable(inst) || gMemberOf.find(inst) != gMemberOf.end())
			continue;
		candidates[inst->m_objectId].push_back(inst);
	}
	if(candidates.empty())
		return;
	int rebound = 0;
	for(size_t i = 0; i < deleted.size(); i++){
		ObjectInst *old = deleted[i];
		std::unordered_map<int, std::vector<ObjectInst*>>::iterator c = candidates.find(old->m_objectId);
		if(c == candidates.end())
			continue;
		std::vector<ObjectInst*> &list = c->second;
		for(size_t k = 0; k < list.size(); k++){
			if(length(sub(list[k]->m_translation, old->m_translation)) < 0.05f){
				gMemberOf[list[k]] = gMemberOf[old];
				gMemberOf.erase(old);
				list.erase(list.begin() + k);
				rebound++;
				break;
			}
		}
	}
	if(rebound)
		log("Groups: re-attached %d member(s) to their replacement objects\n", rebound);
	int matched = 0;
	for(size_t i = gPending.size(); i-- > 0; ){
		PendingMember &pm = gPending[i];
		std::unordered_map<int, std::vector<ObjectInst*>>::iterator c = candidates.find(pm.model);
		if(c == candidates.end())
			continue;
		std::vector<ObjectInst*> &list = c->second;
		for(size_t k = 0; k < list.size(); k++){
			if(length(sub(list[k]->m_translation, pm.pos)) < 0.05f){
				gMemberOf[list[k]] = pm.group;
				list.erase(list.begin() + k);
				gPending.erase(gPending.begin() + i);
				matched++;
				break;
			}
		}
	}
	if(matched){
		gPosHash = PositionHash();	// matching isn't an edit: no save
		log("Groups: matched %d saved member(s), %d still to find\n", matched, (int)gPending.size());
	}
}

void
GroupsUpdate(void)
{
	double t = Now();
	if(!gLoaded){
		gLoaded = true;
		LoadGroups();
		gPosHash = PositionHash();
		gNextResolve = t;
		gNextHashCheck = t + 1.0;
	}
	if(t >= gNextResolve && (!gPending.empty() || HasDeletedMember())){
		gNextResolve = t + 2.0;
		Resolve();
	}
	if(t >= gNextHashCheck){
		gNextHashCheck = t + 1.0;
		if(HasDeletedMember())
			Resolve();	// re-attach replaced objects before a change gets saved
		unsigned long long h = PositionHash();
		if(h != gPosHash){
			gPosHash = h;
			if(gSaveAt < 0.0) gSaveAt = t + 0.5;
		}
	}
	if(gSaveAt >= 0.0 && t >= gSaveAt){
		gSaveAt = -1.0;
		SaveGroups();
		gPosHash = PositionHash();
	}
}

// ------------------------------------------------------------------ public API

void
GroupsOpenWindow(void)
{
	gShowWindow = true;
}

static int
CountEditableSelected(void)
{
	int n = 0;
	for(CPtrNode *p = selection.first; p; p = p->next)
		if(Editable((ObjectInst*)p->item)) n++;
	return n;
}

void
GroupsRequestCreate(void)
{
	if(CountEditableSelected() == 0){
		Toast(TOAST_SELECTION, "Select the objects to group first");
		return;
	}
	gRenameId = 0;
	snprintf(gNameBuf, sizeof(gNameBuf), "Group %d", gNextGroupId);
	gOpenNameModal = true;
}

int
GroupsUngroupSelection(void)
{
	std::set<int> ids;
	for(CPtrNode *p = selection.first; p; p = p->next){
		std::map<ObjectInst*, int>::iterator it = gMemberOf.find((ObjectInst*)p->item);
		if(it != gMemberOf.end()) ids.insert(it->second);
	}
	for(std::set<int>::iterator it = ids.begin(); it != ids.end(); ++it)
		DropGroup(*it);
	if(!ids.empty()){
		MarkDirty();
		Toast(TOAST_SELECTION, "Ungrouped %d group(s)", (int)ids.size());
	}else
		Toast(TOAST_SELECTION, "Nothing selected is in a group");
	return (int)ids.size();
}

const char*
GroupNameOf(ObjectInst *inst)
{
	std::map<ObjectInst*, int>::iterator it = gMemberOf.find(inst);
	if(it == gMemberOf.end())
		return nil;
	EditorGroup *g = FindGroup(it->second);
	return g ? g->name.c_str() : nil;
}

bool
GroupSelectionPivot(rw::V3d *centre)
{
	std::set<int> ids;
	int n = 0;
	rw::V3d mn = { 0, 0, 0 }, mx = { 0, 0, 0 };
	for(CPtrNode *p = selection.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_isDeleted)
			continue;
		std::map<ObjectInst*, int>::iterator it = gMemberOf.find(inst);
		if(it == gMemberOf.end())
			return false;
		ids.insert(it->second);
		rw::V3d q = inst->m_translation;
		if(n++ == 0){ mn = q; mx = q; }
		else{
			mn.x = fminf(mn.x, q.x); mn.y = fminf(mn.y, q.y); mn.z = fminf(mn.z, q.z);
			mx.x = fmaxf(mx.x, q.x); mx.y = fmaxf(mx.y, q.y); mx.z = fmaxf(mx.z, q.z);
		}
	}
	if(n == 0)
		return false;
	for(std::map<ObjectInst*, int>::iterator it = gMemberOf.begin(); it != gMemberOf.end(); ++it)
		if(ids.count(it->second) && !it->first->m_isDeleted && !it->first->m_selected)
			return false;
	*centre = scale(add(mn, mx), 0.5f);
	return true;
}

void
GroupPlacedPrefab(const char *prefabPath)
{
	std::vector<ObjectInst*> sel;
	SelectionList(sel);
	int live = 0;
	for(size_t i = 0; i < sel.size(); i++)
		if(Editable(sel[i])) live++;
	if(live < 2)
		return;
	std::string base = prefabPath ? prefabPath : "Prefab";
	size_t slash = base.find_last_of("/\\");
	if(slash != std::string::npos) base = base.substr(slash + 1);
	size_t dot = base.find_last_of('.');
	if(dot != std::string::npos && dot > 0) base = base.substr(0, dot);
	int id = CreateFromList(base, sel);
	if(id){
		EditorGroup *g = FindGroup(id);
		Toast(TOAST_SELECTION, "Placed prefab as group \"%s\" (%d objects)", g ? g->name.c_str() : base.c_str(), live);
	}
}

static std::string
JsonEscape(const std::string &s)
{
	std::string o = "\"";
	for(size_t i = 0; i < s.size(); i++){
		char c = s[i];
		if(c == '"' || c == '\\'){ o += '\\'; o += c; }
		else if((unsigned char)c < 0x20){ char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
		else o += c;
	}
	return o + "\"";
}

// "groups":[{"name":..,"live":n,"members":[instance ids]}],"pending":n  (fields for a bridge response)
std::string
GroupsJson(void)
{
	std::string s = "\"groups\":[";
	std::vector<ObjectInst*> m;
	int n = 0;
	for(size_t g = 0; g < gGroups.size(); g++){
		Members(gGroups[g].id, m, true);
		if(n++) s += ",";
		s += "{\"name\":" + JsonEscape(gGroups[g].name) + ",\"live\":" + std::to_string(m.size()) + ",\"members\":[";
		for(size_t i = 0; i < m.size(); i++){
			if(i) s += ",";
			s += std::to_string(m[i]->m_id);
		}
		s += "],\"not_found\":" + std::to_string(PendingCount(gGroups[g].id)) + "}";
	}
	return s + "],\"pending\":" + std::to_string(gPending.size());
}

bool
GroupCreateNamed(const char *name, std::vector<ObjectInst*> &members, char *err, size_t errSize)
{
	if(name == nil || name[0] == '\0'){
		snprintf(err, errSize, "group name required");
		return false;
	}
	if(FindGroupByName(name)){
		snprintf(err, errSize, "a group named '%s' already exists", name);
		return false;
	}
	if(CreateFromList(name, members) == 0){
		snprintf(err, errSize, "none of the objects can be grouped (deleted, LOD, or outside the opened IPL)");
		return false;
	}
	return true;
}

bool
GroupRemoveNamed(const char *name)
{
	EditorGroup *g = name ? FindGroupByName(name) : nil;
	if(g == nil)
		return false;
	DropGroup(g->id);
	MarkDirty();
	return true;
}

// ------------------------------------------------------------------ UI

static void
NameModal(void)
{
	if(gOpenNameModal){
		ImGui::OpenPopup("###groupname");
		gOpenNameModal = false;
	}
	if(!ImGui::BeginPopupModal(gRenameId ? "Rename group###groupname" : "Name this group###groupname", nil,
	                           ImGuiWindowFlags_AlwaysAutoResize))
		return;
	if(ImGui::IsWindowAppearing())
		ImGui::SetKeyboardFocusHere();
	ImGui::SetNextItemWidth(260);
	bool enter = ImGui::InputText("##groupname", gNameBuf, sizeof(gNameBuf),
		ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
	if(gRenameId == 0)
		ImGui::TextDisabled("%d selected object(s) become one group", CountEditableSelected());
	if(ImGui::Button(gRenameId ? "Rename" : "Group", ImVec2(120, 0)) || enter){
		if(gNameBuf[0]){
			if(gRenameId){
				EditorGroup *g = FindGroup(gRenameId);
				if(g){
					g->name = UniqueName(gNameBuf, g->id);
					MarkDirty();
				}
			}else{
				std::vector<ObjectInst*> sel;
				SelectionList(sel);
				int id = CreateFromList(gNameBuf, sel);
				EditorGroup *g = FindGroup(id);
				if(g)
					Toast(TOAST_SELECTION, "Grouped %d object(s) as \"%s\"", LiveCount(id), g->name.c_str());
			}
		}
		gRenameId = 0;
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if(ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)){
		gRenameId = 0;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

static bool
ContainsNoCase(const std::string &hay, const char *needle)
{
	if(needle == nil || needle[0] == '\0')
		return true;
	std::string a = hay, b = needle;
	std::transform(a.begin(), a.end(), a.begin(), ::tolower);
	std::transform(b.begin(), b.end(), b.begin(), ::tolower);
	return a.find(b) != std::string::npos;
}

// a group is "selected" in the list when all of its live members are selected in the viewport,
// so the list and the viewport never disagree
static bool
GroupFullySelected(int id)
{
	bool any = false;
	for(std::map<ObjectInst*, int>::iterator it = gMemberOf.begin(); it != gMemberOf.end(); ++it){
		if(it->second != id || it->first->m_isDeleted)
			continue;
		if(!it->first->m_selected)
			return false;
		any = true;
	}
	return any;
}

// add or remove a group's members to/from the viewport selection
static void
SetGroupSelected(int id, bool selected)
{
	std::vector<ObjectInst*> m;
	Members(id, m, true);
	gExpanding++;
	for(size_t i = 0; i < m.size(); i++){
		if(selected) m[i]->Select();
		else m[i]->Deselect();
	}
	gExpanding--;
}

static void
SelectGroups(const std::vector<int> &ids)
{
	ClearSelection();
	for(size_t i = 0; i < ids.size(); i++)
		SetGroupSelected(ids[i], true);
}

static void
GoToGroups(const std::vector<int> &ids)
{
	bool have = false;
	rw::V3d mn = { 0, 0, 0 }, mx = { 0, 0, 0 };
	for(size_t g = 0; g < ids.size(); g++){
		std::vector<ObjectInst*> m;
		Members(ids[g], m, true);
		for(size_t i = 0; i < m.size(); i++){
			rw::V3d p = m[i]->m_translation;
			if(!have){ mn = mx = p; have = true; continue; }
			mn.x = fminf(mn.x, p.x); mn.y = fminf(mn.y, p.y); mn.z = fminf(mn.z, p.z);
			mx.x = fmaxf(mx.x, p.x); mx.y = fmaxf(mx.y, p.y); mx.z = fmaxf(mx.z, p.z);
		}
	}
	if(!have)
		return;
	rw::V3d c = { (mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f };
	float r = length(sub(mx, mn)) * 0.5f;
	TheCamera.setTarget(c);
	TheCamera.setDistanceFromTarget(fmaxf(15.0f, r * 2.5f + 10.0f));
}

static void
StartRename(int id)
{
	EditorGroup *g = FindGroup(id);
	if(g == nil)
		return;
	gRenameId = id;
	snprintf(gNameBuf, sizeof(gNameBuf), "%s", g->name.c_str());
	gOpenNameModal = true;
}

void
GroupsWindow(void)
{
	NameModal();
	if(!gShowWindow)
		return;
	ImGui::SetNextWindowSize(ImVec2(560, 400), ImGuiCond_FirstUseEver);
	if(!ImGui::Begin(ICON_FA_LIST " Groups", &gShowWindow)){
		ImGui::End();
		return;
	}
	ImGui::Checkbox("Clicking a member in the viewport selects the whole group", &gGroupSelectWhole);
	ImGui::SetItemTooltip("Off: a click picks single members (the gizmo then moves only those)");

	// listed groups, and the ones currently selected
	std::vector<int> ids, picked;
	for(size_t i = 0; i < gGroups.size(); i++){
		int id = gGroups[i].id;
		if(LiveCount(id) == 0 && PendingCount(id) == 0)
			continue;
		if(GroupFullySelected(id))
			picked.push_back(id);
		if(ContainsNoCase(gGroups[i].name, gFilter))
			ids.push_back(id);
	}
	int n = (int)picked.size();

	// toolbar: acts on the selected groups
	if(ImGui::Button(ICON_FA_PLUS " New group"))
		GroupsRequestCreate();
	ImGui::SetItemTooltip("Group the objects selected in the viewport (Ctrl+Shift+G)");
	ImGui::SameLine(0, 16);
	ImGui::BeginDisabled(n == 0);
	if(ImGui::Button("Go to"))
		GoToGroups(picked);
	ImGui::SameLine();
	ImGui::BeginDisabled(n != 1);
	if(ImGui::Button("Rename"))
		StartRename(picked[0]);
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("F2");
	ImGui::SameLine();
	if(ImGui::Button("Duplicate")){
		std::vector<int> copies;
		for(size_t g = 0; g < picked.size(); g++){
			EditorGroup *src = FindGroup(picked[g]);
			if(src == nil) continue;
			std::string name = src->name;
			SelectGroup(picked[g]);
			CopySelected();
			if(PasteClipboard() > 0){
				std::vector<ObjectInst*> sel;
				SelectionList(sel);
				int copy = CreateFromList(name + " copy", sel);
				if(copy) copies.push_back(copy);
			}
		}
		SelectGroups(copies);
		Toast(TOAST_SELECTION, "Duplicated %d group(s); the copies are selected", (int)copies.size());
	}
	ImGui::SetItemTooltip("Copy and paste the selected groups (uses the clipboard); each copy is a new group");
	ImGui::SameLine();
	if(ImGui::Button("Prefab")){
		EditorGroup *g = FindGroup(picked[0]);
		SelectGroups(picked);
		OpenExportPrefabPopup(n == 1 && g ? g->name.c_str() : "");
	}
	ImGui::SetItemTooltip("Save the selected groups as one prefab to place again later");
	ImGui::SameLine();
	if(ImGui::Button("Ungroup")){
		for(size_t g = 0; g < picked.size(); g++)
			DropGroup(picked[g]);
		MarkDirty();
		Toast(TOAST_SELECTION, "Ungrouped %d group(s); the objects stay", n);
	}
	ImGui::SetItemTooltip("Forget the selected groups; their objects stay (Ctrl+Shift+U)");
	ImGui::SameLine();
	if(ImGui::Button(ICON_FA_TRASH " Delete")){
		SelectGroups(picked);
		SampDeleteSelection();
	}
	ImGui::SetItemTooltip("Delete the selected groups' objects (Delete key; Ctrl+Z brings them back, still grouped)");
	ImGui::EndDisabled();

	ImGui::SetNextItemWidth(220);
	ImGui::InputTextWithHint("##groupfilter", "Filter by name", gFilter, sizeof(gFilter));
	ImGui::SameLine();
	if(n > 0) ImGui::Text("%d selected", n);
	else ImGui::TextDisabled("Click a group to select it");

	float footer = ImGui::GetTextLineHeightWithSpacing() * (gPending.empty() ? 1.2f : 2.4f);
	if(ids.empty())
		ImGui::TextDisabled(gFilter[0] ? "No group matches the filter." :
		                    "No groups with objects yet: select objects and press Ctrl+Shift+G.");
	else if(ImGui::BeginTable("##groups", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
	                          ImGuiTableFlags_BordersInnerH, ImVec2(0, -footer))){
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Objects", ImGuiTableColumnFlags_WidthFixed, 80);
		ImGui::TableHeadersRow();
		static int anchor;		// shift-click range start
		int clicked = 0, doubleClicked = 0;
		bool ctrl = ImGui::GetIO().KeyCtrl, shift = ImGui::GetIO().KeyShift;
		for(size_t i = 0; i < ids.size(); i++){
			EditorGroup *g = FindGroup(ids[i]);
			if(g == nil)
				continue;
			int live = LiveCount(g->id), pending = PendingCount(g->id);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::PushID(g->id);
			bool sel = GroupFullySelected(g->id);
			if(ImGui::Selectable(g->name.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)){
				clicked = g->id;
				if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					doubleClicked = g->id;
			}
			ImGui::PopID();
			ImGui::TableNextColumn();
			if(pending){
				ImGui::Text("%d (+%d)", live, pending);
				ImGui::SetItemTooltip("%d saved member(s) not found yet", pending);
			}else
				ImGui::Text("%d", live);
		}
		ImGui::EndTable();

		if(clicked){
			if(shift && anchor){
				// range between the anchor and the clicked row, in list order
				int a = -1, b = -1;
				for(int i = 0; i < (int)ids.size(); i++){
					if(ids[i] == anchor) a = i;
					if(ids[i] == clicked) b = i;
				}
				if(a >= 0 && b >= 0){
					if(!ctrl) ClearSelection();
					for(int i = a < b ? a : b; i <= (a < b ? b : a); i++)
						SetGroupSelected(ids[i], true);
				}
			}else if(ctrl){
				SetGroupSelected(clicked, !GroupFullySelected(clicked));
				anchor = clicked;
			}else{
				SelectGroup(clicked);
				anchor = clicked;
			}
			if(doubleClicked){
				std::vector<int> one(1, doubleClicked);
				SelectGroup(doubleClicked);
				GoToGroups(one);
			}
		}
	}
	// F2 renames the selected group while the panel has focus
	if(n == 1 && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
	   !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F2))
		StartRename(picked[0]);
	ImGui::TextDisabled("Click selects, Ctrl+click adds, Shift+click a range, double-click flies there; Delete deletes, F2 renames.");
	if(!gPending.empty()){
		ImGui::TextDisabled("%d saved member(s) not found yet (moved or removed outside Ariane?)", (int)gPending.size());
		ImGui::SameLine();
		if(ImGui::SmallButton("Forget")){
			gPending.clear();
			DropEmptyGroups();
			MarkDirty();
		}
	}
	ImGui::End();
}
