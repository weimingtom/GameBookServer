#include "thread_mgr.h"
#include "common.h"
#include "message_system.h"
#include "yaml.h"
#include "packet.h"

#include "log4_help.h"
#include "thread_collector_exclusive.h"

#include <iostream>

ThreadMgr::ThreadMgr()
{
}

void ThreadMgr::InitializeThread()
{
    const auto pConfig = Yaml::GetInstance()->GetConfig(Global::GetInstance()->GetCurAppType());
    auto pAppCofig = dynamic_cast<AppConfig*>(pConfig);

    // 线程程上的基本组件
    InitComponent(ThreadType::MainThread);

    if (pAppCofig->LogicThreadNum > 0)
    {
        CreateThread(LogicThread, pAppCofig->LogicThreadNum);
    }

    if (pAppCofig->MysqlThreadNum > 0)
    {
        CreateThread(MysqlThread, pAppCofig->MysqlThreadNum);
    }
}

void ThreadMgr::CreateThread(ThreadType iType, int num)
{
    // 不需要创建线程，单线程
    const auto pConfig = Yaml::GetInstance()->GetConfig(Global::GetInstance()->GetCurAppType());
    auto pAppCofig = dynamic_cast<AppConfig*>(pConfig);
    if (pAppCofig->LogicThreadNum == 0 && pAppCofig->MysqlThreadNum == 0)
        return;

    LOG_DEBUG("Initialize thread:" << GetThreadTypeName(iType) << " thread num:" << num);

    auto iter = _threads.find(iType);
    if (iter == _threads.end())
    {
        if (iType == MysqlThread)
            _threads[iType] = new ThreadCollectorExclusive(iType, num);
        else
            _threads[iType] = new ThreadCollector(iType, num);
    }
    else
    {
        _threads[iType]->CreateThread(num);
    }
}

void ThreadMgr::Update()
{
    UpdateCreatePacket();
    UpdateDispatchPacket();
    SystemManager::Update();
}

void ThreadMgr::UpdateCreatePacket()
{
    _create_lock.lock();
    if (_createPackets.CanSwap()) {
        _createPackets.Swap();
    }
    _create_lock.unlock();

    auto pList = _createPackets.GetReaderCache();
    for (auto iter = pList->begin(); iter != pList->end(); ++iter)
    {
        auto packet = (*iter);
        if (_threads.size() > 0)
        {
            auto pCreateProto = packet->ParseToProto<Proto::CreateComponent>();
            auto threadType = (ThreadType)(pCreateProto.thread_type());
            if (_threads.find(threadType) == _threads.end())
            {
                LOG_ERROR("can't find threadtype:" << GetThreadTypeName(threadType));
                continue;
            }

            auto pThreadCollector = _threads[threadType];
            pThreadCollector->HandlerCreateMessage(packet);
        }
        else
        {
            // 单线程
            GetMessageSystem()->AddPacketToList(packet);
        }
    }
    pList->clear();
}

void ThreadMgr::UpdateDispatchPacket()
{
    _packet_lock.lock();
    if (_packets.CanSwap()) {
        _packets.Swap();
    }
    _packet_lock.unlock();

    auto pList = _packets.GetReaderCache();
    for (auto iter = pList->begin(); iter != pList->end(); ++iter)
    {
        auto pPacket = (*iter);

        // 主线程
        GetMessageSystem()->AddPacketToList(pPacket);

        // 子线程
        for (auto iter = _threads.begin(); iter != _threads.end(); ++iter)
        {
            iter->second->HandlerMessage(pPacket);
        }
    }
    pList->clear();
}

bool ThreadMgr::IsStopAll()
{
    for (auto iter = _threads.begin(); iter != _threads.end(); ++iter)
    {
        if (!iter->second->IsStopAll())
        {
            return false;
        }
    }
    return true;
}

bool ThreadMgr::IsDisposeAll()
{
    for (auto iter = _threads.begin(); iter != _threads.end(); ++iter)
    {
        if (!iter->second->IsDisposeAll())
        {
            return false;
        }
    }
    return true;
}

void ThreadMgr::Dispose()
{
    SystemManager::Dispose();

    for (auto iter = _threads.begin(); iter != _threads.end(); ++iter)
    {
        auto pCollector = iter->second;
        delete pCollector;
    }

    _threads.clear();
}

void ThreadMgr::DispatchPacket(Packet* pPacket)
{
    std::lock_guard<std::mutex> guard(_packet_lock);
    _packets.GetWriterCache()->emplace_back(pPacket);
}
