/*
 * Copyright (c) 2023 ARM Limited
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "CHIGenericController.hh"

#include <cassert>
#include <sstream>
#include <string>

#include "mem/ruby/network/Network.hh"
#include "mem/ruby/system/RubySystem.hh"

namespace gem5
{

namespace ruby
{

using namespace CHI;

CHIGenericController::CHIGenericController(const std::string& name,
                                           MachineID id,
                                           RubySystem* rs,
                                           Network* net)
    : AbstractController(name, id, rs, net)
    , reqOut(new MessageBuffer())
    , snpOut(new MessageBuffer())
    , rspOut(new MessageBuffer())
    , datOut(new MessageBuffer())
    , reqIn (new MessageBuffer())
    , snpIn (new MessageBuffer())
    , rspIn (new MessageBuffer())
    , datIn (new MessageBuffer())
    , cacheLineSize(rs->getBlockSizeBytes())
    , cacheLineBits([](int sz){ int b=0; while(sz>1){sz>>=1;b++;} return b; }(rs->getBlockSizeBytes()))
{
    m_ruby_system = rs;
}

CHIGenericController::~CHIGenericController()
{
    delete reqOut;
    delete snpOut;
    delete rspOut;
    delete datOut;
    delete reqIn;
    delete snpIn;
    delete rspIn;
    delete datIn;
}

void
CHIGenericController::initNetQueues()
{
    int base = m_ruby_system->MachineType_base_number(m_machineID.type);
    int node = m_version + base;

    assert(m_net_ptr != nullptr);

    m_net_ptr->setToNetQueue(node, reqOut->getOrdered(), CHI_REQ, "none",     reqOut);
    m_net_ptr->setToNetQueue(node, snpOut->getOrdered(), CHI_SNP, "none",     snpOut);
    m_net_ptr->setToNetQueue(node, rspOut->getOrdered(), CHI_RSP, "none",     rspOut);
    m_net_ptr->setToNetQueue(node, datOut->getOrdered(), CHI_DAT, "response", datOut);

    m_net_ptr->setFromNetQueue(node, reqIn->getOrdered(), CHI_REQ, "none",     reqIn);
    m_net_ptr->setFromNetQueue(node, snpIn->getOrdered(), CHI_SNP, "none",     snpIn);
    m_net_ptr->setFromNetQueue(node, rspIn->getOrdered(), CHI_RSP, "none",     rspIn);
    m_net_ptr->setFromNetQueue(node, datIn->getOrdered(), CHI_DAT, "response", datIn);
}

void
CHIGenericController::init()
{
    AbstractController::init();

    rspIn->setConsumer(this);
    datIn->setConsumer(this);
    snpIn->setConsumer(this);
    reqIn->setConsumer(this);
}

void
CHIGenericController::print(std::ostream& out) const
{
    out << "[CHIGenericController " << m_version << "]";
}

void
CHIGenericController::wakeup()
{
    bool pending = false;

    DPRINTF(RubyCHIGeneric, "wakeup: checking incoming rsp messages\n");
    pending = pending || receiveAllRdyMessages<CHIResponseMsg>(rspIn,
        [this](const CHIResponseMsg* msg){ return recvResponseMsg(msg); });

    DPRINTF(RubyCHIGeneric, "wakeup: checking incoming dat messages\n");
    pending = pending || receiveAllRdyMessages<CHIDataMsg>(datIn,
        [this](const CHIDataMsg* msg){ return recvDataMsg(msg); });

    DPRINTF(RubyCHIGeneric, "wakeup: checking incoming snp messages\n");
    pending = pending || receiveAllRdyMessages<CHIRequestMsg>(snpIn,
        [this](const CHIRequestMsg* msg){ return recvSnoopMsg(msg); });

    DPRINTF(RubyCHIGeneric, "wakeup: checking incoming req messages\n");
    pending = pending || receiveAllRdyMessages<CHIRequestMsg>(reqIn,
        [this](const CHIRequestMsg* msg){ return recvRequestMsg(msg); });

    if (pending) {
        scheduleEvent(1);
    }
}

} // namespace ruby
} // namespace gem5
