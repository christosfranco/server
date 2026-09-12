// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MANGOS_GUILD_BANK_CLEANUP_H
#define MANGOS_GUILD_BANK_CLEANUP_H

namespace GuildBankPersistence
{
    inline bool CanCleanup(bool blocked, bool durable)
    {
        return !blocked || !durable;
    }

    // A rolled-back deposit leaves caller-owned item identities in this cache.
    // Destruction may free memory, but cannot infer durable ownership from it.
    template<class Tabs>
    bool DeleteItems(Tabs& tabs, bool blocked, bool durable)
    {
        if (!CanCleanup(blocked, durable))
        {
            return false;
        }
        for (auto* tab : tabs)
        {
            for (auto* item : tab->Slots)
            {
                if (item)
                {
                    item->RemoveFromWorld();
                    if (durable)
                    {
                        item->DeleteFromDB();
                    }
                    delete item;
                }
            }
            delete tab;
        }
        tabs.clear();
        return true;
    }
}
#endif
