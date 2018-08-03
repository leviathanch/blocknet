//*****************************************************************************
//*****************************************************************************

#include "xrouterapp.h"
#include "init.h"
#include "keystore.h"
#include "main.h"
#include "net.h"
#include "servicenodeconfig.h"
#include "servicenodeman.h"
#include "addrman.h"
#include "script/standard.h"
#include "wallet.h"
#include "bloom.h"

#include "xbridge/xkey.h"
#include "xbridge/util/settings.h"
#include "xbridge/xbridgewallet.h"
#include "xbridge/xbridgewalletconnector.h"
#include "xrouterlogger.h"

#include "xrouterconnector.h"
#include "xrouterconnectorbtc.h"
#include "xrouterconnectoreth.h"

#include "json/json_spirit_reader_template.h"
#include "json/json_spirit_writer_template.h"
#include "json/json_spirit_utils.h"
#include <assert.h>

#include <boost/chrono/chrono.hpp>
#include <boost/thread/thread.hpp>
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/range/adaptor/map.hpp>
#include <iostream>
#include <sstream>
#include <vector>
#include <chrono>

static const CAmount minBlock = 200;

#define TEST_RUN_ON_CLIENT 1
#define DEFAULT_TIMEOUT 20000

#ifdef _WIN32
#include <objbase.h>
    
static std::string generateUUID()
{
    GUID guid;
	CoCreateGuid(&guid);
    char guid_string[37];
    sprintf(guid_string,
          "%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
          guid.Data1, guid.Data2, guid.Data3,
          guid.Data4[0], guid.Data4[1], guid.Data4[2],
          guid.Data4[3], guid.Data4[4], guid.Data4[5],
          guid.Data4[6], guid.Data4[7]);
    return guid_string;
}
    
#else

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

static std::string generateUUID()
{
    boost::uuids::uuid uuid = boost::uuids::random_generator()();
    return boost::uuids::to_string(uuid);
}

#endif 

//*****************************************************************************
//*****************************************************************************
namespace xrouter
{   
boost::container::map<CNode*, double > App::snodeScore = boost::container::map<CNode*, double >();    

//*****************************************************************************
//*****************************************************************************
App::App()
    : server(new XRouterServer), queries()
{
}

//*****************************************************************************
//*****************************************************************************
App::~App()
{
    stop();
}

//*****************************************************************************
//*****************************************************************************
// static
App& App::instance()
{
    static App app;
    return app;
}

//*****************************************************************************
//*****************************************************************************
// static
bool App::isEnabled()
{
    // enabled by default
    return true;
}

bool App::init(int argc, char *argv[])
{
    // init xbridge settings
    Settings & s = settings();
    
    std::string path(GetDataDir(false).string());
    std::string xbridgepath = path + "/xbridge.conf";
    s.read(xbridgepath.c_str());
    s.parseCmdLine(argc, argv);
    LOG() << "Loading xbridge config from file " << xbridgepath;

    this->xrouterpath = path + "/xrouter.conf";
    LOG() << "Loading xrouter config from file " << xrouterpath;
    this->xrouter_settings.read(xrouterpath.c_str());
    this->xrouter_settings.loadPlugins();

    return true;
}

static std::vector<pair<int, CServicenode> > getServiceNodes()
{
    int nHeight;
    {
        LOCK(cs_main);
        CBlockIndex* pindex = chainActive.Tip();
        if(!pindex) return std::vector<pair<int, CServicenode> >();
        nHeight = pindex->nHeight;
    }
    return mnodeman.GetServicenodeRanks(nHeight);
}

//*****************************************************************************
//*****************************************************************************
bool App::start()
{
    updateConfigs();
    bool res = server->start();
    openConnections();
    return res;
}

void App::openConnections()
{
    //LOCK(cs_vNodes);
    LOG() << "Current peers count = " << vNodes.size();
    std::vector<pair<int, CServicenode> > vServicenodeRanks = getServiceNodes();
    BOOST_FOREACH (PAIRTYPE(int, CServicenode) & s, vServicenodeRanks) {
        bool connected = false;
        for (CNode* pnode : vNodes) {
            if (s.second.addr.ToString() == pnode->addr.ToString()) {
                connected = true;
            }
        }
        
        if (!connected) {
            CAddress addr;
            CNode* res = ConnectNode(addr, s.second.addr.ToString().c_str());
            LOG() << "Trying to connect to " << s.second.addr.ToString() << "; result=" << ((res == NULL) ? "fail" : "success");
        }
    }

    LOG() << "Current peers count = " << vNodes.size();
}

std::string App::updateConfigs()
{
    int xrouter_on = xrouter_settings.get<int>("Main.xrouter", 0);
    if (!xrouter_on)
        return "XRouter is turned off. Please check that xrouter.conf is set up correctly.";
    
    std::vector<pair<int, CServicenode> > vServicenodeRanks = getServiceNodes();
    std::chrono::time_point<std::chrono::system_clock> time = std::chrono::system_clock::now();
    
    LOCK(cs_vNodes);
    for (CNode* pnode : vNodes) {
        if (snodeConfigs.count(pnode->addr.ToString())) {
            continue;
        }

        if (lastConfigUpdates.count(pnode)) {
            // There was a request to this node already, a new one will be sent only after 5 minutes
            std::chrono::time_point<std::chrono::system_clock> prev_time = lastConfigUpdates[pnode];
            std::chrono::system_clock::duration diff = time - prev_time;
            if (std::chrono::duration_cast<std::chrono::seconds>(diff) < std::chrono::seconds(300)) 
                continue;
        }
         
        if (TEST_RUN_ON_CLIENT) {
            std::string uuid = this->getXrouterConfig(pnode);
            LOG() << "Getting config from node " << pnode->addrName << " request id = " << uuid;
            lastConfigUpdates[pnode] = time;
            continue;
        }
        BOOST_FOREACH (PAIRTYPE(int, CServicenode) & s, vServicenodeRanks) {
            if (s.second.addr.ToString() == pnode->addr.ToString()) {
                // This node is a service node
                std::string uuid = this->getXrouterConfig(pnode);
                LOG() << "Getting config from node " << pnode->addrName << " request id = " << uuid;
                lastConfigUpdates[pnode] = time;
            }
        }
    }
    
    return "Config requests have been sent";
}

std::string App::printConfigs()
{
    Array result;
    
    for (const auto& it : this->snodeConfigs) {
        Object val;
        val.emplace_back("node", it.first);
        val.emplace_back("config", it.second.rawText());
        result.push_back(Value(val));
    }
    
    return json_spirit::write_string(Value(result), true);
}

//*****************************************************************************
//*****************************************************************************
bool App::stop()
{
    return true;
}

void App::addConnector(const WalletConnectorXRouterPtr & conn)
{
    boost::mutex::scoped_lock l(server->m_connectorsLock);
    server->m_connectors.push_back(conn);
    server->m_connectorCurrencyMap[conn->currency] = conn;
}

WalletConnectorXRouterPtr App::connectorByCurrency(const std::string & currency) const
{
    boost::mutex::scoped_lock l(server->m_connectorsLock);
    if (server->m_connectorCurrencyMap.count(currency))
    {
        return server->m_connectorCurrencyMap.at(currency);
    }

    return xrouter::WalletConnectorXRouterPtr();
}
 
//*****************************************************************************
//*****************************************************************************

std::string App::sendPacketAndWait(const XRouterPacketPtr & packet, std::string id, std::string currency, int confirmations)
{
    Object error;
    boost::shared_ptr<boost::mutex> m(new boost::mutex());
    boost::shared_ptr<boost::condition_variable> cond(new boost::condition_variable());
    boost::mutex::scoped_lock lock(*m);
    int timeout = this->xrouter_settings.get<int>("Main.wait", DEFAULT_TIMEOUT);
    LOG() << "Sending query " << id;
    queriesLocks[id] = std::pair<boost::shared_ptr<boost::mutex>, boost::shared_ptr<boost::condition_variable> >(m, cond);
    if (!sendPacketToServer(packet, confirmations, currency)) {
        error.emplace_back(Pair("error", "Could not find available nodes for your request"));
        return json_spirit::write_string(Value(error), true);
    }

    int confirmation_count = 0;
    while ((confirmation_count < confirmations) && cond->timed_wait(lock, boost::posix_time::milliseconds(timeout)))
        confirmation_count++;

    if(confirmation_count <= confirmations / 2) {
        error.emplace_back(Pair("error", "Failed to get response"));
        error.emplace_back(Pair("uuid", id));
        return json_spirit::write_string(Value(error), true);
    }
    else
    {
        for (unsigned int i = 0; i < queries[id].size(); i++)
        {
            std::string cand = queries[id][i];
            int cnt = 0;
            for (unsigned int j = 0; j < queries[id].size(); j++)
            {
                if (queries[id][j] == cand)
                {
                    cnt++;
                    if (cnt > confirmations / 2)
                        return cand;
                }
            }
        }

        error.emplace_back(Pair("error", "No consensus between responses"));
        return json_spirit::write_string(Value(error), true);
    }
}

std::vector<CNode*> App::getAvailableNodes(const XRouterPacketPtr & packet, std::string wallet)
{
    // Send only to the service nodes that have the required wallet
    std::vector<pair<int, CServicenode> > vServicenodeRanks = getServiceNodes();

    std::vector<CNode*> selectedNodes;
    
    LOCK(cs_vNodes);
    BOOST_FOREACH(const std::string key, snodeConfigs | boost::adaptors::map_keys)
    {
        XRouterSettings settings = snodeConfigs[key];
        if (!settings.walletEnabled(wallet))
            continue;
        if (!settings.isAvailableCommand(packet->command(), wallet))
            continue;
        
        CNode* res = NULL;
        for (CNode* pnode : vNodes) {
            if (key == pnode->addr.ToString()) {
                // This node is a running xrouter
                res = pnode;
                break;
            }
        }
        
        if (!res) {
            CAddress addr;
            res = ConnectNode(addr, key.c_str());
        }
        
        if (!res)
            continue;
        
        std::chrono::time_point<std::chrono::system_clock> time = std::chrono::system_clock::now();
        std::string keystr = wallet + "::" + XRouterCommand_ToString(packet->command());
        double timeout = settings.getCommandTimeout(packet->command(), wallet);
        if (lastPacketsSent.count(res)) {
            if (lastPacketsSent[res].count(keystr)) {
                std::chrono::time_point<std::chrono::system_clock> prev_time = lastPacketsSent[res][keystr];
                std::chrono::system_clock::duration diff = time - prev_time;
                if (std::chrono::duration_cast<std::chrono::milliseconds>(diff) < std::chrono::milliseconds((int)(timeout * 1000))) {
                    continue;
                }
            }
        }
        
        selectedNodes.push_back(res);
    }
    
    for (CNode* node: selectedNodes) {
        if (!snodeScore.count(node))
            snodeScore[node] = 0;
    }
    
    std::sort(selectedNodes.begin(), selectedNodes.end(), cmpNodeScore);
    
    return selectedNodes;
}

CNode* App::getNodeForService(std::string name)
{
    // Send only to the service nodes that have the required wallet
    std::vector<pair<int, CServicenode> > vServicenodeRanks = getServiceNodes();

    LOCK(cs_vNodes);
    BOOST_FOREACH(const std::string key, snodeConfigs | boost::adaptors::map_keys)
    {
        XRouterSettings settings = snodeConfigs[key];
        if (!settings.hasPlugin(name))
            continue;
        
        CNode* res = NULL;
        for (CNode* pnode : vNodes) {
            if (key == pnode->addr.ToString()) {
                // This node is a running xrouter
                res = pnode;
                break;
            }
        }
        
        if (!res) {
            CAddress addr;
            res = ConnectNode(addr, key.c_str());
        }
        
        if (!res)
            continue;
        
        std::chrono::time_point<std::chrono::system_clock> time = std::chrono::system_clock::now();
        double timeout = settings.getPluginSettings(name).get<double>("timeout", -1.0);
        if (lastPacketsSent.count(res)) {
            if (lastPacketsSent[res].count(name)) {
                std::chrono::time_point<std::chrono::system_clock> prev_time = lastPacketsSent[res][name];
                std::chrono::system_clock::duration diff = time - prev_time;
                if (std::chrono::duration_cast<std::chrono::milliseconds>(diff) < std::chrono::milliseconds((int)(timeout * 1000))) {
                    continue;
                }
            }
        }
        
        return res;
    }
    
    return NULL;
}

//*****************************************************************************
//*****************************************************************************
bool App::sendPacketToServer(const XRouterPacketPtr& packet, int confirmations, std::string wallet)
{
    // Send only to the service nodes that have the required wallet
    std::vector<CNode*> selectedNodes = getAvailableNodes(packet, wallet);
    
    if ((int)selectedNodes.size() < confirmations)
        return false;
    
    int sent = 0;
    for (CNode* pnode : selectedNodes) {
        pnode->PushMessage("xrouter", packet->body());
        sent++;
        
        std::chrono::time_point<std::chrono::system_clock> time = std::chrono::system_clock::now();
        std::string keystr = wallet + "::" + XRouterCommand_ToString(packet->command());
        if (!lastPacketsSent.count(pnode)) {
            lastPacketsSent[pnode] = boost::container::map<std::string, std::chrono::time_point<std::chrono::system_clock> >();
        }
        lastPacketsSent[pnode][keystr] = time;
        LOG() << "Sent message to node " << pnode->addrName;
        if (sent == confirmations)
            return true;
    }
    
    return false;
}

void App::sendPacketToClient(const XRouterPacketPtr& packet, CNode* pnode)
{
    pnode->PushMessage("xrouter", packet->body());
}

//*****************************************************************************
//*****************************************************************************
static bool verifyBlockRequirement(const XRouterPacketPtr& packet)
{
    if (packet->size() < 36) {
        LOG() << "Packet not big enough";
        return false;
    }

    uint256 txHash(packet->data());
    CTransaction txval;
    uint256 hashBlock;
    int offset = 32;
    uint32_t vout = *static_cast<uint32_t*>(static_cast<void*>(packet->data() + offset));

    CCoins coins;
    CTxOut txOut;
    if (pcoinsTip->GetCoins(txHash, coins)) {
        if (vout > coins.vout.size()) {
            LOG() << "Invalid vout index " << vout;
            return false;
        }

        txOut = coins.vout[vout];
    } else if (GetTransaction(txHash, txval, hashBlock, true)) {
        txOut = txval.vout[vout];
    } else {
        LOG() << "Could not find " << txHash.ToString();
        return false;
    }

    if (txOut.nValue < minBlock) {
        LOG() << "Insufficient BLOCK " << txOut.nValue;
        return false;
    }

    CTxDestination destination;
    if (!ExtractDestination(txOut.scriptPubKey, destination)) {
        LOG() << "Unable to extract destination";
        return false;
    }

    auto txKeyID = boost::get<CKeyID>(&destination);
    if (!txKeyID) {
        LOG() << "destination must be a single address";
        return false;
    }

    CPubKey packetKey(packet->pubkey(),
        packet->pubkey() + XRouterPacket::pubkeySize);

    if (packetKey.GetID() != *txKeyID) {
        LOG() << "Public key provided doesn't match UTXO destination.";
        return false;
    }

    return true;
}

//*****************************************************************************
//*****************************************************************************
std::string App::processGetBlockCount(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    Object result;
    Object error;

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);
    if (conn)
    {
        result.push_back(Pair("result", conn->getBlockCount()));
    }
    else
    {
        error.emplace_back(Pair("error", "No connector for currency " + currency));
        result = error;
    }

    return json_spirit::write_string(Value(result), true);
}

std::string App::processGetBlockHash(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string blockId((const char *)packet->data()+offset);
    offset += blockId.size() + 1;

    Object result;
    Object error;

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);
    if (conn)
    {
        result.push_back(Pair("result", conn->getBlockHash(blockId)));
    }
    else
    {
        error.emplace_back(Pair("error", "No connector for currency " + currency));
        result = error;
    }

    return json_spirit::write_string(Value(result), true);
}

std::string App::processGetBlock(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string blockHash((const char *)packet->data()+offset);
    offset += blockHash.size() + 1;

    Object result;
    Object error;

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);
    if (conn)
    {
        result = conn->getBlock(blockHash);
    }
    else
    {
        error.emplace_back(Pair("error", "No connector for currency " + currency));
        result = error;
    }
    
    return json_spirit::write_string(Value(result), true);
}

std::string App::processGetTransaction(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string hash((const char *)packet->data()+offset);
    offset += hash.size() + 1;

    Object result;
    Object error;

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);
    if (conn)
    {
        result = conn->getTransaction(hash);
    }
    else
    {
        error.emplace_back(Pair("error", "No connector for currency " + currency));
        result = error;
    }

    return json_spirit::write_string(Value(result), true);
}

std::string App::processGetAllBlocks(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string number_s((const char *)packet->data()+offset);
    offset += number_s.size() + 1;
    int number = std::stoi(number_s);

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);
    Array result;
    if (conn)
    {
        result = conn->getAllBlocks(number);
    }

    return json_spirit::write_string(Value(result), true);
}

std::string App::processGetAllTransactions(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string account((const char *)packet->data()+offset);
    offset += account.size() + 1;
    std::string number_s((const char *)packet->data()+offset);
    offset += number_s.size() + 1;
    int number = std::stoi(number_s);

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);

    Array result;
    if (conn)
    {
        result = conn->getAllTransactions(account, number);
    }

    return json_spirit::write_string(Value(result), true);
}

//*****************************************************************************
//*****************************************************************************
std::string App::processGetBalance(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string account((const char *)packet->data()+offset);
    offset += account.size() + 1;

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);
    std::string result;
    if (conn)
    {
        result = conn->getBalance(account);
    }

    return result;
}

std::string App::processGetBalanceUpdate(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string account((const char *)packet->data()+offset);
    offset += account.size() + 1;
    std::string number_s((const char *)packet->data()+offset);
    offset += number_s.size() + 1;
    int number = std::stoi(number_s);

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);

    std::string result;
    if (conn)
    {
        result = conn->getBalanceUpdate(account, number);
    }

    return result;
}

std::string App::processGetTransactionsBloomFilter(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string number_s((const char *)packet->data()+offset);
    offset += number_s.size() + 1;

    CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
    stream.resize(packet->size() - offset);
    memcpy(&stream[0], packet->data()+offset, packet->size() - offset);

    int number = std::stoi(number_s);

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);

    Array result;
    if (conn)
    {
        result = conn->getTransactionsBloomFilter(number, stream);
    }

    return json_spirit::write_string(Value(result), true);
}

std::string App::processSendTransaction(XRouterPacketPtr packet, uint32_t offset, std::string currency) {
    std::string transaction((const char *)packet->data()+offset);
    offset += transaction.size() + 1;

    xrouter::WalletConnectorXRouterPtr conn = connectorByCurrency(currency);

    Object result;
    Object error;
    
    if (conn)
    {
        result = conn->sendTransaction(transaction);
    }
    else
    {
        error.emplace_back(Pair("error", "No connector for currency " + currency));
        error.emplace_back(Pair("errorcode", "-100"));
        result = error;
    }
    
    return json_spirit::write_string(Value(result), true);
}

std::string App::processCustomCall(std::string name, std::vector<std::string> params)
{
    
    if (!this->xrouter_settings.hasPlugin(name))
        return "Custom call not found";

    XRouterPluginSettings psettings = this->xrouter_settings.getPluginSettings(name);
    std::string callType = psettings.getParam("type");
    LOG() << "Plugin call " << name << " type = " << callType; 
    if (callType == "rpc") {
        Array jsonparams;
        int count = psettings.getMaxParamCount();
        std::vector<std::string> paramtypes;
        std::string typestring = psettings.getParam("paramsType");
        boost::split(paramtypes, typestring, boost::is_any_of(","));
        std::string p;
        for (int i = 0; i < count; i++) {
            p = params[i];
            if (p == "")
                continue;
            if (paramtypes[i] == "string")
                jsonparams.push_back(p);
            else if (paramtypes[i] == "int") {
                try {
                    jsonparams.push_back(std::stoi(p));
                } catch (...) {
                    return "Parameter #" + std::to_string(i+1) + " can not be converted to integer";
                }
            } else if (paramtypes[i] == "bool") {
                if (params[i] == "true")
                    jsonparams.push_back(true);
                else if (params[i] == "false")
                    jsonparams.push_back(true);
                else
                    return "Parameter #" + std::to_string(i+1) + " can not be converted to bool";
            }
        }
        
        std::string user, passwd, ip, port, command;
        user = psettings.getParam("rpcUser");
        passwd = psettings.getParam("rpcPassword");
        ip = psettings.getParam("rpcIp", "127.0.0.1");
        port = psettings.getParam("rpcPort");
        command = psettings.getParam("rpcCommand");
        Object result = xbridge::rpc::CallRPC(user, passwd, ip, port, command, jsonparams);
        return json_spirit::write_string(Value(result), true);
    } else if (callType == "shell") {
        std::string cmd = psettings.getParam("cmd");
        int count = psettings.getMaxParamCount();
        std::string p;
        for (int i = 0; i < count; i++) {
            cmd += " " + params[i];
        }
        
        LOG() << "Executing shell command " << cmd;
        std::string result = CallCMD(cmd);
        return result;
    }  
    
    return "Unknown type";
}

std::string App::processGetPaymentAddress(XRouterPacketPtr packet) {
    return "";
}

std::string App::processGetXrouterConfig(XRouterSettings cfg) {
    Object result;
    result.emplace_back(Pair("config", cfg.rawText()));
    Object plugins;
    for (std::string s : cfg.getPlugins())
        plugins.emplace_back(s, cfg.getPluginSettings(s).rawText());
    result.emplace_back(Pair("plugins", plugins));
    LOG() << "Sending config " << json_spirit::write_string(Value(result), true);
    return json_spirit::write_string(Value(result), true);
}

//*****************************************************************************
//*****************************************************************************
bool App::processReply(XRouterPacketPtr packet) {
    uint32_t offset = 0;

    std::string uuid((const char *)packet->data()+offset);
    offset += uuid.size() + 1;
    std::string reply((const char *)packet->data()+offset);
    offset += reply.size() + 1;

    LOG() << "Got reply to query " << uuid;
    
    if (configQueries.count(uuid)) {
        LOG() << "Got xrouter config from node " << configQueries[uuid]->addrName;
        LOG() << reply;
        Value reply_val;
        read_string(reply, reply_val);
        Object reply_obj = reply_val.get_obj();
        std::string config = find_value(reply_obj, "config").get_str();
        Object plugins  = find_value(reply_obj, "plugins").get_obj();
        
        XRouterSettings settings;
        settings.read(config);
        
        for (Object::size_type i = 0; i != plugins.size(); i++ ) {
            XRouterPluginSettings psettings;
            psettings.read(std::string(plugins[i].value_.get_str()));
            settings.addPlugin(std::string(plugins[i].name_), psettings);
        }
        
        snodeConfigs[configQueries[uuid]->addr.ToString()] = settings;
        return true;
    }
    
    // check uuid is in queriesLock keys
    if (!queriesLocks.count(uuid))
        return true;
    
    LOG() << reply;
    boost::mutex::scoped_lock l(*queriesLocks[uuid].first);
    if (!queries.count(uuid))
        queries[uuid] = vector<std::string>();
    queries[uuid].push_back(reply);
    queriesLocks[uuid].second->notify_all();
    return true;
}

//*****************************************************************************
//*****************************************************************************
void App::onMessageReceived(CNode* node, const std::vector<unsigned char>& message, CValidationState& state)
{
    LOG() << "Received xrouter packet";

    // If Main.xrouter == 0, xrouter is turned off on this snode
    int xrouter_on = xrouter_settings.get<int>("Main.xrouter", 0);
    if (!xrouter_on)
        return;
    
    XRouterPacketPtr packet(new XRouterPacket);
    if (!packet->copyFrom(message)) {
        LOG() << "incorrect packet received " << __FUNCTION__;
        state.DoS(10, error("XRouter: invalid packet received"), REJECT_INVALID, "xrouter-error");
        return;
    }

    // TODO: here it implies that xrReply and xrConfig reply are first in enum before others, better compare explicitly
    if ((packet->command() > xrConfigReply) && (packet->command() != xrGetXrouterConfig) && !packet->verify()) {
        LOG() << "unsigned packet or signature error " << __FUNCTION__;
        state.DoS(10, error("XRouter: unsigned packet or signature error"), REJECT_INVALID, "xrouter-error");
        return;
    }

    if ((packet->command() > xrConfigReply) && (packet->command() != xrGetXrouterConfig) && !verifyBlockRequirement(packet)) {
        LOG() << "Block requirement not satisfied\n";
        state.DoS(10, error("XRouter: block requirement not satisfied"), REJECT_INVALID, "xrouter-error");
        return;
    }

    std::string reply;
    uint32_t offset = 36;
    std::string uuid((const char *)packet->data()+offset);
    offset += uuid.size() + 1;
    std::string currency((const char *)packet->data()+offset);
    offset += currency.size() + 1;
    LOG() << "XRouter command: " << std::string(XRouterCommand_ToString(packet->command()));
    if ((packet->command() > xrConfigReply) && !this->xrouter_settings.isAvailableCommand(packet->command(), currency)) {
        LOG() << "This command is blocked in xrouter.conf";
        return;
    }
    
    std::chrono::time_point<std::chrono::system_clock> time = std::chrono::system_clock::now();
    if (packet->command() == xrGetXrouterConfig) {
        std::string addr((const char *)packet->data()+36); //36 = base offset
        XRouterSettings cfg;
        if (addr == "self")
            cfg = this->xrouter_settings;
        else {
            if (!this->snodeConfigs.count(addr))
                return;
            else
                cfg = this->snodeConfigs[addr];
        }
        
        reply = processGetXrouterConfig(cfg);
        if (lastConfigQueries.count(node)) {
            std::chrono::time_point<std::chrono::system_clock> prev_time = lastConfigQueries[node];
            std::chrono::system_clock::duration diff = time - prev_time;
            if (std::chrono::duration_cast<std::chrono::seconds>(diff) < std::chrono::seconds(10))
                state.DoS(10, error("XRouter: too many config requests"), REJECT_INVALID, "xrouter-error");
            lastConfigQueries[node] = time;
        } else {
            lastConfigQueries[node] = time;
        }
    } else if (packet->command() == xrCustomCall) {
        XRouterPluginSettings psettings = this->xrouter_settings.getPluginSettings(currency);
        
        std::string keystr = currency;
        double timeout = psettings.get<double>("timeout", -1.0);
        if (timeout >= 0) {
            if (lastPacketsReceived.count(node)) {
                if (lastPacketsReceived[node].count(keystr)) {
                    std::chrono::time_point<std::chrono::system_clock> prev_time = lastPacketsReceived[node][keystr];
                    std::chrono::system_clock::duration diff = time - prev_time;
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(diff) < std::chrono::milliseconds((int)(timeout * 1000))) {
                        std::string err_msg = "XRouter: too many requests to plugin " + keystr; 
                        state.DoS(100, error(err_msg.c_str()), REJECT_INVALID, "xrouter-error");
                    }
                    if (!lastPacketsReceived.count(node))
                        lastPacketsReceived[node] = boost::container::map<std::string, std::chrono::time_point<std::chrono::system_clock> >();
                    lastPacketsReceived[node][keystr] = time;
                } else {
                    lastPacketsReceived[node][keystr] = time;
                }
            } else {
                lastPacketsReceived[node] = boost::container::map<std::string, std::chrono::time_point<std::chrono::system_clock> >();
                lastPacketsReceived[node][keystr] = time;
            }
        }
        
        std::vector<std::string> params;
        int count = psettings.getMaxParamCount();
        std::string p;
        for (int i = 0; i < count; i++) {
            p = (const char *)packet->data()+offset;
            params.push_back(p);
            offset += p.size() + 1;
        }
        
        reply = processCustomCall(currency, params);
    } else {
        std::string keystr = currency + "::" + XRouterCommand_ToString(packet->command());
        double timeout = this->xrouter_settings.getCommandTimeout(packet->command(), currency);
        if (lastPacketsReceived.count(node)) {
            if (lastPacketsReceived[node].count(keystr)) {
                std::chrono::time_point<std::chrono::system_clock> prev_time = lastPacketsReceived[node][keystr];
                std::chrono::system_clock::duration diff = time - prev_time;
                if (std::chrono::duration_cast<std::chrono::milliseconds>(diff) < std::chrono::milliseconds((int)(timeout * 1000))) {
                    std::string err_msg = "XRouter: too many requests of type " + keystr; 
                    state.DoS(100, error(err_msg.c_str()), REJECT_INVALID, "xrouter-error");
                }
                if (!lastPacketsReceived.count(node))
                    lastPacketsReceived[node] = boost::container::map<std::string, std::chrono::time_point<std::chrono::system_clock> >();
                lastPacketsReceived[node][keystr] = time;
            } else {
                lastPacketsReceived[node][keystr] = time;
            }
        } else {
            lastPacketsReceived[node] = boost::container::map<std::string, std::chrono::time_point<std::chrono::system_clock> >();
            lastPacketsReceived[node][keystr] = time;
        }
            
        switch (packet->command()) {
        case xrGetBlockCount:
            reply = processGetBlockCount(packet, offset, currency);
            break;
        case xrGetBlockHash:
            reply = processGetBlockHash(packet, offset, currency);
            break;
        case xrGetBlock:
            reply = processGetBlock(packet, offset, currency);
            break;
        case xrGetTransaction:
            reply = processGetTransaction(packet, offset, currency);
            break;
        case xrGetAllBlocks:
            reply = processGetAllBlocks(packet, offset, currency);
            break;
        case xrGetAllTransactions:
            reply = processGetAllTransactions(packet, offset, currency);
            break;
        case xrGetBalance:
            reply = processGetBalance(packet, offset, currency);
            break;
        case xrGetBalanceUpdate:
            reply = processGetBalanceUpdate(packet, offset, currency);
            break;
        case xrGetTransactionsBloomFilter:
            reply = processGetTransactionsBloomFilter(packet, offset, currency);
            break;
        case xrSendTransaction:
            reply = processSendTransaction(packet, offset, currency);
            break;
        case xrReply:
            processReply(packet);
            return;
        default:
            LOG() << "Unknown packet";
            return;
        }
    }

    XRouterPacketPtr rpacket(new XRouterPacket(xrReply));

    rpacket->append(uuid);
    rpacket->append(reply);
    sendPacketToClient(rpacket, node);
    return;
}

//*****************************************************************************
//*****************************************************************************
static bool satisfyBlockRequirement(uint256& txHash, uint32_t& vout, CKey& key)
{
    if (!pwalletMain) {
        return false;
    }
    for (auto& addressCoins : pwalletMain->AvailableCoinsByAddress()) {
        for (auto& output : addressCoins.second) {
            if (output.Value() >= minBlock) {
                CKeyID keyID;
                if (!addressCoins.first.GetKeyID(keyID)) {
                    //std::cerr << "GetKeyID failed\n";
                    continue;
                }
                if (!pwalletMain->GetKey(keyID, key)) {
                    //std::cerr << "GetKey failed\n";
                    continue;
                }
                txHash = output.tx->GetHash();
                vout = output.i;
                return true;
            }
        }
    }
    return false;
}

//*****************************************************************************
//*****************************************************************************
std::string App::xrouterCall(enum XRouterCommand command, const std::string & currency, std::string param1, std::string param2, std::string confirmations)
{
    int xrouter_on = xrouter_settings.get<int>("Main.xrouter", 0);
    if (!xrouter_on)
        return "XRouter is turned off. Please check that xrouter.conf is set up correctly.";
    
    updateConfigs();
    
    XRouterPacketPtr packet(new XRouterPacket(command));

    uint256 txHash;
    uint32_t vout = 0;
    CKey key;
    if ((command != xrGetXrouterConfig) && !satisfyBlockRequirement(txHash, vout, key)) {
        LOG() << "Minimum block requirement not satisfied";
        return "Minimum block requirement not satisfied. Make sure that your wallet is unlocked.";
    }

    std::string id = generateUUID();

    packet->append(txHash.begin(), 32);
    packet->append(vout);
    packet->append(id);
    packet->append(currency);
    if (!param1.empty())
        packet->append(param1);
    if (!param2.empty())
        packet->append(param2);
    packet->sign(key);

    if (!confirmations.empty())
        return sendPacketAndWait(packet, id, currency, std::stoi(confirmations));
    else
        return sendPacketAndWait(packet, id, currency);
}

std::string App::getBlockCount(const std::string & currency, const std::string & confirmations)
{
    return this->xrouterCall(xrGetBlockCount, currency, "", "", confirmations);
}

std::string App::getBlockHash(const std::string & currency, const std::string & blockId, const std::string & confirmations)
{
    return this->xrouterCall(xrGetBlockHash, currency, blockId, "", confirmations);
}

std::string App::getBlock(const std::string & currency, const std::string & blockHash, const std::string & confirmations)
{
    return this->xrouterCall(xrGetBlock, currency, blockHash, "", confirmations);
}

std::string App::getTransaction(const std::string & currency, const std::string & hash, const std::string & confirmations)
{
    return this->xrouterCall(xrGetTransaction, currency, hash, "", confirmations);
}

std::string App::getAllBlocks(const std::string & currency, const std::string & number, const std::string & confirmations)
{
    return this->xrouterCall(xrGetAllBlocks, currency, number, "", confirmations);
}

std::string App::getAllTransactions(const std::string & currency, const std::string & account, const std::string & number, const std::string & confirmations)
{
    return this->xrouterCall(xrGetAllTransactions, currency, account, number, confirmations);
}

std::string App::getBalance(const std::string & currency, const std::string & account, const std::string & confirmations)
{
    return this->xrouterCall(xrGetBalance, currency, account, "", confirmations);
}

std::string App::getBalanceUpdate(const std::string & currency, const std::string & account, const std::string & number, const std::string & confirmations)
{
    return this->xrouterCall(xrGetBalanceUpdate, currency, account, number, confirmations);
}

std::string App::getTransactionsBloomFilter(const std::string & currency, const std::string & number, const std::string & filter, const std::string & confirmations)
{
    return this->xrouterCall(xrGetTransactionsBloomFilter, currency, number, filter, confirmations);
}

std::string App::getReply(const std::string & id)
{
    Object result;

    if(queries[id].size() == 0) {
        result.emplace_back(Pair("error", "No replies found"));
        result.emplace_back(Pair("uuid", id));
        return json_spirit::write_string(Value(result), true);
    } else {
        for (unsigned int i = 0; i < queries[id].size(); i++) {
            std::string cand = queries[id][i];
            result.emplace_back(Pair("reply" + std::to_string(i+1), cand));
        }

        return json_spirit::write_string(Value(result), true);
    }
}

std::string App::sendTransaction(const std::string & currency, const std::string & transaction)
{
    int xrouter_on = xrouter_settings.get<int>("Main.xrouter", 0);
    if (!xrouter_on)
        return "XRouter is turned off. Please check that xrouter.conf is set up correctly.";
    
    updateConfigs();
    
    XRouterPacketPtr packet(new XRouterPacket(xrSendTransaction));

    uint256 txHash;
    uint32_t vout = 0;
    CKey key;
    if (!satisfyBlockRequirement(txHash, vout, key)) {
        return "Minimum block requirement not satisfied. Make sure that your wallet is unlocked.";
    }

    std::string id = generateUUID();

    packet->append(txHash.begin(), 32);
    packet->append(vout);
    packet->append(id);
    packet->append(currency);
    packet->append(transaction);
    packet->sign(key);
    
    boost::shared_ptr<boost::mutex> m(new boost::mutex());
    boost::shared_ptr<boost::condition_variable> cond(new boost::condition_variable());
    boost::mutex::scoped_lock lock(*m);
    queriesLocks[id] = std::pair<boost::shared_ptr<boost::mutex>, boost::shared_ptr<boost::condition_variable> >(m, cond);

    std::vector<unsigned char> msg;
    msg.insert(msg.end(), packet->body().begin(), packet->body().end());

    std::vector<CNode*> selectedNodes = getAvailableNodes(packet, currency);
    
    if ((int)selectedNodes.size() == 0)
        return "No available nodes";
    
    for (CNode* pnode : selectedNodes) {
        pnode->PushMessage("xrouter", msg);
        if (cond->timed_wait(lock, boost::posix_time::milliseconds(3000))) {
            std::string reply = queries[id][0];
            Value reply_val;
            read_string(reply, reply_val);
            Object reply_obj = reply_val.get_obj();
            const Value & errorcode  = find_value(reply_obj, "errorcode");
            if (errorcode.type() != null_type)
                if (errorcode.get_int() < 0) {
                    // Try sending to another node
                    queries[id].clear();
                    continue;
                }
        
            return reply;
        }
    }
    
    return "No available nodes";
}

std::string App::sendCustomCall(const std::string & name, std::vector<std::string> & params)
{
    int xrouter_on = xrouter_settings.get<int>("Main.xrouter", 0);
    if (!xrouter_on)
        return "XRouter is turned off. Please check that xrouter.conf is set up correctly.";
    
    if (this->xrouter_settings.hasPlugin(name)) {
        // Run the plugin locally
        return processCustomCall(name, params);
    }
    
    updateConfigs();
    
    XRouterPacketPtr packet(new XRouterPacket(xrCustomCall));

    uint256 txHash;
    uint32_t vout = 0;
    CKey key;
    if (!satisfyBlockRequirement(txHash, vout, key)) {
        return "Minimum block requirement not satisfied. Make sure that your wallet is unlocked.";
    }
    
    std::string id = generateUUID();

    packet->append(txHash.begin(), 32);
    packet->append(vout);
    packet->append(id);
    packet->append(name);
    for (std::string param: params)
        packet->append(param);
    packet->sign(key);

    boost::shared_ptr<boost::mutex> m(new boost::mutex());
    boost::shared_ptr<boost::condition_variable> cond(new boost::condition_variable());
    boost::mutex::scoped_lock lock(*m);
    queriesLocks[id] = std::pair<boost::shared_ptr<boost::mutex>, boost::shared_ptr<boost::condition_variable> >(m, cond);

    std::vector<unsigned char> msg;
    msg.insert(msg.end(), packet->body().begin(), packet->body().end());
    
    CNode* pnode = getNodeForService(name);
    if (!pnode)
        return "No available nodes";
    
    unsigned int min_count = snodeConfigs[pnode->addr.ToString()].getPluginSettings(name).getMinParamCount();
    if (params.size() < min_count) {
        return "Not enough plugin parameters";
    }
    
    unsigned int max_count = snodeConfigs[pnode->addr.ToString()].getPluginSettings(name).getMaxParamCount();
    if (params.size() > max_count) {
        return "Too many plugin parameters";
    }
    
    Object result;
    
    std::chrono::time_point<std::chrono::system_clock> time = std::chrono::system_clock::now();
    if (!lastPacketsSent.count(pnode)) {
        lastPacketsSent[pnode] = boost::container::map<std::string, std::chrono::time_point<std::chrono::system_clock> >();
    }
    lastPacketsSent[pnode][name] = time;
    
    pnode->PushMessage("xrouter", msg);
    int timeout = this->xrouter_settings.get<int>("Main.wait", DEFAULT_TIMEOUT);
    if (cond->timed_wait(lock, boost::posix_time::milliseconds(timeout))) {
        std::string reply = queries[id][0];
        return reply;
    }
    
    result.emplace_back(Pair("error", "Failed to get response"));
    result.emplace_back(Pair("uuid", id));
    return json_spirit::write_string(Value(result), true);
}


std::string App::getPaymentAddress(CNode* node)
{
    return "";
}

std::string App::getXrouterConfig(CNode* node, std::string addr) {
    XRouterPacketPtr packet(new XRouterPacket(xrGetXrouterConfig));

    uint256 txHash;
    uint32_t vout = 0;

    std::string id = generateUUID();
    packet->append(txHash.begin(), 32);
    packet->append(vout);
    packet->append(id);
    packet->append(addr);
    
    std::vector<unsigned char> msg;
    msg.insert(msg.end(), packet->body().begin(), packet->body().end());
    
    this->configQueries[id] = node;
    node->PushMessage("xrouter", msg);
    return id;
}

std::string App::getXrouterConfigSync(CNode* node) {
    XRouterPacketPtr packet(new XRouterPacket(xrGetXrouterConfig));

    uint256 txHash;
    uint32_t vout = 0;

    std::string id = generateUUID();

    packet->append(txHash.begin(), 32);
    packet->append(vout);
    packet->append(id);

    boost::shared_ptr<boost::mutex> m(new boost::mutex());
    boost::shared_ptr<boost::condition_variable> cond(new boost::condition_variable());
    boost::mutex::scoped_lock lock(*m);
    queriesLocks[id] = std::pair<boost::shared_ptr<boost::mutex>, boost::shared_ptr<boost::condition_variable> >(m, cond);

    std::vector<unsigned char> msg;
    msg.insert(msg.end(), packet->body().begin(), packet->body().end());
    node->PushMessage("xrouter", msg);
    int timeout = this->xrouter_settings.get<int>("Main.wait", DEFAULT_TIMEOUT);
    if (!cond->timed_wait(lock, boost::posix_time::milliseconds(timeout)))
        return "Could not get XRouter config";

    std::string reply = queries[id][0];
    XRouterSettings settings;
    settings.read(reply);
    this->snodeConfigs[node->addr.ToString()] = settings;
    return reply;
}

void App::reloadConfigs() {
    LOG() << "Reloading xrouter config from file " << xrouterpath;
    this->xrouter_settings.read(xrouterpath.c_str());
    this->xrouter_settings.loadPlugins();
}

std::string App::getStatus() {
    Object result;
    result.emplace_back(Pair("enabled", xrouter_settings.get<int>("Main.xrouter", 0) != 0));
    result.emplace_back(Pair("config", this->xrouter_settings.rawText()));
    Object myplugins;
    for (std::string s : this->xrouter_settings.getPlugins())
        myplugins.emplace_back(s, this->xrouter_settings.getPluginSettings(s).rawText());
    result.emplace_back(Pair("plugins", myplugins));
    
    Object nodes;
    for (CNode* node: vNodes) {
        if (!snodeConfigs.count(node->addr.ToString()))
            continue;
        Object vnode;
        vnode.emplace_back(Pair("config", snodeConfigs[node->addr.ToString()].rawText()));
        Object plugins;
        for (std::string s : snodeConfigs[node->addr.ToString()].getPlugins())
            plugins.emplace_back(s, snodeConfigs[node->addr.ToString()].getPluginSettings(s).rawText());
        vnode.emplace_back(Pair("plugins", plugins));
        nodes.emplace_back(Pair(node->addrName, vnode));
    }
    
    result.emplace_back(Pair("nodes", nodes));
    
    return json_spirit::write_string(Value(result), true);
}

} // namespace xrouter
