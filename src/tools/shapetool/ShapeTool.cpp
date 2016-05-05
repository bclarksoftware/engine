#include "ShapeTool.h"
#include "sauce/ShapeToolInjector.h"
#include "video/Shader.h"
#include "video/Color.h"
#include "video/GLDebug.h"
#include "ui/WorldParametersWindow.h"
#include "ui/TreeParametersWindow.h"
#include "frontend/Movement.h"
#include <glm/gtc/constants.hpp>

// tool for testing the world createXXX functions without starting the application
ShapeTool::ShapeTool(video::MeshPoolPtr meshPool, io::FilesystemPtr filesystem, core::EventBusPtr eventBus, voxel::WorldPtr world) :
		ui::UIApp(filesystem, eventBus), _meshPool(meshPool), _worldRenderer(world), _world(world), _worldShader(), _meshShader(new frontend::MeshShader()) {
	init("engine", "shapetool");
	_world->setClientData(true);
	_speed = core::Var::get(cfg::ClientMouseSpeed, "0.1");
}

ShapeTool::~ShapeTool() {
	core::Command::unregisterCommand("+move_right");
	core::Command::unregisterCommand("+move_left");
	core::Command::unregisterCommand("+move_upt");
	core::Command::unregisterCommand("+move_down");
}

core::AppState ShapeTool::onInit() {
	core::AppState state = ui::UIApp::onInit();
	GLDebug::enable(GLDebug::Medium);

	if (!_worldShader.init()) {
		return core::Cleanup;
	}
	if (!_meshShader->init()) {
		return core::Cleanup;
	}

	registerMoveCmd("+move_right", MOVERIGHT);
	registerMoveCmd("+move_left", MOVELEFT);
	registerMoveCmd("+move_forward", MOVEFORWARD);
	registerMoveCmd("+move_backward", MOVEBACKWARD);

	_world->setSeed(1);
	_worldRenderer.onInit(_width, _height);
	_camera.init(_width, _height);
	_camera.setAngles(-glm::half_pi<float>(), glm::pi<float>());
	_camera.setPosition(glm::vec3(0.0f, 100.0f, 0.0f));

	_clearColor = video::Color::LightBlue;

	_worldRenderer.onSpawn(_camera.getPosition(), 5);

	const char *meshName = "chr_fatkid";
	const video::MeshPtr& mesh = _meshPool->getMesh(meshName);
	if (!mesh) {
		Log::error("Failed to load the mesh '%s'", meshName);
		return core::Cleanup;
	}
	const frontend::ClientEntityPtr& entity = std::make_shared<frontend::ClientEntity>(1, -1, _now, _camera.getPosition(), 0.0f, mesh);
	if (!_worldRenderer.addEntity(entity)) {
		Log::error("Failed to create entity");
		return core::Cleanup;
	}

	new WorldParametersWindow(this);
	new TreeParametersWindow(this);

	return state;
}

void ShapeTool::beforeUI() {
	_world->onFrame(_deltaFrame);

	if (_resetTriggered && !_world->isReset()) {
		_world->setContext(_ctx);
		_worldRenderer.onSpawn(_camera.getPosition());
		_resetTriggered = false;
	}

	const bool left = _moveMask & MOVELEFT;
	const bool right = _moveMask & MOVERIGHT;
	const bool forward = _moveMask & MOVEFORWARD;
	const bool backward = _moveMask & MOVEBACKWARD;
	const float speed = _speed->floatVal();
	_camera.updatePosition(_deltaFrame, left, right, forward, backward, speed);
	_camera.updateViewMatrix();
	const float farPlane = _worldRenderer.getViewDistance();
	const glm::mat4& projection = glm::perspective(45.0f, _aspect, 0.1f, farPlane);
	_camera.updateFrustumPlanes(projection);

	_worldRenderer.extractNewMeshes(_camera.getPosition());
	_worldRenderer.onRunning(_deltaFrame);
	_drawCallsWorld = _worldRenderer.renderWorld(_worldShader, _camera, projection, _width, _height);
	_drawCallsEntities = _worldRenderer.renderEntities(_meshShader, _camera, projection, _width, _height);
}

void ShapeTool::afterUI() {
	ui::UIApp::afterUI();
	tb::TBStr drawCallsWorld;
	drawCallsWorld.SetFormatted("drawcalls world: %i", _drawCallsWorld);
	tb::TBStr drawCallsEntity;
	drawCallsEntity.SetFormatted("drawcalls entities: %i", _drawCallsEntities);
	tb::TBStr position;
	const glm::vec3& pos = _camera.getPosition();
	position.SetFormatted("pos: %.2f:%.2f:%.2f", pos.x, pos.y, pos.z);
	tb::TBStr extractions;
	int meshes;
	int extracted;
	int pending;
	_worldRenderer.stats(meshes, extracted, pending);
	extractions.SetFormatted("pending: %i, meshes: %i, extracted: %i", pending, meshes, extracted);
	_root.GetFont()->DrawString(5, 20, tb::TBColor(255, 255, 255), drawCallsEntity);
	_root.GetFont()->DrawString(5, 35, tb::TBColor(255, 255, 255), drawCallsWorld);
	_root.GetFont()->DrawString(5, 50, tb::TBColor(255, 255, 255), position);
	_root.GetFont()->DrawString(5, 65, tb::TBColor(255, 255, 255), extractions);
}

core::AppState ShapeTool::onCleanup() {
	_worldRenderer.onCleanup();
	core::AppState state = UIApp::onCleanup();
	_world->destroy();
	return state;
}

bool ShapeTool::onKeyPress(int32_t key, int16_t modifier) {
	if (key == SDLK_ESCAPE) {
		const SDL_bool current = SDL_GetRelativeMouseMode();
		const SDL_bool mode = current ? SDL_FALSE : SDL_TRUE;
		SDL_SetRelativeMouseMode(mode);
		if (mode) {
			_root.SetVisibility(tb::WIDGET_VISIBILITY::WIDGET_VISIBILITY_INVISIBLE);
		} else {
			_root.SetVisibility(tb::WIDGET_VISIBILITY::WIDGET_VISIBILITY_VISIBLE);
		}
	} else if (key == SDLK_PLUS || key == SDLK_KP_PLUS) {
		const float speed = _speed->floatVal() + 0.1f;
		_speed->setVal(std::to_string(speed));
	} else if (key == SDLK_MINUS || key == SDLK_KP_MINUS) {
		const float speed = std::max(0.1f, _speed->floatVal() - 0.1f);
		_speed->setVal(std::to_string(speed));
	}
	return UIApp::onKeyPress(key, modifier);
}

void ShapeTool::onMouseMotion(int32_t x, int32_t y, int32_t relX, int32_t relY) {
	UIApp::onMouseMotion(x, y, relX, relY);
	const bool current = SDL_GetRelativeMouseMode();
	if (!current) {
		return;
	}
	_camera.onMotion(x, y, relX, relY);
}

void ShapeTool::placeTree(const voxel::TreeContext& ctx) {
	_world->placeTree(ctx);
	regenerate(ctx.pos);
	// TODO: might have affected more than one chunk
}

void ShapeTool::regenerate(const glm::ivec2& pos) {
	_worldRenderer.extractNewMeshes(glm::ivec3(pos.x, 0, pos.y), true);
}

void ShapeTool::reset(const voxel::WorldContext& ctx) {
	_ctx = ctx;
	_worldRenderer.reset();
	_world->reset();
	_resetTriggered = true;
}

int main(int argc, char *argv[]) {
	return getInjector()->get<ShapeTool>()->startMainLoop(argc, argv);
}
