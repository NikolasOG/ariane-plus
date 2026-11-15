// Named object groups (groups.cpp): select, move and rotate objects as one, name them, duplicate,
// delete them and save them as prefabs. Kept in <data>/groups.txt, matched by model + position.
#pragma once
#include <string>
#include <vector>

extern bool gGroupSelectWhole;		// clicking a member selects the whole group

void GroupsUpdate(void);			// once per frame: lazy load, resolve saved members, debounced save
void GroupsWindow(void);			// Groups panel + the "Name this group" modal (call every frame)
void GroupsOpenWindow(void);
void GroupsRequestCreate(void);			// asks for a name, then groups the selection
int GroupsUngroupSelection(void);		// removes every group the selection touches; returns how many
const char *GroupNameOf(ObjectInst *inst);	// nil if not grouped
bool GroupSelectionPivot(rw::V3d *centre);	// selection is exactly whole group(s): centre of their bounds

// selection hooks (ObjectInst::Select / Deselect)
void GroupOnSelect(ObjectInst *inst);
void GroupOnDeselect(ObjectInst *inst);

// prefab placement: group what was just placed (when more than one object)
void GroupPlacedPrefab(const char *prefabPath);

// agent bridge
std::string GroupsJson(void);
bool GroupCreateNamed(const char *name, std::vector<ObjectInst*> &members, char *err, size_t errSize);
bool GroupRemoveNamed(const char *name);
