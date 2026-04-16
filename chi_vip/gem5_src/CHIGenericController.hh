/*
 * Copyright (c) 2023 ARM Limited
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_RUBY_PROTOCOL_CHI_CHIGenericController_HH__
#define __MEM_RUBY_PROTOCOL_CHI_CHIGenericController_HH__

#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "mem/ruby/common/Consumer.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/protocol/AccessPermission.hh"
#include "mem/ruby/slicc_interface/AbstractController.hh"
#include "mem/ruby/system/CacheRecorder.hh"

// SLICC-generated CHI message types (in gem5_src/, on the include path)
#include "CHIDataMsg.hh"
#include "CHIRequestMsg.hh"
#include "CHIResponseMsg.hh"

namespace gem5
{

namespace ruby
{

class CHIGenericController : public AbstractController
{
  public:
    CHIGenericController(const std::string& name,
                         MachineID id,
                         RubySystem* rs,
                         Network* net);

    ~CHIGenericController();

    void init() override;

    MessageBuffer* getMandatoryQueue() const override { return nullptr; }
    MessageBuffer* getMemReqQueue()    const override { return nullptr; }
    MessageBuffer* getMemRespQueue()   const override { return nullptr; }

    void initNetQueues() override;

    void print(std::ostream& out) const;
    void wakeup() override;
    void resetStats()   override {}
    void regStats()     override {}
    void collateStats() override {}

    bool functionalReadBuffers(PacketPtr&)  override { return false; }
    bool functionalWriteBuffers(PacketPtr&) override { return false; }

    AccessPermission getAccessPermission(const Addr&) override {
        return AccessPermission_NotPresent;
    }

  protected:
    // 8 message buffers — all heap-allocated in the constructor
    MessageBuffer* reqOut;
    MessageBuffer* snpOut;
    MessageBuffer* rspOut;
    MessageBuffer* datOut;
    MessageBuffer* reqIn;
    MessageBuffer* snpIn;
    MessageBuffer* rspIn;
    MessageBuffer* datIn;

  public:
    // Test/debug accessors — expose message buffers for inspection
    MessageBuffer* getReqIn()  { return reqIn;  }
    MessageBuffer* getSnpIn()  { return snpIn;  }
    MessageBuffer* getRspIn()  { return rspIn;  }
    MessageBuffer* getDatIn()  { return datIn;  }
    MessageBuffer* getReqOut() { return reqOut; }
    MessageBuffer* getSnpOut() { return snpOut; }
    MessageBuffer* getRspOut() { return rspOut; }
    MessageBuffer* getDatOut() { return datOut; }

  public:
    const int cacheLineSize;
    const int cacheLineBits;
    const int dataChannelSize  = 64;
    const int dataMsgsPerLine  = 1;

    // CHI channel indices (used by initNetQueues)
    enum CHIChannel {
        CHI_REQ = 0,
        CHI_SNP = 1,
        CHI_RSP = 2,
        CHI_DAT = 3
    };

    // Type aliases matching the old gem5 style used by VIP subclass
    using CHIRequestMsg    = CHI::CHIRequestMsg;
    using CHIResponseMsg   = CHI::CHIResponseMsg;
    using CHIDataMsg       = CHI::CHIDataMsg;
    using CHIRequestMsgPtr  = std::shared_ptr<CHIRequestMsg>;
    using CHIResponseMsgPtr = std::shared_ptr<CHIResponseMsg>;
    using CHIDataMsgPtr     = std::shared_ptr<CHIDataMsg>;

    bool sendRequestMsg (CHIRequestMsgPtr  msg) { return sendMessage(msg, reqOut); }
    bool sendSnoopMsg   (CHIRequestMsgPtr  msg) { return sendMessage(msg, snpOut); }
    bool sendResponseMsg(CHIResponseMsgPtr msg) { return sendMessage(msg, rspOut); }
    bool sendDataMsg    (CHIDataMsgPtr     msg) { return sendMessage(msg, datOut); }

  protected:
    virtual bool recvRequestMsg (const CHIRequestMsg*  msg) = 0;
    virtual bool recvSnoopMsg   (const CHIRequestMsg*  msg) = 0;
    virtual bool recvResponseMsg(const CHIResponseMsg* msg) = 0;
    virtual bool recvDataMsg    (const CHIDataMsg*     msg) = 0;

  private:
    template<typename MsgType>
    bool receiveAllRdyMessages(MessageBuffer* buffer,
                               const std::function<bool(const MsgType*)>& callback)
    {
        bool pending = false;
        Tick cur_tick = curTick();
        while (buffer->isReady(cur_tick)) {
            const MsgType* msg = dynamic_cast<const MsgType*>(buffer->peek());
            assert(msg);
            if (callback(msg))
                buffer->dequeue(cur_tick);
            else {
                pending = true;
                break;
            }
        }
        return pending;
    }

    template<typename MessageType>
    bool sendMessage(MessageType& msg, MessageBuffer* buffer)
    {
        Tick cur_tick = curTick();
        if (buffer->areNSlotsAvailable(1, cur_tick)) {
            buffer->enqueue(msg, curTick(), cyclesToTicks(1),
                m_ruby_system->getRandomization(),
                m_ruby_system->getWarmupEnabled());
            return true;
        }
        return false;
    }
};

} // namespace ruby
} // namespace gem5

#endif // __MEM_RUBY_PROTOCOL_CHI_CHIGenericController_HH__
