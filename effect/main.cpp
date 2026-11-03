#include "truehdr.h"

namespace KWin
{
KWIN_EFFECT_FACTORY_SUPPORTED(TrueHdrEffect, "metadata.json", return TrueHdrEffect::supported();)
}

#include "main.moc"
