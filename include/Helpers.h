#pragma once

#include <string>
#include <Windows.h>
#include <gl/GL.h>

#include "core/Vector2.h"

bool LoadTextureFromMemory(const void* data, size_t data_size, GLuint* out_texture, int* out_width, int* out_height);

bool LoadTextureFromFile(const char* file_name, GLuint* out_texture, int* out_width, int* out_height);

core::Vector2 getMouseWorldPosition();
