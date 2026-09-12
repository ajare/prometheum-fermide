#pragma once

#include <string>
#include <Windows.h>
#include <gl/GL.h>

#pragma warning(push)
#pragma warning(disable: 4307)
#include <spdlog/spdlog.h>
#pragma warning(pop)

#include "core/Vector2.h"


struct LogMessage
{
	std::string level;
	spdlog::string_view_t msg;
};

bool LoadTextureFromMemory(const void* data, size_t data_size, GLuint* out_texture, int* out_width, int* out_height);

bool LoadTextureFromFile(const char* file_name, GLuint* out_texture, int* out_width, int* out_height);

core::Vector2 getMouseWorldPosition();
