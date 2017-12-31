/**
 * @file
 */

#pragma once

#include "NuklearApp.h"
#include "commonlua/LUA.h"

namespace nuklear {

/**
 * @brief The lua UI application is using a lua script with in @code ui/$appname$.lua @endcode to assemble the
 * UI. This script is automatically reloaded if it is changed in the filesystem.
 */
class LUAUIApp : public NuklearApp {
private:
	using Super = NuklearApp;
protected:
	lua::LUA _lua;
public:
	LUAUIApp(const io::FilesystemPtr& filesystem, const core::EventBusPtr& eventBus, const core::TimeProviderPtr& timeProvider, uint16_t traceport = 17815);
	virtual ~LUAUIApp();

	core::AppState onInit() override;

	bool reload();
	void onRenderUI() override;
};

}
