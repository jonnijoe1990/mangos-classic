/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Log.h"
#include "MoveMap.h"
#include "MoveMapSharedDefines.h"

namespace MMAP
{
    // ######################## MMapFactory ########################
    // our global singleton copy
    MMapManager* g_MMapManager = nullptr;

    MMapManager* MMapFactory::createOrGetMMapManager()
    {
        if (g_MMapManager == nullptr)
            g_MMapManager = new MMapManager();

        return g_MMapManager;
    }

    void MMapFactory::clear()
    {
        delete g_MMapManager;

        g_MMapManager = nullptr;
    }

    // ######################## MMapManager ########################
    MMapManager::~MMapManager()
    {
        // by now we should not have maps loaded
        // if we had, tiles in MMapData->mmapLoadedTiles, their actual data is lost!
    }

    bool MMapManager::loadMapData(uint32 mapId)
    {
        // we already have this map loaded?
        if (loadedMMaps.find(mapId) != loadedMMaps.end())
            return true;

        // load and init dtNavMesh - read parameters from file
        uint32 pathLen = sWorld.GetDataPath().length() + strlen("mmaps/%03i.mmap") + 1;
        char* fileName = new char[pathLen];
        snprintf(fileName, pathLen, (sWorld.GetDataPath() + "mmaps/%03i.mmap").c_str(), mapId);

        FILE* file = fopen(fileName, "rb");
        if (!file)
        {
            delete[] fileName;
            return false;
        }

        dtNavMeshParams params;
        fread(&params, sizeof(dtNavMeshParams), 1, file);
        fclose(file);

        dtNavMesh* mesh = dtAllocNavMesh();
        MANGOS_ASSERT(mesh);
        dtStatus dtResult = mesh->init(&params);
        if (dtStatusFailed(dtResult))
        {
            dtFreeNavMesh(mesh);
            sLog.outError("MMAP:loadMapData: Failed to initialize dtNavMesh for mmap %03u from file %s", mapId, fileName);
            delete[] fileName;
            return false;
        }

        delete[] fileName;

        DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:loadMapData: Loaded %03i.mmap", mapId);

        // store inside our map list
        loadedMMaps.emplace(mapId, std::make_unique<MMapData>(mesh));
        return true;
    }

    uint32 MMapManager::packTileID(int32 x, int32 y) const
    {
        return uint32(x << 16 | y);
    }

    bool MMapManager::IsMMapIsLoaded(uint32 mapId, uint32 x, uint32 y) const
    {
        // get this mmap data
        auto itr = loadedMMaps.find(mapId);

        if (itr == loadedMMaps.end())
            return false;

        const auto& mmapData = itr->second;

        uint32 packedGridPos = packTileID(x, y);
        if (mmapData->mmapLoadedTiles.find(packedGridPos) != mmapData->mmapLoadedTiles.end())
            return true;

        return false;
    }

    bool MMapManager::loadMap(uint32 mapId, int32 x, int32 y)
    {
        // make sure the mmap is loaded and ready to load tiles
        if (!loadMapData(mapId))
            return false;

        // get this mmap data
        const auto& mmapData = loadedMMaps[mapId];
        MANGOS_ASSERT(mmapData->navMesh);

        // check if we already have this tile loaded
        uint32 packedGridPos = packTileID(x, y);
        if (mmapData->mmapLoadedTiles.find(packedGridPos) != mmapData->mmapLoadedTiles.end())
        {
            sLog.outError("MMAP:loadMap: Asked to load already loaded navmesh tile. %03u%02i%02i.mmtile", mapId, x, y);
            return false;
        }

        // load this tile :: mmaps/MMMXXYY.mmtile
        uint32 pathLen = sWorld.GetDataPath().length() + strlen("mmaps/%03i%02i%02i.mmtile") + 1;
        char* fileName = new char[pathLen];
        snprintf(fileName, pathLen, (sWorld.GetDataPath() + "mmaps/%03i%02i%02i.mmtile").c_str(), mapId, x, y);

        FILE* file = fopen(fileName, "rb");
        if (!file)
        {
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "ERROR: MMAP:loadMap: Could not open mmtile file '%s'", fileName);
            delete[] fileName;
            return false;
        }
        delete[] fileName;

        // read header
        MmapTileHeader fileHeader;
        fread(&fileHeader, sizeof(MmapTileHeader), 1, file);

        if (fileHeader.mmapMagic != MMAP_MAGIC)
        {
            sLog.outError("MMAP:loadMap: Bad header in mmap %03u%02i%02i.mmtile", mapId, x, y);
            fclose(file);
            return false;
        }

        if (fileHeader.mmapVersion != MMAP_VERSION)
        {
            sLog.outError("MMAP:loadMap: %03u%02i%02i.mmtile was built with generator v%i, expected v%i",
                          mapId, x, y, fileHeader.mmapVersion, MMAP_VERSION);
            fclose(file);
            return false;
        }

        unsigned char* data = (unsigned char*)dtAlloc(fileHeader.size, DT_ALLOC_PERM);
        MANGOS_ASSERT(data);

        size_t result = fread(data, fileHeader.size, 1, file);
        if (!result)
        {
            sLog.outError("MMAP:loadMap: Bad header or data in mmap %03u%02i%02i.mmtile", mapId, x, y);
            fclose(file);
            return false;
        }

        fclose(file);

        dtMeshHeader* header = (dtMeshHeader*)data;
        dtTileRef tileRef = 0;

        // memory allocated for data is now managed by detour, and will be deallocated when the tile is removed
        dtStatus dtResult = mmapData->navMesh->addTile(data, fileHeader.size, DT_TILE_FREE_DATA, 0, &tileRef);
        if (dtStatusFailed(dtResult))
        {
            sLog.outError("MMAP:loadMap: Could not load %03u%02i%02i.mmtile into navmesh", mapId, x, y);
            dtFree(data);
            return false;
        }

        mmapData->mmapLoadedTiles.insert(std::pair<uint32, dtTileRef>(packedGridPos, tileRef));
        ++loadedTiles;
        DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:loadMap: Loaded mmtile %03i[%02i,%02i] into %03i[%02i,%02i]", mapId, x, y, mapId, header->x, header->y);
        return true;
    }

    void MMapManager::loadAllGameObjectModels(std::vector<uint32> const& displayIds)
    {
        for (uint32 displayId : displayIds)
            loadGameObject(displayId);
    }

    bool MMapManager::loadGameObject(uint32 displayId)
    {
        // we already have this map loaded?
        if (m_loadedModels.find(displayId) != m_loadedModels.end())
            return true;

        // load and init dtNavMesh - read parameters from file
        uint32 pathLen = sWorld.GetDataPath().length() + strlen("mmaps/go%04i.mmtile") + 1;
        char* fileName = new char[pathLen];
        snprintf(fileName, pathLen, (sWorld.GetDataPath() + "mmaps/go%04i.mmtile").c_str(), displayId);

        FILE* file = fopen(fileName, "rb");
        if (!file)
        {
            DEBUG_LOG("MMAP:loadGameObject: Error: Could not open mmap file %s", fileName);
            delete[] fileName;
            return false;
        }

        MmapTileHeader fileHeader;
        fread(&fileHeader, sizeof(MmapTileHeader), 1, file);

        if (fileHeader.mmapMagic != MMAP_MAGIC)
        {
            sLog.outError("MMAP:loadGameObject: Bad header in mmap %s", fileName);
            fclose(file);
            return false;
        }

        if (fileHeader.mmapVersion != MMAP_VERSION)
        {
            sLog.outError("MMAP:loadGameObject: %s was built with generator v%i, expected v%i",
                fileName, fileHeader.mmapVersion, MMAP_VERSION);
            fclose(file);
            return false;
        }
        unsigned char* data = (unsigned char*)dtAlloc(fileHeader.size, DT_ALLOC_PERM);
        MANGOS_ASSERT(data);

        size_t result = fread(data, fileHeader.size, 1, file);
        if (!result)
        {
            sLog.outError("MMAP:loadGameObject: Bad header or data in mmap %s", fileName);
            fclose(file);
            return false;
        }

        fclose(file);

        dtNavMesh* mesh = dtAllocNavMesh();
        MANGOS_ASSERT(mesh);
        dtStatus r = mesh->init(data, fileHeader.size, DT_TILE_FREE_DATA);
        if (dtStatusFailed(r))
        {
            dtFreeNavMesh(mesh);
            sLog.outError("MMAP:loadGameObject: Failed to initialize dtNavMesh from file %s. Result 0x%x.", fileName, r);
            delete[] fileName;
            return false;
        }
        DETAIL_LOG("MMAP:loadGameObject: Loaded file %s [size=%u]", fileName, fileHeader.size);
        delete[] fileName;

        m_loadedModels.emplace(displayId, std::make_unique<MMapGOData>(mesh));
        return true;
    }

    bool MMapManager::unloadMap(uint32 mapId, int32 x, int32 y)
    {
        // check if we have this map loaded
        if (loadedMMaps.find(mapId) == loadedMMaps.end())
        {
            // file may not exist, therefore not loaded
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMap: Asked to unload not loaded navmesh map. %03u%02i%02i.mmtile", mapId, x, y);
            return false;
        }

        const auto& mmapData = loadedMMaps[mapId];

        // check if we have this tile loaded
        uint32 packedGridPos = packTileID(x, y);
        if (mmapData->mmapLoadedTiles.find(packedGridPos) == mmapData->mmapLoadedTiles.end())
        {
            // file may not exist, therefore not loaded
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMap: Asked to unload not loaded navmesh tile. %03u%02i%02i.mmtile", mapId, x, y);
            return false;
        }

        dtTileRef tileRef = mmapData->mmapLoadedTiles[packedGridPos];

        // unload, and mark as non loaded
        dtStatus dtResult = mmapData->navMesh->removeTile(tileRef, nullptr, nullptr);
        if (dtStatusFailed(dtResult))
        {
            // this is technically a memory leak
            // if the grid is later reloaded, dtNavMesh::addTile will return error but no extra memory is used
            // we cannot recover from this error - assert out
            sLog.outError("MMAP:unloadMap: Could not unload %03u%02i%02i.mmtile from navmesh", mapId, x, y);
            MANGOS_ASSERT(false);
        }
        else
        {
            mmapData->mmapLoadedTiles.erase(packedGridPos);
            --loadedTiles;
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMap: Unloaded mmtile %03i[%02i,%02i] from %03i", mapId, x, y, mapId);
            return true;
        }

        return false;
    }

    bool MMapManager::unloadMap(uint32 mapId)
    {
        if (loadedMMaps.find(mapId) == loadedMMaps.end())
        {
            // file may not exist, therefore not loaded
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMap: Asked to unload not loaded navmesh map %03u", mapId);
            return false;
        }

        // unload all tiles from given map
        const auto& mmapData = loadedMMaps[mapId];
        for (MMapTileSet::iterator i = mmapData->mmapLoadedTiles.begin(); i != mmapData->mmapLoadedTiles.end(); ++i)
        {
            uint32 x = (i->first >> 16);
            uint32 y = (i->first & 0x0000FFFF);
            dtStatus dtResult = mmapData->navMesh->removeTile(i->second, nullptr, nullptr);
            if (dtStatusFailed(dtResult))
                sLog.outError("MMAP:unloadMap: Could not unload %03u%02i%02i.mmtile from navmesh", mapId, x, y);
            else
            {
                --loadedTiles;
                DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMap: Unloaded mmtile %03i[%02i,%02i] from %03i", mapId, x, y, mapId);
            }
        }

        loadedMMaps.erase(mapId);
        DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMap: Unloaded %03i.mmap", mapId);

        return true;
    }

    bool MMapManager::unloadMapInstance(uint32 mapId, uint32 instanceId)
    {
        // check if we have this map loaded
        if (loadedMMaps.find(mapId) == loadedMMaps.end())
        {
            // file may not exist, therefore not loaded
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMapInstance: Asked to unload not loaded navmesh map %03u", mapId);
            return false;
        }

        const auto& mmapData = loadedMMaps[mapId];
        if (mmapData->navMeshQueries.find(instanceId) == mmapData->navMeshQueries.end())
        {
            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMapInstance: Asked to unload not loaded dtNavMeshQuery mapId %03u instanceId %u", mapId, instanceId);
            return false;
        }

        dtNavMeshQuery* query = mmapData->navMeshQueries[instanceId];

        dtFreeNavMeshQuery(query);
        mmapData->navMeshQueries.erase(instanceId);
        DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:unloadMapInstance: Unloaded mapId %03u instanceId %u", mapId, instanceId);

        return true;
    }

    dtNavMesh const* MMapManager::GetNavMesh(uint32 mapId)
    {
        if (loadedMMaps.find(mapId) == loadedMMaps.end())
            return nullptr;

        return loadedMMaps[mapId]->navMesh;
    }

    dtNavMesh const* MMapManager::GetGONavMesh(uint32 mapId)
    {
        if (m_loadedModels.find(mapId) == m_loadedModels.end())
            return nullptr;

        return m_loadedModels[mapId]->navMesh;
    }

    dtNavMeshQuery const* MMapManager::GetNavMeshQuery(uint32 mapId, uint32 instanceId)
    {
        if (loadedMMaps.find(mapId) == loadedMMaps.end())
            return nullptr;

        const auto& mmapData = loadedMMaps[mapId];
        if (mmapData->navMeshQueries.find(instanceId) == mmapData->navMeshQueries.end())
        {
            // allocate mesh query
            dtNavMeshQuery* query = dtAllocNavMeshQuery();
            MANGOS_ASSERT(query);
            dtStatus dtResult = query->init(mmapData->navMesh, 1024);
            if (dtStatusFailed(dtResult))
            {
                dtFreeNavMeshQuery(query);
                sLog.outError("MMAP:GetNavMeshQuery: Failed to initialize dtNavMeshQuery for mapId %03u instanceId %u", mapId, instanceId);
                return nullptr;
            }

            DEBUG_FILTER_LOG(LOG_FILTER_MAP_LOADING, "MMAP:GetNavMeshQuery: created dtNavMeshQuery for mapId %03u instanceId %u", mapId, instanceId);
            mmapData->navMeshQueries.insert(std::pair<uint32, dtNavMeshQuery*>(instanceId, query));
        }

        return mmapData->navMeshQueries[instanceId];
    }

    dtNavMeshQuery const* MMapManager::GetModelNavMeshQuery(uint32 displayId)
    {
        if (m_loadedModels.find(displayId) == m_loadedModels.end())
            return nullptr;

        auto threadId = std::this_thread::get_id();
        const auto& mmapGOData = m_loadedModels[displayId];
        if (mmapGOData->navMeshGOQueries.find(threadId) == mmapGOData->navMeshGOQueries.end())
        {
            std::lock_guard<std::mutex> guard(m_modelsMutex);
            if (mmapGOData->navMeshGOQueries.find(threadId) == mmapGOData->navMeshGOQueries.end())
            {
                // allocate mesh query
                std::stringstream ss;
                ss << threadId;
                dtNavMeshQuery* query = dtAllocNavMeshQuery();
                MANGOS_ASSERT(query);
                if (dtStatusFailed(query->init(mmapGOData->navMesh, 2048)))
                {
                    dtFreeNavMeshQuery(query);
                    sLog.outError("MMAP:GetNavMeshQuery: Failed to initialize dtNavMeshQuery for displayid %03u tid %s", displayId, ss.str().data());
                    return nullptr;
                }

                DETAIL_LOG("MMAP:GetNavMeshQuery: created dtNavMeshQuery for displayid %03u tid %s", displayId, ss.str().data());
                mmapGOData->navMeshGOQueries.insert(std::pair<std::thread::id, dtNavMeshQuery*>(threadId, query));
            }
        }

        return mmapGOData->navMeshGOQueries[threadId];
    }
}
