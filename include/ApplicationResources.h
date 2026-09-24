#pragma once

#include <map>
#include <memory>
#include <string>

#include <willpower/application/resourcesystem/Resource.h>
#include <willpower/application/resourcesystem/ResourceFactory.h>

namespace core
{
	class AgentBehaviourRegistry;
	class AgentTagRegistry;
	class World;
}

namespace wp::application::resourcesystem
{
	class ResourceManager;
}

class LuaScriptResource final : public wp::application::resourcesystem::Resource
{
public:
	LuaScriptResource(std::string const& name, std::string const& namesp,
		std::string const& source, std::map<std::string, std::string> const& tags,
		wp::application::resourcesystem::ResourceLocation* location);
	std::string const& text() const { return mText; }

private:
	void create(wp::application::resourcesystem::DataStreamPtr data,
		wp::application::resourcesystem::ResourceManager* manager) override;
	void destroy() override;
	std::string mText;
};

class AgentTagRegistryResource final : public wp::application::resourcesystem::Resource
{
public:
	AgentTagRegistryResource(std::string const& name, std::string const& namesp,
		std::string const& source, std::map<std::string, std::string> const& tags,
		wp::application::resourcesystem::ResourceLocation* location);
	std::shared_ptr<core::AgentTagRegistry> const& registry() const { return mRegistry; }

private:
	void create(wp::application::resourcesystem::DataStreamPtr data,
		wp::application::resourcesystem::ResourceManager* manager) override;
	void destroy() override;
	std::shared_ptr<core::AgentTagRegistry> mRegistry;
};

class AgentBehaviourRegistryResource final : public wp::application::resourcesystem::Resource
{
public:
	AgentBehaviourRegistryResource(std::string const& name, std::string const& namesp,
		std::string const& source, std::map<std::string, std::string> const& tags,
		wp::application::resourcesystem::ResourceLocation* location);
	std::shared_ptr<core::AgentBehaviourRegistry> const& registry() const { return mRegistry; }

private:
	void create(wp::application::resourcesystem::DataStreamPtr data,
		wp::application::resourcesystem::ResourceManager* manager) override;
	void destroy() override;
	std::shared_ptr<core::AgentBehaviourRegistry> mRegistry;
};

class WorldResource final : public wp::application::resourcesystem::Resource
{
public:
	WorldResource(std::string const& name, std::string const& namesp,
		std::string const& source, std::map<std::string, std::string> const& tags,
		wp::application::resourcesystem::ResourceLocation* location);
	std::shared_ptr<core::World> const& world() const { return mWorld; }

private:
	void create(wp::application::resourcesystem::DataStreamPtr data,
		wp::application::resourcesystem::ResourceManager* manager) override;
	void destroy() override;
	std::shared_ptr<core::World> mWorld;
};

void registerApplicationResourceTypes(
	wp::application::resourcesystem::ResourceManager& manager);
