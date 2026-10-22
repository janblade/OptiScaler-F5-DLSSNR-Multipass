#pragma once

class Config;

// The main menu's Plugins page: the asi files the loader saw, the built-in MFG unlock (no file needed) and the optional
// loaders, each with the state the loader recorded (plugins/PluginRegistry.h). Changes apply at the next start.
namespace PluginsPage
{
void Render(Config* config);
} // namespace PluginsPage
