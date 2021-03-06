// See the file in the main distribution directory for copyright.

#include "plugin/Plugin.h"

#include "SMB.h"

namespace plugin {
namespace Zeek_SMB {

class Plugin : public plugin::Plugin {
public:
	plugin::Configuration Configure() override
		{
		AddComponent(new ::analyzer::Component("SMB", ::analyzer::smb::SMB_Analyzer::Instantiate));
		AddComponent(new ::analyzer::Component("Contents_SMB", nullptr));

		plugin::Configuration config;
		config.name = "Zeek::SMB";
		config.description = "SMB analyzer";
		return config;
		}
} plugin;

}
}
