#pragma once
// =============================================================================
// 互換shim: src/ui_finder.hpp → src/ai/ui_finder.hpp
// 旧 mirage:: 名前空間のシンボルを mirage::ai:: へ転送
// =============================================================================

#include "ai/ui_finder.hpp"

namespace mirage {

// 型エイリアス（旧APIコード向け互換）
using UiElement       = ai::UiElement;
using SearchStrategy  = ai::SearchStrategy;
using CoordinateEntry = ai::CoordinateEntry;
using UiFinder        = ai::UiFinder;

} // namespace mirage
