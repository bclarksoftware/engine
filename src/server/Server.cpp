/**
 * @file
 */

#include "Server.h"

#include "io/Filesystem.h"
#include "core/Var.h"
#include "command/Command.h"
#include "cooldown/CooldownProvider.h"
#include "backend/network/ServerNetwork.h"
#include "backend/network/ServerMessageSender.h"
#include "attrib/ContainerProvider.h"
#include "poi/PoiProvider.h"
#include "eventmgr/EventMgr.h"
#include "backend/entity/EntityStorage.h"
#include "backend/entity/ai/AIRegistry.h"
#include "backend/entity/ai/AILoader.h"
#include "backend/loop/ServerLoop.h"
#include "backend/spawn/SpawnMgr.h"
#include "backend/world/MapProvider.h"
#include "backend/metric/MetricMgr.h"
#include "backend/world/DBChunkPersister.h"
#include "persistence/DBHandler.h"
#include "persistence/PersistenceMgr.h"
#include "stock/StockDataProvider.h"
#include "voxelformat/VolumeCache.h"
#include "compute/Compute.h"
#include <stdlib.h>
#include "engine-config.h"

Server::Server(const metric::MetricPtr& metric, const backend::ServerLoopPtr& serverLoop,
		const core::TimeProviderPtr& timeProvider, const io::FilesystemPtr& filesystem,
		const core::EventBusPtr& eventBus, const http::HttpServerPtr& httpServer) :
		Super(metric, filesystem, eventBus, timeProvider),
		_serverLoop(serverLoop) {
	_syslog = true;
	_coredump = true;
	init(ORGANISATION, "server");
}

app::AppState Server::onConstruct() {
	const app::AppState state = Super::onConstruct();

	core::Var::get(cfg::DatabaseName, "vengi");
	core::Var::get(cfg::DatabaseHost, "localhost");
	core::Var::get(cfg::DatabasePort, "5432");
	core::Var::get(cfg::DatabaseUser, "vengi");
	core::Var::get(cfg::DatabasePassword, DB_PW, core::CV_SECRET);
	core::Var::get(cfg::ServerUserTimeout, "60000");
	core::Var::get(cfg::ServerPort, SERVER_PORT);
	core::Var::get(cfg::ServerHost, "0.0.0.0");
	core::Var::get(cfg::ServerMaxClients, "1024");
	core::Var::get(cfg::ServerHttpPort, HTTP_SERVER_PORT, core::CV_REPLICATE);
	core::Var::get(cfg::ServerSeed, "1", core::CV_REPLICATE);
	core::Var::get(cfg::VoxelMeshSize, "16", core::CV_READONLY);
	core::Var::get(cfg::DatabaseMinConnections, "2");
	core::Var::get(cfg::DatabaseMaxConnections, "100");

	const core::VarPtr& chunkUrl = core::Var::get(cfg::ServerChunkBaseUrl, "", core::CV_REPLICATE);
	if (chunkUrl->strVal().empty()) {
		char hostname[256];
		size_t size = sizeof(hostname);
		if (uv_os_gethostname(hostname, &size) == 0) {
			const core::String& url = core::string::format("http://%s:" HTTP_SERVER_PORT "/chunk", hostname);
			chunkUrl->setVal(url);
		} else {
			chunkUrl->setVal("http://localhost:" HTTP_SERVER_PORT "/chunk");
		}
	}

	_serverLoop->construct();

	return state;
}

app::AppState Server::onInit() {
	const app::AppState state = Super::onInit();
	if (state != app::AppState::Running) {
		return state;
	}

	if (!compute::init()) {
		Log::warn("Failed to initialize the compute module");
		// no hard error...
	}

	if (!_serverLoop->init()) {
		Log::error("Failed to initialize the main loop - can't run server");
		return app::AppState::InitFailure;
	}

	return app::AppState::Running;
}

app::AppState Server::onCleanup() {
	_serverLoop->shutdown();
	return Super::onCleanup();
}

app::AppState Server::onRunning() {
	Super::onRunning();
	_serverLoop->update();
	return app::AppState::Running;
}

int main(int argc, char *argv[]) {
	const core::EventBusPtr& eventBus = std::make_shared<core::EventBus>();
	const core::TimeProviderPtr& timeProvider = std::make_shared<core::TimeProvider>();
	const io::FilesystemPtr& filesystem = std::make_shared<io::Filesystem>();
	const backend::AIRegistryPtr& registry = std::make_shared<backend::LUAAIRegistry>();
	const attrib::ContainerProviderPtr& containerProvider = core::make_shared<attrib::ContainerProvider>();
	const metric::MetricPtr& metric = std::make_shared<metric::Metric>();

	const network::ProtocolHandlerRegistryPtr& protocolHandlerRegistry = core::make_shared<network::ProtocolHandlerRegistry>();
	const network::ServerNetworkPtr& network = std::make_shared<network::ServerNetwork>(protocolHandlerRegistry, eventBus, metric);
	const network::ServerMessageSenderPtr& messageSender = std::make_shared<network::ServerMessageSender>(network, metric);

	const backend::AILoaderPtr& loader = std::make_shared<backend::AILoader>(registry);

	const cooldown::CooldownProviderPtr& cooldownProvider = std::make_shared<cooldown::CooldownProvider>();

	const stock::StockDataProviderPtr& stockDataProvider = std::make_shared<stock::StockDataProvider>();
	const persistence::DBHandlerPtr& dbHandler = std::make_shared<persistence::DBHandler>();
	const persistence::PersistenceMgrPtr& persistenceMgr = std::make_shared<persistence::PersistenceMgr>(dbHandler);
	const backend::EntityStoragePtr& entityStorage = std::make_shared<backend::EntityStorage>(eventBus);
	const voxelformat::VolumeCachePtr& volumeCache = std::make_shared<voxelformat::VolumeCache>();

	const http::HttpServerPtr httpServer = std::make_shared<http::HttpServer>(metric);
	core::Factory<backend::DBChunkPersister> chunkPersisterFactory;
	const backend::MapProviderPtr& mapProvider = std::make_shared<backend::MapProvider>(filesystem, eventBus, timeProvider,
			entityStorage, messageSender, loader, containerProvider, cooldownProvider, persistenceMgr, volumeCache, httpServer,
			chunkPersisterFactory, dbHandler);

	const eventmgr::EventProviderPtr& eventProvider = std::make_shared<eventmgr::EventProvider>(dbHandler);
	const eventmgr::EventMgrPtr& eventMgr = std::make_shared<eventmgr::EventMgr>(eventProvider, timeProvider);

	const backend::WorldPtr& world = std::make_shared<backend::World>(mapProvider, registry, eventBus, filesystem, metric);
	const backend::MetricMgrPtr& metricMgr = std::make_shared<backend::MetricMgr>(metric, eventBus);
	const backend::ServerLoopPtr& serverLoop = std::make_shared<backend::ServerLoop>(timeProvider, mapProvider,
			messageSender, world, dbHandler, network, filesystem, entityStorage, eventBus, containerProvider,
			cooldownProvider, eventMgr, stockDataProvider, metricMgr, persistenceMgr, volumeCache, httpServer);

	Server app(metric, serverLoop, timeProvider, filesystem, eventBus, httpServer);
	return app.startMainLoop(argc, argv);
}
