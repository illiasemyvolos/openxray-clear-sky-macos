#pragma once

// Deliberately not xrEngine/stdafx.h. That header reaches the editor and the
// script engine, whose global registrations would have to be linked into a
// module that only holds no-ops. These are what the stubs actually need.

#include "Common/Common.hpp"
#include "xrCore/xrCore.h"

// Engine.h before anything that reaches EngineAPI.h. The two include each
// other, and only this order leaves CEngineAPI declared before Engine.h uses
// it. An upstream cycle, not something this module introduced.
#include "xrEngine/Engine.h"

// xrEngine/Render.h names ImTextureID without including imgui itself, so
// whoever includes it has to have pulled imgui in first.
#include <imgui.h>

#include "Include/xrAPI/xrAPI.h"
