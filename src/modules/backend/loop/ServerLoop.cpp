/**
 * @file
 */

#include "ClientMessages_generated.h"
#include "ServerLoop.h"

#include "core/command/Command.h"
#include "core/Var.h"
#include "core/Log.h"
#include "core/App.h"
#include "core/io/Filesystem.h"
#include "core/Password.h"
#include "cooldown/CooldownProvider.h"
#include "attrib/ContainerProvider.h"
#include "persistence/ConnectionPool.h"
#include "BackendModels.h"
#include "EventMgrModels.h"
#include "backend/metric/MetricMgr.h"
#include "backend/entity/User.h"
#include "backend/entity/Npc.h"
#include "backend/entity/ai/AICommon.h"
#include "backend/network/UserConnectHandler.h"
#include "backend/network/UserConnectedHandler.h"
#include "backend/network/UserDisconnectHandler.h"
#include "backend/network/AttackHandler.h"
#include "backend/network/VarUpdateHandler.h"
#include "backend/network/MoveHandler.h"
#include "persistence/PersistenceMgr.h"
#include "backend/world/World.h"
#include "core/command/CommandHandler.h"
#include "voxel/MaterialColor.h"
#include "voxelformat/VolumeCache.h"
#include "eventmgr/EventMgr.h"
#include "stock/StockDataProvider.h"
#include "util/EMailValidator.h"

namespace backend {

ServerLoop::ServerLoop(const core::TimeProviderPtr& timeProvider, const MapProviderPtr& mapProvider,
		const network::ServerMessageSenderPtr& messageSender,
		const WorldPtr& world, const persistence::DBHandlerPtr& dbHandler,
		const network::ServerNetworkPtr& network, const io::FilesystemPtr& filesystem,
		const EntityStoragePtr& entityStorage, const core::EventBusPtr& eventBus,
		const attrib::ContainerProviderPtr& containerProvider,
		const cooldown::CooldownProviderPtr& cooldownProvider, const eventmgr::EventMgrPtr& eventMgr,
		const stock::StockDataProviderPtr& stockDataProvider, const MetricMgrPtr& metricMgr,
		const persistence::PersistenceMgrPtr& persistenceMgr,
		const voxelformat::VolumeCachePtr& volumeCache, const http::HttpServerPtr& httpServer) :
		_network(network), _timeProvider(timeProvider), _mapProvider(mapProvider), _messageSender(messageSender),
		_world(world),
		_entityStorage(entityStorage), _eventBus(eventBus), _attribContainerProvider(containerProvider),
		_cooldownProvider(cooldownProvider), _eventMgr(eventMgr), _dbHandler(dbHandler),
		_stockDataProvider(stockDataProvider), _metricMgr(metricMgr), _filesystem(filesystem),
		_persistenceMgr(persistenceMgr), _volumeCache(volumeCache), _httpServer(httpServer) {
	_eventBus->subscribe<network::DisconnectEvent>(*this);
}

bool ServerLoop::addTimer(uv_timer_t* timer, uv_timer_cb cb, uint64_t repeatMillis, uint64_t initialDelayMillis) {
	timer->data = this;
	return uv_timer_start(timer, cb, initialDelayMillis, repeatMillis) == 0;
}

void ServerLoop::signalCallback(uv_signal_t* handle, int signum) {
	if (signum == SIGHUP) {
		core::App::getInstance()->requestQuit();
		return;
	}

	if (signum == SIGINT) {
		//ServerLoop* loop = (ServerLoop*)handle->data;
		// TODO: only quit if this was hit twice in under 2 seconds
		core::App::getInstance()->requestQuit();
		return;
	}
}

void ServerLoop::onIdle(uv_idle_t* handle) {
	core_trace_scoped(IdleTimer);
	ServerLoop* loop = (ServerLoop*)handle->data;
	const metric::MetricPtr& metric = loop->_metricMgr->metric();

	const core::App* app = core::App::getInstance();
	const int deltaFrame = app->deltaFrame();
	constexpr int delta = 10;
	if (std::abs(deltaFrame - loop->_lastDeltaFrame) > delta) {
		metric->timing("frame.delta", app->deltaFrame());
		loop->_lastDeltaFrame = deltaFrame;
	}
	const uint64_t lifetimeSeconds = app->lifetimeInSeconds();
	if (lifetimeSeconds != loop->_lifetimeSeconds) {
		metric->gauge("uptime", lifetimeSeconds);
		loop->_lifetimeSeconds = lifetimeSeconds;
	}
}

void ServerLoop::construct() {
	core::Command::registerCommand("sv_killnpc", [this] (const core::CmdArgs& args) {
		if (args.size() != 1) {
			Log::info("Usage: sv_killnpc <entityid>");
			return;
		}
		const EntityId id = core::string::toLong(args[0]);
		const NpcPtr& npc = _entityStorage->npc(id);
		if (!npc) {
			Log::info("No npc with id " PRIEntId " found", id);
			return;
		}
		if (!npc->die()) {
			Log::info("Could not kill npc with id " PRIEntId, id);
		} else {
			Log::info("Killed npc with id " PRIEntId, id);
		}
	}).setHelp("Kill npc with given entity id");

	core::Command::registerCommand("sv_entitylist", [this] (const core::CmdArgs& args) {
		_entityStorage->visit([] (const EntityPtr& e) {
			Log::info("Id: " PRIEntId, e->id());
			Log::info("- type: %s", e->type());
			const glm::vec3& pos = e->pos();
			Log::info("- pos: %.0f:%.0f:%.0f", pos.x, pos.y, pos.z);
		});
	}).setHelp("Show all entities in the server");

	core::Command::registerCommand("sv_userlist", [this] (const core::CmdArgs& args) {
		_entityStorage->visitUsers([] (const UserPtr& e) {
			Log::info("Id: " PRIEntId, e->id());
			Log::info("- name: %s", e->name().c_str());
			const glm::vec3& pos = e->pos();
			Log::info("- pos: %.0f:%.0f:%.0f", pos.x, pos.y, pos.z);
		});
	}).setHelp("Show all users in the server");

	core::Command::registerCommand("sv_npclist", [this] (const core::CmdArgs& args) {
		_entityStorage->visitNpcs([] (const NpcPtr& e) {
			Log::info("Id: " PRIEntId, e->id());
			Log::info("- type: %s", e->type());
			const glm::vec3& pos = e->pos();
			Log::info("- pos: %.0f:%.0f:%.0f", pos.x, pos.y, pos.z);
		});
	}).setHelp("Show all npcs in the server");

	core::Command::registerCommand("sv_teleport", [this] (const core::CmdArgs& args) {
		if (args.size() < 3) {
			Log::info("Usage: sv_teleport <entityid> <x> <z>");
			return;
		}
		const EntityId id = core::string::toInt(args[0]);
		const int x = core::string::toInt(args[1]);
		const int z = core::string::toInt(args[2]);
		UserPtr user = _entityStorage->user(id);
		if (user) {
			Log::info("Set user position to %i:%i", x, z);
			user->setPos(glm::vec3(x, 20, z));
			return;
		}
		NpcPtr npc = _entityStorage->npc(id);
		if (npc) {
			Log::info("Set npc position to %i:%i", x, z);
			npc->setPos(glm::vec3(x, 20, z));
			return;
		}
		Log::warn("Could not update position for entity id %s", args[0].c_str());
	}).setHelp("Set the position of a specific entity to the given position");

	core::Command::registerCommand("sv_createuser", [this] (const core::CmdArgs& args) {
		if (args.size() != 3) {
			Log::info("Usage: sv_createuser <email> <user> <passwd>");
			return;
		}
		const std::string& email = args[0];
		const std::string& user = args[1];
		const std::string& passwd = args[2];
		if (!util::isValidEmail(email)) {
			Log::error("%s is no valid email address", email.c_str());
			return;
		}
		if (user.empty()) {
			Log::error("Invalid user name given");
			return;
		}
		if (passwd.empty()) {
			Log::error("Invalid password name given");
			return;
		}
		db::UserModel model;
		model.setEmail(email);
		model.setName(user);
		model.setPassword(core::pwhash(passwd, "TODO"));
		if (!_dbHandler->insert(model)) {
			Log::error("Failed to register user");
		} else {
			Log::info("User registered");
		}
	}).setHelp("Create a new user with a given email, name and password");

	_world->construct();
	_volumeCache->construct();
}

#define regHandler(type, handler, ...) \
	r->registerHandler(network::EnumNameClientMsgType(type), std::make_shared<handler>(__VA_ARGS__));

bool ServerLoop::init() {
	_loop = new uv_loop_t;
	if (uv_loop_init(_loop) != 0) {
		Log::error("Failed to init event loop");
		uv_loop_close(_loop);
		delete _loop;
		_loop = nullptr;
		return false;
	}

	const int httpPort = core::Var::getSafe(cfg::ServerHttpPort)->intVal();
	if (!_httpServer->init(httpPort)) {
		Log::error("Failed to initialize the HTTP server on port %i", httpPort);
		return false;
	}
	Log::info("Listen for HTTP requests at port %i", httpPort);

	_httpServer->registerRoute(http::HttpMethod::GET, "/info", [] (const http::RequestParser& request, http::HttpResponse* response) {
		response->setText("Server info");
	});

	if (!_metricMgr->init()) {
		Log::warn("Failed to init metric sender");
	}
	if (!_dbHandler->init()) {
		Log::error("Failed to init the dbhandler");
		return false;
	}
	if (!_dbHandler->createTable(db::UserModel())) {
		Log::error("Failed to create user table");
		return false;
	}
	if (!_dbHandler->createTable(db::AttribModel())) {
		Log::error("Failed to create attrib table");
		return false;
	}
	if (!_dbHandler->createTable(db::InventoryModel())) {
		Log::error("Failed to create stock table");
		return false;
	}
	if (!_dbHandler->createTable(db::CooldownModel())) {
		Log::error("Failed to create cooldown table");
		return false;
	}
	if (!_volumeCache->init()) {
		Log::error("Failed to init volume cache");
		return false;
	}

	const std::string& events = _filesystem->load("events.lua");
	if (!_eventMgr->init(events)) {
		Log::error("Failed to init event manager");
		return false;
	}

	const std::string& cooldowns = _filesystem->load("cooldowns.lua");
	if (!_cooldownProvider->init(cooldowns)) {
		Log::error("Failed to load the cooldown configuration: %s", _cooldownProvider->error().c_str());
		return false;
	}

	const std::string& stockLuaString = _filesystem->load("stock.lua");
	if (!_stockDataProvider->init(stockLuaString)) {
		Log::error("Failed to load the stock configuration: %s", _stockDataProvider->error().c_str());
		return false;
	}

	const std::string& attributes = _filesystem->load("attributes.lua");
	if (!_attribContainerProvider->init(attributes)) {
		Log::error("Failed to load the attributes: %s", _attribContainerProvider->error().c_str());
		return false;
	}

	if (!_persistenceMgr->init()) {
		Log::error("Failed to init the persistence manager");
		return false;
	}

	const network::ProtocolHandlerRegistryPtr& r = _network->registry();
	regHandler(network::ClientMsgType::UserConnect, UserConnectHandler,
			_network, _mapProvider, _dbHandler, _persistenceMgr, _entityStorage, _messageSender,
			_timeProvider, _attribContainerProvider, _cooldownProvider, _stockDataProvider);
	regHandler(network::ClientMsgType::UserConnected, UserConnectedHandler);
	regHandler(network::ClientMsgType::UserDisconnect, UserDisconnectHandler);
	regHandler(network::ClientMsgType::Attack, AttackHandler);
	regHandler(network::ClientMsgType::Move, MoveHandler);
	regHandler(network::ClientMsgType::VarUpdate, VarUpdateHandler);

	if (!voxel::initDefaultMaterialColors()) {
		Log::error("Failed to initialize the palette data");
		return false;
	}

	if (!_world->init()) {
		Log::error("Failed to init the world");
		return false;
	}

	_worldTimer = new uv_timer_t;
	uv_timer_init(_loop, _worldTimer);
	addTimer(_worldTimer, [] (uv_timer_t* handle) {
		core_trace_scoped(WorldTimer);
		const ServerLoop* loop = (const ServerLoop*)handle->data;
		loop->_world->update(handle->repeat);
	}, 1000);

	_persistenceMgrTimer = new uv_timer_t;
	uv_timer_init(_loop, _persistenceMgrTimer);
	addTimer(_persistenceMgrTimer, [] (uv_timer_t* handle) {
		core_trace_scoped(PersistenceTimer);
		const ServerLoop* loop = (const ServerLoop*)handle->data;
		const long dt = handle->repeat;
		const persistence::PersistenceMgrPtr& persistenceMgr = loop->_persistenceMgr;
		core::App::getInstance()->threadPool().enqueue([=] () {
			persistenceMgr->update(dt);
		});
	}, 10000);

	_idleTimer = new uv_idle_t;
	_idleTimer->data = this;
	if (uv_idle_init(_loop, _idleTimer) != 0) {
		Log::warn("Couldn't init the idle timer");
		return false;
	}
	uv_idle_start(_idleTimer, onIdle);

	_signal = new uv_signal_t;
	_signal->data = this;
	uv_signal_init(_loop, _signal);
	uv_signal_start(_signal, signalCallback, SIGHUP);
	uv_signal_start(_signal, signalCallback, SIGINT);

	// init the network last...
	if (!_network->init()) {
		Log::error("Failed to init the network");
		return false;
	}

	const core::VarPtr& port = core::Var::getSafe(cfg::ServerPort);
	const core::VarPtr& host = core::Var::getSafe(cfg::ServerHost);
	const core::VarPtr& maxclients = core::Var::getSafe(cfg::ServerMaxClients);
	if (!_network->bind(port->intVal(), host->strVal(), maxclients->intVal(), 2)) {
		Log::error("Failed to bind the server socket on %s:%i", host->strVal().c_str(), port->intVal());
		return false;
	}
	Log::info("Server socket is up at %s:%i", host->strVal().c_str(), port->intVal());

	return true;
}

void ServerLoop::shutdown() {
	_persistenceMgr->shutdown();
	_world->shutdown();
	_dbHandler->shutdown();
	_metricMgr->shutdown();
	_volumeCache->shutdown();
	_network->shutdown();
	_httpServer->shutdown();
	if (_loop != nullptr) {
		if (_signal != nullptr) {
			uv_close((uv_handle_t*)_signal, nullptr);
		}
		if (_worldTimer != nullptr) {
			uv_close((uv_handle_t*)_worldTimer, nullptr);
		}
		if (_persistenceMgrTimer != nullptr) {
			uv_close((uv_handle_t*)_persistenceMgrTimer, nullptr);
		}
		if (_idleTimer != nullptr) {
			uv_close((uv_handle_t*)_idleTimer, nullptr);
		}
		uv_tty_reset_mode();
		uv_run(_loop, UV_RUN_NOWAIT);
		core_assert_always(uv_loop_close(_loop) == 0);
		delete _signal;
		_signal = nullptr;
		delete _worldTimer;
		_worldTimer = nullptr;
		delete _persistenceMgrTimer;
		_persistenceMgrTimer = nullptr;
		delete _idleTimer;
		_idleTimer = nullptr;
		delete _loop;
		_loop = nullptr;
	}
}

void ServerLoop::update(long dt) {
	core_trace_scoped(ServerLoop);
	// not everything is ticked in here directly, a lot is handled by libuv timers
	uv_run(_loop, UV_RUN_NOWAIT);
	_network->update();
	_httpServer->update();
	const int eventSkip = _eventBus->update(200);
	if (eventSkip != _lastEventSkip) {
		_metricMgr->metric()->gauge("events.skip", eventSkip);
		_lastEventSkip = eventSkip;
	}

	replicateVars();
}

void ServerLoop::replicateVars() const {
	std::vector<core::VarPtr> vars;
	core::Var::visitDirtyReplicate([&vars] (const core::VarPtr& var) {
		vars.push_back(var);
	});
	if (vars.empty()) {
		return;
	}
	static flatbuffers::FlatBufferBuilder fbb;
	auto fbbVars = fbb.CreateVector<flatbuffers::Offset<network::Var>>(vars.size(),
		[&] (size_t i) {
			auto name = fbb.CreateString(vars[i]->name());
			auto value = fbb.CreateString(vars[i]->strVal());
			return network::CreateVar(fbb, name, value);
		});
	_messageSender->broadcastServerMessage(fbb, network::ServerMsgType::VarUpdate,
			network::CreateVarUpdate(fbb, fbbVars).Union());
}

// TODO: doesn't belong here
void ServerLoop::onEvent(const network::DisconnectEvent& event) {
	ENetPeer* peer = event.peer();
	Log::info("disconnect peer: %u", peer->connectID);
	User* user = reinterpret_cast<User*>(peer->data);
	if (user == nullptr) {
		return;
	}
	user->logoutMgr().triggerLogout();
}

}
