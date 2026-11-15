// Model Viewer: an orbitable close-up of one model, in its own window.
//
// Inspired by CodeWalker's model preview: orbit / pan / zoom around the model,
// textured or untextured ("clay") shading, wireframe, collision / bounds /
// grid / axes / human-scale overlays, SA-MP material overrides of a selected
// object, and an inspector (model info, material slots with their textures,
// every texture in the TXD, collision stats). Rendered off screen into a
// camera texture before the main camera, like the browser thumbnails.

#define WITH_D3D
#include "euryopa.h"
#include "icons.h"
#include <vector>
#include <string>
#include <math.h>
#include <algorithm>

struct ModelViewer
{
	bool open;
	int modelId;
	ObjectInst *inst;		// world object the viewer was opened from (its SA-MP materials), or nil
	int variant;			// atomic index (LOD / damage variant)

	// camera (orbit around target)
	float yaw, pitch, dist, fov;
	rw::V3d target;
	bool framed;

	// display
	int shading;			// 0 textured, 1 clay
	bool wire, collision, bounds, grid, axes, human, instMaterials;
	int background;
	float light;

	// render target
	int rtW, rtH, wantW, wantH;
	rw::Raster *color, *depth;
	rw::Texture *texture;
	rw::Camera *camera;

	// clone of the model
	rw::Atomic *atomic;
	rw::Clump *clump;
	int cloneId, cloneVariant;
	bool cloneFailed;

	// model facts (computed with the clone)
	rw::V3d rmin, rmax;
	bool haveRender;
	int tris, verts, numMaterials, numAtomics;

	int shownTexture;		// texture zoom popup
	rw::Texture *zoomTexture;
};

static ModelViewer V;

static void
InitViewerDefaults(void)
{
	static bool done;
	if(done) return;
	done = true;
	V.modelId = -1;
	V.yaw = 0.8f; V.pitch = 0.35f; V.dist = 10.0f; V.fov = 50.0f;
	V.grid = true; V.instMaterials = true; V.light = 1.0f;
	V.wantW = 640; V.wantH = 480;
	V.cloneId = -1; V.cloneVariant = -1;
	V.shownTexture = -1;
}

static const rw::RGBA kBackgrounds[] = {
	{ 34, 34, 38, 255 }, { 96, 96, 100, 255 }, { 150, 190, 225, 255 }, { 235, 235, 235, 255 }, { 0, 0, 0, 255 },
};
static const char *kBackgroundNames[] = { "Dark", "Grey", "Sky", "White", "Black" };

// ------------------------------------------------------------------ clone

static void
DestroyClone(void)
{
	if(V.atomic){ if(V.atomic->getFrame()) V.atomic->getFrame()->destroy(); V.atomic->destroy(); V.atomic = nil; }
	if(V.clump){ V.clump->destroy(); V.clump = nil; }
	V.cloneId = -1;
	V.cloneVariant = -1;
}

static void
CountGeometry(rw::Geometry *g)
{
	if(g == nil) return;
	V.tris += g->numTriangles;
	V.verts += g->numVertices;
	V.numMaterials += g->matList.numMaterials;
	V.numAtomics++;
}

static bool
EnsureClone(void)
{
	if(V.cloneId == V.modelId && V.cloneVariant == V.variant && (V.atomic || V.clump))
		return true;
	if(V.cloneId == V.modelId && V.cloneVariant == V.variant && V.cloneFailed)
		return false;
	DestroyClone();
	V.cloneId = V.modelId;
	V.cloneVariant = V.variant;
	V.cloneFailed = false;
	ObjectDef *obj = GetObjectDef(V.modelId);
	if(obj == nil){ V.cloneFailed = true; return false; }
	if(!obj->IsLoaded()){
		RequestObject(V.modelId);
		LoadAllRequestedObjects();
	}
	// a specific LOD / damage atomic when the model is loaded and has one
	if(obj->IsLoaded() && obj->m_type == ObjectDef::ATOMIC && V.variant > 0 && V.variant < 3 && obj->m_atomics[V.variant]){
		V.atomic = obj->m_atomics[V.variant]->clone();
		if(V.atomic) V.atomic->setFrame(rw::Frame::create());
	}else if(!CreateObjectPreviewRwObject(V.modelId, &V.atomic, &V.clump)){
		V.cloneFailed = true;
		return false;
	}
	// facts
	V.tris = V.verts = V.numMaterials = V.numAtomics = 0;
	if(V.atomic) CountGeometry(V.atomic->geometry);
	if(V.clump){ FORLIST(lnk, V.clump->atomics) CountGeometry(rw::Atomic::fromClump(lnk)->geometry); }
	V.haveRender = GetRenderBounds(V.modelId, &V.rmin, &V.rmax);
	if(!V.haveRender && obj->m_colModel){
		V.rmin = obj->m_colModel->boundingBox.min;
		V.rmax = obj->m_colModel->boundingBox.max;
		V.haveRender = true;
	}
	return V.atomic || V.clump;
}

// The narrower half-angle of the view, mirroring rw::Camera::setFOV (its fov is horizontal at 4:3 and is
// re-derived for the real aspect), so the whole model fits whichever way the viewport is wider.
static float
FitHalfAngle(float aspect)
{
	if(aspect <= 0.0f)
		aspect = V.rtW > 0 && V.rtH > 0 ? (float)V.rtW / V.rtH :
		         V.wantW > 0 && V.wantH > 0 ? (float)V.wantW / V.wantH : 4.0f / 3.0f;
	float h = V.fov * 3.14159f / 360.0f;
	float vfov = atanf(tanf(h / 2) / (4.0f / 3.0f)) * 2;
	float hfov = atanf(tanf(vfov / 2) * aspect) * 2;
	float tx = tanf(hfov), ty = tx / aspect;
	return atanf(fminf(tx, ty));
}

static void
FrameModel(float aspect = 0.0f)
{
	rw::V3d mn = V.rmin, mx = V.rmax;
	if(!V.haveRender){ mn = { -1, -1, 0 }; mx = { 1, 1, 2 }; }
	V.target = scale(add(mn, mx), 0.5f);
	float radius = length(sub(mx, mn)) * 0.5f;
	if(radius < 0.3f) radius = 0.3f;
	V.dist = radius / sinf(FitHalfAngle(aspect)) * 1.04f;
	V.framed = true;
}

// ------------------------------------------------------------------ lines (own im3d buffer: the shared
// debug-line buffer belongs to the world pass)

static std::vector<rw::RWDEVICE::Im3DVertex> gLineVerts;

static void
Line(rw::V3d a, rw::V3d b, rw::RGBA c)
{
	rw::RWDEVICE::Im3DVertex v;
	v.setX(a.x); v.setY(a.y); v.setZ(a.z); v.setColor(c.red, c.green, c.blue, c.alpha);
	gLineVerts.push_back(v);
	v.setX(b.x); v.setY(b.y); v.setZ(b.z);
	gLineVerts.push_back(v);
}

static void
FlushLines(void)
{
	if(gLineVerts.empty())
		return;
	rw::SetRenderStatePtr(rw::TEXTURERASTER, nil);
	rw::SetRenderState(rw::VERTEXALPHA, 1);
	static uint16 index[1024];
	for(size_t start = 0; start < gLineVerts.size(); start += 1024){
		int n = (int)std::min<size_t>(1024, gLineVerts.size() - start);
		for(int i = 0; i < n; i++) index[i] = (uint16)i;
		rw::im3d::Transform(&gLineVerts[start], n, nil, rw::im3d::EVERYTHING);
		rw::im3d::RenderIndexedPrimitive(rw::PRIMTYPELINELIST, index, n);
		rw::im3d::End();
	}
	gLineVerts.clear();
}

static void
Box(rw::V3d mn, rw::V3d mx, rw::RGBA c)
{
	rw::V3d p[8];
	for(int i = 0; i < 8; i++)
		p[i] = { (i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z };
	static const int e[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
	for(auto &k : e) Line(p[k[0]], p[k[1]], c);
}

static void
Circle(rw::V3d c, float r, int axis, rw::RGBA col)
{
	const int n = 24;
	rw::V3d prev = c;
	for(int i = 0; i <= n; i++){
		float a = i * 6.2831853f / n, s = sinf(a) * r, k = cosf(a) * r;
		rw::V3d p = c;
		if(axis == 0){ p.y += k; p.z += s; }
		else if(axis == 1){ p.x += k; p.z += s; }
		else { p.x += k; p.y += s; }
		if(i) Line(prev, p, col);
		prev = p;
	}
}

static void
CollisionLines(CColModel *col)
{
	const rw::RGBA red = { 255, 60, 60, 255 }, white = { 255, 255, 255, 255 }, magenta = { 255, 80, 255, 255 },
	               green = { 60, 255, 90, 200 };
	Box(col->boundingBox.min, col->boundingBox.max, red);
	for(int i = 0; i < col->numBoxes; i++)
		Box(col->boxes[i].box.min, col->boxes[i].box.max, white);
	for(int i = 0; i < col->numSpheres; i++)
		for(int a = 0; a < 3; a++)
			Circle(col->spheres[i].sph.center, col->spheres[i].sph.radius, a, magenta);
	for(int i = 0; i < col->numTriangles; i++){
		CColTriangle *t = &col->triangles[i];
		rw::V3d v[3];
		if(col->flags & 0x80){
			v[0] = col->compVertices[t->a].Uncompress();
			v[1] = col->compVertices[t->b].Uncompress();
			v[2] = col->compVertices[t->c].Uncompress();
		}else{
			v[0] = col->vertices[t->a]; v[1] = col->vertices[t->b]; v[2] = col->vertices[t->c];
		}
		Line(v[0], v[1], green); Line(v[1], v[2], green); Line(v[2], v[0], green);
	}
}

static void
OverlayLines(void)
{
	ObjectDef *obj = GetObjectDef(V.modelId);
	rw::V3d mn = V.haveRender ? V.rmin : rw::makeV3d(-1, -1, 0);
	rw::V3d mx = V.haveRender ? V.rmax : rw::makeV3d(1, 1, 2);
	if(V.grid){
		// 1 m grid on the model's base, 5 m lines brighter
		float ext = ceilf(fmaxf(fmaxf(fabsf(mn.x), fabsf(mx.x)), fmaxf(fabsf(mn.y), fabsf(mx.y))) + 2.0f);
		if(ext > 60.0f) ext = 60.0f;
		float z = mn.z;
		for(int i = -(int)ext; i <= (int)ext; i++){
			rw::RGBA c = (i % 5 == 0) ? rw::makeRGBA(150, 150, 150, 160) : rw::makeRGBA(90, 90, 90, 110);
			Line({ (float)i, -ext, z }, { (float)i, ext, z }, c);
			Line({ -ext, (float)i, z }, { ext, (float)i, z }, c);
		}
	}
	if(V.axes){
		float a = fmaxf(1.0f, length(sub(mx, mn)) * 0.25f);
		Line({ 0, 0, 0 }, { a, 0, 0 }, rw::makeRGBA(255, 60, 60, 255));
		Line({ 0, 0, 0 }, { 0, a, 0 }, rw::makeRGBA(60, 255, 60, 255));
		Line({ 0, 0, 0 }, { 0, 0, a }, rw::makeRGBA(80, 120, 255, 255));
	}
	if(V.bounds)
		Box(mn, mx, rw::makeRGBA(255, 220, 60, 255));
	if(V.collision && obj && obj->m_colModel)
		CollisionLines(obj->m_colModel);
	if(V.human){
		// 1.8 m person standing beside the model, facing it
		float x = mx.x + 1.0f, z = mn.z;
		rw::RGBA c = rw::makeRGBA(80, 200, 255, 255);
		Box({ x - 0.25f, -0.15f, z }, { x + 0.25f, 0.15f, z + 1.45f }, c);
		Circle({ x, 0, z + 1.6f }, 0.15f, 0, c);
		Circle({ x, 0, z + 1.6f }, 0.15f, 1, c);
	}
}

// ------------------------------------------------------------------ material swaps

struct MatSave { rw::Material *mat; rw::Texture *tex; rw::RGBA col; };
static std::vector<MatSave> gMatSaves;
struct GeoSave { rw::Geometry *geo; uint32 flags; };
static std::vector<GeoSave> gGeoSaves;

static void
ClayGeometry(rw::Geometry *g, rw::RGBA colour = rw::makeRGBA(205, 205, 200, 255))
{
	if(g == nil) return;
	for(int i = 0; i < g->matList.numMaterials; i++){
		rw::Material *m = g->matList.materials[i];
		if(m == nil) continue;
		MatSave s = { m, m->texture, m->color };
		gMatSaves.push_back(s);
		m->texture = nil;
		m->color = rw::makeRGBA(colour.red, colour.green, colour.blue, m->color.alpha);
	}
	GeoSave gs = { g, g->flags };
	gGeoSaves.push_back(gs);
	g->flags |= rw::Geometry::MODULATE;
}

static void
RestoreMaterials(void)
{
	for(size_t i = gMatSaves.size(); i-- > 0; ){
		gMatSaves[i].mat->texture = gMatSaves[i].tex;
		gMatSaves[i].mat->color = gMatSaves[i].col;
	}
	for(size_t i = gGeoSaves.size(); i-- > 0; )
		gGeoSaves[i].geo->flags = gGeoSaves[i].flags;
	gMatSaves.clear();
	gGeoSaves.clear();
}

static void
RenderModel(void)
{
	rw::Matrix ident;
	ident.setIdentity();
	if(V.atomic){
		V.atomic->getFrame()->transform(&ident, rw::COMBINEREPLACE);
		V.atomic->render();
	}else if(V.clump){
		V.clump->getFrame()->transform(&ident, rw::COMBINEREPLACE);
		V.clump->render();
	}
}

// ------------------------------------------------------------------ render

static rw::Camera*
ViewerCamera(void)
{
	if(V.camera)
		return V.camera;
	V.camera = rw::Camera::create();
	if(V.camera == nil)
		return nil;
	V.camera->setFrame(rw::Frame::create());
	if(Scene.world)
		Scene.world->addCamera(V.camera);
	return V.camera;
}

// pngOrientation: world-up camera for PNG readback; UI camera textures are drawn flipped
static bool
RenderView(rw::Raster *color, rw::Raster *depth, int w, int h, bool pngOrientation)
{
	if(!EnsureClone())
		return false;
	if(!V.framed)
		FrameModel((float)w / (float)h);
	rw::Camera *cam = ViewerCamera();
	if(cam == nil)
		return false;

	float cp = cosf(V.pitch), sp = sinf(V.pitch);
	rw::V3d eye = { V.target.x + V.dist * cp * cosf(V.yaw), V.target.y + V.dist * cp * sinf(V.yaw), V.target.z + V.dist * sp };
	rw::V3d up = { 0.0f, 0.0f, pngOrientation ? 1.0f : -1.0f };
	rw::V3d dir = normalize(sub(V.target, eye));

	rw::Frame *f = cam->getFrame();
	cam->frameBuffer = color;
	cam->zBuffer = depth;
	float nearPlane = fmaxf(0.02f, V.dist * 0.01f);
	cam->setNearPlane(nearPlane);
	cam->setFarPlane(fmaxf(200.0f, V.dist * 20.0f));
	cam->fogPlane = cam->farPlane;
	cam->setFOV(V.fov, (float)w / (float)h);
	f->matrix.pos = eye;
	f->matrix.lookAt(dir, up);
	f->updateObjects();

	rw::RGBA bg = kBackgrounds[V.background];
	cam->clear(&bg, rw::Camera::CLEARIMAGE | rw::Camera::CLEARZ);
	cam->beginUpdate();
	rw::SetRenderState(rw::FOGENABLE, 0);
	rw::SetRenderState(rw::ZTESTENABLE, 1);
	rw::SetRenderState(rw::ZWRITEENABLE, 1);
	pAmbient->setColor(0.55f * V.light, 0.55f * V.light, 0.55f * V.light);
	pDirect->setColor(0.6f * V.light, 0.6f * V.light, 0.6f * V.light);

	bool instMats = V.instMaterials && V.inst && V.inst->m_objectId == V.modelId && V.inst->m_rwObject;
	if(V.shading == 1){
		if(V.atomic) ClayGeometry(V.atomic->geometry);
		if(V.clump){ FORLIST(lnk, V.clump->atomics) ClayGeometry(rw::Atomic::fromClump(lnk)->geometry); }
	}else if(instMats)
		SampPushInstMaterials(V.inst);		// same geometry as the clone

	RenderModel();
	if(V.shading == 1) RestoreMaterials();
	else if(instMats) SampPopInstMaterials();
#ifdef RW_D3D9
	if(V.wire){
		// edges in a flat colour on top of the solid pass (equal depth must pass)
		rw::RGBA wc = V.background == 3 ? rw::makeRGBA(20, 60, 120, 255) : rw::makeRGBA(90, 220, 255, 255);
		if(V.atomic) ClayGeometry(V.atomic->geometry, wc);
		if(V.clump){ FORLIST(lnk, V.clump->atomics) ClayGeometry(rw::Atomic::fromClump(lnk)->geometry, wc); }
		rw::d3d::setRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
		rw::d3d::setRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		RenderModel();
		rw::d3d::setRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
		RestoreMaterials();
	}
#endif

	OverlayLines();
	FlushLines();

	cam->endUpdate();
	Timecycle::SetLights();
	return true;
}

static void
DestroyTarget(void)
{
	if(V.texture){ V.texture->raster = nil; V.texture->destroy(); V.texture = nil; }
	if(V.color){ V.color->destroy(); V.color = nil; }
	if(V.depth){ V.depth->destroy(); V.depth = nil; }
	V.rtW = V.rtH = 0;
}

// Called once per frame before the main camera (euryopa.cpp, next to the thumbnails).
void
ModelViewerRender(void)
{
	if(!V.open || V.modelId < 0)
		return;
	int w = V.wantW, h = V.wantH;
	if(w < 64 || h < 64)
		return;
	if(w != V.rtW || h != V.rtH){
		DestroyTarget();
		V.color = rw::Raster::create(w, h, 0, rw::Raster::CAMERATEXTURE);
		V.depth = rw::Raster::create(w, h, 0, rw::Raster::ZBUFFER);
		if(V.color == nil || V.depth == nil){ DestroyTarget(); return; }
		V.texture = rw::Texture::create(V.color);
		V.texture->setFilter(rw::Texture::LINEAR);
		V.rtW = w; V.rtH = h;
	}
	RenderView(V.color, V.depth, w, h, false);
}

// ------------------------------------------------------------------ open / capture

void
ModelViewerOpen(int modelId, ObjectInst *inst)
{
	InitViewerDefaults();
	if(GetObjectDef(modelId) == nil)
		return;
	if(V.modelId != modelId){
		V.framed = false;
		V.variant = 0;
	}
	V.modelId = modelId;
	V.inst = inst && inst->m_objectId == modelId ? inst : nil;
	V.open = true;
}

bool
ModelViewerIsOpen(void)
{
	return V.open;
}

// Agent capture: same renderer, explicit view, PNG on disk. flags: 1 clay, 2 wire, 4 collision, 8 bounds,
// 16 grid, 32 axes, 64 human. yaw/pitch in degrees; zoom multiplies the framed distance.
bool
ModelViewerCapture(int modelId, const char *path, int w, int h, float yawDeg, float pitchDeg, float zoom,
	int flags, char *error, size_t errorSize)
{
	if(GetObjectDef(modelId) == nil){ snprintf(error, errorSize, "unknown model"); return false; }
	InitViewerDefaults();
	DestroyClone();			// the interactive viewer rebuilds its clone on demand
	ModelViewer saved = V;
	V.modelId = modelId; V.variant = 0; V.inst = nil;
	V.shading = flags & 1 ? 1 : 0;
	V.wire = (flags & 2) != 0; V.collision = (flags & 4) != 0; V.bounds = (flags & 8) != 0;
	V.grid = (flags & 16) != 0; V.axes = (flags & 32) != 0; V.human = (flags & 64) != 0;
	V.yaw = yawDeg * 3.14159265f / 180.0f;
	V.pitch = fmaxf(-1.55f, fminf(1.55f, pitchDeg * 3.14159265f / 180.0f));
	V.background = 0; V.light = 1.0f; V.fov = 50.0f;
	bool ok = false;
	w = std::max(64, std::min(w, 2048)); h = std::max(64, std::min(h, 2048));
	rw::Raster *color = rw::Raster::create(w, h, 24, rw::Raster::C888 | rw::Raster::CAMERATEXTURE);
	rw::Raster *depth = rw::Raster::create(w, h, 0, rw::Raster::ZBUFFER);
	if(color && depth && EnsureClone()){
		FrameModel((float)w / (float)h);
		V.dist *= zoom > 0.05f ? zoom : 1.0f;
		ok = RenderView(color, depth, w, h, true);
		if(ok){
			rw::Image *img = color->toImage();
			ok = img != nil;
			if(img){ rw::writePNG(img, path); img->destroy(); }
		}
		if(!ok) snprintf(error, errorSize, "render or readback failed");
	}else
		snprintf(error, errorSize, V.cloneFailed ? "model can't be loaded" : "render target unavailable");
	if(color) color->destroy();
	if(depth) depth->destroy();
	DestroyClone();
	V = saved;
	return ok;
}

static bool
SaveViewPng(char *outPath, size_t size)
{
	char dir[1024];
	if(!GetArianeDataPath(dir, sizeof(dir), "screenshots"))
		return false;
	CreateDirectoryA(dir, nil);
	ObjectDef *obj = GetObjectDef(V.modelId);
	snprintf(outPath, size, "%s\\model_%d_%s.png", dir, V.modelId, obj ? obj->m_name : "model");
	int w = std::max(V.rtW, 1024), h = (int)(w * (V.rtH > 0 ? (float)V.rtH / V.rtW : 0.75f));
	rw::Raster *color = rw::Raster::create(w, h, 24, rw::Raster::C888 | rw::Raster::CAMERATEXTURE);
	rw::Raster *depth = rw::Raster::create(w, h, 0, rw::Raster::ZBUFFER);
	bool ok = color && depth && RenderView(color, depth, w, h, true);
	if(ok){
		rw::Image *img = color->toImage();
		ok = img != nil;
		if(img){ rw::writePNG(img, outPath); img->destroy(); }
	}
	if(color) color->destroy();
	if(depth) depth->destroy();
	return ok;
}

// ------------------------------------------------------------------ UI helpers

static int
StepModel(int from, int dir)
{
	for(int id = from + dir, n = 0; n < NUMOBJECTDEFS; id += dir, n++){
		if(id < 0) id = NUMOBJECTDEFS - 1;
		if(id >= NUMOBJECTDEFS) id = 0;
		ObjectDef *o = GetObjectDef(id);
		if(o && !o->m_cantLoad)
			return id;
	}
	return from;
}

static void
GoToNearestInstance(void)
{
	ObjectInst *best = nil;
	float bestD = 1e30f;
	rw::V3d cam = TheCamera.m_position;
	for(CPtrNode *p = instances.first; p; p = p->next){
		ObjectInst *inst = (ObjectInst*)p->item;
		if(inst->m_objectId != V.modelId || inst->m_isDeleted)
			continue;
		float d = length(sub(inst->m_translation, cam));
		if(d < bestD){ bestD = d; best = inst; }
	}
	if(best){
		best->JumpTo();
		Toast(TOAST_SPAWN, "Nearest %s: %.0f m away", GetObjectDef(V.modelId)->m_name, bestD);
	}else
		Toast(TOAST_SPAWN, "No instance of this model in the loaded map");
}

static void
TextureImage(rw::Texture *t, float size)
{
	if(t == nil || t->raster == nil){
		ImGui::Dummy(ImVec2(size, size));
		return;
	}
	ImGui::Image((void*)(intptr_t)t, ImVec2(size, size));
	if(ImGui::IsItemHovered()){
		ImGui::BeginTooltip();
		ImGui::Text("%s  %dx%d", t->name, t->raster->width, t->raster->height);
		ImGui::Image((void*)(intptr_t)t, ImVec2(256, 256));
		ImGui::EndTooltip();
	}
	if(ImGui::IsItemClicked())
		V.zoomTexture = t;
}

static void
FlagRow(const char *name, bool on)
{
	if(on){ ImGui::BulletText("%s", name); }
}

static void
InfoTab(ObjectDef *obj)
{
	ImGui::Text("ID %d", V.modelId);
	ImGui::SameLine();
	ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", obj->m_name);
	int sampId = SampExportModel(V.modelId);
	if(sampId != V.modelId)
		ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "SA-MP custom model %d", sampId);
	else if(V.modelId >= 18631 && V.modelId < 20000)
		ImGui::TextDisabled("SA-MP object");
	TxdDef *td = GetTxdDef(obj->m_txdSlot);
	ImGui::Text("TXD: %s", td ? td->name : "-");
	if(td && td->parentId >= 0 && GetTxdDef(td->parentId))
		ImGui::TextDisabled("  parent TXD: %s", GetTxdDef(td->parentId)->name);
	if(obj->m_file && obj->m_file->name)
		ImGui::TextDisabled("Defined in %s", obj->m_file->name);
	ImGui::Text("Type: %s, draw distance %.0f m", obj->m_type == ObjectDef::CLUMP ? "clump" : "atomic", obj->GetLargestDrawDist());
	if(obj->m_isTimed)
		ImGui::Text("Timed: visible %02d:00 - %02d:00", obj->m_timeOn, obj->m_timeOff);
	int lod = GetLodForObject(V.modelId);
	if(lod >= 0 && GetObjectDef(lod))
		ImGui::Text("LOD: %s (%d)", GetObjectDef(lod)->m_name, lod);

	ImGui::SeparatorText("Size");
	if(V.haveRender){
		rw::V3d d = sub(V.rmax, V.rmin);
		ImGui::Text("Drawn: %.2f x %.2f x %.2f m", d.x, d.y, d.z);
		ImGui::TextDisabled("  from %.2f %.2f %.2f to %.2f %.2f %.2f", V.rmin.x, V.rmin.y, V.rmin.z, V.rmax.x, V.rmax.y, V.rmax.z);
	}
	if(obj->m_colModel){
		rw::V3d d = sub(obj->m_colModel->boundingBox.max, obj->m_colModel->boundingBox.min);
		ImGui::Text("Collision: %.2f x %.2f x %.2f m", d.x, d.y, d.z);
		if(V.haveRender && obj->m_colModel->boundingBox.min.z < V.rmin.z - 0.05f)
			ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "  mesh starts %.2f m above its collision",
				V.rmin.z - obj->m_colModel->boundingBox.min.z);
	}else
		ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "No collision");

	ImGui::SeparatorText("Geometry");
	ImGui::Text("%d atomic(s), %d triangles, %d vertices, %d materials", V.numAtomics, V.tris, V.verts, V.numMaterials);
	if(obj->m_type == ObjectDef::ATOMIC && obj->m_numAtomics > 1)
		ImGui::TextDisabled("%d LOD / damage variants (Variant above)", obj->m_numAtomics);
	if(obj->m_numEffects > 0)
		ImGui::Text("%d 2D effects (lights, particles...)", obj->m_numEffects);

	ImGui::SeparatorText("Flags");
	FlagRow("draw last (alpha)", obj->m_drawLast);
	FlagRow("additive", obj->m_additive);
	FlagRow("no z-write", obj->m_noZwrite);
	FlagRow("no backface culling", obj->m_noBackfaceCulling);
	FlagRow("ignore lighting", obj->m_ignoreLight);
	FlagRow("no shadows", obj->m_noShadows);
	FlagRow("damageable", obj->m_isDamageable);
	FlagRow("tree", obj->m_isTree);
	FlagRow("palm", obj->m_isPalmTree);
	FlagRow("tag", obj->m_isTag);
	FlagRow("garage door", obj->m_isGarageDoor);
	FlagRow("door", obj->m_isDoor);
	FlagRow("big building (LOD-sized)", obj->m_isBigBuilding);
	FlagRow("code glass", obj->m_isCodeGlass);
	FlagRow("artist glass", obj->m_isArtistGlass);
}

static void
MaterialsTab(void)
{
	rw::Geometry *g = V.atomic ? V.atomic->geometry : nil;
	if(g == nil && V.clump){ FORLIST(lnk, V.clump->atomics){ g = rw::Atomic::fromClump(lnk)->geometry; break; } }
	if(g == nil){ ImGui::TextDisabled("No geometry"); return; }
	ImGui::TextDisabled("SA-MP material index = slot below (SetObjectMaterial)");
	if(ImGui::BeginTable("##mats", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)){
		ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 34);
		ImGui::TableSetupColumn("Texture", ImGuiTableColumnFlags_WidthFixed, 56);
		ImGui::TableSetupColumn("Name");
		for(int i = 0; i < g->matList.numMaterials; i++){
			rw::Material *m = g->matList.materials[i];
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%d", i);
			ImGui::TableSetColumnIndex(1);
			ImGui::PushID(i);
			TextureImage(m ? m->texture : nil, 48);
			ImGui::PopID();
			ImGui::TableSetColumnIndex(2);
			if(m && m->texture){
				ImGui::TextUnformatted(m->texture->name);
				if(m->texture->raster)
					ImGui::TextDisabled("%dx%d", m->texture->raster->width, m->texture->raster->height);
			}else
				ImGui::TextDisabled("(no texture)");
			if(m){
				ImVec4 c(m->color.red / 255.0f, m->color.green / 255.0f, m->color.blue / 255.0f, m->color.alpha / 255.0f);
				ImGui::ColorButton("##c", c, ImGuiColorEditFlags_AlphaPreview, ImVec2(14, 14));
				ImGui::SameLine();
				ImGui::TextDisabled("0x%02X%02X%02X%02X", m->color.alpha, m->color.red, m->color.green, m->color.blue);
			}
			if(ImGui::SmallButton("Copy##name") && m && m->texture)
				ImGui::SetClipboardText(m->texture->name);
		}
		ImGui::EndTable();
	}
	if(V.inst){
		std::string mats = SampMaterialsJson(V.inst);
		if(!mats.empty()){
			ImGui::SeparatorText("SA-MP overrides on the selected object");
			ImGui::TextWrapped("%s", mats.c_str());
		}
	}
}

static void
TexturesTab(ObjectDef *obj)
{
	TxdDef *td = GetTxdDef(obj->m_txdSlot);
	if(td == nil || td->txd == nil){ ImGui::TextDisabled("TXD not loaded"); return; }
	float cell = 84.0f;
	int cols = std::max(1, (int)(ImGui::GetContentRegionAvail().x / (cell + 8.0f)));
	int i = 0;
	FORLIST(lnk, td->txd->textures){
		rw::Texture *t = rw::Texture::fromDict(lnk);
		if(t == nil) continue;
		if(i % cols) ImGui::SameLine();
		ImGui::BeginGroup();
		ImGui::PushID(i);
		TextureImage(t, cell);
		ImGui::PopID();
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cell);
		ImGui::TextDisabled("%s", t->name);
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		i++;
	}
	if(i == 0) ImGui::TextDisabled("Empty TXD");
}

static void
CollisionTab(ObjectDef *obj)
{
	CColModel *c = obj->m_colModel;
	if(c == nil){ ImGui::TextDisabled("No collision model"); return; }
	ImGui::Text("Spheres %d, boxes %d, lines %d, triangles %d", c->numSpheres, c->numBoxes, c->numLines, c->numTriangles);
	ImGui::Text("Bounding sphere r = %.2f m", c->boundingSphere.radius);
	int counts[256] = {};
	for(int i = 0; i < c->numTriangles; i++) counts[c->triangles[i].surface]++;
	for(int i = 0; i < c->numBoxes; i++) counts[c->boxes[i].surface]++;
	for(int i = 0; i < c->numSpheres; i++) counts[c->spheres[i].surface]++;
	ImGui::SeparatorText("Surfaces (surfinfo.dat index: count)");
	for(int s = 0; s < 256; s++)
		if(counts[s]) ImGui::BulletText("%d: %d", s, counts[s]);
	ImGui::TextDisabled("Tick Collision in the toolbar to see it on the model.");
}

// ------------------------------------------------------------------ window

void
ModelViewerWindow(void)
{
	InitViewerDefaults();
	if(!V.open)
		return;
	ImGui::SetNextWindowSize(ImVec2(1040, 660), ImGuiCond_FirstUseEver);
	if(!ImGui::Begin(ICON_FA_CUBE " Model Viewer", &V.open)){
		V.wantW = V.wantH = 0;
		ImGui::End();
		return;
	}
	ObjectDef *obj = GetObjectDef(V.modelId);
	if(obj == nil){
		ImGui::TextDisabled("Select a model in the Object Browser and press Preview.");
		ImGui::End();
		return;
	}

	// toolbar
	if(ImGui::ArrowButton("##prev", ImGuiDir_Left)) ModelViewerOpen(StepModel(V.modelId, -1), nil);
	ImGui::SameLine();
	if(ImGui::ArrowButton("##next", ImGuiDir_Right)) ModelViewerOpen(StepModel(V.modelId, 1), nil);
	ImGui::SameLine();
	int id = V.modelId;
	ImGui::SetNextItemWidth(90);
	if(ImGui::InputInt("##id", &id, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue) && GetObjectDef(id))
		ModelViewerOpen(id, nil);
	ImGui::SameLine();
	ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", obj->m_name);
	ImGui::SameLine(0, 20);
	ImGui::SetNextItemWidth(90);
	const char *shading[] = { "Textured", "Clay" };
	ImGui::Combo("##shading", &V.shading, shading, 2);
	ImGui::SameLine(); ImGui::Checkbox("Wire", &V.wire);
	ImGui::SameLine(); ImGui::Checkbox("Collision", &V.collision);
	ImGui::SameLine(); ImGui::Checkbox("Bounds", &V.bounds);
	ImGui::SameLine(); ImGui::Checkbox("Grid", &V.grid);
	ImGui::SameLine(); ImGui::Checkbox("Axes", &V.axes);
	ImGui::SameLine(); ImGui::Checkbox("Human", &V.human);
	if(ImGui::IsItemHovered()) ImGui::SetTooltip("1.8 m person for scale");

	if(ImGui::Button("Frame")) FrameModel();
	ImGui::SameLine();
	if(ImGui::Button("Front")){ V.yaw = -1.5707963f; V.pitch = 0.0f; }
	ImGui::SameLine();
	if(ImGui::Button("Side")){ V.yaw = 0.0f; V.pitch = 0.0f; }
	ImGui::SameLine();
	if(ImGui::Button("Top")){ V.yaw = -1.5707963f; V.pitch = 1.55f; }
	ImGui::SameLine();
	if(ImGui::Button("Iso")){ V.yaw = -0.785f; V.pitch = 0.5f; }
	ImGui::SameLine(0, 20);
	ImGui::SetNextItemWidth(70);
	ImGui::Combo("##bg", &V.background, kBackgroundNames, IM_ARRAYSIZE(kBackgroundNames));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(110);
	ImGui::SliderFloat("##light", &V.light, 0.2f, 2.0f, "light %.1f");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(110);
	ImGui::SliderFloat("##fov", &V.fov, 15.0f, 90.0f, "fov %.0f");
	if(obj->m_type == ObjectDef::ATOMIC && obj->m_numAtomics > 1){
		ImGui::SameLine();
		ImGui::SetNextItemWidth(90);
		const char *variants[] = { "Variant 0", "Variant 1", "Variant 2" };
		ImGui::Combo("##variant", &V.variant, variants, std::min(3, obj->m_numAtomics));
	}
	if(V.inst){
		ImGui::SameLine();
		ImGui::Checkbox("SA-MP materials", &V.instMaterials);
		if(ImGui::IsItemHovered()) ImGui::SetTooltip("Show the material overrides of the selected object");
	}

	// viewport + inspector
	float inspectorW = 330.0f;
	ImVec2 avail = ImGui::GetContentRegionAvail();
	ImVec2 vp(fmaxf(64.0f, avail.x - inspectorW - 8.0f), fmaxf(64.0f, avail.y));
	V.wantW = ((int)vp.x + 15) & ~15;
	V.wantH = ((int)vp.y + 15) & ~15;

	ImGui::BeginChild("##viewport", vp, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImVec2 p0 = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##vpbtn", vp, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
		ImGuiButtonFlags_MouseButtonMiddle);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	if(V.texture && V.rtW > 0)
		dl->AddImage((void*)(intptr_t)V.texture, p0, ImVec2(p0.x + vp.x, p0.y + vp.y),
			// same orientation as CAMTEX_UV0/1, cropped to the viewport (the target is rounded up to 16 px)
			ImVec2(CAMTEX_FLIP_U ? vp.x / V.rtW : 0.0f, 1.0f),
			ImVec2(CAMTEX_FLIP_U ? 0.0f : vp.x / V.rtW, 1.0f - vp.y / V.rtH));
	else
		dl->AddText(ImVec2(p0.x + 12, p0.y + 12), ImGui::GetColorU32(ImGuiCol_TextDisabled),
			V.cloneFailed ? "This model can't be loaded" : "Loading...");
	ImGuiIO &io = ImGui::GetIO();
	bool hovered = ImGui::IsItemHovered();
	if(ImGui::IsItemActive()){
		if(ImGui::IsMouseDragging(ImGuiMouseButton_Left)){
			V.yaw -= io.MouseDelta.x * 0.01f;
			V.pitch = fmaxf(-1.55f, fminf(1.55f, V.pitch + io.MouseDelta.y * 0.01f));
		}
		if(ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)){
			// pan in the view plane
			float k = V.dist * 0.0018f;
			rw::V3d right = { -sinf(V.yaw), cosf(V.yaw), 0.0f };
			rw::V3d upv = { -sinf(V.pitch) * cosf(V.yaw), -sinf(V.pitch) * sinf(V.yaw), cosf(V.pitch) };
			V.target = add(V.target, scale(right, io.MouseDelta.x * k));
			V.target = add(V.target, scale(upv, io.MouseDelta.y * k));
		}
	}
	if(hovered){
		if(io.MouseWheel != 0.0f)
			V.dist = fmaxf(0.1f, V.dist * powf(0.88f, io.MouseWheel));
		if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) FrameModel();
		if(ImGui::IsKeyPressed(ImGuiKey_F)) FrameModel();
		if(ImGui::IsKeyPressed(ImGuiKey_1) || ImGui::IsKeyPressed(ImGuiKey_Keypad1)){ V.yaw = -1.5707963f; V.pitch = 0.0f; }
		if(ImGui::IsKeyPressed(ImGuiKey_3) || ImGui::IsKeyPressed(ImGuiKey_Keypad3)){ V.yaw = 0.0f; V.pitch = 0.0f; }
		if(ImGui::IsKeyPressed(ImGuiKey_7) || ImGui::IsKeyPressed(ImGuiKey_Keypad7)){ V.yaw = -1.5707963f; V.pitch = 1.55f; }
		if(ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) ModelViewerOpen(StepModel(V.modelId, -1), nil);
		if(ImGui::IsKeyPressed(ImGuiKey_RightArrow)) ModelViewerOpen(StepModel(V.modelId, 1), nil);
	}
	char hud[160];
	snprintf(hud, sizeof(hud), "%d tris  |  LMB orbit  RMB/MMB pan  wheel zoom  F frame  1/3/7 views  </> model",
		V.tris);
	dl->AddText(ImVec2(p0.x + 8, p0.y + vp.y - ImGui::GetFontSize() - 6), IM_COL32(220, 220, 220, 170), hud);
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild("##inspector", ImVec2(inspectorW, vp.y), ImGuiChildFlags_Borders);
	if(ImGui::Button("Place")){
		SetSpawnObjectId(V.modelId);
		gPlaceMode = true;
	}
	if(ImGui::IsItemHovered()) ImGui::SetTooltip("Place this model in the world (click in the 3D view)");
	ImGui::SameLine();
	if(ImGui::Button("Go to nearest")) GoToNearestInstance();
	ImGui::SameLine();
	if(ImGui::Button("Save PNG")){
		char path[1200];
		if(SaveViewPng(path, sizeof(path))) Toast(TOAST_SAVE, "Saved %s", path);
		else Toast(TOAST_SAVE, "Couldn't save the view");
	}
	if(ImGui::SmallButton("Copy ID")){ char b[16]; snprintf(b, sizeof(b), "%d", V.modelId); ImGui::SetClipboardText(b); }
	ImGui::SameLine();
	if(ImGui::SmallButton("Copy name")) ImGui::SetClipboardText(obj->m_name);
	if(ImGui::BeginTabBar("##inspect")){
		if(ImGui::BeginTabItem("Info")){ InfoTab(obj); ImGui::EndTabItem(); }
		if(ImGui::BeginTabItem("Materials")){ MaterialsTab(); ImGui::EndTabItem(); }
		if(ImGui::BeginTabItem("Textures")){ TexturesTab(obj); ImGui::EndTabItem(); }
		if(ImGui::BeginTabItem("Collision")){ CollisionTab(obj); ImGui::EndTabItem(); }
		ImGui::EndTabBar();
	}
	ImGui::EndChild();

	if(V.zoomTexture){
		ImGui::OpenPopup("Texture##viewer");
	}
	if(ImGui::BeginPopup("Texture##viewer")){
		rw::Texture *t = V.zoomTexture;
		if(t && t->raster){
			ImGui::Text("%s  %dx%d", t->name, t->raster->width, t->raster->height);
			float s = fminf(512.0f, (float)std::max(t->raster->width, t->raster->height) * 2.0f);
			ImGui::Image((void*)(intptr_t)t, ImVec2(s, s));
			if(ImGui::Button("Copy name")) ImGui::SetClipboardText(t->name);
		}
		ImGui::EndPopup();
	}else
		V.zoomTexture = nil;
	ImGui::End();
	if(!V.open)
		V.wantW = V.wantH = 0;
}
