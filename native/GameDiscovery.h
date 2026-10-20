#pragma once

#include "EngineApi.h"

#include <vector>

namespace gameoptimizer::engine
{
    std::vector<GO_GAME_TARGET> FindVisibleGames(); // 선택 가능한 앱 창 목록 조회
}
