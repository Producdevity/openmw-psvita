#ifndef OPENMW_MECHANICS_SUMMONING_H
#define OPENMW_MECHANICS_SUMMONING_H

#include <string_view>
#include <utility>
#include <vector>

#include <components/esm3/refnum.hpp>

namespace ESM
{
    class RefId;
}
namespace MWWorld
{
    class Ptr;
}

namespace MWMechanics
{
    bool isSummoningEffect(ESM::RefId effectId);

    ESM::RefId getSummonedCreature(ESM::RefId effectId);

    /// Every creature a summon effect can produce. Summons appear instantly
    /// on cast, so their assets must already be warm.
    void getSummonableCreatures(std::vector<ESM::RefId>& out);

    void purgeSummonEffect(const MWWorld::Ptr& summoner, const std::pair<ESM::RefId, ESM::RefNum>& summon);

    ESM::RefNum summonCreature(ESM::RefId effectId, const MWWorld::Ptr& summoner);

    void updateSummons(const MWWorld::Ptr& summoner, bool cleanup);
}

#endif
