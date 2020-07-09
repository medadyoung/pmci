#include "MCTPBinding.hpp"

#include <systemd/sd-id128.h>

#include <phosphor-logging/log.hpp>

#include "libmctp-msgtypes.h"
#include "libmctp.h"

constexpr sd_id128_t mctpdAppId = SD_ID128_MAKE(c4, e4, d9, 4a, 88, 43, 4d, f0,
                                                94, 9d, bb, 0a, af, 53, 4e, 6d);
constexpr int ctrlTxPollInterval = 5;
constexpr int ctrlTxRetryDelay = 100;
constexpr int ctrlTxRetryCount = 2;

// map<EID, assigned>
static std::unordered_map<mctp_eid_t, bool> eidPoolMap;
std::mutex eidPoolLock;
bool ctrlTxTimerExpired = true;
// <state, retryCount, maxRespDelay, destEid, BindingPrivate, ReqPacket,
//  Callback>
static std::vector<
    std::tuple<PacketState, int, int, mctp_eid_t, std::vector<uint8_t>,
               std::vector<uint8_t>,
               std::function<void(PacketState, std::vector<uint8_t>&)>>>
    ctrlTxQueue;
std::mutex ctrlTxQueueLock;

void rxMessage(uint8_t srcEid, void* /*data*/, void* msg, size_t len,
               void* /*binding_private*/)
{
    uint8_t* payload = reinterpret_cast<uint8_t*>(msg);
    uint8_t msgType = payload[0]; // Always the first byte
    uint8_t msgTag = 0;           // Currently libmctp doesn't expose msgTag
    bool tagOwner = false;
    std::vector<uint8_t> response;

    response.assign(payload, payload + len);

    if (msgType != MCTP_MESSAGE_TYPE_MCTP_CTRL)
    {
        auto msgSignal =
            conn->new_signal("/xyz/openbmc_project/mctp",
                             mctp_server::interface, "MessageReceivedSignal");
        msgSignal.append(msgType, srcEid, msgTag, tagOwner, response);
        msgSignal.signal_send();
    }
}

bool MctpBinding::getBindingPrivateData(uint8_t /*dstEid*/,
                                        std::vector<uint8_t>& pvtData)
{
    // No Binding data by default
    pvtData.clear();
    return true;
}

MctpBinding::MctpBinding(std::shared_ptr<object_server>& objServer,
                         std::string& objPath, ConfigurationVariant& conf,
                         boost::asio::io_context& ioc) :
    io(ioc),
    objectServer(objServer), ctrlTxTimer(io)
{
    mctpInterface = objServer->add_interface(objPath, mctp_server::interface);

    try
    {
        if (SMBusConfiguration* smbusConf =
                std::get_if<SMBusConfiguration>(&conf))
        {
            ownEid = smbusConf->defaultEid;
            bindingID = smbusConf->bindingType;
            bindingMediumID = smbusConf->mediumId;
            bindingModeType = smbusConf->mode;

            // TODO: Add bus owner interface.
            // TODO: If we are not top most busowner, wait for top mostbus owner
            // to issue EID Pool
            if (smbusConf->mode == mctp_server::BindingModeTypes::BusOwner)
            {
                initializeEidPool(smbusConf->eidPool);
            }
        }
        else if (PcieConfiguration* pcieConf =
                     std::get_if<PcieConfiguration>(&conf))
        {
            ownEid = pcieConf->defaultEid;
            bindingID = pcieConf->bindingType;
            bindingMediumID = pcieConf->mediumId;
            bindingModeType = pcieConf->mode;
        }
        else
        {
            throw std::system_error(
                std::make_error_code(std::errc::invalid_argument));
        }

        createUuid();
        registerProperty(mctpInterface, "Eid", ownEid);

        registerProperty(mctpInterface, "StaticEid", staticEid);

        registerProperty(mctpInterface, "Uuid", uuid);

        registerProperty(mctpInterface, "BindingID",
                         mctp_server::convertBindingTypesToString(bindingID));

        registerProperty(
            mctpInterface, "BindingMediumID",
            mctp_server::convertMctpPhysicalMediumIdentifiersToString(
                bindingMediumID));

        registerProperty(
            mctpInterface, "BindingMode",
            mctp_server::convertBindingModeTypesToString(bindingModeType));

        mctpInterface->register_method(
            "SendMctpMessagePayload",
            [this](uint8_t dstEid, uint8_t msgTag, bool tagOwner,
                   std::vector<uint8_t> payload) {
                tagOwner = tagOwner;
                msgTag = msgTag;

                std::vector<uint8_t> pvtData;

                getBindingPrivateData(dstEid, pvtData);

                return mctp_message_tx(mctp, dstEid, payload.data(),
                                       payload.size(), pvtData.data());
            });

        mctpInterface->register_signal<uint8_t, uint8_t, uint8_t, bool,
                                       std::vector<uint8_t>>(
            "MessageReceivedSignal");

        if (mctpInterface->initialize() == false)
        {
            throw std::system_error(
                std::make_error_code(std::errc::function_not_supported));
        }
    }
    catch (std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "MCTP Interface initialization failed.",
            phosphor::logging::entry("Exception:", e.what()));
        throw;
    }
}

MctpBinding::~MctpBinding()
{
    objectServer->remove_interface(mctpInterface);
    if (mctp)
    {
        mctp_destroy(mctp);
    }
}

void MctpBinding::createUuid(void)
{
    sd_id128_t id;

    if (sd_id128_get_machine_app_specific(mctpdAppId, &id))
    {
        throw std::system_error(
            std::make_error_code(std::errc::address_not_available));
    }

    uuid.insert(uuid.begin(), std::begin(id.bytes), std::end(id.bytes));
    if (uuid.size() != 16)
    {
        throw std::system_error(std::make_error_code(std::errc::bad_address));
    }
}

void MctpBinding::initializeMctp(void)
{
    mctp_set_log_stdio(MCTP_LOG_INFO);
    mctp = mctp_init();
    if (!mctp)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Failed to init mctp");
        throw std::system_error(
            std::make_error_code(std::errc::not_enough_memory));
    }
    mctp_set_rx_all(mctp, rxMessage, nullptr);
}

void MctpBinding::initializeEidPool(const std::vector<mctp_eid_t>& pool)
{
    const std::lock_guard<std::mutex> lock(eidPoolLock);
    for (auto const& epId : pool)
    {
        eidPoolMap.emplace(epId, false);
    }
}

void MctpBinding::updateEidStatus(const mctp_eid_t endpointId,
                                  const bool assigned)
{
    const std::lock_guard<std::mutex> lock(eidPoolLock);
    auto eidItr = eidPoolMap.find(endpointId);
    if (eidItr != eidPoolMap.end())
    {
        eidItr->second = assigned;
        if (assigned)
        {
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                ("EID " + std::to_string(endpointId) + " is assigned").c_str());
        }
        else
        {
            phosphor::logging::log<phosphor::logging::level::DEBUG>(
                ("EID " + std::to_string(endpointId) + " added to pool")
                    .c_str());
        }
    }
    else
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            ("Unable to find EID " + std::to_string(endpointId) +
             " in the pool")
                .c_str());
    }
}

mctp_eid_t MctpBinding::getAvailableEidFromPool(void)
{
    // Note:- No need to check for busowner role explicitly when accessing EID
    // pool since getAvailableEidFromPool will be called only in busowner mode.

    const std::lock_guard<std::mutex> lock(eidPoolLock);
    for (auto& eidPair : eidPoolMap)
    {
        if (!eidPair.second)
        {
            phosphor::logging::log<phosphor::logging::level::INFO>(
                ("Allocated EID: " + std::to_string(eidPair.first)).c_str());
            eidPair.second = true;
            return eidPair.first;
        }
    }
    phosphor::logging::log<phosphor::logging::level::ERR>(
        "No free EID in the pool");
    throw std::system_error(
        std::make_error_code(std::errc::address_not_available));
}

bool MctpBinding::sendMctpMessage(mctp_eid_t destEid, std::vector<uint8_t> req,
                                  std::vector<uint8_t> bindingPrivate)
{
    if (mctp_message_tx(mctp, destEid, req.data(), req.size(),
                        bindingPrivate.data()) < 0)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "Error in mctp_message_tx");
        return false;
    }
    return true;
}

void MctpBinding::processCtrlTxQueue(void)
{
    ctrlTxTimerExpired = false;
    ctrlTxTimer.expires_after(std::chrono::milliseconds(ctrlTxPollInterval));
    ctrlTxTimer.async_wait([this](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted)
        {
            // timer aborted do nothing
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "ctrlTxTimer operation_aborted");
            return;
        }
        else if (ec)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "ctrlTxTimer failed");
            return;
        }

        // Discard the packet if retry count exceeded
        ctrlTxQueueLock.lock();

        ctrlTxQueue.erase(
            std::remove_if(
                ctrlTxQueue.begin(), ctrlTxQueue.end(),
                [this](auto& ctrlTx) {
                    auto& [state, retryCount, maxRespDelay, destEid,
                           bindingPrivate, req, callback] = ctrlTx;

                    maxRespDelay -= ctrlTxPollInterval;

                    // If no reponse:
                    // Retry the packet on every ctrlTxRetryDelay
                    // Total no of tries = 1 + ctrlTxRetryCount
                    if (maxRespDelay > 0 &&
                        state != PacketState::receivedResponse)
                    {
                        if (retryCount > 0 &&
                            maxRespDelay <= retryCount * ctrlTxRetryDelay)
                        {
                            if (sendMctpMessage(destEid, req, bindingPrivate))
                            {
                                phosphor::logging::log<
                                    phosphor::logging::level::INFO>(
                                    "Packet transmited");
                                state = PacketState::transmitted;
                            }

                            // Decrement retry count
                            retryCount--;
                        }

                        return false;
                    }

                    state = PacketState::noResponse;
                    std::vector<uint8_t> resp1 = {};
                    phosphor::logging::log<phosphor::logging::level::ERR>(
                        "Retry timed out, No response");

                    // Call Callback function
                    callback(state, resp1);
                    return true;
                }),
            ctrlTxQueue.end());

        ctrlTxQueueLock.unlock();

        if (ctrlTxQueue.empty())
        {
            ctrlTxTimer.cancel();
            ctrlTxTimerExpired = true;
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "ctrlTxQueue empty, canceling timer");
        }
        else
        {
            processCtrlTxQueue();
        }
    });
}

void MctpBinding::pushToCtrlTxQueue(
    PacketState state, const mctp_eid_t destEid,
    const std::vector<uint8_t>& bindingPrivate, const std::vector<uint8_t>& req,
    std::function<void(PacketState, std::vector<uint8_t>&)>& callback)
{
    ctrlTxQueue.push_back(std::make_tuple(
        state, ctrlTxRetryCount, ((ctrlTxRetryCount + 1) * ctrlTxRetryDelay),
        destEid, bindingPrivate, req, callback));

    if (sendMctpMessage(destEid, req, bindingPrivate))
    {
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "Packet transmited");
        state = PacketState::transmitted;
    }

    if (ctrlTxTimerExpired)
    {
        processCtrlTxQueue();
    }
}

PacketState MctpBinding::sendAndRcvMctpCtrl(
    boost::asio::yield_context& yield, const std::vector<uint8_t>& req,
    const mctp_eid_t destEid, const std::vector<uint8_t>& bindingPrivate,
    std::vector<uint8_t>& resp)
{
    if (req.empty())
    {
        return PacketState::invalidPacket;
    }

    PacketState pktState = PacketState::pushedForTransmission;
    boost::system::error_code ec;
    boost::asio::steady_timer timer(io);

    std::function<void(PacketState, std::vector<uint8_t>&)> callback =
        [&resp, &pktState, &timer](PacketState state,
                                   std::vector<uint8_t>& response) {
            phosphor::logging::log<phosphor::logging::level::INFO>(
                "Callback triggered");

            resp = response;
            pktState = state;
            timer.cancel();

            phosphor::logging::log<phosphor::logging::level::INFO>(
                ("Packet state: " + std::to_string(static_cast<int>(pktState)))
                    .c_str());
        };

    pushToCtrlTxQueue(pktState, destEid, bindingPrivate, req, callback);

    do
    {
        timer.expires_after(std::chrono::milliseconds(ctrlTxRetryDelay));
        phosphor::logging::log<phosphor::logging::level::INFO>(
            "sendAndRcvMctpCtrl: Timer created, ctrl cmd waiting");
        timer.async_wait(yield[ec]);
        if (ec && ec != boost::asio::error::operation_aborted)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "sendAndRcvMctpCtrl: async_wait error");
        }
    } while (pktState == PacketState::pushedForTransmission);
    // Wait for the state to change

    return pktState;
}
