#include "cellbindings.hpp"

#include <components/esm3/loadacti.hpp>
#include <components/esm3/loadalch.hpp>
#include <components/esm3/loadappa.hpp>
#include <components/esm3/loadarmo.hpp>
#include <components/esm3/loadbook.hpp>
#include <components/esm3/loadclot.hpp>
#include <components/esm3/loadcont.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/esm3/loaddoor.hpp>
#include <components/esm3/loadingr.hpp>
#include <components/esm3/loadlevlist.hpp>
#include <components/esm3/loadligh.hpp>
#include <components/esm3/loadlock.hpp>
#include <components/esm3/loadmisc.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadprob.hpp>
#include <components/esm3/loadrepa.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/esm3/loadweap.hpp>

#include <components/esm4/loadacti.hpp>
#include <components/esm4/loadalch.hpp>
#include <components/esm4/loadammo.hpp>
#include <components/esm4/loadarmo.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/esm4/loadclot.hpp>
#include <components/esm4/loadcont.hpp>
#include <components/esm4/loaddoor.hpp>
#include <components/esm4/loadflor.hpp>
#include <components/esm4/loadfurn.hpp>
#include <components/esm4/loadimod.hpp>
#include <components/esm4/loadingr.hpp>
#include <components/esm4/loadligh.hpp>
#include <components/esm4/loadmisc.hpp>
#include <components/esm4/loadmstt.hpp>
#include <components/esm4/loadscol.hpp>
#include <components/esm4/loadstat.hpp>
#include <components/esm4/loadtree.hpp>
#include <components/esm4/loadweap.hpp>

#include <components/misc/convert.hpp>

#include <components/translation/translation.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/worldmodel.hpp"

#include "types/types.hpp"

namespace sol
{
    template <>
    struct is_automagical<MWLua::LCell> : std::false_type
    {
    };
    template <>
    struct is_automagical<MWLua::GCell> : std::false_type
    {
    };
    template <>
    struct is_automagical<ESM::Pathgrid> : std::false_type
    {
    };
}

namespace MWLua
{
    LCell::LCell(const MWWorld::CellStore* store)
        : mId(store->getCell()->getId())
    {
    }

    MWWorld::CellStore* LCell::store() const
    {
        return &MWBase::Environment::get().getWorldModel()->getCell(mId);
    }

    GCell::GCell(const MWWorld::CellStore* store)
        : mId(store->getCell()->getId())
    {
    }

    MWWorld::CellStore* GCell::store() const
    {
        return &MWBase::Environment::get().getWorldModel()->getCell(mId);
    }


    template <class CellT, class ObjectT>
    static void initCellBindings(const std::string& prefix, const Context& context)
    {
        auto view = context.sol();
        sol::usertype<CellT> cellT = view.new_usertype<CellT>(prefix + "Cell");

        cellT[sol::meta_function::equal_to] = [](const CellT& a, const CellT& b) { return a.mId == b.mId; };
        cellT[sol::meta_function::to_string] = [](const CellT& c) {
            auto cell = c.store()->getCell();
            std::stringstream res;
            if (cell->isExterior())
                res << "exterior(" << cell->getGridX() << ", " << cell->getGridY() << ", "
                    << cell->getWorldSpace().toDebugString() << ")";
            else
                res << "interior(" << cell->getNameId() << ")";
            return res.str();
        };

        cellT["name"] = sol::readonly_property([](const CellT& c) { return c.store()->getCell()->getNameId(); });
        cellT["displayName"] = sol::readonly_property([](const CellT& c) -> std::string_view {
            const auto& storage = MWBase::Environment::get().getWindowManager()->getTranslationDataStorage();
            return storage.translateCellName(c.store()->getCell()->getNameId());
        });
        cellT["id"] = sol::readonly_property([](const CellT& c) -> ESM::RefId { return c.store()->getCell()->getId(); });
        cellT["region"]
            = sol::readonly_property([](const CellT& c) -> ESM::RefId { return c.store()->getCell()->getRegion(); });
        cellT["worldSpaceId"]
            = sol::readonly_property([](const CellT& c) -> ESM::RefId { return c.store()->getCell()->getWorldSpace(); });
        cellT["gridX"] = sol::readonly_property([](const CellT& c) { return c.store()->getCell()->getGridX(); });
        cellT["gridY"] = sol::readonly_property([](const CellT& c) { return c.store()->getCell()->getGridY(); });
        cellT["hasWater"] = sol::readonly_property([](const CellT& c) { return c.store()->getCell()->hasWater(); });
        cellT["hasSky"] = sol::readonly_property([](const CellT& c) {
            return c.store()->getCell()->isExterior() || (c.store()->getCell()->isQuasiExterior()) != 0;
        });
        cellT["isExterior"] = sol::readonly_property([](const CellT& c) { return c.store()->isExterior(); });

        // deprecated, use cell:hasTag("QuasiExterior") instead
        cellT["isQuasiExterior"]
            = sol::readonly_property([](const CellT& c) { return (c.store()->getCell()->isQuasiExterior()) != 0; });

        cellT["hasTag"] = [](const CellT& c, std::string_view tag) -> bool {
            if (tag == "NoSleep")
                return (c.store()->getCell()->noSleep()) != 0;
            else if (tag == "QuasiExterior")
                return (c.store()->getCell()->isQuasiExterior()) != 0;
            return false;
        };

        cellT["isInSameSpace"] = [](const CellT& c, const ObjectT& obj) {
            const MWWorld::Ptr& ptr = obj.ptr();
            if (!ptr.isInCell())
                return false;
            MWWorld::CellStore* cell = ptr.getCell();
            return cell == c.store() || (cell->getCell()->getWorldSpace() == c.store()->getCell()->getWorldSpace());
        };

        cellT["waterLevel"] = sol::readonly_property([](const CellT& c) -> sol::optional<float> {
            if (c.store()->getCell()->hasWater())
                return c.store()->getWaterLevel();
            else
                return sol::nullopt;
        });

        cellT["pathGrid"] = sol::readonly_property([](const CellT& c) -> const ESM::Pathgrid* {
            const ESM::Pathgrid* grid
                = MWBase::Environment::get().getESMStore()->get<ESM::Pathgrid>().search(*c.store()->getCell());
            if (grid && grid->mPoints.empty())
                return nullptr;
            return grid;
        });

        if constexpr (std::is_same_v<CellT, GCell>)
        { // only for global scripts
            cellT["getAll"] = [ids = getPackageToTypeTable(view)](const CellT& cell, sol::optional<sol::table> type) {
                if (cell.store()->getState() != MWWorld::CellStore::State_Loaded)
                    cell.store()->load();
                ObjectIdList res = std::make_shared<std::vector<ObjectId>>();
                auto visitor = [&](const MWWorld::Ptr& ptr) {
                    if (ptr.mRef->isDeleted())
                        return true;
                    MWBase::Environment::get().getWorldModel()->registerPtr(ptr);
                    if (getLiveCellRefType(ptr.mRef) == ptr.getType())
                        res->push_back(getId(ptr));
                    return true;
                };

                bool ok = true;
                if (!type.has_value())
                    cell.store()->forEach(std::move(visitor));
                else if (ids[*type] == sol::nil)
                    ok = false;
                else
                {
                    uint32_t typeId = ids[*type];
                    switch (typeId)
                    {
                        case ESM::REC_INTERNAL_PLAYER:
                        {
                            MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
                            if (player.getCell() == cell.store())
                                res->push_back(getId(player));
                        }
                        break;

                        case ESM::REC_CREA:
                            cell.store()->template forEachType<ESM::Creature>(visitor);
                            break;
                        case ESM::REC_NPC_:
                            cell.store()->template forEachType<ESM::NPC>(visitor);
                            break;
                        case ESM::REC_ACTI:
                            cell.store()->template forEachType<ESM::Activator>(visitor);
                            break;
                        case ESM::REC_DOOR:
                            cell.store()->template forEachType<ESM::Door>(visitor);
                            break;
                        case ESM::REC_CONT:
                            cell.store()->template forEachType<ESM::Container>(visitor);
                            break;

                        case ESM::REC_ALCH:
                            cell.store()->template forEachType<ESM::Potion>(visitor);
                            break;
                        case ESM::REC_ARMO:
                            cell.store()->template forEachType<ESM::Armor>(visitor);
                            break;
                        case ESM::REC_BOOK:
                            cell.store()->template forEachType<ESM::Book>(visitor);
                            break;
                        case ESM::REC_CLOT:
                            cell.store()->template forEachType<ESM::Clothing>(visitor);
                            break;
                        case ESM::REC_INGR:
                            cell.store()->template forEachType<ESM::Ingredient>(visitor);
                            break;
                        case ESM::REC_LIGH:
                            cell.store()->template forEachType<ESM::Light>(visitor);
                            break;
                        case ESM::REC_MISC:
                            cell.store()->template forEachType<ESM::Miscellaneous>(visitor);
                            break;
                        case ESM::REC_WEAP:
                            cell.store()->template forEachType<ESM::Weapon>(visitor);
                            break;
                        case ESM::REC_APPA:
                            cell.store()->template forEachType<ESM::Apparatus>(visitor);
                            break;
                        case ESM::REC_LOCK:
                            cell.store()->template forEachType<ESM::Lockpick>(visitor);
                            break;
                        case ESM::REC_PROB:
                            cell.store()->template forEachType<ESM::Probe>(visitor);
                            break;
                        case ESM::REC_REPA:
                            cell.store()->template forEachType<ESM::Repair>(visitor);
                            break;
                        case ESM::REC_STAT:
                            cell.store()->template forEachType<ESM::Static>(visitor);
                            break;
                        case ESM::REC_LEVC:
                            cell.store()->template forEachType<ESM::CreatureLevList>(visitor);
                            break;

                        case ESM::REC_ACTI4:
                            cell.store()->template forEachType<ESM4::Activator>(visitor);
                            break;
                        case ESM::REC_AMMO4:
                            cell.store()->template forEachType<ESM4::Ammunition>(visitor);
                            break;
                        case ESM::REC_ARMO4:
                            cell.store()->template forEachType<ESM4::Armor>(visitor);
                            break;
                        case ESM::REC_BOOK4:
                            cell.store()->template forEachType<ESM4::Book>(visitor);
                            break;
                        case ESM::REC_CLOT4:
                            cell.store()->template forEachType<ESM4::Clothing>(visitor);
                            break;
                        case ESM::REC_CONT4:
                            cell.store()->template forEachType<ESM4::Container>(visitor);
                            break;
                        case ESM::REC_DOOR4:
                            cell.store()->template forEachType<ESM4::Door>(visitor);
                            break;
                        case ESM::REC_FLOR4:
                            cell.store()->template forEachType<ESM4::Flora>(visitor);
                            break;
                        case ESM::REC_FURN4:
                            cell.store()->template forEachType<ESM4::Furniture>(visitor);
                            break;
                        case ESM::REC_IMOD4:
                            cell.store()->template forEachType<ESM4::ItemMod>(visitor);
                            break;
                        case ESM::REC_INGR4:
                            cell.store()->template forEachType<ESM4::Ingredient>(visitor);
                            break;
                        case ESM::REC_LIGH4:
                            cell.store()->template forEachType<ESM4::Light>(visitor);
                            break;
                        case ESM::REC_MISC4:
                            cell.store()->template forEachType<ESM4::MiscItem>(visitor);
                            break;
                        case ESM::REC_MSTT4:
                            cell.store()->template forEachType<ESM4::MovableStatic>(visitor);
                            break;
                        case ESM::REC_ALCH4:
                            cell.store()->template forEachType<ESM4::Potion>(visitor);
                            break;
                        case ESM::REC_SCOL4:
                            cell.store()->template forEachType<ESM4::StaticCollection>(visitor);
                            break;
                        case ESM::REC_STAT4:
                            cell.store()->template forEachType<ESM4::Static>(visitor);
                            break;
                        case ESM::REC_TREE4:
                            cell.store()->template forEachType<ESM4::Tree>(visitor);
                            break;
                        case ESM::REC_WEAP4:
                            cell.store()->template forEachType<ESM4::Weapon>(visitor);
                            break;

                        default:
                            ok = false;
                    }
                }
                if (!ok)
                    throw std::runtime_error(
                        std::string("Incorrect type argument in cell:getAll: " + LuaUtil::toString(*type)));
                return GObjectList{ std::move(res) };
            };
        }

        if (context.initializeOnce("openmw_cellbindings"))
        {
            auto pathGridT = view.new_usertype<ESM::Pathgrid>("ESM3_PathGrid");
            pathGridT[sol::meta_function::to_string] = [](const ESM::Pathgrid& rec) -> std::string {
                return "ESM3_PathGrid[" + rec.mCell.toDebugString() + "]";
            };
            pathGridT["getPoints"] = [](sol::this_state lua, const ESM::Pathgrid& rec) -> sol::table {
                sol::table points(lua, sol::create);
                for (const ESM::Pathgrid::Point& point : rec.mPoints)
                {
                    sol::table table(lua, sol::create);
                    table["autoGenerated"] = point.mAutogenerated == 0;
                    table["relativePosition"] = Misc::Convert::makeOsgVec3f(point);
                    sol::table edges(lua, sol::create);
                    table["connections"] = edges;
                    points.add(table);
                }
                for (const ESM::Pathgrid::Edge& edge : rec.mEdges)
                {
                    sol::table p1 = points[edge.mV0 + 1];
                    sol::table p2 = points[edge.mV1 + 1];
                    p1.get<sol::table>("connections").add(p2);
                    p2.get<sol::table>("connections").add(p1);
                }
                return points;
            };
        }
    }

    void initCellBindingsForLocalScripts(const Context& context)
    {
        initCellBindings<LCell, LObject>("L", context);
    }

    void initCellBindingsForGlobalScripts(const Context& context)
    {
        initCellBindings<GCell, GObject>("G", context);
    }

}
