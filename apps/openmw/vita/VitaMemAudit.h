#ifndef OPENMW_VITA_MEMAUDIT_H
#define OPENMW_VITA_MEMAUDIT_H

#ifdef __vita__

namespace MWWorld
{
    class ESMStore;
    class WorldModel;
}

namespace Resource
{
    class ResourceSystem;
}

namespace Vita
{
    // Heap audits: [VitaAudit] lines to boot.log + sceClibPrintf.

    // Dialogue store byte census; call after ESMStore::setUp.
    void auditDialogueStore(const MWWorld::ESMStore& store);

    // CellStore residency and pinned LiveCellRef counts.
    void auditWorldModel(MWWorld::WorldModel& worldModel);

    // Resource cache entry counts and byte estimates.
    void auditResourceCaches(Resource::ResourceSystem* resourceSystem);
}

#endif // __vita__

#endif
